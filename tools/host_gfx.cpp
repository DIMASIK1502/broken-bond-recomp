// Opens the host Vulkan device, checks one shader module, untile of an RGBA8
// image, and a GPU readback of that image. Does not start the game.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "gfx/device.h"
#include "host/device_repack.h"
#include "gfx/device_shadow.h"
#include "gfx/edram.h"
#include "gfx/shader_object.h"
#include "gfx/texture.h"

#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace {

// Vertex stage that only writes gl_Position. SPIR-V 1.5, Vulkan 1.2.
constexpr uint32_t kPositionVertex[] = {
    0x07230203, 0x00010500, 0x00070000, 0x0000000b, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
    0x00000000, 0x00000001, 0x0006000f, 0x00000000, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002,
    0x00040047, 0x00000002, 0x0000000b, 0x00000000, 0x00020013, 0x00000003, 0x00030021, 0x00000004,
    0x00000003, 0x00030016, 0x00000005, 0x00000020, 0x00040017, 0x00000006, 0x00000005, 0x00000004,
    0x00040020, 0x00000007, 0x00000003, 0x00000006, 0x0004002b, 0x00000005, 0x00000008, 0x00000000,
    0x0007002c, 0x00000006, 0x00000009, 0x00000008, 0x00000008, 0x00000008, 0x00000008, 0x0004003b,
    0x00000007, 0x00000002, 0x00000003, 0x00050036, 0x00000003, 0x00000001, 0x00000000, 0x00000004,
    0x000200f8, 0x0000000a, 0x0003003e, 0x00000002, 0x00000009, 0x000100fd, 0x00010038};

int Fail(const std::string& reason) {
  std::cerr << reason << "\n";
  return 1;
}

}  // namespace

