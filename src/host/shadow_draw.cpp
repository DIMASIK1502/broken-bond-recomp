#include "host/shadow_draw.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/system/xmemory.h>

#include "generated/default/broken_bond_init.h"
#include "gfx/xenos_shader_common.h"
#include "host/d3d_census.h"
#if defined(BROKEN_BOND_HAS_SHADER_CACHE)
#include "gfx/shader_cache.h"
#endif

REXCVAR_DEFINE_BOOL(shadow_draws, false, "Game",
                    "Record every guest draw from the D3D device shadow and log coverage every 5 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(shadow_dump_frame, 0, "Game",
                     "With shadow_draws, write the records of this present (counted from 1) to "
                     "logs/shadow-frame-<n>.txt; 0 writes none")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace broken_bond::host {
namespace {

// Device fields, docs/D3D_MAP.md.
constexpr uint32_t kDeviceDecl = 11820;
constexpr uint32_t kDeviceIndices = 12436;
constexpr uint32_t kDeviceColorTargets = 12440;
constexpr uint32_t kDevicePixelShader = 12692;
constexpr uint32_t kDeviceVertexShader = 12696;
constexpr uint32_t kDeviceFetch = 1152;
constexpr uint32_t kDeviceSurfaceInfo = 10368;  // RB_SURFACE_INFO
constexpr uint32_t kDeviceColorInfo = 10372;    // RB_COLOR_INFO
constexpr uint32_t kDeviceDepthInfo = 10376;    // RB_DEPTH_INFO

// Render-state register blocks of the shadow: {device offset, dword count}.
constexpr std::pair<uint32_t, uint32_t> kStateBlocks[] = {
    {10368, 16}, {10444, 21}, {10528, 5}, {10548, 12}, {10596, 21}, {10680, 38}, {10832, 8},
};
constexpr uint32_t kStateDwords = 16 + 21 + 5 + 12 + 21 + 38 + 8;

// Shader objects, from the game's loaders 82179200 (vertex) and 82179380
// (pixel): the virtual half of the 0x102A110x container is copied into the
// object, the microcode into physical memory.
constexpr uint32_t kVertexShaderType = 6;
constexpr uint32_t kPixelShaderType = 7;
constexpr uint32_t kVertexContainer = 872;
constexpr uint32_t kVertexMicrocode = 32;
constexpr uint32_t kPixelContainer = 40;
constexpr uint32_t kPixelMicrocode = 24;

// Vertex declaration object: element count at +24, 12-byte elements at +52.
constexpr uint32_t kDeclCount = 24;
constexpr uint32_t kDeclElements = 52;

// Guest pointers go through GuestPtr: on Windows the 0xE0000000 physical heap
// (shader microcode, vertex data) sits 0x1000 further into the host view.
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

bool Enabled() {
  static const bool enabled = REXCVAR_GET(shadow_draws);
  return enabled;
}

struct DrawRecord {
  DrawKind kind;
  uint32_t prim = 0;
  uint32_t base_vertex = 0;
  uint32_t start = 0;
  uint32_t count = 0;
  uint32_t vs = 0;
  uint32_t ps = 0;
  uint64_t vs_key = 0;
  uint64_t ps_key = 0;
  bool vs_cached = false;
  bool ps_cached = false;
  uint32_t decl = 0;
  uint32_t elements = 0;
  uint32_t unmapped_elements = 0;
  uint32_t streams = 0;  // bit per stream a declaration element reads
  uint32_t indices = 0;
  bool index32 = false;
  uint32_t surface_info = 0;
  uint32_t color_info = 0;
  uint32_t depth_info = 0;
  uint32_t color_targets = 0;  // bit per bound color target
  uint32_t textures = 0;       // bit per fetch slot holding a texture constant
  uint64_t state_key = 0;
  std::array<uint32_t, kStateDwords> state{};
};

struct Window {
  uint64_t draws = 0;
  uint64_t vs_cached = 0;
  uint64_t ps_cached = 0;
  uint64_t both_cached = 0;
  uint64_t no_shader = 0;
  uint64_t unmapped_elements = 0;
  uint64_t frames = 0;
  std::unordered_set<uint32_t> vs;
  std::unordered_set<uint32_t> ps;
  std::unordered_set<uint64_t> states;
  std::unordered_set<uint32_t> color_infos;
  std::unordered_set<uint64_t> missing_keys;
};

std::mutex g_mutex;
std::vector<DrawRecord> g_frame;
Window g_window;
uint64_t g_presents = 0;
uint64_t g_title_draws_total = 0;
uint64_t g_window_title_draws = 0;
uint64_t g_window_packets_start = 0;
bool g_window_started = false;

// Guest shader object -> XenosRecomp key, filled when the loader builds it.
std::unordered_map<uint32_t, uint64_t> g_shader_keys;
uint64_t g_keys_at_create = 0;
uint64_t g_keys_late = 0;

const std::unordered_set<uint64_t>& CacheKeys() {
  static const std::unordered_set<uint64_t> keys = [] {
    std::unordered_set<uint64_t> out;
#if defined(BROKEN_BOND_HAS_SHADER_CACHE)
    out.reserve(g_shaderCacheEntryCount);
    for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) out.insert(g_shaderCacheEntries[i].hash);
#endif
    return out;
  }();
  return keys;
}

// XXH3 over the container as it sits in xeshader.sdb: the virtual half the
// object holds, then the microcode. Returns 0 when the object is not a shader.
uint64_t ContainerKey(const uint8_t* base, uint32_t object) {
  if (object == 0) return 0;
  const uint32_t type = Be32(base, object);
  uint32_t container = 0;
  uint32_t microcode = 0;
  if (type == kVertexShaderType) {
    container = object + kVertexContainer;
    microcode = Be32(base, object + kVertexMicrocode);
  } else if (type == kPixelShaderType) {
    container = object + kPixelContainer;
    microcode = Be32(base, object + kPixelMicrocode);
  } else {
    return 0;
  }
  if ((Be32(base, container) & 0xFFFFFF00u) != 0x102A1100u || microcode == 0) return 0;
  const uint32_t virtual_size = Be32(base, container + 4);
  const uint32_t physical_size = Be32(base, container + 8);
  if (virtual_size == 0 || virtual_size > (1u << 20) || physical_size > (1u << 20)) return 0;
  XXH3_state_t state;
  XXH3_64bits_reset(&state);
  XXH3_64bits_update(&state, At(base, container), virtual_size);
  XXH3_64bits_update(&state, At(base, microcode), physical_size);
  return XXH3_64bits_digest(&state);
}

// Writes the first few containers whose key misses the cache, as the bytes
// that were hashed, for comparison with shader_db's extracted objects.
void DumpMissingContainer(const uint8_t* base, uint32_t object, uint64_t key) {
  static std::atomic<int> dumped{0};
  if (dumped.fetch_add(1) >= 4) return;
  const bool vertex = Be32(base, object) == kVertexShaderType;
  const uint32_t container = object + (vertex ? kVertexContainer : kPixelContainer);
  const uint32_t microcode = Be32(base, object + (vertex ? kVertexMicrocode : kPixelMicrocode));
  const uint32_t virtual_size = Be32(base, container + 4);
  const uint32_t physical_size = Be32(base, container + 8);
  const std::string path = fmt::format("logs/shader-miss-{:016X}.bin", key);
  if (FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(At(base, container), 1, virtual_size, f);
    std::fwrite(At(base, microcode), 1, physical_size, f);
    std::fclose(f);
  }
}

void NoteShaderCreated(uint32_t object, const uint8_t* base) {
  const uint64_t key = ContainerKey(base, object);
  if (key == 0) return;
  if (CacheKeys().count(key) == 0) DumpMissingContainer(base, object, key);
  std::lock_guard<std::mutex> lock(g_mutex);
  g_shader_keys[object] = key;
  ++g_keys_at_create;
}

// Caller holds g_mutex. Objects built before the hook ran are keyed here; by
// then D3D may have patched vfetch formats into the microcode, which changes
// the key, so they are counted apart.
uint64_t ShaderKey(const uint8_t* base, uint32_t object) {
  if (object == 0) return 0;
  const auto it = g_shader_keys.find(object);
  if (it != g_shader_keys.end()) return it->second;
  const uint64_t key = ContainerKey(base, object);
  g_shader_keys[object] = key;
  ++g_keys_late;
  return key;
}

bool UsageMapped(uint32_t usage, uint32_t index) {
  for (const auto& location : broken_bond::gfx::kVertexLocations) {
    if (static_cast<uint32_t>(location.usage) == usage && location.usage_index == index) return true;
  }
  return false;
}

void ReadDeclaration(const uint8_t* base, uint32_t decl, DrawRecord& record) {
  if (decl == 0) return;
  const uint32_t count = std::min<uint32_t>(Be32(base, decl + kDeclCount), 32);
  record.elements = count;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t element = decl + kDeclElements + i * 12;
    const uint32_t stream = Be16(base, element);
    const uint32_t usage = *At(base, element + 9);
    const uint32_t usage_index = *At(base, element + 10);
    if (stream < 16) record.streams |= 1u << stream;
    if (!UsageMapped(usage, usage_index)) ++record.unmapped_elements;
  }
}

