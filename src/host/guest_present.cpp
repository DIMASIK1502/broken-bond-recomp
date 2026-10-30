#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/system/xthread.h>
#include <rex/ui/window.h>

#include "generated/default/broken_bond_init.h"
#include "broken_bond_app.h"
#include "gfx/device.h"
#include "gfx/host_draw.h"
#include "host/d3d_census.h"
#include "host/device_repack.h"
#include "host/shadow_draw.h"
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
#include "render/renderer.h"
#endif

REXCVAR_DEFINE_BOOL(dump_device_draw, false, "Game",
                    "Write up to eight textured triangle draws with different pixel shaders");
REXCVAR_DEFINE_STRING(renderer, "xenos", "Game",
                      "xenos presents the game. host draws into a retained Vulkan target and still presents "
                      "through Xenos. native presents through the Plume renderer (with --gpu_plugin=host)");

namespace {

std::atomic<bool> g_stop{false};
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
std::unique_ptr<broken_bond::render::Renderer> g_native;
// The GPU plugin's graphics system; its command processor keeps the gamma
// ramp the game writes through DC_LUT registers.
rex::graphics::GraphicsSystem* g_graphics = nullptr;

// gamma_ramp_256_entry_table() is protected; a derived class may name it, and
// the pointer to member it yields can be called on any command processor.
struct GammaRampAccess : rex::graphics::CommandProcessor {
  static const rex::graphics::reg::DC_LUT_30_COLOR* Table(const rex::graphics::CommandProcessor& processor) {
    return (processor.*&GammaRampAccess::gamma_ramp_256_entry_table)();
  }
};
#endif
std::thread g_thread;

const char* BucketName(int64_t dt_us) {
  if (dt_us >= 14000 && dt_us < 20000) return "60Hz";
  if (dt_us >= 28000 && dt_us < 38000) return "30Hz";
  if (dt_us >= 38000 && dt_us < 52000) return "24Hz";
  return "other";
}

// Return addresses of the four call sites of sub_821B1DD0 (the only VdSwap).
// 821610F0 takes one branch or the other, so 610F0 and 2578 are not nested.
std::atomic<int> g_call_610F0{0};
std::atomic<int> g_call_2578{0};
std::atomic<int> g_call_A15B0{0};
std::atomic<int> g_call_8740{0};
std::atomic<int> g_call_other{0};
std::atomic<uint32_t> g_other_lr{0};

void NoteSwapCaller(uint32_t lr) {
  switch (lr) {
    case 0x821612D4:
      g_call_610F0.fetch_add(1, std::memory_order_relaxed);
      break;
    case 0x821B26A8:
      g_call_2578.fetch_add(1, std::memory_order_relaxed);
      break;
    case 0x821A1750:
      g_call_A15B0.fetch_add(1, std::memory_order_relaxed);
      break;
    case 0x821B8848:
      g_call_8740.fetch_add(1, std::memory_order_relaxed);
      break;
    default:
      g_call_other.fetch_add(1, std::memory_order_relaxed);
      g_other_lr.store(lr, std::memory_order_relaxed);
      break;
  }
}

void SampleLoop() {
  uint64_t last_serial = rex::perf::GetPresentedFrameSerial();
  int n60 = 0, n30 = 0, n24 = 0, n_other = 0;
  int64_t last_dt = 0;
  const char* last_bucket = "none";
  auto window_start = std::chrono::steady_clock::now();
  while (!g_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(4));
    const uint64_t serial = rex::perf::GetPresentedFrameSerial();
    if (serial != last_serial) {
      const uint64_t missed = serial > last_serial + 1 ? serial - last_serial - 1 : 0;
      last_serial = serial;
      last_dt = rex::perf::GetPresentedFrameDtUs();
      last_bucket = BucketName(last_dt);
      if (last_bucket[0] == '6')
        ++n60;
      else if (last_bucket[0] == '3')
        ++n30;
      else if (last_bucket[0] == '2')
        ++n24;
      else
        ++n_other;
      if (missed) n_other += static_cast<int>(missed > 1000 ? 1000 : missed);
    }
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start < std::chrono::seconds(5)) continue;
    window_start = now;
    if (n60 + n30 + n24 + n_other == 0) continue;
    const int c610 = g_call_610F0.exchange(0, std::memory_order_relaxed);
    const int c2578 = g_call_2578.exchange(0, std::memory_order_relaxed);
    const int cA15 = g_call_A15B0.exchange(0, std::memory_order_relaxed);
    const int c8740 = g_call_8740.exchange(0, std::memory_order_relaxed);
    const int c_other = g_call_other.exchange(0, std::memory_order_relaxed);
    const uint32_t other_lr = g_other_lr.load(std::memory_order_relaxed);
    REXLOG_INFO(
        "guest present 5s: 60Hz={} 30Hz={} 24Hz={} other={} last={} us ({})",
        n60, n30, n24, n_other, last_dt, last_bucket);
    REXLOG_INFO(
        "guest swap callers 5s: 821610F0={} 821B2578={} 821A15B0={} 821B8740={} other={} other_lr={:08X}",
        c610, c2578, cA15, c8740, c_other, other_lr);
    broken_bond::host::CensusLog();
    broken_bond::host::ShadowLog();
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
    if (g_native) REXLOG_INFO("native 5s: {}", g_native->TakeStats());
#endif
    n60 = n30 = n24 = n_other = 0;
  }
}

}  // namespace

