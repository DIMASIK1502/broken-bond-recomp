#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <plume_render_interface.h>
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>
#include <smolv.h>
#include <zstd.h>

#include "gfx/shader_cache.h"
#include "gfx/texture.h"
#include "gfx/xenos_shader_common.h"
#include "host/shadow_draw.h"

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

namespace broken_bond::render {

namespace {

namespace xenos = rex::graphics::xenos;
using broken_bond::gfx::DeclUsage;

constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT;
constexpr uint32_t kSwapChainBuffers = 2;
constexpr uint64_t kUploadBytes = 256ull << 20;
constexpr uint32_t kTexture2DCount = 8192;
constexpr uint32_t kTexture3DCount = 64;
constexpr uint32_t kTextureCubeCount = 256;
constexpr uint32_t kTexture1DCount = 64;
constexpr uint32_t kSamplerCount = 1024;

// Guest frame the D3D viewport registers describe; the host target scales it.
constexpr float kGuestWidth = 1280.0f;
constexpr float kGuestHeight = 720.0f;

// ---------------------------------------------------------------------------
// Guest memory and the D3D device (docs/D3D_MAP.md).

const uint8_t* At(const uint8_t* base, uint32_t address) {
  return rex::memory::GuestPtr<const uint8_t*>(const_cast<uint8_t*>(base), address);
}

uint32_t Be32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = At(base, address);
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

uint16_t Be16(const uint8_t* base, uint32_t address) {
  const uint8_t* p = At(base, address);
  return uint16_t((p[0] << 8) | p[1]);
}

float BeFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = Be32(base, address);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

// Fetch constants and D3D objects hold physical addresses; any of the
// 0xA0000000+ views reaches them.
uint32_t PhysicalView(uint32_t physical) { return 0xA0000000u | (physical & 0x1FFFFFFFu); }

constexpr uint32_t kDeviceFetch = 1152;
constexpr uint32_t kDeviceVsConstants = 1920;
constexpr uint32_t kDevicePsConstants = 6016;
constexpr uint32_t kDeviceBools = 10112;
constexpr uint32_t kDeviceDecl = 11820;
constexpr uint32_t kDeviceIndices = 12436;
constexpr uint32_t kDeviceStrides = 12528;
constexpr uint32_t kDevicePixelShader = 12692;
constexpr uint32_t kDeviceVertexShader = 12696;

struct RegisterBlock {
  uint32_t first;
  uint32_t offset;
  uint32_t count;
};
constexpr RegisterBlock kRegisterBlocks[] = {
    {0x2000, 10368, 16}, {0x2100, 10444, 21}, {0x2180, 10528, 5},  {0x2200, 10548, 12},
    {0x2280, 10596, 21}, {0x2300, 10680, 38}, {0x2380, 10832, 8},
};

bool RegisterOffset(uint32_t reg, uint32_t& offset) {
  for (const RegisterBlock& block : kRegisterBlocks) {
    if (reg >= block.first && reg < block.first + block.count) {
      offset = block.offset + 4 * (reg - block.first);
      return true;
    }
  }
  return false;
}

uint32_t Reg(const uint8_t* base, uint32_t device, uint32_t reg) {
  uint32_t offset = 0;
  return RegisterOffset(reg, offset) ? Be32(base, device + offset) : 0;
}

float RegFloat(const uint8_t* base, uint32_t device, uint32_t reg) {
  const uint32_t bits = Reg(base, device, reg);
  float value;
  std::memcpy(&value, &bits, 4);
  return value;
}

constexpr uint32_t kRbDepthControl = 0x2200;
constexpr uint32_t kRbBlendControl0 = 0x2201;
constexpr uint32_t kRbColorControl = 0x2202;
constexpr uint32_t kPaSuScModeCntl = 0x2205;
constexpr uint32_t kRbColorMask = 0x2104;
constexpr uint32_t kRbAlphaRef = 0x210E;
constexpr uint32_t kPaClVportXScale = 0x210F;
constexpr uint32_t kPaClVportXOffset = 0x2110;
constexpr uint32_t kPaClVportYScale = 0x2111;
constexpr uint32_t kPaClVportYOffset = 0x2112;
constexpr uint32_t kPaClVportZScale = 0x2113;
constexpr uint32_t kPaClVportZOffset = 0x2114;

void SwapDwords(uint8_t* data, size_t bytes, uint32_t endian) {
  // xenos::Endian: 0 none, 1 8in16, 2 8in32, 3 16in32.
  if (endian == 1) {
    for (size_t i = 0; i + 1 < bytes; i += 2) std::swap(data[i], data[i + 1]);
  } else if (endian == 2) {
    for (size_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(data[i], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if (endian == 3) {
    for (size_t i = 0; i + 3 < bytes; i += 4) {
      std::swap(data[i], data[i + 2]);
      std::swap(data[i + 1], data[i + 3]);
    }
  }
}

// ---------------------------------------------------------------------------
// Translation tables.

plume::RenderComparisonFunction CompareFunction(uint32_t xenos_func) {
  // xenos::CompareFunction 0..7 is never, less, equal, lequal, greater,
  // notequal, gequal, always: Plume's order after UNKNOWN.
  return static_cast<plume::RenderComparisonFunction>(1 + (xenos_func & 7));
}

plume::RenderBlend BlendFactor(uint32_t factor) {
  using B = plume::RenderBlend;
  switch (factor) {
    case 0: return B::ZERO;
    case 1: return B::ONE;
    case 4: return B::SRC_COLOR;
    case 5: return B::INV_SRC_COLOR;
    case 6: return B::SRC_ALPHA;
    case 7: return B::INV_SRC_ALPHA;
    case 8: return B::DEST_COLOR;
    case 9: return B::INV_DEST_COLOR;
    case 10: return B::DEST_ALPHA;
    case 11: return B::INV_DEST_ALPHA;
    case 12: case 14: return B::BLEND_FACTOR;
    case 13: case 15: return B::INV_BLEND_FACTOR;
    case 16: return B::SRC_ALPHA_SAT;
    default: return B::ONE;
  }
}

plume::RenderBlendOperation BlendOp(uint32_t op) {
  using O = plume::RenderBlendOperation;
  switch (op) {
    case 0: return O::ADD;
    case 1: return O::SUBTRACT;
    case 2: return O::MIN;
    case 3: return O::MAX;
    case 4: return O::REV_SUBTRACT;
    default: return O::ADD;
  }
}

// Host format of one declaration element. XenosRecomp reads NORMAL, TANGENT,
// BINORMAL and BLENDINDICES as uint4 (the first three as raw float bits or
// packed 11:11:10), everything else as float4.
bool ElementFormat(uint32_t usage, uint32_t format_word, plume::RenderFormat& out,
                   bool& packed_normal) {
  using F = plume::RenderFormat;
  const auto format = static_cast<xenos::VertexFormat>(format_word & 0x3F);
  const bool is_signed = (format_word & 0x100) != 0;
  const bool is_integer = (format_word & 0x200) != 0;
  const bool raw = usage == uint32_t(DeclUsage::kNormal) || usage == uint32_t(DeclUsage::kTangent) ||
                   usage == uint32_t(DeclUsage::kBinormal);
  const bool uint_input = raw || usage == uint32_t(DeclUsage::kBlendIndices);
  packed_normal = false;
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
      if (uint_input) out = F::R8G8B8A8_UINT;
      else if (is_integer) return false;
      else out = is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
      return true;
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
      if (!raw) return false;
      out = F::R32_UINT;
      packed_normal = true;
      return true;
    case xenos::VertexFormat::k_16_16:
      if (uint_input || is_integer) return false;
      out = is_signed ? F::R16G16_SNORM : F::R16G16_UNORM;
      return true;
    case xenos::VertexFormat::k_16_16_16_16:
      if (uint_input || is_integer) return false;
      out = is_signed ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM;
      return true;
    case xenos::VertexFormat::k_16_16_FLOAT:
      if (uint_input) return false;
      out = F::R16G16_FLOAT;
      return true;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      if (uint_input) return false;
      out = F::R16G16B16A16_FLOAT;
      return true;
    case xenos::VertexFormat::k_32_FLOAT:
      out = uint_input ? F::R32_UINT : F::R32_FLOAT;
      return true;
    case xenos::VertexFormat::k_32_32_FLOAT:
      out = uint_input ? F::R32G32_UINT : F::R32G32_FLOAT;
      return true;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      out = uint_input ? F::R32G32B32_UINT : F::R32G32B32_FLOAT;
      return true;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      out = uint_input ? F::R32G32B32A32_UINT : F::R32G32B32A32_FLOAT;
      return true;
    default:
      return false;
  }
}

uint32_t UsageLocation(uint32_t usage, uint32_t index) {
  for (const auto& location : broken_bond::gfx::kVertexLocations) {
    if (uint32_t(location.usage) == usage && location.usage_index == index) return location.location;
  }
  return UINT32_MAX;
}

const char* UsageSemantic(uint32_t usage) {
  switch (static_cast<DeclUsage>(usage)) {
    case DeclUsage::kPosition: return "POSITION";
    case DeclUsage::kBlendWeight: return "BLENDWEIGHT";
    case DeclUsage::kBlendIndices: return "BLENDINDICES";
    case DeclUsage::kNormal: return "NORMAL";
    case DeclUsage::kTexCoord: return "TEXCOORD";
    case DeclUsage::kTangent: return "TANGENT";
    case DeclUsage::kBinormal: return "BINORMAL";
    case DeclUsage::kColor: return "COLOR";
  }
  return "TEXCOORD";
}

plume::RenderTextureAddressMode AddressMode(uint32_t clamp) {
  using A = plume::RenderTextureAddressMode;
  switch (clamp) {
    case 0: return A::WRAP;
    case 1: return A::MIRROR;
    case 2: return A::CLAMP;
    case 3: return A::MIRROR_ONCE;
    default: return A::BORDER;
  }
}

}  // namespace

// ---------------------------------------------------------------------------

struct Renderer::State {
  std::unique_ptr<plume::RenderInterface> render_interface;
  std::unique_ptr<plume::RenderDevice> device;
  std::unique_ptr<plume::RenderCommandQueue> queue;
  std::unique_ptr<plume::RenderCommandList> commands;
  std::unique_ptr<plume::RenderCommandFence> fence;
  std::unique_ptr<plume::RenderSwapChain> swap_chain;
  std::unique_ptr<plume::RenderCommandSemaphore> acquire;
  std::vector<std::unique_ptr<plume::RenderCommandSemaphore>> release;
  std::string device_name;

  // Target the guest frame is drawn into, at swapchain size.
  std::unique_ptr<plume::RenderTexture> color;
  std::unique_ptr<plume::RenderTexture> depth;
  std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  uint32_t width = 0;
  uint32_t height = 0;

  // Per-frame upload ring: constants, vertices, indices, texels.
  std::unique_ptr<plume::RenderBuffer> upload;
  uint8_t* upload_cpu = nullptr;
  uint64_t upload_gpu = 0;
  uint64_t upload_used = 0;

  std::unique_ptr<plume::RenderPipelineLayout> layout;
  std::unique_ptr<plume::RenderDescriptorSet> sets[6];

  // Shader cache from XenosRecomp, decompressed once.
  std::vector<uint8_t> spirv;
  std::unordered_map<uint64_t, const ShaderCacheEntry*> entries;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderShader>> shaders;
  std::unordered_map<uint64_t, std::unique_ptr<plume::RenderPipeline>> pipelines;

  struct Texture {
    std::unique_ptr<plume::RenderTexture> texture;
    uint32_t index = 0;
  };
  std::unordered_map<uint64_t, Texture> textures;
  uint32_t next_texture2d = 1;  // 0 is white in every heap
  std::vector<std::unique_ptr<plume::RenderTexture>> defaults;
  std::unordered_map<uint32_t, uint32_t> sampler_index;
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers;

  std::unordered_map<uint64_t, uint64_t> frame_streams;  // (address, bytes, endian) -> ring
  bool recording = false;
  uint64_t presents = 0;

  std::mutex mutex;  // Draw and Present run on the guest thread, TakeStats on the sampler
  std::map<std::string, uint64_t> skipped;
  uint64_t drawn = 0;

  bool Upload(size_t bytes, size_t align, uint64_t& offset) {
    const uint64_t start = (upload_used + align - 1) & ~uint64_t(align - 1);
    if (start + bytes > kUploadBytes) return false;
    offset = start;
    upload_used = start + bytes;
    return true;
  }

  void Skip(const std::string& reason) { ++skipped[reason]; }

  bool CreateTarget() {
    width = swap_chain->getWidth();
    height = swap_chain->getHeight();
    color = device->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, kColorFormat));
    depth = device->createTexture(plume::RenderTextureDesc::DepthTarget(width, height, kDepthFormat));
    const plume::RenderTexture* attachment = color.get();
    plume::RenderFramebufferDesc desc;
    desc.colorAttachments = &attachment;
    desc.colorAttachmentsCount = 1;
    desc.depthAttachment = depth.get();
    framebuffer = device->createFramebuffer(desc);
    return color && depth && framebuffer;
  }