void WriteDump(const std::vector<DrawRecord>& frame, uint64_t present) {
  const std::string path = fmt::format("logs/shadow-frame-{}.txt", present);
  FILE* f = std::fopen(path.c_str(), "w");
  if (!f) {
    REXLOG_INFO("shadow: cannot write {}", path);
    return;
  }
  for (size_t i = 0; i < frame.size(); ++i) {
    const DrawRecord& r = frame[i];
    std::string line = fmt::format(
        "{} kind={} prim={} base={} start={} count={} vs={:08X}:{:016X}{} ps={:08X}:{:016X}{} "
        "decl={:08X} elements={} unmapped={} streams={:04X} ib={:08X}{} surface={:08X} "
        "color={:08X} depth={:08X} targets={:X} textures={:08X} state={:016X}",
        i, static_cast<int>(r.kind), r.prim, r.base_vertex, r.start, r.count, r.vs, r.vs_key,
        r.vs_cached ? "" : "!", r.ps, r.ps_key, r.ps_cached ? "" : "!", r.decl, r.elements,
        r.unmapped_elements, r.streams, r.indices, r.index32 ? "/32" : "/16", r.surface_info,
        r.color_info, r.depth_info, r.color_targets, r.textures, r.state_key);
    line += " regs=";
    for (uint32_t v : r.state) line += fmt::format("{:08X}", v);
    std::fprintf(f, "%s\n", line.c_str());
  }
  std::fclose(f);
  REXLOG_INFO("shadow: wrote {} draws of present {} to {}", frame.size(), present, path);
}

}  // namespace