extern "C" void __imp__sub_821B1DD0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AE158(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AE5D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AE9C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821ACB10(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AD328(PPCContext& __restrict ctx, uint8_t* base);

struct LiveGuestBytes : broken_bond::host::GuestBytes {
  uint8_t* base = nullptr;
  bool Read(uint32_t address, void* dst, size_t bytes) const override {
    if (base == nullptr || bytes == 0 || bytes > (8u << 20) ||
        static_cast<uint64_t>(address) + bytes > 0x100000000ull) {
      return false;
    }
    const uint8_t* src = base + address;
    if (address >= 0xE0000000u) {
      src += 0x1000u;
    }
    std::memcpy(dst, src, bytes);
    return true;
  }
};

std::atomic<bool> g_draw_dumped{false};
std::unique_ptr<broken_bond::gfx::Device> g_host;
int g_host_budget = 64;
int g_host_learn = 1;
std::unordered_set<uint64_t> g_host_seen;

bool ReadBeU32(const LiveGuestBytes& live, uint32_t address, uint32_t& out);

uint64_t ShaderAddressKey(uint32_t vertex, uint32_t pixel, uint32_t declaration) {
  return (uint64_t(vertex) << 32) ^ uint64_t(pixel) ^ (uint64_t(declaration) << 1);
}

struct HostCounters {
  int submitted = 0;
  int cached = 0;
  int skipped = 0;
  int deferred = 0;
  int capped = 0;
  int learned = 0;
  int unique = 0;
  std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
};

HostCounters g_host_counts;

void HostNote(const std::string& reason) {
  static std::vector<std::string> seen;
  if (seen.size() >= 8u) {
    return;
  }
  for (const std::string& item : seen) {
    if (item == reason) {
      return;
    }
  }
  seen.push_back(reason);
  REXLOG_INFO("host draw waiting: {}", reason);
}

void HostLog() {
  const auto now = std::chrono::steady_clock::now();
  if (now - g_host_counts.window < std::chrono::seconds(5)) {
    return;
  }
  g_host_counts.window = now;
  uint32_t covered = 0;
  uint32_t colors = 0;
  std::vector<uint8_t> pixels;
  std::string reason;
  const bool sampled = g_host && g_host->ReadRetainedFrame(pixels, reason);
  if (sampled) {
    std::unordered_set<uint32_t> seen;
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      if (pixels[i] == 255 && pixels[i + 1] == 0 && pixels[i + 2] == 255 && pixels[i + 3] == 255) {
        continue;
      }
      seen.insert((uint32_t(pixels[i]) << 16) | (uint32_t(pixels[i + 1]) << 8) | uint32_t(pixels[i + 2]));
      ++covered;
    }
    colors = uint32_t(seen.size());
  }
  REXLOG_INFO(
      "host frame 5s: submitted={} cached={} skipped={} deferred={} capped={} learned={} unique={} covered={} colors={}",
      g_host_counts.submitted, g_host_counts.cached, g_host_counts.skipped, g_host_counts.deferred,
      g_host_counts.capped, g_host_counts.learned, g_host_counts.unique, covered, colors);
  g_host_counts.submitted = 0;
  g_host_counts.cached = 0;
  g_host_counts.skipped = 0;
  g_host_counts.deferred = 0;
  g_host_counts.capped = 0;
  g_host_counts.learned = 0;
}

void SubmitHostSpan(uint32_t device, const broken_bond::host::DrawSpan& span, uint8_t* base) {
  if (!g_host) {
    return;
  }
  const bool room = g_host_budget > 0;
  const bool learn = g_host_learn > 0 && g_host_counts.unique < 128;
  if (!room && !learn) {
    ++g_host_counts.deferred;
    HostLog();
    return;
  }
  LiveGuestBytes live;
  live.base = base;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t declaration = 0;
  const bool have_address = ReadBeU32(live, device + 12696u, vertex_shader) &&
                            ReadBeU32(live, device + 12692u, pixel_shader) &&
                            ReadBeU32(live, device + 11820u, declaration);
  const uint64_t address_key = ShaderAddressKey(vertex_shader, pixel_shader, declaration);
  if (!room && have_address && g_host_seen.find(address_key) != g_host_seen.end()) {
    ++g_host_counts.deferred;
    HostLog();
    return;
  }
  std::vector<uint8_t> identity;
  uint32_t identity_device = 0;
  std::string reason;
  if (!broken_bond::host::RepackDeviceDraw(live, device, identity, identity_device, reason, span,
                                           false)) {
    if (have_address) {
      g_host_seen.insert(address_key);
    }
    ++g_host_counts.skipped;
    HostNote(reason.empty() ? "host draw was not packed" : reason);
    HostLog();
    return;
  }
  bool shader_cached = false;
  if (!broken_bond::gfx::RetainedShaderCached(identity.data(), identity.size(), identity_device,
                                             shader_cached, reason)) {
    if (have_address) {
      g_host_seen.insert(address_key);
    }
    ++g_host_counts.skipped;
    HostNote(reason.empty() ? "host shader was not read" : reason);
    HostLog();
    return;
  }
  if (!shader_cached && g_host_counts.unique >= 128) {
    ++g_host_counts.capped;
    HostLog();
    return;
  }
  if (shader_cached && have_address) {
    g_host_seen.insert(address_key);
  }
  if (!room && shader_cached) {
    ++g_host_counts.deferred;
    HostLog();
    return;
  }
  const bool learning = !room && !shader_cached;
  std::vector<uint8_t> image;
  uint32_t image_device = 0;
  if (!broken_bond::host::RepackDeviceDraw(live, device, image, image_device, reason, span)) {
    if (have_address) {
      g_host_seen.insert(address_key);
    }
    ++g_host_counts.skipped;
    HostNote(reason.empty() ? "host draw was not packed" : reason);
    HostLog();
    return;
  }
  broken_bond::gfx::RetainedDrawInfo info;
  if (!broken_bond::gfx::SubmitRetainedDraw(*g_host, image.data(), image.size(), image_device,
                                           !shader_cached, false, info, reason)) {
    if (have_address) {
      g_host_seen.insert(address_key);
    }
    if (reason.rfind("deferred", 0) == 0) {
      ++g_host_counts.deferred;
    } else {
      ++g_host_counts.skipped;
      HostNote(reason.empty() ? "host draw failed" : reason);
    }
    HostLog();
    return;
  }
  if (learning) {
    --g_host_learn;
    ++g_host_counts.learned;
  } else {
    --g_host_budget;
  }
  if (have_address) {
    g_host_seen.insert(address_key);
  }
  ++g_host_counts.submitted;
  if (info.from_cache) {
    ++g_host_counts.cached;
  } else {
    ++g_host_counts.unique;
  }
  HostLog();
}

bool TrianglePrimitive(uint32_t prim);

void MaybeHostAuto(uint32_t device, uint32_t prim, uint32_t vertex_start, uint32_t vertex_count,
                   uint8_t* base) {
  if (REXCVAR_GET(renderer) != "host" || !g_host || !TrianglePrimitive(prim) || vertex_count < 3u) {
    return;
  }
  broken_bond::host::DrawSpan span;
  span.vertex_count = vertex_count;
  span.vertex_start = vertex_start;
  SubmitHostSpan(device, span, base);
}

void MaybeHostIndexed(uint32_t device, uint32_t prim, uint32_t base_vertex, uint32_t index_start,
                      uint32_t index_count, uint8_t* base) {
  if (REXCVAR_GET(renderer) != "host" || !g_host || !TrianglePrimitive(prim) || index_count < 3u) {
    return;
  }
  LiveGuestBytes live;
  live.base = base;
  uint32_t index_object = 0;
  uint32_t flags = 0;
  uint32_t index_base = 0;
  if (!ReadBeU32(live, device + 12436u, index_object) || index_object == 0 ||
      !ReadBeU32(live, index_object, flags) || !ReadBeU32(live, index_object + 24u, index_base)) {
    ++g_host_counts.skipped;
    HostNote("index buffer is missing");
    return;
  }
  const uint32_t index_size = (flags & 0x80000000u) != 0u ? 4u : 2u;
  const uint64_t index_bytes = uint64_t(index_count) * index_size;
  const uint64_t index_skip = uint64_t(index_start) * index_size;
  if (index_bytes == 0 || index_bytes > (8u << 20) || index_skip > 0xFFFFFFFFull ||
      uint64_t(index_base) + index_skip > 0xFFFFFFFFull) {
    ++g_host_counts.skipped;
    HostNote("index list is " + std::to_string(index_bytes) + " bytes");
    return;
  }
  broken_bond::host::DrawSpan span;
  span.vertex_start = 0;
  span.index_address = index_base + static_cast<uint32_t>(index_skip);
  span.index_bytes = static_cast<uint32_t>(index_bytes);
  span.index_size = index_size;
  span.base_vertex = base_vertex;
  std::vector<uint8_t> indices(static_cast<size_t>(index_bytes));
  std::string reason;
  uint32_t first = 0;
  uint32_t vertex_count = 0;
  if (!live.Read(span.index_address, indices.data(), indices.size()) ||
      !broken_bond::host::IndexRange(indices.data(), span.index_bytes, index_size, first,
                                     vertex_count, reason)) {
    ++g_host_counts.skipped;
    HostNote(reason.empty() ? "index list is outside guest memory" : reason);
    return;
  }
  span.vertex_count = vertex_count;
  span.vertex_start = first;
  span.index_first = first;
  SubmitHostSpan(device, span, base);
}

bool ReadBeU32(const LiveGuestBytes& live, uint32_t address, uint32_t& out) {
  uint8_t bytes[4];
  if (!live.Read(address, bytes, 4)) {
    return false;
  }
  out = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
        uint32_t(bytes[3]);
  return true;
}

void NoteDumpWait(const std::string& reason) {
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("device draw dump waiting: {}", reason);
  }
}

