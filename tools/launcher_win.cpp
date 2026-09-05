#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>

#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(linker, \
    "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' " \
    "version='6.0.0.0' processorArchitecture='*' " \
    "publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

using std::string;
using std::wstring;

int g_dpi = 96;
bool g_applying = false;
HFONT g_font = nullptr;
HWND g_main = nullptr;
HWND g_tabs = nullptr;
HWND g_lv = nullptr;
HWND g_bind_hint = nullptr;
HHOOK g_kb_hook = nullptr;
int g_capture_row = -1;
bool g_capture_append = false;
bool g_capture_posted = false;
wstring g_capture_prev;
wstring g_pending_key;

constexpr UINT WM_CAPTURE_CANCEL = WM_APP + 1;
constexpr UINT WM_CAPTURE_KEY = WM_APP + 2;
const wchar_t* kHintIdle =
    L"Двойной клик по строке или «Назначить», затем нажми клавишу. Esc — отмена.";

enum Id : int {
    IDC_R_PERF = 101,
    IDC_R_MED,
    IDC_R_QUAL,
    IDC_R_CUSTOM,
    IDC_TABS,
    IDC_SCALE,
    IDC_SCALE_FIT,
    IDC_SCALE_HINT,
    IDC_PRESENT,
    IDC_FXAA,
    IDC_ANISO,
    IDC_THREADS,
    IDC_FULLSCREEN,
    IDC_VSYNC,
    IDC_DITHER,
    IDC_ASYNC,
    IDC_GFX_NOTE,
    IDC_DEV_KB,
    IDC_DEV_PAD,
    IDC_DEV_BOTH,
    IDC_BACKEND,
    IDC_GUIDE,
    IDC_MNK_MOUSE,
    IDC_SENS_LBL,
    IDC_SENS,
    IDC_BINDS,
    IDC_BIND_HINT,
    IDC_BIND_ASSIGN,
    IDC_BIND_ADD,
    IDC_BIND_CLEAR,
    IDC_BIND_RESET,
    IDC_ABOUT,
    IDC_SAVE,
    IDC_PLAY,
};

const wchar_t* kKeyIds[] = {
    L"keybind_a", L"keybind_b", L"keybind_x", L"keybind_y", L"keybind_start", L"keybind_back",
    L"keybind_left_trigger", L"keybind_right_trigger", L"keybind_left_shoulder", L"keybind_right_shoulder",
    L"keybind_lstick_up", L"keybind_lstick_down", L"keybind_lstick_left", L"keybind_lstick_right",
    L"keybind_lstick_press", L"keybind_rstick_up", L"keybind_rstick_down", L"keybind_rstick_left",
    L"keybind_rstick_right", L"keybind_rstick_press", L"keybind_dpad_up", L"keybind_dpad_down",
    L"keybind_dpad_left", L"keybind_dpad_right",
};
const wchar_t* kKeyLabels[] = {
    L"A (подтверждение / атака)", L"B (отмена)", L"X", L"Y", L"Start (пауза / меню)", L"Back",
    L"LT", L"RT", L"LB", L"RB",
    L"Левый стик вверх", L"Левый стик вниз", L"Левый стик влево", L"Левый стик вправо", L"Л3",
    L"Правый стик вверх", L"Правый стик вниз", L"Правый стик влево", L"Правый стик вправо", L"R3",
    L"D-pad вверх", L"D-pad вниз", L"D-pad влево", L"D-pad вправо",
};
const wchar_t* kKeyDefaults[] = {
    L"Semicolon,Space", L"Quote,Backspace", L"L", L"P", L"X,Return", L"Z,Tab",
    L"Q,I", L"E,O", L"1", L"3",
    L"W", L"S", L"A", L"D", L"F",
    L"Up", L"Down", L"Left", L"Right", L"K",
    L"Shift+Up", L"Shift+Down", L"Shift+Left", L"Shift+Right",
};
constexpr int kBindCount = 24;

int S(int v) { return MulDiv(v, g_dpi, 96); }

wstring u8_to_wide(const string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

string wide_to_u8(const wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

wstring exe_dir() {
    wchar_t buf[32768];
    DWORD n = GetModuleFileNameW(nullptr, buf, 32768);
    wstring p(buf, n);
    auto slash = p.find_last_of(L"\\/");
    return slash == wstring::npos ? L"." : p.substr(0, slash);
}

string read_file_u8(const wstring& path) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return {};
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    string s(sz > 0 ? (size_t)sz : 0, '\0');
    if (sz > 0) fread(s.data(), 1, (size_t)sz, f);
    fclose(f);
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB &&
        (unsigned char)s[2] == 0xBF) {
        s.erase(0, 3);
    }
    return s;
}

bool write_file_u8(const wstring& path, const string& data) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
    fwrite(data.data(), 1, data.size(), f);
    fclose(f);
    return true;
}

string json_escape(const string& s) {
    string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') {
            o.push_back('\\');
            o.push_back((char)c);
        } else {
            o.push_back((char)c);
        }
    }
    return o;
}

string toml_str(const string& s) {
    string o = "\"";
    for (unsigned char c : s) {
        if (c == '\\' || c == '"') {
            o.push_back('\\');
            o.push_back((char)c);
        } else {
            o.push_back((char)c);
        }
    }
    o.push_back('"');
    return o;
}

bool json_raw(const string& j, const string& key, string& out) {
    string pat = "\"" + key + "\"";
    size_t p = 0;
    for (;;) {
        p = j.find(pat, p);
        if (p == string::npos) return false;
        if (p > 0) {
            unsigned char prev = (unsigned char)j[p - 1];
            if (prev == '\\') {
                p += pat.size();
                continue;
            }
        }
        size_t c = j.find(':', p + pat.size());
        if (c == string::npos) return false;
        c++;
        while (c < j.size() && (j[c] == ' ' || j[c] == '\t' || j[c] == '\n' || j[c] == '\r')) c++;
        if (c >= j.size()) return false;
        if (j[c] == '"') {
            string v;
            c++;
            while (c < j.size()) {
                if (j[c] == '\\' && c + 1 < j.size()) {
                    v.push_back(j[c + 1]);
                    c += 2;
                    continue;
                }
                if (j[c] == '"') break;
                v.push_back(j[c++]);
            }
            out = v;
            return true;
        }
        size_t e = c;
        while (e < j.size() && j[e] != ',' && j[e] != '}' && j[e] != '\n' && j[e] != '\r') e++;
        out = j.substr(c, e - c);
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        return true;
    }
}

