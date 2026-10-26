#include "gfx/texture.h"

#include <cstring>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>

namespace broken_bond::gfx {

bool RoundTripTiledRgba8(const uint8_t* linear, uint32_t pitch, uint32_t height,
                         std::string& reason) {
  if (linear == nullptr || pitch == 0 || height == 0) {
    reason = "empty texture";
    return false;
  }
  const rex::graphics::FormatInfo* format = rex::graphics::FormatInfo::Get(
      static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_8_8_8_8));
  if (format == nullptr || format->bytes_per_block() != 4) {
    reason = "k_8_8_8_8 is not a 4-byte block";
    return false;
  }
  constexpr uint32_t kBytes = 4;
  constexpr uint32_t kLog2 = 2;
  const uint32_t bound = rex::graphics::texture_util::GetTiledAddressUpperBound2D(pitch, height,
                                                                                  pitch, kLog2);
  std::vector<uint8_t> tiled(static_cast<size_t>(bound) + kBytes);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < pitch; ++x) {
      const uint32_t address = static_cast<uint32_t>(
          rex::graphics::texture_util::GetTiledOffset2D(int32_t(x), int32_t(y), pitch, kLog2));
      if (static_cast<size_t>(address) + kBytes > tiled.size()) {
        reason = "tiled address is outside the upper bound";
        return false;
      }
      std::memcpy(tiled.data() + address, linear + (static_cast<size_t>(y) * pitch + x) * kBytes,
                  kBytes);
    }
  }

  std::vector<uint8_t> restored(static_cast<size_t>(pitch) * height * kBytes);
  rex::graphics::texture_conversion::UntileInfo info{};
  info.width = pitch;
  info.height = height;
  info.input_pitch = pitch;
  info.output_pitch = pitch;
  info.input_format_info = format;
  info.output_format_info = format;
  info.copy_callback = [](void* output, const void* input, size_t length) {
    std::memcpy(output, input, length);
  };
  rex::graphics::texture_conversion::Untile(restored.data(), tiled.data(), &info);
  if (restored.size() != static_cast<size_t>(pitch) * height * kBytes ||
      std::memcmp(restored.data(), linear, restored.size()) != 0) {
    reason = "untile did not restore the RGBA8 image";
    return false;
  }
  return true;
}

namespace {

constexpr uint32_t kRgba8 = static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_8_8_8_8);

const rex::graphics::FormatInfo* Rgba8Format(std::string& reason) {
  const rex::graphics::FormatInfo* format = rex::graphics::FormatInfo::Get(kRgba8);
  if (format == nullptr || format->bytes_per_block() != 4) {
    reason = "k_8_8_8_8 is not a 4-byte block";
    return nullptr;
  }
  return format;
}

uint32_t SwapTexel(uint32_t value, uint32_t endian) {
  return rex::graphics::xenos::GpuSwap(value, static_cast<rex::graphics::xenos::Endian>(endian));
}

bool UsableFetch(const TextureFetch& fetch, std::string& reason) {
  if (fetch.format != kRgba8) {
    reason = "fetch constant format is not k_8_8_8_8";
    return false;
  }
  if (fetch.endian > 3) {
    reason = "fetch constant endian is out of range";
    return false;
  }
  if (fetch.width == 0 || fetch.height == 0 || fetch.pitch < fetch.width ||
      (fetch.pitch & 31u) != 0) {
    reason = "fetch constant pitch does not cover the image";
    return false;
  }
  return true;
}

}  // namespace

bool ReadTextureFetch(const uint32_t words[6], TextureFetch& out, std::string& reason) {
  if (words == nullptr) {
    reason = "missing fetch constant";
    return false;
  }
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  std::memcpy(&fetch, words, sizeof(fetch));
  if (fetch.type != rex::graphics::xenos::FetchConstantType::kTexture ||
      fetch.dimension != rex::graphics::xenos::DataDimension::k2DOrStacked) {
    reason = "fetch constant is not a 2D texture";
    return false;
  }
  out.format = static_cast<uint32_t>(fetch.format);
  out.endian = static_cast<uint32_t>(fetch.endianness);
  out.tiled = fetch.tiled != 0;
  out.pitch = fetch.pitch << 5;
  out.width = fetch.size_2d.width + 1;
  out.height = fetch.size_2d.height + 1;
  out.mip_min = fetch.mip_min_level;
  out.mip_max = fetch.mip_max_level;
  out.guest_address = fetch.base_address << 12;
  return UsableFetch(out, reason);
}