bool TrianglePrimitive(uint32_t prim) {
  const uint32_t type = prim & 0x3Fu;
  return type >= 4u && type <= 7u;
}

void WriteDump(const LiveGuestBytes& live, uint32_t device, const broken_bond::host::DrawSpan& span) {
  std::vector<uint8_t> image;
  uint32_t image_device = 0;
  std::string reason;
  if (!broken_bond::host::RepackDeviceDraw(live, device, image, image_device, reason, span)) {
    if (reason.rfind("texture ", 0) == 0) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        REXLOG_INFO("device draw dump waiting: {}", reason);
      }
      return;
    }
    static std::atomic<int> copies{0};
    if (reason.rfind("vertex stream is ", 0) != 0 && reason.rfind("index list is ", 0) != 0 &&
        copies.fetch_add(1, std::memory_order_relaxed) > 32) {
      g_draw_dumped.store(true, std::memory_order_relaxed);
      REXLOG_INFO("device draw dump stopped");
      return;
    }
    NoteDumpWait(reason);
    return;
  }
  if (broken_bond::host::DeviceDrawStreamBytes(image) == 0) {
    NoteDumpWait("vertex stream is empty");
    return;
  }
  broken_bond::host::DeviceDrawIndexInfo indices;
  broken_bond::host::ReadDeviceDrawIndices(image, indices);
  auto load_be = [&](uint32_t at) {
    const uint8_t* bytes = image.data() + at;
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
           uint32_t(bytes[3]);
  };
  const uint32_t texture_bytes = image.size() >= 12464u ? load_be(12460u) : 0u;
  const uint32_t texture_format = image.size() >= 12468u ? load_be(12464u) : 0u;
  if (texture_bytes == 0) {
    NoteDumpWait(texture_format == 0 ? "texture is absent"
                                     : "texture format is " + std::to_string(texture_format));
    return;
  }
  // Pixel microcode sits at the offset stored in the pixel-shader header.
  const uint32_t pixel_at = image.size() >= 0x50B0u ? load_be(0x50A8u) : 0u;
  const uint32_t pixel_bytes = image.size() >= 0x50B0u ? load_be(0x50ACu) : 0u;
  if (pixel_bytes == 0 || pixel_at > image.size() || pixel_bytes > image.size() - pixel_at) {
    NoteDumpWait("pixel shader is absent");
    return;
  }
  uint64_t pixel_hash = 14695981039346656037ull;
  for (uint32_t i = 0; i < pixel_bytes; ++i) {
    pixel_hash ^= image[pixel_at + i];
    pixel_hash *= 1099511628211ull;
  }
  static std::vector<uint64_t> seen;
  static int repeats = 0;
  static int attempts = 0;
  if (++attempts > 400) {
    g_draw_dumped.store(true, std::memory_order_relaxed);
    REXLOG_INFO("device draw dump stopped after {} shaders", seen.size());
    return;
  }
  for (uint64_t saved : seen) {
    if (saved == pixel_hash) {
      if (++repeats > 96 && !seen.empty()) {
        g_draw_dumped.store(true, std::memory_order_relaxed);
        REXLOG_INFO("device draw dump stopped after {} shaders", seen.size());
      }
      return;
    }
  }
  repeats = 0;
  const std::string path = "device-draw-" + std::to_string(seen.size()) + ".bin";
  if (!broken_bond::host::WriteDeviceDrawFile(path, image, image_device, reason)) {
    NoteDumpWait(reason);
    return;
  }
  broken_bond::host::DeviceDrawTexture texture;
  broken_bond::host::ReadDeviceDrawTexture(image, texture);
  REXLOG_INFO(
      "device draw dumped: {} bytes, stream {}, indices {}, texture {} slot {}, pixel {} -> {}",
      image.size(), broken_bond::host::DeviceDrawStreamBytes(image), indices.bytes, texture.bytes,
      texture.slot, pixel_bytes, path);
  seen.push_back(pixel_hash);
  if (seen.size() >= 8u) {
    g_draw_dumped.store(true, std::memory_order_relaxed);
  }
}