struct Settings {
    string preset = "quality";
    string input_mode = "keyboard";
    string input_backend = "sdl";
    bool guide_button = false;
    bool mnk_mode = true;
    bool mnk_mouse = false;
    double mnk_sensitivity = 1.0;
    int resolution_scale = 2;
    string present_effect = "cas";
    string swap_post_effect = "none";
    int anisotropic_override = 5;
    bool present_dither = true;
    bool vsync = true;
    bool fullscreen = true;
    bool async_shader_compilation = true;
    int d3d12_pipeline_creation_threads = 8;
    int tex_soft = 1024;
    int tex_hard = 2048;
    wstring binds[kBindCount];
};

void desktop_size(int& w, int& h) {
    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmPelsWidth > 0) {
        w = (int)dm.dmPelsWidth;
        h = (int)dm.dmPelsHeight;
        return;
    }
    w = GetSystemMetrics(SM_CXSCREEN);
    h = GetSystemMetrics(SM_CYSCREEN);
}

int preset_resolution_scale(const string& name) {
    int w = 1920, h = 1080;
    desktop_size(w, h);
    (void)h;
    if (name == "performance") return 1;
    if (name == "medium") return w >= 2560 ? 2 : 1;
    if (w >= 3800) return 3;
    if (w >= 2560) return 2;
    return 2;
}

void apply_preset_values(Settings& s, const string& name) {
    if (name == "performance") {
        s.resolution_scale = preset_resolution_scale(name);
        s.present_effect = "bilinear";
        s.swap_post_effect = "none";
        s.anisotropic_override = 3;
        s.present_dither = false;
        s.vsync = true;
        s.fullscreen = true;
        s.async_shader_compilation = true;
        s.d3d12_pipeline_creation_threads = -1;
        s.tex_soft = 384;
        s.tex_hard = 768;
    } else if (name == "medium") {
        s.resolution_scale = preset_resolution_scale(name);
        s.present_effect = "cas";
        s.swap_post_effect = "none";
        s.anisotropic_override = 5;
        s.present_dither = false;
        s.vsync = true;
        s.fullscreen = true;
        s.async_shader_compilation = true;
        s.d3d12_pipeline_creation_threads = 8;
        s.tex_soft = 1024;
        s.tex_hard = 2048;
    } else if (name == "quality") {
        s.resolution_scale = preset_resolution_scale(name);
        s.present_effect = "cas";
        s.swap_post_effect = "none";
        s.anisotropic_override = 5;
        s.present_dither = true;
        s.vsync = true;
        s.fullscreen = true;
        s.async_shader_compilation = true;
        s.d3d12_pipeline_creation_threads = 8;
        s.tex_soft = 1024;
        s.tex_hard = 2048;
    }
}

Settings defaults() {
    Settings s;
    apply_preset_values(s, "quality");
    s.preset = "quality";
    for (int i = 0; i < kBindCount; i++) s.binds[i] = kKeyDefaults[i];
    return s;
}

int json_int(const string& j, const string& key, int def) {
    string v;
    if (!json_raw(j, key, v) || v.empty()) return def;
    try {
        return std::stoi(v);
    } catch (...) {
        return def;
    }
}

bool json_bool(const string& j, const string& key, bool def) {
    string v;
    if (!json_raw(j, key, v)) return def;
    return v == "true" || v == "1";
}

double json_double(const string& j, const string& key, double def) {
    string v;
    if (!json_raw(j, key, v) || v.empty()) return def;
    try {
        return std::stod(v);
    } catch (...) {
        return def;
    }
}

string json_str(const string& j, const string& key, const string& def) {
    string v;
    if (!json_raw(j, key, v)) return def;
    return v;
}

Settings load_settings(const wstring& dir) {
    Settings s = defaults();
    string j = read_file_u8(dir + L"\\launcher.json");
    if (j.empty()) return s;
    s.preset = json_str(j, "preset", s.preset);
    s.mnk_mode = json_bool(j, "mnk_mode", s.mnk_mode);
    s.mnk_mouse = json_bool(j, "mnk_mouse", s.mnk_mouse);
    s.input_backend = json_str(j, "input_backend", s.input_backend);
    if (s.input_backend != "xinput") s.input_backend = "sdl";
    s.guide_button = json_bool(j, "guide_button", s.guide_button);
    s.input_mode = json_str(j, "input_mode", s.mnk_mode ? "keyboard" : "gamepad");
    if (s.input_mode != "gamepad" && s.input_mode != "both") s.input_mode = "keyboard";
    s.mnk_mode = s.input_mode != "gamepad";
    s.mnk_sensitivity = json_double(j, "mnk_sensitivity", s.mnk_sensitivity);
    s.resolution_scale = json_int(j, "resolution_scale", s.resolution_scale);
    s.present_effect = json_str(j, "present_effect", s.present_effect);
    s.swap_post_effect = json_str(j, "swap_post_effect", s.swap_post_effect);
    s.anisotropic_override = json_int(j, "anisotropic_override", s.anisotropic_override);
    s.present_dither = json_bool(j, "present_dither", s.present_dither);
    s.vsync = json_bool(j, "vsync", s.vsync);
    s.fullscreen = json_bool(j, "fullscreen", s.fullscreen);
    s.async_shader_compilation = json_bool(j, "async_shader_compilation", s.async_shader_compilation);
    s.d3d12_pipeline_creation_threads =
        json_int(j, "d3d12_pipeline_creation_threads", s.d3d12_pipeline_creation_threads);
    s.tex_soft = json_int(j, "texture_cache_memory_limit_soft", s.tex_soft);
    s.tex_hard = json_int(j, "texture_cache_memory_limit_hard", s.tex_hard);
    for (int i = 0; i < kBindCount; i++) {
        string id = wide_to_u8(kKeyIds[i]);
        string v;
        if (json_raw(j, id, v)) s.binds[i] = u8_to_wide(v);
    }
    return s;
}

