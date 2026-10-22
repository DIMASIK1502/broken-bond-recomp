#include "gfx/texture.h"

#include <cstring>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/util.h>

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

}  // namespace broken_bond::gfx
