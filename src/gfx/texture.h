// 2D Xenos texture layout for the host renderer. Uses the SDK untile, not a
// second copy of the address math.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace broken_bond::gfx {

// Tile a tightly packed RGBA8 image with GetTiledOffset2D and untile it with
// the SDK. Fails when the bytes do not come back unchanged.
bool RoundTripTiledRgba8(const uint8_t* linear, uint32_t pitch, uint32_t height,
                         std::string& reason);

// The six dwords of one texture fetch constant, in the layout the device
// shadow stores. pitch, width, and height are pixels. endian is the Xenos
// Endian value. guest_address is the base address field shifted up by 12.
struct TextureFetch {
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;
  uint32_t format = 0;
  uint32_t endian = 0;
  bool tiled = false;
  uint32_t mip_min = 0;
  uint32_t mip_max = 0;
  uint32_t guest_address = 0;
};

// Reads one fetch constant. The base level is k_8_8_8_8. pitch is a multiple
// of 32 and at least the width.
bool ReadTextureFetch(const uint32_t words[6], TextureFetch& out, std::string& reason);

// Writes those six dwords for a 2D k_8_8_8_8 image. mip levels stay 0.
bool WriteRgba8Fetch(uint32_t words[6], uint32_t width, uint32_t height, uint32_t pitch,
                     bool tiled, uint32_t endian, std::string& reason);

// linear is tightly packed width*height RGBA8. guest is the memory the fetch
// constant describes: rows of pitch texels, endian-swapped, tiled when the
// constant says so.
bool StoreFetchRgba8(const uint32_t words[6], const uint8_t* linear, std::vector<uint8_t>& guest,
                     std::string& reason);

// The reverse. linear comes back tightly packed.
bool LoadFetchRgba8(const uint32_t words[6], const uint8_t* guest, size_t guest_bytes,
                    std::vector<uint8_t>& linear, std::string& reason);

// Bytes of one 2D base level, using the SDK tile bound. format 6 is
// k_8_8_8_8, format 18 is k_DXT1, and format 20 is k_DXT4_5. Zero when the
// pitch does not cover it.
uint32_t GuestTextureBytes(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                           bool tiled);

// guest is the raw base level. format 18 is DXT1 and format 20 is DXT5,
// decoded after the fetch endian swap. linear comes back tightly packed RGBA8.
bool LoadGuestTextureRgba8(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                           uint32_t endian, bool tiled, const uint8_t* guest, size_t guest_bytes,
                           std::vector<uint8_t>& linear, std::string& reason);

// Any format the SDK's FormatInfo describes, as tightly packed rows of
// blocks: untiled when the fetch says so, each dword swapped by the fetch
// endian, nothing decoded. block_width / block_height / block_bytes describe
// the output. Fails for formats without a FormatInfo.
struct LinearTexture {
  std::vector<uint8_t> blocks;
  uint32_t block_width = 1;
  uint32_t block_height = 1;
  uint32_t block_bytes = 4;
  uint32_t width_blocks = 0;
  uint32_t height_blocks = 0;
};
uint32_t GuestTextureBytesAny(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                              bool tiled);
bool LoadGuestTextureLinear(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                            uint32_t endian, bool tiled, const uint8_t* guest, size_t guest_bytes,
                            LinearTexture& out, std::string& reason);

// The base level of a 2D texture. With packed_mips, a texture whose shorter
// side is at most 16 texels keeps even its base level in the packed mip tail,
// at a block offset inside the first 32x32-block tile; the region from the
// tile's origin is loaded and the level cropped out of it.
uint32_t GuestTextureBaseBytes(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                               bool tiled, bool packed_mips);
bool LoadGuestTextureBase(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                          uint32_t endian, bool tiled, bool packed_mips, const uint8_t* guest,
                          size_t guest_bytes, LinearTexture& out, std::string& reason);

}  // namespace broken_bond::gfx
