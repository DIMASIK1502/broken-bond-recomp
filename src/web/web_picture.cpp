#include "web/web_picture.h"
#include "web/web_draw.h"

#include "gfx/texture.h"
#include "web/guest_fold.h"

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <emscripten.h>
#include <emscripten/threading.h>

#include <plume_device_thread.h>
#include <plume_render_interface.h>
#include <plume_webgpu.h>

#include <rex/graphics/xenos.h>

#include "broken_bond_pch.h"

namespace broken_bond::web {
namespace {

constexpr uint32_t kMaxEdge = 2048;
constexpr uint64_t kMaxUpload = 16ull * 1024ull * 1024ull;

const char kBlitWgsl[] = R"(
struct VsOut {
  @builtin(position) pos: vec4f,
  @location(0) uv: vec2f,
}

@group(0) @binding(0) var src: texture_2d<f32>;
@group(1) @binding(0) var src_sampler: sampler;

@vertex
fn VSMain(@builtin(vertex_index) id: u32) -> VsOut {
  let xy = vec2f(f32((id << 1u) & 2u), f32(id & 2u));
  var out: VsOut;
  out.pos = vec4f(xy * 2.0 - 1.0, 0.0, 1.0);
  out.uv = xy * 0.5;
  return out;
}

@fragment
fn PSMain(in: VsOut) -> @location(0) vec4f {
  return textureSample(src, src_sampler, in.uv);
}
)";

struct Picture {
  std::unique_ptr<plume::RenderInterface> interface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandList> commands;
  std::unique_ptr<plume::RenderCommandFence> fence;
  std::unique_ptr<plume::RenderSwapChain> swap;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> framebuffers;
  std::unique_ptr<plume::RenderShader> vertex_shader;
  std::unique_ptr<plume::RenderShader> pixel_shader;
  std::unique_ptr<plume::RenderPipelineLayout> layout;
  std::unique_ptr<plume::RenderPipeline> pipeline;
  std::unique_ptr<plume::RenderDescriptorSet> textures;
  std::unique_ptr<plume::RenderDescriptorSet> samplers;
  std::unique_ptr<plume::RenderSampler> sampler;
  std::unique_ptr<plume::RenderTexture> image;
  std::unique_ptr<plume::RenderTextureView> image_view;
  std::unique_ptr<plume::RenderBuffer> upload;
  uint32_t image_width = 0;
  uint32_t image_height = 0;
  uint32_t row_texels = 0;
  uint32_t fb_width = 0;
  uint32_t fb_height = 0;
  plume::RenderFormat swap_format = plume::RenderFormat::R8G8B8A8_UNORM;
  bool ready = false;
  bool logged_canvas = false;
  bool logged_image = false;
  bool logged_fetch = false;
};

Picture& State() {
  static Picture picture;
  return picture;
}

bool InWindow(uint32_t address, uint32_t bytes) {
  if (bytes != 0 && address > UINT32_MAX - bytes) return false;
  const int64_t offset = HostOffset(address);
  if (offset < 0) return false;
  return uint64_t(offset) + bytes <= kGuestWindowSize;
}

const uint8_t* Guest(uint8_t* base, uint32_t address) {
  return base + HostOffset(address);
}

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

bool DecodeFrontbuffer(uint32_t texture, uint8_t* base, std::vector<uint8_t>& rgba, uint32_t& width,
                       uint32_t& height, std::string& reason) {
  if (texture == 0 || base == nullptr) {
    reason = "no frontbuffer";
    return false;
  }
  if (!InWindow(texture, 52)) {
    reason = "frontbuffer object is outside guest memory";
    return false;
  }
  uint32_t words[6] = {};
  const uint8_t* object = Guest(base, texture + 28);
  for (uint32_t i = 0; i < 6; ++i) words[i] = Be32(object + i * 4);

  rex::graphics::xenos::xe_gpu_texture_fetch_t fetch{};
  static_assert(sizeof(fetch) >= 24);
  std::memcpy(&fetch, words, 24);
  if (fetch.type != rex::graphics::xenos::FetchConstantType::kTexture ||
      fetch.dimension != rex::graphics::xenos::DataDimension::k2DOrStacked) {
    reason = "frontbuffer is not a 2D texture";
    return false;
  }
  width = fetch.size_2d.width + 1;
  height = fetch.size_2d.height + 1;
  const uint32_t pitch = fetch.pitch << 5;
  const uint32_t format = uint32_t(fetch.format);
  if (width == 0 || height == 0 || width > kMaxEdge || height > kMaxEdge || pitch < width) {
    reason = "frontbuffer size is not usable";
    return false;
  }
  uint32_t guest_address = fetch.base_address << 12;
  if (guest_address < 0x20000000u) guest_address += 0xA0000000u;
  const uint32_t need = gfx::GuestTextureBytes(format, width, height, pitch, fetch.tiled != 0);
  if (need == 0 || !InWindow(guest_address, need)) {
    reason = "frontbuffer texels are outside guest memory";
    return false;
  }
  if (!gfx::LoadGuestTextureRgba8(format, width, height, pitch, uint32_t(fetch.endianness),
                                  fetch.tiled != 0, Guest(base, guest_address), need, rgba, reason)) {
    return false;
  }
  return rgba.size() == size_t(width) * height * 4u;
}

bool HasInk(const std::vector<uint8_t>& pixels) {
  for (uint8_t byte : pixels) {
    if (byte != 0) return true;
  }
  return false;
}

bool EnsureFramebuffers(Picture& picture) {
  const uint32_t width = picture.swap->getWidth();
  const uint32_t height = picture.swap->getHeight();
  if (width == 0 || height == 0) return false;
  if (width == picture.fb_width && height == picture.fb_height && !picture.framebuffers.empty()) return true;
  picture.framebuffers.clear();
  const uint32_t count = picture.swap->getTextureCount();
  picture.framebuffers.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const plume::RenderTexture* color = picture.swap->getTexture(i);
    plume::RenderFramebufferDesc framebuffer;
    framebuffer.colorAttachments = &color;
    framebuffer.colorAttachmentsCount = 1;
    auto made = picture.device->createFramebuffer(framebuffer);
    if (!made) return false;
    picture.framebuffers.push_back(std::move(made));
  }
  picture.fb_width = width;
  picture.fb_height = height;
  if (!picture.logged_canvas) {
    picture.logged_canvas = true;
    std::cout << "broken_bond web: canvas " << width << "x" << height << "\n";
  }
  return true;
}

