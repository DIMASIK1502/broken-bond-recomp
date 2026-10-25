#include "host/device_repack.h"

#include <cstring>
#include <fstream>

namespace broken_bond::host {
namespace {

constexpr uint32_t kPhysicalMask = 0x1FFFFFFFu;
constexpr uint32_t kDeviceBytes = 0x3800u;
constexpr uint32_t kDeclAt = 0x4000u;
constexpr uint32_t kVsAt = 0x4800u;
constexpr uint32_t kVsBytes = 0x800u;
constexpr uint32_t kPsAt = 0x5000u;
constexpr uint32_t kPsBytes = 0x200u;
constexpr uint32_t kPayloadAt = 0x6000u;
constexpr uint32_t kMaxBlob = 1u << 20;
constexpr uint32_t kMaxStream = 8u << 20;

bool ReadExact(const GuestBytes& live, uint32_t address, void* dst, size_t bytes,
               std::string& reason) {
  if (bytes == 0) {
    return true;
  }
  if (!live.Read(address, dst, bytes)) {
    reason = "device draw range is outside guest memory";
    return false;
  }
  return true;
}

bool LoadBe(const GuestBytes& live, uint32_t address, uint32_t& out, std::string& reason) {
  uint8_t bytes[4];
  if (!ReadExact(live, address, bytes, 4, reason)) {
    return false;
  }
  out = (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
        uint32_t(bytes[3]);
  return true;
}

void StoreBe(std::vector<uint8_t>& image, uint32_t physical, uint32_t value) {
  image[physical] = static_cast<uint8_t>(value >> 24);
  image[physical + 1] = static_cast<uint8_t>(value >> 16);
  image[physical + 2] = static_cast<uint8_t>(value >> 8);
  image[physical + 3] = static_cast<uint8_t>(value);
}

uint32_t GuestAddress(uint32_t sum) {
  const uint32_t rotated = (sum << 12) | (sum >> 20);
  const uint32_t high = rotated & 0xFFFu;
  const uint32_t low = sum & kPhysicalMask;
  const uint32_t carry = (high + 512u) & 0x1000u;
  return (low + carry) + 0xC0000000u;
}

bool Microcode(const GuestBytes& live, uint32_t shader, uint32_t base_at, uint32_t table_at,
               uint32_t header_address_at, uint32_t header_size_at, bool clear_low_bit,
               uint32_t& guest_address, uint32_t& byte_count, std::string& reason) {
  uint32_t base = 0;
  uint32_t inner_offset = 0;
  if (!LoadBe(live, shader + base_at, base, reason) ||
      !LoadBe(live, shader + table_at, inner_offset, reason)) {
    return false;
  }
  const uint32_t inner = shader + inner_offset;
  uint32_t address_part = 0;
  if (!LoadBe(live, inner + header_address_at, address_part, reason) ||
      !LoadBe(live, inner + header_size_at, byte_count, reason)) {
    return false;
  }
  if (byte_count == 0 || (byte_count & 3u) != 0 || byte_count > kMaxBlob) {
    reason = "shader microcode size is not a usable dword count";
    return false;
  }
  uint32_t sum = address_part + base;
  if (clear_low_bit) {
    sum &= ~1u;
  }
  guest_address = GuestAddress(sum);
  return true;
}

}  // namespace

bool RepackDeviceDraw(const GuestBytes& live, uint32_t device, std::vector<uint8_t>& image,
                      uint32_t& image_device, std::string& reason, const DrawSpan& span) {
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t declaration = 0;
  uint32_t stream_packed = 0;
  uint32_t stream_bytes = 0;
  if (!LoadBe(live, device + 12696u, vertex_shader, reason) ||
      !LoadBe(live, device + 12692u, pixel_shader, reason) ||
      !LoadBe(live, device + 11820u, declaration, reason) ||
      !LoadBe(live, device + 1912u, stream_packed, reason)) {
    return false;
  }
  if (vertex_shader == 0 || pixel_shader == 0) {
    reason = "device draw is missing a shader";
    return false;
  }
  uint64_t stream_skip = 0;
  if (span.vertex_count == 0) {
    // device+1916 is the buffer end minus the offset. On this title that is
    // the arena leftover, hundreds of megabytes, not the indices of the draw.
    if (!LoadBe(live, device + 1916u, stream_bytes, reason)) {
      return false;
    }
  } else {
    uint8_t stride_dwords = 0;
    if (!ReadExact(live, device + 12528u, &stride_dwords, 1, reason)) {
      return false;
    }
    if (stride_dwords == 0) {
      reason = "vertex stride is 0";
      return false;
    }
    const uint64_t stride_bytes = uint64_t(stride_dwords) * 4u;
    const uint64_t bytes = uint64_t(span.vertex_count) * stride_bytes;
    stream_skip = uint64_t(span.vertex_start) * stride_bytes;
    if (bytes == 0 || bytes > kMaxStream || stream_skip > 0xFFFFFFFFull) {
      reason = "vertex stream is " + std::to_string(bytes) + " bytes";
      return false;
    }
    stream_bytes = static_cast<uint32_t>(bytes);
  }
  // An oversized value is rejected before the copy.
  if (stream_bytes == 0 || stream_bytes > kMaxStream) {
    reason = "vertex stream is " + std::to_string(stream_bytes) + " bytes";
    return false;
  }
  if (span.index_bytes > kMaxStream || (span.index_bytes != 0 && span.index_size != 2u &&
                                        span.index_size != 4u)) {
    reason = "index list is " + std::to_string(span.index_bytes) + " bytes";
    return false;
  }
  const uint64_t stream_address = uint64_t(stream_packed) + 0xC0000000ull + stream_skip;
  if (stream_address > 0xFFFFFFFFull) {
    reason = "vertex stream is outside guest memory";
    return false;
  }
  uint32_t decl_count = 0;
  if (declaration != 0 && !LoadBe(live, declaration + 24u, decl_count, reason)) {
    return false;
  }
  if (decl_count > 32u) {
    reason = "vertex declaration has more than 32 elements";
    return false;
  }
  uint32_t vertex_flags = 0;
  if (!LoadBe(live, vertex_shader + 872u, vertex_flags, reason)) {
    return false;
  }
  const uint32_t vertex_table = (vertex_flags & 0x20u) != 0 ? 904u : 896u;
  uint32_t vertex_ucode = 0;
  uint32_t vertex_ucode_bytes = 0;
  uint32_t pixel_ucode = 0;
  uint32_t pixel_ucode_bytes = 0;
  if (!Microcode(live, vertex_shader, 32u, vertex_table, 872u, 876u, false, vertex_ucode,
                 vertex_ucode_bytes, reason) ||
      !Microcode(live, pixel_shader, 24u, 64u, 40u, 44u, true, pixel_ucode, pixel_ucode_bytes,
                 reason)) {
    return false;
  }

  const uint32_t decl_bytes = declaration == 0 ? 0u : 52u + decl_count * 12u;
  const uint32_t pixel_ucode_at = (kPayloadAt + vertex_ucode_bytes + 15u) & ~15u;
  const uint32_t stream_at = (pixel_ucode_at + pixel_ucode_bytes + 15u) & ~15u;
  const uint32_t index_at = (stream_at + stream_bytes + 15u) & ~15u;
  const uint32_t image_bytes = span.index_bytes == 0 ? stream_at + stream_bytes : index_at + span.index_bytes;
  image.assign(image_bytes, 0);
  if (!ReadExact(live, device, image.data(), kDeviceBytes, reason) ||
      (decl_bytes != 0 &&
       !ReadExact(live, declaration, image.data() + kDeclAt, decl_bytes, reason)) ||
      !ReadExact(live, vertex_shader, image.data() + kVsAt, kVsBytes, reason) ||
      !ReadExact(live, pixel_shader, image.data() + kPsAt, kPsBytes, reason) ||
      !ReadExact(live, vertex_ucode, image.data() + kPayloadAt, vertex_ucode_bytes, reason) ||
      !ReadExact(live, pixel_ucode, image.data() + pixel_ucode_at, pixel_ucode_bytes, reason) ||
      !ReadExact(live, static_cast<uint32_t>(stream_address), image.data() + stream_at, stream_bytes,
                 reason) ||
      (span.index_bytes != 0 &&
       !ReadExact(live, span.index_address, image.data() + index_at, span.index_bytes, reason))) {
    return false;
  }

  StoreBe(image, 12696u, kVsAt + 0xC0000000u);
  StoreBe(image, 12692u, kPsAt + 0xC0000000u);
  StoreBe(image, 11820u, declaration == 0 ? 0u : kDeclAt + 0xC0000000u);
  StoreBe(image, 1912u, stream_at);
  StoreBe(image, 1916u, stream_bytes);
  StoreBe(image, 12436u, span.index_bytes == 0 ? 0u : index_at);
  StoreBe(image, 12440u, span.index_bytes);
  StoreBe(image, 12444u, span.index_size);
  StoreBe(image, 12448u, span.index_first);
  StoreBe(image, 12452u, span.base_vertex);

  StoreBe(image, kVsAt + 32u, 0);
  StoreBe(image, kVsAt + vertex_table, 0x400u);
  StoreBe(image, kVsAt + 0x400u + 872u, kPayloadAt);
  StoreBe(image, kVsAt + 0x400u + 876u, vertex_ucode_bytes);

  StoreBe(image, kPsAt + 24u, 0);
  StoreBe(image, kPsAt + 64u, 0x80u);
  StoreBe(image, kPsAt + 0x80u + 40u, pixel_ucode_at);
  StoreBe(image, kPsAt + 0x80u + 44u, pixel_ucode_bytes);

  image_device = 0xC0000000u;
  return true;
}

bool WriteDeviceDrawFile(const std::string& path, const std::vector<uint8_t>& image,
                         uint32_t image_device, std::string& reason) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  const uint32_t magic = 0x31445242u;
  const uint32_t bytes = static_cast<uint32_t>(image.size());
  if (!out || bytes != image.size()) {
    reason = "device draw file was not opened";
    return false;
  }
  out.write(reinterpret_cast<const char*>(&magic), 4);
  out.write(reinterpret_cast<const char*>(&image_device), 4);
  out.write(reinterpret_cast<const char*>(&bytes), 4);
  if (!image.empty()) {
    out.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
  }
  if (!out) {
    reason = "device draw file was not written";
    return false;
  }
  return true;
}

bool ReadDeviceDrawFile(const std::string& path, std::vector<uint8_t>& image, uint32_t& image_device,
                        std::string& reason) {
  std::ifstream in(path, std::ios::binary);
  uint32_t magic = 0;
  uint32_t bytes = 0;
  if (!in || !in.read(reinterpret_cast<char*>(&magic), 4) ||
      !in.read(reinterpret_cast<char*>(&image_device), 4) ||
      !in.read(reinterpret_cast<char*>(&bytes), 4) || magic != 0x31445242u || bytes > (20u << 20)) {
    reason = "device draw file is not a snapshot";
    return false;
  }
  image.resize(bytes);
  if (bytes != 0 && !in.read(reinterpret_cast<char*>(image.data()), static_cast<std::streamsize>(bytes))) {
    reason = "device draw file is short";
    return false;
  }
  return true;
}

uint32_t DeviceDrawStreamBytes(const std::vector<uint8_t>& image) {
  if (image.size() < 1920u) {
    return 0;
  }
  const uint8_t* bytes = image.data() + 1916u;
  return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
         uint32_t(bytes[3]);
}

bool ReadDeviceDrawIndices(const std::vector<uint8_t>& image, DeviceDrawIndexInfo& out) {
  out = {};
  if (image.size() < 12456u) {
    return false;
  }
  auto load = [&](uint32_t at) {
    const uint8_t* bytes = image.data() + at;
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
           uint32_t(bytes[3]);
  };
  out.offset = load(12436u);
  out.bytes = load(12440u);
  out.size = load(12444u);
  out.first = load(12448u);
  out.base_vertex = load(12452u);
  if (out.bytes != 0 &&
      (out.offset > image.size() || out.bytes > image.size() - out.offset)) {
    out = {};
    return false;
  }
  return true;
}

bool IndexRange(const uint8_t* indices, uint32_t byte_count, uint32_t index_size, uint32_t& first,
                uint32_t& count, std::string& reason) {
  if (indices == nullptr || (index_size != 2u && index_size != 4u) || byte_count == 0 ||
      (byte_count % index_size) != 0) {
    reason = "index list is " + std::to_string(byte_count) + " bytes";
    return false;
  }
  const uint32_t restart = index_size == 2u ? 0xFFFFu : 0xFFFFFFFFu;
  uint32_t lowest = 0xFFFFFFFFu;
  uint32_t highest = 0;
  bool any = false;
  for (uint32_t at = 0; at < byte_count; at += index_size) {
    uint32_t index = 0;
    if (index_size == 2u) {
      index = (uint32_t(indices[at]) << 8) | uint32_t(indices[at + 1]);
    } else {
      index = (uint32_t(indices[at]) << 24) | (uint32_t(indices[at + 1]) << 16) |
              (uint32_t(indices[at + 2]) << 8) | uint32_t(indices[at + 3]);
    }
    if (index == restart) {
      continue;
    }
    any = true;
    if (index < lowest) {
      lowest = index;
    }
    if (index > highest) {
      highest = index;
    }
  }
  if (!any || uint64_t(highest) - lowest + 1u > 0xFFFFFFFFull) {
    reason = "index list has no vertices";
    return false;
  }
  first = lowest;
  count = highest - lowest + 1u;
  return true;
}

}  // namespace broken_bond::host