  void BeginFrame() {
    if (recording) return;
    recording = true;
    upload_used = 0;
    frame_streams.clear();
    commands->begin();
    plume::RenderTextureBarrier barriers[] = {
        plume::RenderTextureBarrier(color.get(), plume::RenderTextureLayout::COLOR_WRITE),
        plume::RenderTextureBarrier(depth.get(), plume::RenderTextureLayout::DEPTH_WRITE)};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, 2);
    BindTarget();
    commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
    commands->clearDepthStencil(true, true, 1.0f, 0);
  }

  void BindTarget() {
    commands->setFramebuffer(framebuffer.get());
    commands->setGraphicsPipelineLayout(layout.get());
    for (uint32_t i = 0; i < 6; ++i) commands->setGraphicsDescriptorSet(sets[i].get(), i);
  }

  bool CreateLayout() {
    using R = plume::RenderDescriptorRangeType;
    const plume::RenderDescriptorRange ranges[6] = {
        plume::RenderDescriptorRange(R::TEXTURE, 0, kTexture2DCount),
        plume::RenderDescriptorRange(R::TEXTURE, 0, kTexture3DCount),
        plume::RenderDescriptorRange(R::TEXTURE, 0, kTextureCubeCount),
        plume::RenderDescriptorRange(R::SAMPLER, 0, kSamplerCount),
        plume::RenderDescriptorRange(R::SAMPLER, 0, 1),  // set 4 is unused by the shaders
        plume::RenderDescriptorRange(R::TEXTURE, 0, kTexture1DCount),
    };
    plume::RenderDescriptorSetDesc set_descs[6];
    for (uint32_t i = 0; i < 6; ++i) {
      const bool boundless = i != 4;
      set_descs[i] = plume::RenderDescriptorSetDesc(&ranges[i], 1, boundless,
                                                    boundless ? ranges[i].count : 0);
      sets[i] = device->createDescriptorSet(set_descs[i]);
      if (!sets[i]) return false;
    }
    const plume::RenderPushConstantRange push(0, 0, 0, 24,
                                              plume::RenderShaderStageFlag::VERTEX |
                                                  plume::RenderShaderStageFlag::PIXEL);
    plume::RenderPipelineLayoutDesc desc(&push, 1, set_descs, 6, false, true);
    layout = device->createPipelineLayout(desc);
    return layout != nullptr;
  }