string toml_bool(bool v) { return v ? "true" : "false"; }

bool save_all(const wstring& dir, const Settings& s) {
    std::ostringstream json;
    json << "{\n";
    json << "  \"preset\": \"" << json_escape(s.preset) << "\",\n";
    json << "  \"input_mode\": \"" << json_escape(s.input_mode) << "\",\n";
    json << "  \"input_backend\": \"" << json_escape(s.input_backend) << "\",\n";
    json << "  \"guide_button\": " << toml_bool(s.guide_button) << ",\n";
    json << "  \"mnk_mode\": " << toml_bool(s.mnk_mode) << ",\n";
    json << "  \"mnk_mouse\": " << toml_bool(s.mnk_mouse) << ",\n";
    json << "  \"mnk_sensitivity\": " << s.mnk_sensitivity << ",\n";
    json << "  \"resolution_scale\": " << s.resolution_scale << ",\n";
    json << "  \"present_effect\": \"" << json_escape(s.present_effect) << "\",\n";
    json << "  \"swap_post_effect\": \"" << json_escape(s.swap_post_effect) << "\",\n";
    json << "  \"anisotropic_override\": " << s.anisotropic_override << ",\n";
    json << "  \"present_dither\": " << toml_bool(s.present_dither) << ",\n";
    json << "  \"vsync\": " << toml_bool(s.vsync) << ",\n";
    json << "  \"fullscreen\": " << toml_bool(s.fullscreen) << ",\n";
    json << "  \"async_shader_compilation\": " << toml_bool(s.async_shader_compilation) << ",\n";
    json << "  \"d3d12_pipeline_creation_threads\": " << s.d3d12_pipeline_creation_threads << ",\n";
    json << "  \"texture_cache_memory_limit_soft\": " << s.tex_soft << ",\n";
    json << "  \"texture_cache_memory_limit_hard\": " << s.tex_hard;
    for (int i = 0; i < kBindCount; i++) {
        json << ",\n  \"" << wide_to_u8(kKeyIds[i]) << "\": \"" << json_escape(wide_to_u8(s.binds[i]))
             << "\"";
    }
    json << "\n}\n";
    if (!write_file_u8(dir + L"\\launcher.json", json.str())) return false;

    std::ostringstream t;
    t << "# Сгенерировано лаунчером. F4 в игре перезапишет этот файл.\n";
    t << "gpu_plugin = \"xenos\"\n";
    t << "render_target_path_d3d12 = \"rov\"\n";
    t << "game_data_root = \"game\"\n";
    t << "occlusion_query_enable = true\n\n";
    t << "input_backend = " << toml_str(s.input_backend) << "\n";
    t << "guide_button = " << toml_bool(s.guide_button) << "\n";
    t << "mnk_mode = " << toml_bool(s.mnk_mode) << "\n";
    t << "mnk_mouse = " << toml_bool(s.mnk_mouse) << "\n";
    t << "mnk_sensitivity = " << s.mnk_sensitivity << "\n\n";
    t << "resolution_scale = " << s.resolution_scale << "\n";
    t << "present_effect = " << toml_str(s.present_effect) << "\n";
    t << "swap_post_effect = " << toml_str(s.swap_post_effect) << "\n";
    t << "anisotropic_override = " << s.anisotropic_override << "\n";
    t << "present_dither = " << toml_bool(s.present_dither) << "\n";
    t << "vsync = " << toml_bool(s.vsync) << "\n";
    t << "fullscreen = " << toml_bool(s.fullscreen) << "\n";
    t << "async_shader_compilation = " << toml_bool(s.async_shader_compilation) << "\n";
    t << "d3d12_pipeline_creation_threads = " << s.d3d12_pipeline_creation_threads << "\n";
    t << "texture_cache_memory_limit_soft = " << s.tex_soft << "\n";
    t << "texture_cache_memory_limit_hard = " << s.tex_hard << "\n\n";
    for (int i = 0; i < kBindCount; i++) {
        t << wide_to_u8(kKeyIds[i]) << " = " << toml_str(wide_to_u8(s.binds[i])) << "\n";
    }
    return write_file_u8(dir + L"\\broken_bond.toml", t.str());
}

HWND Mk(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id,
        DWORD ex = 0) {
    HWND hwnd = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h),
                                g_main, (HMENU)(INT_PTR)id, GetModuleHandleW(nullptr), nullptr);
    SendMessageW(hwnd, WM_SETFONT, (WPARAM)g_font, TRUE);
    return hwnd;
}

void combo_add(HWND cb, const wchar_t* label, LPARAM data) {
    int i = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)label);
    SendMessageW(cb, CB_SETITEMDATA, i, data);
}

void combo_sel_data(HWND cb, LPARAM data) {
    int n = (int)SendMessageW(cb, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < n; i++) {
        if (SendMessageW(cb, CB_GETITEMDATA, i, 0) == data) {
            SendMessageW(cb, CB_SETCURSEL, i, 0);
            return;
        }
    }
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
}

void combo_sel_str(HWND cb, const string& v, const char* const* values, int count) {
    for (int i = 0; i < count; i++) {
        if (v == values[i]) {
            SendMessageW(cb, CB_SETCURSEL, i, 0);
            return;
        }
    }
    SendMessageW(cb, CB_SETCURSEL, 0, 0);
}

LPARAM combo_data(HWND cb) {
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i < 0) return 0;
    return SendMessageW(cb, CB_GETITEMDATA, i, 0);
}

string combo_str(HWND cb, const char* const* values, int count) {
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= count) return values[0];
    return values[i];
}

const char* kPresentVals[] = {"bilinear", "cas", "fsr", "fsr2", "fsr3"};
const char* kFxaaVals[] = {"none", "fxaa", "fxaa_extreme"};
const char* kBackendVals[] = {"sdl", "xinput"};

std::vector<HWND> g_gfx;
std::vector<HWND> g_ctl;
HWND g_about = nullptr;

