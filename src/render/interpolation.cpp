#include "render/interpolation.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace broken_bond::render::interp {

namespace {

uint64_t Mix(uint64_t hash, uint64_t value) {
  hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
  return hash * 0xFF51AFD7ED558CCDull;
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0x1F) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      // Subnormal: normalize.
      exponent = 127 - 15 + 1;
      while ((mantissa & 0x400) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      bits = sign | (exponent << 23) | ((mantissa & 0x3FF) << 13);
    }
  } else {
    bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint16_t FloatToHalf(float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, 4);
  const uint16_t sign = uint16_t((bits >> 16) & 0x8000);
  const int32_t exponent = int32_t((bits >> 23) & 0xFF) - 127 + 15;
  uint32_t mantissa = bits & 0x7FFFFF;
  if (((bits >> 23) & 0xFF) == 0xFF) return uint16_t(sign | 0x7C00 | (mantissa ? 0x200 : 0));
  if (exponent >= 0x1F) return uint16_t(sign | 0x7C00);
  if (exponent <= 0) {
    if (exponent < -10) return sign;
    mantissa |= 0x800000;
    const uint32_t shift = uint32_t(14 - exponent);
    uint32_t half = mantissa >> shift;
    const uint32_t rest = mantissa & ((1u << shift) - 1);
    const uint32_t halfway = 1u << (shift - 1);
    if (rest > halfway || (rest == halfway && (half & 1))) ++half;
    return uint16_t(sign | half);
  }
  uint32_t half = (uint32_t(exponent) << 10) | (mantissa >> 13);
  const uint32_t rest = mantissa & 0x1FFF;
  if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) ++half;  // may carry into the exponent, as it should
  return uint16_t(sign | half);
}

}  // namespace

uint64_t DrawKey(uint64_t vs_key, uint64_t ps_key, const uint32_t* stream_addresses, uint32_t stream_count,
                 uint32_t index_address, uint32_t index_start, uint32_t count, const uint32_t* texture_indices,
                 uint32_t texture_count) {
  uint64_t hash = Mix(Mix(0, vs_key), ps_key);
  for (uint32_t i = 0; i < stream_count; ++i) hash = Mix(hash, stream_addresses[i]);
  hash = Mix(Mix(Mix(hash, index_address), index_start), count);
  for (uint32_t i = 0; i < texture_count; ++i) hash = Mix(hash, texture_indices[i]);
  return hash;
}

std::vector<int32_t> MatchDraws(const std::vector<uint64_t>& prev, const std::vector<uint64_t>& cur) {
  // Per key, the positions in prev still waiting for a pair, in order.
  std::unordered_map<uint64_t, std::vector<int32_t>> waiting;
  for (size_t i = prev.size(); i-- > 0;) waiting[prev[i]].push_back(int32_t(i));
  std::vector<int32_t> match(cur.size(), -1);
  for (size_t i = 0; i < cur.size(); ++i) {
    auto found = waiting.find(cur[i]);
    if (found == waiting.end() || found->second.empty()) continue;
    match[i] = found->second.back();
    found->second.pop_back();
  }
  return match;
}

std::vector<int32_t> MatchDrawsTwoStage(const std::vector<uint64_t>& prev_strict,
                                        const std::vector<uint64_t>& prev_loose,
                                        const std::vector<uint64_t>& cur_strict,
                                        const std::vector<uint64_t>& cur_loose) {
  std::vector<int32_t> match = MatchDraws(prev_strict, cur_strict);
  std::vector<bool> taken(prev_loose.size(), false);
  for (int32_t m : match) {
    if (m >= 0) taken[size_t(m)] = true;
  }
  std::unordered_map<uint64_t, std::vector<int32_t>> waiting;
  for (size_t i = prev_loose.size(); i-- > 0;) {
    if (!taken[i]) waiting[prev_loose[i]].push_back(int32_t(i));
  }
  for (size_t i = 0; i < cur_loose.size(); ++i) {
    if (match[i] >= 0) continue;
    auto found = waiting.find(cur_loose[i]);
    if (found == waiting.end() || found->second.empty()) continue;
    match[i] = found->second.back();
    found->second.pop_back();
  }
  return match;
}

float MatchedShare(const std::vector<int32_t>& match) {
  if (match.empty()) return 0.0f;
  const size_t paired = size_t(std::count_if(match.begin(), match.end(), [](int32_t m) { return m >= 0; }));
  return float(paired) / float(match.size());
}

void LerpFloats(const float* prev, const float* cur, float* out, size_t count, float t) {
  for (size_t i = 0; i < count; ++i) {
    out[i] = std::isfinite(prev[i]) && std::isfinite(cur[i]) ? prev[i] + (cur[i] - prev[i]) * t : cur[i];
  }
}

