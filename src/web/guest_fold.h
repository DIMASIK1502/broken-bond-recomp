// Guest address fold for the browser build.
//
// WebAssembly has one linear memory, so the Xbox 360 views cannot be separate
// mmap aliases. This is the same map ReXGlue uses for the 2.75 GiB guest
// window, including the +0x1000 shift of the 0xE0000000 physical view that
// the native renderer already applies (docs/HOST_RENDERER.md).

#pragma once

#include <array>
#include <cstdint>

namespace broken_bond::web {

inline constexpr uint64_t kGuestWindowSize = 0xB0000000ull;
inline constexpr uint64_t kPhysicalWindowOffset = 0x90000000ull;
inline constexpr uint32_t kPhysicalSize = 0x20000000u;

inline constexpr std::array<int64_t, 256> kAddressFold = [] {
  std::array<int64_t, 256> fold{};
  fold[0x7F] = int64_t(kPhysicalWindowOffset) - 0x7F000000;
  for (uint32_t i = 0x90; i < 0xA0; ++i) {
    fold[i] = -0x10000000;
  }
  for (uint32_t i = 0xA0; i < 0xC0; ++i) {
    fold[i] = int64_t(kPhysicalWindowOffset) - 0xA0000000;
  }
  for (uint32_t i = 0xC0; i < 0xE0; ++i) {
    fold[i] = int64_t(kPhysicalWindowOffset) - 0xC0000000;
  }
  for (uint32_t i = 0xE0; i < 0x100; ++i) {
    fold[i] = int64_t(kPhysicalWindowOffset) - 0xE0000000 + 0x1000;
  }
  return fold;
}();

// Byte offset from the guest window base to the host byte of `guest_address`.
inline constexpr int64_t HostOffset(uint32_t guest_address) {
  return int64_t(guest_address) + kAddressFold[guest_address >> 24];
}

}  // namespace broken_bond::web