bool EnsureImage(Picture& picture, const uint8_t* rgba, uint32_t width, uint32_t height) {
  const uint32_t row_bytes = (width * 4u + 255u) & ~255u;
  const uint32_t row_texels = row_bytes / 4u;
  const uint64_t bytes = uint64_t(row_bytes) * height;
  if (bytes == 0 || bytes >= kMaxUpload) return false;
  if (!picture.image || picture.image_width != width || picture.image_height != height) {
    picture.image_view.reset();
    picture.image = picture.device->createTexture(
        plume::RenderTextureDesc::Texture2D(width, height, 1, plume::RenderFormat::R8G8B8A8_UNORM));
    if (!picture.image) return false;
    picture.image_view =
        picture.image->createTextureView(plume::RenderTextureViewDesc::Texture2D(plume::RenderFormat::R8G8B8A8_UNORM));
    if (!picture.image_view) return false;
    picture.image_width = width;
    picture.image_height = height;
  }
  if (!picture.upload || picture.row_texels != row_texels) {
    picture.upload = picture.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(bytes));
    picture.row_texels = row_texels;
  }
  if (!picture.upload) return false;
  auto* dst = static_cast<uint8_t*>(picture.upload->map());
  if (dst == nullptr) return false;
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(dst + uint64_t(y) * row_bytes, rgba + uint64_t(y) * width * 4u, width * 4u);
  }
  picture.upload->unmap();
  picture.textures->setTexture(0, picture.image.get(), plume::RenderTextureLayout::SHADER_READ,
                               picture.image_view.get());
  return picture.pipeline != nullptr;
}