void update_scale_hint() {
    HWND h = GetDlgItem(g_main, IDC_SCALE_HINT);
    if (!h) return;
    int w = 0, ht = 0;
    desktop_size(w, ht);
    int rec = preset_resolution_scale("quality");
    const wchar_t* kind = L"Full HD";
    if (w >= 3800)
        kind = L"4K";
    else if (w >= 3000)
        kind = L"3K";
    else if (w >= 2500)
        kind = L"1440p / 2K";
    wchar_t buf[320];
    swprintf_s(buf,
               L"Монитор %d×%d (%s). Full HD: 1x или 2x. 1440p: 2x. 4K: 3x. Для качества сейчас %dx.",
               w, ht, kind, rec);
    SetWindowTextW(h, buf);
}

void show_tab(int idx) {
    for (HWND h : g_gfx) ShowWindow(h, idx == 0 ? SW_SHOW : SW_HIDE);
    for (HWND h : g_ctl) ShowWindow(h, idx == 1 ? SW_SHOW : SW_HIDE);
    if (g_about) ShowWindow(g_about, idx == 2 ? SW_SHOW : SW_HIDE);
}

wstring get_text(HWND h) {
    int n = GetWindowTextLengthW(h);
    wstring s(n, 0);
    GetWindowTextW(h, s.data(), n + 1);
    return s;
}

string current_preset() {
    if (IsDlgButtonChecked(g_main, IDC_R_PERF) == BST_CHECKED) return "performance";
    if (IsDlgButtonChecked(g_main, IDC_R_MED) == BST_CHECKED) return "medium";
    if (IsDlgButtonChecked(g_main, IDC_R_QUAL) == BST_CHECKED) return "quality";
    return "custom";
}

void set_preset_radio(const string& p) {
    CheckDlgButton(g_main, IDC_R_PERF, p == "performance" ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_R_MED, p == "medium" ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_R_QUAL, p == "quality" ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_R_CUSTOM, p == "custom" ? BST_CHECKED : BST_UNCHECKED);
}

void fill_binds_list(const Settings& s) {
    ListView_DeleteAllItems(g_lv);
    for (int i = 0; i < kBindCount; i++) {
        LVITEMW it{};
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = (LPWSTR)kKeyLabels[i];
        it.lParam = i;
        int row = ListView_InsertItem(g_lv, &it);
        ListView_SetItemText(g_lv, row, 1, (LPWSTR)s.binds[i].c_str());
    }
}

void cancel_capture();

void set_kb_enabled(bool on) {
    HWND ids[] = {
        GetDlgItem(g_main, IDC_MNK_MOUSE), GetDlgItem(g_main, IDC_SENS_LBL),
        GetDlgItem(g_main, IDC_SENS),      g_lv,
        GetDlgItem(g_main, IDC_BIND_HINT), GetDlgItem(g_main, IDC_BIND_ASSIGN),
        GetDlgItem(g_main, IDC_BIND_ADD),  GetDlgItem(g_main, IDC_BIND_CLEAR),
        GetDlgItem(g_main, IDC_BIND_RESET),
    };
    for (HWND h : ids) {
        if (h) EnableWindow(h, on ? TRUE : FALSE);
    }
    if (g_bind_hint) {
        SetWindowTextW(g_bind_hint,
                       on ? kHintIdle
                          : L"Раскладка геймпада как на Xbox 360. Кнопки пада переназначить нельзя.");
    }
}

void sync_input_ui() {
    bool pad_only = IsDlgButtonChecked(g_main, IDC_DEV_PAD) == BST_CHECKED;
    if (pad_only && g_capture_row >= 0) cancel_capture();
    set_kb_enabled(!pad_only);
}

