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