  void CreateDefaults() {
    // White 1x1 at index 0 of every texture heap, uploaded on the first frame.
    using F = plume::RenderFormat;
    defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture2D(1, 1, 1, F::R8G8B8A8_UNORM)));
    defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture3D(1, 1, 1, 1, F::R8G8B8A8_UNORM)));
    plume::RenderTextureDesc cube = plume::RenderTextureDesc::Texture2D(1, 1, 1, F::R8G8B8A8_UNORM);
    cube.arraySize = 6;
    cube.flags |= plume::RenderTextureFlag::CUBE;
    defaults.push_back(device->createTexture(cube));
    defaults.push_back(device->createTexture(plume::RenderTextureDesc::Texture1D(1, 1, F::R8G8B8A8_UNORM)));
    samplers.push_back(device->createSampler(plume::RenderSamplerDesc()));
    sets[3]->setSampler(0, samplers[0].get());
    sets[4]->setSampler(0, samplers[0].get());
  }

  bool defaults_uploaded = false;

  void UploadDefaults() {
    if (defaults_uploaded) return;
    defaults_uploaded = true;
    const uint32_t layers[4] = {1, 1, 6, 1};
    for (uint32_t d = 0; d < 4; ++d) {
      for (uint32_t layer = 0; layer < layers[d]; ++layer) {
        uint64_t offset = 0;
        if (!Upload(256, 256, offset)) return;
        std::memset(upload_cpu + offset, 0xFF, 4);
        commands->barriers(plume::RenderBarrierStage::COPY,
                           plume::RenderTextureBarrier(defaults[d].get(), plume::RenderTextureLayout::COPY_DEST));
        commands->copyTextureRegion(
            plume::RenderTextureCopyLocation::Subresource(defaults[d].get(), 0, layer),
            plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                              1, 1, 1, 64, offset));
      }
      commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(defaults[d].get(), plume::RenderTextureLayout::SHADER_READ));
    }
    sets[0]->setTexture(0, defaults[0].get(), plume::RenderTextureLayout::SHADER_READ);
    sets[1]->setTexture(0, defaults[1].get(), plume::RenderTextureLayout::SHADER_READ);
    sets[2]->setTexture(0, defaults[2].get(), plume::RenderTextureLayout::SHADER_READ);
    sets[5]->setTexture(0, defaults[3].get(), plume::RenderTextureLayout::SHADER_READ);
  }

  bool LoadShaderCache() {
    spirv.resize(g_spirvCacheDecompressedSize);
    const size_t n = ZSTD_decompress(spirv.data(), spirv.size(), g_compressedSpirvCache,
                                     g_spirvCacheCompressedSize);
    if (ZSTD_isError(n) || n != spirv.size()) return false;
    for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
      entries[g_shaderCacheEntries[i].hash] = &g_shaderCacheEntries[i];
    }
    return true;
  }

  plume::RenderShader* Shader(uint64_t key) {
    if (key == 0) return nullptr;
    auto found = shaders.find(key);
    if (found != shaders.end()) return found->second.get();
    auto entry = entries.find(key);
    if (entry == entries.end()) return nullptr;
    const uint8_t* smol = spirv.data() + entry->second->spirvOffset;
    std::vector<uint8_t> code(smolv::GetDecodedBufferSize(smol, entry->second->spirvSize));
    if (code.empty() || !smolv::Decode(smol, entry->second->spirvSize, code.data(), code.size())) {
      return nullptr;
    }
    auto shader = device->createShader(code.data(), code.size(), "main", plume::RenderShaderFormat::SPIRV);
    return shaders.emplace(key, std::move(shader)).first->second.get();
  }

  uint32_t SamplerFor(const xenos::xe_gpu_texture_fetch_t& fetch) {
    const uint32_t key = uint32_t(fetch.clamp_x) | (uint32_t(fetch.clamp_y) << 3) |
                         (uint32_t(fetch.clamp_z) << 6) | (uint32_t(fetch.mag_filter) << 9) |
                         (uint32_t(fetch.min_filter) << 11) | (uint32_t(fetch.mip_filter) << 13);
    auto found = sampler_index.find(key);
    if (found != sampler_index.end()) return found->second;
    if (samplers.size() >= kSamplerCount) return 0;
    plume::RenderSamplerDesc desc;
    desc.addressU = AddressMode(uint32_t(fetch.clamp_x));
    desc.addressV = AddressMode(uint32_t(fetch.clamp_y));
    desc.addressW = AddressMode(uint32_t(fetch.clamp_z));
    desc.magFilter = uint32_t(fetch.mag_filter) == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
    desc.minFilter = uint32_t(fetch.min_filter) == 0 ? plume::RenderFilter::NEAREST : plume::RenderFilter::LINEAR;
    desc.mipmapMode = uint32_t(fetch.mip_filter) == 0 ? plume::RenderMipmapMode::NEAREST : plume::RenderMipmapMode::LINEAR;
    const uint32_t index = uint32_t(samplers.size());
    samplers.push_back(device->createSampler(desc));
    sets[3]->setSampler(index, samplers.back().get());
    sampler_index[key] = index;
    return index;
  }

  // Texture index in the 2D heap for one fetch constant, 0 (white) when the
  // format or dimension is not handled yet.
  uint32_t Texture2DFor(const uint32_t words[6], const xenos::xe_gpu_texture_fetch_t& fetch,
                        const uint8_t* base, bool& uploaded) {
    const uint32_t format = uint32_t(fetch.format);
    if (format != 6 && format != 18 && format != 20) {
      Skip(fmt::format("texture format {}", format));
      return 0;
    }
    const uint32_t width = fetch.size_2d.width + 1;
    const uint32_t height = fetch.size_2d.height + 1;
    const uint32_t pitch = fetch.pitch * 32;
    const uint64_t key = XXH3_64bits(words, 24) ^ 0x5bd1e995u;
    auto found = textures.find(key);
    if (found != textures.end()) return found->second.index;
    if (next_texture2d >= kTexture2DCount) return 0;
    const uint32_t bytes = gfx::GuestTextureBytes(format, width, height, pitch, fetch.tiled);
    if (bytes == 0) {
      Skip("texture size");
      return 0;
    }
    std::vector<uint8_t> linear;
    std::string reason;
    const uint8_t* guest = At(base, PhysicalView(fetch.base_address << 12));
    if (!gfx::LoadGuestTextureRgba8(format, width, height, pitch, uint32_t(fetch.endianness),
                                    fetch.tiled, guest, bytes, linear, reason)) {
      Skip("texture decode");
      return 0;
    }
    // Fetch swizzle: three bits per output component, 0-3 picks x/y/z/w of the
    // decoded texel, 4 is 0 and 5 is 1.
    const uint32_t swizzle = fetch.swizzle;
    if (swizzle != 0x688) {  // xyzw
      for (size_t p = 0; p + 3 < linear.size(); p += 4) {
        const uint8_t in[6] = {linear[p], linear[p + 1], linear[p + 2], linear[p + 3], 0, 255};
        for (uint32_t c = 0; c < 4; ++c) {
          const uint32_t s = (swizzle >> (3 * c)) & 7;
          linear[p + c] = in[s < 6 ? s : 4];
        }
      }
    }
    const uint32_t row = (width * 4 + 255) & ~255u;
    uint64_t offset = 0;
    if (!Upload(size_t(row) * height, 512, offset)) {
      Skip("upload ring full");
      return 0;
    }
    for (uint32_t y = 0; y < height; ++y) {
      std::memcpy(upload_cpu + offset + size_t(y) * row, linear.data() + size_t(y) * width * 4, width * 4);
    }
    Texture texture;
    texture.texture = device->createTexture(
        plume::RenderTextureDesc::Texture2D(width, height, 1, plume::RenderFormat::R8G8B8A8_UNORM));
    texture.index = next_texture2d++;
    commands->barriers(plume::RenderBarrierStage::COPY,
                       plume::RenderTextureBarrier(texture.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    commands->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(texture.texture.get()),
        plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), plume::RenderFormat::R8G8B8A8_UNORM,
                                                          width, height, 1, row / 4, offset));
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(texture.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    sets[0]->setTexture(texture.index, texture.texture.get(), plume::RenderTextureLayout::SHADER_READ);
    uploaded = true;
    const uint32_t index = texture.index;
    textures.emplace(key, std::move(texture));
    return index;
  }

  // Copies one stream once per frame, swapped to host order by the fetch
  // constant's endian. Returns the ring offset.
  bool Stream(uint32_t address, uint32_t bytes, uint32_t endian, const uint8_t* base, uint64_t& offset) {
    const uint64_t key = (uint64_t(address) << 32) ^ (uint64_t(bytes) << 2) ^ endian;
    auto found = frame_streams.find(key);
    if (found != frame_streams.end()) {
      offset = found->second;
      return true;
    }
    if (!Upload(bytes, 16, offset)) return false;
    std::memcpy(upload_cpu + offset, At(base, PhysicalView(address)), bytes);
    SwapDwords(upload_cpu + offset, bytes, endian);
    frame_streams[key] = offset;
    return true;
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
  const plume::RenderDeviceCapabilities& caps = s.device->getCapabilities();
  if (!caps.descriptorIndexing || !caps.bufferDeviceAddress) {
    reason = "the device lacks descriptor indexing or buffer device address";
    return false;
  }
  s.device_name = s.device->getDescription().name;
  s.queue = s.device->createCommandQueue(plume::RenderCommandListType::DIRECT);
  s.fence = s.device->createCommandFence();
  s.commands = s.queue->createCommandList();
  s.acquire = s.device->createCommandSemaphore();
  s.swap_chain = s.queue->createSwapChain(plume::RenderSwapChainDesc(
      static_cast<plume::RenderWindow>(native_window), kColorFormat, kSwapChainBuffers));
  if (!s.swap_chain || !s.swap_chain->resize()) {
    reason = "no swapchain on the game window";
    return false;
  }
  while (s.release.size() < s.swap_chain->getTextureCount()) {
    s.release.push_back(s.device->createCommandSemaphore());
  }
  if (!s.CreateTarget()) {
    reason = "no render target";
    return false;
  }
  s.upload = s.device->createBuffer(plume::RenderBufferDesc::UploadBuffer(
      kUploadBytes, plume::RenderBufferFlag::VERTEX | plume::RenderBufferFlag::INDEX |
                        plume::RenderBufferFlag::CONSTANT | plume::RenderBufferFlag::DEVICE_ADDRESSABLE));
  if (!s.upload) {
    reason = "no upload buffer";
    return false;
  }
  s.upload_cpu = static_cast<uint8_t*>(s.upload->map());
  s.upload_gpu = s.upload->getDeviceAddress();
  if (!s.CreateLayout()) {
    reason = "no pipeline layout";
    return false;
  }
  s.CreateDefaults();
  if (!s.LoadShaderCache()) {
    reason = "the shader cache did not inflate";
    return false;
  }
  host::ShadowKeepShaderKeys();
  return true;
}

