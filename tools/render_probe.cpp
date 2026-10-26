// Opens a Plume Vulkan device and creates a shader module from every SPIR-V
// shader in the XenosRecomp cache built from this dump's xeshader.sdb.
// Draws nothing and writes nothing.

#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

#include <plume_vulkan.h>
#include <smolv.h>
#include <zstd.h>

#include "gfx/shader_cache.h"

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace {

const char* DeviceType(plume::RenderDeviceType type) {
  switch (type) {
    case plume::RenderDeviceType::INTEGRATED:
      return "integrated";
    case plume::RenderDeviceType::DISCRETE:
      return "discrete";
    case plume::RenderDeviceType::VIRTUAL:
      return "virtual";
    case plume::RenderDeviceType::CPU:
      return "cpu";
    default:
      return "unknown";
  }
}

}  // namespace

int main() {
  std::unique_ptr<plume::RenderInterface> render_interface = plume::CreateVulkanInterface();
  if (render_interface == nullptr) {
    std::fprintf(stderr, "plume: no Vulkan interface\n");
    return 1;
  }
  std::unique_ptr<plume::RenderDevice> device = render_interface->createDevice();
  if (device == nullptr) {
    std::fprintf(stderr, "plume: no Vulkan device\n");
    return 1;
  }
  const plume::RenderDeviceDescription& description = device->getDescription();
  std::printf("device %s (%s), driver 0x%llX\n", description.name.c_str(),
              DeviceType(description.type),
              static_cast<unsigned long long>(description.driverVersion));

  std::vector<uint8_t> spirv(g_spirvCacheDecompressedSize);
  const size_t inflated = ZSTD_decompress(spirv.data(), spirv.size(), g_compressedSpirvCache,
                                          g_spirvCacheCompressedSize);
  if (ZSTD_isError(inflated) || inflated != spirv.size()) {
    std::fprintf(stderr, "zstd: spirv cache did not inflate to %zu bytes\n", spirv.size());
    return 1;
  }
  std::printf("cache %zu shaders, spirv %zu bytes (%zu compressed)\n", g_shaderCacheEntryCount,
              spirv.size(), g_spirvCacheCompressedSize);

  size_t decode_failed = 0;
  size_t module_failed = 0;
  size_t created = 0;
  size_t spec_constant_shaders = 0;
  size_t words = 0;
  std::vector<uint8_t> code;
  const auto start = std::chrono::steady_clock::now();
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    const ShaderCacheEntry& entry = g_shaderCacheEntries[i];
    const uint8_t* smol = spirv.data() + entry.spirvOffset;
    code.resize(smolv::GetDecodedBufferSize(smol, entry.spirvSize));
    if (code.empty() || !smolv::Decode(smol, entry.spirvSize, code.data(), code.size())) {
      ++decode_failed;
      continue;
    }
    words += code.size() / 4;
    if (entry.specConstantsMask != 0) {
      ++spec_constant_shaders;
    }
    std::unique_ptr<plume::RenderShader> shader =
        device->createShader(code.data(), code.size(), "main", plume::RenderShaderFormat::SPIRV);
    auto* vulkan_shader = static_cast<plume::VulkanShader*>(shader.get());
    if (vulkan_shader == nullptr || vulkan_shader->vk == VK_NULL_HANDLE) {
      if (module_failed == 0) {
        std::fprintf(stderr, "first failed module: hash 0x%016llX\n",
                     static_cast<unsigned long long>(entry.hash));
      }
      ++module_failed;
      continue;
    }
    ++created;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - start);
  std::printf("modules %zu created, %zu decode failed, %zu module failed, %lld ms\n", created,
              decode_failed, module_failed, static_cast<long long>(elapsed.count()));
  std::printf("  spirv words %zu, spec-constant shaders %zu\n", words, spec_constant_shaders);
  return (decode_failed == 0 && module_failed == 0) ? 0 : 1;
}
