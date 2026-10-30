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

  // Pairs by key in order of appearance.
  const uint64_t A = 1, B = 2, C = 3;
  CHECK((interp::MatchDraws({A, B, A}, {A, A, C, B}) == std::vector<int32_t>{0, 2, -1, 1}));
  CHECK(interp::MatchedShare({0, 2, -1, 1}) == 0.75f);
  CHECK(interp::MatchedShare({}) == 0.0f);

  float out[2];
  const float lp[2] = {0, 10}, lc[2] = {2, 20};
  interp::LerpFloats(lp, lc, out, 2, 0.5f);
  CHECK(out[0] == 1.0f && out[1] == 15.0f);
  const float np[1] = {nan}, nc[1] = {3};
  interp::LerpFloats(np, nc, out, 1, 0.5f);
  CHECK(out[0] == 3.0f);

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