void Present(const uint8_t* rgba, uint32_t width, uint32_t height) {
  Picture& picture = State();
  if (!picture.ready) return;
  if (!EnsureFramebuffers(picture)) return;

  const bool blit = rgba != nullptr && EnsureImage(picture, rgba, width, height);
  uint32_t image = 0;
  if (!picture.swap->acquireTexture(picture.acquire.get(), &image) || image >= picture.framebuffers.size()) return;

  plume::RenderTexture* target = picture.swap->getTexture(image);
  picture.commands->begin();
  if (blit) {
    picture.commands->barriers(plume::RenderBarrierStage::COPY, plume::RenderTextureBarrier(
                                                                    picture.image.get(), plume::RenderTextureLayout::COPY_DEST));
    picture.commands->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(picture.image.get()),
        plume::RenderTextureCopyLocation::PlacedFootprint(picture.upload.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                          width, height, 1, picture.row_texels));
    picture.commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                               plume::RenderTextureBarrier(picture.image.get(), plume::RenderTextureLayout::SHADER_READ));
  }
  picture.commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                             plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
  picture.commands->setFramebuffer(picture.framebuffers[image].get());
  picture.commands->setViewports(
      plume::RenderViewport(0.0f, 0.0f, float(picture.fb_width), float(picture.fb_height)));
  picture.commands->setScissors(plume::RenderRect(0, 0, picture.fb_width, picture.fb_height));
  picture.commands->clearColor(0, plume::RenderColor(0.15f, 0.35f, 0.75f, 1.0f));
  if (blit) {
    picture.commands->setGraphicsPipelineLayout(picture.layout.get());
    picture.commands->setPipeline(picture.pipeline.get());
    picture.commands->setGraphicsDescriptorSet(picture.textures.get(), 0);
    picture.commands->setGraphicsDescriptorSet(picture.samplers.get(), 1);
    picture.commands->drawInstanced(3, 1, 0, 0);
  }
  picture.commands->barriers(plume::RenderBarrierStage::NONE,
                             plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
  picture.commands->end();

  while (picture.release.size() < picture.swap->getTextureCount()) {
    picture.release.push_back(picture.device->createCommandSemaphore());
  }
  const plume::RenderCommandList* lists = picture.commands.get();
  plume::RenderCommandSemaphore* wait = picture.acquire.get();
  plume::RenderCommandSemaphore* signal = picture.release[image].get();
  picture.queue->executeCommandLists(&lists, 1, &wait, 1, &signal, 1, picture.fence.get());
  picture.swap->present(image, &signal, 1);
  picture.queue->waitForCommandFence(picture.fence.get());
}

bool CreateBlitPipeline(Picture& picture) {
  const plume::RenderShaderFormat format = picture.interface->getCapabilities().shaderFormat;
  if (format != plume::RenderShaderFormat::WGSL) {
    std::cout << "broken_bond web: shader format is not WGSL\n";
    return false;
  }
  picture.vertex_shader = picture.device->createShader(kBlitWgsl, sizeof(kBlitWgsl), "VSMain", format);
  picture.pixel_shader = picture.device->createShader(kBlitWgsl, sizeof(kBlitWgsl), "PSMain", format);
  if (!picture.vertex_shader || !picture.pixel_shader) return false;

  const plume::RenderDescriptorRange texture_range(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
  const plume::RenderDescriptorRange sampler_range(plume::RenderDescriptorRangeType::SAMPLER, 0, 1);
  const plume::RenderDescriptorSetDesc set_descs[2] = {
      plume::RenderDescriptorSetDesc(&texture_range, 1),
      plume::RenderDescriptorSetDesc(&sampler_range, 1),
  };
  picture.textures = picture.device->createDescriptorSet(set_descs[0]);
  picture.samplers = picture.device->createDescriptorSet(set_descs[1]);
  picture.sampler = picture.device->createSampler(plume::RenderSamplerDesc());
  if (!picture.textures || !picture.samplers || !picture.sampler) return false;
  picture.samplers->setSampler(0, picture.sampler.get());

  picture.layout = picture.device->createPipelineLayout(plume::RenderPipelineLayoutDesc(nullptr, 0, set_descs, 2));
  if (!picture.layout) return false;

  plume::RenderGraphicsPipelineDesc pipeline;
  pipeline.pipelineLayout = picture.layout.get();
  pipeline.vertexShader = picture.vertex_shader.get();
  pipeline.pixelShader = picture.pixel_shader.get();
  pipeline.renderTargetFormat[0] = picture.swap_format;
  pipeline.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
  pipeline.renderTargetCount = 1;
  pipeline.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
  picture.pipeline = picture.device->createGraphicsPipeline(pipeline);
  return picture.pipeline != nullptr;
}

}  // namespace

