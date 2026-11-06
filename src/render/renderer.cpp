#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <bit>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <filesystem>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <ranges>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <fmt/ranges.h>
#include <rex/cvar.h>
#include <plume_render_interface.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <smolv.h>
#include <zstd.h>

#include "gfx/shader_cache.h"
#include "gfx/texture.h"
#include "gfx/xenos_shader_common.h"
#include <bit>
#if defined(_WIN32)
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int key);
#endif
#include <unordered_set>

#include <rex/logging.h>

#include "render/interpolation.h"

#include "depth_resolve_ps.h"
#include "depth_resolve_vs.h"
#include "present_ps.h"
#include "present_vs.h"
#include "host/shadow_draw.h"

#if defined(BROKEN_BOND_HAS_ADRENOTOOLS)
#include <dlfcn.h>

#include <adrenotools/driver.h>
#endif
#if defined(__ANDROID__)
#include <cxxabi.h>
#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
void SetVulkanGetInstanceProcAddr(void* getInstanceProcAddr);
}

REXCVAR_DEFINE_INT32(native_dump_frame, 0, "Game",
                     "With renderer=native, describe every draw of this present (from 1) in "
                     "logs/native-frame-<n>.txt; 0 writes none")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_decl_swizzle, true, "Game",
                    "With renderer=native, apply the vertex declaration swizzle (D3DCOLOR is zyxw) "
                    "while gathering streams")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(android_big_cores, false, "Game",
                    "On Android, keep the game and recording threads on the fastest CPU cores and tell the system "
                    "(performance hint) how long their frames take, so it does not move them to slow cores or "
                    "lower the clock mid-fight")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(android_profile, false, "Game",
                    "On Android, sample the game thread's call stack 1000 times a second and log the functions it "
                    "spends its time in every 5 s (a profiler for a phone with no adb at hand)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(android_vulkan_driver, "", "Game",
                      "Android arm64: a custom Vulkan driver (the .so of an AdrenoTools package such as Turnip, "
                      "in the app's private folder) loaded through libadrenotools; empty uses the system driver")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(android_native_lib_dir, "", "Game",
                      "Android: the app's native library folder, which holds libadrenotools' hook libraries")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(native_resolution_scale, 0.0, "Game",
                      "With renderer=native, the internal resolution as a multiple of the game's 1280x720 "
                      "(1 is 720p, 1.5 is 1080p); 0 matches the window")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_interpolate, false, "Game",
                    "With renderer=native, draw an in-between frame when the game steps at 30 Hz "
                    "(shows 60 FPS one step late)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_interpolate_blend, "motion,bones", "Game",
                      "With native_interpolate, what the in-between frame blends: comma-separated motion (the "
                      "vertex shader's matrices and skin bones), vs (all vertex constants), ps (pixel constants), "
                      "bones (bone textures). Left-out parts use the real frame's")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_interpolate_debug, false, "Game",
                    "With native_interpolate, show the in-between frame in place of the real one too (to compare a "
                    "replayed frame with a real one)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(native_interpolate_dump, 0, "Game",
                     "With native_interpolate, save this many pairs of a real frame and its in-between frame, one "
                     "pair every 900 frames, as logs/interp-N-{real,mid}.rgba (half size, BGRA)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_record_thread, false, "Game",
                    "With renderer=native, record Vulkan commands on a second thread: the game thread keeps "
                    "reading its memory and queues each draw's commands (a phone's fight frame is bound by them)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_draw_timing, true, "Game",
                    "With renderer=native, time each section of a draw for the 5 s stats (laps); a phone spends "
                    "a few percent of the game thread reading the clock for them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_trim_constants, false, "Game",
                    "With renderer=native, copy only the float constants each shader's constant table covers "
                    "instead of all 256 vertex and 224 pixel registers on every draw")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_flat_maps, false, "Game",
                    "With renderer=native, keep the per-draw caches (vertex streams, index lists, watched ranges) "
                    "in flat open-addressing tables instead of std::unordered_map")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_clear_new_textures, false, "Game",
                    "With renderer=native, fill a texture a resolve creates with zeros first, so the texels the "
                    "resolve does not reach are transparent black, as fresh memory reads on a PC, and not "
                    "leftover memory (colored checkers on a phone)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_blank_descriptors, false, "Game",
                    "With renderer=native, point every texture slot not yet given a texture at a transparent black "
                    "1x1 one, so a shader sampling such a slot reads zeros as on a PC driver and not stray memory "
                    "(colored checkers on a phone)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_watch_memory, false, "Game",
                    "With renderer=native, write-protect the guest pages of vertex and index data once hashed, so "
                    "the same data is not hashed again every frame until the game writes to it")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(native_frames_in_flight, 2, "Game",
                     "With renderer=native, frames the GPU may still be drawing while the next is recorded: 2 lets "
                     "the CPU record the next one meanwhile, 1 waits for each frame")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(native_msaa, 4, "Game",
                     "With renderer=native, the most samples a multisampled guest surface gets on the "
                     "host (1 turns MSAA off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_skip_shaders, "", "Game",
                      "With renderer=native, skip the draws whose vertex or pixel shader key starts with "
                      "one of these comma-separated hex prefixes (to find which pass draws something)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_skip_verts, "", "Game",
                      "With renderer=native, skip the draws that use one of these comma-separated "
                      "vertex counts (to find which mesh is which)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(native_dump_mesh, 0, "Game",
                     "With a frame dump, write every vertex of the draws with this many vertices to "
                     "logs/mesh-<draw>-<verts>.csv, decoded as the vertex shader gets them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_tint_verts, "", "Game",
                      "With renderer=native, tint the draws that use these comma-separated vertex counts: "
                      "the first writes only red, the second green, the third blue, the fourth red and green")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_dump_texture, "", "Game",
                      "With renderer=native, write the untiled texels of the 2D textures at this hex "
                      "physical address, or of this <w>x<h> size, to logs/texture-<address>-f<format>-<w>x<h>.bin "
                      "when they are uploaded")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_dump_constants, "", "Game",
                      "With a frame dump, list every non-zero vertex and pixel shader constant of the "
                      "draws whose vertex or pixel shader key starts with one of these comma-separated "
                      "hex prefixes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_dump_outline, false, "Game",
                    "With renderer=native, dump the present 60 after the first draw that culls front "
                    "faces (character outlines) as native_dump_frame would")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_debug_target, false, "Game",
                    "Debug: skip guest clears and show the render target itself at Swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace broken_bond::render {

namespace {

// Adds the wall time of a scope to a millisecond counter.
struct ScopedMs {
  double& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  explicit ScopedMs(double& counter) : total(counter) {}
  ~ScopedMs() {
    total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }
};

namespace xenos = rex::graphics::xenos;
using broken_bond::gfx::DeclUsage;

constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::B8G8R8A8_UNORM;
// Only the present pass writes the swap chain. Android surfaces offer RGBA8
// (BGRA8 rarely), so there the swap chain differs from the targets.
#if defined(__ANDROID__)
constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::R8G8B8A8_UNORM;
#else
constexpr plume::RenderFormat kSwapChainFormat = kColorFormat;
#endif
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
// What a guest depth resolve becomes on the host: the depth value itself.
constexpr plume::RenderFormat kDepthCopyFormat = plume::RenderFormat::R32_FLOAT;
// Interpolation queues two presents per guest frame. With two more images
// per frame in flight than the display holds, the next frame acquires its
// two while the earlier ones wait for the display, so the guest thread
// (whose logic steps once per frame) never waits for a vblank.
constexpr uint32_t kSwapChainBuffers = 2;
constexpr uint32_t kInterpolationSwapChainBuffersPerFrame = 2;
constexpr uint32_t kInterpolationSwapChainBuffersExtra = 2;
constexpr uint64_t kUploadBytes = 256ull << 20;
constexpr uint32_t kTexture2DCount = 8192;
constexpr uint32_t kTexture3DCount = 64;
constexpr uint32_t kTextureCubeCount = 256;
constexpr uint32_t kTexture1DCount = 1024;
constexpr uint32_t kSamplerCount = 1024;

// Guest frame the D3D viewport registers describe; the host target scales it.
constexpr float kGuestWidth = 1280.0f;
constexpr float kGuestHeight = 720.0f;

// ---------------------------------------------------------------------------
// Guest memory and the D3D device (docs/D3D_MAP.md).

const uint8_t* At(const uint8_t* base, uint32_t address) {
  return rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
}

uint32_t Be32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = At(base, address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t Be16(const uint8_t* base, uint32_t address) {
  const uint8_t* p = At(base, address);
  return uint16_t((p[0] << 8) | p[1]);
}

float BeFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Be32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// Fetch constants hold physical addresses (below 0x20000000), which the
// 0xA0000000 view reaches unshifted. Some D3D objects (index buffers) keep a
// virtual address in the 0xE0000000 range instead; that range maps physical
// memory 0x1000 further on, so it is read as the virtual address it is.
// True when the vertex or pixel shader key starts with one of the
// comma-separated hex prefixes in `list`.
bool ShaderListed(std::string_view list, uint64_t vs_key, uint64_t ps_key) {
  if (list.empty()) return false;
  const std::string vs_hex = fmt::format("{:016X}", vs_key);
  const std::string ps_hex = fmt::format("{:016X}", ps_key);
  for (const auto part : std::views::split(list, ',')) {
    const std::string_view want(part.begin(), part.end());
    if (!want.empty() && (vs_hex.starts_with(want) || ps_hex.starts_with(want))) return true;
  }
  return false;
}

uint32_t PhysicalView(uint32_t address) {
  return address < 0x20000000u ? 0xA0000000u + address : address;
}

// Write watch over guest physical memory (native_watch_memory). A guest
// write to a watched page faults once; the SDK then calls OnGuestWrite with
// the page range and lifts the protection. Each 4 KB page keeps the clock
// value of its last write, so a hash taken at clock C still describes the
// bytes while every page it covers is at C or below.
//
// Protecting pages and taking the fault both hold the SDK's global lock and
// change page protection, which on a phone stalls other threads (audio
// among them). So a page written to in the last kWatchHotFrames frames is
// "hot" and is not protected again until it cools down; ranges over it are
// hashed unwatched meanwhile.
constexpr uint32_t kWatchPageShift = 12;
constexpr uint32_t kWatchPages = 0x20000000u >> kWatchPageShift;
constexpr uint32_t kWatchHotFrames = 120;
std::atomic<uint32_t> g_watch_clock{1};
std::unique_ptr<std::atomic<uint32_t>[]> g_watch_page_clock;
// Frame of each page's last write plus one; 0 for never.
std::unique_ptr<std::atomic<uint32_t>[]> g_watch_page_frame;
std::atomic<uint32_t> g_watch_frame{0};
std::atomic<uint64_t> g_watch_faults{0};

std::pair<uint32_t, uint32_t> OnGuestWrite(void*, uint32_t physical, uint32_t length, bool) {
  const uint32_t clock = g_watch_clock.fetch_add(1, std::memory_order_acq_rel) + 1;
  const uint32_t last = std::min<uint64_t>((uint64_t(physical) + std::max(length, 1u) - 1) >> kWatchPageShift,
                                           kWatchPages - 1);
  const uint32_t frame = g_watch_frame.load(std::memory_order_relaxed) + 1;
  for (uint32_t page = physical >> kWatchPageShift; page <= last; ++page) {
    g_watch_page_clock[page].store(clock, std::memory_order_release);
    g_watch_page_frame[page].store(frame, std::memory_order_relaxed);
  }
  g_watch_faults.fetch_add(1, std::memory_order_relaxed);
  // Only the pages written to stop being watched.
  return {physical, length};
}

#if defined(__ANDROID__)
// The CPUs of every cluster but the slowest one, by their top clock.
cpu_set_t BigCores() {
  cpu_set_t set;
  CPU_ZERO(&set);
  std::vector<std::pair<int, long>> cpus;
  long lowest = LONG_MAX;
  const long count = sysconf(_SC_NPROCESSORS_CONF);
  for (int cpu = 0; cpu < count && cpu < CPU_SETSIZE; ++cpu) {
    FILE* f = std::fopen(fmt::format("/sys/devices/system/cpu/cpu{}/cpufreq/cpuinfo_max_freq", cpu).c_str(), "r");
    if (!f) continue;
    long khz = 0;
    if (std::fscanf(f, "%ld", &khz) == 1 && khz > 0) {
      cpus.emplace_back(cpu, khz);
      lowest = std::min(lowest, khz);
    }
    std::fclose(f);
  }
  for (const auto& [cpu, khz] : cpus) {
    if (khz > lowest) CPU_SET(cpu, &set);
  }
  return set;
}

void PinToBigCores(const char* thread) {
  cpu_set_t set = BigCores();
  if (CPU_COUNT(&set) == 0) return;
  std::string list;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (CPU_ISSET(cpu, &set)) list += fmt::format("{}{}", list.empty() ? "" : ",", cpu);
  }
  if (sched_setaffinity(0, sizeof(set), &set) == 0) {
    REXLOG_INFO("native: {} thread kept on CPUs {}", thread, list);
  } else {
    REXLOG_WARN("native: could not keep the {} thread on CPUs {}", thread, list);
  }
}

// Android's performance hint (ADPF, API 33): the threads of a session get
// the clock and cores their reported frame times call for. Loaded at run
// time since the APK also runs on API 30.
struct PerformanceHint {
  using ReportFn = int (*)(void*, int64_t);
  void* session = nullptr;
  ReportFn report = nullptr;

  void Start(const std::vector<int32_t>& threads, int64_t target_ns) {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    using GetManagerFn = void* (*)();
    using CreateFn = void* (*)(void*, const int32_t*, size_t, int64_t);
    auto get_manager = reinterpret_cast<GetManagerFn>(dlsym(lib, "APerformanceHint_getManager"));
    auto create = reinterpret_cast<CreateFn>(dlsym(lib, "APerformanceHint_createSession"));
    report = reinterpret_cast<ReportFn>(dlsym(lib, "APerformanceHint_reportActualWorkDuration"));
    void* manager = get_manager ? get_manager() : nullptr;
    session = manager && create && report ? create(manager, threads.data(), threads.size(), target_ns) : nullptr;
    REXLOG_INFO("native: performance hint session {}", session ? "started" : "unavailable");
  }

  void Report(int64_t ns) {
    if (session) report(session, ns);
  }
};
#endif

#if defined(__ANDROID__) && defined(__aarch64__)
// android_profile: a timer signal samples the game thread's program counter
// and its frame-pointer chain into a ring the 5 s stats drain. The handler
// only reads registers and the thread's own stack, within its bounds.
namespace profiler {
constexpr uint32_t kDepth = 16;
constexpr uint32_t kRing = 16384;  // 16 s at 1 kHz; drained every 5 s
struct Sample {
  uintptr_t pc[kDepth];
  uint32_t depth;
};
Sample g_ring[kRing];
std::atomic<uint32_t> g_head{0};
uint32_t g_tail = 0;
uintptr_t g_stack_low = 0, g_stack_high = 0;

uintptr_t Strip(uintptr_t address) { return address & ((uintptr_t(1) << 48) - 1); }

void OnSample(int, siginfo_t*, void* context) {
  const auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
  const uint32_t at = g_head.load(std::memory_order_relaxed);
  Sample& sample = g_ring[at % kRing];
  uint32_t depth = 0;
  sample.pc[depth++] = Strip(mc.pc);
  sample.pc[depth++] = Strip(mc.regs[30]);  // lr: the caller of a leaf without a frame
  uintptr_t fp = mc.regs[29];
  while (depth < kDepth && fp >= g_stack_low && fp + 16 <= g_stack_high && (fp & 7) == 0) {
    const uintptr_t* frame = reinterpret_cast<const uintptr_t*>(fp);
    const uintptr_t ret = Strip(frame[1]);
    if (ret != sample.pc[depth - 1]) sample.pc[depth++] = ret;
    if (frame[0] <= fp) break;
    fp = frame[0];
  }
  sample.depth = depth;
  g_head.store(at + 1, std::memory_order_release);
}

void Start() {
  pthread_attr_t attr;
  if (pthread_getattr_np(pthread_self(), &attr) == 0) {
    void* low = nullptr;
    size_t size = 0;
    pthread_attr_getstack(&attr, &low, &size);
    pthread_attr_destroy(&attr);
    g_stack_low = reinterpret_cast<uintptr_t>(low);
    g_stack_high = g_stack_low + size;
  }
  struct sigaction action {};
  action.sa_sigaction = OnSample;
  action.sa_flags = SA_SIGINFO | SA_RESTART;
  sigemptyset(&action.sa_mask);
  sigaction(SIGPROF, &action, nullptr);
  sigevent event{};
  event.sigev_notify = SIGEV_THREAD_ID;
  event.sigev_signo = SIGPROF;
  event.sigev_notify_thread_id = gettid();
  timer_t timer;
  if (timer_create(CLOCK_MONOTONIC, &event, &timer) != 0) {
    REXLOG_WARN("native: profiler timer failed");
    return;
  }
  itimerspec every{};
  every.it_interval.tv_nsec = 1'000'000;
  every.it_value.tv_nsec = 1'000'000;
  timer_settime(timer, 0, &every, nullptr);
  REXLOG_INFO("native: profiling the game thread at 1 kHz");
}

// Name of the function holding `pc` (exported symbols; the game's and the
// SDK's are), or module+offset.
const std::string& NameOf(uintptr_t pc) {
  static std::unordered_map<uintptr_t, std::string> names;
  static std::unordered_map<uintptr_t, uintptr_t> function_of;
  auto known = function_of.find(pc);
  if (known == function_of.end()) {
    Dl_info info{};
    uintptr_t function = pc;
    std::string name;
    if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
      const char* module = std::strrchr(info.dli_fname, '/');
      module = module ? module + 1 : info.dli_fname;
      if (info.dli_sname && info.dli_saddr) {
        function = reinterpret_cast<uintptr_t>(info.dli_saddr);
        int status = 0;
        char* demangled = abi::__cxa_demangle(info.dli_sname, nullptr, nullptr, &status);
        name = demangled && status == 0 ? demangled : info.dli_sname;
        std::free(demangled);
        if (name.size() > 90) name = name.substr(0, 90) + "...";
        name += fmt::format(" [{}]", module);
      } else {
        name = fmt::format("{}+{:x}", module, pc - reinterpret_cast<uintptr_t>(info.dli_fbase));
      }
    } else {
      name = fmt::format("?{:x}", pc);
    }
    known = function_of.emplace(pc, function).first;
    names.emplace(function, std::move(name));
  }
  return names[known->second];
}

// Logs the functions the game thread was in (self) and under (total) since
// the last report.
void Report() {
  const uint32_t head = g_head.load(std::memory_order_acquire);
  const uint32_t count = std::min(head - g_tail, kRing);
  if (count == 0) return;
  std::unordered_map<const std::string*, uint32_t> self, total;
  std::vector<const std::string*> seen;
  for (uint32_t i = head - count; i != head; ++i) {
    const Sample& sample = g_ring[i % kRing];
    seen.clear();
    for (uint32_t d = 0; d < sample.depth; ++d) {
      const std::string* name = &NameOf(sample.pc[d]);
      if (d == 0) ++self[name];
      if (std::find(seen.begin(), seen.end(), name) == seen.end()) {
        seen.push_back(name);
        ++total[name];
      }
    }
  }
  g_tail = head;
  auto top = [&](std::unordered_map<const std::string*, uint32_t>& counts, const char* what) {
    std::vector<std::pair<uint32_t, const std::string*>> sorted;
    for (const auto& [name, n] : counts) sorted.emplace_back(n, name);
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (size_t i = 0; i < std::min<size_t>(sorted.size(), 25); ++i) {
      REXLOG_INFO("profile {} {:5.1f}% {}", what, 100.0 * sorted[i].first / count, *sorted[i].second);
    }
  };
  REXLOG_INFO("profile: {} samples of the game thread", count);
  top(self, "self ");
  top(total, "total");
}
}  // namespace profiler
#endif

bool WatchedPagesClean(uint32_t physical, uint32_t bytes, uint32_t clock) {
  const uint32_t last = (physical + bytes - 1) >> kWatchPageShift;
  for (uint32_t page = physical >> kWatchPageShift; page <= last; ++page) {
    if (g_watch_page_clock[page].load(std::memory_order_acquire) > clock) return false;
  }
  return true;
}

constexpr uint32_t kDeviceFetch = 1152;
constexpr uint32_t kDeviceVsConstants = 1920;
constexpr uint32_t kDevicePsConstants = 6016;
constexpr uint32_t kDeviceBools = 10112;
constexpr uint32_t kDeviceDecl = 11820;
constexpr uint32_t kDeviceIndices = 12436;
constexpr uint32_t kDeviceStrides = 12528;
constexpr uint32_t kDevicePixelShader = 12692;
constexpr uint32_t kDeviceVertexShader = 12696;

struct RegisterBlock {
  uint32_t first;
  uint32_t offset;
  uint32_t count;
};
constexpr RegisterBlock kRegisterBlocks[] = {
    {0x2000, 10368, 16}, {0x2100, 10444, 21}, {0x2180, 10528, 5},  {0x2200, 10548, 12},
    {0x2280, 10596, 21}, {0x2300, 10680, 38}, {0x2380, 10832, 8},
};

bool RegisterOffset(uint32_t reg, uint32_t& offset) {
  for (const RegisterBlock& block : kRegisterBlocks) {
    if (reg >= block.first && reg < block.first + block.count) {
      offset = block.offset + 4 * (reg - block.first);
      return true;
    }
  }
  return false;
}

uint32_t Reg(const uint8_t* base, uint32_t device, uint32_t reg) {
  uint32_t offset = 0;
  return RegisterOffset(reg, offset) ? Be32(base, device + offset) : 0;
}

