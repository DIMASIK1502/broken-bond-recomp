// Opens the host Vulkan device, checks one shader module, untile of an RGBA8
// image, and a GPU readback of that image. Does not start the game.

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "gfx/device.h"
#include "gfx/texture.h"

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
  return 0;
}