bool StartPicture() {
  Picture& picture = State();
  std::cout << "broken_bond web: opening WebGPU" << std::endl;
  auto device_thread = plume::CreateEmscriptenDeviceThread(emscripten_main_runtime_thread_id());
  picture.interface = plume::CreateDeviceThreadInterface(std::move(device_thread), [] {
    return plume::CreateWebGPUInterface();
  });
  if (!picture.interface) {
    std::cout << "broken_bond web: no WebGPU interface\n";
    return false;
  }
  picture.device = picture.interface->createDevice();
  if (!picture.device) {
    std::cout << "broken_bond web: WebGPU device was not created\n";
    return false;
  }
  picture.swap_format = picture.device->getCapabilities().preferredSwapChainFormat;
  picture.queue = picture.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  picture.fence = picture.device->createCommandFence();
  picture.swap = picture.queue->createSwapChain(plume::RenderSwapChainDesc("#canvas", picture.swap_format, 2));
  if (!picture.swap || !picture.swap->resize()) {
    std::cout << "broken_bond web: swapchain on #canvas was not created\n";
    return false;
  }
  picture.commands = picture.queue->createCommandList();
  picture.acquire = picture.device->createCommandSemaphore();
  if (!picture.commands || !picture.acquire) return false;
  if (!CreateBlitPipeline(picture)) {
    std::cout << "broken_bond web: blit pipeline was not created\n";
  }
  picture.ready = true;
  Present(nullptr, 0, 0);
  std::cout << "broken_bond web: picture presented" << std::endl;
  return true;
}

extern "C" EMSCRIPTEN_KEEPALIVE int broken_bond_web_start_picture() {
  return StartPicture() ? 0 : 1;
}

void ReleasePicture() { State() = Picture{}; }

void ShowFrontbuffer(uint32_t texture, uint8_t* base) {
  Picture& picture = State();
  if (!picture.ready) return;
  std::vector<uint8_t> rgba;
  uint32_t width = 0;
  uint32_t height = 0;
  std::string reason;
  const bool decoded = DecodeFrontbuffer(texture, base, rgba, width, height, reason);
  const bool ink = decoded && HasInk(rgba);
  if (decoded && !picture.logged_image) {
    picture.logged_image = true;
    std::cout << "broken_bond web: frontbuffer " << width << "x" << height
              << (ink ? " has pixels" : " decoded empty") << std::endl;
  } else if (!decoded && !picture.logged_fetch) {
    picture.logged_fetch = true;
    std::cout << "broken_bond web: " << reason << std::endl;
  }
  Present(ink ? rgba.data() : nullptr, width, height);
}

}  // namespace broken_bond::web

extern "C" void __imp__sub_821B1DD0(PPCContext& __restrict ctx, uint8_t* base);
REX_HOOK_RAW(sub_821B1DD0) {
  if (!broken_bond::web::PresentRenderer(ctx.r4.u32, base)) {
    broken_bond::web::ShowFrontbuffer(ctx.r4.u32, base);
  }
  __imp__sub_821B1DD0(ctx, base);
}
