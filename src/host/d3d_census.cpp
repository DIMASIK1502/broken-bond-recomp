#include "host/d3d_census.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "generated/default/broken_bond_init.h"

REXCVAR_DEFINE_BOOL(d3d_census, false, "Game",
                    "Count calls into the guest D3D library and log them every 5 s")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Every function in 8219C358..821B4060 that code outside 82190000..821C7FFF
// calls (tools/ppc_xref.py entries), plus the draw, shader-load and flush
// helpers named in PORTING_LOG.md. 821B1DD0, 821AE158, 821AE5D0 and 821AE9C0
// are hooked in guest_present.cpp, 821A5C98 and 821A59B0 in shadow_draw.cpp;
// they report from there.
#define BROKEN_BOND_CENSUS_HOOKS(X)                                           \
  X(8219C358) X(8219C468) X(8219C588) X(8219C6A8) X(8219CA18) X(8219CAB0) \
  X(8219CB50) X(8219D070) X(8219D270) X(8219D6E0) X(8219D8F8) X(8219DE88) \
  X(8219DE98) X(8219DF68) X(8219DF80) X(8219E038) X(8219E058) X(8219E068) \
  X(8219E070) X(8219E138) X(8219E140) X(8219E148) X(8219E168) X(8219E288) \
  X(8219E3F0) X(8219E488) X(8219E4B0) X(8219E4C8) X(8219E510) X(8219E530) \
  X(8219F890) X(8219F990) X(8219FEB8) X(821A0558) X(821A0818) X(821A0A00) \
  X(821A0A78) X(821A0AF8) X(821A0B60) X(821A0BF0) X(821A0CB8) X(821A0D08) \
  X(821A0D18) X(821A0E70) X(821A1348) X(821A15B0) X(821A1AD0) X(821A1AE8) \
  X(821A1D80) X(821A2358) X(821A2A80) X(821A2E00) X(821A3590) X(821A3C10) \
  X(821A3DB8) X(821A4730) X(821A47B0) X(821A48D0) X(821A4958) X(821A49E8) \
  X(821A4D30) X(821A5008) X(821A5630) X(821A5710) X(821A57F0) X(821A5850) \
  X(821A58B0) X(821A5908) X(821A5960) X(821A59E8) X(821A5AD8) \
  X(821A5D90) X(821A5E80) X(821A6050) X(821A6158) X(821A62C8) \
  X(821A62D0) X(821A9F20) X(821A9FD8) X(821A9FE0) X(821AA000) X(821AA018) \
  X(821AA038) X(821AA060) X(821AA0D8) X(821AA158) X(821AA1D8) X(821AA270) \
  X(821AA2C8) X(821AA360) X(821AA3C0) X(821AA418) X(821AA478) X(821AA490) \
  X(821AA4A8) X(821AA4B0) X(821AA578) X(821AA590) X(821AA628) X(821AA690) \
  X(821AA6A8) X(821AA6C0) X(821AA6F0) X(821AA708) X(821AA780) X(821AA7D8) \
  X(821AA868) X(821AAD10) X(821AB270) X(821AB668) X(821ACC38) \
  X(821AEE10) X(821AEF50) X(821AEFF0) X(821AF240) X(821AF4C8) \
  X(821B1860) X(821B2578) X(821B2958) X(821B2CF0) X(821B4060) X(821B42C8) \
  X(821B5D68) X(821B6CE0) X(821BD3D0) X(821BDFD8) X(821BEB30) X(8257A0C8) \
  X(8257A2A0) X(8257A480)

namespace broken_bond::host {
namespace {

constexpr uint32_t kReportedElsewhere[] = {0x821A59B0, 0x821A5C98, 0x821ACB10, 0x821AD328,
                                           0x821AE158, 0x821AE5D0, 0x821AE9C0, 0x821B1DD0};

#define BROKEN_BOND_CENSUS_ADDRESS(addr) 0x##addr,
constexpr uint32_t kHooked[] = {BROKEN_BOND_CENSUS_HOOKS(BROKEN_BOND_CENSUS_ADDRESS)};
#undef BROKEN_BOND_CENSUS_ADDRESS

constexpr size_t kSlots = std::size(kHooked) + std::size(kReportedElsewhere);
constexpr int kSamples = 3;

constexpr std::array<uint32_t, kSlots> SortedAddresses() {
  std::array<uint32_t, kSlots> all{};
  size_t n = 0;
  for (uint32_t a : kHooked) all[n++] = a;
  for (uint32_t a : kReportedElsewhere) all[n++] = a;
  std::sort(all.begin(), all.end());
  return all;
}
constexpr std::array<uint32_t, kSlots> kAddresses = SortedAddresses();

struct Sample {
  uint32_t lr = 0;
  uint32_t r[8] = {};  // r3..r10
  uint32_t ret = 0;
  bool returned = false;
};

struct Entry {
  std::atomic<uint64_t> total{0};
  std::atomic<uint64_t> window{0};
  std::atomic<int> sampled{0};
  Sample samples[kSamples];
  bool logged_samples = false;
};

std::array<Entry, kSlots> g_entries;
std::mutex g_sample_mutex;
std::atomic<uint8_t*> g_base{nullptr};
bool g_tables_logged = false;

// sub_821B38F8 resets the device from two static tables of 12-byte records
// {tag, setter, default}: 101 render states, then 20 sampler states that it
// applies to each of 26 samplers. It copies each setter into the device
// (render states at device+64, sampler states at device+468) and calls it.
constexpr uint32_t kRenderStateTable = 0x82CD5E00;
constexpr uint32_t kRenderStateCount = 101;
constexpr uint32_t kSamplerStateTable = 0x82CD62C0;
constexpr uint32_t kSamplerStateCount = 20;

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

void LogStateTable(const uint8_t* base, const char* name, uint32_t table, uint32_t count) {
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t record = table + i * 12;
    REXLOG_INFO("d3d {} {:3} tag={:08X} setter={:08X} default={:08X}", name, i,
                LoadBe32(base, record), LoadBe32(base, record + 4), LoadBe32(base, record + 8));
  }
}

bool Enabled() {
  static const bool enabled = REXCVAR_GET(d3d_census);
  return enabled;
}

}  // namespace

