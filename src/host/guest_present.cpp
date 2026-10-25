#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/system/xthread.h>

#include "generated/default/broken_bond_init.h"
#include "broken_bond_app.h"
#include "host/device_repack.h"

REXCVAR_DEFINE_BOOL(dump_device_draw, false, "Game",
                    "Write the first triangle draw's own vertices to device-draw.bin");

namespace {

std::atomic<bool> g_stop{false};
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
    n60 = n30 = n24 = n_other = 0;
  }
}

}  // namespace

extern "C" void __imp__sub_821B1DD0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AE158(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AE5D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_821AE9C0(PPCContext& __restrict ctx, uint8_t* base);

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
  if (!broken_bond::host::WriteDeviceDrawFile("device-draw.bin", image, image_device, reason)) {
    NoteDumpWait(reason);
    return;
  }
  g_draw_dumped.store(true, std::memory_order_relaxed);
  broken_bond::host::DeviceDrawIndexInfo indices;
  broken_bond::host::ReadDeviceDrawIndices(image, indices);
  broken_bond::host::DeviceDrawTexture texture;
  broken_bond::host::ReadDeviceDrawTexture(image, texture);
  REXLOG_INFO("device draw dumped: {} bytes, stream {}, indices {}, texture {} -> device-draw.bin",
              image.size(), broken_bond::host::DeviceDrawStreamBytes(image), indices.bytes,
              texture.bytes);
}

void MaybeDumpAuto(uint32_t device, uint32_t prim, uint32_t vertex_start, uint32_t vertex_count,
                   uint8_t* base) {
  if (!REXCVAR_GET(dump_device_draw) || g_draw_dumped.load(std::memory_order_relaxed)) {
    return;
  }
  if (!TrianglePrimitive(prim) || vertex_count < 64u) {
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
  if (!TrianglePrimitive(prim) || index_count < 64u) {
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
  if (!REXCVAR_GET(dump_device_draw) || g_draw_dumped.load(std::memory_order_relaxed)) {
    return;
  }
  LiveGuestBytes live;
  live.base = base;
  std::vector<uint8_t> image;
  uint32_t image_device = 0;
  std::string reason;
  if (!broken_bond::host::RepackDeviceDraw(live, device, image, image_device, reason)) {
    // Empty and oversized streams return before the copy. A failed copy of a
    // stream that fit is the only path that can repeat real work.
    static std::atomic<int> copies{0};
    if (reason.rfind("vertex stream is ", 0) != 0 &&
        copies.fetch_add(1, std::memory_order_relaxed) > 32) {
      g_draw_dumped.store(true, std::memory_order_relaxed);
      REXLOG_INFO("device draw dump stopped");
      return;
    }
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("device draw dump waiting: {}", reason);
    }
    return;
  }
  if (broken_bond::host::DeviceDrawStreamBytes(image) == 0) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("device draw dump waiting: vertex stream is empty");
    }
    return;
  }
  if (!broken_bond::host::WriteDeviceDrawFile("device-draw.bin", image, image_device, reason)) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("device draw dump waiting: {}", reason);
    }
    return;
  }
  g_draw_dumped.store(true, std::memory_order_relaxed);
  REXLOG_INFO("device draw dumped: {} bytes at {:08X} -> device-draw.bin", image.size(),
              image_device);
}

REX_HOOK_RAW(sub_821B1DD0) {
  NoteSwapCaller(static_cast<uint32_t>(ctx.lr));
  __imp__sub_821B1DD0(ctx, base);
}

REX_HOOK_RAW(sub_821AE158) {
  MaybeDumpDeviceDraw(static_cast<uint32_t>(ctx.r3.u32), base);
  __imp__sub_821AE158(ctx, base);
}

REX_HOOK_RAW(sub_821AE5D0) {
  MaybeDumpAuto(static_cast<uint32_t>(ctx.r3.u32), static_cast<uint32_t>(ctx.r4.u32),
                static_cast<uint32_t>(ctx.r5.u32), static_cast<uint32_t>(ctx.r6.u32), base);
  __imp__sub_821AE5D0(ctx, base);
}

REX_HOOK_RAW(sub_821AE9C0) {
  MaybeDumpIndexed(static_cast<uint32_t>(ctx.r3.u32), static_cast<uint32_t>(ctx.r4.u32),
                   static_cast<uint32_t>(ctx.r5.u32), static_cast<uint32_t>(ctx.r6.u32),
                   static_cast<uint32_t>(ctx.r7.u32), base);
  __imp__sub_821AE9C0(ctx, base);
}

void BrokenBondApp::OnPostLaunchModule(rex::system::XThread*) {
  if (g_thread.joinable()) return;
  g_stop.store(false);
  g_thread = std::thread(SampleLoop);
  REXLOG_INFO("guest present sampler started (5s buckets: 60 / 30 / 24 Hz)");
}

void BrokenBondApp::OnShutdown() {
  g_stop.store(true);
  if (g_thread.joinable()) g_thread.join();
}
