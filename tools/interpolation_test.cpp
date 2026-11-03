// Checks the frame interpolation helpers (src/render/interpolation.h).
// Prints the first failed check and returns 1, or prints "ok".

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

#include "render/interpolation.h"

namespace interp = broken_bond::render::interp;

#define CHECK(cond)                                                    \
  do {                                                                 \
    if (!(cond)) {                                                     \
      std::printf("interpolation_test: %d: %s\n", __LINE__, #cond);    \
      return 1;                                                        \
    }                                                                  \
  } while (0)

namespace {

uint64_t Key(uint64_t vs, uint32_t stream, uint32_t index_start, uint32_t texture) {
  const uint32_t streams[2] = {stream, 0x1000};
  const uint32_t textures[2] = {texture, 7};
  return interp::DrawKey(vs, 0x55, streams, 2, 0x2000, index_start, 300, textures, 2);
}

}  // namespace

int main() {
  const float nan = std::numeric_limits<float>::quiet_NaN();

  // Only the named motion registers blend, each matrix whole.
  {
    std::vector<float> prev(256 * 4, 0.5f), cur(256 * 4, 0.25f), out(256 * 4);
    const uint32_t all = interp::kWorldViewProj | interp::kWorldView | interp::kInvTransWorld | interp::kWorld |
                         interp::kViewerPosition;
    CHECK(interp::LerpMotion(prev.data(), cur.data(), out.data(), out.size(), all, 0.5f, 0.6f) == 0);
    CHECK(out[0] == 0.375f && out[15] == 0.375f);              // c0-c3
    CHECK(out[4 * 7] == 0.25f);                                // c7: not motion
    CHECK(out[4 * 14 + 3] == 0.375f && out[4 * 16] == 0.25f);  // c14 / c16
    CHECK(out[4 * 21] == 0.375f && out[4 * 42] == 0.25f);      // c21 / no bones
    CHECK(interp::LerpMotion(prev.data(), cur.data(), out.data(), out.size(), all | interp::kSkinMatrices, 0.5f,
                             0.6f) == 0);
    CHECK(out[4 * 42] == 0.375f && out[4 * 137 + 3] == 0.375f && out[4 * 138] == 0.25f);
    // A shader that names no motion registers keeps everything.
    CHECK(interp::LerpMotion(prev.data(), cur.data(), out.data(), out.size(), 0, 0.5f, 0.6f) == 0);
    CHECK(out[0] == 0.25f && out[4 * 21] == 0.25f);
    // Only g_mWorldViewProj named: g_mWorld keeps cur.
    CHECK(interp::LerpMotion(prev.data(), cur.data(), out.data(), out.size(), interp::kWorldViewProj, 0.5f, 0.6f) ==
          0);
    CHECK(out[0] == 0.375f && out[4 * 12] == 0.25f);
    // One element of g_mWorld jumps: the whole matrix takes cur.
    cur[4 * 13 + 1] = 5.0f;
    CHECK(interp::LerpMotion(prev.data(), cur.data(), out.data(), out.size(), all, 0.5f, 0.6f) == 1);
    CHECK(out[4 * 12] == 0.25f && out[4 * 13 + 1] == 5.0f && out[0] == 0.375f);
    // A 2D matrix whose small scale changes several times over is a cut.
    const float scale_prev[16] = {0, 0, 0, 0.003f, 0, 0, 0, 0.075f, 0, 0, 1, 0, 0, 0, 0, 1};
    float scale_cur[16] = {0, 0, 0, 0.0004f, 0, 0, 0, 0.0f, 0, 0, 1, 0, 0, 0, 0, 1};
    float scale_out[16];
    CHECK(interp::LerpMotion(scale_prev, scale_cur, scale_out, 16, interp::kWorldViewProj, 0.5f, 0.6f) == 1);
    CHECK(scale_out[3] == 0.0004f);
    // The same rows moving by a fifth of themselves blend.
    scale_cur[3] = 0.0024f;
    scale_cur[7] = 0.06f;
    CHECK(interp::LerpMotion(scale_prev, scale_cur, scale_out, 16, interp::kWorldViewProj, 0.5f, 0.6f) == 0);
    CHECK(std::fabs(scale_out[3] - 0.0027f) < 1e-6f);
  }

  // Pairs by key in order of appearance.
  const uint64_t A = 1, B = 2, C = 3;
  CHECK((interp::MatchDraws({A, B, A}, {A, A, C, B}) == std::vector<int32_t>{0, 2, -1, 1}));
  CHECK(interp::MatchedShare({0, 2, -1, 1}) == 0.75f);
  CHECK(interp::MatchedShare({}) == 0.0f);

  // Strict keys pair first; the rest pair by the loose key, in order.
  // prev: strict {S1, S2, S3}, loose {L, L, M}; cur: strict {S2, S9, S8}, loose {L, L, M}.
  // cur[0] takes prev[1] by its strict key, cur[1] the remaining L (prev[0]),
  // cur[2] the M (prev[2]).
  CHECK((interp::MatchDrawsTwoStage({11, 12, 13}, {1, 1, 2}, {12, 19, 18}, {1, 1, 2}) ==
         std::vector<int32_t>{1, 0, 2}));
  // A prev draw taken by its strict key is not paired again by the loose one.
  CHECK((interp::MatchDrawsTwoStage({11, 12}, {1, 1}, {19, 12, 18}, {1, 1, 1}) == std::vector<int32_t>{0, 1, -1}));

  float out[2];
  const float lp[2] = {0, 10}, lc[2] = {2, 20};
  interp::LerpFloats(lp, lc, out, 2, 0.5f);
  CHECK(out[0] == 1.0f && out[1] == 15.0f);
  const float np[1] = {nan}, nc[1] = {3};
  interp::LerpFloats(np, nc, out, 1, 0.5f);
  CHECK(out[0] == 3.0f);

  // Values that jump by more than half their size (at least 1) in a step
  // take this frame's value: a wrapping UV offset (0.98 -> 0.02), a 0/1
  // switch. Smooth changes still blend.
  {
    const float sp[4] = {0.98f, 0.0f, 0.30f, 100.0f}, sc[4] = {0.02f, 1.0f, 0.40f, 110.0f};
    float so[4];
    const size_t snapped = interp::LerpFloatsSnap(sp, sc, so, 4, 0.5f, 0.5f);
    CHECK(so[0] == 0.02f);
    CHECK(so[1] == 1.0f);
    CHECK(std::fabs(so[2] - 0.35f) < 1e-6f);
    CHECK(so[3] == 105.0f);
    CHECK(snapped == 2);
  }

  uint16_t half_out[1];
  const uint16_t hp[1] = {0x3C00}, hc[1] = {0x4000};  // 1.0, 2.0
  interp::LerpHalfs(hp, hc, half_out, 1, 0.5f);
  CHECK(half_out[0] == 0x3E00);  // 1.5
  const uint16_t hinf[1] = {0x7C00};
  interp::LerpHalfs(hinf, hc, half_out, 1, 0.5f);
  CHECK(half_out[0] == 0x4000);

  const uint64_t key = Key(0x10, 0x3000, 0, 5);
  CHECK(Key(0x11, 0x3000, 0, 5) != key);
  CHECK(Key(0x10, 0x3004, 0, 5) != key);
  CHECK(Key(0x10, 0x3000, 3, 5) != key);
  CHECK(Key(0x10, 0x3000, 0, 6) != key);
  CHECK(Key(0x10, 0x3000, 0, 5) == key);

  const float j1[2] = {1, 2}, j2[2] = {1.2f, 2}, j3[2] = {3, 2};
  CHECK(!interp::Jumped(j1, j2, 2, 0.5f));
  CHECK(interp::Jumped(j1, j3, 2, 0.5f));
  const float z0[1] = {0}, z1[1] = {0.4f}, five[1] = {5};
  CHECK(!interp::Jumped(z0, z1, 1, 0.5f));
  CHECK(!interp::Jumped(np, five, 1, 0.5f));

  CHECK(interp::ShouldInterpolate(true, 2, true, 2));
  CHECK(!interp::ShouldInterpolate(false, 2, true, 2));
  CHECK(!interp::ShouldInterpolate(true, 1, true, 2));
  CHECK(!interp::ShouldInterpolate(true, 2, false, 2));
  CHECK(!interp::ShouldInterpolate(true, 2, true, 1));

  std::printf("interpolation_test: ok\n");
  return 0;
}