float RegFloat(const uint8_t* base, uint32_t device, uint32_t reg) {
  const uint32_t bits = Reg(base, device, reg);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

constexpr uint32_t kRbDepthControl = 0x2200;
constexpr uint32_t kRbStencilRefMaskBf = 0x210C;
constexpr uint32_t kRbStencilRefMask = 0x210D;
constexpr uint32_t kRbBlendControl0 = 0x2201;
constexpr uint32_t kRbColorControl = 0x2202;
constexpr uint32_t kPaSuScModeCntl = 0x2205;
constexpr uint32_t kRbColorMask = 0x2104;
constexpr uint32_t kRbAlphaRef = 0x210E;
constexpr uint32_t kPaClVportXScale = 0x210F;
constexpr uint32_t kPaClVportXOffset = 0x2110;
constexpr uint32_t kPaClVportYScale = 0x2111;
constexpr uint32_t kPaClVportYOffset = 0x2112;
constexpr uint32_t kPaClVportZScale = 0x2113;
constexpr uint32_t kPaClVportZOffset = 0x2114;

void SwapDwords(uint8_t* data, size_t bytes, uint32_t endian) {
  // xenos::Endian: 0 none, 1 8in16, 2 8in32, 3 16in32.
  if (endian == 1) {
    for (size_t i = 0; i + 1 < bytes; i += 2) std::swap(data[i], data[i + 1]);
  } else if (endian == 2) {
    for (size_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (endian == 3) {
    for (size_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }
}

// Copies big-endian dwords to `dst` as little-endian ones. The upload ring is
// write-combined GPU memory, so it is only ever written: swapping in place
// there reads it back, which is many times slower than the copy.
void CopySwappedDwords(uint8_t* dst, const uint8_t* src, size_t bytes) {
  for (size_t i = 0; i + 3 < bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, src + i, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    std::memcpy(dst + i, &v, 4);
  }
}

// Float registers [first, end) a guest shader's constant table (D3DX CTAB in
// the virtual half of its container) declares. The recompiled shader would
// read an array indexed past its declared size up to the end of the file;
// D3DX declares a relatively indexed array whole, so the declared registers
// are taken as all it reads. All registers when the table cannot be read.
struct ConstantRange {
  uint16_t first = 0, end = 0;
};

ConstantRange ShaderConstantRange(const uint8_t* base, uint32_t object, bool vertex, uint32_t registers) {
  const ConstantRange all{0, uint16_t(registers)};
  if (object == 0) return all;
  // Offsets of the container in the shader object, as in host/shadow_draw.
  const uint32_t container = object + (vertex ? 872 : 40);
  if ((Be32(base, container) & 0xFFFFFF00u) != 0x102A1100u) return all;
  const uint32_t virtual_size = Be32(base, container + 4);
  const uint32_t table_offset = Be32(base, container + 16);
  if (virtual_size > (1u << 20) || table_offset == 0 || uint64_t(table_offset) + 32 > virtual_size) return all;
  // A size word, then D3DXSHADER_CONSTANTTABLE; its offsets count from itself.
  const uint32_t table = container + table_offset + 4;
  const uint32_t count = Be32(base, table + 12);
  const uint32_t info = Be32(base, table + 16);
  if (count > 4096 || uint64_t(table_offset) + 4 + info + uint64_t(count) * 20 > virtual_size) return all;
  uint32_t first = registers, end = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t entry = table + info + i * 20;  // D3DXSHADER_CONSTANTINFO
    if (Be16(base, entry + 4) != 2) continue;      // RegisterSet::Float4
    const uint32_t index = Be16(base, entry + 6);
    const uint32_t size = Be16(base, entry + 8);
    if (size == 0) continue;
    first = std::min(first, index);
    end = std::max(end, index + size);
  }
  if (end == 0) return {};
  return {uint16_t(std::min(first, registers)), uint16_t(std::min(end, registers))};
}

// A map from well-mixed 64-bit keys (hashes) to values in one array, linear
// probing: a lookup reads a slot or two where std::unordered_map follows a
// bucket to a node. Any insert or erase moves values, so references to them
// last until the next one.
template <typename V>
class FlatMap {
 public:
  V& operator[](uint64_t key) {
    if ((count_ + 1) * 2 > slots_.size()) Grow();
    size_t i = Home(key);
    for (; slots_[i].used; i = (i + 1) & mask_) {
      if (slots_[i].key == key) return slots_[i].value;
    }
    slots_[i].used = true;
    slots_[i].key = key;
    ++count_;
    return slots_[i].value;
  }
  size_t size() const { return count_; }

  template <typename Pred>
  void EraseIf(Pred pred) {
    std::vector<uint64_t> doomed;
    for (const Slot& slot : slots_) {
      if (slot.used && pred(slot.value)) doomed.push_back(slot.key);
    }
    for (uint64_t key : doomed) Erase(key);
  }

 private:
  struct Slot {
    uint64_t key = 0;
    bool used = false;
    V value{};
  };

  size_t Home(uint64_t key) const { return size_t((key * 0x9E3779B97F4A7C15ull) >> shift_); }

  // Backward-shift deletion: later slots of the probe run move up so no
  // run is cut by the hole.
  void Erase(uint64_t key) {
    size_t hole = Home(key);
    while (!(slots_[hole].used && slots_[hole].key == key)) hole = (hole + 1) & mask_;
    for (size_t j = (hole + 1) & mask_; slots_[j].used; j = (j + 1) & mask_) {
      // j may fill the hole when its home is not between the hole and j.
      if (((j - Home(slots_[j].key)) & mask_) >= ((j - hole) & mask_)) {
        slots_[hole] = std::move(slots_[j]);
        hole = j;
      }
    }
    slots_[hole] = Slot{};
    --count_;
  }

  void Grow() {
    std::vector<Slot> old;
    old.swap(slots_);
    const size_t size = std::max<size_t>(64, old.size() * 2);
    slots_.resize(size);
    mask_ = size - 1;
    shift_ = 64 - uint32_t(std::countr_zero(size));
    count_ = 0;
    for (Slot& slot : old) {
      if (slot.used) (*this)[slot.key] = std::move(slot.value);
    }
  }

  std::vector<Slot> slots_;
  size_t mask_ = 0, count_ = 0;
  uint32_t shift_ = 64;
};

// FlatMap or std::unordered_map, as native_flat_maps chose at start.
template <typename V>
class HotMap {
 public:
  V& operator[](uint64_t key) { return Flat() ? flat_[key] : std_[key]; }
  size_t size() const { return Flat() ? flat_.size() : std_.size(); }
  template <typename Pred>
  void EraseIf(Pred pred) {
    if (Flat()) {
      flat_.EraseIf(pred);
    } else {
      std::erase_if(std_, [&](const auto& entry) { return pred(entry.second); });
    }
  }

 private:
  static bool Flat() {
    static const bool flat = REXCVAR_GET(native_flat_maps);
    return flat;
  }
  FlatMap<V> flat_;
  std::unordered_map<uint64_t, V> std_;
};

// ---------------------------------------------------------------------------
// Translation tables.

plume::RenderComparisonFunction CompareFunction(uint32_t xenos_func) {
  // xenos::CompareFunction 0..7 is never, less, equal, lequal, greater,
  // notequal, gequal, always: Plume's order after UNKNOWN.
  return static_cast<plume::RenderComparisonFunction>(1 + (xenos_func & 7));
}

plume::RenderStencilOp StencilOp(uint32_t xenos_op) {
  // keep, zero, replace, incr_sat, decr_sat, invert, incr_wrap, decr_wrap:
  // Plume's order after UNKNOWN.
  return static_cast<plume::RenderStencilOp>(1 + (xenos_op & 7));
}

// One face of RB_DEPTHCONTROL's stencil state: function, then the fail,
// depth-pass and depth-fail operations, three bits each from `shift`.
plume::RenderStencilFaceDesc StencilFace(uint32_t depth_control, uint32_t shift) {
  plume::RenderStencilFaceDesc face;
  face.compareFunction = CompareFunction(depth_control >> shift);
  face.failOp = StencilOp(depth_control >> (shift + 3));
  face.passOp = StencilOp(depth_control >> (shift + 6));
  face.depthFailOp = StencilOp(depth_control >> (shift + 9));
  return face;
}

plume::RenderBlend BlendFactor(uint32_t factor) {
  using B = plume::RenderBlend;
  switch (factor) {
    case 0: return B::ZERO;
    case 1: return B::ONE;
    case 4: return B::SRC_COLOR;
    case 5: return B::INV_SRC_COLOR;
    case 6: return B::SRC_ALPHA;
    case 7: return B::INV_SRC_ALPHA;
    case 8: return B::DEST_COLOR;
    case 9: return B::INV_DEST_COLOR;
    case 10: return B::DEST_ALPHA;
    case 11: return B::INV_DEST_ALPHA;
    case 12: case 14: return B::BLEND_FACTOR;
    case 13: case 15: return B::INV_BLEND_FACTOR;
    case 16: return B::SRC_ALPHA_SAT;
    default: return B::ONE;
  }
}

plume::RenderBlendOperation BlendOp(uint32_t op) {
  using O = plume::RenderBlendOperation;
  switch (op) {
    case 0: return O::ADD;
    case 1: return O::SUBTRACT;
    case 2: return O::MIN;
    case 3: return O::MAX;
    case 4: return O::REV_SUBTRACT;
    default: return O::ADD;
  }
}

// How a guest element becomes host vertex data: copied as is, or decoded to
// four 32-bit floats (written as floats, or as their bits for the uint4
// inputs XenosRecomp reads through asfloat).
enum class ElementConvert : uint8_t { kCopy, kFloat4, kFloat4Bits, kPacked101111 };

struct ElementSource {
  xenos::VertexFormat format;
  bool is_signed;
  bool is_integer;
  // Declaration swizzle (format word bits 10-21), three bits per output
  // component: 0-3 pick x/y/z/w, 4 is 0, 5 is 1. D3D patches it into the
  // vfetch instructions when it binds a declaration, after the shader cache
  // was keyed, so the host applies it while gathering the stream.
  uint32_t swizzle;
};

uint32_t ComponentCount(xenos::VertexFormat format) {
  switch (format) {
    case xenos::VertexFormat::k_32_FLOAT:
      return 1;
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
      return 2;
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      return 3;
    default:
      return 4;
  }
}

// True when the swizzle does more than the host fetch already does (absent
// components read as 0, absent w as 1).
bool SwizzleMatters(const ElementSource& source) {
  const uint32_t n = ComponentCount(source.format);
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t pick = (source.swizzle >> (3 * c)) & 7;
    const uint32_t natural = c < n ? c : (c == 3 ? 5 : 4);
    if (pick < 6 && pick != natural) return true;
  }
  return false;
}

void ApplySwizzle(uint32_t swizzle, float value[4]) {
  float in[4];
  std::memcpy(in, value, sizeof(in));
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t pick = (swizzle >> (3 * c)) & 7;
    if (pick < 4) {
      value[c] = in[pick];
    } else if (pick == 4) {
      value[c] = 0.0f;
    } else if (pick == 5) {
      value[c] = 1.0f;
    }
  }
}

uint32_t GuestElementSize(xenos::VertexFormat format) {
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_32_FLOAT:
      return 4;
    case xenos::VertexFormat::k_16_16_16_16:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
      return 8;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      return 12;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      return 16;
    default:
      return 0;
  }
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 1;
      while ((mantissa & 0x400) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FF;
      bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// One component `bits` wide, as Xenos fetch would return it.
float FixedComponent(uint32_t value, uint32_t bits, bool is_signed, bool is_integer) {
  if (is_signed) {
    const int32_t shift = 32 - int32_t(bits);
    const int32_t v = int32_t(value << shift) >> shift;
    if (is_integer) return float(v);
    return std::max(float(v) / float((1u << (bits - 1)) - 1), -1.0f);
  }
  if (is_integer) return float(value);
  return float(value) / float((1u << bits) - 1);
}

// Decodes one element (already in host byte order) to xyzw; absent
// components are 0, absent w is 1.
void DecodeElement(const ElementSource& source, const uint8_t* src, float out[4]) {
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;
  uint32_t word0;
  std::memcpy(&word0, src, 4);
  const bool sg = source.is_signed;
  const bool in = source.is_integer;
  switch (source.format) {
    case xenos::VertexFormat::k_8_8_8_8:
      for (uint32_t c = 0; c < 4; ++c) out[c] = FixedComponent((word0 >> (8 * c)) & 0xFF, 8, sg, in);
      return;
    case xenos::VertexFormat::k_2_10_10_10:
      out[0] = FixedComponent(word0 & 0x3FF, 10, sg, in);
      out[1] = FixedComponent((word0 >> 10) & 0x3FF, 10, sg, in);
      out[2] = FixedComponent((word0 >> 20) & 0x3FF, 10, sg, in);
      out[3] = FixedComponent(word0 >> 30, 2, sg, in);
      return;
    case xenos::VertexFormat::k_10_11_11:
      out[0] = FixedComponent(word0 & 0x7FF, 11, sg, in);
      out[1] = FixedComponent((word0 >> 11) & 0x7FF, 11, sg, in);
      out[2] = FixedComponent(word0 >> 22, 10, sg, in);
      return;
    case xenos::VertexFormat::k_11_11_10:
      out[0] = FixedComponent(word0 & 0x3FF, 10, sg, in);
      out[1] = FixedComponent((word0 >> 10) & 0x7FF, 11, sg, in);
      out[2] = FixedComponent(word0 >> 21, 11, sg, in);
      return;
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_16_16: {
      const uint32_t n = source.format == xenos::VertexFormat::k_16_16 ? 2 : 4;
      for (uint32_t c = 0; c < n; ++c) {
        uint16_t v;
        std::memcpy(&v, src + 2 * c, 2);
        out[c] = FixedComponent(v, 16, sg, in);
      }
      return;
    }
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT: {
      const uint32_t n = source.format == xenos::VertexFormat::k_16_16_FLOAT ? 2 : 4;
      for (uint32_t c = 0; c < n; ++c) {
        uint16_t v;
        std::memcpy(&v, src + 2 * c, 2);
        out[c] = HalfToFloat(v);
      }
      return;
    }
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      std::memcpy(out, src, GuestElementSize(source.format));
      return;
    default:
      return;
  }
}

// Host format of one declaration element. XenosRecomp reads NORMAL, TANGENT,
// BINORMAL and BLENDINDICES as uint4 (the first three as raw float bits, or
// as packed 11:11:10 when the vertex shader's spec constant says so),
// everything else as float4. Formats Vulkan cannot fetch the way Xenos does
// are decoded on the CPU while the stream is gathered.
bool ElementFormat(uint32_t usage, uint32_t format_word, plume::RenderFormat& out, uint32_t& in_size,
                   uint32_t& out_size, ElementConvert& convert, ElementSource& source,
                   bool& packed_normal) {
  using F = plume::RenderFormat;
  source.format = static_cast<xenos::VertexFormat>(format_word & 0x3F);
  source.is_signed = (format_word & 0x100) != 0;
  source.is_integer = (format_word & 0x200) != 0;
  source.swizzle = (format_word >> 10) & 0xFFF;
  if (!REXCVAR_GET(native_decl_swizzle)) {
    // The fetch as is: present components in order, absent ones 0, w 1.
    const uint32_t n = ComponentCount(source.format);
    source.swizzle = 0;
    for (uint32_t c = 0; c < 4; ++c) source.swizzle |= (c < n ? c : (c == 3 ? 5u : 4u)) << (3 * c);
  }
  in_size = GuestElementSize(source.format);
  if (in_size == 0) return false;
  const bool raw = usage == uint32_t(DeclUsage::kNormal) || usage == uint32_t(DeclUsage::kTangent) ||
                   usage == uint32_t(DeclUsage::kBinormal);
  packed_normal = false;
  convert = ElementConvert::kCopy;
  out_size = in_size;
  if (usage == uint32_t(DeclUsage::kBlendIndices)) {
    if (source.format != xenos::VertexFormat::k_8_8_8_8) return false;
    out = F::R8G8B8A8_UINT;
    return true;
  }
  const bool swizzled = SwizzleMatters(source);
  if (raw) {
    if (!swizzled && (source.format == xenos::VertexFormat::k_11_11_10 ||
                      source.format == xenos::VertexFormat::k_10_11_11)) {
      out = F::R32_UINT;
      packed_normal = true;
      return true;
    }
    convert = ElementConvert::kFloat4Bits;
    out = F::R32G32B32A32_UINT;
    out_size = 16;
    return true;
  }
  if (swizzled) {
    convert = ElementConvert::kFloat4;
    out = F::R32G32B32A32_FLOAT;
    out_size = 16;
    return true;
  }
  switch (source.format) {
    case xenos::VertexFormat::k_8_8_8_8:
      if (!source.is_integer) {
        out = source.is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
        return true;
      }
      break;
    case xenos::VertexFormat::k_16_16:
      if (!source.is_integer) {
        out = source.is_signed ? F::R16G16_SNORM : F::R16G16_UNORM;
        return true;
      }
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      if (!source.is_integer) {
        out = source.is_signed ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM;
        return true;
      }
      break;
    case xenos::VertexFormat::k_16_16_FLOAT: out = F::R16G16_FLOAT; return true;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT: out = F::R16G16B16A16_FLOAT; return true;
    case xenos::VertexFormat::k_32_FLOAT: out = F::R32_FLOAT; return true;
    case xenos::VertexFormat::k_32_32_FLOAT: out = F::R32G32_FLOAT; return true;
    case xenos::VertexFormat::k_32_32_32_FLOAT: out = F::R32G32B32_FLOAT; return true;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT: out = F::R32G32B32A32_FLOAT; return true;
    default:
      break;
  }
  convert = ElementConvert::kFloat4;
  out = F::R32G32B32A32_FLOAT;
  out_size = 16;
  return true;
}

bool SixteenBitComponents(xenos::VertexFormat format) {
  return format == xenos::VertexFormat::k_16_16 || format == xenos::VertexFormat::k_16_16_16_16 ||
         format == xenos::VertexFormat::k_16_16_FLOAT || format == xenos::VertexFormat::k_16_16_16_16_FLOAT;
}

// src is the element after the fetch endian swap. With an 8in32 swap the two
// 16-bit components of each dword come out in reverse order, but Xenos
// fetches the one at the lower guest address as x (seen on the boot screens:
// SHORT2 positions only fill the screen with x and y in guest order).
void ConvertElement(const uint8_t* src, uint8_t* dst, uint32_t in_size, ElementConvert convert,
                    const ElementSource& source, uint32_t endian) {
  uint8_t fixed[16];
  if (endian == 2 && SixteenBitComponents(source.format)) {
    for (uint32_t i = 0; i + 3 < in_size; i += 4) {
      fixed[i] = src[i + 2];
      fixed[i + 1] = src[i + 3];
      fixed[i + 2] = src[i];
      fixed[i + 3] = src[i + 1];
    }
    src = fixed;
  }
  if (convert == ElementConvert::kCopy) {
    std::memcpy(dst, src, in_size);
    return;
  }
  float value[4];
  DecodeElement(source, src, value);
  ApplySwizzle(source.swizzle, value);
  if (convert == ElementConvert::kPacked101111) {
    // XenosRecomp's tfetchR11G11B10: x and y 11-bit, z 10-bit, two's
    // complement over 1024, 1024 and 512.
    auto field = [](float v, float scale, int32_t lo, int32_t hi, uint32_t mask) {
      return uint32_t(std::clamp<int32_t>(int32_t(std::lround(v * scale)), lo, hi)) & mask;
    };
    const uint32_t packed = field(value[0], 1024.0f, -1024, 1023, 0x7FF) |
                            (field(value[1], 1024.0f, -1024, 1023, 0x7FF) << 11) |
                            (field(value[2], 512.0f, -512, 511, 0x3FF) << 22);
    std::memcpy(dst, &packed, 4);
    return;
  }
  std::memcpy(dst, value, 16);
}

uint32_t UsageLocation(uint32_t usage, uint32_t index) {
  for (const auto& location : broken_bond::gfx::kVertexLocations) {
    if (uint32_t(location.usage) == usage && location.usage_index == index) return location.location;
  }
  return UINT32_MAX;
}

const char* UsageSemantic(uint32_t usage) {
  switch (static_cast<DeclUsage>(usage)) {
    case DeclUsage::kPosition: return "POSITION";
    case DeclUsage::kBlendWeight: return "BLENDWEIGHT";
    case DeclUsage::kBlendIndices: return "BLENDINDICES";
    case DeclUsage::kNormal: return "NORMAL";
    case DeclUsage::kTexCoord: return "TEXCOORD";
    case DeclUsage::kTangent: return "TANGENT";
    case DeclUsage::kBinormal: return "BINORMAL";
    case DeclUsage::kColor: return "COLOR";
  }
  return "TEXCOORD";
}

// Host format holding a Xenos texture format's texels unchanged.
plume::RenderFormat HostTextureFormat(uint32_t format) {
  using F = plume::RenderFormat;
  switch (static_cast<xenos::TextureFormat>(format)) {
    case xenos::TextureFormat::k_8: return F::R8_UNORM;
    case xenos::TextureFormat::k_8_8: return F::R8G8_UNORM;
    case xenos::TextureFormat::k_8_8_8_8: return F::R8G8B8A8_UNORM;
    case xenos::TextureFormat::k_DXT1: return F::BC1_UNORM;
    case xenos::TextureFormat::k_DXT2_3: return F::BC2_UNORM;
    case xenos::TextureFormat::k_DXT4_5: return F::BC3_UNORM;
    case xenos::TextureFormat::k_DXT5A: return F::BC4_UNORM;
    case xenos::TextureFormat::k_DXN: return F::BC5_UNORM;
    case xenos::TextureFormat::k_16: return F::R16_UNORM;
    case xenos::TextureFormat::k_16_16: return F::R16G16_UNORM;
    case xenos::TextureFormat::k_16_16_16_16: return F::R16G16B16A16_UNORM;
    case xenos::TextureFormat::k_16_FLOAT: return F::R16_FLOAT;
    case xenos::TextureFormat::k_16_16_FLOAT: return F::R16G16_FLOAT;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT: return F::R16G16B16A16_FLOAT;
    case xenos::TextureFormat::k_32_FLOAT: return F::R32_FLOAT;
    case xenos::TextureFormat::k_32_32_FLOAT: return F::R32G32_FLOAT;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT: return F::R32G32B32A32_FLOAT;
    default: return F::UNKNOWN;
  }
}

// Fetch swizzle: three bits per output component, 0-3 pick x/y/z/w of the
// fetched texel, 4 is 0 and 5 is 1.
plume::RenderComponentMapping ComponentMapping(uint32_t swizzle) {
  auto one = [](uint32_t s) {
    switch (s & 7) {
      case 0: return plume::RenderSwizzle::R;
      case 1: return plume::RenderSwizzle::G;
      case 2: return plume::RenderSwizzle::B;
      case 3: return plume::RenderSwizzle::A;
      case 4: return plume::RenderSwizzle::ZERO;
      default: return plume::RenderSwizzle::ONE;
    }
  };
  return plume::RenderComponentMapping(one(swizzle), one(swizzle >> 3), one(swizzle >> 6), one(swizzle >> 9));
}

// Xenos fetches of one-component formats return that component in all four
// channels, and of two-component formats return the second one in z and w
// too. Host views of such formats read 0 and 1 there instead, so the fetch
// swizzle's picks are redirected to the components that exist.
uint32_t HostSwizzle(uint32_t swizzle, xenos::TextureFormat format) {
  uint32_t components = 4;
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT5A:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
      components = 1;
      break;
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
      components = 2;
      break;
    default:
      return swizzle;
  }
  uint32_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t pick = (swizzle >> (3 * c)) & 7;
    if (pick < 4) pick = std::min(pick, components - 1);
    out |= pick << (3 * c);
  }
  return out;
}

// A resolve leaves 8:8:8:8 and 2:10:10:10 pixels in guest memory in D3D's
// ARGB order, which a fetch reads as BGRA, and the game's fetch swizzle
// (zyxw) turns back into RGBA. The host copy of a resolve keeps the target's
// RGBA order, so for it the swizzle's picks of x and z trade places.
uint32_t ResolvedSwizzle(uint32_t swizzle, xenos::TextureFormat format) {
  if (format != xenos::TextureFormat::k_8_8_8_8 && format != xenos::TextureFormat::k_2_10_10_10) return swizzle;
  uint32_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t pick = (swizzle >> (3 * c)) & 7;
    if (pick == 0) {
      pick = 2;
    } else if (pick == 2) {
      pick = 0;
    }
    out |= pick << (3 * c);
  }
  return out;
}

// A depth resolve's host copy holds depth in its only component, which is
// what the guest's k_24_8 fetch returns first; every pick of a texel
// component reads it.
uint32_t DepthSwizzle(uint32_t swizzle) {
  uint32_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t pick = (swizzle >> (3 * c)) & 7;
    if (pick < 4) pick = 0;
    out |= pick << (3 * c);
  }
  return out;
}

// Names one guest image by its texture fetch constant. Clamp and filter
// bits are sampler state and are left out.
// Physical address of a texture base. Objects the game builds keep virtual
// 0xE0000000-range addresses; SetTexture writes the physical one, which is
// 0x1000 further on (the 4 KB physical heap's offset).
uint32_t TexturePhysical(uint32_t base_address_field) {
  const uint32_t address = base_address_field << 12;
  return address >= 0xE0000000u ? (address & 0x1FFFFFFFu) + 0x1000u : address & 0x1FFFFFFFu;
}

uint64_t ImageKey(const uint32_t words[6]) {
  uint32_t image_words[6];
  std::memcpy(image_words, words, sizeof(image_words));
  image_words[0] &= ~0x3FFC00u;    // clamp_x/y/z
  image_words[3] &= ~0x0FF80000u;  // mag/min/mip/aniso filter
  return XXH3_64bits(image_words, sizeof(image_words));
}

// xenos::ClampMode: repeat, mirrored repeat, clamp to edge, mirror clamp to
// edge, clamp halfway, mirror clamp halfway, clamp to border, mirror clamp to
// border. Halfway clamps sit between edge and border; edge is the closer.
plume::RenderTextureAddressMode AddressMode(uint32_t clamp) {
  using A = plume::RenderTextureAddressMode;
  switch (clamp) {
    case 0: return A::WRAP;
    case 1: return A::MIRROR;
    case 2: case 4: return A::CLAMP;
    case 3: case 5: return A::MIRROR_ONCE;
    default: return A::BORDER;
  }
}

}  // namespace

// ---------------------------------------------------------------------------

struct Renderer::State {
  std::unique_ptr<plume::RenderInterface> render_interface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandList> commands;
  std::unique_ptr<plume::RenderCommandFence> fence;
  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release;
  std::string device_name;

  // Swapchain size; the guest's 1280x720 frame is scaled to it, and every
  // render target uses the same scale.
  uint32_t width = 0;
  uint32_t height = 0;
  float scale_x = 1.0f;
  float scale_y = 1.0f;

  // Host render targets. Xenos keeps colour and depth in EDRAM, and surfaces
  // that share an EDRAM base share their pixels whatever their MSAA or the
  // other attachment (the title draws its scene at 4x and resolves the same
  // EDRAM through a 1x surface). So colour images are keyed by colour base,
  // format and pitch, depth images by depth base and pitch, and a framebuffer
  // pairs them. MSAA is not emulated: everything is one sample.
  // Surfaces the guest multisamples are drawn into a multisampled twin of
  // the image; `msaa_newer` says the twin holds pixels the single-sample
  // texture does not have yet, so a single-sample surface on the same EDRAM
  // (the title composites through one) resolves it first.
  struct Image {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTexture> msaa;
    uint32_t msaa_samples = 0;
    bool msaa_newer = false;
    uint32_t guest_width = 0;
    uint32_t guest_height = 0;
    uint32_t width = 0;  // host pixels
    uint32_t height = 0;
    // Depth images only: a view of the texture or its multisampled twin in
    // the 2D heap, for the depth resolve pass to read.
    std::unique_ptr<plume::RenderTextureView> depth_view;
    const plume::RenderTexture* depth_view_of = nullptr;
    uint32_t depth_index = 0;
  };
  struct Target {
    Image* color = nullptr;
    Image* depth = nullptr;
    uint32_t samples = 1;
    uint64_t id = 0;  // creation number; a new target may reuse a freed one's address
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  };
  std::unordered_map<uint64_t, Image> color_images;
  std::unordered_map<uint64_t, Image> depth_images;
  std::unordered_map<uint64_t, Target> targets;
  Target* current = nullptr;
  plume::RenderTexture* color = nullptr;  // current target's
  uint32_t current_samples = 1;
  plume::RenderTexture* depth = nullptr;
  plume::RenderFramebuffer* framebuffer = nullptr;
  uint32_t target_width = 0;  // current target, host pixels
  uint32_t target_height = 0;
  uint64_t target_switches = 0;

  // Per-frame upload ring: constants, vertices, indices, texels.
  std::unique_ptr<plume::RenderBuffer> upload;
  uint8_t* upload_cpu = nullptr;
  uint64_t upload_gpu = 0;
  uint64_t upload_used = 0;

  std::unique_ptr<plume::RenderPipelineLayout> layout;
  // Set 0 holds the texture heaps as bindings (2D, 3D, cube, 1D), set 1 the
  // samplers. HeapBase gives a heap's first flat index in set 0.
  static constexpr uint32_t kSetCount = 2;
  std::unique_ptr<plume::RenderDescriptorSet> sets[kSetCount];

  // Shader cache from XenosRecomp, decompressed once.
  std::vector<uint8_t> spirv;
  std::unordered_map<uint64_t, const ShaderCacheEntry*> entries;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderShader>> shaders;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines;

  struct Texture {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t index = 0;
    bool resolved = false;  // a host copy of the target, at host scale
    uint64_t content = 0;   // XXH3 of the guest texels last uploaded
    uint64_t sample = 0;    // SampleHash of the same texels
    uint64_t checked = UINT64_MAX;  // present the content was last compared
    // native_watch_memory: watch clock before the texels were last hashed,
    // or UINT32_MAX when they are not watched.
    uint32_t watch_clock = UINT32_MAX;
    uint32_t width = 0;
    uint32_t height = 0;
    plume::RenderFormat format = kColorFormat;
    uint32_t view_swizzle = 0x688;  // fetch swizzle of the view; identity xyzw
    // Depth resolves draw into the texture.
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  };
  std::unordered_map<uint64_t, Texture> textures;
  uint32_t next_texture2d = 1;  // 0 is white in every heap
  // 1D textures hold per-draw data the CPU rewrites every frame (skinned
  // meshes fetch their bone matrices from one), so they live in a pool that
  // is refilled each frame: slot i is heap index i + 1, and the same texels
  // used again in the frame (shadow, body and outline passes) share a slot.
  struct Texture1DSlot {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t width = 0;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t swizzle = 0;
    // This frame's texels in the upload ring.
    uint64_t ring = 0;
    uint32_t ring_bytes = 0;
    uint32_t row_texels = 0;
    std::vector<uint8_t> cpu;  // the same texels, with native_interpolate
  };
  std::vector<Texture1DSlot> texture1d_slots;
  // Present pass: the frontbuffer through the game's gamma ramp.
  std::unique_ptr<plume::RenderShader> present_vs, present_ps;
  std::unique_ptr<plume::RenderPipeline> present_pipeline;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> swap_framebuffers;
  std::array<uint32_t, 256> gamma_ramp{};
  bool has_gamma_ramp = false;

  // Depth resolve pass: a depth buffer into an R32_FLOAT texture.
  std::unique_ptr<plume::RenderShader> depth_resolve_vs, depth_resolve_ps;
  std::unique_ptr<plume::RenderPipeline> depth_resolve_pipeline;

  void CreateDepthResolvePipeline() {
    depth_resolve_vs = device->createShader(g_depth_resolve_vs, sizeof(g_depth_resolve_vs), "VSMain",
                                            plume::RenderShaderFormat::SPIRV);
    depth_resolve_ps = device->createShader(g_depth_resolve_ps, sizeof(g_depth_resolve_ps), "PSMain",
                                            plume::RenderShaderFormat::SPIRV);
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout.get();
    desc.vertexShader = depth_resolve_vs.get();
    desc.pixelShader = depth_resolve_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = kDepthCopyFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    depth_resolve_pipeline = device->createGraphicsPipeline(desc);
  }

  void CreatePresentPipeline() {
    present_vs = device->createShader(g_present_vs, sizeof(g_present_vs), "VSMain", plume::RenderShaderFormat::SPIRV);
    present_ps = device->createShader(g_present_ps, sizeof(g_present_ps), "PSMain", plume::RenderShaderFormat::SPIRV);
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout.get();
    desc.vertexShader = present_vs.get();
    desc.pixelShader = present_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = kSwapChainFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    present_pipeline = device->createGraphicsPipeline(desc);
  }

  // Bilinear, clamped sampler for scaling the frontbuffer to the window.
  uint32_t PresentSampler() {
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.clamp_x = fetch.clamp_y = fetch.clamp_z = xenos::ClampMode::kClampToEdge;
    fetch.mag_filter = fetch.min_filter = xenos::TextureFilter::kLinear;
    return SamplerFor(fetch);
  }

  plume::RenderFramebuffer* SwapFramebuffer(uint32_t image) {
    if (swap_framebuffers.size() != swap_chain->getTextureCount()) {
      swap_framebuffers.clear();
      swap_framebuffers.resize(swap_chain->getTextureCount());
    }
    auto& framebuffer = swap_framebuffers[image];
    if (!framebuffer) {
      const plume::RenderTexture* attachment = swap_chain->getTexture(image);
      plume::RenderFramebufferDesc desc;
      desc.colorAttachments = &attachment;
      desc.colorAttachmentsCount = 1;
      framebuffer = device->createFramebuffer(desc);
    }
    return framebuffer.get();
  }
  // Cube textures, keyed like 2D ones, in the cube heap.
  std::unordered_map<uint64_t, Texture> cube_textures;
  uint32_t next_texture_cube = 1;
  uint32_t next_texture1d = 0;
  std::unordered_map<uint64_t, uint32_t> texture1d_frame;
  std::vector<std::unique_ptr<plume::RenderTexture>> defaults;
  std::unordered_map<uint32_t, uint32_t> sampler_index;
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers;

  std::string last_viewport;
  std::vector<uint32_t> index_scratch;  // one draw's guest indices
  // The viewport line of the last draw, kept while its registers repeat.
  std::array<uint32_t, 12> viewport_words{};
  std::string viewport_line;
  std::string last_bounds;
  std::map<std::string, uint64_t> viewports;  // VTE_CNTL and viewport per draw, for the log
  std::string dump;  // lines for native_dump_frame
  uint64_t outline_dump = 0;  // present to dump for native_dump_outline
  std::array<uint64_t, 8> cull_modes{};  // triangle draws per PA_SU_SC_MODE_CNTL & 7
  // Vertex streams per (fetch constant endian, element endian) pair.
  std::array<uint64_t, 16> stream_endians{};
  bool Dumping() const {
    return REXCVAR_GET(native_dump_frame) == int32_t(presents + 1) || outline_dump == presents + 1 ||
           key_dump == presents + 1;
  }
  // F9 asks for a dump of the next frame, with its picture.
  uint64_t key_dump = 0;
  bool key_down = false;
  bool frame_image_pending = false;
  bool recording = false;
  uint64_t clears = 0;
  uint64_t resolves = 0;
  uint64_t reuploads = 0;
  // Guest base address of every resolve destination -> its texture key. The
  // game samples a resolved image through fetch constants that differ from
  // the destination object's (format view, flags), so the address decides.
  // Resolve destinations by physical address. With Xenos off nothing writes
  // the guest memory a resolve targets, so if its first page changes the CPU
  // has put something else there (a texture streamed into freed memory) and
  // the address no longer means the resolved image.
  struct ResolvedAt {
    uint64_t key;
    uint64_t guest;
    uint64_t checked = UINT64_MAX;  // present the guest page was last compared
  };
  std::unordered_map<uint32_t, ResolvedAt> resolved_at;

