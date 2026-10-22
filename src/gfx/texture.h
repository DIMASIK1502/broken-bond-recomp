// 2D Xenos texture layout for the host renderer. Uses the SDK untile, not a
// second copy of the address math.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace broken_bond::gfx {

// Tile a tightly packed RGBA8 image with GetTiledOffset2D and untile it with
// the SDK. Fails when the bytes do not come back unchanged.
bool RoundTripTiledRgba8(const uint8_t* linear, uint32_t pitch, uint32_t height,
                         std::string& reason);

}  // namespace broken_bond::gfx
