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

// image_device is the guest address of the device inside image. Shader
// pointers, the declaration, stream 0, and both microcode blobs are moved
// next to it and the shadow words are rewritten to match.
bool RepackDeviceDraw(const GuestBytes& live, uint32_t device, std::vector<uint8_t>& image,
                      uint32_t& image_device, std::string& reason);

// Little-endian file: magic, image_device, byte count, then the image.
bool WriteDeviceDrawFile(const std::string& path, const std::vector<uint8_t>& image,
                         uint32_t image_device, std::string& reason);
bool ReadDeviceDrawFile(const std::string& path, std::vector<uint8_t>& image, uint32_t& image_device,
                        std::string& reason);

}  // namespace broken_bond::host