  // A cheap fingerprint of guest texels: all of them up to 16 KB, else 64
  // spread 64-byte runs. Rewrites the game makes (video frames, redrawn
  // images) change it; a small local change may not, which the periodic
  // full hash in Unchanged catches.
  static uint64_t SampleHash(const uint8_t* data, size_t bytes) {
    if (bytes <= 16384) return XXH3_64bits(data, bytes);
    // The runs are gathered and hashed in one call: 64 streaming updates
    // cost a phone about a microsecond per texture per frame.
    alignas(16) uint8_t runs[64 * 64];
    const size_t step = (bytes - 64) / 63;
    for (size_t i = 0; i < 64; ++i) std::memcpy(runs + i * 64, data + i * step, 64);
    return XXH3_64bits_withSeed(runs, sizeof(runs), bytes);
  }

  // Whether a texture's guest texels still match what was uploaded. Each
  // frame compares the sample; every 64th frame (staggered by heap index)
  // the full hash, which only catches a small local rewrite the sample
  // misses (about a second late at 60 Hz). Every 16th frame cost a phone a
  // millisecond of each fight frame. On a mismatch `content` and `sample`
  // hold the new hashes.
  bool Unchanged(const Texture& texture, const uint8_t* guest, size_t bytes, uint64_t& content, uint64_t& sample) {
    sample = SampleHash(guest, bytes);
    const bool full_due = (presents + texture.index) % 64 == 0;
    if (sample == texture.sample && !full_due) {
      content = texture.content;
      return true;
    }
    content = bytes <= 16384 ? sample : XXH3_64bits(guest, bytes);
    return content == texture.content;
  }

  static uint64_t GuestPageHash(const uint8_t* base, uint32_t physical) {
    return XXH3_64bits(At(base, PhysicalView(physical)), 4096);
  }

  // A guest D3DRECT {x1, y1, x2, y2} scaled to the host target.
  plume::RenderRect HostRect(const uint8_t* base, uint32_t rect) const {
    auto at = [&](uint32_t i) { return int32_t(Be32(base, rect + 4 * i)); };
    plume::RenderRect out(int32_t(std::lround(at(0) * scale_x)), int32_t(std::lround(at(1) * scale_y)),
                          int32_t(std::lround(at(2) * scale_x)), int32_t(std::lround(at(3) * scale_y)));
    out.left = std::clamp<int32_t>(out.left, 0, int32_t(target_width));
    out.right = std::clamp<int32_t>(out.right, out.left, int32_t(target_width));
    out.top = std::clamp<int32_t>(out.top, 0, int32_t(target_height));
    out.bottom = std::clamp<int32_t>(out.bottom, out.top, int32_t(target_height));
    return out;
  }

  // Guest rows a rect reaches (its bottom), for sizing the target.
  static uint32_t RectBottom(const uint8_t* base, uint32_t rect) {
    return rect ? uint32_t(std::max<int32_t>(0, int32_t(Be32(base, rect + 12)))) : 0;
  }

  // Makes sure `image` holds `rows` guest rows of `pitch` pixels. Returns
  // true when it was (re)created, which drops framebuffers built on it.
  bool EnsureImage(Image& image, uint32_t pitch, uint32_t rows, bool is_depth, uint32_t samples = 1) {
    bool fresh = false;
    if (!image.texture || image.guest_width != pitch || image.guest_height < rows) {
      CreateImage(image, pitch, rows, is_depth);
      fresh = true;
    }
    if (samples > 1 && (fresh || !image.msaa || image.msaa_samples != samples)) {
      const plume::RenderMultisampling multisampling(static_cast<plume::RenderSampleCounts>(samples));
      Retire(image.msaa);
      image.msaa = device->createTexture(
          is_depth ? plume::RenderTextureDesc::DepthTarget(image.width, image.height, kDepthFormat, multisampling)
                   : plume::RenderTextureDesc::ColorTarget(image.width, image.height, kColorFormat, multisampling));
      image.msaa_samples = samples;
      image.msaa_newer = false;
      Rec([this, msaa = image.msaa.get(), is_depth] {
        commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(msaa, is_depth ? plume::RenderTextureLayout::DEPTH_WRITE
                                                                      : plume::RenderTextureLayout::COLOR_WRITE));
      });
      fresh = true;
    }
    return fresh;
  }

  // Writes the multisampled twin's pixels into the single-sample texture.
  void ResolveMsaa(Image& image) {
    if (!image.msaa_newer || !image.msaa) return;
    Rec([this, msaa = image.msaa.get(), texture = image.texture.get()] {
      plume::RenderTextureBarrier to_resolve[] = {
          plume::RenderTextureBarrier(msaa, plume::RenderTextureLayout::RESOLVE_SOURCE),
          plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::RESOLVE_DEST)};
      commands->barriers(plume::RenderBarrierStage::COPY, to_resolve, 2);
      commands->resolveTexture(texture, msaa);
      plume::RenderTextureBarrier back[] = {
          plume::RenderTextureBarrier(msaa, plume::RenderTextureLayout::COLOR_WRITE),
          plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COLOR_WRITE)};
      commands->barriers(plume::RenderBarrierStage::GRAPHICS, back, 2);
    });
    image.msaa_newer = false;
    ++msaa_resolves;
  }

  void CreateImage(Image& image, uint32_t pitch, uint32_t rows, bool is_depth) {
    // The multisampled twin must match the new size, so it is made again
    // the next time a multisampled surface uses the image.
    Retire(image.msaa);
    image.msaa_samples = 0;
    image.msaa_newer = false;
    Retire(image.texture);
    image.guest_width = pitch;
    image.guest_height = std::max(rows, image.guest_height);
    if (pitch == 1280) image.guest_height = std::max<uint32_t>(image.guest_height, 720);
    image.width = std::max(1u, uint32_t(std::lround(image.guest_width * scale_x)));
    image.height = std::max(1u, uint32_t(std::lround(image.guest_height * scale_y)));
    image.texture = device->createTexture(
        is_depth ? plume::RenderTextureDesc::DepthTarget(image.width, image.height, kDepthFormat)
                 : plume::RenderTextureDesc::ColorTarget(image.width, image.height, kColorFormat));
    Rec([this, texture = image.texture.get(), is_depth] {
      commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(texture, is_depth ? plume::RenderTextureLayout::DEPTH_WRITE
                                                                       : plume::RenderTextureLayout::COLOR_WRITE));
    });
  }

  // Host samples for the surface the device's RB_SURFACE_INFO describes
  // (bits 16-17: 1, 2 or 4 samples), capped by native_msaa. Draws build their
  // pipeline with it before the target is bound, so both must agree.
  static uint32_t SurfaceSamples(const uint8_t* base, uint32_t guest_device) {
    const uint32_t surface = Reg(base, guest_device, 0x2000);
    const uint32_t guest_samples = 1u << std::min<uint32_t>((surface >> 16) & 3, 2);
    return std::clamp<uint32_t>(guest_samples, 1, uint32_t(std::max<int32_t>(REXCVAR_GET(native_msaa), 1)));
  }

  // Binds the target the device's EDRAM registers describe, creating or
  // growing its images so they hold `rows` guest rows.
  void SelectTarget(const uint8_t* base, uint32_t guest_device, uint32_t rows) {
    const uint32_t surface = Reg(base, guest_device, 0x2000);  // RB_SURFACE_INFO
    const uint32_t color_info = Reg(base, guest_device, 0x2001);
    const uint32_t depth_info = Reg(base, guest_device, 0x2002);
    const uint32_t pitch = std::max<uint32_t>(surface & 0x3FFF, 1);
    const uint32_t samples = SurfaceSamples(base, guest_device);
    rows = std::clamp<uint32_t>(rows, 1, 2048);
    const uint64_t color_key = (uint64_t(pitch) << 32) | (color_info & 0x000F0FFF);
    const uint64_t depth_key = (uint64_t(pitch) << 32) | (depth_info & 0x00010FFF);
    Image& color_image = color_images[color_key];
    Image& depth_image = depth_images[depth_key];
    bool fresh_color = EnsureImage(color_image, pitch, rows, false, samples);
    bool fresh_depth = EnsureImage(depth_image, pitch, rows, true, samples);
    // Both attachments must be the same size.
    if (depth_image.guest_height < color_image.guest_height) {
      fresh_depth |= EnsureImage(depth_image, pitch, color_image.guest_height, true, samples);
    } else if (color_image.guest_height < depth_image.guest_height) {
      fresh_color |= EnsureImage(color_image, pitch, depth_image.guest_height, false, samples);
    }
    if (fresh_color || fresh_depth) {
      for (auto it = targets.begin(); it != targets.end();) {
        if ((fresh_color && it->second.color == &color_image) || (fresh_depth && it->second.depth == &depth_image)) {
          if (current == &it->second) current = nullptr;
          Retire(it->second.framebuffer);
          it = targets.erase(it);
        } else {
          ++it;
        }
      }
    }
    Target& target = targets[(color_key * 1000003u ^ depth_key) * 8 + samples];
    if (!target.framebuffer) {
      target.color = &color_image;
      target.depth = &depth_image;
      target.samples = samples;
      const plume::RenderTexture* attachment =
          samples > 1 ? color_image.msaa.get() : color_image.texture.get();
      plume::RenderFramebufferDesc desc;
      desc.colorAttachments = &attachment;
      desc.colorAttachmentsCount = 1;
      desc.depthAttachment = samples > 1 ? depth_image.msaa.get() : depth_image.texture.get();
      target.framebuffer = device->createFramebuffer(desc);
      target.id = ++next_target_id;
      current = nullptr;
    }
    if (UseTarget(target) && (fresh_color || fresh_depth)) {
      Rec([this, fresh_color, fresh_depth] {
        if (fresh_color) commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
        if (fresh_depth) commands->clearDepthStencil(true, true, 1.0f, 0);
      });
    }
  }

  // Makes `target` the one draws go to. Returns true when it was not already.
  bool UseTarget(Target& target) {
    Image& color_image = *target.color;
    Image& depth_image = *target.depth;
    if (target.samples > 1) {
      color_image.msaa_newer = true;
    } else {
      ResolveMsaa(color_image);
    }
    if (current == &target) return false;
    ++target_switches;
    current = &target;
    current_samples = target.samples;
    color = target.samples > 1 ? color_image.msaa.get() : color_image.texture.get();
    depth = target.samples > 1 ? depth_image.msaa.get() : depth_image.texture.get();
    framebuffer = target.framebuffer.get();
    target_width = color_image.width;
    target_height = color_image.height;
    BindTarget();
    return true;
  }

  uint64_t presents = 0;
  uint64_t next_target_id = 0;
  uint64_t msaa_resolves = 0;
  // Vblanks per flip the game last asked for (2 at 30 Hz, 1 at 60 Hz).
  std::atomic<uint32_t> flip_interval{0};
  uint64_t depth_resolves = 0;

  // GPU objects replaced while a frame is being recorded. The command list
  // may still reference them (a framebuffer, a barrier, a copy), and
  // destroying them invalidates it, which NVIDIA's driver does not survive.
  // They are destroyed once the frame's fence has passed; with two frames
  // in flight that also covers the frame before it, which may use them too.
  struct Retired {
    std::vector<std::unique_ptr<plume::RenderTexture>> textures;
    std::vector<std::unique_ptr<plume::RenderTextureView>> views;
    std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
    std::vector<std::unique_ptr<plume::RenderBuffer>> buffers;
  };
  std::array<Retired, 2> retired;

  // native_clear_new_textures: an upload buffer of zeros, grown on demand,
  // copied into new resolve textures.
  std::unique_ptr<plume::RenderBuffer> zeros;
  uint64_t zeros_size = 0;
  // Fills a new 4-byte-texel texture with zeros, then leaves it for reading.
  void ClearNewTexture(plume::RenderTexture* texture, plume::RenderFormat format, uint32_t width, uint32_t height) {
    const uint32_t row = (width + 63) & ~63u;  // 256-byte rows
    const uint64_t bytes = uint64_t(row) * height * 4;
    if (bytes > zeros_size) {
      if (zeros) retired[frame_slot].buffers.push_back(std::move(zeros));
      zeros = device->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));
      std::memset(zeros->map(), 0, bytes);
      zeros->unmap();
      zeros_size = bytes;
    }
    Rec([this, texture, format, width, height, row, buffer = zeros.get()] {
      commands->barriers(plume::RenderBarrierStage::COPY,
                         plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COPY_DEST));
      commands->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(texture, 0, 0),
          plume::RenderTextureCopyLocation::PlacedFootprint(buffer, format, width, height, 1, row, 0));
      commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::SHADER_READ));
    });
  }

  void Retire(std::unique_ptr<plume::RenderTexture>& texture) {
    if (texture) retired[frame_slot].textures.push_back(std::move(texture));
  }
  void Retire(std::unique_ptr<plume::RenderTextureView>& view) {
    if (view) retired[frame_slot].views.push_back(std::move(view));
  }
  void Retire(std::unique_ptr<plume::RenderFramebuffer>& framebuffer) {
    if (framebuffer) retired[frame_slot].framebuffers.push_back(std::move(framebuffer));
  }
  void DestroyRetired(uint32_t slot) {
    retired[slot].framebuffers.clear();
    retired[slot].views.clear();
    retired[slot].textures.clear();
    retired[slot].buffers.clear();
  }

  // Frames in flight. Each slot has its own command list, fence, acquire
  // semaphore, half of the upload ring and half of the 1D texture pool.
  uint32_t frame_slot = 0;
  uint32_t frames_in_flight = 1;
  std::unique_ptr<plume::RenderCommandList> commands_other;
  std::unique_ptr<plume::RenderCommandFence> fence_other;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire_other;
  std::array<bool, 2> fence_pending{};
  uint64_t upload_begin = 0, upload_end = kUploadBytes;
  uint32_t texture1d_begin = 0, texture1d_end = kTexture1DCount;

  // Descriptor writes, skipped when the slot already holds the same view:
  // a frame still on the GPU may be reading it.
  struct Bound {
    const void* texture = nullptr;
    const void* view = nullptr;
    plume::RenderTextureLayout layout = plume::RenderTextureLayout::UNKNOWN;
  };
  std::array<std::vector<Bound>, 6> bound;

  // The last fetch constant each slot resolved to, valid for one generation:
  // neighbouring draws mostly sample the same textures, and the key and map
  // lookups behind Texture2DFor cost more than the rest of a draw on a phone.
  // A new frame or a resolve (which can turn an address into another texture
  // mid-frame) starts a new generation.
  struct FetchMemo {
    uint32_t words[6] = {};
    uint32_t kind = UINT32_MAX;  // DESCRIPTOR_KIND_* of `index`, or none
    uint32_t index = 0;
    uint32_t sampler = 0;
    uint64_t generation = UINT64_MAX;
  };
  std::array<FetchMemo, DESCRIPTOR_SLOT_COUNT> fetch_memo;
  uint64_t fetch_generation = 0;

  // A large vertex stream drawn again in the same frame (shadow, depth and
  // colour passes of one mesh) at the same place with the same layout keeps
  // the key its full hash gave earlier in the frame, if its sample still
  // matches. The first use in a frame always hashes it whole.
  struct StreamMemo {
    uint64_t key = 0;
    uint64_t sample = 0;
    uint64_t present = UINT64_MAX;
  };
  std::unordered_map<uint64_t, StreamMemo> stream_memo;
  uint64_t fetch_memo_hits = 0, fetch_memo_misses = 0;
  void SetTexture(uint32_t set, uint32_t index, plume::RenderTexture* texture, plume::RenderTextureLayout layout,
                  plume::RenderTextureView* view) {
    std::vector<Bound>& slots = bound[set];
    if (slots.size() <= index) slots.resize(index + 1);
    Bound& b = slots[index];
    if (b.texture == texture && b.view == view && b.layout == layout) return;
    b = {texture, view, layout};
    sets[0]->setTexture(HeapBase(set) + index, texture, layout, view);
  }

  std::mutex mutex;  // Draw and Present run on the guest thread, TakeStats on the sampler
  std::map<std::string, uint64_t> skipped;
  uint64_t drawn = 0;
  double ms_pipeline = 0, ms_texture = 0, ms_stream = 0, ms_submit = 0;
  // Wall time inside each entry point, for the 5 s stats.
  double ms_draw = 0, ms_clear = 0, ms_resolve = 0, ms_present = 0;
  // Draw split in sections: setup, streams, state, constants+textures, indices, record, dump.
  std::array<double, 13> ms_lap{};
  // Reused CPU buffers for gathering a stream before it goes to the
  // write-combined upload ring in one copy.
  std::vector<uint8_t> stream_raw, stream_out;
  // Converted vertex streams kept in their own buffers, keyed by the guest
  // bytes and the layout they were converted with. Static geometry is then
  // converted once instead of every frame. A stream gets a buffer the second
  // time it is seen, so data rewritten every frame stays in the ring.
  struct CachedStream {
    std::unique_ptr<plume::RenderBuffer> buffer;
    std::vector<uint8_t> converted;  // host bytes of a stream too small for a buffer
    uint64_t last_used = 0;
    uint32_t seen = 0;
  };
  HotMap<CachedStream> stream_cache;
  // Indexed draws' host index lists by the guest index bytes. `host` holds
  // indices relative to the lowest one (quad lists already as triangles),
  // so the same bytes serve any base vertex. Lists of 4 KB and more get a
  // GPU buffer of their own when they come back.
  struct CachedIndices {
    std::vector<uint32_t> host;
    uint32_t lo = 0, hi = 0;
    bool empty = false;  // only restart indices
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint32_t seen = 0;
    uint64_t last_used = 0;
  };
  HotMap<CachedIndices> index_cache;
  uint64_t index_hits = 0, index_misses = 0;
  uint64_t stream_hits = 0, stream_misses = 0;
  uint64_t indexed_fetch_draws = 0;
  // Hashes of guest ranges kept while their pages stay unwritten
  // (native_watch_memory), by range and seed.
  struct WatchedHash {
    uint32_t physical = 0, bytes = 0;
    uint64_t seed = 0, hash = 0;
    uint32_t clock = 0;  // watch clock before the bytes were hashed
    uint64_t last_used = 0;
    bool valid = false;
  };
  HotMap<WatchedHash> watched;
  bool watch_registered = false;
  uint64_t watch_hits = 0, watch_misses = 0, watch_paused = 0, watch_arms = 0;
  uint64_t texture_watch_hits = 0;

  void RegisterWatch() {
    if (watch_registered) return;
    watch_registered = true;
    g_watch_page_clock = std::make_unique<std::atomic<uint32_t>[]>(kWatchPages);
    g_watch_page_frame = std::make_unique<std::atomic<uint32_t>[]>(kWatchPages);
    rex::system::kernel_memory()->RegisterPhysicalMemoryInvalidationCallback(OnGuestWrite, nullptr);
  }

  // Write-protects a guest range in the 0xA0000000 view before its bytes are
  // hashed. Returns the watch clock to keep with the hash, or UINT32_MAX when
  // the range is not watched (out of the view, or over a hot page).
  uint32_t ArmWatch(uint32_t address, uint32_t bytes) {
    if (!REXCVAR_GET(native_watch_memory) || bytes == 0 || address < 0xA0000000u ||
        uint64_t(address) + bytes > 0xC0000000u) {
      return UINT32_MAX;
    }
    RegisterWatch();
    const uint32_t physical = address - 0xA0000000u;
    const uint32_t frame = uint32_t(presents) + 1;
    for (uint32_t page = physical >> kWatchPageShift, last = (physical + bytes - 1) >> kWatchPageShift;
         page <= last; ++page) {
      const uint32_t written = g_watch_page_frame[page].load(std::memory_order_relaxed);
      if (written != 0 && frame - written < kWatchHotFrames) return UINT32_MAX;
    }
    ++watch_arms;
    const uint32_t clock = g_watch_clock.load(std::memory_order_acquire);
    rex::system::kernel_memory()->EnablePhysicalMemoryAccessCallbacks(address - 0xA0000000u, bytes, true, false);
    return clock;
  }

  uint64_t HashGuest(const uint8_t* base, uint32_t address, uint32_t bytes, uint64_t seed) {
    // Only the 0xA0000000 view maps physical memory one to one.
    if (!REXCVAR_GET(native_watch_memory) || bytes == 0 || address < 0xA0000000u ||
        uint64_t(address) + bytes > 0xC0000000u) {
      return XXH3_64bits_withSeed(At(base, address), bytes, seed);
    }
    RegisterWatch();
    const uint32_t physical = address - 0xA0000000u;
    WatchedHash& w = watched[(seed ^ ((uint64_t(physical) << 32) | bytes)) * 0x9E3779B97F4A7C15ull];
    if (w.physical != physical || w.bytes != bytes || w.seed != seed) {
      w = WatchedHash{};
      w.physical = physical;
      w.bytes = bytes;
      w.seed = seed;
    }
    w.last_used = presents;
    if (w.valid && WatchedPagesClean(physical, bytes, w.clock)) {
      ++watch_hits;
      return w.hash;
    }
    // The clock is read before the pages are protected: a write landing
    // between the two only costs one more hash.
    w.clock = ArmWatch(address, bytes);
    w.hash = XXH3_64bits_withSeed(At(base, address), bytes, seed);
    w.valid = w.clock != UINT32_MAX;
    ++(w.valid ? watch_misses : watch_paused);
    return w.hash;
  }
  // native_trim_constants: each shader's constant range by shader key,
  // direct-mapped.
  struct ConstantRangeMemo {
    uint64_t key = 0;
    ConstantRange range;
  };
  std::array<ConstantRangeMemo, 1024> constant_ranges{};
  uint64_t constant_bytes = 0, constant_bytes_full = 0;
  ConstantRange ConstantsOf(uint64_t key, const uint8_t* base, uint32_t object, bool vertex, uint32_t registers) {
    ConstantRangeMemo& memo = constant_ranges[(key * 0x9E3779B97F4A7C15ull) >> 54];
    if (memo.key != key || key == 0) {
      memo.key = key;
      memo.range = ShaderConstantRange(base, object, vertex, registers);
    }
    return memo.range;
  }

  std::chrono::steady_clock::time_point lap_start;
  bool timing = true;  // native_draw_timing
  void Lap(size_t section) {
    if (!timing) return;
    const auto now = std::chrono::steady_clock::now();
    ms_lap[section] += std::chrono::duration<double, std::milli>(now - lap_start).count();
    lap_start = now;
  }

  bool Upload(size_t bytes, size_t align, uint64_t& offset) {
    const uint64_t start = (upload_used + align - 1) & ~uint64_t(align - 1);
    if (start + bytes > upload_end) return false;
    offset = start;
    upload_used = start + bytes;
    return true;
  }

  void Skip(const std::string& reason) { ++skipped[reason]; }

  bool CreateTarget() {
    width = swap_chain->getWidth();
    height = swap_chain->getHeight();
    scale_x = float(width) / kGuestWidth;
    scale_y = float(height) / kGuestHeight;
    if (const double scale = REXCVAR_GET(native_resolution_scale); scale > 0.0) {
      scale_x = scale_y = float(std::clamp(scale, 0.25, 4.0));
    }
    targets.clear();
    color_images.clear();
    depth_images.clear();
    current = nullptr;
    color = nullptr;
    depth = nullptr;
    framebuffer = nullptr;
    return width > 0 && height > 0;
  }

  void BeginFrame() {
    if (recording) return;
    recording = true;
    const uint64_t ring_half = kUploadBytes / frames_in_flight;
    upload_begin = ring_half * frame_slot;
    upload_end = upload_begin + ring_half;
    upload_used = upload_begin;
    const uint32_t pool_half = kTexture1DCount / frames_in_flight;
    texture1d_begin = pool_half * frame_slot;
    texture1d_end = texture1d_begin + pool_half;
    next_texture1d = texture1d_begin;
    texture1d_frame.clear();
    journal.clear();
    frame_vs.clear();
    frame_ps.clear();
    frame_shared.clear();
    draw_ops.clear();
    clear_ops.clear();
    resolve_ops.clear();
    commands->begin();
    ForgetBound();
    deferring = recorder.joinable();
    // Targets keep their contents across frames (the game draws before Swap
    // and resolves after it); a new command list only needs them rebound.
    current = nullptr;
  }

  void BindTarget() {
    Rec([this, fb = framebuffer] {
      if (fb) commands->setFramebuffer(fb);
      commands->setGraphicsPipelineLayout(layout.get());
      for (uint32_t i = 0; i < kSetCount; ++i) commands->setGraphicsDescriptorSet(sets[i].get(), i);
    });
  }

  // Command recording thread (native_record_thread). During a frame the game
  // thread keeps all bookkeeping (targets, textures, the upload ring,
  // descriptors) and queues each run of command list calls with the values
  // it needs; the thread records them in order, so the command list is the
  // same as when recorded directly. Present drains the queue and records
  // directly. Turnip's per-draw recording took a quarter of a phone's game
  // thread in fights.
  std::thread recorder;
  double ms_recorder = 0;  // the recording thread's busy time, under record_mutex
  double ms_flush = 0;     // the game thread's wait for it at Present
  uint32_t frames = 0;
  std::array<uint32_t, 16> frames_on_cpu{};  // the game thread's CPU at each Present
#if defined(__ANDROID__)
  std::atomic<int32_t> recorder_tid{0};
  bool game_thread_set = false;
  bool profiling = false;
  PerformanceHint hint;
  std::chrono::steady_clock::time_point last_present{};
