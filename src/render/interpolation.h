// Frame interpolation helpers for the native renderer: pairing the draws of
// two frames, blending their data and deciding when an in-between frame is
// drawn. No GPU objects; tools/interpolation_test.cpp checks them.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace broken_bond::render::interp {

// Names a draw across frames: its shaders, guest vertex streams, index
// range, vertex or index count and 2D texture heap indices.
uint64_t DrawKey(uint64_t vs_key, uint64_t ps_key, const uint32_t* stream_addresses, uint32_t stream_count,
                 uint32_t index_address, uint32_t index_start, uint32_t count, const uint32_t* texture_indices,
                 uint32_t texture_count);

// For each draw of `cur`, the index of its pair in `prev`, or -1. The k-th
// draw with a key in `cur` pairs with the k-th draw with that key in `prev`.
std::vector<int32_t> MatchDraws(const std::vector<uint64_t>& prev, const std::vector<uint64_t>& cur);

// Pairs by `strict` keys first, as MatchDraws; draws left over on both
// sides then pair by `loose` keys, in order. Same-looking draws that differ
// in what the strict key holds (textures, index ranges) keep their own pair.
std::vector<int32_t> MatchDrawsTwoStage(const std::vector<uint64_t>& prev_strict,
                                        const std::vector<uint64_t>& prev_loose,
                                        const std::vector<uint64_t>& cur_strict,
                                        const std::vector<uint64_t>& cur_loose);

// Share of draws that have a pair; 0 for no draws.
float MatchedShare(const std::vector<int32_t>& match);

// out = prev + (cur - prev) * t. A non-finite value on either side gives cur.
void LerpFloats(const float* prev, const float* cur, float* out, size_t count, float t);

// As LerpFloats, except that a value which moved by more than
// jump * max(1, |prev|, |cur|) takes cur: such a change in one step is a
// wrap (a UV offset going 0.98 -> 0.02) or a switch, not motion, and its
// halfway value is wrong. Returns how many values took cur that way.
size_t LerpFloatsSnap(const float* prev, const float* cur, float* out, size_t count, float t, float jump);

// Motion registers of the game's vertex shaders, where their constant
// tables put them; a bit per register block in LerpMotion's `motion`.
enum Motion : uint32_t {
  kSkinMatrices = 1 << 0,    // g_aSkinMatrices c42-c137, 32 bones of three rows
  kWorldViewProj = 1 << 1,   // g_mWorldViewProj c0-c3
  kWorldView = 1 << 2,       // g_mWorldView c4-c6
  kInvTransWorld = 1 << 3,   // g_mInvTransWorld c8-c11
  kWorld = 1 << 4,           // g_mWorld c12-c14
  kViewerPosition = 1 << 5,  // g_vViewerPosition c21
  // Cascade counts (0-4) in two 3-bit fields: g_mShadowTransform0 from c42,
  // three rows each, then g_mShadowTransform1, two rows each.
  kShadow0Shift = 6,
  kShadow1Shift = 9,
};

// Kinds of matrix LerpMotion counts in `snapped_by_kind`.
enum MotionKind : size_t {
  kKindWorldViewProj,
  kKindWorldView,
  kKindInvTransWorld,
  kKindWorld,
  kKindViewerPosition,
  kKindSkin,
  kKindShadow0,
  kKindShadow1,
  kMotionKinds,
};

// True when a matrix of `rows` float4 rows moved by more than `jump` times
// its length (or holds a non-finite value). A 3D matrix is measured whole:
// a row of an object straight ahead is almost all a near-zero offset that
// any camera turn changes many times over, while the whole matrix also
// holds the object's distance. A 4-row matrix without perspective (last row
// 0, 0, 0, w: the 2D videos and menus, scales of 0.002) is measured row by
// row against each row's own length, with no absolute floor.
bool MatrixMoved(const float* prev, const float* cur, size_t rows, float jump);

// Mixes `count` floats (bit patterns) into `key`.
uint64_t MixFloats(uint64_t key, const float* values, size_t count);

// Blends only the vertex constants that move things, each matrix whole: the
// blocks named in `motion`. Other constants (UV scrolls, wind, switches, and
// whatever a shader without these names keeps there) take cur, and so does
// a matrix that MatrixMoved by `jump`. `floats` is the size of all three
// arrays. Returns how many matrices took cur that way, and adds them per
// MotionKind to `snapped_by_kind` when given.
size_t LerpMotion(const float* prev, const float* cur, float* out, size_t floats, uint32_t motion, float t,
                  float jump, uint64_t* snapped_by_kind = nullptr);

// The same for IEEE half floats.
void LerpHalfs(const uint16_t* prev, const uint16_t* cur, uint16_t* out, size_t count, float t);

// True when some value moved by more than threshold * max(1, |prev|).
// Pairs with a non-finite value are left out.
bool Jumped(const float* prev, const float* cur, size_t count, float threshold);

// An in-between frame is drawn only when enabled and both frames come from
// a 30 Hz step (the game asked for a flip every 2 vblanks).
bool ShouldInterpolate(bool enabled, uint32_t flip_interval, bool have_prev, uint32_t prev_interval);

}  // namespace broken_bond::render::interp