void MaybeDumpAuto(uint32_t device, uint32_t prim, uint32_t vertex_start, uint32_t vertex_count,
                   uint8_t* base) {
  if (!REXCVAR_GET(dump_device_draw) || g_draw_dumped.load(std::memory_order_relaxed)) {
    return;
  }
  if (!TrianglePrimitive(prim) || vertex_count < 3u) {
    NoteDumpWait("vertex count is " + std::to_string(vertex_count));
    return;
  }
  LiveGuestBytes live;
  live.base = base;
  broken_bond::host::DrawSpan span;
  span.vertex_count = vertex_count;
  span.vertex_start = vertex_start;
  WriteDump(live, device, span);
}

void MaybeDumpIndexed(uint32_t device, uint32_t prim, uint32_t base_vertex, uint32_t index_start,
                      uint32_t index_count, uint8_t* base) {
  if (!REXCVAR_GET(dump_device_draw) || g_draw_dumped.load(std::memory_order_relaxed)) {
    return;
  }
  if (!TrianglePrimitive(prim) || index_count < 3u) {
    NoteDumpWait("vertex count is " + std::to_string(index_count));
    return;
  }
  LiveGuestBytes live;
  live.base = base;
  uint32_t index_object = 0;
  uint32_t flags = 0;
  uint32_t index_base = 0;
  if (!ReadBeU32(live, device + 12436u, index_object) || index_object == 0 ||
      !ReadBeU32(live, index_object, flags) || !ReadBeU32(live, index_object + 24u, index_base)) {
    NoteDumpWait("index buffer is missing");
    return;
  }
  const uint32_t index_size = (flags & 0x80000000u) != 0u ? 4u : 2u;
  const uint64_t index_bytes = uint64_t(index_count) * index_size;
  const uint64_t index_skip = uint64_t(index_start) * index_size;
  if (index_bytes == 0 || index_bytes > (8u << 20) || index_skip > 0xFFFFFFFFull ||
      uint64_t(index_base) + index_skip > 0xFFFFFFFFull) {
    NoteDumpWait("index list is " + std::to_string(index_bytes) + " bytes");
    return;
  }
  const uint32_t index_address = index_base + static_cast<uint32_t>(index_skip);
  std::vector<uint8_t> indices(static_cast<size_t>(index_bytes));
  std::string reason;
  if (!live.Read(index_address, indices.data(), indices.size())) {
    NoteDumpWait("index list is outside guest memory");
    return;
  }
  uint32_t first = 0;
  uint32_t vertex_count = 0;
  if (!broken_bond::host::IndexRange(indices.data(), static_cast<uint32_t>(index_bytes), index_size,
                                     first, vertex_count, reason)) {
    NoteDumpWait(reason);
    return;
  }
  broken_bond::host::DrawSpan span;
  span.vertex_count = vertex_count;
  span.vertex_start = first;
  span.index_address = index_address;
  span.index_bytes = static_cast<uint32_t>(index_bytes);
  span.index_size = index_size;
  span.index_first = first;
  span.base_vertex = base_vertex;
  WriteDump(live, device, span);
}

