#include "gfx/device_shadow.h"

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

}  // namespace broken_bond::gfx
