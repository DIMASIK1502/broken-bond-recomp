#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <rex/cvar.h>
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

REXCVAR_DEFINE_INT32(native_dump_frame, 0, "Game",
                     "With renderer=native, describe every draw of this present (from 1) in "
                     "logs/native-frame-<n>.txt; 0 writes none")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_debug_target, false, "Game",
                    "Debug: skip guest clears and show the render target itself at Swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

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

// Fetch constants hold physical addresses (below 0x20000000), which the
// 0xA0000000 view reaches unshifted. Some D3D objects (index buffers) keep a
// virtual address in the 0xE0000000 range instead; that range maps physical
// memory 0x1000 further on, so it is read as the virtual address it is.
uint32_t PhysicalView(uint32_t address) {
  return address < 0x20000000u ? 0xA0000000u + address : address;
}

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

// How a guest element becomes host vertex data: copied as is, or decoded to
// four 32-bit floats (written as floats, or as their bits for the uint4
// inputs XenosRecomp reads through asfloat).
enum class ElementConvert : uint8_t { kCopy, kFloat4, kFloat4Bits };

struct ElementSource {
  xenos::VertexFormat format;
  bool is_signed;
  bool is_integer;
};

uint32_t GuestElementSize(xenos::VertexFormat format) {
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
    case xenos::VertexFormat::k_2_10_10_10:
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_32_FLOAT:
      return 4;
    case xenos::VertexFormat::k_16_16_16_16:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
      return 8;
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      return 12;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      return 16;
    default:
      return 0;
  }
}

