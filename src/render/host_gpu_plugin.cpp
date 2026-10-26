// rexgpu-host: the GPU plugin for the native renderer (docs/HOST_RENDERER.md).
//
// The game still needs a GPU behind its ring buffer: D3D waits for the read
// pointer, fences, vblank and swap interrupts. This plugin keeps the SDK's
// PM4 command processor for all of that and draws nothing. The picture comes
// from the title's own renderer, fed by the draw hooks in the executable.
// Select it with --gpu_plugin=host.

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

namespace broken_bond::render {
namespace {

using rex::graphics::CommandProcessor;
using rex::graphics::GraphicsSystem;
using rex::graphics::Shader;
using rex::X_STATUS;

class HostCommandProcessor : public CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;

  void IssueSwap(uint32_t, uint32_t, uint32_t) override {}

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override { shaders_.clear(); }

  // Xenos keeps the active shaders for register-driven state; give it real
  // Shader objects so the base class never sees null.
  Shader* LoadShader(rex::graphics::xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override {
    const uint64_t key = (uint64_t(guest_address) << 8) | uint64_t(shader_type);
    auto it = shaders_.find(key);
    if (it == shaders_.end()) {
      it = shaders_
               .emplace(key, std::make_unique<Shader>(shader_type, key, host_address, dword_count,
                                                      std::endian::big))
               .first;
    }
    return it->second.get();
  }

  bool IssueDraw(rex::graphics::xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override {
    return true;
  }
  bool IssueCopy() override { return true; }

 private:
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
};

class HostGraphicsSystem : public GraphicsSystem {
 public:
  std::string name() const override { return "host"; }

  // No SDK presenter: the window stays free for the title's own swapchain.
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext*) override {
    return X_STATUS_SUCCESS;
  }

 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<HostCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace
}  // namespace broken_bond::render

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}

extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t abi_version, const rex::system::GpuCreateInfo* info) {
  if (abi_version != rex::system::kGpuPluginAbiVersion || !info ||
      info->struct_size < sizeof(rex::system::GpuCreateInfo)) {
    REXLOG_ERROR("rexgpu-host: ABI {} or create info does not match the host", abi_version);
    return nullptr;
  }
  return new broken_bond::render::HostGraphicsSystem();
}