bool WriteRgba8Fetch(uint32_t words[6], uint32_t width, uint32_t height, uint32_t pitch, bool tiled,
                     uint32_t endian, std::string& reason) {
  if (words == nullptr) {
    reason = "missing fetch constant";
    return false;
  }
  TextureFetch check;
  check.format = kRgba8;
  check.endian = endian;
  check.width = width;
  check.height = height;
  check.pitch = pitch;
  if (!UsableFetch(check, reason)) {
    return false;
  }
  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  fetch.type = rex::graphics::xenos::FetchConstantType::kTexture;
  fetch.format = rex::graphics::xenos::TextureFormat::k_8_8_8_8;
  fetch.endianness = static_cast<rex::graphics::xenos::Endian>(endian);
  fetch.pitch = pitch >> 5;
  fetch.tiled = tiled ? 1u : 0u;
  fetch.size_2d.width = width - 1;
  fetch.size_2d.height = height - 1;
  fetch.dimension = rex::graphics::xenos::DataDimension::k2DOrStacked;
  std::memcpy(words, &fetch, sizeof(fetch));
  return true;
}

bool StoreFetchRgba8(const uint32_t words[6], const uint8_t* linear, std::vector<uint8_t>& guest,
                     std::string& reason) {
  TextureFetch fetch;
  if (!ReadTextureFetch(words, fetch, reason)) {
    return false;
  }
  const rex::graphics::FormatInfo* format = Rgba8Format(reason);
  if (linear == nullptr || format == nullptr) {
    if (linear == nullptr) {
      reason = "empty texture";
    }
    return false;
  }
  std::vector<uint8_t> rows(static_cast<size_t>(fetch.pitch) * fetch.height * 4u);
  for (uint32_t y = 0; y < fetch.height; ++y) {
    for (uint32_t x = 0; x < fetch.width; ++x) {
      uint32_t texel = 0;
      std::memcpy(&texel, linear + (static_cast<size_t>(y) * fetch.width + x) * 4u, 4);
      texel = SwapTexel(texel, fetch.endian);
      std::memcpy(rows.data() + (static_cast<size_t>(y) * fetch.pitch + x) * 4u, &texel, 4);
    }
  }
  if (!fetch.tiled) {
    guest = std::move(rows);
    return true;
  }
  const uint32_t bound = rex::graphics::texture_util::GetTiledAddressUpperBound2D(
      fetch.pitch, fetch.height, fetch.pitch, 2);
  guest.assign(static_cast<size_t>(bound) + 4u, 0);
  for (uint32_t y = 0; y < fetch.height; ++y) {
    for (uint32_t x = 0; x < fetch.pitch; ++x) {
      const uint32_t address = static_cast<uint32_t>(rex::graphics::texture_util::GetTiledOffset2D(
          int32_t(x), int32_t(y), fetch.pitch, 2));
      if (static_cast<size_t>(address) + 4u > guest.size()) {
        reason = "tiled address is outside the upper bound";
        return false;
      }
      std::memcpy(guest.data() + address, rows.data() + (static_cast<size_t>(y) * fetch.pitch + x) * 4u,
                  4);
    }
  }
  return true;
}