void ShadowDraw(DrawKind kind, const PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return;
  const uint32_t device = ctx.r3.u32;
  DrawRecord r;
  r.kind = kind;
  if (kind == DrawKind::kIndexed) {
    r.prim = ctx.r4.u32;
    r.base_vertex = ctx.r5.u32;
    r.start = ctx.r6.u32;
    r.count = ctx.r7.u32;
  } else if (kind == DrawKind::kAuto) {
    r.prim = ctx.r4.u32;
    r.start = ctx.r5.u32;
    r.count = ctx.r6.u32;
  }
  r.vs = Be32(base, device + kDeviceVertexShader);
  r.ps = Be32(base, device + kDevicePixelShader);
  r.decl = Be32(base, device + kDeviceDecl);
  ReadDeclaration(base, r.decl, r);
  r.indices = Be32(base, device + kDeviceIndices);
  if (r.indices != 0) r.index32 = (Be32(base, r.indices) & 0x80000000u) != 0;
  r.surface_info = Be32(base, device + kDeviceSurfaceInfo);
  r.color_info = Be32(base, device + kDeviceColorInfo);
  r.depth_info = Be32(base, device + kDeviceDepthInfo);
  for (uint32_t i = 0; i < 4; ++i) {
    if (Be32(base, device + kDeviceColorTargets + 4 * i) != 0) r.color_targets |= 1u << i;
  }
  for (uint32_t s = 0; s < 32; ++s) {
    // Fetch constant dword 0 bits 0:1: 2 is a texture, 3 a vertex fetch.
    if ((Be32(base, device + kDeviceFetch + 24 * s) & 3u) == 2u) r.textures |= 1u << s;
  }
  size_t n = 0;
  for (const auto& [offset, count] : kStateBlocks) {
    for (uint32_t i = 0; i < count; ++i) r.state[n++] = Be32(base, device + offset + 4 * i);
  }
  r.state_key = XXH3_64bits(r.state.data(), sizeof(r.state));

  const auto& keys = CacheKeys();
  std::lock_guard<std::mutex> lock(g_mutex);
  r.vs_key = ShaderKey(base, r.vs);
  r.ps_key = ShaderKey(base, r.ps);
  r.vs_cached = r.vs_key != 0 && keys.count(r.vs_key) != 0;
  r.ps_cached = r.ps_key != 0 && keys.count(r.ps_key) != 0;

  Window& w = g_window;
  ++w.draws;
  ++g_title_draws_total;
  ++g_window_title_draws;
  if (r.vs == 0 || r.ps == 0) ++w.no_shader;
  if (r.vs_cached) ++w.vs_cached;
  if (r.ps_cached) ++w.ps_cached;
  if (r.vs_cached && r.ps_cached) ++w.both_cached;
  if (!r.vs_cached && r.vs_key) w.missing_keys.insert(r.vs_key);
  if (!r.ps_cached && r.ps_key) w.missing_keys.insert(r.ps_key);
  w.unmapped_elements += r.unmapped_elements;
  w.vs.insert(r.vs);
  w.ps.insert(r.ps);
  w.states.insert(r.state_key);
  w.color_infos.insert(r.color_info);
  g_frame.push_back(r);
}