size_t LerpFloatsSnap(const float* prev, const float* cur, float* out, size_t count, float t, float jump) {
  size_t snapped = 0;
  for (size_t i = 0; i < count; ++i) {
    const float a = prev[i], b = cur[i];
    if (!std::isfinite(a) || !std::isfinite(b)) {
      out[i] = b;
    } else if (std::fabs(b - a) > jump * std::max({1.0f, std::fabs(a), std::fabs(b)})) {
      out[i] = b;
      ++snapped;
    } else {
      out[i] = a + (b - a) * t;
    }
  }
  return snapped;
}

bool MatrixMoved(const float* prev, const float* cur, size_t rows, float jump) {
  for (size_t i = 0; i < rows * 4; ++i) {
    if (!std::isfinite(prev[i]) || !std::isfinite(cur[i])) return true;
  }
  auto flat = [&](const float* m) {
    const float* w = m + 12;
    return std::fabs(w[0]) + std::fabs(w[1]) + std::fabs(w[2]) <= 1e-6f * std::fabs(w[3]);
  };
  const bool by_row = rows == 4 && flat(prev) && flat(cur);
  float moved = 0.0f, length_prev = 0.0f, length_cur = 0.0f;  // squared
  for (size_t row = 0; row < rows * 4; row += 4) {
    for (size_t i = row; i < row + 4; ++i) {
      moved += (cur[i] - prev[i]) * (cur[i] - prev[i]);
      length_prev += prev[i] * prev[i];
      length_cur += cur[i] * cur[i];
    }
    if (by_row) {
      if (moved > jump * jump * std::max(length_prev, length_cur)) return true;
      moved = length_prev = length_cur = 0.0f;
    }
  }
  return !by_row && moved > jump * jump * std::max(length_prev, length_cur);
}

uint64_t MixFloats(uint64_t key, const float* values, size_t count) {
  for (size_t i = 0; i < count; ++i) {
    uint32_t bits;
    std::memcpy(&bits, &values[i], 4);
    key = Mix(key, bits);
  }
  return key;
}

size_t LerpMotion(const float* prev, const float* cur, float* out, size_t floats, uint32_t motion, float t,
                  float jump, uint64_t* snapped_by_kind) {
  std::copy(cur, cur + floats, out);
  size_t snapped = 0;
  auto block = [&](size_t first, size_t count, MotionKind kind) {
    const size_t begin = first * 4, size = count * 4;
    if (begin + size > floats) return;
    if (MatrixMoved(prev + begin, cur + begin, count, jump)) {
      ++snapped;
      if (snapped_by_kind) ++snapped_by_kind[kind];
      return;
    }
    LerpFloats(prev + begin, cur + begin, out + begin, size, t);
  };
  if (motion & kWorldViewProj) block(0, 4, kKindWorldViewProj);
  if (motion & kWorldView) block(4, 3, kKindWorldView);
  if (motion & kInvTransWorld) block(8, 4, kKindInvTransWorld);
  if (motion & kWorld) block(12, 3, kKindWorld);
  if (motion & kViewerPosition) block(21, 1, kKindViewerPosition);
  if (motion & kSkinMatrices) {
    for (size_t bone = 0; bone < 32; ++bone) block(42 + bone * 3, 3, kKindSkin);
  }
  const size_t shadow0 = (motion >> kShadow0Shift) & 7, shadow1 = (motion >> kShadow1Shift) & 7;
  for (size_t cascade = 0; cascade < shadow0; ++cascade) block(42 + cascade * 3, 3, kKindShadow0);
  for (size_t cascade = 0; cascade < shadow1; ++cascade) block(42 + shadow0 * 3 + cascade * 2, 2, kKindShadow1);
  return snapped;
}

void LerpHalfs(const uint16_t* prev, const uint16_t* cur, uint16_t* out, size_t count, float t) {
  for (size_t i = 0; i < count; ++i) {
    const float a = HalfToFloat(prev[i]);
    const float b = HalfToFloat(cur[i]);
    out[i] = std::isfinite(a) && std::isfinite(b) ? FloatToHalf(a + (b - a) * t) : cur[i];
  }
}

bool Jumped(const float* prev, const float* cur, size_t count, float threshold) {
  for (size_t i = 0; i < count; ++i) {
    if (!std::isfinite(prev[i]) || !std::isfinite(cur[i])) continue;
    if (std::fabs(cur[i] - prev[i]) > threshold * std::max(1.0f, std::fabs(prev[i]))) return true;
  }
  return false;
}

bool ShouldInterpolate(bool enabled, uint32_t flip_interval, bool have_prev, uint32_t prev_interval) {
  return enabled && flip_interval == 2 && have_prev && prev_interval == 2;
}

}  // namespace broken_bond::render::interp