bool LoadFetchRgba8(const uint32_t words[6], const uint8_t* guest, size_t guest_bytes,
                    std::vector<uint8_t>& linear, std::string& reason) {
  TextureFetch fetch;
  if (!ReadTextureFetch(words, fetch, reason)) {
    return false;
  }
  const rex::graphics::FormatInfo* format = Rgba8Format(reason);
  if (guest == nullptr || format == nullptr) {
    if (guest == nullptr) {
      reason = "empty texture";
    }
    return false;
  }
  linear.assign(static_cast<size_t>(fetch.width) * fetch.height * 4u, 0);
  if (!fetch.tiled) {
    const size_t rows = static_cast<size_t>(fetch.pitch) * fetch.height * 4u;
    if (guest_bytes < rows) {
      reason = "linear texture is shorter than its pitch";
      return false;
    }
    for (uint32_t y = 0; y < fetch.height; ++y) {
      for (uint32_t x = 0; x < fetch.width; ++x) {
        uint32_t texel = 0;
        std::memcpy(&texel, guest + (static_cast<size_t>(y) * fetch.pitch + x) * 4u, 4);
        texel = SwapTexel(texel, fetch.endian);
        std::memcpy(linear.data() + (static_cast<size_t>(y) * fetch.width + x) * 4u, &texel, 4);
      }
    }
    return true;
  }
  const uint32_t bound = rex::graphics::texture_util::GetTiledAddressUpperBound2D(
      fetch.width, fetch.height, fetch.pitch, 2);
  if (guest_bytes < static_cast<size_t>(bound) + 4u) {
    reason = "tiled texture is shorter than its upper bound";
    return false;
  }
  const uint32_t endian = fetch.endian;
  rex::graphics::texture_conversion::UntileInfo info{};
  info.width = fetch.width;
  info.height = fetch.height;
  info.input_pitch = fetch.pitch;
  info.output_pitch = fetch.width;
  info.input_format_info = format;
  info.output_format_info = format;
  info.copy_callback = [endian](void* output, const void* input, size_t length) {
    if (length != 4) {
      std::memcpy(output, input, length);
      return;
    }
    uint32_t texel = 0;
    std::memcpy(&texel, input, 4);
    texel = SwapTexel(texel, endian);
    std::memcpy(output, &texel, 4);
  };
  rex::graphics::texture_conversion::Untile(linear.data(), guest, &info);
  return true;
}

namespace {

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
  if (format == static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT4_5)) {
    out = {4u, 16u, 4u};
    return true;
  }
  return false;
}

void Expand565(uint16_t packed, uint8_t out[4]) {
  const uint32_t red = (packed >> 11) & 31u;
  const uint32_t green = (packed >> 5) & 63u;
  const uint32_t blue = packed & 31u;
  out[0] = static_cast<uint8_t>((red << 3) | (red >> 2));
  out[1] = static_cast<uint8_t>((green << 2) | (green >> 4));
  out[2] = static_cast<uint8_t>((blue << 3) | (blue >> 2));
  out[3] = 255;
}

void DecodeDxt1Block(const uint8_t* block, uint8_t* linear, uint32_t width, uint32_t height,
                     uint32_t x0, uint32_t y0) {
  const uint16_t color0 = uint16_t(block[0] | (uint16_t(block[1]) << 8));
  const uint16_t color1 = uint16_t(block[2] | (uint16_t(block[3]) << 8));
  const uint32_t indices = uint32_t(block[4]) | (uint32_t(block[5]) << 8) |
                           (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
  uint8_t colors[4][4];
  Expand565(color0, colors[0]);
  Expand565(color1, colors[1]);
  if (color0 > color1) {
    for (int channel = 0; channel < 3; ++channel) {
      colors[2][channel] =
          static_cast<uint8_t>((2u * colors[0][channel] + colors[1][channel]) / 3u);
      colors[3][channel] =
          static_cast<uint8_t>((colors[0][channel] + 2u * colors[1][channel]) / 3u);
    }
    colors[2][3] = 255;
    colors[3][3] = 255;
  } else {
    for (int channel = 0; channel < 3; ++channel) {
      colors[2][channel] = static_cast<uint8_t>((colors[0][channel] + colors[1][channel]) / 2u);
      colors[3][channel] = 0;
    }
    colors[2][3] = 255;
    colors[3][3] = 0;
  }
  for (uint32_t py = 0; py < 4u; ++py) {
    for (uint32_t px = 0; px < 4u; ++px) {
      const uint32_t x = x0 + px;
      const uint32_t y = y0 + py;
      if (x >= width || y >= height) {
        continue;
      }
      const uint32_t index = (indices >> ((py * 4u + px) * 2u)) & 3u;
      std::memcpy(linear + (static_cast<size_t>(y) * width + x) * 4u, colors[index], 4);
    }
  }
}

void DecodeDxt5Block(const uint8_t* block, uint8_t* linear, uint32_t width, uint32_t height,
                     uint32_t x0, uint32_t y0) {
  DecodeDxt1Block(block + 8, linear, width, height, x0, y0);
  const uint32_t alpha0 = block[0];
  const uint32_t alpha1 = block[1];
  uint8_t alpha[8];
  alpha[0] = static_cast<uint8_t>(alpha0);
  alpha[1] = static_cast<uint8_t>(alpha1);
  if (alpha0 > alpha1) {
    for (uint32_t i = 1; i <= 6u; ++i) {
      alpha[i + 1] = static_cast<uint8_t>(((7u - i) * alpha0 + i * alpha1) / 7u);
    }
  } else {
    for (uint32_t i = 1; i <= 4u; ++i) {
      alpha[i + 1] = static_cast<uint8_t>(((5u - i) * alpha0 + i * alpha1) / 5u);
    }
    alpha[6] = 0;
    alpha[7] = 255;
  }
  uint64_t indices = 0;
  for (uint32_t i = 0; i < 6u; ++i) {
    indices |= uint64_t(block[2u + i]) << (8u * i);
  }
  for (uint32_t py = 0; py < 4u; ++py) {
    for (uint32_t px = 0; px < 4u; ++px) {
      const uint32_t x = x0 + px;
      const uint32_t y = y0 + py;
      if (x >= width || y >= height) {
        continue;
      }
      const uint32_t index = (indices >> ((py * 4u + px) * 3u)) & 7u;
      linear[(static_cast<size_t>(y) * width + x) * 4u + 3u] = alpha[index];
    }
  }
}

void SwapBlock(uint8_t* block, uint32_t bytes, uint32_t endian) {
  for (uint32_t at = 0; at + 4u <= bytes; at += 4u) {
    uint32_t word = 0;
    std::memcpy(&word, block + at, 4);
    word = rex::graphics::xenos::GpuSwap(word, static_cast<rex::graphics::xenos::Endian>(endian));
    std::memcpy(block + at, &word, 4);
  }
}

}  // namespace