int CensusEnter(uint32_t address, const PPCContext& ctx, uint8_t* base) {
  if (!Enabled()) return -1;
  g_base.store(base, std::memory_order_relaxed);
  const auto it = std::lower_bound(kAddresses.begin(), kAddresses.end(), address);
  if (it == kAddresses.end() || *it != address) return -1;
  const int slot = static_cast<int>(it - kAddresses.begin());
  Entry& entry = g_entries[slot];
  entry.total.fetch_add(1, std::memory_order_relaxed);
  entry.window.fetch_add(1, std::memory_order_relaxed);
  const int index = entry.sampled.fetch_add(1, std::memory_order_relaxed);
  if (index >= kSamples) return -1;
  std::lock_guard<std::mutex> lock(g_sample_mutex);
  Sample& sample = entry.samples[index];
  sample.lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t regs[8] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                            ctx.r7.u32, ctx.r8.u32, ctx.r9.u32, ctx.r10.u32};
  std::copy(std::begin(regs), std::end(regs), sample.r);
  return slot * kSamples + index;
}

void CensusReturn(int slot, const PPCContext& ctx) {
  if (slot < 0) return;
  std::lock_guard<std::mutex> lock(g_sample_mutex);
  Sample& sample = g_entries[slot / kSamples].samples[slot % kSamples];
  sample.ret = ctx.r3.u32;
  sample.returned = true;
}

void CensusLog() {
  if (!Enabled()) return;
  std::string line;
  int active = 0;
  for (size_t i = 0; i < kSlots; ++i) {
    const uint64_t window = g_entries[i].window.exchange(0, std::memory_order_relaxed);
    if (window == 0) continue;
    ++active;
    line += fmt::format(" {:08X}={}", kAddresses[i], window);
  }
  REXLOG_INFO("d3d census 5s: {} entries{}", active, line);
  if (const uint8_t* base = g_base.load(std::memory_order_relaxed); base && !g_tables_logged) {
    g_tables_logged = true;
    LogStateTable(base, "renderstate", kRenderStateTable, kRenderStateCount);
    LogStateTable(base, "samplerstate", kSamplerStateTable, kSamplerStateCount);
  }
  std::lock_guard<std::mutex> lock(g_sample_mutex);
  for (size_t i = 0; i < kSlots; ++i) {
    Entry& entry = g_entries[i];
    if (entry.logged_samples || entry.sampled.load(std::memory_order_relaxed) == 0) continue;
    entry.logged_samples = true;
    const int count = std::min(entry.sampled.load(std::memory_order_relaxed), kSamples);
    for (int s = 0; s < count; ++s) {
      const Sample& sample = entry.samples[s];
      REXLOG_INFO(
          "d3d census {:08X} #{} lr={:08X} r3={:08X} r4={:08X} r5={:08X} r6={:08X} r7={:08X} "
          "r8={:08X} r9={:08X} r10={:08X} ret={}",
          kAddresses[i], s, sample.lr, sample.r[0], sample.r[1], sample.r[2], sample.r[3],
          sample.r[4], sample.r[5], sample.r[6], sample.r[7],
          sample.returned ? fmt::format("{:08X}", sample.ret) : std::string("-"));
    }
  }
}

}  // namespace broken_bond::host

#define BROKEN_BOND_CENSUS_HOOK(addr)                                        \
  REX_HOOK_RAW(sub_##addr) {                                                 \
    const int slot = broken_bond::host::CensusEnter(0x##addr, ctx, base);          \
    __imp__sub_##addr(ctx, base);                                            \
    broken_bond::host::CensusReturn(slot, ctx);                              \
  }
BROKEN_BOND_CENSUS_HOOKS(BROKEN_BOND_CENSUS_HOOK)
#undef BROKEN_BOND_CENSUS_HOOK
