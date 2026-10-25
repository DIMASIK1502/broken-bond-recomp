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

// One vertex-fetch constant and the attributes packed into it. Float attributes
// are filled with a triangle that covers the viewport when the shader copies
// them to the position.
struct VertexAttribute {
  int32_t offset_words = 0;
  uint32_t components = 0;
  bool is_float = false;
  // Four signed integer shorts packed into two dwords: x,y then z,w.
  bool short4 = false;
  // When this float attribute is not the triangle, write (1, 0.5, 0.25, 1).
  bool constant_fill = false;
  uint32_t usage = 0;
  uint32_t usage_index = 0;
};

// Fragment that copies one vertex-shader interpolator location to the color target.
bool InterpolatorPassFragment(uint32_t location, std::vector<uint32_t>& words);

struct VertexStream {
  uint32_t fetch_constant = 0;
  uint32_t stride_words = 0;
  uint32_t index_register = 0xFFFFFFFFu;
  // Which attribute receives the covering triangle. The others stay zero.
  uint32_t triangle_attribute = 0;
  // For a short4 position, xyz is multiplied by short_scale and w by short_w_scale.
  // short_w_scale 0 uses short_scale, so both grow together.
  int32_t short_scale = 1;
  int32_t short_w_scale = 0;
  VertexAttribute attributes[8]{};
  uint32_t attribute_count = 0;
};

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

  // Words from the device shadow. surface_info is RB_SURFACE_INFO: pitch in
  // bits 0:13 must equal the draw width, and MSAA bits 16:17 must be 1x.
  // depth_control is RB_DEPTHCONTROL, blend_control is RB_BLENDCONTROL0, and
  // mode_cntl is PA_SU_SC_MODE_CNTL. The clear color is what shows through
  // when the triangle is culled, blended away, or rejected by the depth test.
  struct RasterState {
    uint32_t surface_info = 0;
    uint32_t depth_control = 0;
    uint32_t blend_control = 0;
    uint32_t mode_cntl = 0;
    float clear_red = 0;
    float clear_green = 0;
    float clear_blue = 0;
    float clear_alpha = 0;
  };

  // Draw the image onto an offscreen target with a vertex interpolator and read it back.
  // raster, when set, applies the register words above.
  bool DrawTexturedRgba8(const uint8_t* pixels, uint32_t width, uint32_t height,
                         std::vector<uint8_t>& out, std::string& reason,
                         const RasterState* fixed_state = nullptr);

  // Run one guest fragment module. The target is cleared to magenta. Alpha test
  // is disabled and the color scale is 1. Pixel float constants are
  // (1, 0.5, 0.25, 1). When image_count is not zero, every image binding sees
  // the same 32×32 RGBA8 texture and every sampler binding is nearest.
  // texture_key 0 is the gradient texel (17+x, 34+y, 51, 255). texture_key 1
  // is the solid texel (200, 10, 20, 255). write_interpolator makes the vertex
  // stage write (interpolator_x, 0.25, 0.5, 1) at location 0. guest_vertex
  // treats words as a vertex shader and copies location 0 to the color target.
  // fragment_words, when guest_vertex is set, replaces that copy with a guest
  // pixel shader. Both stages must use interpolator 0 at location 0.
  // streams, when guest_vertex is set, are the vertex fetches placed in shared
  // memory. Vertex float constants are a repeated identity matrix.
  // texture_rgba, when set, replaces the 32×32 pattern. It is tightly packed
  // RGBA8 of texture_width by texture_height, and texture_width is a multiple
  // of 32 so the fetch pitch can name it.
  bool DrawGuestFragment(const uint32_t* words, size_t word_count, uint32_t width,
                         uint32_t height, uint32_t image_count, uint32_t sampler_count,
                         uint32_t texture_key, bool write_interpolator, float interpolator_x,
                         bool guest_vertex, const VertexStream* streams, uint32_t stream_count,
                         std::vector<uint8_t>& out, std::string& reason,
                         const uint32_t* fragment_words = nullptr, size_t fragment_word_count = 0,
                         const uint8_t* texture_rgba = nullptr, uint32_t texture_width = 0,
                         uint32_t texture_height = 0, const RasterState* fixed_state = nullptr);

 private:
  struct Vulkan;
  std::unique_ptr<Vulkan> vulkan_;
  DeviceInfo info_;
};

}  // namespace broken_bond::gfx