uint32_t GuestTextureBytes(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
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
  const uint32_t bound = rex::graphics::texture_util::GetTiledAddressUpperBound2D(
      x_blocks, y_blocks, pitch_blocks, level.log2);
  const uint64_t bytes = uint64_t(bound) + level.bytes_per_block;
  return bytes > 0xFFFFFFFFull ? 0u : static_cast<uint32_t>(bytes);
}

bool LoadGuestTextureRgba8(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                           uint32_t endian, bool tiled, const uint8_t* guest, size_t guest_bytes,
                           std::vector<uint8_t>& linear, std::string& reason) {
  LevelLayout level;
  if (!LevelLayoutFor(format, level) || endian > 3u || guest == nullptr) {
    reason = guest == nullptr ? "empty texture" : "texture format is not RGBA8, DXT1, or DXT5";
    if (endian > 3u) {
      reason = "texture endian is out of range";
    }
    return false;
  }
  const uint32_t need = GuestTextureBytes(format, width, height, pitch, tiled);
  if (need == 0 || guest_bytes < need) {
    reason = "texture is shorter than its base level";
    return false;
  }
  linear.assign(static_cast<size_t>(width) * height * 4u, 0);
  if (format == static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_8_8_8_8)) {
    uint32_t words[6] = {};
    if (!WriteRgba8Fetch(words, width, height, pitch, tiled, endian, reason)) {
      return false;
    }
    return LoadFetchRgba8(words, guest, guest_bytes, linear, reason);
  }
  const uint32_t pitch_blocks = pitch / level.block;
  const uint32_t width_blocks = (width + level.block - 1u) / level.block;
  const uint32_t height_blocks = (height + level.block - 1u) / level.block;
  std::vector<uint8_t> blocks(static_cast<size_t>(width_blocks) * height_blocks * level.bytes_per_block);
  if (!tiled) {
    for (uint32_t by = 0; by < height_blocks; ++by) {
      std::memcpy(blocks.data() + static_cast<size_t>(by) * width_blocks * level.bytes_per_block,
                  guest + static_cast<size_t>(by) * pitch_blocks * level.bytes_per_block,
                  static_cast<size_t>(width_blocks) * level.bytes_per_block);
    }
  } else {
    const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(format);
    if (info == nullptr || info->bytes_per_block() != level.bytes_per_block) {
      reason = "compressed block size does not match the format";
      return false;
    }
    rex::graphics::texture_conversion::UntileInfo untile{};
    untile.width = width_blocks;
    untile.height = height_blocks;
    untile.input_pitch = pitch_blocks;
    untile.output_pitch = width_blocks;
    untile.input_format_info = info;
    untile.output_format_info = info;
    untile.copy_callback = [](void* output, const void* input, size_t length) {
      std::memcpy(output, input, length);
    };
    rex::graphics::texture_conversion::Untile(blocks.data(), guest, &untile);
  }
  for (uint32_t by = 0; by < height_blocks; ++by) {
    for (uint32_t bx = 0; bx < width_blocks; ++bx) {
      uint8_t* block = blocks.data() + (static_cast<size_t>(by) * width_blocks + bx) * level.bytes_per_block;
      SwapBlock(block, level.bytes_per_block, endian);
      if (format == static_cast<uint32_t>(rex::graphics::xenos::TextureFormat::k_DXT4_5)) {
        DecodeDxt5Block(block, linear.data(), width, height, bx * level.block, by * level.block);
      } else {
        DecodeDxt1Block(block, linear.data(), width, height, bx * level.block, by * level.block);
      }
    }
  }
  return true;
}

