// Shadow mode of the native renderer (docs/HOST_RENDERER.md, phase 3).
//
// With --shadow_draws every guest draw is recorded from the D3D device's
// register shadow and object fields (docs/D3D_MAP.md) right before the
// original runs, while Xenos still draws the frame. Each record names its
// shaders by the XenosRecomp cache key, so the log says how much of a frame
// the host renderer could already translate.

#pragma once

#include <cstdint>

struct PPCContext;

namespace broken_bond::host {

enum class DrawKind : uint8_t { kIndexed, kAuto, kFlush };

// Call from the draw hooks before the original: r3 is the device.
void ShadowDraw(DrawKind kind, const PPCContext& ctx, uint8_t* base);

// Call from the Swap hook before the original: closes the frame.
void ShadowSwap(uint8_t* base);

// Logs the last window's coverage. Called by the 5 s present sampler.
void ShadowLog();

// Keys shader objects from the first load on, as --shadow_draws does, without
// the per-draw records. The native renderer calls it before the game starts.
void ShadowKeepShaderKeys();

// XenosRecomp cache key of a guest shader object, 0 when it is not a shader.
uint64_t ShadowShaderKey(uint32_t object, const uint8_t* base);

}  // namespace broken_bond::host
