// Vulkan device for the host renderer. The game does not construct this yet.
// Xenos remains the renderer until this path can run the story, fights, and
// cutscenes.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace broken_bond::gfx {

struct DeviceInfo {
  std::string name;
  uint32_t api = 0;
  bool scalar_block = false;
  bool standard_layout = false;
  bool dynamic_rendering = false;
  bool demote = false;
  bool sample_rate = false;
};

class Device {
 public:
  Device();
  ~Device();

  Device(const Device&) = delete;
  Device& operator=(const Device&) = delete;

  bool Open(std::string& reason);
  const DeviceInfo& info() const;

  // The driver accepts this SPIR-V module. The module is destroyed before return.
  bool AcceptShader(const uint32_t* words, size_t word_count, std::string& reason);

  // Copy tightly packed RGBA8 into an image and read the same bytes back.
  bool ReadbackRgba8(const uint8_t* pixels, uint32_t width, uint32_t height,
                     std::vector<uint8_t>& out, std::string& reason);

  // Draw the image onto an offscreen target with a vertex interpolator and read it back.
  bool DrawTexturedRgba8(const uint8_t* pixels, uint32_t width, uint32_t height,
                         std::vector<uint8_t>& out, std::string& reason);

  // Run one guest fragment module. The target is cleared to magenta. Alpha test
  // is disabled and the color scale is 1. Pixel float constants are
  // (1, 0.5, 0.25, 1). When image_count is not zero, every image binding sees
  // the same 32×32 RGBA8 texture and every sampler binding is nearest.
  // texture_key 0 is the gradient texel (17+x, 34+y, 51, 255). texture_key 1
  // is the solid texel (200, 10, 20, 255).
  bool DrawGuestFragment(const uint32_t* words, size_t word_count, uint32_t width,
                         uint32_t height, uint32_t image_count, uint32_t sampler_count,
                         uint32_t texture_key, std::vector<uint8_t>& out, std::string& reason);

 private:
  struct Vulkan;
  std::unique_ptr<Vulkan> vulkan_;
  DeviceInfo info_;
};

}  // namespace broken_bond::gfx