void Renderer::Draw(const GuestDraw& draw, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  s.BeginFrame();
  s.UploadDefaults();
  const uint32_t device = draw.device;

  // Shaders.
  const uint32_t vs_object = Be32(base, device + kDeviceVertexShader);
  const uint32_t ps_object = Be32(base, device + kDevicePixelShader);
  const uint64_t vs_key = host::ShadowShaderKey(vs_object, base);
  const uint64_t ps_key = host::ShadowShaderKey(ps_object, base);
  plume::RenderShader* vs = s.Shader(vs_key);
  plume::RenderShader* ps = ps_object ? s.Shader(ps_key) : nullptr;
  if (!vs || (ps_object && !ps)) {
    s.Skip("shader not in cache");
    return;
  }

  // Topology. Quad lists become triangle lists through an index list below.
  plume::RenderPrimitiveTopology topology;
  bool quads = false;
  switch (draw.prim) {
    case 1: topology = plume::RenderPrimitiveTopology::POINT_LIST; break;
    case 2: topology = plume::RenderPrimitiveTopology::LINE_LIST; break;
    case 3: topology = plume::RenderPrimitiveTopology::LINE_STRIP; break;
    case 4: topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST; break;
    case 5: topology = plume::RenderPrimitiveTopology::TRIANGLE_FAN; break;
    case 6: topology = plume::RenderPrimitiveTopology::TRIANGLE_STRIP; break;
    case 13:
      topology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
      quads = true;
      break;
    default:
      s.Skip(fmt::format("primitive {}", draw.prim));
      return;
  }

  // Vertex declaration -> input layout. Stream s is vertex fetch constant 95-s.
  const uint32_t decl = Be32(base, device + kDeviceDecl);
  const uint32_t element_count = decl ? std::min<uint32_t>(Be32(base, decl + 24), 16) : 0;
  std::array<plume::RenderInputElement, 16> elements;
  std::array<plume::RenderInputSlot, 16> slots;
  std::array<plume::RenderVertexBufferView, 16> views;
  uint32_t slot_count = 0;
  uint32_t stream_slot[16];
  std::fill(std::begin(stream_slot), std::end(stream_slot), UINT32_MAX);
  bool packed_normals = false;
  XXH3_state_t key_state;
  XXH3_64bits_reset(&key_state);
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t element = decl + 52 + i * 12;
    const uint32_t stream = Be16(base, element);
    const uint32_t offset = Be16(base, element + 2);
    const uint32_t format_word = Be32(base, element + 4);
    const uint32_t usage = *At(base, element + 9);
    const uint32_t usage_index = *At(base, element + 10);
    const uint32_t location = UsageLocation(usage, usage_index);
    plume::RenderFormat format;
    bool packed = false;
    if (stream >= 16 || location == UINT32_MAX || !ElementFormat(usage, format_word, format, packed)) {
      s.Skip(fmt::format("vertex element usage {} format {:03X}", usage, format_word & 0x33F));
      return;
    }
    packed_normals |= packed;
    if (stream_slot[stream] == UINT32_MAX) {
      const uint32_t fetch = device + kDeviceFetch + 8 * (95 - stream);
      xenos::xe_gpu_vertex_fetch_t vf;
      vf.dword_0 = Be32(base, fetch);
      vf.dword_1 = Be32(base, fetch + 4);
      const uint32_t stride = uint32_t(*At(base, device + kDeviceStrides + stream)) * 4;
      const uint32_t bytes = vf.size * 4;
      uint64_t ring = 0;
      if (bytes == 0 || bytes > (32u << 20) ||
          !s.Stream(vf.address * 4, bytes, uint32_t(vf.endian), base, ring)) {
        s.Skip("vertex stream");
        return;
      }
      stream_slot[stream] = slot_count;
      slots[slot_count] = plume::RenderInputSlot(slot_count, stride);
      views[slot_count] = plume::RenderVertexBufferView(s.upload->at(ring), bytes);
      ++slot_count;
      XXH3_64bits_update(&key_state, &stride, 4);
    }
    elements[i] = plume::RenderInputElement(UsageSemantic(usage), usage_index, location, format,
                                            stream_slot[stream], offset);
    const uint32_t element_key[4] = {location, uint32_t(format), stream_slot[stream], offset};
    XXH3_64bits_update(&key_state, element_key, sizeof(element_key));
  }

  // Render state.
  const uint32_t depth_control = Reg(base, device, kRbDepthControl);
  const uint32_t blend_control = Reg(base, device, kRbBlendControl0);
  const uint32_t color_control = Reg(base, device, kRbColorControl);
  const uint32_t color_mask = Reg(base, device, kRbColorMask) & 0xF;
  const bool alpha_test = (color_control & 0x8) != 0;
  uint32_t spec = 0;
  const auto vs_entry = s.entries.find(vs_key);
  const auto ps_entry = s.entries.find(ps_key);
  if (packed_normals && vs_entry != s.entries.end() &&
      (vs_entry->second->specConstantsMask & SPEC_CONSTANT_R11G11B10_NORMAL)) {
    spec |= SPEC_CONSTANT_R11G11B10_NORMAL;
  }
  if (alpha_test && ps_entry != s.entries.end() &&
      (ps_entry->second->specConstantsMask & SPEC_CONSTANT_ALPHA_TEST)) {
    spec |= SPEC_CONSTANT_ALPHA_TEST;
  }
  const uint32_t state_key[7] = {depth_control & 0x77, blend_control, color_mask, uint32_t(topology),
                                 spec, uint32_t(ps_object != 0), 0};
  XXH3_64bits_update(&key_state, state_key, sizeof(state_key));
  XXH3_64bits_update(&key_state, &vs_key, 8);
  XXH3_64bits_update(&key_state, &ps_key, 8);
  const uint64_t pipeline_key = XXH3_64bits_digest(&key_state);

  plume::RenderPipeline* pipeline = nullptr;
  if (auto found = s.pipelines.find(pipeline_key); found != s.pipelines.end()) {
    pipeline = found->second.get();
  } else {
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = s.layout.get();
    desc.vertexShader = vs;
    desc.pixelShader = ps;
    desc.primitiveTopology = topology;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.depthClipEnabled = true;
    desc.depthEnabled = (depth_control & 0x2) != 0;
    desc.depthWriteEnabled = (depth_control & 0x4) != 0;
    desc.depthFunction = CompareFunction((depth_control >> 4) & 7);
    desc.depthTargetFormat = kDepthFormat;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = kColorFormat;
    plume::RenderBlendDesc& blend = desc.renderTargetBlend[0];
    blend.srcBlend = BlendFactor(blend_control & 0x1F);
    blend.blendOp = BlendOp((blend_control >> 5) & 7);
    blend.dstBlend = BlendFactor((blend_control >> 8) & 0x1F);
    blend.srcBlendAlpha = BlendFactor((blend_control >> 16) & 0x1F);
    blend.blendOpAlpha = BlendOp((blend_control >> 21) & 7);
    blend.dstBlendAlpha = BlendFactor((blend_control >> 24) & 0x1F);
    // ONE, ZERO, ADD for both halves is blending off.
    blend.blendEnabled = (blend_control & 0x1FFF1FFF) != 0x00010001;
    blend.renderTargetWriteMask = uint8_t(color_mask);
    desc.inputSlots = slots.data();
    desc.inputSlotsCount = slot_count;
    desc.inputElements = elements.data();
    desc.inputElementsCount = element_count;
    const plume::RenderSpecConstant spec_constant(0, spec);
    desc.specConstants = &spec_constant;
    desc.specConstantsCount = 1;
    auto created = s.device->createGraphicsPipeline(desc);
    if (!created) {
      s.Skip("pipeline");
      return;
    }
    pipeline = s.pipelines.emplace(pipeline_key, std::move(created)).first->second.get();
  }

  // Constants: guest floats are big-endian dwords.
  uint64_t vs_constants = 0, ps_constants = 0, shared = 0;
  if (!s.Upload(gfx::kVertexShaderConstantsBytes, 256, vs_constants) ||
      !s.Upload(gfx::kPixelShaderConstantsBytes, 256, ps_constants) ||
      !s.Upload(gfx::kSharedConstantsBytes, 256, shared)) {
    s.Skip("upload ring full");
    return;
  }
  std::memcpy(s.upload_cpu + vs_constants, At(base, device + kDeviceVsConstants), gfx::kVertexShaderConstantsBytes);
  SwapDwords(s.upload_cpu + vs_constants, gfx::kVertexShaderConstantsBytes, 2);
  std::memcpy(s.upload_cpu + ps_constants, At(base, device + kDevicePsConstants), gfx::kPixelShaderConstantsBytes);
  SwapDwords(s.upload_cpu + ps_constants, gfx::kPixelShaderConstantsBytes, 2);

  uint8_t* shared_cpu = s.upload_cpu + shared;
  std::memset(shared_cpu, 0, gfx::kSharedConstantsBytes);
  auto* indices = reinterpret_cast<uint32_t*>(shared_cpu);
  bool uploaded = false;
  for (uint32_t slot = 0; slot < DESCRIPTOR_SLOT_COUNT; ++slot) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, device + kDeviceFetch + 24 * slot + 4 * w);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words, sizeof(words));
    if (uint32_t(fetch.type) != 2) continue;
    indices[DESCRIPTOR_KIND_SAMPLER * DESCRIPTOR_SLOT_COUNT + slot] = s.SamplerFor(fetch);
    if (uint32_t(fetch.dimension) == 1) {
      indices[DESCRIPTOR_KIND_TEXTURE2D * DESCRIPTOR_SLOT_COUNT + slot] =
          s.Texture2DFor(words, fetch, base, uploaded);
    }
  }
  const uint32_t bools0 = Be32(base, device + kDeviceBools);
  const uint32_t bools4 = Be32(base, device + kDeviceBools + 16);
  const uint32_t booleans = (bools0 & 0xFFFF) | ((bools4 & 0xFFFF) << 16);
  std::memcpy(shared_cpu + gfx::kSharedBooleansOffset, &booleans, 4);
  const float alpha_ref = RegFloat(base, device, kRbAlphaRef);
  std::memcpy(shared_cpu + gfx::kSharedAlphaThresholdOffset, &alpha_ref, 4);

  // Index list: guest indices are big-endian; quad lists are expanded.
  uint32_t index_count = draw.count;
  bool use_indices = draw.indexed || quads;
  uint64_t index_ring = 0;
  if (use_indices) {
    std::vector<uint32_t> list;
    if (draw.indexed) {
      const uint32_t ib = Be32(base, device + kDeviceIndices);
      if (ib == 0) {
        s.Skip("index buffer");
        return;
      }
      const bool index32 = (Be32(base, ib) & 0x80000000u) != 0;
      const uint32_t address = PhysicalView(Be32(base, ib + 24));
      list.resize(draw.count);
      for (uint32_t i = 0; i < draw.count; ++i) {
        list[i] = index32 ? Be32(base, address + 4 * (draw.start + i))
                          : Be16(base, address + 2 * (draw.start + i));
      }
    } else {
      list.resize(draw.count);
      for (uint32_t i = 0; i < draw.count; ++i) list[i] = draw.start + i;
    }
    if (quads) {
      std::vector<uint32_t> triangles;
      triangles.reserve(list.size() / 4 * 6);
      for (size_t q = 0; q + 3 < list.size(); q += 4) {
        const uint32_t v[6] = {list[q], list[q + 1], list[q + 2], list[q], list[q + 2], list[q + 3]};
        triangles.insert(triangles.end(), std::begin(v), std::end(v));
      }
      list.swap(triangles);
    }
    index_count = uint32_t(list.size());
    if (index_count == 0 || !s.Upload(list.size() * 4, 16, index_ring)) {
      s.Skip("index list");
      return;
    }
    std::memcpy(s.upload_cpu + index_ring, list.data(), list.size() * 4);
  }

  // Viewport from the D3D registers, scaled to the host target.
  const float sx = float(s.width) / kGuestWidth;
  const float sy = float(s.height) / kGuestHeight;
  const float xscale = std::fabs(RegFloat(base, device, kPaClVportXScale));
  const float yscale = std::fabs(RegFloat(base, device, kPaClVportYScale));
  const float xoffset = RegFloat(base, device, kPaClVportXOffset);
  const float yoffset = RegFloat(base, device, kPaClVportYOffset);
  const float zscale = RegFloat(base, device, kPaClVportZScale);
  const float zoffset = RegFloat(base, device, kPaClVportZOffset);
  plume::RenderViewport viewport((xoffset - xscale) * sx, (yoffset - yscale) * sy, 2.0f * xscale * sx,
                                 2.0f * yscale * sy, std::clamp(zoffset, 0.0f, 1.0f),
                                 std::clamp(zoffset + zscale, 0.0f, 1.0f));
  if (viewport.width <= 0.0f || viewport.height <= 0.0f) {
    viewport = plume::RenderViewport(0.0f, 0.0f, float(s.width), float(s.height));
  }

  if (uploaded) s.BindTarget();
  s.commands->setPipeline(pipeline);
  const uint64_t push[3] = {s.upload_gpu + vs_constants, s.upload_gpu + ps_constants, s.upload_gpu + shared};
  s.commands->setGraphicsPushConstants(0, push, 0, sizeof(push));
  s.commands->setViewports(&viewport, 1);
  const plume::RenderRect scissor(0, 0, int32_t(s.width), int32_t(s.height));
  s.commands->setScissors(&scissor, 1);
  if (slot_count) s.commands->setVertexBuffers(0, views.data(), slot_count, slots.data());
  if (use_indices) {
    const plume::RenderIndexBufferView view(s.upload->at(index_ring), index_count * 4,
                                            plume::RenderFormat::R32_UINT);
    s.commands->setIndexBuffer(&view);
    s.commands->drawIndexedInstanced(index_count, 1, 0, draw.indexed ? int32_t(draw.base_vertex) : 0, 0);
  } else {
    s.commands->drawInstanced(draw.count, 1, draw.start, 0);
  }
  ++s.drawn;
}

