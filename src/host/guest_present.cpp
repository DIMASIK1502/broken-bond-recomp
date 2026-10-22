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
    REXLOG_INFO(
        "guest present 5s: 60Hz={} 30Hz={} 24Hz={} other={} last={} us ({})",
        n60, n30, n24, n_other, last_dt, last_bucket);
    n60 = n30 = n24 = n_other = 0;
  }
}

}  // namespace

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