#endif
  std::mutex record_mutex;
  std::condition_variable record_wake;
  std::condition_variable record_idle;
  std::vector<std::function<void()>> record_queue;
  bool record_busy = false;    // the thread is running a batch
  bool record_waiting = false;  // the thread sleeps on record_wake
  bool record_quit = false;
  bool deferring = false;  // Rec queues instead of running

  void Rec(std::function<void()> fn) {
    if (!deferring) {
      fn();
      return;
    }
    // A sleeping thread is woken once a batch has gathered: it records
    // faster than draws are prepared, and a wake per draw costs the game
    // thread more than the recording it hands over.
    constexpr size_t kRecordBatch = 64;
    bool wake = false;
    {
      std::lock_guard<std::mutex> lock(record_mutex);
      record_queue.push_back(std::move(fn));
      wake = record_waiting && record_queue.size() >= kRecordBatch;
    }
    if (wake) record_wake.notify_one();
  }

  // Waits until every queued call is recorded.
  void Flush() {
    if (!recorder.joinable()) return;
    std::unique_lock<std::mutex> lock(record_mutex);
    if (record_waiting && !record_queue.empty()) record_wake.notify_one();
    record_idle.wait(lock, [&] { return record_queue.empty() && !record_busy; });
  }

  void RecorderLoop() {
#if defined(__ANDROID__)
    recorder_tid = gettid();
    if (REXCVAR_GET(android_big_cores)) PinToBigCores("recording");
#endif
    std::vector<std::function<void()>> batch;
    std::unique_lock<std::mutex> lock(record_mutex);
    while (true) {
      if (record_queue.empty()) {
        record_idle.notify_all();
        if (record_quit) return;
        record_waiting = true;
        record_wake.wait(lock, [&] { return !record_queue.empty() || record_quit; });
        record_waiting = false;
        continue;
      }
      batch.swap(record_queue);
      record_busy = true;
      lock.unlock();
      const auto t_batch = std::chrono::steady_clock::now();
      for (auto& fn : batch) fn();
      batch.clear();
      const auto busy = std::chrono::steady_clock::now() - t_batch;
      lock.lock();
      ms_recorder += std::chrono::duration<double, std::milli>(busy).count();
      record_busy = false;
    }
  }

  void StopRecorder() {
    if (!recorder.joinable()) return;
    {
      std::lock_guard<std::mutex> lock(record_mutex);
      record_quit = true;
    }
    record_wake.notify_one();
    recorder.join();
  }

  // First flat index of a texture heap in set 0. `heap` is the heap's old
  // set number: 0 2D, 1 3D, 2 cube, 5 1D.
  static uint32_t HeapBase(uint32_t heap) {
    switch (heap) {
      case 1: return kTexture2DCount;
      case 2: return kTexture2DCount + kTexture3DCount;
      case 5: return kTexture2DCount + kTexture3DCount + kTextureCubeCount;
      default: return 0;
    }
  }

  bool CreateLayout() {
    using R = plume::RenderDescriptorRangeType;
    const plume::RenderDescriptorRange textures[4] = {
        plume::RenderDescriptorRange(R::TEXTURE, 0, kTexture2DCount),
        plume::RenderDescriptorRange(R::TEXTURE, 1, kTexture3DCount),
        plume::RenderDescriptorRange(R::TEXTURE, 2, kTextureCubeCount),
        plume::RenderDescriptorRange(R::TEXTURE, 3, kTexture1DCount),
    };
    const plume::RenderDescriptorRange sampler_range(R::SAMPLER, 0, kSamplerCount);
    plume::RenderDescriptorSetDesc set_descs[kSetCount] = {
        plume::RenderDescriptorSetDesc(textures, 4, true, kTexture1DCount),
        plume::RenderDescriptorSetDesc(&sampler_range, 1, true, kSamplerCount),
    };
    for (uint32_t i = 0; i < kSetCount; ++i) {
      std::fprintf(stderr, "native open: descriptor set %u\n", i);
      sets[i] = device->createDescriptorSet(set_descs[i]);
      if (!sets[i]) return false;
    }
    std::fputs("native open: pipeline layout object\n", stderr);
    const plume::RenderPushConstantRange push(0, 0, 0, 24,
                                              plume::RenderShaderStageFlag::VERTEX |
                                                  plume::RenderShaderStageFlag::PIXEL);
    plume::RenderPipelineLayoutDesc desc(&push, 1, set_descs, kSetCount, false, true);
    layout = device->createPipelineLayout(desc);
    if (layout) {
      std::fputs("native open: present pipeline\n", stderr);
      CreatePresentPipeline();
      std::fputs("native open: depth resolve pipeline\n", stderr);
      CreateDepthResolvePipeline();
    }
    return layout != nullptr;
  }

  void CreateDefaults() {
    // White 1x1 at index 0 of every texture heap, uploaded on the first frame.
    using F = plume::RenderFormat;
    defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, F::R8G8B8A8_UNORM)));
    defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, F::R8G8B8A8_UNORM)));
    plume::RenderTextureDesc cube = plume::RenderTextureDesc::Texture2D(1, 1, 1, F::R8G8B8A8_UNORM);
    cube.arraySize = 6;
    cube.flags |= plume::RenderTextureFlag::CUBE;
    defaults.push_back(device->createTexture(cube));
    defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture1D(1, 1, F::R8G8B8A8_UNORM)));
    if (REXCVAR_GET(native_blank_descriptors)) {
      // Transparent black twins (defaults 4-7) for the slots not yet used.
      defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, F::R8G8B8A8_UNORM)));
      defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, F::R8G8B8A8_UNORM)));
      defaults.push_back(device->createTexture(cube));
      defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture1D(1, 1, F::R8G8B8A8_UNORM)));
    }
    samplers.push_back(device->createSampler(plume::RenderSamplerDesc()));
    sets[1]->setSampler(0, samplers[0].get());
  }

  bool defaults_uploaded = false;

  void UploadDefaults() {
    if (defaults_uploaded) return;
    defaults_uploaded = true;
    const uint32_t layers[4] = {1, 1, 6, 1};
    for (uint32_t d = 0; d < defaults.size(); ++d) {
      for (uint32_t layer = 0; layer < layers[d % 4]; ++layer) {
        uint64_t offset = 0;
        if (!Upload(256, 256, offset)) return;
        std::memset(upload_cpu + offset, d < 4 ? 0xFF : 0x00, 4);
        Rec([this, texture = defaults[d].get(), layer, offset] {
          commands->barriers(plume::RenderBarrierStage::COPY,
                             plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COPY_DEST));
          commands->copyTextureRegion(
              plume::RenderTextureCopyLocation::Subresource(texture, 0, layer),
              plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                                1, 1, 1, 64, offset));
        });
      }
      Rec([this, texture = defaults[d].get()] {
        commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::SHADER_READ));
      });
    }
    sets[0]->setTexture(HeapBase(0), defaults[0].get(), plume::RenderTextureLayout::SHADER_READ);
    sets[0]->setTexture(HeapBase(1), defaults[1].get(), plume::RenderTextureLayout::SHADER_READ);
    sets[0]->setTexture(HeapBase(2), defaults[2].get(), plume::RenderTextureLayout::SHADER_READ);
    sets[0]->setTexture(HeapBase(5), defaults[3].get(), plume::RenderTextureLayout::SHADER_READ);
    if (defaults.size() == 8) {
      // Nothing is allocated yet: this runs before the first draw, clear,
      // resolve or present does anything else.
      const uint32_t heaps[4] = {0, 1, 2, 5};
      const uint32_t counts[4] = {kTexture2DCount, kTexture3DCount, kTextureCubeCount, kTexture1DCount};
      for (uint32_t d = 0; d < 4; ++d) {
        for (uint32_t i = 1; i < counts[d]; ++i) {
          sets[0]->setTexture(HeapBase(heaps[d]) + i, defaults[4 + d].get(), plume::RenderTextureLayout::SHADER_READ);
        }
      }
      for (uint32_t i = 1; i < kSamplerCount; ++i) sets[1]->setSampler(i, samplers[0].get());
    }
  }

  bool LoadShaderCache() {
    spirv.resize(g_spirvCacheDecompressedSize);
    const size_t n = ZSTD_decompress(spirv.data(), spirv.size(), g_compressedSpirvCache,
                                     g_spirvCacheCompressedSize);
    if (ZSTD_isError(n) || n != spirv.size()) return false;
    for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
      entries[g_shaderCacheEntries[i].hash] = &g_shaderCacheEntries[i];
    }
    return true;
  }

  plume::RenderShader* Shader(uint64_t key) {
    if (key == 0) return nullptr;
    auto found = shaders.find(key);
    if (found != shaders.end()) return found->second.get();
    auto entry = entries.find(key);
    if (entry == entries.end()) return nullptr;
    const uint8_t* smol = spirv.data() + entry->second->spirvOffset;
    std::vector<uint8_t> code(smolv::GetDecodedBufferSize(smol, entry->second->spirvSize));
    if (code.empty() || !smolv::Decode(smol, entry->second->spirvSize, code.data(), code.size())) {
      return nullptr;
    }
    auto shader = device->createShader(code.data(), code.size(), "main", plume::RenderShaderFormat::SPIRV);
    return shaders.emplace(key, std::move(shader)).first->second.get();
  }

  uint32_t SamplerFor(const xenos::xe_gpu_texture_fetch_t& fetch) {
    const uint32_t key = uint32_t(fetch.clamp_x) | (uint32_t(fetch.clamp_y) << 3) |
                         (uint32_t(fetch.clamp_z) << 6) | (uint32_t(fetch.mag_filter) << 9) |
                         (uint32_t(fetch.min_filter) << 11) | (uint32_t(fetch.mip_filter) << 13);
    auto found = sampler_index.find(key);
    if (found != sampler_index.end()) return found->second;
    if (samplers.size() >= kSamplerCount) return 0;
    plume::RenderSamplerDesc desc;
    desc.addressU = AddressMode(uint32_t(fetch.clamp_x));
    desc.addressV = AddressMode(uint32_t(fetch.clamp_y));
    desc.addressW = AddressMode(uint32_t(fetch.clamp_z));
    desc.magFilter = uint32_t(fetch.mag_filter) == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
    desc.minFilter = uint32_t(fetch.min_filter) == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
    desc.mipmapMode = uint32_t(fetch.mip_filter) == 0 ? plume::RenderMipmapMode::NEAREST : plume::RenderMipmapMode::LINEAR;
    const uint32_t index = uint32_t(samplers.size());
    samplers.push_back(device->createSampler(desc));
    sets[1]->setSampler(index, samplers.back().get());
    sampler_index[key] = index;
    return index;
  }

  // Texture index in the 1D heap for one fetch constant, 0 (white) when the
  // format is not handled or the pool is full.
  uint32_t Texture1DFor(const uint32_t words[6], const xenos::xe_gpu_texture_fetch_t& fetch,
                        const uint8_t* base, bool& uploaded) {
    const uint32_t format = uint32_t(fetch.format);
    const plume::RenderFormat host = HostTextureFormat(format);
    if (host == plume::RenderFormat::UNKNOWN) {
      Skip(fmt::format("1d texture format {}", format));
      return 0;
    }
    const uint32_t width = fetch.size_1d.width + 1;
    const uint32_t pitch = std::max<uint32_t>(fetch.pitch * 32, (width + 31) & ~31u);
    const uint32_t bytes = gfx::GuestTextureBytesAny(format, width, 1, pitch, fetch.tiled);
    if (bytes == 0 || width > 16384) {
      Skip("1d texture size");
      return 0;
    }
    const uint8_t* guest = At(base, PhysicalView(fetch.base_address << 12));
    const uint64_t frame_key = ImageKey(words) * 0x9E3779B97F4A7C15ull ^ XXH3_64bits(guest, bytes);
    if (auto found = texture1d_frame.find(frame_key); found != texture1d_frame.end()) return found->second;
    if (next_texture1d + 1 >= texture1d_end) {
      Skip("1d texture pool full");
      return 0;
    }
    gfx::LinearTexture linear;
    std::string reason;
    if (!gfx::LoadGuestTextureLinear(format, width, 1, pitch, uint32_t(fetch.endianness), fetch.tiled, guest,
                                     bytes, linear, reason)) {
      Skip("1d texture layout");
      return 0;
    }
    const size_t row_bytes = size_t(linear.width_blocks) * linear.block_bytes;
    const size_t row = (row_bytes + 255) & ~size_t(255);
    uint64_t offset = 0;
    if (!Upload(row * linear.height_blocks, 512, offset)) {
      Skip("upload ring full");
      return 0;
    }
    for (uint32_t y = 0; y < linear.height_blocks; ++y) {
      std::memcpy(upload_cpu + offset + y * row, linear.blocks.data() + y * row_bytes, row_bytes);
    }
    if (Dumping() && REXCVAR_GET(native_dump_texture).find("1d") != std::string::npos) {
      const std::string path = fmt::format("logs/texture1d-{}-{:08X}-f{}-w{}.bin", next_texture1d + 1,
                                           fetch.base_address << 12, format, width);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(linear.blocks.data(), 1, linear.blocks.size(), f);
        std::fclose(f);
      }
    }
    if (texture1d_slots.size() <= next_texture1d) texture1d_slots.resize(next_texture1d + 1);
    const uint32_t row_texels = uint32_t(row / linear.block_bytes) * linear.block_width;
    std::vector<uint8_t> cpu;
    if (REXCVAR_GET(native_interpolate)) {
      cpu.assign(row * linear.height_blocks, 0);
      for (uint32_t y = 0; y < linear.height_blocks; ++y) {
        std::memcpy(cpu.data() + y * row, linear.blocks.data() + y * row_bytes, row_bytes);
      }
    }
    const uint32_t index = FillTexture1DSlot(width, host, HostSwizzle(fetch.swizzle, fetch.format), offset,
                                             uint32_t(row * linear.height_blocks), row_texels,
                                             cpu.empty() ? nullptr : cpu.data());
    texture1d_frame[frame_key] = index;
    uploaded = true;
    return index;
  }

  // Takes the next 1D pool slot and copies `bytes` of texels at ring offset
  // `ring` into it. Returns its heap index; the caller checked the pool.
  uint32_t FillTexture1DSlot(uint32_t width, plume::RenderFormat host, uint32_t swizzle, uint64_t ring,
                             uint32_t bytes, uint32_t row_texels, const uint8_t* cpu = nullptr) {
    if (texture1d_slots.size() <= next_texture1d) texture1d_slots.resize(next_texture1d + 1);
    Texture1DSlot& slot = texture1d_slots[next_texture1d];
    const uint32_t index = next_texture1d + 1;
    if (!slot.texture || slot.width != width || slot.format != host || slot.swizzle != swizzle) {
      Retire(slot.view);
      Retire(slot.texture);
      slot.texture = device->createTexture(plume::RenderTextureDesc::Texture1D(width, 1, host));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture1D(host);
      view.componentMapping = ComponentMapping(swizzle);
      slot.view = slot.texture->createTextureView(view);
      slot.width = width;
      slot.format = host;
      slot.swizzle = swizzle;
    }
    slot.ring = ring;
    slot.ring_bytes = bytes;
    slot.row_texels = row_texels;
    if (cpu) {
      slot.cpu.assign(cpu, cpu + bytes);
    } else {
      slot.cpu.clear();
    }
    Rec([this, texture = slot.texture.get(), host, width, row_texels, ring] {
      commands->barriers(plume::RenderBarrierStage::COPY,
                         plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::COPY_DEST));
      commands->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(texture),
          plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), host, width, 1, 1, row_texels, ring));
      commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(texture, plume::RenderTextureLayout::SHADER_READ));
    });
    SetTexture(5, index, slot.texture.get(), plume::RenderTextureLayout::SHADER_READ, slot.view.get());
    ++next_texture1d;
    return index;
  }

  // Texture index in the cube heap for one fetch constant, 0 (white) when the
  // format or layout is not handled. Xenos keeps the six faces as array
  // slices of the base level, each aligned like a separate 2D level.
  uint32_t TextureCubeFor(const uint32_t words[6], const xenos::xe_gpu_texture_fetch_t& fetch,
                          const uint8_t* base, bool& uploaded) {
    const uint64_t key = ImageKey(words);
    auto found = cube_textures.find(key);
    if (found != cube_textures.end() && found->second.checked == presents) return found->second.index;
    const uint32_t format = uint32_t(fetch.format);
    const plume::RenderFormat host = HostTextureFormat(format);
    if (host == plume::RenderFormat::UNKNOWN) {
      Skip(fmt::format("cube texture format {}", format));
      return 0;
    }
    const uint32_t width = fetch.size_2d.width + 1;
    const uint32_t height = fetch.size_2d.height + 1;
    const uint32_t pitch = fetch.pitch * 32;
    const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
        xenos::DataDimension::kCube, fetch.pitch, width, height, 6, fetch.tiled, fetch.format, fetch.packed_mips,
        true, 0);
    const uint32_t face_stride = layout.base.array_slice_stride_bytes;
    const uint32_t face_bytes =
        gfx::GuestTextureBaseBytes(format, width, height, pitch, fetch.tiled, fetch.packed_mips);
    if (face_bytes == 0 || face_stride == 0) {
      Skip("cube texture size");
      return 0;
    }
    const uint8_t* guest = At(base, PhysicalView(fetch.base_address << 12));
    const size_t cube_bytes = size_t(face_stride) * 5 + face_bytes;
    uint64_t content = 0, sample = 0;
    if (found != cube_textures.end()) {
      found->second.checked = presents;
      if (Unchanged(found->second, guest, cube_bytes, content, sample)) {
        found->second.sample = sample;
        return found->second.index;
      }
      ++reuploads;
    } else if (next_texture_cube >= kTextureCubeCount) {
      return 0;
    } else {
      sample = SampleHash(guest, cube_bytes);
      content = cube_bytes <= 16384 ? sample : XXH3_64bits(guest, cube_bytes);
    }
    std::array<gfx::LinearTexture, 6> faces;
    for (uint32_t face = 0; face < 6; ++face) {
      std::string reason;
      if (!gfx::LoadGuestTextureBase(format, width, height, pitch, uint32_t(fetch.endianness), fetch.tiled,
                                     fetch.packed_mips, guest + size_t(face) * face_stride, face_bytes,
                                     faces[face], reason)) {
        Skip("cube texture layout");
        return found != cube_textures.end() ? found->second.index : 0;
      }
    }
    if (found == cube_textures.end()) {
      Texture texture;
      texture.texture = device->createTexture(plume::RenderTextureDesc::Texture(
          plume::RenderTextureDimension::TEXTURE_2D, width, height, 1, 1, 6, host, plume::RenderTextureFlag::CUBE));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::TextureCube(host);
      view.componentMapping = ComponentMapping(HostSwizzle(fetch.swizzle, fetch.format));
      texture.view = texture.texture->createTextureView(view);
      texture.index = next_texture_cube++;
      found = cube_textures.emplace(key, std::move(texture)).first;
    }
    Texture& texture = found->second;
    texture.content = content;
    texture.sample = sample;
    texture.checked = presents;
    Rec([this, target = texture.texture.get()] {
      commands->barriers(plume::RenderBarrierStage::COPY,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COPY_DEST));
    });
    for (uint32_t face = 0; face < 6; ++face) {
      const gfx::LinearTexture& linear = faces[face];
      const size_t row_bytes = size_t(linear.width_blocks) * linear.block_bytes;
      const size_t row = (row_bytes + 255) & ~size_t(255);
      uint64_t offset = 0;
      if (!Upload(row * linear.height_blocks, 512, offset)) {
        Skip("upload ring full");
        return texture.index;
      }
      for (uint32_t y = 0; y < linear.height_blocks; ++y) {
        std::memcpy(upload_cpu + offset + y * row, linear.blocks.data() + y * row_bytes, row_bytes);
      }
      const uint32_t row_texels = uint32_t(row / linear.block_bytes) * linear.block_width;
      Rec([this, target = texture.texture.get(), face, host, width, height, row_texels, offset] {
        commands->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(target, 0, face),
            plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), host, width, height, 1, row_texels,
                                                              offset));
      });
    }
    Rec([this, target = texture.texture.get()] {
      commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::SHADER_READ));
    });
    SetTexture(2, texture.index, texture.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                        texture.view.get());
    uploaded = true;
    return texture.index;
  }

  // Texture index in the 2D heap for one fetch constant, 0 (white) when the
  // format is not handled yet. Texels are uploaded untiled and endian-swapped
  // but otherwise as the guest stores them; the fetch swizzle becomes the
  // view's component mapping. Guest memory is hashed the first time a texture
  // is used in a frame, and changed texels (Bink video, CPU-written images)
  // are uploaded again.
  uint32_t Texture2DFor(const uint32_t words[6], const xenos::xe_gpu_texture_fetch_t& fetch,
                        const uint8_t* base, bool& uploaded) {
    const uint32_t physical = TexturePhysical(fetch.base_address);
    if (auto at = resolved_at.find(physical); at != resolved_at.end()) {
      auto resolved = textures.find(at->second.key);
      // The page is compared once a frame; draws of one frame share it.
      if (resolved != textures.end() &&
          (at->second.checked == presents || at->second.guest == GuestPageHash(base, physical))) {
        at->second.checked = presents;
        return resolved->second.index;
      }
      resolved_at.erase(at);
    }
    const uint64_t key = ImageKey(words);
    auto found = textures.find(key);
    if (found != textures.end() && (found->second.resolved || found->second.checked == presents)) {
      return found->second.index;
    }
    const uint32_t format = uint32_t(fetch.format);
    const plume::RenderFormat host = HostTextureFormat(format);
    if (host == plume::RenderFormat::UNKNOWN) {
      Skip(fmt::format("texture format {}", format));
      return 0;
    }
    const uint32_t width = fetch.size_2d.width + 1;
    const uint32_t height = fetch.size_2d.height + 1;
    const uint32_t pitch = fetch.pitch * 32;
    const uint32_t bytes = gfx::GuestTextureBaseBytes(format, width, height, pitch, fetch.tiled, fetch.packed_mips);
    if (bytes == 0) {
      Skip("texture size");
      return 0;
    }
    const uint32_t guest_address = PhysicalView(fetch.base_address << 12);
    const uint8_t* guest = At(base, guest_address);
    uint64_t content = 0, sample = 0;
    uint32_t watch_clock = UINT32_MAX;
    if (found != textures.end()) {
      Texture& known = found->second;
      known.checked = presents;
      // Watched texels nobody wrote to since they were hashed need no look.
      if (known.watch_clock != UINT32_MAX) {
        if (WatchedPagesClean(guest_address - 0xA0000000u, bytes, known.watch_clock)) {
          ++texture_watch_hits;
          return known.index;
        }
        known.watch_clock = UINT32_MAX;
      }
      // Texels rewritten lately stay unwatched (ArmWatch refuses hot pages)
      // and are compared as before.
      watch_clock = ArmWatch(guest_address, bytes);
      bool unchanged;
      if (watch_clock != UINT32_MAX) {
        // The watch stands in for the periodic full hash from now on, so a
        // newly watched texture is compared whole: a small rewrite the
        // sample misses would otherwise stay unseen for good.
        sample = SampleHash(guest, bytes);
        content = bytes <= 16384 ? sample : XXH3_64bits(guest, bytes);
        unchanged = content == known.content;
      } else {
        unchanged = Unchanged(known, guest, bytes, content, sample);
      }
      if (unchanged) {
        known.sample = sample;
        known.watch_clock = watch_clock;
        return known.index;
      }
      ++reuploads;
    } else if (next_texture2d >= kTexture2DCount) {
      return 0;
    } else {
      watch_clock = ArmWatch(guest_address, bytes);
      sample = SampleHash(guest, bytes);
      content = bytes <= 16384 ? sample : XXH3_64bits(guest, bytes);
    }
    gfx::LinearTexture linear;
    std::string reason;
    if (!gfx::LoadGuestTextureBase(format, width, height, pitch, uint32_t(fetch.endianness), fetch.tiled,
                                   fetch.packed_mips, guest, bytes, linear, reason)) {
      Skip("texture layout");
      return found != textures.end() ? found->second.index : 0;
    }
    const size_t row_bytes = size_t(linear.width_blocks) * linear.block_bytes;
    const size_t row = (row_bytes + 255) & ~size_t(255);
    bool dump_texture = false;
    for (const auto part : std::views::split(std::string_view(REXCVAR_GET(native_dump_texture)), ',')) {
      const std::string want(part.begin(), part.end());
      dump_texture |= !want.empty() && (want == fmt::format("{}x{}", width, height) ||
                                        std::strtoul(want.c_str(), nullptr, 16) == (fetch.base_address << 12));
    }
    if (dump_texture) {
      const std::string path =
          fmt::format("logs/texture-{:08X}-f{}-{}x{}.bin", fetch.base_address << 12, format, width, height);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(linear.blocks.data(), 1, linear.blocks.size(), f);
        std::fclose(f);
      }
    }
    uint64_t offset = 0;
    if (!Upload(row * linear.height_blocks, 512, offset)) {
      Skip("upload ring full");
      return found != textures.end() ? found->second.index : 0;
    }
    for (uint32_t y = 0; y < linear.height_blocks; ++y) {
      std::memcpy(upload_cpu + offset + y * row, linear.blocks.data() + y * row_bytes, row_bytes);
    }
    if (found == textures.end()) {
      Texture texture;
      texture.texture = device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, host));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(host);
      view.componentMapping = ComponentMapping(HostSwizzle(fetch.swizzle, fetch.format));
      texture.view = texture.texture->createTextureView(view);
      texture.index = next_texture2d++;
      found = textures.emplace(key, std::move(texture)).first;
    }
    Texture& texture = found->second;
    texture.content = content;
    texture.sample = sample;
    texture.checked = presents;
    texture.watch_clock = watch_clock;
    const uint32_t row_texels = uint32_t(row / linear.block_bytes) * linear.block_width;
    Rec([this, target = texture.texture.get(), host, width, height, row_texels, offset] {
      commands->barriers(plume::RenderBarrierStage::COPY,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COPY_DEST));
      commands->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(target),
          plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), host, width, height, 1, row_texels,
                                                            offset));
      commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::SHADER_READ));
    });
    SetTexture(0, texture.index, texture.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                        texture.view.get());
    uploaded = true;
    return texture.index;
  }

  // Draws texture `source_index` of the 2D heap into swap chain image
  // `image` through the gamma ramp, as the display controller shows the
  // frontbuffer. `constants` is 16 + 256 * 4 bytes of the upload ring.
  void DrawPresentPass(uint32_t image, uint32_t source_index, uint64_t constants) {
    plume::RenderTexture* target = swap_chain->getTexture(image);
    const uint32_t header[4] = {source_index, PresentSampler(), 0, 0};
    std::memcpy(upload_cpu + constants, header, sizeof(header));
    if (has_gamma_ramp) {
      std::memcpy(upload_cpu + constants + 16, gamma_ramp.data(), 256 * 4);
    } else {
      // No ramp written yet: an identity table.
      std::array<uint32_t, 256> identity;
      for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t v = (i * 1023 + 127) / 255;
        identity[i] = v | (v << 10) | (v << 20);
      }
      std::memcpy(upload_cpu + constants + 16, identity.data(), 256 * 4);
    }
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
    commands->setFramebuffer(SwapFramebuffer(image));
    commands->setGraphicsPipelineLayout(layout.get());
    for (uint32_t i = 0; i < kSetCount; ++i) commands->setGraphicsDescriptorSet(sets[i].get(), i);
    commands->setPipeline(present_pipeline.get());
    ForgetBound();
    const uint64_t push[3] = {upload_gpu + constants, 0, 0};
    commands->setGraphicsPushConstants(0, push, 0, sizeof(push));
    const plume::RenderViewport viewport(0.0f, 0.0f, float(width), float(height));
    commands->setViewports(&viewport, 1);
    const plume::RenderRect scissor(0, 0, int32_t(width), int32_t(height));
    commands->setScissors(&scissor, 1);
    commands->drawInstanced(3, 1, 0, 0);
    commands->barriers(plume::RenderBarrierStage::NONE,
                       plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
    current = nullptr;
  }

  // The real frame, kept while the in-between one is drawn into the same
  // targets and shown first.
  Texture held;
  // The real frame's present pass is a submit of its own: Plume gives a
  // submit one wait stage, so it waits on one swap chain image only.
  // One set per frame slot: with two frames in flight the other slot's
  // second present may still be on the GPU.
  std::array<std::unique_ptr<plume::RenderCommandSemaphore>, 2> acquire_second;
  std::array<std::unique_ptr<plume::RenderCommandList>, 2> commands_second;
  std::array<std::unique_ptr<plume::RenderCommandFence>, 2> fence_second;
  std::array<bool, 2> second_pending{};
  void WaitSecond(uint32_t slot) {
    if (!second_pending[slot]) return;
    queue->waitForCommandFence(fence_second[slot].get());
    second_pending[slot] = false;
  }
  uint64_t interp_frames = 0;
  // native_interpolate_debug: one replayed frame and its real frame read
  // back to logs/interp-{real,replay}.rgba after the frame's fence.
  std::unique_ptr<plume::RenderBuffer> debug_readback;
  uint32_t debug_width = 0, debug_height = 0;
  bool debug_pending = false;
  int32_t debug_dumped = 0;

  // The previous presented frame, for blending: each draw's key and its
  // vertex shader constants (kVsFloats floats per draw).
  static constexpr size_t kVsFloats = gfx::kVertexShaderConstantsBytes / 4;
  static constexpr size_t kPsFloats = gfx::kPixelShaderConstantsBytes / 4;
  // native_interpolate_blend, read once.
  bool blend_motion = true, blend_vs = false, blend_ps = false, blend_bones = true;
  bool blend_read = false;
  uint64_t guarded = 0;  // paired draws whose transform jumped, for the cut rule
  uint64_t snapped = 0;  // constants (motion: matrices) that jumped and kept this frame's value
  uint64_t snapped_by_kind[interp::kMotionKinds] = {};
  struct DrawOp;  // the frame journal's, below
  struct PrevBones {
    uint32_t fetch_slot;  // which of the draw's fetch slots
    uint32_t width;
    plume::RenderFormat format;
    size_t offset;  // into PrevFrame::bones_data
    uint32_t bytes;
  };
  struct PrevFrame {
    std::vector<uint64_t> keys, keys_strict, keys_no_textures, keys_shaders;
    std::vector<float> ps_constants;
    std::vector<float> vs_constants;
    // Half-float 1D textures (skinning bones) per draw: bones[bones_first[i]
    // .. bones_first[i + 1]) belong to draw i.
    std::vector<uint32_t> bones_first;
    std::vector<PrevBones> bones;
    std::vector<uint8_t> bones_data;
    uint32_t interval = 0;
    bool valid = false;
  } prev;
  uint64_t bones_blended = 0;
  // Within one replay: real 1D slot index -> its blended slot index, so the
  // body, outline and shadow passes that share bones share the blend.
  std::unordered_map<uint32_t, uint32_t> blended_slots;

  // The 1D pool slot behind fetch slot `fetch_slot` of a draw, or null.
  const Texture1DSlot* DrawTexture1D(const DrawOp& op, uint32_t fetch_slot) const {
    const auto* indices =
        reinterpret_cast<const uint32_t*>(frame_shared.data() + size_t(op.cpu) * gfx::kSharedConstantsBytes);
    const uint32_t index = indices[DESCRIPTOR_KIND_TEXTURE1D * DESCRIPTOR_SLOT_COUNT + fetch_slot];
    if (index == 0 || index > texture1d_slots.size()) return nullptr;
    return &texture1d_slots[index - 1];
  }
  float last_match = 0.0f;
  // Pairing report for the 5 s log: summed shares and frames, and a sample
  // of one frame's unpaired draws.
  double match_sum = 0, match_no_textures_sum = 0, match_shaders_sum = 0, match_strict_sum = 0;
  uint32_t match_frames = 0;
  std::string unpaired_sample;

  const float* VsCpu(const DrawOp& op) const { return frame_vs.data() + size_t(op.cpu) * kVsFloats; }
  const float* PsCpu(const DrawOp& op) const { return frame_ps.data() + size_t(op.cpu) * kPsFloats; }

  // Keeps what the next frame blends with. Called after the frame's fence,
  // while its upload ring still holds its data.
  void SnapshotFrame() {
    prev.keys.resize(draw_ops.size());
    prev.keys_strict.resize(draw_ops.size());
    prev.ps_constants.resize(draw_ops.size() * kPsFloats);
    prev.keys_no_textures.resize(draw_ops.size());
    prev.keys_shaders.resize(draw_ops.size());
    prev.vs_constants.resize(draw_ops.size() * kVsFloats);
    for (size_t i = 0; i < draw_ops.size(); ++i) {
      prev.keys[i] = draw_ops[i].key;
      prev.keys_strict[i] = draw_ops[i].key_strict;
      std::memcpy(prev.ps_constants.data() + i * kPsFloats, PsCpu(draw_ops[i]), kPsFloats * 4);
      prev.keys_no_textures[i] = draw_ops[i].key_no_textures;
      prev.keys_shaders[i] = draw_ops[i].key_shaders;
      std::memcpy(prev.vs_constants.data() + i * kVsFloats, VsCpu(draw_ops[i]), kVsFloats * 4);
    }
    prev.bones_first.assign(draw_ops.size() + 1, 0);
    prev.bones.clear();
    prev.bones_data.clear();
    for (size_t i = 0; i < draw_ops.size(); ++i) {
      prev.bones_first[i] = uint32_t(prev.bones.size());
      for (uint32_t fetch_slot = 0; fetch_slot < DESCRIPTOR_SLOT_COUNT; ++fetch_slot) {
        const Texture1DSlot* slot = DrawTexture1D(draw_ops[i], fetch_slot);
        if (!slot || slot->format != plume::RenderFormat::R16G16B16A16_FLOAT || slot->cpu.size() != slot->ring_bytes) {
          continue;
        }
        prev.bones.push_back({fetch_slot, slot->width, slot->format, prev.bones_data.size(), slot->ring_bytes});
        prev.bones_data.insert(prev.bones_data.end(), slot->cpu.begin(), slot->cpu.end());
      }
    }
    prev.bones_first[draw_ops.size()] = uint32_t(prev.bones.size());
    prev.interval = flip_interval.load(std::memory_order_relaxed);
    prev.valid = !draw_ops.empty();
  }

  // Records the frame's journal again, on top of what the frame drew, with
  // each paired draw's vertex constants halfway between the previous frame
  // and this one. False, with a reason counted, when an op's target is gone,
  // too few draws pair or the ring has no room; nothing is recorded then.
  bool ReplayFrame() {
    if (journal.empty()) {
      Skip("interp: empty journal");
      return false;
    }
    if (!interp::ShouldInterpolate(true, flip_interval.load(std::memory_order_relaxed), prev.valid, prev.interval)) {
      Skip(prev.valid ? "interp: mode change" : "interp: no previous frame");
      return false;
    }
    if (!blend_read) {
      const std::string parts = REXCVAR_GET(native_interpolate_blend);
      blend_motion = parts.find("motion") != std::string::npos;
      blend_vs = parts.find("vs") != std::string::npos;
      blend_ps = parts.find("ps") != std::string::npos;
      blend_bones = parts.find("bones") != std::string::npos;
      blend_read = true;
    }
    std::vector<uint64_t> keys(draw_ops.size()), keys_strict(draw_ops.size());
    for (size_t i = 0; i < draw_ops.size(); ++i) {
      keys[i] = draw_ops[i].key;
      keys_strict[i] = draw_ops[i].key_strict;
    }
    const std::vector<int32_t> match = interp::MatchDrawsTwoStage(prev.keys_strict, prev.keys, keys_strict, keys);
    last_match = interp::MatchedShare(match);
    {
      std::vector<uint64_t> a(draw_ops.size()), b(draw_ops.size());
      for (size_t i = 0; i < draw_ops.size(); ++i) {
        a[i] = draw_ops[i].key_no_textures;
        b[i] = draw_ops[i].key_shaders;
      }
      const std::vector<int32_t> ma = interp::MatchDraws(prev.keys_no_textures, a);
      const std::vector<int32_t> mb = interp::MatchDraws(prev.keys_shaders, b);
      match_sum += last_match;
      match_strict_sum += interp::MatchedShare(interp::MatchDraws(prev.keys_strict, keys_strict));
      match_no_textures_sum += interp::MatchedShare(ma);
      match_shaders_sum += interp::MatchedShare(mb);
      ++match_frames;
      if (last_match < 0.9f && unpaired_sample.empty()) {
        uint32_t listed = 0;
        for (size_t i = 0; i < match.size() && listed < 24; ++i) {
          if (match[i] >= 0) continue;
          unpaired_sample += fmt::format(" [#{} n={} idx={} notex={} shaders={}]", i, draw_ops[i].count,
                                         draw_ops[i].indexed ? 1 : 0, ma[i] >= 0 ? 1 : 0, mb[i] >= 0 ? 1 : 0);
          ++listed;
        }
        unpaired_sample = fmt::format("{} of {} unpaired:", std::count(match.begin(), match.end(), -1), match.size()) +
                          unpaired_sample;
      }
    }
    if (last_match < 0.9f) {
      Skip("interp: few matches");
      return false;
    }
    // A cut: most paired draws' g_mWorldViewProj moved by more than 0.6 of
    // its length in one step (MatrixMoved). A sharp turn moves it less.
    size_t pairs = 0, jumps = 0;
    for (size_t i = 0; i < match.size(); ++i) {
      if (match[i] < 0 || !(draw_ops[i].motion & interp::kWorldViewProj)) continue;
      ++pairs;
      jumps += interp::MatrixMoved(prev.vs_constants.data() + size_t(match[i]) * kVsFloats, VsCpu(draw_ops[i]), 4,
                                 0.6f);
    }
    guarded += jumps;
    if (pairs > 0 && jumps * 2 > pairs) {
      Skip("interp: camera jump");
      return false;
    }
    const size_t paired = size_t(std::count_if(match.begin(), match.end(), [](int32_t m) { return m >= 0; }));
    blended_slots.clear();
    uint64_t blended = 0, blended_ps = 0;
    if (!Upload(paired * kVsFloats * 4, 256, blended) || !Upload(paired * kPsFloats * 4, 256, blended_ps)) {
      Skip("interp: upload ring full");
      return false;
    }
    std::unordered_map<const Target*, uint64_t> live;
    for (const auto& [key, target] : targets) live[&target] = target.id;
    auto alive = [&](const Target* target, uint64_t id) {
      const auto found = live.find(target);
      return found != live.end() && found->second == id;
    };
    for (const DrawOp& op : draw_ops) {
      if (!alive(op.target, op.target_id)) {
        Skip("interp: target gone");
        return false;
      }
    }
    for (const ClearOp& op : clear_ops) {
      if (!alive(op.target, op.target_id)) {
        Skip("interp: target gone");
        return false;
      }
    }
    for (const ResolveOp& op : resolve_ops) {
      if (!alive(op.target, op.target_id)) {
        Skip("interp: target gone");
        return false;
      }
    }
    for (const FrameOp& op : journal) {
      switch (op.kind) {
        case FrameOp::kDraw: {
          const int32_t pair = match[op.index];
          if (pair < 0) {
            ExecuteDraw(draw_ops[op.index]);
            break;
          }
          DrawOp mid = draw_ops[op.index];
          if (blend_vs) {
            float lerped[kVsFloats];
            snapped += interp::LerpFloatsSnap(prev.vs_constants.data() + size_t(pair) * kVsFloats, VsCpu(mid),
                                              lerped, kVsFloats, 0.5f, 0.5f);
            std::memcpy(upload_cpu + blended, lerped, sizeof(lerped));
            mid.vs_constants = blended;
            blended += kVsFloats * 4;
          } else if (blend_motion) {
            float lerped[kVsFloats];
            snapped += interp::LerpMotion(prev.vs_constants.data() + size_t(pair) * kVsFloats, VsCpu(mid), lerped,
                                          kVsFloats, mid.motion, 0.5f, 0.6f, snapped_by_kind);
            std::memcpy(upload_cpu + blended, lerped, sizeof(lerped));
            mid.vs_constants = blended;
            blended += kVsFloats * 4;
          }
          if (blend_ps) {
            float lerped[kPsFloats];
            snapped += interp::LerpFloatsSnap(prev.ps_constants.data() + size_t(pair) * kPsFloats, PsCpu(mid),
                                              lerped, kPsFloats, 0.5f, 0.5f);
            std::memcpy(upload_cpu + blended_ps, lerped, sizeof(lerped));
            mid.ps_constants = blended_ps;
            blended_ps += kPsFloats * 4;
          }
          if (blend_bones) BlendBones(mid, size_t(pair));
          ExecuteDraw(mid);
          break;
        }
        case FrameOp::kClear: ExecuteClear(clear_ops[op.index]); break;
        case FrameOp::kResolve: ExecuteResolve(resolve_ops[op.index]); break;
      }
    }
    return true;
  }

  // Points `mid`'s half-float 1D textures (bones) at new pool slots holding
  // texels halfway between the paired previous draw's and this one's. A
  // texture without a counterpart of the same size, or no room in the pool
  // or the ring, keeps this frame's texels.
  void BlendBones(DrawOp& mid, size_t pair) {
    if (pair + 1 >= prev.bones_first.size()) return;
    uint64_t shared = 0;
    bool copied = false;
    for (uint32_t b = prev.bones_first[pair]; b < prev.bones_first[pair + 1]; ++b) {
      const PrevBones& old = prev.bones[b];
      const Texture1DSlot* slot = DrawTexture1D(mid, old.fetch_slot);
      if (!slot || slot->format != old.format || slot->width != old.width || slot->cpu.size() != old.bytes) continue;
      const uint32_t real_index = uint32_t(slot - texture1d_slots.data()) + 1;
      uint32_t index = 0;
      if (auto found = blended_slots.find(real_index); found != blended_slots.end()) index = found->second;
      uint64_t ring = 0;
      if (index == 0 && (next_texture1d + 1 >= texture1d_end || !Upload(old.bytes, 512, ring))) {
        Skip("interp: bone pool full");
        return;
      }
      if (!copied) {
        if (!Upload(gfx::kSharedConstantsBytes, 256, shared)) return;
        std::memcpy(upload_cpu + shared, frame_shared.data() + size_t(mid.cpu) * gfx::kSharedConstantsBytes,
                    gfx::kSharedConstantsBytes);
        mid.shared = shared;
        copied = true;
      }
      if (index == 0) {
        std::vector<uint16_t> lerped(old.bytes / 2);
        interp::LerpHalfs(reinterpret_cast<const uint16_t*>(prev.bones_data.data() + old.offset),
                          reinterpret_cast<const uint16_t*>(slot->cpu.data()), lerped.data(), lerped.size(), 0.5f);
        std::memcpy(upload_cpu + ring, lerped.data(), old.bytes);
        index = FillTexture1DSlot(slot->width, slot->format, slot->swizzle, ring, old.bytes, slot->row_texels);
        blended_slots[real_index] = index;
        ++bones_blended;
        mid.uploaded = true;
      }
      // The shared copy was written above; patching the ring is a write.
      auto* indices = reinterpret_cast<uint32_t*>(upload_cpu + mid.shared);
      indices[DESCRIPTOR_KIND_TEXTURE1D * DESCRIPTOR_SLOT_COUNT + old.fetch_slot] = index;
    }
  }

  // Shows an in-between frame, then the real one, as two presents in a row
  // (the FIFO swap chain puts them on consecutive vblanks). `front` is the
  // frontbuffer's entry in `textures`. Records nothing and returns false
  // when either swap chain image or the ring space is missing, or the
  // journal cannot be replayed; the caller then presents as usual.
  // Where the frontbuffer's resolve sits in the journal: its op index and
  // how many draws into its source target came before it. 0 draws before
  // means the resolve copies what the previous frame drew (after Swap).
  std::string front_resolve;
  void NoteFrontResolve(uint64_t front_key) {
    if (!front_resolve.empty()) return;
    for (size_t j = journal.size(); j-- > 0;) {
      if (journal[j].kind != FrameOp::kResolve) continue;
      const ResolveOp& r = resolve_ops[journal[j].index];
      if (r.key != front_key) continue;
      size_t before = 0;
      for (size_t k = 0; k < j; ++k) {
        if (journal[k].kind == FrameOp::kDraw && draw_ops[journal[k].index].target == r.target) ++before;
      }
      front_resolve = fmt::format("{}/{} after {} draws into its target", j, journal.size(), before);
      return;
    }
    front_resolve = fmt::format("none in {} ops", journal.size());
  }

  bool PresentInterpolated(Texture& front) {
    if (!present_pipeline || front.index == 0 || !front.texture) {
      Skip("interp: no frontbuffer");
      return false;
    }
    if (!acquire_second[frame_slot]) {
      acquire_second[frame_slot] = device->createCommandSemaphore();
      commands_second[frame_slot] = queue->createCommandList();
      fence_second[frame_slot] = device->createCommandFence();
    }
    // This slot's last second present (two frames ago) is done with its list.
    WaitSecond(frame_slot);
    uint64_t constants_mid = 0, constants_real = 0;
    if (!Upload(16 + 256 * 4, 256, constants_mid) || !Upload(16 + 256 * 4, 256, constants_real)) {
      Skip("interp: upload ring full");
      return false;
    }
    if (!held.texture || held.width != front.width || held.height != front.height ||
        held.view_swizzle != front.view_swizzle) {
      Retire(held.view);
      Retire(held.texture);
      held.texture = device->createTexture(
          plume::RenderTextureDesc::Texture2D(front.width, front.height, 1, kColorFormat));
      // The copy has the frontbuffer's texels, so it is read the same way.
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(kColorFormat);
      view.componentMapping = ComponentMapping(front.view_swizzle);
      held.view = held.texture->createTextureView(view);
      held.view_swizzle = front.view_swizzle;
      if (held.index == 0) {
        if (next_texture2d >= kTexture2DCount) return false;
        held.index = next_texture2d++;
      }
      held.width = front.width;
      held.height = front.height;
    }
    uint32_t mid_image = 0, real_image = 0;
    if (!swap_chain->acquireTexture(acquire.get(), &mid_image)) {
      Skip("interp: no swap chain image");
      return false;
    }
    const bool second = swap_chain->acquireTexture(acquire_second[frame_slot].get(), &real_image);
    // Keep frame N.
    plume::RenderTextureBarrier to_copy[] = {
        plume::RenderTextureBarrier(front.texture.get(), plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(held.texture.get(), plume::RenderTextureLayout::COPY_DEST)};
    commands->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
    commands->copyTexture(held.texture.get(), front.texture.get());
    plume::RenderTextureBarrier after[] = {
        plume::RenderTextureBarrier(front.texture.get(), plume::RenderTextureLayout::SHADER_READ),
        plume::RenderTextureBarrier(held.texture.get(), plume::RenderTextureLayout::SHADER_READ)};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS, after, 2);
    SetTexture(0, held.index, held.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                        held.view.get());
    const bool replayed = second && ReplayFrame();
    if (!second) Skip("interp: no second swap chain image");
    // native_interpolate_debug: replayed frames only for 150 presents, then
    // real frames only for 150, to compare the two in one run.
    const bool debug = REXCVAR_GET(native_interpolate_debug);
    const bool show_replay = debug && (presents / 150) % 2 == 0;
    if (replayed) {
      DrawPresentPass(mid_image, debug && !show_replay ? held.index : front.index, constants_mid);
      if (replayed && debug_dumped < REXCVAR_GET(native_interpolate_dump) && presents % 450 == 449 &&
          front.format == kColorFormat) {
        // Both pictures of this frame: held (real) and front (replayed).
        const uint32_t row = front.width * 4;
        debug_width = front.width;
        debug_height = front.height;
        debug_readback = device->createBuffer(plume::RenderBufferDesc::ReadbackBuffer(uint64_t(row) * front.height * 2));
        plume::RenderTextureBarrier to_read[] = {
            plume::RenderTextureBarrier(held.texture.get(), plume::RenderTextureLayout::COPY_SOURCE),
            plume::RenderTextureBarrier(front.texture.get(), plume::RenderTextureLayout::COPY_SOURCE)};
        commands->barriers(plume::RenderBarrierStage::COPY, to_read, 2);
        commands->copyTextureRegion(plume::RenderTextureCopyLocation::PlacedFootprint(
                                        debug_readback.get(), kColorFormat, front.width, front.height, 1, front.width, 0),
                                    plume::RenderTextureCopyLocation::Subresource(held.texture.get()));
        commands->copyTextureRegion(
            plume::RenderTextureCopyLocation::PlacedFootprint(debug_readback.get(), kColorFormat, front.width,
                                                              front.height, 1, front.width, uint64_t(row) * front.height),
            plume::RenderTextureCopyLocation::Subresource(front.texture.get()));
        plume::RenderTextureBarrier back_to_read[] = {
            plume::RenderTextureBarrier(held.texture.get(), plume::RenderTextureLayout::SHADER_READ),
            plume::RenderTextureBarrier(front.texture.get(), plume::RenderTextureLayout::SHADER_READ)};
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, back_to_read, 2);
        debug_pending = true;
      }
      // The frontbuffer holds the in-between picture now; a later present
      // that shows it without a new resolve must see the real frame.
      if (!show_replay) {
      plume::RenderTextureBarrier to_restore[] = {
          plume::RenderTextureBarrier(held.texture.get(), plume::RenderTextureLayout::COPY_SOURCE),
          plume::RenderTextureBarrier(front.texture.get(), plume::RenderTextureLayout::COPY_DEST)};
      commands->barriers(plume::RenderBarrierStage::COPY, to_restore, 2);
      commands->copyTexture(front.texture.get(), held.texture.get());
      plume::RenderTextureBarrier restored[] = {
          plume::RenderTextureBarrier(held.texture.get(), plume::RenderTextureLayout::SHADER_READ),
          plume::RenderTextureBarrier(front.texture.get(), plume::RenderTextureLayout::SHADER_READ)};
      commands->barriers(plume::RenderBarrierStage::GRAPHICS, restored, 2);
      }
      ++interp_frames;
    } else {
      // No in-between frame: the real one goes into both images, so each
      // image acquired is shown.
      DrawPresentPass(mid_image, held.index, constants_mid);
    }
    journal_last = journal.size();
    commands->end();
    recording = false;
    const plume::RenderCommandList* list = commands.get();
    plume::RenderCommandSemaphore* wait = acquire.get();
    plume::RenderCommandSemaphore* signal = release[mid_image].get();
    queue->executeCommandLists(&list, 1, &wait, 1, &signal, 1, fence.get());
    swap_chain->present(mid_image, &signal, 1);
    if (second) {
      // The held frame was written above; a later submit on the queue sees it.
      std::swap(commands, commands_second[frame_slot]);
      commands->begin();
      ForgetBound();
      DrawPresentPass(real_image, replayed && show_replay ? front.index : held.index,
                      constants_real);
      commands->end();
      std::swap(commands, commands_second[frame_slot]);
      ForgetBound();
      const plume::RenderCommandList* second_list = commands_second[frame_slot].get();
      plume::RenderCommandSemaphore* second_wait = acquire_second[frame_slot].get();
      plume::RenderCommandSemaphore* second_signal = release[real_image].get();
      queue->executeCommandLists(&second_list, 1, &second_wait, 1, &second_signal, 1,
                                 fence_second[frame_slot].get());
      swap_chain->present(real_image, &second_signal, 1);
      second_pending[frame_slot] = true;
    }
    return true;
  }

  // Frame journal (native_interpolate): what each guest call recorded, in
  // order, so the frame can be recorded a second time. Everything a call read
  // from guest memory is resolved into the op; data lives in the upload ring,
  // which holds until the next BeginFrame.
  struct DrawOp {
    Target* target = nullptr;
    uint64_t target_id = 0;
    uint32_t cpu = 0;  // index into frame_vs and frame_shared
    plume::RenderPipeline* pipeline = nullptr;
    uint64_t vs_constants = 0, ps_constants = 0, shared = 0;  // upload ring offsets
    plume::RenderViewport viewport;
    std::array<plume::RenderVertexBufferView, 16> views;
    std::array<plume::RenderInputSlot, 16> slots;
    uint32_t slot_count = 0;
    bool indexed = false;
    uint64_t index_ring = 0;
    plume::RenderBufferReference index_ref;  // ring or cached buffer
    uint32_t index_count = 0;
    uint32_t vertex_count = 0;
    bool uploaded = false;  // textures were uploaded first, so rebind
    uint64_t key = 0;         // interp::DrawKey of shaders and count (second pairing stage)
    uint64_t key_strict = 0;  // with index range and textures (first stage)
    // Looser keys, only to report how many draws they would pair.
    uint64_t key_no_textures = 0;  // shaders, index range, count
    uint64_t key_shaders = 0;      // shaders and count
    uint32_t count = 0;
    uint32_t motion = 0;  // interp::Motion blocks the vertex shader's constant table names
  };
  struct ClearOp {
    Target* target = nullptr;
    uint64_t target_id = 0;
    uint32_t flags = 0;
    plume::RenderRect rect;
    plume::RenderColor color;
    float z = 0.0f;
    uint32_t stencil = 0;
  };
  struct ResolveOp {
    Target* target = nullptr;
    uint64_t target_id = 0;
    enum Kind { kNone, kColor, kDepth } kind = kNone;
    uint64_t key = 0;  // destination in `textures`
    plume::RenderRect src;
    int32_t dst_x = 0, dst_y = 0;
    int32_t copy_w = 0, copy_h = 0;  // depth
    uint64_t constants = 0;          // depth pass constants in the upload ring
    uint32_t clear_flags = 0;        // 0x100 color, 0x200 depth and stencil
    plume::RenderRect clear_region;
    plume::RenderColor clear_color;
    float z = 0.0f;
  };
  struct FrameOp {
    enum Kind { kDraw, kClear, kResolve } kind;
    uint32_t index;
  };
  std::vector<FrameOp> journal;
  // CPU copies of each draw's vertex constants and shared constants. The
  // upload ring is write-combined on discrete GPUs and slow to read back.
  std::vector<float> frame_vs;
  std::vector<float> frame_ps;
  std::vector<uint8_t> frame_shared;
  std::vector<DrawOp> draw_ops;
  std::vector<ClearOp> clear_ops;
  std::vector<ResolveOp> resolve_ops;
  size_t journal_last = 0;  // ops the last presented frame recorded

  void Record(const DrawOp& op) {
    if (!REXCVAR_GET(native_interpolate)) return;
    journal.push_back({FrameOp::kDraw, uint32_t(draw_ops.size())});
    draw_ops.push_back(op);
  }
  void Record(const ClearOp& op) {
    if (!REXCVAR_GET(native_interpolate)) return;
    journal.push_back({FrameOp::kClear, uint32_t(clear_ops.size())});
    clear_ops.push_back(op);
  }
  void Record(const ResolveOp& op) {
    if (!REXCVAR_GET(native_interpolate)) return;
    journal.push_back({FrameOp::kResolve, uint32_t(resolve_ops.size())});
    resolve_ops.push_back(op);
  }

  // Pipeline, viewport and scissor last set on `commands` by ExecuteDraw.
  // Neighbouring draws mostly repeat them, and each Vulkan call costs a phone
  // (Turnip) noticeable CPU. Anything else that sets them, a new or swapped
  // command list, forgets them.
  const plume::RenderPipeline* bound_pipeline = nullptr;
  plume::RenderViewport bound_viewport{};
  plume::RenderRect bound_scissor{};
  bool bound_dynamic = false;  // bound_viewport and bound_scissor are set

  void ForgetBound() {
    bound_pipeline = nullptr;
    bound_dynamic = false;
  }

  void ExecuteDraw(const DrawOp& op) {
    UseTarget(*op.target);
    if (op.uploaded) BindTarget();
    Rec([this, op, width = target_width, height = target_height] { RecordDraw(op, width, height); });
  }

  // ExecuteDraw's command list calls, with the target size at queue time.
  void RecordDraw(const DrawOp& op, uint32_t target_width, uint32_t target_height) {
    if (bound_pipeline != op.pipeline) {
      commands->setPipeline(op.pipeline);
      bound_pipeline = op.pipeline;
    }
    const uint64_t push[3] = {upload_gpu + op.vs_constants, upload_gpu + op.ps_constants, upload_gpu + op.shared};
    commands->setGraphicsPushConstants(0, push, 0, sizeof(push));
    const plume::RenderRect scissor(0, 0, int32_t(target_width), int32_t(target_height));
    if (!bound_dynamic || std::memcmp(&bound_viewport, &op.viewport, sizeof(op.viewport)) != 0 ||
        std::memcmp(&bound_scissor, &scissor, sizeof(scissor)) != 0) {
      commands->setViewports(&op.viewport, 1);
      commands->setScissors(&scissor, 1);
      bound_viewport = op.viewport;
      bound_scissor = scissor;
      bound_dynamic = true;
    }
    if (op.slot_count) commands->setVertexBuffers(0, op.views.data(), op.slot_count, op.slots.data());
    if (op.indexed) {
      const plume::RenderIndexBufferView view(op.index_ref, op.index_count * 4,
                                              plume::RenderFormat::R32_UINT);
      commands->setIndexBuffer(&view);
      commands->drawIndexedInstanced(op.index_count, 1, 0, 0, 0);
    } else {
      commands->drawInstanced(op.vertex_count, 1, 0, 0);
    }
  }

  void ExecuteClear(const ClearOp& op) {
    UseTarget(*op.target);
    Rec([this, flags = op.flags, color = op.color, rect = op.rect, z = op.z, stencil = op.stencil] {
      if (flags & 0xF) commands->clearColor(0, color, &rect, 1);
      if (flags & 0x30) {
        commands->clearDepthStencil((flags & 0x10) != 0, (flags & 0x20) != 0, std::clamp(z, 0.0f, 1.0f),
                                    stencil & 0xFF, &rect, 1);
      }
    });
  }

  void ExecuteResolve(const ResolveOp& op) {
    UseTarget(*op.target);
    if (op.kind == ResolveOp::kDepth) {
      Texture& entry = textures[op.key];
      Image& depth_image = *op.target->depth;
      SetTexture(0, depth_image.depth_index, depth, plume::RenderTextureLayout::DEPTH_READ,
                          depth_image.depth_view.get());
      Rec([this, op, depth = depth, target = entry.texture.get(), framebuffer = entry.framebuffer.get()] {
        plume::RenderTextureBarrier to_pass[] = {
            plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::DEPTH_READ),
            plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE)};
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, to_pass, 2);
        commands->setFramebuffer(framebuffer);
        commands->setGraphicsPipelineLayout(layout.get());
        for (uint32_t i = 0; i < kSetCount; ++i) commands->setGraphicsDescriptorSet(sets[i].get(), i);
        commands->setPipeline(depth_resolve_pipeline.get());
        ForgetBound();
        const uint64_t push[3] = {upload_gpu + op.constants, 0, 0};
        commands->setGraphicsPushConstants(0, push, 0, sizeof(push));
        const plume::RenderViewport viewport{float(op.dst_x), float(op.dst_y), float(op.copy_w), float(op.copy_h)};
        commands->setViewports(&viewport, 1);
        const plume::RenderRect scissor(op.dst_x, op.dst_y, op.dst_x + op.copy_w, op.dst_y + op.copy_h);
        commands->setScissors(&scissor, 1);
        commands->drawInstanced(3, 1, 0, 0);
        plume::RenderTextureBarrier back[] = {
            plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::DEPTH_WRITE),
            plume::RenderTextureBarrier(target, plume::RenderTextureLayout::SHADER_READ)};
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, back, 2);
      });
      SetTexture(0, entry.index, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                          entry.view.get());
    } else if (op.kind == ResolveOp::kColor) {
      Texture& entry = textures[op.key];
      Rec([this, op, color = color, target = entry.texture.get(), multisampled = current_samples > 1] {
        plume::RenderTextureBarrier to_copy[] = {
            plume::RenderTextureBarrier(color, multisampled ? plume::RenderTextureLayout::RESOLVE_SOURCE
                                                            : plume::RenderTextureLayout::COPY_SOURCE),
            plume::RenderTextureBarrier(target, multisampled ? plume::RenderTextureLayout::RESOLVE_DEST
                                                             : plume::RenderTextureLayout::COPY_DEST)};
        commands->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
        if (multisampled) {
          commands->resolveTextureRegion(target, uint32_t(op.dst_x), uint32_t(op.dst_y), color, &op.src);
        } else {
          const plume::RenderBox box(op.src.left, op.src.top, op.src.right, op.src.bottom);
          commands->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(target),
                                      plume::RenderTextureCopyLocation::Subresource(color), uint32_t(op.dst_x),
                                      uint32_t(op.dst_y), 0, &box);
        }
        plume::RenderTextureBarrier back[] = {
            plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COLOR_WRITE),
            plume::RenderTextureBarrier(target, plume::RenderTextureLayout::SHADER_READ)};
        commands->barriers(plume::RenderBarrierStage::GRAPHICS, back, 2);
      });
      SetTexture(0, entry.index, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                          entry.view.get());
    }
    BindTarget();
    if (op.clear_flags & 0x300) {
      Rec([this, flags = op.clear_flags, color = op.clear_color, region = op.clear_region, z = op.z] {
        if (flags & 0x100) commands->clearColor(0, color, &region, 1);
        if (flags & 0x200) commands->clearDepthStencil(true, true, std::clamp(z, 0.0f, 1.0f), 0, &region, 1);
      });
    }
  }
};

