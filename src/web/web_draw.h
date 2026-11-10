#pragma once

#include <cstdint>

namespace broken_bond::web {

// Creates the Plume WebGPU device on #canvas and loads the WGSL cache.
// The blue present must already have been released.
bool OpenRenderer();

// Guest Swap. Returns false when the renderer is not open.
bool PresentRenderer(uint32_t frontbuffer, const uint8_t* base);

}  // namespace broken_bond::web
