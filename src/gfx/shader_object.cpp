#include "gfx/shader_object.h"

namespace broken_bond::gfx {
namespace {

constexpr uint32_t kPhysicalMask = 0x1FFFFFFFu;

bool LoadBe(const uint8_t* guest, size_t guest_bytes, uint32_t address, uint32_t& out,
            std::string& reason) {
  const uint32_t physical = address & kPhysicalMask;
  if (guest == nullptr || static_cast<uint64_t>(physical) + 4u > guest_bytes) {
    reason = "shader object dword is outside guest memory";
    return false;
  }
  const uint8_t* bytes = guest + physical;
  out = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
        uint32_t(bytes[3]);
  return true;
}

uint32_t GuestAddress(uint32_t sum) {
  const uint32_t rotated = (sum << 12) | (sum >> 20);
  const uint32_t high = rotated & 0xFFFu;
  const uint32_t low = sum & kPhysicalMask;
  const uint32_t carry = (high + 512u) & 0x1000u;
  return (low + carry) + 0xC0000000u;
}

bool Finish(const uint8_t* guest, size_t guest_bytes, uint32_t sum, uint32_t byte_count,
            bool clear_low_bit, ShaderMicrocode& out, std::vector<uint8_t>& bytes,
            std::string& reason) {
  if (byte_count == 0 || (byte_count & 3u) != 0) {
    reason = "shader microcode size is not a whole number of dwords";
    return false;
  }
  if (clear_low_bit) {
    sum &= ~1u;
  }
  const uint32_t guest_address = GuestAddress(sum);
  const uint32_t physical = guest_address & kPhysicalMask;
  if (static_cast<uint64_t>(physical) + byte_count > guest_bytes) {
    reason = "shader microcode is outside guest memory";
    return false;
  }
  out.guest_address = guest_address;
  out.byte_count = byte_count;
  bytes.assign(guest + physical, guest + physical + byte_count);
  return true;
}

}  // namespace

bool ReadVertexShaderMicrocode(const uint8_t* guest, size_t guest_bytes, uint32_t shader,
                               ShaderMicrocode& out, std::vector<uint8_t>& bytes,
                               std::string& reason) {
  uint32_t flags = 0;
  uint32_t base = 0;
  if (!LoadBe(guest, guest_bytes, shader + 872u, flags, reason) ||
      !LoadBe(guest, guest_bytes, shader + 32u, base, reason)) {
    return false;
  }
  const uint32_t table = (flags & 0x20u) != 0 ? 904u : 896u;
  uint32_t inner_offset = 0;
  if (!LoadBe(guest, guest_bytes, shader + table, inner_offset, reason)) {
    return false;
  }
  const uint32_t inner = shader + inner_offset;
  uint32_t address_part = 0;
  uint32_t byte_count = 0;
  if (!LoadBe(guest, guest_bytes, inner + 872u, address_part, reason) ||
      !LoadBe(guest, guest_bytes, inner + 876u, byte_count, reason)) {
    return false;
  }
  return Finish(guest, guest_bytes, address_part + base, byte_count, false, out, bytes, reason);
}

bool ReadPixelShaderMicrocode(const uint8_t* guest, size_t guest_bytes, uint32_t shader,
                              ShaderMicrocode& out, std::vector<uint8_t>& bytes,
                              std::string& reason) {
  uint32_t base = 0;
  uint32_t inner_offset = 0;
  if (!LoadBe(guest, guest_bytes, shader + 24u, base, reason) ||
      !LoadBe(guest, guest_bytes, shader + 64u, inner_offset, reason)) {
    return false;
  }
  const uint32_t inner = shader + inner_offset;
  uint32_t address_part = 0;
  uint32_t byte_count = 0;
  if (!LoadBe(guest, guest_bytes, inner + 40u, address_part, reason) ||
      !LoadBe(guest, guest_bytes, inner + 44u, byte_count, reason)) {
    return false;
  }
  return Finish(guest, guest_bytes, address_part + base, byte_count, true, out, bytes, reason);
}

}  // namespace broken_bond::gfx