void Renderer::Present() {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  s.BeginFrame();
  s.UploadDefaults();
  uint32_t image = 0;
  const bool resize = s.swap_chain->needsResize();
  const bool acquired = !resize && !s.swap_chain->isEmpty() &&
                        s.swap_chain->acquireTexture(s.acquire.get(), &image);
  plume::RenderTexture* target = acquired ? s.swap_chain->getTexture(image) : nullptr;
  if (target) {
    plume::RenderTextureBarrier to_copy[] = {
        plume::RenderTextureBarrier(s.color.get(), plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COPY_DEST)};
    s.commands->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
    s.commands->copyTexture(target, s.color.get());
    s.commands->barriers(plume::RenderBarrierStage::NONE,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
  }
  s.commands->end();
  s.recording = false;
  const plume::RenderCommandList* list = s.commands.get();
  if (target) {
    plume::RenderCommandSemaphore* wait = s.acquire.get();
    plume::RenderCommandSemaphore* signal = s.release[image].get();
    s.queue->executeCommandLists(&list, 1, &wait, 1, &signal, 1, s.fence.get());
    s.swap_chain->present(image, &signal, 1);
  } else {
    s.queue->executeCommandLists(&list, 1, nullptr, 0, nullptr, 0, s.fence.get());
  }
  s.queue->waitForCommandFence(s.fence.get());
  ++s.presents;
  if (resize) {
    if (s.swap_chain->resize()) {
      while (s.release.size() < s.swap_chain->getTextureCount()) {
        s.release.push_back(s.device->createCommandSemaphore());
      }
      s.CreateTarget();
    }
  }
}

std::string Renderer::TakeStats() {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  std::string out = fmt::format("drawn={} pipelines={} shaders={} textures={} samplers={}", s.drawn,
                                s.pipelines.size(), s.shaders.size(), s.textures.size(), s.samplers.size());
  for (const auto& [reason, count] : s.skipped) out += fmt::format(" | {}: {}", reason, count);
  s.drawn = 0;
  s.skipped.clear();
  return out;
}

}  // namespace broken_bond::render
