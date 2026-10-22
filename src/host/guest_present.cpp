#include <atomic>
#include <chrono>
#include <thread>

#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <rex/system/xthread.h>

#include "generated/default/broken_bond_init.h"
#include "broken_bond_app.h"

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

REX_HOOK_RAW(sub_821B1DD0) {
  NoteSwapCaller(static_cast<uint32_t>(ctx.lr));
  __imp__sub_821B1DD0(ctx, base);
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