void ui_from_settings(const Settings& s) {
    g_applying = true;
    combo_sel_data(GetDlgItem(g_main, IDC_SCALE), s.resolution_scale);
    combo_sel_str(GetDlgItem(g_main, IDC_PRESENT), s.present_effect, kPresentVals, 5);
    combo_sel_str(GetDlgItem(g_main, IDC_FXAA), s.swap_post_effect, kFxaaVals, 3);
    combo_sel_data(GetDlgItem(g_main, IDC_ANISO), s.anisotropic_override);
    combo_sel_data(GetDlgItem(g_main, IDC_THREADS), s.d3d12_pipeline_creation_threads);
    CheckDlgButton(g_main, IDC_FULLSCREEN, s.fullscreen ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_VSYNC, s.vsync ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_DITHER, s.present_dither ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_ASYNC, s.async_shader_compilation ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_DEV_KB, s.input_mode == "keyboard" ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_DEV_PAD, s.input_mode == "gamepad" ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_DEV_BOTH, s.input_mode == "both" ? BST_CHECKED : BST_UNCHECKED);
    combo_sel_str(GetDlgItem(g_main, IDC_BACKEND), s.input_backend, kBackendVals, 2);
    CheckDlgButton(g_main, IDC_GUIDE, s.guide_button ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(g_main, IDC_MNK_MOUSE, s.mnk_mouse ? BST_CHECKED : BST_UNCHECKED);
    wchar_t buf[32];
    swprintf_s(buf, L"%.2f", s.mnk_sensitivity);
    SetWindowTextW(GetDlgItem(g_main, IDC_SENS), buf);
    fill_binds_list(s);
    set_preset_radio(s.preset);
    g_applying = false;
    sync_input_ui();
}

void cancel_capture();

Settings collect() {
    if (g_capture_row >= 0) cancel_capture();
    Settings s = defaults();
    s.preset = current_preset();
    s.resolution_scale = (int)combo_data(GetDlgItem(g_main, IDC_SCALE));
    s.present_effect = combo_str(GetDlgItem(g_main, IDC_PRESENT), kPresentVals, 5);
    s.swap_post_effect = combo_str(GetDlgItem(g_main, IDC_FXAA), kFxaaVals, 3);
    s.anisotropic_override = (int)combo_data(GetDlgItem(g_main, IDC_ANISO));
    s.d3d12_pipeline_creation_threads = (int)combo_data(GetDlgItem(g_main, IDC_THREADS));
    s.fullscreen = IsDlgButtonChecked(g_main, IDC_FULLSCREEN) == BST_CHECKED;
    s.vsync = IsDlgButtonChecked(g_main, IDC_VSYNC) == BST_CHECKED;
    s.present_dither = IsDlgButtonChecked(g_main, IDC_DITHER) == BST_CHECKED;
    s.async_shader_compilation = IsDlgButtonChecked(g_main, IDC_ASYNC) == BST_CHECKED;
    if (IsDlgButtonChecked(g_main, IDC_DEV_PAD) == BST_CHECKED)
        s.input_mode = "gamepad";
    else if (IsDlgButtonChecked(g_main, IDC_DEV_BOTH) == BST_CHECKED)
        s.input_mode = "both";
    else
        s.input_mode = "keyboard";
    s.mnk_mode = s.input_mode != "gamepad";
    s.input_backend = combo_str(GetDlgItem(g_main, IDC_BACKEND), kBackendVals, 2);
    s.guide_button = IsDlgButtonChecked(g_main, IDC_GUIDE) == BST_CHECKED;
    s.mnk_mouse = IsDlgButtonChecked(g_main, IDC_MNK_MOUSE) == BST_CHECKED;
    try {
        s.mnk_sensitivity = std::stod(wide_to_u8(get_text(GetDlgItem(g_main, IDC_SENS))));
    } catch (...) {
        s.mnk_sensitivity = 1.0;
    }
    if (s.mnk_sensitivity < 0.2) s.mnk_sensitivity = 0.2;
    if (s.mnk_sensitivity > 3.0) s.mnk_sensitivity = 3.0;
    for (int i = 0; i < kBindCount; i++) {
        wchar_t buf[256]{};
        ListView_GetItemText(g_lv, i, 1, buf, 256);
        s.binds[i] = buf;
    }
    if (s.preset == "performance") {
        s.tex_soft = 384;
        s.tex_hard = 768;
    } else {
        s.tex_soft = 1024;
        s.tex_hard = 2048;
    }
    return s;
}

void mark_custom() {
    if (g_applying) return;
    g_applying = true;
    set_preset_radio("custom");
    g_applying = false;
}

void apply_named_preset(const string& name) {
    if (g_applying || name == "custom") return;
    Settings s = collect();
    s.preset = name;
    apply_preset_values(s, name);
    ui_from_settings(s);
}

bool is_modifier_vk(DWORD vk) {
    return vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT || vk == VK_CONTROL ||
           vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_MENU || vk == VK_LMENU ||
           vk == VK_RMENU;
}

wstring vk_to_name(DWORD vk) {
    if (vk >= 'A' && vk <= 'Z') return wstring(1, (wchar_t)vk);
    if (vk >= '0' && vk <= '9') return wstring(1, (wchar_t)vk);
    if (vk >= VK_F1 && vk <= VK_F24) {
        wchar_t b[8];
        swprintf_s(b, L"F%d", (int)(vk - VK_F1 + 1));
        return b;
    }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        wchar_t b[16];
        swprintf_s(b, L"Numpad%d", (int)(vk - VK_NUMPAD0));
        return b;
    }
    switch (vk) {
        case VK_SPACE:
            return L"Space";
        case VK_RETURN:
            return L"Return";
        case VK_BACK:
            return L"Backspace";
        case VK_TAB:
            return L"Tab";
        case VK_DELETE:
            return L"Delete";
        case VK_INSERT:
            return L"Insert";
        case VK_HOME:
            return L"Home";
        case VK_END:
            return L"End";
        case VK_PRIOR:
            return L"PageUp";
        case VK_NEXT:
            return L"PageDown";
        case VK_LEFT:
            return L"Left";
        case VK_RIGHT:
            return L"Right";
        case VK_UP:
            return L"Up";
        case VK_DOWN:
            return L"Down";
        case VK_OEM_3:
            return L"Backtick";
        case VK_OEM_MINUS:
            return L"Minus";
        case VK_OEM_PLUS:
            return L"Plus";
        case VK_OEM_COMMA:
            return L"Comma";
        case VK_OEM_PERIOD:
            return L"Period";
        case VK_OEM_1:
            return L"Semicolon";
        case VK_OEM_2:
            return L"Slash";
        case VK_OEM_5:
            return L"Backslash";
        case VK_OEM_4:
            return L"LBracket";
        case VK_OEM_6:
            return L"RBracket";
        case VK_OEM_7:
            return L"Quote";
        case VK_ADD:
            return L"NumpadPlus";
        case VK_SUBTRACT:
            return L"NumpadMinus";
        case VK_MULTIPLY:
            return L"NumpadStar";
        case VK_DIVIDE:
            return L"NumpadSlash";
        case VK_SNAPSHOT:
            return L"PrintScreen";
        case VK_PAUSE:
            return L"Pause";
        case VK_CAPITAL:
            return L"CapsLock";
        case VK_NUMLOCK:
            return L"NumLock";
        case VK_SCROLL:
            return L"ScrollLock";
        default:
            return {};
    }
}

bool list_has_token(const wstring& list, const wstring& key) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(L',', start);
        wstring t = list.substr(start, comma == wstring::npos ? wstring::npos : comma - start);
        while (!t.empty() && t.front() == L' ') t.erase(t.begin());
        while (!t.empty() && t.back() == L' ') t.pop_back();
        if (t == key) return true;
        if (comma == wstring::npos) break;
        start = comma + 1;
    }
    return false;
}

void set_bind_hint(const wchar_t* text) {
    if (g_bind_hint) SetWindowTextW(g_bind_hint, text);
}

void stop_capture_hook() {
    if (g_kb_hook) {
        UnhookWindowsHookEx(g_kb_hook);
        g_kb_hook = nullptr;
    }
}

void cancel_capture() {
    if (g_capture_row < 0) return;
    stop_capture_hook();
    wchar_t buf[256]{};
    wcsncpy_s(buf, g_capture_prev.c_str(), _TRUNCATE);
    ListView_SetItemText(g_lv, g_capture_row, 1, buf);
    g_capture_row = -1;
    set_bind_hint(kHintIdle);
}

void commit_capture(const wstring& key) {
    if (g_capture_row < 0) return;
    stop_capture_hook();
    int row = g_capture_row;
    g_capture_row = -1;
    wstring result = key;
    if (g_capture_append && !g_capture_prev.empty()) {
        result = list_has_token(g_capture_prev, key) ? g_capture_prev : g_capture_prev + L"," + key;
    }
    wchar_t buf[256]{};
    wcsncpy_s(buf, result.c_str(), _TRUNCATE);
    ListView_SetItemText(g_lv, row, 1, buf);
    set_bind_hint(kHintIdle);
}