Renderer::Renderer() : state_(std::make_unique<State>()) {}

Renderer::~Renderer() {
  State& s = *state_;
  s.StopRecorder();
  if (s.queue && s.fence && s.fence_pending[s.frame_slot]) s.queue->waitForCommandFence(s.fence.get());
  if (s.queue && s.fence_other && s.fence_pending[s.frame_slot ^ 1]) s.queue->waitForCommandFence(s.fence_other.get());
  if (s.queue) {
    s.WaitSecond(0);
    s.WaitSecond(1);
  }
}

const std::string& Renderer::device_name() const { return state_->device_name; }

bool Renderer::Open(void* native_window, std::string& reason) {
  State& s = *state_;
  // Each step goes to stderr unbuffered before it runs: a driver that
  // crashes the process (the phone) leaves the last one in stderr.txt.
  auto step = [](const char* what) { std::fprintf(stderr, "native open: %s\n", what); };
#if defined(BROKEN_BOND_HAS_ADRENOTOOLS)
  // Adreno 6xx's stock driver lacks shaderInt64 and BC textures; Turnip has
  // both. libadrenotools loads it into this process from the private folder.
  if (const std::string driver = REXCVAR_GET(android_vulkan_driver); !driver.empty()) {
    step("custom vulkan driver");
    const std::filesystem::path path(driver);
    const std::string driver_dir = path.parent_path().string() + "/";
    const std::string hook_dir = REXCVAR_GET(android_native_lib_dir) + "/";
    void* vulkan = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, nullptr, hook_dir.c_str(),
                                              driver_dir.c_str(), path.filename().c_str(), nullptr, nullptr);
    void* get_instance_proc_addr = vulkan ? dlsym(vulkan, "vkGetInstanceProcAddr") : nullptr;
    if (get_instance_proc_addr) {
      plume::SetVulkanGetInstanceProcAddr(get_instance_proc_addr);
      REXLOG_INFO("native: custom Vulkan driver {}", driver);
    } else {
      REXLOG_ERROR("native: custom Vulkan driver {} did not load ({}); using the system one", driver,
                   vulkan ? "no vkGetInstanceProcAddr" : "adrenotools_open_libvulkan failed");
    }
  }
