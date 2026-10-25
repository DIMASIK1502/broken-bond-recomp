#include "host/device_repack.h"

#include <cstring>
#include <fstream>

#include <rex/graphics/xenos.h>

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
constexpr uint32_t kMaxTexture = 1u << 20;

struct LevelLayout {
  uint32_t block = 1;
  uint32_t bytes_per_block = 4;
  uint32_t log2 = 2;
};

bool LevelLayoutFor(uint32_t format, LevelLayout& out) {
  if (format == static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_8_8_8_8)) {
    out = {1u, 4u, 2u};
    return true;
  }
  if (format == static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT1)) {
    out = {4u, 8u, 3u};
    return true;
  }
  return false;
}

// Same 2D address as rex::graphics::texture_util::GetTiledOffset2D. The game
// does not link the host texture library, so the copy length lives here.
int32_t TiledOffset2D(int32_t x, int32_t y, uint32_t pitch, uint32_t bytes_per_block_log2) {
  const int32_t macro =
      ((x >> 5) + (y >> 5) * int32_t(pitch >> 5)) << (int32_t(bytes_per_block_log2) + 7);
  const int32_t micro = ((x & 7) + ((y & 0xE) << 2)) << int32_t(bytes_per_block_log2);
  const int32_t offset = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

uint32_t TiledBound2D(uint32_t right, uint32_t bottom, uint32_t pitch, uint32_t log2) {
  if (right == 0 || bottom == 0) {
    return 0;
  }
  uint32_t upper = static_cast<uint32_t>(TiledOffset2D(int32_t((right - 1u) & ~31u),
                                                       int32_t((bottom - 1u) & ~31u), pitch, log2));
  if (log2 == 0) {
    upper += 0xA00u;
  } else if (log2 == 1) {
    upper += 0xC00u;
  } else {
    upper += 0x400u << log2;
  }
  return upper;
}

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

uint32_t TextureCopyBytes(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                          bool tiled) {
  LevelLayout level;
  if (!LevelLayoutFor(format, level) || width == 0 || height == 0 || pitch < width ||
      (pitch % level.block) != 0) {
    return 0;
  }
  const uint32_t pitch_blocks = pitch / level.block;
  const uint32_t y_blocks = (height + level.block - 1u) / level.block;
  const uint32_t x_blocks = (width + level.block - 1u) / level.block;
  if (!tiled) {
    const uint64_t bytes = uint64_t(pitch_blocks) * y_blocks * level.bytes_per_block;
    return bytes > 0xFFFFFFFFull ? 0u : static_cast<uint32_t>(bytes);
  }
  const uint64_t bytes =
      uint64_t(TiledBound2D(x_blocks, y_blocks, pitch_blocks, level.log2)) + level.bytes_per_block;
  return bytes > 0xFFFFFFFFull ? 0u : static_cast<uint32_t>(bytes);
}

struct SlotTexture {
  uint32_t guest = 0;
  uint32_t bytes = 0;
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;
  uint32_t endian = 0;
  uint32_t tiled = 0;
};

bool DescribeSlot0(const GuestBytes& live, uint32_t device, SlotTexture& out, std::string& reason) {
  out = {};
  uint32_t words[6] = {};
  for (uint32_t i = 0; i < 6u; ++i) {
    if (!LoadBe(live, device + 1152u + i * 4u, words[i], reason)) {
      return false;
    }
  }
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, words, sizeof(fetch));
  if (fetch.type != rex::graphics::xenos::FetchConstantType::kTexture) {
    return true;
  }
  if (fetch.dimension != rex::graphics::xenos::DataDimension::k2DOrStacked) {
    reason = "texture is not 2D";
    return false;
  }
  out.format = static_cast<uint32_t>(fetch.format);
  out.endian = static_cast<uint32_t>(fetch.endianness);
  out.tiled = fetch.tiled != 0 ? 1u : 0u;
  out.pitch = fetch.pitch << 5;
  out.width = fetch.size_2d.width + 1;
  out.height = fetch.size_2d.height + 1;
  out.bytes = TextureCopyBytes(out.format, out.width, out.height, out.pitch, out.tiled != 0);
  if (out.bytes == 0 || out.bytes > kMaxTexture) {
    reason = "texture is " + std::to_string(out.bytes) + " bytes";
    return false;
  }
  const uint64_t guest = (uint64_t(fetch.base_address) << 12) + 0xC0000000ull;
  if (guest > 0xFFFFFFFFull) {
    reason = "texture is outside guest memory";
    return false;
  }
  out.guest = static_cast<uint32_t>(guest);
  return true;
}

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

  SlotTexture texture;
  if (!DescribeSlot0(live, device, texture, reason)) {
    return false;
  }
  const uint32_t decl_bytes = declaration == 0 ? 0u : 52u + decl_count * 12u;
  const uint32_t pixel_ucode_at = (kPayloadAt + vertex_ucode_bytes + 15u) & ~15u;
  const uint32_t stream_at = (pixel_ucode_at + pixel_ucode_bytes + 15u) & ~15u;
  const uint32_t index_at = (stream_at + stream_bytes + 15u) & ~15u;
  const uint32_t tail = span.index_bytes == 0 ? stream_at + stream_bytes : index_at + span.index_bytes;
  const uint32_t texture_at = (tail + 15u) & ~15u;
  const uint32_t image_bytes = texture.bytes == 0 ? tail : texture_at + texture.bytes;
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
  if (texture.bytes != 0 &&
      !live.Read(texture.guest, image.data() + texture_at, texture.bytes)) {
    reason = "texture is outside guest memory";
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
  StoreBe(image, 12456u, texture.bytes == 0 ? 0u : texture_at);
  StoreBe(image, 12460u, texture.bytes);
  StoreBe(image, 12464u, texture.format);
  StoreBe(image, 12468u, texture.width);
  StoreBe(image, 12472u, texture.height);
  StoreBe(image, 12476u, texture.pitch);
  StoreBe(image, 12480u, texture.endian);
  StoreBe(image, 12484u, texture.tiled);

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

bool ReadDeviceDrawTexture(const std::vector<uint8_t>& image, DeviceDrawTexture& out) {
  out = {};
  if (image.size() < 12488u) {
    return false;
  }
  auto load = [&](uint32_t at) {
    const uint8_t* bytes = image.data() + at;
    return (uint32_t(bytes[0]) << 24) | (uint32_t(bytes[1]) << 16) | (uint32_t(bytes[2]) << 8) |
           uint32_t(bytes[3]);
  };
  out.offset = load(12456u);
  out.bytes = load(12460u);
  out.format = load(12464u);
  out.width = load(12468u);
  out.height = load(12472u);
  out.pitch = load(12476u);
  out.endian = load(12480u);
  out.tiled = load(12484u);
  const bool tail = out.bytes != 0 && out.offset <= image.size() &&
                    out.bytes <= image.size() - out.offset && out.offset + out.bytes == image.size();
  const bool format = out.format == 6u || out.format == 18u;
  if (!tail || !format) {
    out = {};
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