void MaybeDumpDeviceDraw(uint32_t device, uint8_t* base) {
  // This entry has no vertex count, so the stream dword is the arena leftover.
  // The counted draws in sub_821AE5D0 and sub_821AE9C0 own the snapshot.
  (void)device;
  (void)base;
}

REX_HOOK_RAW(sub_821B1DD0) {
  NoteSwapCaller(static_cast<uint32_t>(ctx.lr));
  if (g_host) {
    g_host_budget = 64;
    g_host_learn = 1;
    g_host->RestartRetainedFrame();
  }
  broken_bond::host::ShadowSwap(base);
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
  if (g_native) {
    if (g_graphics && g_graphics->command_processor()) {
      uint32_t ramp[256];
      std::memcpy(ramp, GammaRampAccess::Table(*g_graphics->command_processor()), sizeof(ramp));
      g_native->SetGammaRamp(ramp);
    }
    g_native->Present(ctx.r4.u32, base);
  }
#endif
  const int census = broken_bond::host::CensusEnter(0x821B1DD0, ctx, base);
  __imp__sub_821B1DD0(ctx, base);
  broken_bond::host::CensusReturn(census, ctx);
}

REX_HOOK_RAW(sub_821AE158) {
  MaybeDumpDeviceDraw(static_cast<uint32_t>(ctx.r3.u32), base);
  broken_bond::host::ShadowDraw(broken_bond::host::DrawKind::kFlush, ctx, base);
  const int census = broken_bond::host::CensusEnter(0x821AE158, ctx, base);
  __imp__sub_821AE158(ctx, base);
  broken_bond::host::CensusReturn(census, ctx);
}