float HalfToFloat(uint16_t h) {
  const uint32_t sign = uint32_t(h & 0x8000) << 16;
  uint32_t exponent = (h >> 10) & 0x1F;
  uint32_t mantissa = h & 0x3FF;
  uint32_t bits;
  if (exponent == 0) {
    if (mantissa == 0) {
      bits = sign;
    } else {
      exponent = 1;
      while ((mantissa & 0x400) == 0) {
        mantissa <<= 1;
        --exponent;
      }
      mantissa &= 0x3FF;
      bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
    }
  } else if (exponent == 31) {
    bits = sign | 0x7F800000 | (mantissa << 13);
  } else {
    bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
  }
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// One component `bits` wide, as Xenos fetch would return it.
float FixedComponent(uint32_t value, uint32_t bits, bool is_signed, bool is_integer) {
  if (is_signed) {
    const int32_t shift = 32 - int32_t(bits);
    const int32_t v = int32_t(value << shift) >> shift;
    if (is_integer) return float(v);
    return std::max(float(v) / float((1u << (bits - 1)) - 1), -1.0f);
  }
  if (is_integer) return float(value);
  return float(value) / float((1u << bits) - 1);
}

// Decodes one element (already in host byte order) to xyzw; absent
// components are 0, absent w is 1.
void DecodeElement(const ElementSource& source, const uint8_t* src, float out[4]) {
  out[0] = out[1] = out[2] = 0.0f;
  out[3] = 1.0f;
  uint32_t word0;
  std::memcpy(&word0, src, 4);
  const bool sg = source.is_signed;
  const bool in = source.is_integer;
  switch (source.format) {
    case xenos::VertexFormat::k_8_8_8_8:
      for (uint32_t c = 0; c < 4; ++c) out[c] = FixedComponent((word0 >> (8 * c)) & 0xFF, 8, sg, in);
      return;
    case xenos::VertexFormat::k_2_10_10_10:
      out[0] = FixedComponent(word0 & 0x3FF, 10, sg, in);
      out[1] = FixedComponent((word0 >> 10) & 0x3FF, 10, sg, in);
      out[2] = FixedComponent((word0 >> 20) & 0x3FF, 10, sg, in);
      out[3] = FixedComponent(word0 >> 30, 2, sg, in);
      return;
    case xenos::VertexFormat::k_10_11_11:
      out[0] = FixedComponent(word0 & 0x7FF, 11, sg, in);
      out[1] = FixedComponent((word0 >> 11) & 0x7FF, 11, sg, in);
      out[2] = FixedComponent(word0 >> 22, 10, sg, in);
      return;
    case xenos::VertexFormat::k_11_11_10:
      out[0] = FixedComponent(word0 & 0x3FF, 10, sg, in);
      out[1] = FixedComponent((word0 >> 10) & 0x7FF, 11, sg, in);
      out[2] = FixedComponent(word0 >> 21, 11, sg, in);
      return;
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_16_16: {
      const uint32_t n = source.format == xenos::VertexFormat::k_16_16 ? 2 : 4;
      for (uint32_t c = 0; c < n; ++c) {
        uint16_t v;
        std::memcpy(&v, src + 2 * c, 2);
        out[c] = FixedComponent(v, 16, sg, in);
      }
      return;
    }
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_16_16_16_16_FLOAT: {
      const uint32_t n = source.format == xenos::VertexFormat::k_16_16_FLOAT ? 2 : 4;
      for (uint32_t c = 0; c < n; ++c) {
        uint16_t v;
        std::memcpy(&v, src + 2 * c, 2);
        out[c] = HalfToFloat(v);
      }
      return;
    }
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      std::memcpy(out, src, GuestElementSize(source.format));
      return;
    default:
      return;
  }
}

// Host format of one declaration element. XenosRecomp reads NORMAL, TANGENT,
// BINORMAL and BLENDINDICES as uint4 (the first three as raw float bits, or
// as packed 11:11:10 when the vertex shader's spec constant says so),
// everything else as float4. Formats Vulkan cannot fetch the way Xenos does
// are decoded on the CPU while the stream is gathered.
bool ElementFormat(uint32_t usage, uint32_t format_word, plume::RenderFormat& out, uint32_t& in_size,
                   uint32_t& out_size, ElementConvert& convert, ElementSource& source,
                   bool& packed_normal) {
  using F = plume::RenderFormat;
  source.format = static_cast<xenos::VertexFormat>(format_word & 0x3F);
  source.is_signed = (format_word & 0x100) != 0;
  source.is_integer = (format_word & 0x200) != 0;
  in_size = GuestElementSize(source.format);
  if (in_size == 0) return false;
  const bool raw = usage == uint32_t(DeclUsage::kNormal) || usage == uint32_t(DeclUsage::kTangent) ||
                   usage == uint32_t(DeclUsage::kBinormal);
  packed_normal = false;
  convert = ElementConvert::kCopy;
  out_size = in_size;
  if (usage == uint32_t(DeclUsage::kBlendIndices)) {
    if (source.format != xenos::VertexFormat::k_8_8_8_8) return false;
    out = F::R8G8B8A8_UINT;
    return true;
  }
  if (raw) {
    if (source.format == xenos::VertexFormat::k_11_11_10 ||
        source.format == xenos::VertexFormat::k_10_11_11) {
      out = F::R32_UINT;
      packed_normal = true;
      return true;
    }
    convert = ElementConvert::kFloat4Bits;
    out = F::R32G32B32A32_UINT;
    out_size = 16;
    return true;
  }
  switch (source.format) {
    case xenos::VertexFormat::k_8_8_8_8:
      if (!source.is_integer) {
        out = source.is_signed ? F::R8G8B8A8_SNORM : F::R8G8B8A8_UNORM;
        return true;
      }
      break;
    case xenos::VertexFormat::k_16_16:
      if (!source.is_integer) {
        out = source.is_signed ? F::R16G16_SNORM : F::R16G16_UNORM;
        return true;
      }
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      if (!source.is_integer) {
        out = source.is_signed ? F::R16G16B16A16_SNORM : F::R16G16B16A16_UNORM;
        return true;
      }
      break;
    case xenos::VertexFormat::k_16_16_FLOAT: out = F::R16G16_FLOAT; return true;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT: out = F::R16G16B16A16_FLOAT; return true;
    case xenos::VertexFormat::k_32_FLOAT: out = F::R32_FLOAT; return true;
    case xenos::VertexFormat::k_32_32_FLOAT: out = F::R32G32_FLOAT; return true;
    case xenos::VertexFormat::k_32_32_32_FLOAT: out = F::R32G32B32_FLOAT; return true;
    case xenos::VertexFormat::k_32_32_32_32_FLOAT: out = F::R32G32B32A32_FLOAT; return true;
    default:
      break;
  }
  convert = ElementConvert::kFloat4;
  out = F::R32G32B32A32_FLOAT;
  out_size = 16;
  return true;
}

bool SixteenBitComponents(xenos::VertexFormat format) {
  return format == xenos::VertexFormat::k_16_16 || format == xenos::VertexFormat::k_16_16_16_16 ||
         format == xenos::VertexFormat::k_16_16_FLOAT || format == xenos::VertexFormat::k_16_16_16_16_FLOAT;
}

// src is the element after the fetch endian swap. With an 8in32 swap the two
// 16-bit components of each dword come out in reverse order, but Xenos
// fetches the one at the lower guest address as x (seen on the boot screens:
// SHORT2 positions only fill the screen with x and y in guest order).
void ConvertElement(const uint8_t* src, uint8_t* dst, uint32_t in_size, ElementConvert convert,
                    const ElementSource& source, uint32_t endian) {
  uint8_t fixed[16];
  if (endian == 2 && SixteenBitComponents(source.format)) {
    for (uint32_t i = 0; i + 3 < in_size; i += 4) {
      fixed[i] = src[i + 2];
      fixed[i + 1] = src[i + 3];
      fixed[i + 2] = src[i];
      fixed[i + 3] = src[i + 1];
    }
    src = fixed;
  }
  if (convert == ElementConvert::kCopy) {
    std::memcpy(dst, src, in_size);
    return;
  }
  float value[4];
  DecodeElement(source, src, value);
  std::memcpy(dst, value, 16);
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

// Host format holding a Xenos texture format's texels unchanged.
plume::RenderFormat HostTextureFormat(uint32_t format) {
  using F = plume::RenderFormat;
  switch (static_cast<xenos::TextureFormat>(format)) {
    case xenos::TextureFormat::k_8: return F::R8_UNORM;
    case xenos::TextureFormat::k_8_8: return F::R8G8_UNORM;
    case xenos::TextureFormat::k_8_8_8_8: return F::R8G8B8A8_UNORM;
    case xenos::TextureFormat::k_DXT1: return F::BC1_UNORM;
    case xenos::TextureFormat::k_DXT2_3: return F::BC2_UNORM;
    case xenos::TextureFormat::k_DXT4_5: return F::BC3_UNORM;
    case xenos::TextureFormat::k_DXT5A: return F::BC4_UNORM;
    case xenos::TextureFormat::k_DXN: return F::BC5_UNORM;
    case xenos::TextureFormat::k_16: return F::R16_UNORM;
    case xenos::TextureFormat::k_16_16: return F::R16G16_UNORM;
    case xenos::TextureFormat::k_16_16_16_16: return F::R16G16B16A16_UNORM;
    case xenos::TextureFormat::k_16_FLOAT: return F::R16_FLOAT;
    case xenos::TextureFormat::k_16_16_FLOAT: return F::R16G16_FLOAT;
    case xenos::TextureFormat::k_16_16_16_16_FLOAT: return F::R16G16B16A16_FLOAT;
    case xenos::TextureFormat::k_32_FLOAT: return F::R32_FLOAT;
    case xenos::TextureFormat::k_32_32_FLOAT: return F::R32G32_FLOAT;
    case xenos::TextureFormat::k_32_32_32_32_FLOAT: return F::R32G32B32A32_FLOAT;
    default: return F::UNKNOWN;
  }
}

// Fetch swizzle: three bits per output component, 0-3 pick x/y/z/w of the
// fetched texel, 4 is 0 and 5 is 1.
plume::RenderComponentMapping ComponentMapping(uint32_t swizzle) {
  auto one = [](uint32_t s) {
    switch (s & 7) {
      case 0: return plume::RenderSwizzle::R;
      case 1: return plume::RenderSwizzle::G;
      case 2: return plume::RenderSwizzle::B;
      case 3: return plume::RenderSwizzle::A;
      case 4: return plume::RenderSwizzle::ZERO;
      default: return plume::RenderSwizzle::ONE;
    }
  };
  return plume::RenderComponentMapping(one(swizzle), one(swizzle >> 3), one(swizzle >> 6), one(swizzle >> 9));
}

// Names one guest image by its texture fetch constant. Clamp and filter
// bits are sampler state and are left out.
uint64_t ImageKey(const uint32_t words[6]) {
  uint32_t image_words[6];
  std::memcpy(image_words, words, sizeof(image_words));
  image_words[0] &= ~0x3FFC00u;    // clamp_x/y/z
  image_words[3] &= ~0x0FF80000u;  // mag/min/mip/aniso filter
  return XXH3_64bits(image_words, sizeof(image_words));
}

// xenos::ClampMode: repeat, mirrored repeat, clamp to edge, mirror clamp to
// edge, clamp halfway, mirror clamp halfway, clamp to border, mirror clamp to
// border. Halfway clamps sit between edge and border; edge is the closer.
plume::RenderTextureAddressMode AddressMode(uint32_t clamp) {
  using A = plume::RenderTextureAddressMode;
  switch (clamp) {
    case 0: return A::WRAP;
    case 1: return A::MIRROR;
    case 2: case 4: return A::CLAMP;
    case 3: case 5: return A::MIRROR_ONCE;
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
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t index = 0;
    bool resolved = false;  // a host copy of the target, at host scale
    uint64_t content = 0;   // XXH3 of the guest texels last uploaded
    uint64_t checked = UINT64_MAX;  // present the content was last compared
    uint32_t width = 0;
    uint32_t height = 0;
  };
  std::unordered_map<uint64_t, Texture> textures;
  uint32_t next_texture2d = 1;  // 0 is white in every heap
  std::vector<std::unique_ptr<plume::RenderTexture>> defaults;
  std::unordered_map<uint32_t, uint32_t> sampler_index;
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers;

  std::string last_viewport;
  std::string last_bounds;
  std::map<std::string, uint64_t> viewports;  // VTE_CNTL and viewport per draw, for the log
  std::string dump;  // lines for native_dump_frame
  bool Dumping() const { return REXCVAR_GET(native_dump_frame) == int32_t(presents + 1); }
  bool recording = false;
  uint64_t clears = 0;
  uint64_t resolves = 0;
  uint64_t reuploads = 0;
  // Guest base address of every resolve destination -> its texture key. The
  // game samples a resolved image through fetch constants that differ from
  // the destination object's (format view, flags), so the address decides.
  std::unordered_map<uint32_t, uint64_t> resolved_at;

  // A guest D3DRECT {x1, y1, x2, y2} scaled to the host target.
  plume::RenderRect HostRect(const uint8_t* base, uint32_t rect) const {
    const float sx = float(width) / kGuestWidth;
    const float sy = float(height) / kGuestHeight;
    auto at = [&](uint32_t i) { return int32_t(Be32(base, rect + 4 * i)); };
    plume::RenderRect out(int32_t(std::lround(at(0) * sx)), int32_t(std::lround(at(1) * sy)),
                          int32_t(std::lround(at(2) * sx)), int32_t(std::lround(at(3) * sy)));
    out.left = std::clamp<int32_t>(out.left, 0, int32_t(width));
    out.right = std::clamp<int32_t>(out.right, out.left, int32_t(width));
    out.top = std::clamp<int32_t>(out.top, 0, int32_t(height));
    out.bottom = std::clamp<int32_t>(out.bottom, out.top, int32_t(height));
    return out;
  }
  uint64_t presents = 0;

  std::mutex mutex;  // Draw and Present run on the guest thread, TakeStats on the sampler
  std::map<std::string, uint64_t> skipped;
  uint64_t drawn = 0;
  double ms_pipeline = 0, ms_texture = 0, ms_stream = 0, ms_submit = 0;

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
    commands->begin();
    plume::RenderTextureBarrier barriers[] = {
        plume::RenderTextureBarrier(color.get(), plume::RenderTextureLayout::COLOR_WRITE),
        plume::RenderTextureBarrier(depth.get(), plume::RenderTextureLayout::DEPTH_WRITE)};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS, barriers, 2);
    BindTarget();
    // The game clears through ClearF and Resolve. It draws before Swap and
    // resolves its tiles after it, so the target carries over between frames;
    // only the very first frame starts from a defined state.
    if (presents == 0) {
      commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
      commands->clearDepthStencil(true, true, 1.0f, 0);
    }
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
  // format is not handled yet. Texels are uploaded untiled and endian-swapped
  // but otherwise as the guest stores them; the fetch swizzle becomes the
  // view's component mapping. Guest memory is hashed the first time a texture
  // is used in a frame, and changed texels (Bink video, CPU-written images)
  // are uploaded again.
  uint32_t Texture2DFor(const uint32_t words[6], const xenos::xe_gpu_texture_fetch_t& fetch,
                        const uint8_t* base, bool& uploaded) {
    if (auto at = resolved_at.find(fetch.base_address << 12); at != resolved_at.end()) {
      auto resolved = textures.find(at->second);
      if (resolved != textures.end()) return resolved->second.index;
    }
    const uint64_t key = ImageKey(words);
    auto found = textures.find(key);
    if (found != textures.end() && (found->second.resolved || found->second.checked == presents)) {
      return found->second.index;
    }
    const uint32_t format = uint32_t(fetch.format);
    const plume::RenderFormat host = HostTextureFormat(format);
    if (host == plume::RenderFormat::UNKNOWN) {
      Skip(fmt::format("texture format {}", format));
      return 0;
    }
    const uint32_t width = fetch.size_2d.width + 1;
    const uint32_t height = fetch.size_2d.height + 1;
    const uint32_t pitch = fetch.pitch * 32;
    const uint32_t bytes = gfx::GuestTextureBytesAny(format, width, height, pitch, fetch.tiled);
    if (bytes == 0) {
      Skip("texture size");
      return 0;
    }
    const uint8_t* guest = At(base, PhysicalView(fetch.base_address << 12));
    const uint64_t content = XXH3_64bits(guest, bytes);
    if (found != textures.end()) {
      found->second.checked = presents;
      if (found->second.content == content) return found->second.index;
      ++reuploads;
    } else if (next_texture2d >= kTexture2DCount) {
      return 0;
    }
    gfx::LinearTexture linear;
    std::string reason;
    if (!gfx::LoadGuestTextureLinear(format, width, height, pitch, uint32_t(fetch.endianness), fetch.tiled,
                                     guest, bytes, linear, reason)) {
      Skip("texture layout");
      return found != textures.end() ? found->second.index : 0;
    }
    const size_t row_bytes = size_t(linear.width_blocks) * linear.block_bytes;
    const size_t row = (row_bytes + 255) & ~size_t(255);
    uint64_t offset = 0;
    if (!Upload(row * linear.height_blocks, 512, offset)) {
      Skip("upload ring full");
      return found != textures.end() ? found->second.index : 0;
    }
    for (uint32_t y = 0; y < linear.height_blocks; ++y) {
      std::memcpy(upload_cpu + offset + y * row, linear.blocks.data() + y * row_bytes, row_bytes);
    }
    if (found == textures.end()) {
      Texture texture;
      texture.texture = device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, host));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(host);
      view.componentMapping = ComponentMapping(fetch.swizzle);
      texture.view = texture.texture->createTextureView(view);
      texture.index = next_texture2d++;
      found = textures.emplace(key, std::move(texture)).first;
    }
    Texture& texture = found->second;
    texture.content = content;
    texture.checked = presents;
    commands->barriers(plume::RenderBarrierStage::COPY,
                       plume::RenderTextureBarrier(texture.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    const uint32_t row_texels = uint32_t(row / linear.block_bytes) * linear.block_width;
    commands->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(texture.texture.get()),
        plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), host, width, height, 1, row_texels,
                                                          offset));
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(texture.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    sets[0]->setTexture(texture.index, texture.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                        texture.view.get());
    uploaded = true;
    return texture.index;
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

  // Topology. Quad lists become triangle lists through the index list.
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
  if (draw.count == 0) return;

  // Guest vertex indices of the draw. Vertex v of the draw lives at guest
  // vertex base_vertex + v (indexed) or v (auto).
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
      list[i] = index32 ? Be32(base, address + 4 * (draw.start + i)) : Be16(base, address + 2 * (draw.start + i));
    }
  }
  uint32_t first = 0, last = 0;
  if (draw.indexed) {
    uint32_t lo = UINT32_MAX, hi = 0;
    for (uint32_t index : list) {
      if (index == 0xFFFFu || index == 0xFFFFFFFFu) continue;
      lo = std::min(lo, index);
      hi = std::max(hi, index);
    }
    if (lo == UINT32_MAX) return;
    first = draw.base_vertex + lo;
    last = draw.base_vertex + hi;
  } else {
    first = draw.start;
    last = draw.start + draw.count - 1;
  }
  const uint32_t vertex_count = last - first + 1;
  if (vertex_count > (1u << 20)) {
    s.Skip("vertex range");
    return;
  }

  // Declaration: every element is gathered into a host stream per guest
  // stream, converting what Vulkan cannot fetch (16-bit integers) to float.
  struct Element {
    uint32_t stream, offset, usage, usage_index, location;
    uint32_t in_size, out_size, out_offset;
    uint32_t format_word;
    ElementConvert convert;
    ElementSource source;
    plume::RenderFormat format;
  };
  const uint32_t decl = Be32(base, device + kDeviceDecl);
  const uint32_t element_count = decl ? std::min<uint32_t>(Be32(base, decl + 24), 16) : 0;
  std::array<Element, 16> elems;
  bool packed_normals = false;
  for (uint32_t i = 0; i < element_count; ++i) {
    const uint32_t at = decl + 52 + i * 12;
    Element& e = elems[i];
    e.stream = Be16(base, at);
    e.offset = Be16(base, at + 2);
    const uint32_t format_word = Be32(base, at + 4);
    e.format_word = format_word;
    e.usage = *At(base, at + 9);
    e.usage_index = *At(base, at + 10);
    e.location = UsageLocation(e.usage, e.usage_index);
    bool packed = false;
    if (e.stream >= 16 || e.location == UINT32_MAX ||
        !ElementFormat(e.usage, format_word, e.format, e.in_size, e.out_size, e.convert, e.source, packed)) {
      s.Skip(fmt::format("vertex element usage {} format {:03X}", e.usage, format_word & 0x33F));
      return;
    }
    packed_normals |= packed;
  }

  std::array<plume::RenderInputElement, 16> elements;
  std::array<plume::RenderInputSlot, 16> slots;
  std::array<plume::RenderVertexBufferView, 16> views;
  uint32_t slot_count = 0;
  XXH3_state_t key_state;
  XXH3_64bits_reset(&key_state);
  const auto t_stream = std::chrono::steady_clock::now();
  for (uint32_t stream = 0; stream < 16; ++stream) {
    uint32_t out_stride = 0;
    for (uint32_t i = 0; i < element_count; ++i) {
      if (elems[i].stream != stream) continue;
      elems[i].out_offset = out_stride;
      out_stride += elems[i].out_size;
    }
    if (out_stride == 0) continue;
    const uint32_t fetch = device + kDeviceFetch + 8 * (95 - stream);
    xenos::xe_gpu_vertex_fetch_t vf;
    vf.dword_0 = Be32(base, fetch);
    vf.dword_1 = Be32(base, fetch + 4);
    const uint32_t stride = uint32_t(*At(base, device + kDeviceStrides + stream)) * 4;
    const uint64_t begin = uint64_t(first) * stride;
    const uint64_t bytes = uint64_t(vertex_count) * stride;
    if (stride == 0 || begin + bytes > uint64_t(vf.size) * 4) {
      s.last_bounds = fmt::format("stream {} stride {} first {} count {} base {} fetch_bytes {} indexed {} start {} n {}",
                                  stream, stride, first, vertex_count, draw.base_vertex, vf.size * 4,
                                  draw.indexed, draw.start, draw.count);
      if (draw.indexed) {
        const uint32_t ib = Be32(base, device + kDeviceIndices);
        s.last_bounds += fmt::format(" ib {:08X} flags {:08X} addr {:08X} words", ib, Be32(base, ib),
                                     Be32(base, ib + 24));
        for (uint32_t w = 0; w < 8; ++w) s.last_bounds += fmt::format(" {:08X}", Be32(base, ib + 4 * w));
        s.last_bounds += " idx";
        for (size_t i = 0; i < std::min<size_t>(list.size(), 12); ++i) s.last_bounds += fmt::format(" {}", list[i]);
      }
      s.Skip("vertex stream bounds");
      return;
    }
    std::vector<uint8_t> raw(bytes);
    std::memcpy(raw.data(), At(base, PhysicalView(vf.address * 4 + uint32_t(begin))), bytes);
    SwapDwords(raw.data(), raw.size(), uint32_t(vf.endian));
    uint64_t ring = 0;
    if (!s.Upload(size_t(vertex_count) * out_stride, 16, ring)) {
      s.Skip("upload ring full");
      return;
    }
    uint8_t* out = s.upload_cpu + ring;
    for (uint32_t v = 0; v < vertex_count; ++v) {
      const uint8_t* src = raw.data() + size_t(v) * stride;
      uint8_t* dst = out + size_t(v) * out_stride;
      for (uint32_t i = 0; i < element_count; ++i) {
        const Element& e = elems[i];
        if (e.stream != stream) continue;
        ConvertElement(src + e.offset, dst + e.out_offset, e.in_size, e.convert, e.source,
                       uint32_t(vf.endian));
      }
    }
    slots[slot_count] = plume::RenderInputSlot(slot_count, out_stride);
    views[slot_count] = plume::RenderVertexBufferView(s.upload->at(ring), vertex_count * out_stride);
    for (uint32_t i = 0; i < element_count; ++i) {
      if (elems[i].stream != stream) continue;
      elements[i] = plume::RenderInputElement(UsageSemantic(elems[i].usage), elems[i].usage_index,
                                              elems[i].location, elems[i].format, slot_count,
                                              elems[i].out_offset);
      const uint32_t element_key[4] = {elems[i].location, uint32_t(elems[i].format), slot_count,
                                       elems[i].out_offset};
      XXH3_64bits_update(&key_state, element_key, sizeof(element_key));
    }
    XXH3_64bits_update(&key_state, &out_stride, 4);
    ++slot_count;
  }
  s.ms_stream += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_stream).count();

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
  const uint32_t state_key[6] = {depth_control & 0x77, blend_control, color_mask, uint32_t(topology),
                                 spec, uint32_t(ps_object != 0)};
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
    desc.depthEnabled = (depth_control & 0x2) != 0 && !REXCVAR_GET(native_debug_target);
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
    const auto t0 = std::chrono::steady_clock::now();
    auto created = s.device->createGraphicsPipeline(desc);
    s.ms_pipeline += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
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
      const auto t0 = std::chrono::steady_clock::now();
      indices[DESCRIPTOR_KIND_TEXTURE2D * DESCRIPTOR_SLOT_COUNT + slot] =
          s.Texture2DFor(words, fetch, base, uploaded);
      s.ms_texture += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
  }
  const uint32_t bools0 = Be32(base, device + kDeviceBools);
  const uint32_t bools4 = Be32(base, device + kDeviceBools + 16);
  const uint32_t booleans = (bools0 & 0xFFFF) | ((bools4 & 0xFFFF) << 16);
  std::memcpy(shared_cpu + gfx::kSharedBooleansOffset, &booleans, 4);
  const float alpha_ref = RegFloat(base, device, kRbAlphaRef);
  std::memcpy(shared_cpu + gfx::kSharedAlphaThresholdOffset, &alpha_ref, 4);

  // Host index list, relative to the first copied vertex.
  const bool use_indices = draw.indexed || quads;
  uint32_t index_count = draw.count;
  uint64_t index_ring = 0;
  if (use_indices) {
    std::vector<uint32_t> host;
    if (draw.indexed) {
      host.resize(list.size());
      for (size_t i = 0; i < list.size(); ++i) {
        const uint32_t index = list[i];
        host[i] = (index == 0xFFFFu || index == 0xFFFFFFFFu) ? 0xFFFFFFFFu
                                                             : draw.base_vertex + index - first;
      }
    } else {
      host.resize(draw.count);
      for (uint32_t i = 0; i < draw.count; ++i) host[i] = i;
    }
    if (quads) {
      std::vector<uint32_t> triangles;
      triangles.reserve(host.size() / 4 * 6);
      for (size_t q = 0; q + 3 < host.size(); q += 4) {
        const uint32_t v[6] = {host[q], host[q + 1], host[q + 2], host[q], host[q + 2], host[q + 3]};
        triangles.insert(triangles.end(), std::begin(v), std::end(v));
      }
      host.swap(triangles);
    }
    index_count = uint32_t(host.size());
    if (index_count == 0 || !s.Upload(host.size() * 4, 16, index_ring)) {
      s.Skip("index list");
      return;
    }
    std::memcpy(s.upload_cpu + index_ring, host.data(), host.size() * 4);
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
  if (!(viewport.width > 0.0f) || !(viewport.height > 0.0f)) {
    viewport = plume::RenderViewport(0.0f, 0.0f, float(s.width), float(s.height));
  }
  ++s.viewports[fmt::format("vte={:03X} vp={:.0f},{:.0f},{:.0f}x{:.0f} z={:.2f}+{:.2f} surf={:08X} wo={:08X} sc={:08X}-{:08X}",
                              Reg(base, device, 0x2206), xoffset - xscale, yoffset - yscale, 2 * xscale,
                              2 * yscale, zoffset, zscale, Reg(base, device, 0x2000), Reg(base, device, 0x2080),
                              Reg(base, device, 0x200E), Reg(base, device, 0x200F))];
  s.last_viewport = fmt::format("{:.1f},{:.1f} {:.1f}x{:.1f}", xoffset - xscale, yoffset - yscale,
                                2.0f * xscale, 2.0f * yscale);

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
    s.commands->drawIndexedInstanced(index_count, 1, 0, 0, 0);
  } else {
    s.commands->drawInstanced(draw.count, 1, 0, 0);
  }
  if (s.Dumping()) {
    std::string line = fmt::format("draw {} indexed={} prim={} start={} count={} base={} first={} verts={} "
                                   "vs={:016X} ps={:016X} depth={:08X} blend={:08X} mask={:X} vp={} cull={:08X} "
                                   "clip={:08X}",
                                   s.drawn, draw.indexed, draw.prim, draw.start, draw.count, draw.base_vertex,
                                   first, vertex_count, vs_key, ps_key, depth_control, blend_control, color_mask,
                                   s.last_viewport, Reg(base, device, kPaSuScModeCntl), Reg(base, device, 0x2204));
    for (uint32_t i = 0; i < element_count; ++i) {
      line += fmt::format(" | e{} s{} +{} usage{}.{} type{:08X}", i, elems[i].stream, elems[i].offset,
                          elems[i].usage, elems[i].usage_index, elems[i].format_word);
    }
    // Decoded positions of the first vertices (stream of the POSITION element).
    for (uint32_t i = 0; i < element_count; ++i) {
      if (elems[i].usage != 0 || elems[i].usage_index != 0) continue;
      const uint32_t fetch_at = device + kDeviceFetch + 8 * (95 - elems[i].stream);
      xenos::xe_gpu_vertex_fetch_t vf;
      vf.dword_0 = Be32(base, fetch_at);
      vf.dword_1 = Be32(base, fetch_at + 4);
      const uint32_t stride = uint32_t(*At(base, device + kDeviceStrides + elems[i].stream)) * 4;
      for (uint32_t v = 0; v < std::min<uint32_t>(vertex_count, 6); ++v) {
        uint8_t raw[16] = {};
        std::memcpy(raw, At(base, PhysicalView(vf.address * 4 + (first + v) * stride + elems[i].offset)),
                    elems[i].in_size);
        SwapDwords(raw, sizeof(raw), uint32_t(vf.endian));
        float pos[4];
        DecodeElement(elems[i].source, raw, pos);
        line += fmt::format(" | v{} ({:.2f},{:.2f},{:.2f},{:.2f})", first + v, pos[0], pos[1], pos[2], pos[3]);
      }
    }
    const float* c = reinterpret_cast<const float*>(s.upload_cpu + vs_constants);
    for (uint32_t r = 0; r < 8; ++r) {
      line += fmt::format(" | c{} ({:g},{:g},{:g},{:g})", r, c[4 * r], c[4 * r + 1], c[4 * r + 2],
                          c[4 * r + 3]);
    }
    for (uint32_t slot = 0; slot < DESCRIPTOR_SLOT_COUNT; ++slot) {
      const uint32_t index = indices[DESCRIPTOR_KIND_TEXTURE2D * DESCRIPTOR_SLOT_COUNT + slot];
      if (index == 0) continue;
      bool resolved = false;
      for (const auto& [k, t] : s.textures) resolved |= t.index == index && t.resolved;
      line += fmt::format(" | t{}=#{}{}", slot, index, resolved ? "r" : "");
    }
    s.dump += line + "\n";
  }
  ++s.drawn;
}

