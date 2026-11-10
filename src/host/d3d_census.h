// Call census of the guest D3D library (docs/D3D_MAP.md).
//
// With --d3d_census every candidate API entry counts its calls and keeps its
// first argument registers and return value. The guest present sampler logs
// the counts every 5 s. Off by default; each hook then costs one load.

#pragma once

#include <cstdint>

struct PPCContext;

namespace broken_bond::host {

// Call from a hook before the original runs. Returns the slot to pass to
// CensusReturn, or -1 when the census is off or the address is unknown.
int CensusEnter(uint32_t address, const PPCContext& ctx, uint8_t* base);
void CensusReturn(int slot, const PPCContext& ctx);

// Logs entries called in the last window, and once per entry its samples.
void CensusLog();

}  // namespace broken_bond::host