int main() {
  broken_bond::gfx::Device device;
  std::string reason;
  if (!device.Open(reason)) {
    return Fail(reason);
  }
  const broken_bond::gfx::DeviceInfo& info = device.info();
  std::cout << "device " << info.name << "\n";
  std::cout << "  api " << ((info.api >> 22) & 0x7fu) << "." << ((info.api >> 12) & 0x3ffu) << "."
            << (info.api & 0xfffu) << "\n";
  std::cout << "  scalar_block " << (info.scalar_block ? 1 : 0) << "\n";
  std::cout << "  standard_layout " << (info.standard_layout ? 1 : 0) << "\n";
  std::cout << "  dynamic_rendering " << (info.dynamic_rendering ? 1 : 0) << "\n";
  std::cout << "  demote " << (info.demote ? 1 : 0) << "\n";
  std::cout << "  sample_rate " << (info.sample_rate ? 1 : 0) << "\n";

  if (!device.AcceptShader(kPositionVertex, std::size(kPositionVertex), reason)) {
    return Fail(reason);
  }
  std::cout << "  shader_ok 1\n";

  constexpr uint32_t kPitch = 64;
  constexpr uint32_t kHeight = 48;
  std::vector<uint8_t> linear(static_cast<size_t>(kPitch) * kHeight * 4u);
  for (size_t i = 0; i < linear.size(); ++i) {
    linear[i] = static_cast<uint8_t>(i * 17u + 3u);
  }
  if (!broken_bond::gfx::RoundTripTiledRgba8(linear.data(), kPitch, kHeight, reason)) {
    return Fail(reason);
  }
  std::cout << "  untile_ok 1\n";

  std::vector<uint8_t> readback;
  if (!device.ReadbackRgba8(linear.data(), kPitch, kHeight, readback, reason)) {
    return Fail(reason);
  }
  if (readback != linear) {
    return Fail("GPU readback does not match the uploaded image");
  }
  std::cout << "  readback_ok 1\n";

  std::vector<uint8_t> drawn;
  if (!device.DrawTexturedRgba8(linear.data(), kPitch, kHeight, drawn, reason)) {
    return Fail(reason);
  }
  if (drawn != linear) {
    return Fail("drawn image does not match the texture");
  }
  std::cout << "  draw_ok 1\n";

  broken_bond::gfx::Device::RasterState state;
  rex::graphics::reg::RB_SURFACE_INFO surface{};
  surface.surface_pitch = kPitch;
  state.surface_info = surface.value;
  rex::graphics::reg::PA_SU_SC_MODE_CNTL mode{};
  mode.cull_front = 1;
  state.mode_cntl = mode.value;
  std::vector<uint8_t> culled;
  if (!device.DrawTexturedRgba8(linear.data(), kPitch, kHeight, culled, reason, &state)) {
    return Fail(reason);
  }
  for (uint8_t byte : culled) {
    if (byte != 0) {
      return Fail("front cull still drew the triangle");
    }
  }
  mode.cull_front = 0;
  state.mode_cntl = mode.value;
  rex::graphics::reg::RB_BLENDCONTROL blend{};
  blend.color_srcblend = rex::graphics::xenos::BlendFactor::kZero;
  blend.color_destblend = rex::graphics::xenos::BlendFactor::kOne;
  blend.alpha_srcblend = rex::graphics::xenos::BlendFactor::kZero;
  blend.alpha_destblend = rex::graphics::xenos::BlendFactor::kOne;
  state.blend_control = blend.value;
  state.clear_red = 1.f;
  state.clear_blue = 1.f;
  state.clear_alpha = 1.f;
  std::vector<uint8_t> blended;
  if (!device.DrawTexturedRgba8(linear.data(), kPitch, kHeight, blended, reason, &state)) {
    return Fail(reason);
  }
  for (size_t i = 0; i + 3 < blended.size(); i += 4) {
    if (blended[i] != 255 || blended[i + 1] != 0 || blended[i + 2] != 255 || blended[i + 3] != 255) {
      return Fail("dest-one blend did not keep the clear color");
    }
  }
  blend.color_srcblend = rex::graphics::xenos::BlendFactor::kOne;
  blend.color_destblend = rex::graphics::xenos::BlendFactor::kZero;
  blend.alpha_srcblend = rex::graphics::xenos::BlendFactor::kOne;
  blend.alpha_destblend = rex::graphics::xenos::BlendFactor::kZero;
  state.blend_control = blend.value;
  state.clear_red = 0.f;
  state.clear_blue = 0.f;
  state.clear_alpha = 0.f;
  rex::graphics::reg::RB_DEPTHCONTROL depth{};
  depth.z_enable = 1;
  depth.zfunc = rex::graphics::xenos::CompareFunction::kLess;
  state.depth_control = depth.value;
  std::vector<uint8_t> depth_pass;
  if (!device.DrawTexturedRgba8(linear.data(), kPitch, kHeight, depth_pass, reason, &state) ||
      depth_pass != linear) {
    return Fail(reason.empty() ? "depth less did not draw the image" : reason);
  }
  depth.zfunc = rex::graphics::xenos::CompareFunction::kNever;
  state.depth_control = depth.value;
  std::vector<uint8_t> depth_fail;
  if (!device.DrawTexturedRgba8(linear.data(), kPitch, kHeight, depth_fail, reason, &state)) {
    return Fail(reason);
  }
  for (uint8_t byte : depth_fail) {
    if (byte != 0) {
      return Fail("depth never still drew the triangle");
    }
  }
  std::cout << "  raster_ok 1\n";

  constexpr uint32_t kFetchWidth = 48;
  constexpr uint32_t kFetchHeight = 32;
  constexpr uint32_t kFetchPitch = 64;
  constexpr uint32_t kEndian8in32 = 2;
  std::vector<uint8_t> picture(static_cast<size_t>(kFetchWidth) * kFetchHeight * 4u);
  for (uint32_t y = 0; y < kFetchHeight; ++y) {
    for (uint32_t x = 0; x < kFetchWidth; ++x) {
      uint8_t* pixel = picture.data() + (static_cast<size_t>(y) * kFetchWidth + x) * 4u;
      pixel[0] = static_cast<uint8_t>(x + 1u);
      pixel[1] = static_cast<uint8_t>(y + 3u);
      pixel[2] = static_cast<uint8_t>(x + y);
      pixel[3] = 255;
    }
  }
  uint32_t fetch_words[6] = {};
  if (!broken_bond::gfx::WriteRgba8Fetch(fetch_words, kFetchWidth, kFetchHeight, kFetchPitch, false,
                                        kEndian8in32, reason)) {
    return Fail(reason);
  }
  broken_bond::gfx::TextureFetch fetch;
  if (!broken_bond::gfx::ReadTextureFetch(fetch_words, fetch, reason) || fetch.tiled ||
      fetch.width != kFetchWidth || fetch.height != kFetchHeight || fetch.pitch != kFetchPitch ||
      fetch.endian != kEndian8in32 || fetch.format != 6u) {
    return Fail(reason.empty() ? "fetch constant fields did not round-trip" : reason);
  }
  std::vector<uint8_t> guest;
  if (!broken_bond::gfx::StoreFetchRgba8(fetch_words, picture.data(), guest, reason)) {
    return Fail(reason);
  }
  if (guest.size() < 4 || guest[0] != 255 || guest[1] != picture[2] || guest[2] != picture[1] ||
      guest[3] != picture[0]) {
    return Fail("linear fetch memory did not swap endian");
  }
  std::vector<uint8_t> restored;
  if (!broken_bond::gfx::LoadFetchRgba8(fetch_words, guest.data(), guest.size(), restored, reason) ||
      restored != picture) {
    return Fail(reason.empty() ? "linear fetch did not restore the image" : reason);
  }
  std::cout << "  fetch_linear_ok 1\n";

  if (!broken_bond::gfx::WriteRgba8Fetch(fetch_words, kFetchWidth, kFetchHeight, kFetchPitch, true,
                                        kEndian8in32, reason)) {
    return Fail(reason);
  }
  if (!broken_bond::gfx::StoreFetchRgba8(fetch_words, picture.data(), guest, reason) ||
      !broken_bond::gfx::LoadFetchRgba8(fetch_words, guest.data(), guest.size(), restored, reason) ||
      restored != picture) {
    return Fail(reason.empty() ? "tiled fetch did not restore the image" : reason);
  }
  std::vector<uint8_t> fetch_drawn;
  if (!device.DrawTexturedRgba8(restored.data(), kFetchWidth, kFetchHeight, fetch_drawn, reason) ||
      fetch_drawn != picture) {
    return Fail(reason.empty() ? "drawn fetch image does not match" : reason);
  }
  std::cout << "  fetch_draw_ok 1\n";

  std::vector<uint32_t> pass;
  if (!broken_bond::gfx::InterpolatorPassFragment(0, pass)) {
    return Fail("interpolator pass fragment was not built");
  }
  constexpr uint32_t kGuestWidth = 32;
  constexpr uint32_t kGuestHeight = 8;
  broken_bond::gfx::Device::RasterState guest_state;
  rex::graphics::reg::RB_SURFACE_INFO guest_surface{};
  guest_surface.surface_pitch = kGuestWidth;
  guest_state.surface_info = guest_surface.value;
  guest_state.clear_red = 1.f;
  guest_state.clear_blue = 1.f;
  guest_state.clear_alpha = 1.f;
  rex::graphics::reg::RB_BLENDCONTROL guest_blend{};
  guest_blend.color_srcblend = rex::graphics::xenos::BlendFactor::kOne;
  guest_blend.color_destblend = rex::graphics::xenos::BlendFactor::kZero;
  guest_blend.alpha_srcblend = rex::graphics::xenos::BlendFactor::kOne;
  guest_blend.alpha_destblend = rex::graphics::xenos::BlendFactor::kZero;
  guest_state.blend_control = guest_blend.value;
  rex::graphics::reg::RB_DEPTHCONTROL guest_depth{};
  guest_depth.z_enable = 1;
  guest_depth.zfunc = rex::graphics::xenos::CompareFunction::kLess;
  guest_state.depth_control = guest_depth.value;
  auto draw_guest = [&](std::vector<uint8_t>& out) {
    return device.DrawGuestFragment(pass.data(), pass.size(), kGuestWidth, kGuestHeight, 0, 0, 0,
                                    true, 1.f, false, nullptr, 0, out, reason, nullptr, 0, nullptr,
                                    0, 0, &guest_state);
  };
  std::vector<uint8_t> guest_drawn;
  if (!draw_guest(guest_drawn)) {
    return Fail(reason);
  }
  for (size_t i = 0; i + 3 < guest_drawn.size(); i += 4) {
    if (guest_drawn[i] != 255 || guest_drawn[i + 1] != 64 || guest_drawn[i + 2] != 127 ||
        guest_drawn[i + 3] != 255) {
      return Fail("guest depth less did not keep the interpolator color");
    }
  }
  rex::graphics::reg::PA_SU_SC_MODE_CNTL guest_mode{};
  guest_mode.cull_back = 1;
  guest_state.mode_cntl = guest_mode.value;
  if (!draw_guest(guest_drawn)) {
    return Fail(reason);
  }
  for (size_t i = 0; i + 3 < guest_drawn.size(); i += 4) {
    if (guest_drawn[i] != 255 || guest_drawn[i + 1] != 0 || guest_drawn[i + 2] != 255 ||
        guest_drawn[i + 3] != 255) {
      return Fail("guest back cull still drew the triangle");
    }
  }
  guest_mode.cull_back = 0;
  guest_state.mode_cntl = guest_mode.value;
  guest_depth.zfunc = rex::graphics::xenos::CompareFunction::kNever;
  guest_state.depth_control = guest_depth.value;
  if (!draw_guest(guest_drawn)) {
    return Fail(reason);
  }
  for (size_t i = 0; i + 3 < guest_drawn.size(); i += 4) {
    if (guest_drawn[i] != 255 || guest_drawn[i + 1] != 0 || guest_drawn[i + 2] != 255 ||
        guest_drawn[i + 3] != 255) {
      return Fail("guest depth never still drew the triangle");
    }
  }
  std::cout << "  guest_raster_ok 1\n";

  broken_bond::gfx::EdramSurface native;
  native.pitch_pixels = 1280;
  native.height_pixels = 512;
  native.msaa = static_cast<uint32_t>(rex::graphics::xenos::MsaaSamples::k4X);
  broken_bond::gfx::EdramSpan native_span;
  if (!broken_bond::gfx::EdramSpanFor(native, native_span, reason) ||
      native_span.pitch_tiles != 32 || native_span.height_tiles != 64 ||
      native_span.tile_count != 2048) {
    return Fail(reason.empty() ? "native 1280x512 4x is not the whole EDRAM" : reason);
  }
  broken_bond::gfx::EdramSurface native_depth = native;
  native_depth.base_tiles = 100;
  bool overlap = false;
  if (!broken_bond::gfx::EdramTilesOverlap(native, native_depth, overlap, reason) || !overlap) {
    return Fail(reason.empty() ? "native depth does not alias the color target" : reason);
  }
  broken_bond::gfx::EdramSurface one_tile;
  one_tile.pitch_pixels = 40;
  one_tile.height_pixels = 8;
  one_tile.msaa = native.msaa;
  broken_bond::gfx::EdramSurface next_tile = one_tile;
  next_tile.base_tiles = 1;
  if (!broken_bond::gfx::EdramTilesOverlap(one_tile, next_tile, overlap, reason) || overlap) {
    return Fail(reason.empty() ? "adjacent EDRAM tiles were treated as one" : reason);
  }
  std::cout << "  edram_alias_ok 1\n";

  constexpr uint32_t kFrameWidth = 1280;
  constexpr uint32_t kFrameHeight = 512;
  std::vector<uint8_t> frame(static_cast<size_t>(kFrameWidth) * kFrameHeight * 4u);
  for (uint32_t y = 0; y < kFrameHeight; ++y) {
    for (uint32_t x = 0; x < kFrameWidth; ++x) {
      uint8_t* pixel = frame.data() + (static_cast<size_t>(y) * kFrameWidth + x) * 4u;
      pixel[0] = static_cast<uint8_t>(x);
      pixel[1] = static_cast<uint8_t>(y);
      pixel[2] = static_cast<uint8_t>(x + y);
      pixel[3] = 255;
    }
  }
  broken_bond::gfx::Device::RasterState frame_state;
  rex::graphics::reg::RB_SURFACE_INFO frame_surface{};
  frame_surface.surface_pitch = kFrameWidth;
  frame_surface.msaa_samples = rex::graphics::xenos::MsaaSamples::k4X;
  frame_state.surface_info = frame_surface.value;
  rex::graphics::reg::RB_BLENDCONTROL frame_blend{};
  frame_blend.color_srcblend = rex::graphics::xenos::BlendFactor::kOne;
  frame_blend.color_destblend = rex::graphics::xenos::BlendFactor::kZero;
  frame_blend.alpha_srcblend = rex::graphics::xenos::BlendFactor::kOne;
  frame_blend.alpha_destblend = rex::graphics::xenos::BlendFactor::kZero;
  frame_state.blend_control = frame_blend.value;
  std::vector<uint8_t> frame_drawn;
  if (!device.DrawTexturedRgba8(frame.data(), kFrameWidth, kFrameHeight, frame_drawn, reason,
                                &frame_state) ||
      frame_drawn != frame) {
    return Fail(reason.empty() ? "4x resolve of 1280x512 does not match" : reason);
  }
  std::cout << "  frame_resolve_ok 1\n";

  auto store_be = [](std::vector<uint8_t>& memory, uint32_t address, uint32_t value) {
    uint8_t* slot = memory.data() + (address & 0x1FFFFFFFu);
    slot[0] = static_cast<uint8_t>(value >> 24);
    slot[1] = static_cast<uint8_t>(value >> 16);
    slot[2] = static_cast<uint8_t>(value >> 8);
    slot[3] = static_cast<uint8_t>(value);
  };
  std::vector<uint8_t> shader_guest(0x3000, 0);
  constexpr uint32_t kShader = 0xC0002000u;
  store_be(shader_guest, kShader + 32u, 0x1000u);
  store_be(shader_guest, kShader + 896u, 0x100u);
  store_be(shader_guest, kShader + 0x100u + 872u, 0x234u);
  store_be(shader_guest, kShader + 0x100u + 876u, 8u);
  for (uint32_t i = 0; i < 8; ++i) {
    shader_guest[0x1234u + i] = static_cast<uint8_t>(0x10u + i);
  }
  broken_bond::gfx::ShaderMicrocode microcode;
  std::vector<uint8_t> microcode_bytes;
  if (!broken_bond::gfx::ReadVertexShaderMicrocode(shader_guest.data(), shader_guest.size(),
                                                   kShader, microcode, microcode_bytes, reason) ||
      microcode.guest_address != 0xC0001234u || microcode.byte_count != 8u ||
      microcode_bytes.size() != 8u) {
    return Fail(reason.empty() ? "vertex shader microcode address was not 0xC0001234" : reason);
  }
  for (uint32_t i = 0; i < 8; ++i) {
    if (microcode_bytes[i] != static_cast<uint8_t>(0x10u + i)) {
      return Fail("vertex shader microcode bytes did not match");
    }
  }
  store_be(shader_guest, kShader + 32u, 0);
  store_be(shader_guest, kShader + 872u, 0x20u);
  store_be(shader_guest, kShader + 904u, 0x200u);
  store_be(shader_guest, kShader + 0x200u + 872u, 0xE0000000u);
  store_be(shader_guest, kShader + 0x200u + 876u, 4u);
  shader_guest[0x1000] = 0xAB;
  shader_guest[0x1001] = 0xCD;
  shader_guest[0x1002] = 0xEF;
  shader_guest[0x1003] = 0x01;
  if (!broken_bond::gfx::ReadVertexShaderMicrocode(shader_guest.data(), shader_guest.size(),
                                                   kShader, microcode, microcode_bytes, reason) ||
      microcode.guest_address != 0xC0001000u || microcode_bytes.size() != 4u ||
      microcode_bytes[0] != 0xAB || microcode_bytes[1] != 0xCD || microcode_bytes[2] != 0xEF ||
      microcode_bytes[3] != 0x01) {
    return Fail(reason.empty() ? "alternate shader slot did not land at 0xC0001000" : reason);
  }
  std::cout << "  shader_ucode_ok 1\n";

  std::vector<uint8_t> pixel_guest(0x3000, 0);
  constexpr uint32_t kPixel = 0xC0002000u;
  store_be(pixel_guest, kPixel + 24u, 0);
  store_be(pixel_guest, kPixel + 64u, 0x80u);
  store_be(pixel_guest, kPixel + 0x80u + 40u, 0x1235u);
  store_be(pixel_guest, kPixel + 0x80u + 44u, 4u);
  pixel_guest[0x1234] = 0x11;
  pixel_guest[0x1235] = 0x22;
  pixel_guest[0x1236] = 0x33;
  pixel_guest[0x1237] = 0x44;
  if (!broken_bond::gfx::ReadPixelShaderMicrocode(pixel_guest.data(), pixel_guest.size(), kPixel,
                                                  microcode, microcode_bytes, reason) ||
      microcode.guest_address != 0xC0001234u || microcode.byte_count != 4u ||
      microcode_bytes.size() != 4u || microcode_bytes[0] != 0x11 || microcode_bytes[1] != 0x22 ||
      microcode_bytes[2] != 0x33 || microcode_bytes[3] != 0x44) {
    return Fail(reason.empty() ? "pixel shader microcode did not drop the low address bit"
                               : reason);
  }
  std::cout << "  pixel_ucode_ok 1\n";

  std::vector<uint8_t> device_guest(0xC000, 0);
  constexpr uint32_t kDevice = 0xC0008000u;
  store_be(device_guest, kDevice + 12696u, 0xAABBCCDDu);
  store_be(device_guest, kDevice + 12692u, 0x11223344u);
  store_be(device_guest, kDevice + 10368u, 0x00020500u);
  store_be(device_guest, kDevice + 10548u, 0x11111111u);
  store_be(device_guest, kDevice + 10552u, 0x22222222u);
  store_be(device_guest, kDevice + 10568u, 0x33333333u);
  device_guest[(kDevice + 12528u) & 0x1FFFFFFFu] = 2;
  store_be(device_guest, kDevice + 1912u, 0x4000u);
  store_be(device_guest, kDevice + 1916u, 8u);
  for (uint32_t i = 0; i < 8; ++i) {
    device_guest[0x4000u + i] = static_cast<uint8_t>(i + 1u);
  }
  broken_bond::gfx::DeviceDraw draw;
  std::vector<uint8_t> stream;
  if (!broken_bond::gfx::ReadDeviceDraw(device_guest.data(), device_guest.size(), kDevice, draw,
                                        stream, reason) ||
      draw.vertex_shader != 0xAABBCCDDu || draw.pixel_shader != 0x11223344u ||
      draw.surface_info != 0x00020500u || draw.depth_control != 0x11111111u ||
      draw.blend_control != 0x22222222u || draw.mode_cntl != 0x33333333u ||
      draw.stream_stride_dwords != 2u || draw.stream_address != 0xC0004000u ||
      draw.stream_bytes != 8u || stream.size() != 8u) {
    return Fail(reason.empty() ? "device shadow did not return stream 0" : reason);
  }
  for (uint32_t i = 0; i < 8; ++i) {
    if (stream[i] != static_cast<uint8_t>(i + 1u)) {
      return Fail("vertex stream bytes did not match");
    }
  }
  std::cout << "  device_draw_ok 1\n";

  auto store_be16 = [](std::vector<uint8_t>& memory, uint32_t address, uint16_t value) {
    uint8_t* slot = memory.data() + (address & 0x1FFFFFFFu);
    slot[0] = static_cast<uint8_t>(value >> 8);
    slot[1] = static_cast<uint8_t>(value);
  };
  std::vector<uint8_t> decl_guest(0xC000, 0);
  constexpr uint32_t kDeclDevice = 0xC0008000u;
  constexpr uint32_t kDecl = 0xC0001000u;
  store_be(decl_guest, kDeclDevice + 11820u, kDecl);
  store_be(decl_guest, kDecl + 24u, 2u);
  store_be16(decl_guest, kDecl + 52u, 0);
  store_be16(decl_guest, kDecl + 54u, 0);
  store_be(decl_guest, kDecl + 56u, 0x31Au);
  decl_guest[(kDecl + 61u) & 0x1FFFFFFFu] = 0;
  decl_guest[(kDecl + 62u) & 0x1FFFFFFFu] = 0;
  store_be16(decl_guest, kDecl + 64u, 1);
  store_be16(decl_guest, kDecl + 66u, 8);
  store_be(decl_guest, kDecl + 68u, 6u);
  decl_guest[(kDecl + 73u) & 0x1FFFFFFFu] = 5;
  decl_guest[(kDecl + 74u) & 0x1FFFFFFFu] = 1;
  decl_guest[(kDeclDevice + 12528u) & 0x1FFFFFFFu] = 5;
  decl_guest[(kDeclDevice + 12529u) & 0x1FFFFFFFu] = 4;
  std::vector<broken_bond::gfx::VertexElement> elements;
  if (!broken_bond::gfx::ReadDeviceDeclaration(decl_guest.data(), decl_guest.size(), kDeclDevice,
                                               elements, reason) ||
      elements.size() != 2u || elements[0].format != 26u || elements[0].offset_words != 0 ||
      elements[0].stride_words != 5u || !elements[0].signed_components ||
      !elements[0].integer_components || elements[0].usage != 0u || elements[0].usage_index != 0u ||
      elements[1].format != 6u || elements[1].offset_words != 2 || elements[1].stride_words != 4u ||
      elements[1].signed_components || elements[1].integer_components || elements[1].usage != 5u ||
      elements[1].usage_index != 1u) {
    return Fail(reason.empty() ? "vertex declaration did not match the shadow" : reason);
  }
  std::cout << "  declaration_ok 1\n";

  struct VectorBytes : broken_bond::host::GuestBytes {
    const std::vector<uint8_t>* memory = nullptr;
    bool Read(uint32_t address, void* dst, size_t bytes) const override {
      const uint32_t physical = address & 0x1FFFFFFFu;
      if (memory == nullptr || static_cast<uint64_t>(physical) + bytes > memory->size()) {
        return false;
      }
      std::memcpy(dst, memory->data() + physical, bytes);
      return true;
    }
  };
  std::vector<uint8_t> live(0x40000, 0);
  constexpr uint32_t kLiveDevice = 0xC0010000u;
  store_be(live, kLiveDevice + 11820u, 0xC0020000u);
  store_be(live, kLiveDevice + 12696u, 0xC0024000u);
  store_be(live, kLiveDevice + 12692u, 0xC0028000u);
  store_be(live, kLiveDevice + 1912u, 0x30000u);
  store_be(live, kLiveDevice + 1916u, 8u);
  live[(kLiveDevice + 12528u) & 0x1FFFFFFFu] = 5;
  store_be(live, kLiveDevice + 1920u, 0x3F800000u);
  store_be(live, 0xC0020000u + 24u, 1u);
  store_be16(live, 0xC0020000u + 52u, 0);
  store_be16(live, 0xC0020000u + 54u, 0);
  store_be(live, 0xC0020000u + 56u, 0x31Au);
  store_be(live, 0xC0024000u + 32u, 0);
  store_be(live, 0xC0024000u + 896u, 0x100u);
  store_be(live, 0xC0024100u + 872u, 0x34000u);
  store_be(live, 0xC0024100u + 876u, 8u);
  for (uint32_t i = 0; i < 8; ++i) {
    live[0x34000u + i] = static_cast<uint8_t>(0x40u + i);
  }
  store_be(live, 0xC0028000u + 24u, 0);
  store_be(live, 0xC0028000u + 64u, 0x40u);
  store_be(live, 0xC0028040u + 40u, 0x38001u);
  store_be(live, 0xC0028040u + 44u, 4u);
  live[0x38000] = 0xAB;
  live[0x38001] = 0xCD;
  live[0x38002] = 0xEF;
  live[0x38003] = 0x01;
  for (uint32_t i = 0; i < 8; ++i) {
    live[0x30000u + i] = static_cast<uint8_t>(0x50u + i);
  }
  VectorBytes live_bytes;
  live_bytes.memory = &live;
  std::vector<uint8_t> packed;
  uint32_t packed_device = 0;
  const std::string snapshot = "device-draw-test.bin";
  std::vector<uint8_t> loaded;
  uint32_t loaded_device = 0;
  if (!broken_bond::host::RepackDeviceDraw(live_bytes, kLiveDevice, packed, packed_device, reason) ||
      !broken_bond::host::WriteDeviceDrawFile(snapshot, packed, packed_device, reason) ||
      !broken_bond::host::ReadDeviceDrawFile(snapshot, loaded, loaded_device, reason)) {
    return Fail(reason.empty() ? "device draw snapshot was not written" : reason);
  }
  broken_bond::gfx::DeviceDraw packed_draw;
  std::vector<uint8_t> packed_stream;
  std::vector<broken_bond::gfx::VertexElement> packed_elements;
  broken_bond::gfx::ShaderMicrocode packed_vertex;
  broken_bond::gfx::ShaderMicrocode packed_pixel;
  std::vector<uint8_t> packed_vertex_bytes;
  std::vector<uint8_t> packed_pixel_bytes;
  if (!broken_bond::gfx::ReadDeviceDraw(loaded.data(), loaded.size(), loaded_device, packed_draw,
                                        packed_stream, reason) ||
      !broken_bond::gfx::ReadDeviceDeclaration(loaded.data(), loaded.size(), loaded_device,
                                               packed_elements, reason) ||
      !broken_bond::gfx::ReadVertexShaderMicrocode(loaded.data(), loaded.size(),
                                                   packed_draw.vertex_shader, packed_vertex,
                                                   packed_vertex_bytes, reason) ||
      !broken_bond::gfx::ReadPixelShaderMicrocode(loaded.data(), loaded.size(),
                                                  packed_draw.pixel_shader, packed_pixel,
                                                  packed_pixel_bytes, reason) ||
      packed_draw.stream_bytes != 8u || packed_stream.size() != 8u ||
      packed_draw.stream_stride_dwords != 5u || packed_elements.size() != 1u ||
      packed_elements[0].format != 26u || !packed_elements[0].signed_components ||
      packed_vertex.byte_count != 8u || packed_pixel.byte_count != 4u ||
      packed_vertex_bytes.size() != 8u || packed_pixel_bytes.size() != 4u) {
    return Fail(reason.empty() ? "repacked device draw did not round-trip" : reason);
  }
  for (uint32_t i = 0; i < 8; ++i) {
    if (packed_stream[i] != static_cast<uint8_t>(0x50u + i) ||
        packed_vertex_bytes[i] != static_cast<uint8_t>(0x40u + i)) {
      return Fail("repacked stream or vertex microcode bytes did not match");
    }
  }
  if (packed_pixel_bytes[0] != 0xAB || packed_pixel_bytes[1] != 0xCD ||
      packed_pixel_bytes[2] != 0xEF || packed_pixel_bytes[3] != 0x01) {
    return Fail("repacked pixel microcode bytes did not match");
  }
  std::cout << "  repack_ok 1\n";
  return 0;
}