void Renderer::Clear(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, float z,
                     uint32_t stencil, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  (void)device;
  s.BeginFrame();
  s.UploadDefaults();
  s.BindTarget();
  if (REXCVAR_GET(native_debug_target)) return;
  plume::RenderRect host_rect(0, 0, int32_t(s.width), int32_t(s.height));
  if (rect != 0) host_rect = s.HostRect(base, rect);
  if (flags & 0xF) {
    plume::RenderColor value(0.0f, 0.0f, 0.0f, 0.0f);
    if (color != 0) {
      value = plume::RenderColor(BeFloat(base, color), BeFloat(base, color + 4), BeFloat(base, color + 8),
                                 BeFloat(base, color + 12));
    }
    s.commands->clearColor(0, value, &host_rect, 1);
  }
  if (flags & 0x30) {
    s.commands->clearDepthStencil((flags & 0x10) != 0, (flags & 0x20) != 0, std::clamp(z, 0.0f, 1.0f),
                                  stencil & 0xFF, &host_rect, 1);
  }
  if (s.Dumping()) s.dump += fmt::format("clear flags={:X} rect={:08X} z={}\n", flags, rect, z);
  ++s.clears;
}

void Renderer::Resolve(uint32_t device, uint32_t flags, uint32_t rect, uint32_t texture, uint32_t point,
                       uint32_t clear_color, float z, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  (void)device;
  s.BeginFrame();
  s.UploadDefaults();
  const uint32_t source = flags & 7;
  if (texture != 0 && source < 4) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, texture + 28 + 4 * w);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words, sizeof(words));
    const uint32_t guest_width = fetch.size_2d.width + 1;
    const uint32_t guest_height = fetch.size_2d.height + 1;
    const uint32_t width = std::max(1u, uint32_t(std::lround(guest_width * float(s.width) / kGuestWidth)));
    const uint32_t height = std::max(1u, uint32_t(std::lround(guest_height * float(s.height) / kGuestHeight)));
    const uint64_t key = ImageKey(words);
    s.resolved_at[fetch.base_address << 12] = key;
    State::Texture& entry = s.textures[key];
    if (!entry.texture || !entry.resolved || entry.width != width || entry.height != height) {
      entry.texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, kColorFormat));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(kColorFormat);
      view.componentMapping = ComponentMapping(fetch.swizzle);
      entry.view = entry.texture->createTextureView(view);
      if (entry.index == 0) {
        if (s.next_texture2d >= kTexture2DCount) return;
        entry.index = s.next_texture2d++;
      }
      entry.resolved = true;
      entry.width = width;
      entry.height = height;
    }
    plume::RenderRect src(0, 0, int32_t(s.width), int32_t(s.height));
    if (rect != 0) src = s.HostRect(base, rect);
    int32_t dst_x = 0, dst_y = 0;
    if (point != 0) {
      dst_x = int32_t(std::lround(int32_t(Be32(base, point)) * float(s.width) / kGuestWidth));
      dst_y = int32_t(std::lround(int32_t(Be32(base, point + 4)) * float(s.height) / kGuestHeight));
    }
    src.right = std::min<int32_t>(src.right, src.left + int32_t(width) - dst_x);
    src.bottom = std::min<int32_t>(src.bottom, src.top + int32_t(height) - dst_y);
    if (src.right > src.left && src.bottom > src.top) {
      plume::RenderTextureBarrier to_copy[] = {
          plume::RenderTextureBarrier(s.color.get(), plume::RenderTextureLayout::COPY_SOURCE),
          plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::COPY_DEST)};
      s.commands->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
      const plume::RenderBox box(src.left, src.top, src.right, src.bottom);
      s.commands->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(entry.texture.get()),
                                    plume::RenderTextureCopyLocation::Subresource(s.color.get()), uint32_t(dst_x),
                                    uint32_t(dst_y), 0, &box);
      plume::RenderTextureBarrier back[] = {
          plume::RenderTextureBarrier(s.color.get(), plume::RenderTextureLayout::COLOR_WRITE),
          plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::SHADER_READ)};
      s.commands->barriers(plume::RenderBarrierStage::GRAPHICS, back, 2);
      s.sets[0]->setTexture(entry.index, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                            entry.view.get());
    }
    if (s.Dumping()) {
      s.dump += fmt::format("resolve flags={:X} texture={:08X} {}x{} key={:016X} index=#{} src={},{}-{},{}\n",
                            flags, texture, guest_width, guest_height, key, entry.index, src.left, src.top,
                            src.right, src.bottom);
    }
    ++s.resolves;
  }
  s.BindTarget();
  // Xenos clears the EDRAM the resolve read. A tiled pass resolves and clears
  // one band at a time; the host draws the frame once, so only that band may
  // be cleared or the later bands would copy an empty target.
  plume::RenderRect region(0, 0, int32_t(s.width), int32_t(s.height));
  if (rect != 0) region = s.HostRect(base, rect);
  if (REXCVAR_GET(native_debug_target)) flags &= ~0x300u;
  if (flags & 0x100) {
    plume::RenderColor value(0.0f, 0.0f, 0.0f, 0.0f);
    if (clear_color != 0) {
      value = plume::RenderColor(BeFloat(base, clear_color), BeFloat(base, clear_color + 4),
                                 BeFloat(base, clear_color + 8), BeFloat(base, clear_color + 12));
    }
    s.commands->clearColor(0, value, &region, 1);
  }
  if (flags & 0x200) s.commands->clearDepthStencil(true, true, std::clamp(z, 0.0f, 1.0f), 0, &region, 1);
}

