// Copy one draw out of a guest memory map into a compact image. The existing
// device, declaration, and shader readers index that image by the low 29 bits
// of a guest address.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace broken_bond::host {

class GuestBytes {
 public:
  virtual ~GuestBytes() = default;
  virtual bool Read(uint32_t address, void* dst, size_t bytes) const = 0;
};

// A zero vertex_count keeps the byte count stored at device+1916. A non-zero
// count copies that many vertices from vertex_start, using the stride byte at
// device+12528. index_bytes, when non-zero, are copied from index_address and
// recorded after the vertices. index_first is the lowest index in that list.
struct DrawSpan {
  uint32_t vertex_count = 0;
  uint32_t vertex_start = 0;
  uint32_t index_address = 0;
  uint32_t index_bytes = 0;
  uint32_t index_size = 0;
  uint32_t index_first = 0;
  uint32_t base_vertex = 0;
};

// image_device is the guest address of the device inside image. Shader
// pointers, the declaration, stream 0, and both microcode blobs are moved
// next to it and the shadow words are rewritten to match.
// include_mesh copies the vertex stream, the index list, and one texture.
// A false value keeps the shaders and the declaration, which is enough to
// tell whether the pair is already translated.
bool RepackDeviceDraw(const GuestBytes& live, uint32_t device, std::vector<uint8_t>& image,
                      uint32_t& image_device, std::string& reason, const DrawSpan& span = {},
                      bool include_mesh = true);

// Little-endian file: magic, image_device, byte count, then the image.
bool WriteDeviceDrawFile(const std::string& path, const std::vector<uint8_t>& image,
                         uint32_t image_device, std::string& reason);
bool ReadDeviceDrawFile(const std::string& path, std::vector<uint8_t>& image, uint32_t& image_device,
                        std::string& reason);

// Byte count of stream 0 stored in the compact image. Zero when the image is
// too small to hold that dword.
uint32_t DeviceDrawStreamBytes(const std::vector<uint8_t>& image);

// Index list recorded in the compact image. bytes is zero when the draw had
// no indices. offset is the physical start of those bytes. first is the
// lowest index, so a later draw subtracts it from every index.
struct DeviceDrawIndexInfo {
  uint32_t offset = 0;
  uint32_t bytes = 0;
  uint32_t size = 0;
  uint32_t first = 0;
  uint32_t base_vertex = 0;
};

bool ReadDeviceDrawIndices(const std::vector<uint8_t>& image, DeviceDrawIndexInfo& out);

// Lowest and highest index in a big-endian list. 0xFFFF and 0xFFFFFFFF are
// skipped. first is the lowest. count is highest - lowest + 1.
bool IndexRange(const uint8_t* indices, uint32_t byte_count, uint32_t index_size, uint32_t& first,
                uint32_t& count, std::string& reason);

// Bytes of one 2D base level. format 6 is k_8_8_8_8 and format 18 is k_DXT1.
// Zero when the pitch does not cover the image. A tiled size includes the
// last block the 2D address math can touch.
uint32_t TextureCopyBytes(uint32_t format, uint32_t width, uint32_t height, uint32_t pitch,
                          bool tiled);

// One 2D base level, appended after the index list. slot is the fetch
// constant it came from. bytes is zero when the draw had no RGBA8 or DXT1
// image, or when these dwords are not a texture tail.
struct DeviceDrawTexture {
  uint32_t offset = 0;
  uint32_t bytes = 0;
  uint32_t format = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch = 0;
  uint32_t endian = 0;
  uint32_t tiled = 0;
  uint32_t slot = 0;
};

bool ReadDeviceDrawTexture(const std::vector<uint8_t>& image, DeviceDrawTexture& out);

}  // namespace broken_bond::host
