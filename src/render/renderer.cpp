#include "render/renderer.h"

#include <cmath>
#include <vector>

#include <plume_render_interface.h>

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace broken_bond::render {

namespace {
constexpr plume::RenderFormat kSwapChainFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr uint32_t kSwapChainBuffers = 2;
}  // namespace

struct Renderer::State {
  std::unique_ptr<plume::RenderInterface> render_interface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandList> commands;
  std::unique_ptr<plume::RenderCommandFence> fence;
  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
  std::string device_name;
  uint64_t presents = 0;

  void CreateFramebuffers() {
    framebuffers.clear();
    for (uint32_t i = 0; i < swap_chain->getTextureCount(); ++i) {
      const plume::RenderTexture* color = swap_chain->getTexture(i);
      plume::RenderFramebufferDesc desc;
      desc.colorAttachments = &color;
      desc.colorAttachmentsCount = 1;
      framebuffers.push_back(device->createFramebuffer(desc));
    }
    while (release.size() < swap_chain->getTextureCount()) {
      release.push_back(device->createCommandSemaphore());
    }
  }
};

Renderer::Renderer() : state_(std::make_unique<State>()) {}

Renderer::~Renderer() {
  if (state_->queue && state_->fence) state_->queue->waitForCommandFence(state_->fence.get());
}

const std::string& Renderer::device_name() const { return state_->device_name; }

bool Renderer::Open(void* native_window, std::string& reason) {
  State& s = *state_;
  s.render_interface = plume::CreateVulkanInterface();
  if (!s.render_interface) {
    reason = "no Vulkan interface";
    return false;
  }
  s.device = s.render_interface->createDevice();
  if (!s.device) {
    reason = "no Vulkan device";
    return false;
  }
  s.device_name = s.device->getDescription().name;
  s.queue = s.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  s.fence = s.device->createCommandFence();
  s.commands = s.queue->createCommandList();
  s.acquire = s.device->createCommandSemaphore();
  s.swap_chain = s.queue->createSwapChain(plume::RenderSwapChainDesc(
      static_cast<plume::RenderWindow>(native_window), kSwapChainFormat, kSwapChainBuffers));
  if (!s.swap_chain || !s.swap_chain->resize()) {
    reason = "no swapchain on the game window";
    return false;
  }
  s.CreateFramebuffers();
  return true;
}

void Renderer::Present() {
  State& s = *state_;
  if (s.swap_chain->needsResize()) {
    s.queue->waitForCommandFence(s.fence.get());
    if (!s.swap_chain->resize()) return;
    s.CreateFramebuffers();
  }
  if (s.swap_chain->isEmpty()) return;
  uint32_t image = 0;
  if (!s.swap_chain->acquireTexture(s.acquire.get(), &image)) return;

  plume::RenderTexture* target = s.swap_chain->getTexture(image);
  s.commands->begin();
  s.commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
  s.commands->setFramebuffer(s.framebuffers[image].get());
  const uint32_t width = s.swap_chain->getWidth();
  const uint32_t height = s.swap_chain->getHeight();
  s.commands->setViewports(plume::RenderViewport(0.0f, 0.0f, float(width), float(height)));
  s.commands->setScissors(plume::RenderRect(0, 0, width, height));
  // Until guest draws are translated: a colour that moves with every present,
  // so the cadence is visible.
  const float t = float(s.presents++ % 120) / 120.0f;
  s.commands->clearColor(0, plume::RenderColor(0.1f, 0.2f + 0.3f * std::sin(t * 6.2831853f), 0.4f, 1.0f));
  s.commands->barriers(plume::RenderBarrierStage::NONE,
                       plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
  s.commands->end();

  const plume::RenderCommandList* list = s.commands.get();
  plume::RenderCommandSemaphore* wait = s.acquire.get();
  plume::RenderCommandSemaphore* signal = s.release[image].get();
  s.queue->executeCommandLists(&list, 1, &wait, 1, &signal, 1, s.fence.get());
  s.swap_chain->present(image, &signal, 1);
  s.queue->waitForCommandFence(s.fence.get());
}

}  // namespace broken_bond::render
