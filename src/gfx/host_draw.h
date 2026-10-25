// Submit one repacked device image into the device's retained 1280x512 frame.
// Present stays on Xenos. The first draw clears color and depth. Later draws
// load both.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace broken_bond::gfx {

class Device;

struct RetainedDrawInfo {
  bool from_cache = false;
  uint32_t covered = 0;
  uint32_t colors = 0;
  bool sampled = false;
};

// image is the compact device-draw image. allow_new translates a shader pair
// that is not cached yet. A cached pair is drawn either way. sample copies the
// retained color back and fills covered and colors. The vertex fetch endian is
// the texture endian when that endian is 1 or 2, and 1 otherwise.
bool SubmitRetainedDraw(Device& device, const uint8_t* image, size_t image_bytes,
                        uint32_t image_device, bool allow_new, bool sample, RetainedDrawInfo& info,
                        std::string& reason);

// True when this shader pair is already in the translation cache. The image
// only needs the shaders and the declaration. A read failure returns false.
bool RetainedShaderCached(const uint8_t* image, size_t image_bytes, uint32_t image_device,
                          bool& cached, std::string& reason);

}  // namespace broken_bond::gfx