REX_HOOK_RAW(sub_821AE5D0) {
  MaybeDumpAuto(static_cast<uint32_t>(ctx.r3.u32), static_cast<uint32_t>(ctx.r4.u32),
                static_cast<uint32_t>(ctx.r5.u32), static_cast<uint32_t>(ctx.r6.u32), base);
  MaybeHostAuto(static_cast<uint32_t>(ctx.r3.u32), static_cast<uint32_t>(ctx.r4.u32),
                static_cast<uint32_t>(ctx.r5.u32), static_cast<uint32_t>(ctx.r6.u32), base);
  broken_bond::host::ShadowDraw(broken_bond::host::DrawKind::kAuto, ctx, base);
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
  if (g_native) {
    broken_bond::render::GuestDraw draw;
    draw.device = ctx.r3.u32;
    draw.prim = ctx.r4.u32;
    draw.start = ctx.r5.u32;
    draw.count = ctx.r6.u32;
    g_native->Draw(draw, base);
  }
#endif
  const int census = broken_bond::host::CensusEnter(0x821AE5D0, ctx, base);
  __imp__sub_821AE5D0(ctx, base);
  broken_bond::host::CensusReturn(census, ctx);
}

REX_HOOK_RAW(sub_821AE9C0) {
  MaybeDumpIndexed(static_cast<uint32_t>(ctx.r3.u32), static_cast<uint32_t>(ctx.r4.u32),
                   static_cast<uint32_t>(ctx.r5.u32), static_cast<uint32_t>(ctx.r6.u32),
                   static_cast<uint32_t>(ctx.r7.u32), base);
  MaybeHostIndexed(static_cast<uint32_t>(ctx.r3.u32), static_cast<uint32_t>(ctx.r4.u32),
                   static_cast<uint32_t>(ctx.r5.u32), static_cast<uint32_t>(ctx.r6.u32),
                   static_cast<uint32_t>(ctx.r7.u32), base);
  broken_bond::host::ShadowDraw(broken_bond::host::DrawKind::kIndexed, ctx, base);
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
  if (g_native) {
    broken_bond::render::GuestDraw draw;
    draw.indexed = true;
    draw.device = ctx.r3.u32;
    draw.prim = ctx.r4.u32;
    draw.base_vertex = ctx.r5.u32;
    draw.start = ctx.r6.u32;
    draw.count = ctx.r7.u32;
    g_native->Draw(draw, base);
  }
#endif
  const int census = broken_bond::host::CensusEnter(0x821AE9C0, ctx, base);
  __imp__sub_821AE9C0(ctx, base);
  broken_bond::host::CensusReturn(census, ctx);
}