void Renderer::Present(uint32_t frontbuffer, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  s.BeginFrame();
  s.UploadDefaults();
  // Swap shows a texture: the last resolve into it when there was one, else
  // whatever the target holds.
  plume::RenderTexture* source = s.color.get();
  plume::RenderTextureLayout source_layout = plume::RenderTextureLayout::COLOR_WRITE;
  if (frontbuffer != 0 && !REXCVAR_GET(native_debug_target)) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, frontbuffer + 28 + 4 * w);
    auto found = s.textures.find(ImageKey(words));
    if (found != s.textures.end() && found->second.resolved && found->second.width == s.width &&
        found->second.height == s.height) {
      source = found->second.texture.get();
      source_layout = plume::RenderTextureLayout::SHADER_READ;
    }
  }
  uint32_t image = 0;
  const bool resize = s.swap_chain->needsResize();
  const bool acquired = !resize && !s.swap_chain->isEmpty() &&
                        s.swap_chain->acquireTexture(s.acquire.get(), &image);
  plume::RenderTexture* target = acquired ? s.swap_chain->getTexture(image) : nullptr;
  if (target) {
    plume::RenderTextureBarrier to_copy[] = {
        plume::RenderTextureBarrier(source, plume::RenderTextureLayout::COPY_SOURCE),
        plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COPY_DEST)};
    s.commands->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
    s.commands->copyTexture(target, source);
    plume::RenderTextureBarrier after[] = {
        plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT),
        plume::RenderTextureBarrier(source, source_layout)};
    s.commands->barriers(plume::RenderBarrierStage::NONE, after, 2);
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
  const auto t_submit = std::chrono::steady_clock::now();
  s.queue->waitForCommandFence(s.fence.get());
  s.ms_submit += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_submit).count();
  if (s.Dumping()) {
    const std::string path = fmt::format("logs/native-frame-{}.txt", s.presents + 1);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fputs(s.dump.c_str(), f);
      std::fprintf(f, "present frontbuffer=%08X target=%ux%u\n", frontbuffer, s.width, s.height);
      std::fclose(f);
    }
    s.dump.clear();
  }
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
  std::string out = fmt::format(
      "drawn={} clears={} resolves={} reuploads={} pipelines={} shaders={} textures={} samplers={} ms pipeline={:.0f} texture={:.0f} "
      "stream={:.0f} gpu_wait={:.0f} viewport={}",
      s.drawn, s.clears, s.resolves, s.reuploads, s.pipelines.size(), s.shaders.size(), s.textures.size(), s.samplers.size(),
      s.ms_pipeline, s.ms_texture, s.ms_stream, s.ms_submit, s.last_viewport);
  s.ms_pipeline = s.ms_texture = s.ms_stream = s.ms_submit = 0;
  for (const auto& [reason, count] : s.skipped) out += fmt::format(" | {}: {}", reason, count);
  if (!s.last_bounds.empty()) out += " | last bounds: " + s.last_bounds;
  std::vector<std::pair<uint64_t, std::string>> top;
  for (const auto& [k, n] : s.viewports) top.emplace_back(n, k);
  std::sort(top.rbegin(), top.rend());
  for (size_t i = 0; i < std::min<size_t>(top.size(), 6); ++i) out += fmt::format(" || {} x{}", top[i].second, top[i].first);
  s.viewports.clear();
  s.last_bounds.clear();
  s.drawn = 0;
  s.clears = 0;
  s.resolves = 0;
  s.reuploads = 0;
  s.skipped.clear();
  return out;
}

}  // namespace broken_bond::render
