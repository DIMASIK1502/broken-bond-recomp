#include "gfx/device_shadow.h"

#include <cstring>

namespace broken_bond::gfx {
namespace {

constexpr uint32_t kPhysicalMask = 0x1FFFFFFFu;
constexpr uint32_t kStream0Fetch = 95u;
constexpr uint32_t kFetchBytes = 8u;

bool LoadBe(const uint8_t* guest, size_t guest_bytes, uint32_t address, uint32_t& out,
            std::string& reason) {
  const uint32_t physical = address & kPhysicalMask;
  if (guest == nullptr || static_cast<uint64_t>(physical) + 4u > guest_bytes) {
    reason = "device shadow dword is outside guest memory";
    return false;
  }
  const uint8_t* bytes = guest + physical;
  out = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
        uint32_t(bytes[3]);
  return true;
}

bool LoadByte(const uint8_t* guest, size_t guest_bytes, uint32_t address, uint32_t& out,
              std::string& reason) {
  const uint32_t physical = address & kPhysicalMask;
  if (guest == nullptr || static_cast<uint64_t>(physical) + 1u > guest_bytes) {
    reason = "device shadow byte is outside guest memory";
    return false;
  }
  out = guest[physical];
  return true;
}

bool LoadBe16(const uint8_t* guest, size_t guest_bytes, uint32_t address, uint32_t& out,
              std::string& reason) {
  const uint32_t physical = address & kPhysicalMask;
  if (guest == nullptr || static_cast<uint64_t>(physical) + 2u > guest_bytes) {
    reason = "declaration halfword is outside guest memory";
    return false;
  }
  out = (uint32_t(guest[physical]) << 8) | uint32_t(guest[physical + 1]);
  return true;
}

}  // namespace

bool ReadDeviceDraw(const uint8_t* guest, size_t guest_bytes, uint32_t device, DeviceDraw& out,
                    std::vector<uint8_t>& stream, std::string& reason) {
  if (!LoadBe(guest, guest_bytes, device + 12696u, out.vertex_shader, reason) ||
      !LoadBe(guest, guest_bytes, device + 12692u, out.pixel_shader, reason) ||
      !LoadBe(guest, guest_bytes, device + 10368u, out.surface_info, reason) ||
      !LoadBe(guest, guest_bytes, device + 10548u, out.depth_control, reason) ||
      !LoadBe(guest, guest_bytes, device + 10552u, out.blend_control, reason) ||
      !LoadBe(guest, guest_bytes, device + 10568u, out.mode_cntl, reason) ||
      !LoadByte(guest, guest_bytes, device + 12528u, out.stream_stride_dwords, reason)) {
    return false;
  }
  uint32_t packed = 0;
  const uint32_t fetch = device + 1152u + kStream0Fetch * kFetchBytes;
  if (!LoadBe(guest, guest_bytes, fetch, packed, reason) ||
      !LoadBe(guest, guest_bytes, fetch + 4u, out.stream_bytes, reason)) {
    return false;
  }
  out.stream_address = packed + 0xC0000000u;
  if (out.stream_bytes == 0) {
    stream.clear();
    return true;
  }
  const uint32_t physical = out.stream_address & kPhysicalMask;
  if (static_cast<uint64_t>(physical) + out.stream_bytes > guest_bytes) {
    reason = "vertex stream is outside guest memory";
    return false;
  }
  stream.assign(guest + physical, guest + physical + out.stream_bytes);
  return true;
}

bool ReadDeviceDeclaration(const uint8_t* guest, size_t guest_bytes, uint32_t device,
                           std::vector<VertexElement>& elements, std::string& reason) {
  uint32_t declaration = 0;
  if (!LoadBe(guest, guest_bytes, device + 11820u, declaration, reason)) {
    return false;
  }
  elements.clear();
  if (declaration == 0) {
    return true;
  }
  uint32_t count = 0;
  if (!LoadBe(guest, guest_bytes, declaration + 24u, count, reason)) {
    return false;
  }
  if (count > 32u) {
    reason = "vertex declaration has more than 32 elements";
    return false;
  }
  elements.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t element = declaration + 52u + i * 12u;
    uint32_t stream = 0;
    uint32_t byte_offset = 0;
    uint32_t format = 0;
    uint32_t usage = 0;
    uint32_t usage_index = 0;
    uint32_t stride = 0;
    if (!LoadBe16(guest, guest_bytes, element, stream, reason) ||
        !LoadBe16(guest, guest_bytes, element + 2u, byte_offset, reason) ||
        !LoadBe(guest, guest_bytes, element + 4u, format, reason) ||
        !LoadByte(guest, guest_bytes, element + 9u, usage, reason) ||
        !LoadByte(guest, guest_bytes, element + 10u, usage_index, reason)) {
      return false;
    }
    if (stream > 95u) {
      reason = "vertex declaration stream is above fetch constant 0";
      return false;
    }
    if (!LoadByte(guest, guest_bytes, device + 12528u + stream, stride, reason)) {
      return false;
    }
    VertexElement out;
    out.usage = usage;
    out.usage_index = usage_index;
    out.format = format & 0x3Fu;
    out.offset_words = static_cast<int32_t>(byte_offset >> 2);
    out.stride_words = stride;
    out.signed_components = (format & 0x100u) != 0;
    out.integer_components = (format & 0x200u) != 0;
    elements.push_back(out);
  }
  return true;
}

bool ReadDeviceFloats(const uint8_t* guest, size_t guest_bytes, uint32_t device,
                      std::vector<float>& floats, std::string& reason) {
  constexpr uint32_t kFloat4s = 512u;
  floats.resize(kFloat4s * 4u);
  for (uint32_t i = 0; i < kFloat4s * 4u; ++i) {
    uint32_t bits = 0;
    if (!LoadBe(guest, guest_bytes, device + 1920u + i * 4u, bits, reason)) {
      return false;
    }
    float value = 0.f;
    std::memcpy(&value, &bits, sizeof(value));
    floats[i] = value;
  }
  return true;
}

bool ReadDeviceTexture(const uint8_t* guest, size_t guest_bytes, uint32_t device, uint32_t slot,
                       TextureFetch& out, std::string& reason) {
  if (slot >= 32u) {
    reason = "texture slot is above 31";
    return false;
  }
  uint32_t words[6] = {};
  const uint32_t fetch = device + 1152u + slot * 24u;
  for (uint32_t i = 0; i < 6u; ++i) {
    if (!LoadBe(guest, guest_bytes, fetch + i * 4u, words[i], reason)) {
      return false;
    }
  }
  return ReadTextureFetch(words, out, reason);
}

}  // namespace broken_bond::gfx