// D3DDevice_ClearF and D3DDevice_Resolve (docs/D3D_MAP.md).
REX_HOOK_RAW(sub_821ACB10) {
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
  if (g_native) {
    g_native->Clear(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, float(ctx.f1.f64), ctx.r7.u32, base);
  }
#endif
  const int census = broken_bond::host::CensusEnter(0x821ACB10, ctx, base);
  __imp__sub_821ACB10(ctx, base);
  broken_bond::host::CensusReturn(census, ctx);
}

REX_HOOK_RAW(sub_821AD328) {
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
  if (g_native) {
    g_native->Resolve(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r10.u32,
                      float(ctx.f1.f64), base);
  }
#endif
  const int census = broken_bond::host::CensusEnter(0x821AD328, ctx, base);
  __imp__sub_821AD328(ctx, base);
  broken_bond::host::CensusReturn(census, ctx);
}

void BrokenBondApp::OnPostLaunchModule(rex::system::XThread*) {
#if defined(BROKEN_BOND_HAS_NATIVE_RENDERER)
  if (runtime()) g_graphics = static_cast<rex::graphics::GraphicsSystem*>(runtime()->graphics_system());
  if (REXCVAR_GET(renderer) == "native" && !g_native && window()) {
    auto native = std::make_unique<broken_bond::render::Renderer>();
    std::string reason;
    if (native->Open(window()->GetNativeWindowHandle(), reason)) {
      REXLOG_INFO("native renderer on {}", native->device_name());
      g_native = std::move(native);
    } else {
      REXLOG_ERROR("native renderer did not open: {}", reason);
    }
  }
#endif
  if (REXCVAR_GET(renderer) == "host" && !g_host) {
    g_host = std::make_unique<broken_bond::gfx::Device>();
    std::string reason;
    if (!g_host->Open(reason)) {
      REXLOG_INFO("host renderer did not open: {}", reason);
      g_host.reset();
    } else {
      REXLOG_INFO("host renderer {} api {:08X}", g_host->info().name, g_host->info().api);
    }
  }
  if (g_thread.joinable()) return;
  g_stop.store(false);
  g_thread = std::thread(SampleLoop);
  REXLOG_INFO("guest present sampler started (5s buckets: 60 / 30 / 24 Hz)");
}

void BrokenBondApp::OnShutdown() {
  g_stop.store(true);
  if (g_thread.joinable()) g_thread.join();
}