#endif
  step("vulkan interface");
  s.render_interface = plume::CreateVulkanInterface();
  if (!native_window) {
    reason = "the game window has no native handle yet";
    return false;
  }
  if (!s.render_interface) {
    reason = "no Vulkan interface";
    return false;
  }
  step("device");
  s.device = s.render_interface->createDevice();
  if (!s.device) {
    reason = "no Vulkan device";
    return false;
  }
  const plume::RenderDeviceCapabilities& caps = s.device->getCapabilities();
  if (!caps.descriptorIndexing || !caps.bufferDeviceAddress) {
    reason = fmt::format("{} lacks{}{} (XenosRecomp shaders read constants through buffer addresses)",
                         s.device->getDescription().name,
                         caps.descriptorIndexing ? "" : " descriptor indexing",
                         caps.bufferDeviceAddress ? "" : " buffer device address");
    return false;
  }
  s.device_name = s.device->getDescription().name;
  step("queue, fences, command lists");
  s.queue = s.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  s.fence = s.device->createCommandFence();
  s.commands = s.queue->createCommandList();
  s.acquire = s.device->createCommandSemaphore();
  s.frames_in_flight = uint32_t(std::clamp<int32_t>(REXCVAR_GET(native_frames_in_flight), 1, 2));
  if (s.frames_in_flight == 2) {
    s.fence_other = s.device->createCommandFence();
    s.commands_other = s.queue->createCommandList();
    s.acquire_other = s.device->createCommandSemaphore();
  }
  step("swap chain");
  s.swap_chain = s.queue->createSwapChain(plume::RenderSwapChainDesc(
      static_cast<plume::RenderWindow>(native_window), kSwapChainFormat,
      REXCVAR_GET(native_interpolate)
          ? kInterpolationSwapChainBuffersPerFrame * s.frames_in_flight + kInterpolationSwapChainBuffersExtra
          : kSwapChainBuffers));
  if (!s.swap_chain || !s.swap_chain->resize()) {
    reason = "no swapchain on the game window";
    return false;
  }
  while (s.release.size() < s.swap_chain->getTextureCount()) {
    s.release.push_back(s.device->createCommandSemaphore());
  }
  step("render target");
  if (!s.CreateTarget()) {
    reason = "no render target";
    return false;
  }
  step("upload buffer");
  s.upload = s.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
      kUploadBytes, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
                        plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::DEVICE_ADDRESSABLE));
  if (!s.upload) {
    reason = "no upload buffer";
    return false;
  }
  s.upload_cpu = static_cast<uint8_t*>(s.upload->map());
  s.upload_gpu = s.upload->getDeviceAddress();
  step("pipeline layout");
  if (!s.CreateLayout()) {
    reason = "no pipeline layout";
    return false;
  }
  step("default resources");
  s.CreateDefaults();
  step("shader cache");
  if (!s.LoadShaderCache()) {
    reason = "the shader cache did not inflate";
    return false;
  }
  host::ShadowKeepShaderKeys();
  if (REXCVAR_GET(native_record_thread)) {
    s.recorder = std::thread([&s] { s.RecorderLoop(); });
    REXLOG_INFO("native: recording commands on a second thread");
  }
  step("done");
  return true;
}

