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

}  // namespace broken_bond::gfx