LRESULT CALLBACK KbHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && g_capture_row >= 0 && (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN)) {
        auto* k = (KBDLLHOOKSTRUCT*)lp;
        DWORD vk = k->vkCode;
        if (vk == VK_ESCAPE) {
            PostMessageW(g_main, WM_CAPTURE_CANCEL, 0, 0);
            return 1;
        }
        if (is_modifier_vk(vk)) return 1;
        if (g_capture_posted) return 1;
        wstring name = vk_to_name(vk);
        if (name.empty()) return 1;
        wstring chord;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) chord += L"Shift+";
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) chord += L"Ctrl+";
        if (GetAsyncKeyState(VK_MENU) & 0x8000) chord += L"Alt+";
        chord += name;
        g_pending_key = chord;
        g_capture_posted = true;
        PostMessageW(g_main, WM_CAPTURE_KEY, 0, 0);
        return 1;
    }
    return CallNextHookEx(g_kb_hook, code, wp, lp);
}

bool start_capture(bool append) {
    if (IsDlgButtonChecked(g_main, IDC_DEV_PAD) == BST_CHECKED) return false;
    int i = ListView_GetNextItem(g_lv, -1, LVNI_SELECTED);
    if (i < 0) {
        MessageBoxW(g_main, L"Сначала выбери действие в списке.", L"Лаунчер",
                    MB_OK | MB_ICONINFORMATION);
        return false;
    }
    if (g_capture_row >= 0) cancel_capture();
    wchar_t prev[256]{};
    ListView_GetItemText(g_lv, i, 1, prev, 256);
    g_capture_row = i;
    g_capture_append = append;
    g_capture_posted = false;
    g_capture_prev = prev;
    ListView_SetItemText(g_lv, i, 1, (LPWSTR)L"Нажми клавишу…");
    set_bind_hint(append ? L"Жду дополнительную клавишу. Esc — отмена."
                         : L"Жду клавишу. Esc — отмена.");
    SetFocus(g_main);
    g_kb_hook = SetWindowsHookExW(WH_KEYBOARD_LL, KbHook, GetModuleHandleW(nullptr), 0);
    if (!g_kb_hook) {
        cancel_capture();
        MessageBoxW(g_main, L"Не удалось начать захват клавиши.", L"Ошибка", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

void clear_selected_bind() {
    int i = ListView_GetNextItem(g_lv, -1, LVNI_SELECTED);
    if (i < 0) {
        MessageBoxW(g_main, L"Сначала выбери действие в списке.", L"Лаунчер",
                    MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (g_capture_row >= 0) cancel_capture();
    ListView_SetItemText(g_lv, i, 1, (LPWSTR)L"");
}

bool file_exists(const wstring& p) {
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool launch_game(const Settings& s) {
    wstring dir = exe_dir();
    wstring exe = dir + L"\\broken_bond.exe";
    wstring xex = dir + L"\\game\\default.xex";
    if (!file_exists(exe)) {
        MessageBoxW(g_main, L"Нет broken_bond.exe рядом с лаунчером.", L"Ошибка", MB_OK | MB_ICONERROR);
        return false;
    }
    if (!file_exists(xex)) {
        MessageBoxW(g_main, L"Нет game\\default.xex.", L"Ошибка", MB_OK | MB_ICONERROR);
        return false;
    }
    CreateDirectoryW((dir + L"\\logs").c_str(), nullptr);

    wstring game = dir + L"\\game";
    wstring log = dir + L"\\logs\\run.log";
    wchar_t cmd[4096];
    swprintf_s(cmd,
               L"\"%s\" --game_data_root=\"%s\" --gpu_plugin=xenos --render_target_path_d3d12=rov "
               L"--resolution_scale=%d --present_effect=%S --swap_post_effect=%S --log_file=\"%s\" "
               L"--log_level=info --input_backend=%S %s %s",
               exe.c_str(), game.c_str(), s.resolution_scale, s.present_effect.c_str(),
               s.swap_post_effect.c_str(), log.c_str(), s.input_backend.c_str(),
               s.mnk_mode ? L"--mnk_mode" : L"--no-mnk_mode",
               s.guide_button ? L"--guide_button" : L"--no-guide_button");

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        MessageBoxW(g_main, L"Не удалось запустить игру.", L"Ошибка", MB_OK | MB_ICONERROR);
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

void add_tab(HWND tabs, int idx, const wchar_t* title) {
    TCITEMW it{};
    it.mask = TCIF_TEXT;
    it.pszText = (LPWSTR)title;
    TabCtrl_InsertItem(tabs, idx, &it);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g_main = hwnd;
            g_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH, L"Segoe UI");
            HWND title = Mk(L"STATIC", L"Naruto: The Broken Bond", 0, 16, 10, 500, 26, 0);
            HFONT big = CreateFontW(-S(22), 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH, L"Segoe UI");
            SendMessageW(title, WM_SETFONT, (WPARAM)big, TRUE);
            Mk(L"STATIC", L"ReXGlue · лаунчер", 0, 16, 38, 400, 18, 0);

            Mk(L"BUTTON", L"Пресет графики", BS_GROUPBOX, 16, 62, 748, 118, 0);
            Mk(L"BUTTON", L"Производительность  —  натив, меньше фризов (любой монитор)",
               BS_AUTORADIOBUTTON | WS_GROUP, 28, 82, 720, 22, IDC_R_PERF);
            Mk(L"BUTTON", L"Среднее  —  под монитор, CAS, без лишнего апскейла", BS_AUTORADIOBUTTON,
               28, 104, 720, 22, IDC_R_MED);
            Mk(L"BUTTON", L"Качество  —  под монитор (Full HD/2K: 2x, 4K: 3x)",
               BS_AUTORADIOBUTTON, 28, 126, 720, 22, IDC_R_QUAL);
            Mk(L"BUTTON", L"Свои настройки  —  вкладки ниже", BS_AUTORADIOBUTTON, 28, 148, 720, 22,
               IDC_R_CUSTOM);

            g_tabs = Mk(WC_TABCONTROLW, L"", WS_CLIPSIBLINGS, 16, 190, 748, 28, IDC_TABS);
            add_tab(g_tabs, 0, L"Графика");
            add_tab(g_tabs, 1, L"Управление");
            add_tab(g_tabs, 2, L"Справка");

            auto gfx = [&](HWND h) {
                g_gfx.push_back(h);
                return h;
            };
            auto ctl = [&](HWND h) {
                g_ctl.push_back(h);
                return h;
            };

            gfx(Mk(L"STATIC", L"Внутреннее разрешение", 0, 28, 226, 240, 20, 0));
            HWND scale = gfx(Mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 280, 222, 300, 220,
                                IDC_SCALE));
            combo_add(scale, L"1x  1280×512     Full HD, максимум FPS", 1);
            combo_add(scale, L"2x  2560×1024    1440p и Full HD (резче)", 2);
            combo_add(scale, L"3x  3840×1536    4K", 3);
            combo_add(scale, L"4x  5120×2048    очень тяжело", 4);
            combo_add(scale, L"5x  6400×2560    экстремально", 5);
            gfx(Mk(L"BUTTON", L"По монитору", 0, 588, 222, 160, 26, IDC_SCALE_FIT));
            gfx(Mk(L"STATIC", L"", 0, 28, 250, 720, 18, IDC_SCALE_HINT));

            gfx(Mk(L"STATIC", L"Фильтр вывода на экран", 0, 28, 276, 240, 20, 0));
            HWND present = gfx(Mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 280, 272, 360, 200,
                                  IDC_PRESENT));
            combo_add(present, L"Билинейный", 0);
            combo_add(present, L"CAS (резкость)", 1);
            combo_add(present, L"FSR 1", 2);
            combo_add(present, L"FSR 2", 3);
            combo_add(present, L"FSR 3", 4);

            gfx(Mk(L"STATIC", L"Сглаживание кадра (FXAA)", 0, 28, 308, 240, 20, 0));
            HWND fxaa =
                gfx(Mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 280, 304, 360, 200, IDC_FXAA));
            combo_add(fxaa, L"Нет", 0);
            combo_add(fxaa, L"FXAA", 1);
            combo_add(fxaa, L"FXAA Extreme", 2);

            gfx(Mk(L"STATIC", L"Анизотропная фильтрация", 0, 28, 340, 240, 20, 0));
            HWND aniso =
                gfx(Mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 280, 336, 360, 200, IDC_ANISO));
            combo_add(aniso, L"Как в игре", -1);
            combo_add(aniso, L"Выкл.", 0);
            combo_add(aniso, L"2x", 2);
            combo_add(aniso, L"4x", 3);
            combo_add(aniso, L"8x", 4);
            combo_add(aniso, L"16x", 5);

            gfx(Mk(L"STATIC", L"Потоки компиляции PSO", 0, 28, 372, 240, 20, 0));
            HWND thr = gfx(
                Mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 280, 368, 360, 200, IDC_THREADS));
            combo_add(thr, L"Авто (−1)", -1);
            combo_add(thr, L"4", 4);
            combo_add(thr, L"8", 8);
            combo_add(thr, L"16", 16);

            gfx(Mk(L"BUTTON", L"Полный экран", BS_AUTOCHECKBOX, 28, 408, 300, 22, IDC_FULLSCREEN));
            gfx(Mk(L"BUTTON", L"Вертикальная синхронизация (VSync)", BS_AUTOCHECKBOX, 28, 434, 400, 22,
                   IDC_VSYNC));
            gfx(Mk(L"BUTTON", L"Дизеринг (меньше полос на градиентах)", BS_AUTOCHECKBOX, 28, 460, 420,
                   22, IDC_DITHER));
            gfx(Mk(L"BUTTON", L"Асинхронная компиляция шейдеров", BS_AUTOCHECKBOX, 28, 486, 400, 22,
                   IDC_ASYNC));
            gfx(Mk(L"STATIC",
                   L"Игра рендерит 1280×512×N, потом растягивается на монитор. Full HD: 1x или 2x. "
                   L"1440p: 2x. 4K: 3x. 4x+ очень тяжело (ещё и 4xMSAA). ROV для NVIDIA всегда. "
                   L"FXAA мылит — лучше CAS или «Нет».",
                   0, 28, 514, 720, 48, IDC_GFX_NOTE));

            ctl(Mk(L"BUTTON", L"Клавиатура и мышь", BS_AUTORADIOBUTTON | WS_GROUP, 28, 226, 240, 22,
                   IDC_DEV_KB));
            ctl(Mk(L"BUTTON", L"Геймпад", BS_AUTORADIOBUTTON, 280, 226, 140, 22, IDC_DEV_PAD));
            ctl(Mk(L"BUTTON", L"Клавиатура + геймпад", BS_AUTORADIOBUTTON, 430, 226, 250, 22,
                   IDC_DEV_BOTH));
            ctl(Mk(L"STATIC", L"Драйвер геймпада", 0, 28, 254, 160, 20, 0));
            HWND backend = ctl(Mk(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_VSCROLL, 200, 250, 320, 200,
                                  IDC_BACKEND));
            combo_add(backend, L"SDL — Xbox / DualSense / Switch", 0);
            combo_add(backend, L"XInput — только Xbox-совместимые", 1);
            ctl(Mk(L"BUTTON", L"Кнопка Guide", BS_AUTOCHECKBOX, 540, 252, 160, 22, IDC_GUIDE));
            ctl(Mk(L"BUTTON", L"Мышь = правый стик (камера)", BS_AUTOCHECKBOX, 28, 282, 280, 22,
                   IDC_MNK_MOUSE));
            ctl(Mk(L"STATIC", L"Чувствительность", 0, 320, 284, 140, 20, IDC_SENS_LBL));
            ctl(Mk(L"EDIT", L"1.00", ES_LEFT | WS_BORDER, 470, 280, 80, 24, IDC_SENS, WS_EX_CLIENTEDGE));

            g_lv = ctl(CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                       WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL |
                                           LVS_SHOWSELALWAYS,
                                       S(28), S(312), S(720), S(200), hwnd, (HMENU)IDC_BINDS,
                                       GetModuleHandleW(nullptr), nullptr));
            SendMessageW(g_lv, WM_SETFONT, (WPARAM)g_font, TRUE);
            ListView_SetExtendedListViewStyle(g_lv, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            LVCOLUMNW col{};
            col.mask = LVCF_TEXT | LVCF_WIDTH;
            col.pszText = (LPWSTR)L"Действие";
            col.cx = S(280);
            ListView_InsertColumn(g_lv, 0, &col);
            col.pszText = (LPWSTR)L"Клавиши";
            col.cx = S(400);
            ListView_InsertColumn(g_lv, 1, &col);

            ctl(Mk(L"STATIC", kHintIdle, 0, 28, 516, 720, 18, IDC_BIND_HINT));
            g_bind_hint = GetDlgItem(hwnd, IDC_BIND_HINT);
            ctl(Mk(L"BUTTON", L"Назначить", 0, 28, 538, 120, 28, IDC_BIND_ASSIGN));
            ctl(Mk(L"BUTTON", L"Добавить клавишу", 0, 156, 538, 150, 28, IDC_BIND_ADD));
            ctl(Mk(L"BUTTON", L"Очистить", 0, 314, 538, 100, 28, IDC_BIND_CLEAR));
            ctl(Mk(L"BUTTON", L"Сбросить все", 0, 422, 538, 130, 28, IDC_BIND_RESET));

            g_about = Mk(
                L"STATIC",
                L"Порт на PC через ReXGlue.\r\n\r\n"
                L"• Сохранения: Документы\\broken_bond\\\r\n"
                L"• Не нажимай F4 «Save to config» в игре — собьёт конфиг лаунчера.\r\n"
                L"• Первый запуск и новые приёмы компилируют шейдеры (короткий фриз).\r\n"
                L"• Геймпад: раскладка Xbox 360. DualSense/Switch — драйвер SDL.\r\n"
                L"• A = Space / точка с запятой, Start = Enter / X.\r\n"
                L"• Японская и английская озвучка должны проходить катсцены.",
                0, 28, 230, 720, 200, IDC_ABOUT);

            Mk(L"BUTTON", L"Сохранить", 0, 16, 575, 140, 32, IDC_SAVE);
            Mk(L"BUTTON", L"Сохранить и запустить", BS_DEFPUSHBUTTON, 560, 575, 204, 32, IDC_PLAY);

            ui_from_settings(load_settings(exe_dir()));
            update_scale_hint();
            show_tab(0);
            return 0;
        }
        case WM_NOTIFY: {
            auto* hdr = (NMHDR*)lp;
            if (hdr->idFrom == IDC_TABS && hdr->code == TCN_SELCHANGE) {
                if (g_capture_row >= 0) cancel_capture();
                show_tab(TabCtrl_GetCurSel(g_tabs));
            }
            if (hdr->idFrom == IDC_BINDS && hdr->code == NM_DBLCLK) {
                if (IsDlgButtonChecked(g_main, IDC_DEV_PAD) == BST_CHECKED) return 0;
                auto* ia = (NMITEMACTIVATE*)lp;
                if (ia->iItem >= 0) {
                    ListView_SetItemState(g_lv, ia->iItem, LVIS_SELECTED | LVIS_FOCUSED,
                                          LVIS_SELECTED | LVIS_FOCUSED);
                    start_capture(false);
                }
            }
            return 0;
        }
        case WM_CAPTURE_CANCEL:
            cancel_capture();
            return 0;
        case WM_CAPTURE_KEY:
            commit_capture(g_pending_key);
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            int code = HIWORD(wp);
            if (code == CBN_SELCHANGE &&
                (id == IDC_SCALE || id == IDC_PRESENT || id == IDC_FXAA || id == IDC_ANISO ||
                 id == IDC_THREADS)) {
                mark_custom();
            }
            if (code == BN_CLICKED) {
                if (id == IDC_FULLSCREEN || id == IDC_VSYNC || id == IDC_DITHER || id == IDC_ASYNC) {
                    mark_custom();
                }
                if (id == IDC_R_PERF) apply_named_preset("performance");
                if (id == IDC_R_MED) apply_named_preset("medium");
                if (id == IDC_R_QUAL) apply_named_preset("quality");
                if (id == IDC_SCALE_FIT) {
                    combo_sel_data(GetDlgItem(g_main, IDC_SCALE), preset_resolution_scale("quality"));
                    mark_custom();
                }
                if (id == IDC_DEV_KB || id == IDC_DEV_PAD || id == IDC_DEV_BOTH) sync_input_ui();
                if (id == IDC_BIND_ASSIGN) start_capture(false);
                if (id == IDC_BIND_ADD) start_capture(true);
                if (id == IDC_BIND_CLEAR) clear_selected_bind();
                if (id == IDC_BIND_RESET) {
                    Settings s = collect();
                    for (int i = 0; i < kBindCount; i++) s.binds[i] = kKeyDefaults[i];
                    fill_binds_list(s);
                }
                if (id == IDC_SAVE) {
                    Settings s = collect();
                    if (!save_all(exe_dir(), s)) {
                        MessageBoxW(hwnd, L"Не удалось сохранить конфиг.", L"Ошибка",
                                    MB_OK | MB_ICONERROR);
                    } else {
                        MessageBoxW(hwnd, L"Настройки сохранены в broken_bond.toml", L"Лаунчер",
                                    MB_OK | MB_ICONINFORMATION);
                    }
                }
                if (id == IDC_PLAY) {
                    Settings s = collect();
                    if (!save_all(exe_dir(), s)) {
                        MessageBoxW(hwnd, L"Не удалось сохранить конфиг.", L"Ошибка",
                                    MB_OK | MB_ICONERROR);
                        break;
                    }
                    if (launch_game(s)) DestroyWindow(hwnd);
                }
            }
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, GetSysColor(COLOR_WINDOW));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        case WM_DESTROY:
            stop_capture_hook();
            g_capture_row = -1;
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_dpi = GetDpiForSystem();
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TAB_CLASSES | ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.lpszClassName = L"BrokenBondLauncher";
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, S(796), S(656)};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd =
        CreateWindowW(L"BrokenBondLauncher", L"Naruto: The Broken Bond", style,
                      CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, nullptr,
                      nullptr, inst, nullptr);
    ShowWindow(hwnd, show);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (g_capture_row >= 0 || !IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return (int)msg.wParam;
}