void Renderer::Draw(const GuestDraw& draw, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  ScopedMs timer(s.ms_draw);
  s.timing = REXCVAR_GET(native_draw_timing);
  if (s.timing) s.lap_start = std::chrono::steady_clock::now();
  s.BeginFrame();
  s.UploadDefaults();
  const uint32_t device = draw.device;

  // Shaders.
  const uint32_t vs_object = Be32(base, device + kDeviceVertexShader);
  const uint32_t ps_object = Be32(base, device + kDevicePixelShader);
  const uint64_t vs_key = host::ShadowShaderKey(vs_object, base);
  const uint64_t ps_key = host::ShadowShaderKey(ps_object, base);
  if (ShaderListed(REXCVAR_GET(native_skip_shaders), vs_key, ps_key)) {
    s.Skip("native_skip_shaders");
    return;
  }
  plume::RenderShader* vs = s.Shader(vs_key);
  plume::RenderShader* ps = ps_object ? s.Shader(ps_key) : nullptr;
  if (!vs || (ps_object && !ps)) {
    s.Skip("shader not in cache");
    return;
  }
  if (REXCVAR_GET(native_debug_target) && vs_key == 0x1089333427EFE5F2ull) {
    s.Skip("debug: title composite");
    return;
  }
  // The vertex shader computes its fetch indices (instanced particles): no
  // vertex inputs, each stream is decoded whole into the fetch table.
  const auto vs_cache_entry = s.entries.find(vs_key);
  const bool indexed_fetch = vs_cache_entry != s.entries.end() &&
                             (vs_cache_entry->second->specConstantsMask & SPEC_CONSTANT_INDEXED_FETCH) != 0;

  s.Lap(7);
  // Topology. Quad lists become triangle lists through the index list.
  plume::RenderPrimitiveTopology topology;
  bool quads = false;
  switch (draw.prim) {
    case 1: topology = plume::RenderPrimitiveTopology::POINT_LIST; break;
    case 2: topology = plume::RenderPrimitiveTopology::LINE_LIST; break;
    case 3: topology = plume::RenderPrimitiveTopology::LINE_STRIP; break;
    case 4: topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case 5: topology = plume::RenderPrimitiveTopology::TRIANGLE_FAN; break;
    case 6: topology = plume::RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    case 13:
      topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      quads = true;
      break;
    default:
      s.Skip(fmt::format("primitive {}", draw.prim));
      return;
  }
  if (draw.count == 0) return;

  // Guest vertex indices of the draw. Vertex v of the draw lives at guest
  // vertex base_vertex + v (indexed) or v (auto).
  std::vector<uint32_t>& list = s.index_scratch;
  list.clear();
  uint32_t index_address = 0;
  State::CachedIndices* indices_entry = nullptr;
  if (draw.indexed) {
    const uint32_t ib = Be32(base, device + kDeviceIndices);
    if (ib == 0) {
      s.Skip("index buffer");
      return;
    }
    const bool index32 = (Be32(base, ib) & 0x80000000u) != 0;
    const uint32_t address = PhysicalView(Be32(base, ib + 24));
    index_address = address;
    const size_t index_size = index32 ? 4 : 2;
    const uint8_t* src = At(base, address + uint32_t(index_size) * draw.start);
    const uint64_t index_key = s.HashGuest(base, address + uint32_t(index_size) * draw.start,
                                           uint32_t(index_size * draw.count), (uint64_t(quads) << 1) | uint64_t(index32));
    State::CachedIndices& entry = s.index_cache[index_key];
    entry.last_used = s.presents;
    if (entry.seen == 0) {
      ++s.index_misses;
      list.resize(draw.count);
      if (index32) {
        for (uint32_t i = 0; i < draw.count; ++i) {
          uint32_t v;
          std::memcpy(&v, src + 4 * size_t(i), 4);
          list[i] = __builtin_bswap32(v);
        }
      } else {
        for (uint32_t i = 0; i < draw.count; ++i) {
          uint16_t v;
          std::memcpy(&v, src + 2 * size_t(i), 2);
          list[i] = __builtin_bswap16(v);
        }
      }
      uint32_t lo = UINT32_MAX, hi = 0;
      for (uint32_t index : list) {
        if (index == 0xFFFFu || index == 0xFFFFFFFFu) continue;
        lo = std::min(lo, index);
        hi = std::max(hi, index);
      }
      entry.empty = lo == UINT32_MAX;
      entry.lo = lo;
      entry.hi = hi;
      auto host = [&](uint32_t i) -> uint32_t {
        const uint32_t index = list[i];
        return (index == 0xFFFFu || index == 0xFFFFFFFFu) ? 0xFFFFFFFFu : index - lo;
      };
      entry.host.clear();
      if (!entry.empty && quads) {
        entry.host.reserve(draw.count / 4 * 6);
        for (uint32_t q = 0; q + 3 < draw.count; q += 4) {
          const uint32_t a = host(q), b = host(q + 1), c = host(q + 2), d = host(q + 3);
          entry.host.insert(entry.host.end(), {a, b, c, a, c, d});
        }
      } else if (!entry.empty) {
        entry.host.resize(draw.count);
        for (uint32_t i = 0; i < draw.count; ++i) entry.host[i] = host(i);
      }
    } else {
      ++s.index_hits;
    }
    ++entry.seen;
    if (entry.empty) return;
    indices_entry = &entry;
  }
  uint32_t first = 0, last = 0;
  if (draw.indexed) {
    first = draw.base_vertex + indices_entry->lo;
    last = draw.base_vertex + indices_entry->hi;
  } else {
    first = draw.start;
    last = draw.start + draw.count - 1;
  }
  const uint32_t vertex_count = last - first + 1;
  if (vertex_count > (1u << 20)) {
    s.Skip("vertex range");
    return;
  }

  s.Lap(0);
  // Declaration: every element is gathered into a host stream per guest
  // stream, converting what Vulkan cannot fetch (16-bit integers) to float.
  struct Element {
    uint32_t stream, offset, usage, usage_index, location;
    uint32_t in_size, out_size, out_offset;
    uint32_t format_word;
    ElementConvert convert;
    ElementSource source;
    plume::RenderFormat format;
  };
  const uint32_t decl = Be32(base, device + kDeviceDecl);
  const uint32_t element_count = decl ? std::min<uint32_t>(Be32(base, decl + 24), 16) : 0;
  std::array<Element, 16> elems;
  bool packed_normals = false;
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t at = decl + 52 + i * 12;
    Element& e = elems[i];
    e.stream = Be16(base, at);
    e.offset = Be16(base, at + 2);
    const uint32_t format_word = Be32(base, at + 4);
    e.format_word = format_word;
    e.usage = *At(base, at + 9);
    e.usage_index = *At(base, at + 10);
    e.location = UsageLocation(e.usage, e.usage_index);
    bool packed = false;
    if (e.stream >= 16 || e.location == UINT32_MAX ||
        !ElementFormat(e.usage, format_word, e.format, e.in_size, e.out_size, e.convert, e.source, packed)) {
      s.Skip(fmt::format("vertex element usage {} format {:03X}", e.usage, format_word & 0x33F));
      return;
    }
    packed_normals |= packed;
  }
  // One spec constant makes the vertex shader unpack NORMAL, TANGENT and
  // BINORMAL alike, so when any of them is packed 11:11:10 the others are
  // repacked that way too.
  if (packed_normals) {
    for (uint32_t i = 0; i < element_count; ++i) {
      if (elems[i].convert == ElementConvert::kFloat4Bits) {
        elems[i].convert = ElementConvert::kPacked101111;
        elems[i].format = plume::RenderFormat::R32_UINT;
        elems[i].out_size = 4;
      }
    }
  }

  std::array<plume::RenderInputElement, 16> elements;
  std::array<plume::RenderInputSlot, 16> slots;
  std::array<plume::RenderVertexBufferView, 16> views;
  uint32_t slot_count = 0;
  // Per vertex location: ring offset and element count of the decoded stream.
  std::array<uint64_t, 16> fetch_ring{};
  std::array<uint32_t, 16> fetch_count{};

  XXH3_state_t key_state;
  XXH3_64bits_reset(&key_state);
  const auto t_stream = s.timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  for (uint32_t stream = 0; stream < 16; ++stream) {
    uint32_t out_stride = 0;
    for (uint32_t i = 0; i < element_count; ++i) {
      if (elems[i].stream != stream) continue;
      elems[i].out_offset = out_stride;
      out_stride += elems[i].out_size;
    }
    if (out_stride == 0) continue;
    const uint32_t fetch = device + kDeviceFetch + 8 * (95 - stream);
    xenos::xe_gpu_vertex_fetch_t vf;
    vf.dword_0 = Be32(base, fetch);
    vf.dword_1 = Be32(base, fetch + 4);
    const uint32_t stride = uint32_t(*At(base, device + kDeviceStrides + stream)) * 4;
    if (indexed_fetch) {
      // Everything the fetch constant covers, since the shader may read any
      // element of it.
      const uint32_t count = stride ? std::min<uint32_t>(uint32_t(vf.size) * 4 / stride, 1u << 16) : 0;
      if (count == 0) continue;
      std::vector<uint8_t>& raw = s.stream_raw;
      raw.resize(size_t(count) * stride);
      std::memcpy(raw.data(), At(base, PhysicalView(vf.address * 4)), raw.size());
      SwapDwords(raw.data(), raw.size(), uint32_t(vf.endian));
      for (uint32_t i = 0; i < element_count; ++i) {
        const Element& e = elems[i];
        if (e.stream != stream || e.offset + e.in_size > stride) continue;
        uint64_t ring = 0;
        if (!s.Upload(size_t(count) * 16, 16, ring)) {
          s.Skip("upload ring full");
          return;
        }
        // XenosRecomp reads BLENDINDICES as integers whatever the format says.
        ElementSource source = e.source;
        if (e.usage == uint32_t(DeclUsage::kBlendIndices)) source.is_integer = true;
        for (uint32_t v = 0; v < count; ++v) {
          ConvertElement(raw.data() + size_t(v) * stride + e.offset, s.upload_cpu + ring + size_t(v) * 16, e.in_size,
                         ElementConvert::kFloat4, source, uint32_t(vf.endian));
        }
        fetch_ring[e.location] = ring;
        fetch_count[e.location] = count;
      }
      continue;
    }
    const uint64_t begin = uint64_t(first) * stride;
    const uint64_t bytes = uint64_t(vertex_count) * stride;
    if (stride == 0 || begin + bytes > uint64_t(vf.size) * 4) {
      s.last_bounds = fmt::format("stream {} stride {} first {} count {} base {} fetch_bytes {} indexed {} start {} n {}",
                                  stream, stride, first, vertex_count, draw.base_vertex, vf.size * 4,
                                  draw.indexed, draw.start, draw.count);
      if (draw.indexed) {
        const uint32_t ib = Be32(base, device + kDeviceIndices);
        s.last_bounds += fmt::format(" ib {:08X} flags {:08X} addr {:08X} words", ib, Be32(base, ib),
                                     Be32(base, ib + 24));
        for (uint32_t w = 0; w < 8; ++w) s.last_bounds += fmt::format(" {:08X}", Be32(base, ib + 4 * w));
        s.last_bounds += " idx";
        for (size_t i = 0; i < std::min<size_t>(list.size(), 12); ++i) s.last_bounds += fmt::format(" {}", list[i]);
      }
      s.Skip("vertex stream bounds");
      return;
    }
    if (slot_count == 0 && !std::string_view(REXCVAR_GET(native_skip_verts)).empty()) {
      for (const auto part : std::views::split(std::string_view(REXCVAR_GET(native_skip_verts)), ',')) {
        const std::string_view want(part.begin(), part.end());
        if (!want.empty() && want == std::to_string(vertex_count)) {
          s.Skip("native_skip_verts");
          return;
        }
      }
    }
    const uint8_t* guest_stream = At(base, PhysicalView(vf.address * 4 + uint32_t(begin)));
    const size_t out_bytes = size_t(vertex_count) * out_stride;
    // The layout words are hashed in one call: XXH3's streaming state costs
    // more than these few dozen bytes on a phone.
    uint32_t layout[4 + 6 * 16];
    size_t layout_words = 0;
    for (const uint32_t w : {stride, out_stride, uint32_t(vf.endian), uint32_t(bytes)}) layout[layout_words++] = w;
    for (uint32_t i = 0; i < element_count; ++i) {
      const Element& e = elems[i];
      if (e.stream != stream) continue;
      for (const uint32_t w : {e.offset, e.format_word, e.out_offset, uint32_t(e.convert), uint32_t(e.format),
                               e.source.swizzle}) {
        layout[layout_words++] = w;
      }
    }
    const uint64_t stream_key = s.HashGuest(base, PhysicalView(vf.address * 4 + uint32_t(begin)), uint32_t(bytes),
                                            XXH3_64bits(layout, layout_words * sizeof(uint32_t)));
    s.Lap(8);
    State::CachedStream& cached = s.stream_cache[stream_key];
    cached.last_used = s.presents;
    plume::RenderBufferReference stream_ref;
    if (cached.buffer) {
      ++s.stream_hits;
      stream_ref = cached.buffer->at(0);
    } else if (cached.converted.size() == out_bytes && out_bytes != 0) {
      // Small and seen before: the converted bytes, copied into the ring.
      ++s.stream_hits;
      uint64_t ring = 0;
      if (!s.Upload(out_bytes, 16, ring)) {
        s.Skip("upload ring full");
        return;
      }
      std::memcpy(s.upload_cpu + ring, cached.converted.data(), out_bytes);
      stream_ref = s.upload->at(ring);
    } else {
      ++s.stream_misses;
      std::vector<uint8_t>& raw = s.stream_raw;
      raw.resize(bytes);
      std::memcpy(raw.data(), guest_stream, bytes);
      SwapDwords(raw.data(), raw.size(), uint32_t(vf.endian));
      s.stream_out.assign(out_bytes, 0);
      uint8_t* out = s.stream_out.data();
      for (uint32_t v = 0; v < vertex_count; ++v) {
        const uint8_t* src = raw.data() + size_t(v) * stride;
        uint8_t* dst = out + size_t(v) * out_stride;
        for (uint32_t i = 0; i < element_count; ++i) {
          const Element& e = elems[i];
          if (e.stream != stream) continue;
          if (v == 0) ++s.stream_endians[uint32_t(vf.endian) * 4 + ((e.format_word >> 6) & 3)];
          ConvertElement(src + e.offset, dst + e.out_offset, e.in_size, e.convert, e.source,
                         uint32_t(vf.endian));
        }
      }
      if (++cached.seen >= 2 && out_bytes >= 4096) {
        cached.buffer = s.device->createBuffer(
            plume::RenderBufferDesc::UploadBuffer(out_bytes, plume::RenderBufferFlag::VERTEX));
      }
      if (cached.buffer) {
        void* mapped = cached.buffer->map();
        std::memcpy(mapped, out, out_bytes);
        cached.buffer->unmap();
        stream_ref = cached.buffer->at(0);
      } else {
        uint64_t ring = 0;
        if (!s.Upload(out_bytes, 16, ring)) {
          s.Skip("upload ring full");
          return;
        }
        std::memcpy(s.upload_cpu + ring, out, out_bytes);
        stream_ref = s.upload->at(ring);
        if (out_bytes < 4096) cached.converted.assign(out, out + out_bytes);
      }
    }
    s.Lap(9);
    slots[slot_count] = plume::RenderInputSlot(slot_count, out_stride);
    views[slot_count] = plume::RenderVertexBufferView(stream_ref, uint32_t(out_bytes));
    for (uint32_t i = 0; i < element_count; ++i) {
      if (elems[i].stream != stream) continue;
      elements[i] = plume::RenderInputElement(UsageSemantic(elems[i].usage), elems[i].usage_index,
                                              elems[i].location, elems[i].format, slot_count,
                                              elems[i].out_offset);
      const uint32_t element_key[4] = {elems[i].location, uint32_t(elems[i].format), slot_count,
                                       elems[i].out_offset};
      XXH3_64bits_update(&key_state, element_key, sizeof(element_key));
    }
    XXH3_64bits_update(&key_state, &out_stride, 4);
    ++slot_count;
  }
  if (s.timing) {
    s.ms_stream += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_stream).count();
  }
  if (indexed_fetch) {
    ++s.indexed_fetch_draws;
    XXH3_64bits_update(&key_state, "indexed fetch", 13);
  }

  s.Lap(1);
  // Render state.
  const uint32_t depth_control = Reg(base, device, kRbDepthControl);
  const uint32_t blend_control = Reg(base, device, kRbBlendControl0);
  const uint32_t color_control = Reg(base, device, kRbColorControl);
  uint32_t color_mask = Reg(base, device, kRbColorMask) & 0xF;
  {
    static constexpr uint32_t kTintMasks[] = {0x1, 0x2, 0x4, 0x3};
    uint32_t n = 0;
    for (const auto part : std::views::split(std::string_view(REXCVAR_GET(native_tint_verts)), ',')) {
      const std::string_view want(part.begin(), part.end());
      if (n < 4 && !want.empty() && want == std::to_string(vertex_count) && color_mask != 0) {
        color_mask = kTintMasks[n];
      }
      ++n;
    }
  }
  const bool alpha_test = (color_control & 0x8) != 0;
  uint32_t spec = 0;
  const auto vs_entry = s.entries.find(vs_key);
  const auto ps_entry = s.entries.find(ps_key);
  if (packed_normals && vs_entry != s.entries.end() &&
      (vs_entry->second->specConstantsMask & SPEC_CONSTANT_R11G11B10_NORMAL)) {
    spec |= SPEC_CONSTANT_R11G11B10_NORMAL;
  }
  if (alpha_test && ps_entry != s.entries.end() &&
      (ps_entry->second->specConstantsMask & SPEC_CONSTANT_ALPHA_TEST)) {
    spec |= SPEC_CONSTANT_ALPHA_TEST;
  }
  // Culling only applies to triangles; both bits set is treated as none.
  const bool triangles = topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST ||
                         topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  const uint32_t sc_mode = triangles ? Reg(base, device, kPaSuScModeCntl) & 7 : 0;
  if (triangles) ++s.cull_modes[sc_mode];
  // Anything but no culling or back faces with counter-clockwise front.
  if (REXCVAR_GET(native_dump_outline) && s.outline_dump == 0 && sc_mode != 0 && sc_mode != 2) {
    s.outline_dump = s.presents + 61;
  }
  const uint32_t stencil_ref = (depth_control & 1) ? Reg(base, device, kRbStencilRefMask) & 0xFFFFFF : 0;
  // Polygon offset: PA_SU_SC_MODE_CNTL bits 11 and 12 enable it for front
  // and back faces, PA_SU_POLY_OFFSET_{FRONT,BACK}_{SCALE,OFFSET} hold the
  // slope scale and the constant. Decals and shadow overlays drawn onto
  // geometry use it; without it they fight the surface for depth and
  // flicker as the camera moves.
  int32_t depth_bias = 0;
  float slope_bias = 0.0f;
  if (triangles) {
    const uint32_t sc_full = Reg(base, device, kPaSuScModeCntl);
    if (sc_full & 0x1800) {
      const uint32_t reg = (sc_full & 0x800) ? 0x2380 : 0x2382;
      const float scale = RegFloat(base, device, reg);
      const float offset = RegFloat(base, device, reg + 1);
      // The offset counts in 24-bit depth steps; the slope scale in 1/16
      // pixel units. A D32_FLOAT step at depth 0.5-1 is 2^-24, so the
      // constant carries over as is. Values below 1 are taken as depth.
      const float steps = std::fabs(offset) < 1.0f ? offset * 16777216.0f : offset;
      depth_bias = int32_t(std::lround(std::clamp(steps, -1.0e6f, 1.0e6f)));
      slope_bias = scale / 16.0f;
      static int logged = 0;
      if (logged < 4) {
        ++logged;
        REXLOG_INFO("native: polygon offset sc_mode={:08X} scale={} offset={} -> bias {} slope {}", sc_full, scale,
                    offset, depth_bias, slope_bias);
      }
    }
  }
  const uint32_t state_key[10] = {(depth_control & 1) ? depth_control : depth_control & 0x77,
                                  blend_control, color_mask, uint32_t(topology), spec, uint32_t(ps_object != 0),
                                  sc_mode, stencil_ref, uint32_t(depth_bias), std::bit_cast<uint32_t>(slope_bias)};
  const uint32_t target_samples = State::SurfaceSamples(base, device);
  XXH3_64bits_update(&key_state, &target_samples, sizeof(target_samples));
  XXH3_64bits_update(&key_state, state_key, sizeof(state_key));
  XXH3_64bits_update(&key_state, &vs_key, 8);
  XXH3_64bits_update(&key_state, &ps_key, 8);
  const uint64_t pipeline_key = XXH3_64bits_digest(&key_state);

  plume::RenderPipeline* pipeline = nullptr;
  if (auto found = s.pipelines.find(pipeline_key); found != s.pipelines.end()) {
    pipeline = found->second.get();
  } else {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.layout.get();
    desc.vertexShader = vs;
    desc.pixelShader = ps;
    desc.primitiveTopology = topology;
    // PA_SU_SC_MODE_CNTL: bit 0 culls front faces, bit 1 back faces, bit 2
    // makes clockwise triangles the front ones. Characters draw their
    // outline as an inflated shell with front faces culled.
    switch (sc_mode & 3) {
      case 1: desc.cullMode = plume::RenderCullMode::FRONT; break;
      case 2: desc.cullMode = plume::RenderCullMode::BACK; break;
      default: desc.cullMode = plume::RenderCullMode::NONE; break;
    }
    desc.frontFace = (sc_mode & 4) ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;
    desc.depthClipEnabled = true;
    desc.depthEnabled = (depth_control & 0x2) != 0 && !REXCVAR_GET(native_debug_target);
    desc.depthWriteEnabled = (depth_control & 0x4) != 0;
    desc.depthFunction = CompareFunction((depth_control >> 4) & 7);
    desc.depthBias = depth_bias;
    desc.slopeScaledDepthBias = slope_bias;
    // Stencil: characters mark their pixels so the outline shell drawn after
    // them only shows outside the body. One reference and read mask serve
    // both faces on the host; Xenos keeps a set per face.
    desc.stencilEnabled = (depth_control & 1) != 0;
    if (desc.stencilEnabled) {
      desc.stencilReference = stencil_ref & 0xFF;
      desc.stencilReadMask = (stencil_ref >> 8) & 0xFF;
      desc.stencilWriteMask = (stencil_ref >> 16) & 0xFF;
      desc.stencilFrontFace = StencilFace(depth_control, 8);
      desc.stencilBackFace = (depth_control & 0x80) ? StencilFace(depth_control, 20) : desc.stencilFrontFace;
    }
    desc.depthTargetFormat = kDepthFormat;
    desc.multisampling = plume::RenderMultisampling(static_cast<plume::RenderSampleCounts>(target_samples));
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = kColorFormat;
    plume::RenderBlendDesc& blend = desc.renderTargetBlend[0];
    blend.srcBlend = BlendFactor(blend_control & 0x1F);
    blend.blendOp = BlendOp((blend_control >> 5) & 7);
    blend.dstBlend = BlendFactor((blend_control >> 8) & 0x1F);
    blend.srcBlendAlpha = BlendFactor((blend_control >> 16) & 0x1F);
    blend.blendOpAlpha = BlendOp((blend_control >> 21) & 7);
    blend.dstBlendAlpha = BlendFactor((blend_control >> 24) & 0x1F);
    // ONE, ZERO, ADD for both halves is blending off.
    blend.blendEnabled = (blend_control & 0x1FFF1FFF) != 0x00010001;
    blend.renderTargetWriteMask = uint8_t(color_mask);
    desc.inputSlots = slots.data();
    desc.inputSlotsCount = slot_count;
    desc.inputElements = elements.data();
    desc.inputElementsCount = indexed_fetch ? 0 : element_count;
    const plume::RenderSpecConstant spec_constant(0, spec);
    desc.specConstants = &spec_constant;
    desc.specConstantsCount = 1;
    const auto t0 = std::chrono::steady_clock::now();
    auto created = s.device->createGraphicsPipeline(desc);
    s.ms_pipeline += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!created) {
      s.Skip("pipeline");
      return;
    }
    pipeline = s.pipelines.emplace(pipeline_key, std::move(created)).first->second.get();
  }

  // Constants: guest floats are big-endian dwords.
  uint64_t vs_constants = 0, ps_constants = 0, shared = 0;
  if (!s.Upload(gfx::kVertexShaderConstantsBytes, 256, vs_constants) ||
      !s.Upload(gfx::kPixelShaderConstantsBytes, 256, ps_constants) ||
      !s.Upload(gfx::kSharedConstantsBytes, 256, shared)) {
    s.Skip("upload ring full");
    return;
  }
  s.Lap(2);
  uint32_t cpu_index = 0;
  if (REXCVAR_GET(native_interpolate)) {
    cpu_index = uint32_t(s.frame_vs.size() / State::kVsFloats);
    s.frame_vs.resize(s.frame_vs.size() + State::kVsFloats);
    auto* vs_cpu = reinterpret_cast<uint8_t*>(s.frame_vs.data() + size_t(cpu_index) * State::kVsFloats);
    CopySwappedDwords(vs_cpu, At(base, device + kDeviceVsConstants), gfx::kVertexShaderConstantsBytes);
    std::memcpy(s.upload_cpu + vs_constants, vs_cpu, gfx::kVertexShaderConstantsBytes);
  } else if (REXCVAR_GET(native_trim_constants) && !s.Dumping()) {
    // Registers outside the range stay as the ring left them: the shader
    // never reads them.
    const ConstantRange r =
        s.ConstantsOf(vs_key, base, vs_object, true, gfx::kVertexShaderConstantsBytes / 16);
    CopySwappedDwords(s.upload_cpu + vs_constants + r.first * 16u,
                      At(base, device + kDeviceVsConstants + r.first * 16u), (r.end - r.first) * 16u);
    s.constant_bytes += (r.end - r.first) * 16u;
    s.constant_bytes_full += gfx::kVertexShaderConstantsBytes;
  } else {
    CopySwappedDwords(s.upload_cpu + vs_constants, At(base, device + kDeviceVsConstants),
                      gfx::kVertexShaderConstantsBytes);
  }
  if (REXCVAR_GET(native_interpolate)) {
    s.frame_ps.resize((size_t(cpu_index) + 1) * State::kPsFloats);
    auto* ps_cpu = reinterpret_cast<uint8_t*>(s.frame_ps.data() + size_t(cpu_index) * State::kPsFloats);
    CopySwappedDwords(ps_cpu, At(base, device + kDevicePsConstants), gfx::kPixelShaderConstantsBytes);
    std::memcpy(s.upload_cpu + ps_constants, ps_cpu, gfx::kPixelShaderConstantsBytes);
  } else if (REXCVAR_GET(native_trim_constants) && !s.Dumping()) {
    const ConstantRange r =
        s.ConstantsOf(ps_key, base, ps_object, false, gfx::kPixelShaderConstantsBytes / 16);
    CopySwappedDwords(s.upload_cpu + ps_constants + r.first * 16u,
                      At(base, device + kDevicePsConstants + r.first * 16u), (r.end - r.first) * 16u);
    s.constant_bytes += (r.end - r.first) * 16u;
    s.constant_bytes_full += gfx::kPixelShaderConstantsBytes;
  } else {
    CopySwappedDwords(s.upload_cpu + ps_constants, At(base, device + kDevicePsConstants),
                      gfx::kPixelShaderConstantsBytes);
  }

  s.Lap(10);
  // Shared constants are built in local memory and copied once.
  alignas(16) uint8_t shared_cpu[gfx::kSharedConstantsBytes] = {};
  auto* indices = reinterpret_cast<uint32_t*>(shared_cpu);
  bool uploaded = false;
  for (uint32_t slot = 0; slot < DESCRIPTOR_SLOT_COUNT; ++slot) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, device + kDeviceFetch + 24 * slot + 4 * w);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words, sizeof(words));
    if (uint32_t(fetch.type) != 2) continue;
    State::FetchMemo& memo = s.fetch_memo[slot];
    if (memo.generation == s.fetch_generation && std::memcmp(memo.words, words, sizeof(words)) == 0) {
      indices[DESCRIPTOR_KIND_SAMPLER * DESCRIPTOR_SLOT_COUNT + slot] = memo.sampler;
      if (memo.kind != UINT32_MAX) indices[memo.kind * DESCRIPTOR_SLOT_COUNT + slot] = memo.index;
      ++s.fetch_memo_hits;
      continue;
    }
    ++s.fetch_memo_misses;
    {
    }
    const uint32_t sampler = s.SamplerFor(fetch);
    indices[DESCRIPTOR_KIND_SAMPLER * DESCRIPTOR_SLOT_COUNT + slot] = sampler;
    uint32_t kind = UINT32_MAX;
    if (uint32_t(fetch.dimension) == 0) {
      kind = DESCRIPTOR_KIND_TEXTURE1D;
      indices[kind * DESCRIPTOR_SLOT_COUNT + slot] = s.Texture1DFor(words, fetch, base, uploaded);
    } else if (uint32_t(fetch.dimension) == 3) {
      kind = DESCRIPTOR_KIND_TEXTURECUBE;
      indices[kind * DESCRIPTOR_SLOT_COUNT + slot] = s.TextureCubeFor(words, fetch, base, uploaded);
    } else if (uint32_t(fetch.dimension) == 2) {
      s.Skip("3d texture");
    } else if (uint32_t(fetch.dimension) == 1) {
      const auto t0 = s.timing ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
      kind = DESCRIPTOR_KIND_TEXTURE2D;
      indices[kind * DESCRIPTOR_SLOT_COUNT + slot] = s.Texture2DFor(words, fetch, base, uploaded);
      if (s.timing) {
        s.ms_texture += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      }
    }
    std::memcpy(memo.words, words, sizeof(words));
    memo.kind = kind;
    memo.index = kind != UINT32_MAX ? indices[kind * DESCRIPTOR_SLOT_COUNT + slot] : 0;
    memo.sampler = sampler;
    memo.generation = s.fetch_generation;
  }
  s.Lap(11);
  const uint32_t bools0 = Be32(base, device + kDeviceBools);
  const uint32_t bools4 = Be32(base, device + kDeviceBools + 16);
  const uint32_t booleans = (bools0 & 0xFFFF) | ((bools4 & 0xFFFF) << 16);
  std::memcpy(shared_cpu + gfx::kSharedBooleansOffset, &booleans, 4);
  const float alpha_ref = RegFloat(base, device, kRbAlphaRef);
  std::memcpy(shared_cpu + gfx::kSharedAlphaThresholdOffset, &alpha_ref, 4);
  const uint32_t alpha_func = color_control & 7;
  std::memcpy(shared_cpu + gfx::kSharedAlphaFuncOffset, &alpha_func, 4);
  if (indexed_fetch) {
    uint64_t table = 0;
    if (!s.Upload(gfx::kFetchTableBytes, 16, table)) {
      s.Skip("upload ring full");
      return;
    }
    uint8_t* out = s.upload_cpu + table;
    std::memset(out, 0, gfx::kFetchTableBytes);
    // Host vertex indices start at `first`; the shader adds it back.
    std::memcpy(out, &first, 4);
    for (uint32_t location = 0; location < 16; ++location) {
      if (fetch_count[location] == 0) continue;
      const uint64_t address = s.upload_gpu + fetch_ring[location];
      std::memcpy(out + 16 + 16 * location, &address, 8);
      std::memcpy(out + 16 + 16 * location + 8, &fetch_count[location], 4);
    }
    const uint64_t address = s.upload_gpu + table;
    std::memcpy(shared_cpu + gfx::kSharedFetchTableOffset, &address, 8);
  }
  std::memcpy(s.upload_cpu + shared, shared_cpu, sizeof(shared_cpu));
  if (REXCVAR_GET(native_interpolate)) {
    s.frame_shared.resize((size_t(cpu_index) + 1) * gfx::kSharedConstantsBytes);
    std::memcpy(s.frame_shared.data() + size_t(cpu_index) * gfx::kSharedConstantsBytes, shared_cpu,
                sizeof(shared_cpu));
  }

  s.Lap(3);
  // Host index list, relative to the first copied vertex.
  const bool use_indices = draw.indexed || quads;
  uint32_t index_count = draw.count;
  uint64_t index_ring = 0;
  plume::RenderBufferReference index_ref;
  if (use_indices && indices_entry) {
    // Indexed: the cached list, relative to the lowest index, which `first`
    // already is.
    State::CachedIndices& entry = *indices_entry;
    index_count = uint32_t(entry.host.size());
    const size_t bytes = entry.host.size() * 4;
    if (index_count == 0) {
      s.Skip("index list");
      return;
    }
    if (!entry.buffer && entry.seen >= 2 && bytes >= 4096) {
      entry.buffer =
          s.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes, plume::RenderBufferFlag::INDEX));
      void* mapped = entry.buffer->map();
      std::memcpy(mapped, entry.host.data(), bytes);
      entry.buffer->unmap();
    }
    if (entry.buffer) {
      index_ref = entry.buffer->at(0);
    } else {
      if (!s.Upload(bytes, 16, index_ring)) {
        s.Skip("index list");
        return;
      }
      std::memcpy(s.upload_cpu + index_ring, entry.host.data(), bytes);
      index_ref = s.upload->at(index_ring);
    }
  } else if (use_indices) {
    // Auto-indexed quad list: generated.
    auto host = [&](uint32_t i) -> uint32_t { return i; };
    index_count = quads ? draw.count / 4 * 6 : draw.count;
    if (index_count == 0 || !s.Upload(size_t(index_count) * 4, 16, index_ring)) {
      s.Skip("index list");
      return;
    }
    auto* out = reinterpret_cast<uint32_t*>(s.upload_cpu + index_ring);
    if (quads) {
      for (uint32_t q = 0; q + 3 < draw.count; q += 4) {
        const uint32_t a = host(q), b = host(q + 1), c = host(q + 2), d = host(q + 3);
        out[0] = a;
        out[1] = b;
        out[2] = c;
        out[3] = a;
        out[4] = c;
        out[5] = d;
        out += 6;
      }
    } else {
      for (uint32_t i = 0; i < draw.count; ++i) out[i] = host(i);
    }
    index_ref = s.upload->at(index_ring);
  }

  s.Lap(12);
  // Viewport from the D3D registers, scaled to the host target.
  const float sx = s.scale_x;
  const float sy = s.scale_y;
  const float xscale = std::fabs(RegFloat(base, device, kPaClVportXScale));
  const float yscale = std::fabs(RegFloat(base, device, kPaClVportYScale));
  const float xoffset = RegFloat(base, device, kPaClVportXOffset);
  const float yoffset = RegFloat(base, device, kPaClVportYOffset);
  const float zscale = RegFloat(base, device, kPaClVportZScale);
  const float zoffset = RegFloat(base, device, kPaClVportZOffset);
  plume::RenderViewport viewport((xoffset - xscale) * sx, (yoffset - yscale) * sy, 2.0f * xscale * sx,
                                 2.0f * yscale * sy, std::clamp(zoffset, 0.0f, 1.0f),
                                 std::clamp(zoffset + zscale, 0.0f, 1.0f));
  s.SelectTarget(base, device, uint32_t(std::ceil(yoffset + yscale)));
  if (!(viewport.width > 0.0f) || !(viewport.height > 0.0f)) {
    viewport = plume::RenderViewport(0.0f, 0.0f, float(s.target_width), float(s.target_height));
  }
  const std::array<uint32_t, 12> viewport_words = {
      Reg(base, device, 0x2206), std::bit_cast<uint32_t>(xoffset), std::bit_cast<uint32_t>(yoffset),
      std::bit_cast<uint32_t>(xscale), std::bit_cast<uint32_t>(yscale), std::bit_cast<uint32_t>(zoffset),
      std::bit_cast<uint32_t>(zscale), Reg(base, device, 0x2000), Reg(base, device, 0x2080),
      Reg(base, device, 0x200E), Reg(base, device, 0x200F), 1};
  if (viewport_words != s.viewport_words || s.viewport_line.empty()) {
    s.viewport_words = viewport_words;
    s.viewport_line = fmt::format("vte={:03X} vp={:.0f},{:.0f},{:.0f}x{:.0f} z={:.2f}+{:.2f} surf={:08X} wo={:08X} sc={:08X}-{:08X}",
                                  Reg(base, device, 0x2206), xoffset - xscale, yoffset - yscale, 2 * xscale,
                                  2 * yscale, zoffset, zscale, Reg(base, device, 0x2000), Reg(base, device, 0x2080),
                                  Reg(base, device, 0x200E), Reg(base, device, 0x200F));
    s.last_viewport = fmt::format("{:.1f},{:.1f} {:.1f}x{:.1f}", xoffset - xscale, yoffset - yscale,
                                  2.0f * xscale, 2.0f * yscale);
  }
  ++s.viewports[s.viewport_line];

  s.Lap(4);
  State::DrawOp op;
  op.target = s.current;
  op.target_id = s.current->id;
  op.pipeline = pipeline;
  op.vs_constants = vs_constants;
  op.ps_constants = ps_constants;
  op.shared = shared;
  op.viewport = viewport;
  op.views = views;
  op.slots = slots;
  op.slot_count = slot_count;
  op.indexed = use_indices;
  op.index_ring = index_ring;
  op.index_ref = index_ref;
  op.index_count = index_count;
  op.vertex_count = draw.count;
  op.uploaded = uploaded;
  op.cpu = cpu_index;
  if (REXCVAR_GET(native_interpolate)) {
    uint32_t texture_indices[DESCRIPTOR_SLOT_COUNT];
    for (uint32_t slot = 0; slot < DESCRIPTOR_SLOT_COUNT; ++slot) {
      texture_indices[slot] = indices[DESCRIPTOR_KIND_TEXTURE2D * DESCRIPTOR_SLOT_COUNT + slot];
    }
    // Shaders and count only. The game writes dynamic geometry (UI quads,
    // cables) to a new vertex and index buffer each frame, and in the
    // village some draws' texture indices change every frame; such draws
    // must still pair (98 % of the village's draws pair this way, 72 % with
    // indices and textures). Same-looking draws pair in order; a pair whose
    // transform jumps is drawn unblended.
    op.key = interp::DrawKey(vs_key, ps_key, nullptr, 0, 0, 0, draw.count, nullptr, 0);
    op.key_strict = interp::DrawKey(vs_key, ps_key, nullptr, 0, draw.indexed ? index_address : 0, draw.start,
                                    draw.count, texture_indices, DESCRIPTOR_SLOT_COUNT);
    op.key_no_textures = interp::DrawKey(vs_key, ps_key, nullptr, 0, draw.indexed ? index_address : 0,
                                         draw.start, draw.count, nullptr, 0);
    op.key_shaders = interp::DrawKey(vs_key, ps_key, nullptr, 0, 0, 0, draw.count, nullptr, 0);
    op.count = draw.count;
    if (vs_cache_entry != s.entries.end()) {
      op.motion = (vs_cache_entry->second->specConstantsMask >> SPEC_CONSTANT_MOTION_SHIFT) &
                  ((1u << SPEC_CONSTANT_MOTION_BITS) - 1);
    }
    if (op.motion & interp::kWorld) {
      // An object's own world matrix names it: copies of one mesh (trees,
      // fences) pair with themselves even when the camera changes which of
      // them are drawn. A moved object misses here and pairs by order.
      op.key_strict = interp::MixFloats(op.key_no_textures, s.VsCpu(op) + 12 * 4, 12);
    }
  }
  s.ExecuteDraw(op);
  s.Record(op);
  s.Lap(5);
  if (s.Dumping()) {
    std::string line = fmt::format("draw {} indexed={} prim={} start={} count={} base={} first={} verts={} "
                                   "vs={:016X} ps={:016X} depth={:08X} blend={:08X} mask={:X} vp={} cull={:08X} "
                                   "clip={:08X} rt={:08X}/{:08X}/{:08X} cc={:08X} aref={:g}",
                                   s.drawn, draw.indexed, draw.prim, draw.start, draw.count, draw.base_vertex,
                                   first, vertex_count, vs_key, ps_key, depth_control, blend_control, color_mask,
                                   s.last_viewport, Reg(base, device, kPaSuScModeCntl), Reg(base, device, 0x2204),
                                   Reg(base, device, 0x2000), Reg(base, device, 0x2001), Reg(base, device, 0x2002),
                                   color_control, alpha_ref);
    for (uint32_t i = 0; i < element_count; ++i) {
      line += fmt::format(" | e{} s{} +{} usage{}.{} type{:08X}", i, elems[i].stream, elems[i].offset,
                          elems[i].usage, elems[i].usage_index, elems[i].format_word);
    }
    if (REXCVAR_GET(native_dump_mesh) > 0 && uint32_t(REXCVAR_GET(native_dump_mesh)) == vertex_count) {
      const std::string path = fmt::format("logs/mesh-{}-{}.csv", s.drawn, vertex_count);
      if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (uint32_t v = 0; v < vertex_count; ++v) {
          std::string row = fmt::format("{}", first + v);
          for (uint32_t i = 0; i < element_count; ++i) {
            const uint32_t fetch_at = device + kDeviceFetch + 8 * (95 - elems[i].stream);
            xenos::xe_gpu_vertex_fetch_t vf;
            vf.dword_0 = Be32(base, fetch_at);
            vf.dword_1 = Be32(base, fetch_at + 4);
            const uint32_t stride = uint32_t(*At(base, device + kDeviceStrides + elems[i].stream)) * 4;
            uint8_t raw[16] = {};
            std::memcpy(raw, At(base, PhysicalView(vf.address * 4 + (first + v) * stride + elems[i].offset)),
                        elems[i].in_size);
            SwapDwords(raw, sizeof(raw), uint32_t(vf.endian));
            if (uint32_t(vf.endian) == 2 && SixteenBitComponents(elems[i].source.format)) {
              for (uint32_t b = 0; b + 3 < sizeof(raw); b += 4) {
                std::swap(raw[b], raw[b + 2]);
                std::swap(raw[b + 1], raw[b + 3]);
              }
            }
            float value[4];
            DecodeElement(elems[i].source, raw, value);
            ApplySwizzle(elems[i].source.swizzle, value);
            row += fmt::format(",u{}.{},{:g},{:g},{:g},{:g}", elems[i].usage, elems[i].usage_index, value[0],
                               value[1], value[2], value[3]);
          }
          std::fputs((row + "\n").c_str(), f);
        }
        std::fclose(f);
      }
    }
    // Decoded positions of the first vertices, and every element of them for
    // the shaders native_dump_constants names, as the vertex shader gets them.
    const bool all_elements = ShaderListed(REXCVAR_GET(native_dump_constants), vs_key, ps_key);
    for (uint32_t i = 0; i < element_count; ++i) {
      if (!all_elements && (elems[i].usage != 0 || elems[i].usage_index != 0)) continue;
      const uint32_t fetch_at = device + kDeviceFetch + 8 * (95 - elems[i].stream);
      xenos::xe_gpu_vertex_fetch_t vf;
      vf.dword_0 = Be32(base, fetch_at);
      vf.dword_1 = Be32(base, fetch_at + 4);
      const uint32_t stride = uint32_t(*At(base, device + kDeviceStrides + elems[i].stream)) * 4;
      for (uint32_t v = 0; v < std::min<uint32_t>(vertex_count, 6); ++v) {
        uint8_t raw[16] = {};
        std::memcpy(raw, At(base, PhysicalView(vf.address * 4 + (first + v) * stride + elems[i].offset)),
                    elems[i].in_size);
        SwapDwords(raw, sizeof(raw), uint32_t(vf.endian));
        // The same 16-bit component order fix as ConvertElement.
        if (uint32_t(vf.endian) == 2 && SixteenBitComponents(elems[i].source.format)) {
          for (uint32_t b = 0; b + 3 < sizeof(raw); b += 4) {
            std::swap(raw[b], raw[b + 2]);
            std::swap(raw[b + 1], raw[b + 3]);
          }
        }
        float pos[4];
        DecodeElement(elems[i].source, raw, pos);
        if (all_elements) {
          ApplySwizzle(elems[i].source.swizzle, pos);
          line += fmt::format(" | v{}.u{}.{} ({:.3f},{:.3f},{:.3f},{:.3f})", first + v, elems[i].usage,
                              elems[i].usage_index, pos[0], pos[1], pos[2], pos[3]);
        } else {
          line += fmt::format(" | v{} ({:.2f},{:.2f},{:.2f},{:.2f})", first + v, pos[0], pos[1], pos[2], pos[3]);
        }
      }
    }
    const float* c = reinterpret_cast<const float*>(s.upload_cpu + vs_constants);
    for (uint32_t r = 0; r < 8; ++r) {
      line += fmt::format(" | c{} ({:g},{:g},{:g},{:g})", r, c[4 * r], c[4 * r + 1], c[4 * r + 2],
                          c[4 * r + 3]);
    }
    if (ShaderListed(REXCVAR_GET(native_dump_constants), vs_key, ps_key)) {
      const float* ps_c = reinterpret_cast<const float*>(s.upload_cpu + ps_constants);
      for (uint32_t r = 8; r < gfx::kVertexShaderConstantsBytes / 16; ++r) {
        if (c[4 * r] == 0 && c[4 * r + 1] == 0 && c[4 * r + 2] == 0 && c[4 * r + 3] == 0) continue;
        line += fmt::format(" | c{} ({:g},{:g},{:g},{:g})", r, c[4 * r], c[4 * r + 1], c[4 * r + 2], c[4 * r + 3]);
      }
      for (uint32_t r = 0; r < gfx::kPixelShaderConstantsBytes / 16; ++r) {
        const float* v = ps_c + 4 * r;
        if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0) continue;
        line += fmt::format(" | p{} ({:g},{:g},{:g},{:g})", r, v[0], v[1], v[2], v[3]);
      }
    }
    for (uint32_t slot = 0; slot < DESCRIPTOR_SLOT_COUNT; ++slot) {
      if (const uint32_t index1d = indices[DESCRIPTOR_KIND_TEXTURE1D * DESCRIPTOR_SLOT_COUNT + slot]) {
        uint32_t fw[6];
        for (uint32_t w = 0; w < 6; ++w) fw[w] = Be32(base, device + kDeviceFetch + 24 * slot + 4 * w);
        xenos::xe_gpu_texture_fetch_t ff;
        std::memcpy(&ff, fw, sizeof(fw));
        line += fmt::format(" | t{}=1d#{}@{:08X}/f{}/w{} pitch{} [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", slot,
                            index1d, ff.base_address << 12, uint32_t(ff.format), ff.size_1d.width + 1,
                            ff.pitch * 32, fw[0], fw[1], fw[2], fw[3], fw[4], fw[5]);
      }
      const uint32_t index = indices[DESCRIPTOR_KIND_TEXTURE2D * DESCRIPTOR_SLOT_COUNT + slot];
      if (index == 0) continue;
      bool resolved = false;
      for (const auto& [k, t] : s.textures) resolved |= t.index == index && t.resolved;
      uint32_t fw[6];
      for (uint32_t w = 0; w < 6; ++w) fw[w] = Be32(base, device + kDeviceFetch + 24 * slot + 4 * w);
      xenos::xe_gpu_texture_fetch_t ff;
      std::memcpy(&ff, fw, sizeof(fw));
      line += fmt::format(" | t{}=#{}{}@{:08X}/f{}/{}x{} [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", slot, index,
                          resolved ? "r" : "", ff.base_address << 12, uint32_t(ff.format), ff.size_2d.width + 1,
                          ff.size_2d.height + 1, fw[0], fw[1], fw[2], fw[3], fw[4], fw[5]);
    }
    s.dump += line + "\n";
  }
  s.Lap(6);
  ++s.drawn;
}

void Renderer::Clear(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, float z,
                     uint32_t stencil, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  ScopedMs timer(s.ms_clear);
  s.BeginFrame();
  s.UploadDefaults();
  s.SelectTarget(base, device, std::max<uint32_t>(State::RectBottom(base, rect), 1));
  if (REXCVAR_GET(native_debug_target)) return;
  plume::RenderRect host_rect(0, 0, int32_t(s.target_width), int32_t(s.target_height));
  if (rect != 0) {
    host_rect = s.HostRect(base, rect);
    // In a tiled pass the clear is replayed for every band with that band's
    // offset, but the title passes the first band's rect. A full-width rect
    // from the top therefore stands for the whole surface.
    if (host_rect.left == 0 && host_rect.top == 0 && host_rect.right >= int32_t(s.target_width)) {
      host_rect.bottom = int32_t(s.target_height);
    }
  }
  State::ClearOp op;
  op.target = s.current;
  op.target_id = s.current->id;
  op.flags = flags;
  op.rect = host_rect;
  op.color = plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f);
  if ((flags & 0xF) && color != 0) {
    op.color = plume::RenderColor(BeFloat(base, color), BeFloat(base, color + 4), BeFloat(base, color + 8),
                                  BeFloat(base, color + 12));
  }
  op.z = z;
  op.stencil = stencil;
  s.ExecuteClear(op);
  s.Record(op);
  if (s.Dumping()) {
    s.dump += fmt::format("clear flags={:X} rect={:08X} z={} rt={:08X}/{:08X}/{:08X}\n", flags, rect, z,
                          Reg(base, device, 0x2000), Reg(base, device, 0x2001), Reg(base, device, 0x2002));
  }
  ++s.clears;
}