void ShadowSwap(uint8_t* base) {
  if (!Enabled()) return;
  (void)base;
  std::vector<DrawRecord> frame;
  uint64_t present = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    present = ++g_presents;
    ++g_window.frames;
    frame.swap(g_frame);
  }
  const int dump = REXCVAR_GET(shadow_dump_frame);
  if (dump > 0 && present == static_cast<uint64_t>(dump)) WriteDump(frame, present);
}

void ShadowLog() {
  if (!Enabled()) return;
  Window w;
  uint64_t title_draws = 0;
  uint64_t keys_at_create = 0;
  uint64_t keys_late = 0;
  const uint64_t packets = rex::perf::GetDrawPacketCount();
  uint64_t packets_start = 0;
  bool started = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::swap(w, g_window);
    title_draws = g_window_title_draws;
    g_window_title_draws = 0;
    packets_start = g_window_packets_start;
    started = g_window_started;
    g_window_packets_start = packets;
    g_window_started = true;
    keys_at_create = g_keys_at_create;
    keys_late = g_keys_late;
  }
  if (w.draws == 0) return;
  auto pct = [&](uint64_t n) { return 100.0 * double(n) / double(w.draws); };
  REXLOG_INFO(
      "shadow 5s: frames={} draws={} vs_cached={:.1f}% ps_cached={:.1f}% both={:.1f}% "
      "no_shader={} unmapped_elements={} distinct vs={} ps={} states={} color_infos={} "
      "missing_keys={}",
      w.frames, w.draws, pct(w.vs_cached), pct(w.ps_cached), pct(w.both_cached), w.no_shader,
      w.unmapped_elements, w.vs.size(), w.ps.size(), w.states.size(), w.color_infos.size(),
      w.missing_keys.size());
  if (started) {
    const uint64_t xenos = packets - packets_start;
    REXLOG_INFO("shadow 5s: title draws={} xenos draw packets={} (ratio {:.3f}); shader keys "
                "at create={} late={}",
                title_draws, xenos, title_draws ? double(xenos) / double(title_draws) : 0.0,
                keys_at_create, keys_late);
  }
  int shown = 0;
  for (uint64_t key : w.missing_keys) {
    if (shown++ == 4) break;
    REXLOG_INFO("shadow: key {:016X} is not in the shader cache", key);
  }
}

}  // namespace broken_bond::host

// Shader header fill of the game's loaders. The key is taken after the
// original runs and before any draw patches vfetch into the microcode.
REX_HOOK_RAW(sub_821A5C98) {
  const uint32_t object = ctx.r3.u32;
  const int census = broken_bond::host::CensusEnter(0x821A5C98, ctx, base);
  __imp__sub_821A5C98(ctx, base);
  broken_bond::host::CensusReturn(census, ctx);
  if (broken_bond::host::Enabled()) broken_bond::host::NoteShaderCreated(object, base);
}

REX_HOOK_RAW(sub_821A59B0) {
  const uint32_t object = ctx.r3.u32;
  const int census = broken_bond::host::CensusEnter(0x821A59B0, ctx, base);
  __imp__sub_821A59B0(ctx, base);
  broken_bond::host::CensusReturn(census, ctx);
  if (broken_bond::host::Enabled()) broken_bond::host::NoteShaderCreated(object, base);
}
