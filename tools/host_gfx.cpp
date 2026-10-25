// Opens the host Vulkan device, checks one shader module, untile of an RGBA8
// image, and a GPU readback of that image. Does not start the game.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "gfx/device.h"
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
  return 0;
}