void Renderer::Resolve(uint32_t device, uint32_t flags, uint32_t rect, uint32_t texture, uint32_t point,
                       uint32_t clear_color, float z, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  ScopedMs timer(s.ms_resolve);
  s.BeginFrame();
  s.UploadDefaults();
  s.SelectTarget(base, device, std::max<uint32_t>(State::RectBottom(base, rect), 1));
  const uint32_t source = flags & 7;
  State::ResolveOp op;
  op.target = s.current;
  op.target_id = s.current->id;
  if (texture != 0 && source >= 4) {
    // The EDRAM depth into a texture the game samples later (the cutscene
    // depth of field). The host copy holds the depth value as R32_FLOAT.
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, texture + 28 + 4 * w);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words, sizeof(words));
    const uint32_t guest_width = fetch.size_2d.width + 1;
    const uint32_t guest_height = fetch.size_2d.height + 1;
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("native: first depth resolve: flags={:X} format={} {}x{} swizzle={:03o} depth_info={:08X} samples={}",
                  flags, uint32_t(fetch.format), guest_width, guest_height, uint32_t(fetch.swizzle),
                  Reg(base, device, 0x2002), s.current_samples);
    }
    const uint32_t width = std::max(1u, uint32_t(std::lround(guest_width * s.scale_x)));
    const uint32_t height = std::max(1u, uint32_t(std::lround(guest_height * s.scale_y)));
    State::Image* depth_image = s.current ? s.current->depth : nullptr;
    const uint64_t key = ImageKey(words);
    State::Texture& entry = s.textures[key];
    bool ok = depth_image && s.depth && s.depth_resolve_pipeline;
    if (ok && (!entry.texture || !entry.resolved || entry.format != kDepthCopyFormat || entry.width != width ||
               entry.height != height)) {
      s.Retire(entry.framebuffer);
      s.Retire(entry.view);
      s.Retire(entry.texture);
      entry.texture = s.device->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, kDepthCopyFormat));
      if (REXCVAR_GET(native_clear_new_textures)) {
        s.ClearNewTexture(entry.texture.get(), kDepthCopyFormat, width, height);
      }
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(kDepthCopyFormat);
      view.componentMapping = ComponentMapping(DepthSwizzle(fetch.swizzle));
      entry.view = entry.texture->createTextureView(view);
      const plume::RenderTexture* attachment = entry.texture.get();
      plume::RenderFramebufferDesc framebuffer;
      framebuffer.colorAttachments = &attachment;
      framebuffer.colorAttachmentsCount = 1;
      entry.framebuffer = s.device->createFramebuffer(framebuffer);
      if (entry.index == 0) {
        if (s.next_texture2d >= kTexture2DCount) return;
        entry.index = s.next_texture2d++;
      }
      entry.resolved = true;
      entry.format = kDepthCopyFormat;
      entry.width = width;
      entry.height = height;
    }
    if (ok && (!depth_image->depth_view || depth_image->depth_view_of != s.depth)) {
      s.Retire(depth_image->depth_view);
      depth_image->depth_view = s.depth->createTextureView(plume::RenderTextureViewDesc::Texture2D(kDepthFormat));
      depth_image->depth_view_of = s.depth;
      if (depth_image->depth_index == 0) {
        if (s.next_texture2d >= kTexture2DCount) return;
        depth_image->depth_index = s.next_texture2d++;
      }
    }
    plume::RenderRect src(0, 0, int32_t(s.target_width), int32_t(s.target_height));
    if (rect != 0) src = s.HostRect(base, rect);
    int32_t dst_x = 0, dst_y = 0;
    if (point != 0) {
      dst_x = int32_t(std::lround(int32_t(Be32(base, point)) * s.scale_x));
      dst_y = int32_t(std::lround(int32_t(Be32(base, point + 4)) * s.scale_y));
    }
    const int32_t copy_w = std::min<int32_t>(src.right - src.left, int32_t(width) - dst_x);
    const int32_t copy_h = std::min<int32_t>(src.bottom - src.top, int32_t(height) - dst_y);
    uint64_t constants = 0;
    ok = ok && copy_w > 0 && copy_h > 0 && s.Upload(16, 16, constants);
    if (ok) {
      const int32_t header[4] = {int32_t(depth_image->depth_index), s.current_samples > 1 ? 1 : 0,
                                 src.left - dst_x, src.top - dst_y};
      std::memcpy(s.upload_cpu + constants, header, sizeof(header));
      op.kind = State::ResolveOp::kDepth;
      op.key = key;
      op.dst_x = dst_x;
      op.dst_y = dst_y;
      op.copy_w = copy_w;
      op.copy_h = copy_h;
      op.constants = constants;
      const uint32_t physical = TexturePhysical(fetch.base_address);
      s.resolved_at[physical] = {key, State::GuestPageHash(base, physical)};
      ++s.fetch_generation;
      ++s.depth_resolves;
    } else {
      s.Skip("depth resolve");
    }
    if (s.Dumping()) {
      s.dump += fmt::format("resolve depth flags={:X} texture={:08X} @{:08X}/f{} {}x{} key={:016X} index=#{} "
                            "src={},{}-{},{} {} rt={:08X}/{:08X}/{:08X}\n",
                            flags, texture, fetch.base_address << 12, uint32_t(fetch.format), guest_width,
                            guest_height, key, entry.index, src.left, src.top, src.right, src.bottom,
                            ok ? "done" : "skipped", Reg(base, device, 0x2000), Reg(base, device, 0x2001),
                            Reg(base, device, 0x2002));
    }
  }
  if (texture != 0 && source < 4) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, texture + 28 + 4 * w);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words, sizeof(words));
    const uint32_t guest_width = fetch.size_2d.width + 1;
    const uint32_t guest_height = fetch.size_2d.height + 1;
    const uint32_t width = std::max(1u, uint32_t(std::lround(guest_width * s.scale_x)));
    const uint32_t height = std::max(1u, uint32_t(std::lround(guest_height * s.scale_y)));
    const uint64_t key = ImageKey(words);
    const uint32_t physical = TexturePhysical(fetch.base_address);
    s.resolved_at[physical] = {key, State::GuestPageHash(base, physical)};
    ++s.fetch_generation;
    State::Texture& entry = s.textures[key];
    if (!entry.texture || !entry.resolved || entry.format != kColorFormat || entry.width != width ||
        entry.height != height) {
      s.Retire(entry.framebuffer);
      s.Retire(entry.view);
      s.Retire(entry.texture);
      entry.texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, kColorFormat));
      if (REXCVAR_GET(native_clear_new_textures)) s.ClearNewTexture(entry.texture.get(), kColorFormat, width, height);
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(kColorFormat);
      entry.view_swizzle = ResolvedSwizzle(fetch.swizzle, fetch.format);
      view.componentMapping = ComponentMapping(entry.view_swizzle);
      entry.view = entry.texture->createTextureView(view);
      if (entry.index == 0) {
        if (s.next_texture2d >= kTexture2DCount) return;
        entry.index = s.next_texture2d++;
      }
      entry.resolved = true;
      entry.format = kColorFormat;
      entry.width = width;
      entry.height = height;
    }
    plume::RenderRect src(0, 0, int32_t(s.target_width), int32_t(s.target_height));
    if (rect != 0) src = s.HostRect(base, rect);
    int32_t dst_x = 0, dst_y = 0;
    if (point != 0) {
      dst_x = int32_t(std::lround(int32_t(Be32(base, point)) * s.scale_x));
      dst_y = int32_t(std::lround(int32_t(Be32(base, point + 4)) * s.scale_y));
    }
    src.right = std::min<int32_t>(src.right, src.left + int32_t(width) - dst_x);
    src.bottom = std::min<int32_t>(src.bottom, src.top + int32_t(height) - dst_y);
    if (src.right > src.left && src.bottom > src.top) {
      op.kind = State::ResolveOp::kColor;
      op.key = key;
      op.src = src;
      op.dst_x = dst_x;
      op.dst_y = dst_y;
    }
    if (s.Dumping()) {
      s.dump += fmt::format("resolve flags={:X} texture={:08X} @{:08X}/f{} {}x{} key={:016X} index=#{} "
                            "src={},{}-{},{} rt={:08X}/{:08X}/{:08X}\n",
                            flags, texture, fetch.base_address << 12, uint32_t(fetch.format), guest_width,
                            guest_height, key, entry.index, src.left, src.top, src.right, src.bottom,
                            Reg(base, device, 0x2000), Reg(base, device, 0x2001), Reg(base, device, 0x2002));
    }
    ++s.resolves;
  }
  // Xenos clears the EDRAM the resolve read. A tiled pass resolves and clears
  // one band at a time; the host draws the frame once, so only that band may
  // be cleared or the later bands would copy an empty target.
  plume::RenderRect region(0, 0, int32_t(s.target_width), int32_t(s.target_height));
  if (rect != 0) region = s.HostRect(base, rect);
  // A tiled pass resolves each band with a clear, but on the GPU that clear
  // runs per tile inside the replayed command buffer, before the next band is
  // drawn. The title then copies the whole target again (flags 0) and
  // composites that copy, so the host, which draws the frame once, must not
  // clear here.
  if (rect != 0 && (region.right - region.left < int32_t(s.target_width) ||
                    region.bottom - region.top < int32_t(s.target_height))) {
    flags &= ~0x300u;
  }
  if (REXCVAR_GET(native_debug_target)) flags &= ~0x300u;
  op.clear_flags = flags & 0x300u;
  op.clear_region = region;
  op.clear_color = plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f);
  if ((flags & 0x100) && clear_color != 0) {
    op.clear_color = plume::RenderColor(BeFloat(base, clear_color), BeFloat(base, clear_color + 4),
                                        BeFloat(base, clear_color + 8), BeFloat(base, clear_color + 12));
  }
  op.z = z;
  s.ExecuteResolve(op);
  s.Record(op);
}

void Renderer::Present(uint32_t frontbuffer, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  ScopedMs timer(s.ms_present);
  s.BeginFrame();
  s.UploadDefaults();
  // Present records directly (and replays, reads back, submits): the
  // recording thread's queue is drained first.
  {
    const auto t_flush = std::chrono::steady_clock::now();
    s.Flush();
    s.ms_flush += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_flush).count();
  }
  s.deferring = false;
  ++s.frames;
#if defined(__ANDROID__)
#if defined(__aarch64__)
  if (REXCVAR_GET(android_profile) && !s.profiling) {
    s.profiling = true;
    profiler::Start();
  }
#endif
  if (const int cpu = sched_getcpu(); cpu >= 0) ++s.frames_on_cpu[std::min(cpu, 15)];
  if (REXCVAR_GET(android_big_cores)) {
    const auto now = std::chrono::steady_clock::now();
    if (!s.game_thread_set) {
      s.game_thread_set = true;
      PinToBigCores("game");
      std::vector<int32_t> threads{int32_t(gettid())};
      if (const int32_t tid = s.recorder_tid.load(); tid != 0) threads.push_back(tid);
      s.hint.Start(threads, 16'666'667);
    } else {
      // Present to present: what a 60 Hz frame took, waits included, so a
      // frame over budget asks for more clock.
      s.hint.Report(std::chrono::duration_cast<std::chrono::nanoseconds>(now - s.last_present).count());
    }
    s.last_present = now;
  }
#endif
  // Swap shows a texture: the last resolve into it when there was one, else
  // whatever the target holds.
  if (s.current) s.ResolveMsaa(*s.current->color);
  plume::RenderTexture* source = s.current ? s.current->color->texture.get() : s.color;
  plume::RenderTextureLayout source_layout = plume::RenderTextureLayout::COLOR_WRITE;
  if (source && (s.target_width != s.width || s.target_height != s.height)) source = nullptr;
  if (frontbuffer != 0 && !REXCVAR_GET(native_debug_target)) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, frontbuffer + 28 + 4 * w);
    auto found = s.textures.find(ImageKey(words));
    if (found != s.textures.end() && found->second.resolved &&
        (s.present_pipeline || (found->second.width == s.width && found->second.height == s.height))) {
      source = found->second.texture.get();
      source_layout = plume::RenderTextureLayout::SHADER_READ;
    }
  }
  const bool resize = s.swap_chain->needsResize();
  bool shown = false;
  if (REXCVAR_GET(native_interpolate) && s.flip_interval.load(std::memory_order_relaxed) == 2 && !resize &&
      !s.swap_chain->isEmpty() && source && source_layout == plume::RenderTextureLayout::SHADER_READ) {
    for (auto& [key, texture] : s.textures) {
      if (texture.texture.get() == source) {
        s.NoteFrontResolve(key);
        shown = s.PresentInterpolated(texture);
        break;
      }
    }
  }
  uint32_t image = 0;
  const bool acquired = !shown && !resize && !s.swap_chain->isEmpty() &&
                        s.swap_chain->acquireTexture(s.acquire.get(), &image);
  plume::RenderTexture* target = acquired ? s.swap_chain->getTexture(image) : nullptr;
  uint32_t source_index = 0;
  if (source && source_layout == plume::RenderTextureLayout::SHADER_READ) {
    for (const auto& [key, texture] : s.textures) {
      if (texture.texture.get() == source) source_index = texture.index;
    }
  }
  uint64_t present_constants = 0;
  const bool gamma = target && source_index != 0 && s.present_pipeline &&
                     s.Upload(16 + 256 * 4, 256, present_constants);
  if (target && !source) {
    // Nothing the size of the window to show yet.
    s.commands->barriers(plume::RenderBarrierStage::COPY,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
  } else if (gamma) {
    s.DrawPresentPass(image, source_index, present_constants);
  } else if (target) {
    plume::RenderTextureBarrier to_copy[] = {
        plume::RenderTextureBarrier(source, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COPY_DEST)};
    s.commands->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
    s.commands->copyTexture(target, source);
    plume::RenderTextureBarrier after[] = {
        plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT),
        plume::RenderTextureBarrier(source, source_layout)};
    s.commands->barriers(plume::RenderBarrierStage::NONE, after, 2);
  }
  if (!shown && s.Dumping() && source && source_layout == plume::RenderTextureLayout::SHADER_READ) {
    for (const auto& [key, texture] : s.textures) {
      if (texture.texture.get() != source || texture.format != kColorFormat) continue;
      s.debug_width = texture.width;
      s.debug_height = texture.height;
      s.debug_readback = s.device->createBuffer(
          plume::RenderBufferDesc::ReadbackBuffer(uint64_t(texture.width) * texture.height * 4));
      s.commands->barriers(plume::RenderBarrierStage::COPY,
                           plume::RenderTextureBarrier(source, plume::RenderTextureLayout::COPY_SOURCE));
      s.commands->copyTextureRegion(
          plume::RenderTextureCopyLocation::PlacedFootprint(s.debug_readback.get(), kColorFormat, texture.width,
                                                            texture.height, 1, texture.width, 0),
          plume::RenderTextureCopyLocation::Subresource(source));
      s.commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                           plume::RenderTextureBarrier(source, plume::RenderTextureLayout::SHADER_READ));
      s.frame_image_pending = true;
      break;
    }
  }
  if (!shown) {
    s.journal_last = s.journal.size();
    s.commands->end();
    s.recording = false;
  }
  const plume::RenderCommandList* list = s.commands.get();
  if (shown) {
    // Submitted and presented by PresentInterpolated.
  } else if (target) {
    plume::RenderCommandSemaphore* wait = s.acquire.get();
    plume::RenderCommandSemaphore* signal = s.release[image].get();
    s.queue->executeCommandLists(&list, 1, &wait, 1, &signal, 1, s.fence.get());
    s.swap_chain->present(image, &signal, 1);
  } else {
    s.queue->executeCommandLists(&list, 1, nullptr, 0, nullptr, 0, s.fence.get());
  }
  s.fence_pending[s.frame_slot] = true;
  const auto t_submit = std::chrono::steady_clock::now();
  // A frame whose results are read back here (dumps, interpolation debug
  // pictures) is waited for whatever the frames in flight. An interpolated
  // frame keeps what it blends with in CPU copies, so it overlaps too.
  const bool overlap = s.frames_in_flight == 2 && !s.Dumping() && !s.debug_pending;
  if (overlap) {
    // Record the next frame in the other slot once its last frame is done;
    // this one keeps drawing meanwhile.
    std::swap(s.commands, s.commands_other);
    s.ForgetBound();
    std::swap(s.fence, s.fence_other);
    std::swap(s.acquire, s.acquire_other);
    s.frame_slot ^= 1;
    if (s.fence_pending[s.frame_slot]) {
      s.queue->waitForCommandFence(s.fence.get());
      s.fence_pending[s.frame_slot] = false;
    }
    s.DestroyRetired(s.frame_slot);
  } else {
    s.queue->waitForCommandFence(s.fence.get());
    s.fence_pending[s.frame_slot] = false;
    if (s.frames_in_flight == 2 && s.fence_pending[s.frame_slot ^ 1]) {
      s.queue->waitForCommandFence(s.fence_other.get());
      s.fence_pending[s.frame_slot ^ 1] = false;
    }
  }
  if (!overlap) {
    s.WaitSecond(0);
    s.WaitSecond(1);
  }
  if (REXCVAR_GET(native_interpolate)) s.SnapshotFrame();
  if (s.debug_pending) {
    s.debug_pending = false;
    const uint32_t w = s.debug_width, h = s.debug_height;
    const size_t bytes = size_t(w) * h * 4;
    if (const auto* data = static_cast<const uint8_t*>(s.debug_readback->map())) {
      const int32_t n = ++s.debug_dumped;
      std::vector<uint8_t> half(size_t(w / 2) * (h / 2) * 4);
      for (int i = 0; i < 2; ++i) {
        // Half size: every second pixel of every second row.
        for (uint32_t y = 0; y < h / 2; ++y) {
          for (uint32_t x = 0; x < w / 2; ++x) {
            std::memcpy(&half[(size_t(y) * (w / 2) + x) * 4], data + i * bytes + (size_t(y * 2) * w + x * 2) * 4, 4);
          }
        }
        const std::string path = fmt::format("logs/interp-{}-{}.rgba", n, i ? "mid" : "real");
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
          std::fwrite(half.data(), 1, half.size(), f);
          std::fclose(f);
        }
      }
      s.debug_readback->unmap();
      REXLOG_INFO("native: wrote logs/interp-{}-real.rgba and -mid.rgba ({}x{})", n, w / 2, h / 2);
    }
  }
  if (!overlap) {
    s.DestroyRetired(0);
    s.DestroyRetired(1);
  }
  // Streams unused for 120 frames are in no frame in flight and can go.
  if ((s.presents & 63) == 0) {
    s.stream_cache.EraseIf([&](const auto& entry) { return s.presents - entry.last_used > 120; });
    s.index_cache.EraseIf([&](const auto& entry) { return s.presents - entry.last_used > 120; });
    s.watched.EraseIf([&](const auto& entry) { return s.presents - entry.last_used > 600; });
  }
  s.ms_submit +=std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_submit).count();
  if (s.Dumping()) {
    const std::string path = fmt::format("logs/native-frame-{}.txt", s.presents + 1);
    if (s.frame_image_pending) {
      s.frame_image_pending = false;
      if (const auto* data = static_cast<const uint8_t*>(s.debug_readback->map())) {
        const std::string image = fmt::format("logs/native-frame-{}-{}x{}.bgra", s.presents + 1, s.debug_width,
                                              s.debug_height);
        if (FILE* f = std::fopen(image.c_str(), "wb")) {
          std::fwrite(data, 1, size_t(s.debug_width) * s.debug_height * 4, f);
          std::fclose(f);
        }
        s.debug_readback->unmap();
      }
    }
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fputs(s.dump.c_str(), f);
      std::fprintf(f, "present frontbuffer=%08X target=%ux%u\n", frontbuffer, s.width, s.height);
      for (const auto& [physical, at] : s.resolved_at) {
        auto found = s.textures.find(at.key);
        if (found == s.textures.end()) continue;
        std::fprintf(f, "resolved_at %08X key=%016llX index=#%u %ux%u\n", physical,
                     static_cast<unsigned long long>(at.key), found->second.index, found->second.width,
                     found->second.height);
      }
      std::fclose(f);
    }
    s.dump.clear();
    REXLOG_INFO("native: wrote {}", path);
  }
  ++s.presents;
  g_watch_frame.store(uint32_t(s.presents), std::memory_order_relaxed);
  ++s.fetch_generation;
#if defined(_WIN32)
  // F9 (on its press) dumps the next frame.
  const bool f9 = (GetAsyncKeyState(0x78) & 0x8000) != 0;
  if (f9 && !s.key_down) s.key_dump = s.presents + 1;
  s.key_down = f9;
#endif
  if (resize) {
    // Targets and swap chain images are replaced at once: no frame may be
    // in flight.
    if (s.fence_pending[s.frame_slot]) {
      s.queue->waitForCommandFence(s.fence.get());
      s.fence_pending[s.frame_slot] = false;
    }
    if (s.fence_other && s.fence_pending[s.frame_slot ^ 1]) {
      s.queue->waitForCommandFence(s.fence_other.get());
      s.fence_pending[s.frame_slot ^ 1] = false;
    }
    s.WaitSecond(0);
    s.WaitSecond(1);
    if (s.swap_chain->resize()) {
      while (s.release.size() < s.swap_chain->getTextureCount()) {
        s.release.push_back(s.device->createCommandSemaphore());
      }
      s.swap_framebuffers.clear();
      s.CreateTarget();
    }
  }
}

void Renderer::SetFlipInterval(uint32_t interval) {
  state_->flip_interval.store(interval, std::memory_order_relaxed);
}

void Renderer::SetGammaRamp(const uint32_t table[256]) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  std::memcpy(s.gamma_ramp.data(), table, 256 * 4);
  s.has_gamma_ramp = false;
  for (const uint32_t entry : s.gamma_ramp) s.has_gamma_ramp |= entry != 0;
}

std::string Renderer::TakeStats() {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  std::string out = fmt::format(
      "drawn={} clears={} resolves={} reuploads={} targets={} switches={} pipelines={} shaders={} textures={} samplers={} ms pipeline={:.0f} texture={:.0f} "
      "stream={:.0f} gpu_wait={:.0f} | draw={:.0f} clear={:.0f} resolve={:.0f} present={:.0f} viewport={}",
      s.drawn, s.clears, s.resolves, s.reuploads, s.color_images.size(), s.target_switches, s.pipelines.size(), s.shaders.size(), s.textures.size(), s.samplers.size(),
      s.ms_pipeline, s.ms_texture, s.ms_stream, s.ms_submit, s.ms_draw, s.ms_clear, s.ms_resolve,
      s.ms_present, s.last_viewport);
  s.ms_pipeline = s.ms_texture = s.ms_stream = s.ms_submit = 0;
  s.ms_draw = s.ms_clear = s.ms_resolve = s.ms_present = 0;
  out += fmt::format(" streams hit={} miss={} cached={} msaa_resolves={} depth_resolves={}", s.stream_hits,
                     s.stream_misses, s.stream_cache.size(), s.msaa_resolves, s.depth_resolves);
  s.msaa_resolves = 0;
  s.depth_resolves = 0;
  out += fmt::format(" indexed_fetch={}", s.indexed_fetch_draws);
  s.indexed_fetch_draws = 0;
  if (REXCVAR_GET(native_interpolate)) {
    const double frames = std::max<uint32_t>(s.match_frames, 1);
    out += fmt::format(" journal={} interp={} match={:.2f} match_notex={:.2f} match_shaders={:.2f} bones={} jumped={} snapped={}",
                       s.journal_last, s.interp_frames, s.match_sum / frames, s.match_no_textures_sum / frames,
                       s.match_shaders_sum / frames, s.bones_blended, s.guarded, s.snapped);
    // Snapped matrices by kind (wvp wv itw world eye skin shadow0 shadow1)
    // and the share of draws paired by their strict key (world matrix).
    out += fmt::format(" snapped_kinds={} match_strict={:.2f}", fmt::join(s.snapped_by_kind, "/"),
                       s.match_strict_sum / frames);
    s.guarded = 0;
    s.snapped = 0;
    std::fill(std::begin(s.snapped_by_kind), std::end(s.snapped_by_kind), 0);
    s.match_strict_sum = 0;
    if (!s.unpaired_sample.empty()) out += " | unpaired " + s.unpaired_sample;
    if (!s.front_resolve.empty()) out += " | front resolve " + s.front_resolve;
    s.front_resolve.clear();
    s.bones_blended = 0;
    s.match_sum = s.match_no_textures_sum = s.match_shaders_sum = 0;
    s.match_frames = 0;
    s.unpaired_sample.clear();
    s.interp_frames = 0;
  }
  s.stream_hits = s.stream_misses = 0;
  out += fmt::format(" indices hit={} miss={} cached={}", s.index_hits, s.index_misses, s.index_cache.size());
  s.index_hits = s.index_misses = 0;
  out += fmt::format(" fetch_memo={}/{}", s.fetch_memo_hits, s.fetch_memo_misses);
  if (s.constant_bytes_full != 0) {
    out += fmt::format(" constants={:.0f}%", 100.0 * double(s.constant_bytes) / double(s.constant_bytes_full));
    s.constant_bytes = s.constant_bytes_full = 0;
  }
  {
    std::lock_guard<std::mutex> lock(s.record_mutex);
    out += fmt::format(" frames={} flush={:.0f} recorder={:.0f}", s.frames, s.ms_flush, s.ms_recorder);
    s.ms_recorder = 0;
  }
  s.ms_flush = 0;
  s.frames = 0;
#if defined(__ANDROID__)
  out += " game_cpu";
  for (size_t cpu = 0; cpu < s.frames_on_cpu.size(); ++cpu) {
    if (s.frames_on_cpu[cpu] != 0) out += fmt::format(" {}:{}", cpu, s.frames_on_cpu[cpu]);
  }
  s.frames_on_cpu.fill(0);
#if defined(__aarch64__)
  if (s.profiling) profiler::Report();
#endif
#endif
  s.fetch_memo_hits = s.fetch_memo_misses = 0;
  if (REXCVAR_GET(native_watch_memory)) {
    out += fmt::format(" watch hit={} miss={} paused={} faults={} ranges={} textures={} arms={}", s.watch_hits,
                       s.watch_misses, s.watch_paused, g_watch_faults.exchange(0, std::memory_order_relaxed),
                       s.watched.size(), s.texture_watch_hits, s.watch_arms);
    s.watch_hits = s.watch_misses = s.watch_paused = s.texture_watch_hits = s.watch_arms = 0;
  }
  out += " laps";
  for (const double ms : s.ms_lap) out += fmt::format(" {:.0f}", ms);
  s.ms_lap = {};
  out += " cull";
  for (uint32_t m = 0; m < 8; ++m) {
    if (s.cull_modes[m]) out += fmt::format(" {}:{}", m, s.cull_modes[m]);
  }
  s.cull_modes = {};
  out += " endian(fetch/elem)";
  for (uint32_t m = 0; m < 16; ++m) {
    if (s.stream_endians[m]) out += fmt::format(" {}/{}:{}", m / 4, m % 4, s.stream_endians[m]);
  }
  s.stream_endians = {};
  if (s.outline_dump) out += fmt::format(" outline_dump={}", s.outline_dump);
  for (const auto& [reason, count] : s.skipped) out += fmt::format(" | {}: {}", reason, count);
  if (!s.last_bounds.empty()) out += " | last bounds: " + s.last_bounds;
  std::vector<std::pair<uint64_t, std::string>> top;
  for (const auto& [k, n] : s.viewports) top.emplace_back(n, k);
  std::sort(top.rbegin(), top.rend());
  for (size_t i = 0; i < std::min<size_t>(top.size(), 6); ++i) out += fmt::format(" || {} x{}", top[i].second, top[i].first);
  s.viewports.clear();
  s.last_bounds.clear();
  s.drawn = 0;
  s.clears = 0;
  s.resolves = 0;
  s.reuploads = 0;
  s.target_switches = 0;
  s.skipped.clear();
  return out;
}

}  // namespace broken_bond::render