namespace {

uint32_t Log2Bytes(uint32_t bytes) {
  uint32_t log2 = 0;
  while ((1u << (log2 + 1)) <= bytes) ++log2;
  return log2;
}

}  // namespace

uint32_t GuestTextureBytesAny(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                              bool tiled) {
  const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(format);
  if (info == nullptr || info->bytes_per_block() == 0 || width == 0 || height == 0 ||
      pitch < width || (pitch % info->block_width) != 0) {
    return 0;
  }
  const uint32_t block_bytes = info->bytes_per_block();
  const uint32_t pitch_blocks = pitch / info->block_width;
  const uint32_t x_blocks = (width + info->block_width - 1u) / info->block_width;
  const uint32_t y_blocks = (height + info->block_height - 1u) / info->block_height;
  uint64_t bytes = 0;
  if (!tiled) {
    bytes = uint64_t(pitch_blocks) * y_blocks * block_bytes;
  } else {
    bytes = uint64_t(rex::graphics::texture_util::GetTiledAddressUpperBound2D(
                x_blocks, y_blocks, pitch_blocks, Log2Bytes(block_bytes))) +
            block_bytes;
  }
  return bytes > 0xFFFFFFFFull ? 0u : static_cast<uint32_t>(bytes);
}

bool LoadGuestTextureLinear(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                            uint32_t endian, bool tiled, const uint8_t* guest, size_t guest_bytes,
                            LinearTexture& out, std::string& reason) {
  const rex::graphics::FormatInfo* info = rex::graphics::FormatInfo::Get(format);
  if (info == nullptr || info->bytes_per_block() == 0 || guest == nullptr || endian > 3u) {
    reason = "texture format has no layout";
    return false;
  }
  const uint32_t need = GuestTextureBytesAny(format, width, height, pitch, tiled);
  if (need == 0 || guest_bytes < need) {
    reason = "texture is shorter than its base level";
    return false;
  }
  out.block_width = info->block_width;
  out.block_height = info->block_height;
  out.block_bytes = info->bytes_per_block();
  out.width_blocks = (width + info->block_width - 1u) / info->block_width;
  out.height_blocks = (height + info->block_height - 1u) / info->block_height;
  const uint32_t pitch_blocks = pitch / info->block_width;
  const size_t row = size_t(out.width_blocks) * out.block_bytes;
  out.blocks.assign(row * out.height_blocks, 0);
  if (!tiled) {
    for (uint32_t by = 0; by < out.height_blocks; ++by) {
      std::memcpy(out.blocks.data() + by * row, guest + size_t(by) * pitch_blocks * out.block_bytes, row);
    }
  } else {
    rex::graphics::texture_conversion::UntileInfo untile{};
    untile.width = out.width_blocks;
    untile.height = out.height_blocks;
    untile.input_pitch = pitch_blocks;
    untile.output_pitch = out.width_blocks;
    untile.input_format_info = info;
    untile.output_format_info = info;
    untile.copy_callback = [](void* output, const void* input, size_t length) {
      std::memcpy(output, input, length);
    };
    rex::graphics::texture_conversion::Untile(out.blocks.data(), guest, &untile);
  }
  SwapBlock(out.blocks.data(), uint32_t(out.blocks.size()), endian);
  return true;
}

}  // namespace broken_bond::gfx
