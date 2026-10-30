#include "render/renderer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <ranges>
#include <string_view>
#include <unordered_map>
#include <vector>

#define XXH_INLINE_ALL
#include <xxhash.h>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <plume_render_interface.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/system/xmemory.h>
#include <smolv.h>
#include <zstd.h>

#include "gfx/shader_cache.h"
#include "gfx/texture.h"
#include "gfx/xenos_shader_common.h"
#include <rex/logging.h>

#include "render/interpolation.h"

#include "depth_resolve_ps.h"
#include "depth_resolve_vs.h"
#include "present_ps.h"
#include "present_vs.h"
#include "host/shadow_draw.h"

namespace plume {
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}

REXCVAR_DEFINE_INT32(native_dump_frame, 0, "Game",
                     "With renderer=native, describe every draw of this present (from 1) in "
                     "logs/native-frame-<n>.txt; 0 writes none")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_decl_swizzle, true, "Game",
                    "With renderer=native, apply the vertex declaration swizzle (D3DCOLOR is zyxw) "
                    "while gathering streams")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_DOUBLE(native_resolution_scale, 0.0, "Game",
                      "With renderer=native, the internal resolution as a multiple of the game's 1280x720 "
                      "(1 is 720p, 1.5 is 1080p); 0 matches the window")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_interpolate, false, "Game",
                    "With renderer=native, draw an in-between frame when the game steps at 30 Hz "
                    "(shows 60 FPS one step late)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(native_msaa, 4, "Game",
                     "With renderer=native, the most samples a multisampled guest surface gets on the "
                     "host (1 turns MSAA off)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_skip_shaders, "", "Game",
                      "With renderer=native, skip the draws whose vertex or pixel shader key starts with "
                      "one of these comma-separated hex prefixes (to find which pass draws something)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_skip_verts, "", "Game",
                      "With renderer=native, skip the draws that use one of these comma-separated "
                      "vertex counts (to find which mesh is which)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(native_dump_mesh, 0, "Game",
                     "With a frame dump, write every vertex of the draws with this many vertices to "
                     "logs/mesh-<draw>-<verts>.csv, decoded as the vertex shader gets them")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_tint_verts, "", "Game",
                      "With renderer=native, tint the draws that use these comma-separated vertex counts: "
                      "the first writes only red, the second green, the third blue, the fourth red and green")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_dump_texture, "", "Game",
                      "With renderer=native, write the untiled texels of the 2D textures at this hex "
                      "physical address, or of this <w>x<h> size, to logs/texture-<address>-f<format>-<w>x<h>.bin "
                      "when they are uploaded")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(native_dump_constants, "", "Game",
                      "With a frame dump, list every non-zero vertex and pixel shader constant of the "
                      "draws whose vertex or pixel shader key starts with one of these comma-separated "
                      "hex prefixes")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_dump_outline, false, "Game",
                    "With renderer=native, dump the present 60 after the first draw that culls front "
                    "faces (character outlines) as native_dump_frame would")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(native_debug_target, false, "Game",
                    "Debug: skip guest clears and show the render target itself at Swap")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace broken_bond::render {

namespace {

// Adds the wall time of a scope to a millisecond counter.
struct ScopedMs {
  double& total;
  std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
  explicit ScopedMs(double& counter) : total(counter) {}
  ~ScopedMs() {
    total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  }
};

namespace xenos = rex::graphics::xenos;
using broken_bond::gfx::DeclUsage;

constexpr plume::RenderFormat kColorFormat = plume::RenderFormat::B8G8R8A8_UNORM;
constexpr plume::RenderFormat kDepthFormat = plume::RenderFormat::D32_FLOAT_S8_UINT;
// What a guest depth resolve becomes on the host: the depth value itself.
constexpr plume::RenderFormat kDepthCopyFormat = plume::RenderFormat::R32_FLOAT;
// Interpolation queues two presents per guest frame and needs a third.
constexpr uint32_t kSwapChainBuffers = 2;
constexpr uint32_t kInterpolationSwapChainBuffers = 3;
constexpr uint64_t kUploadBytes = 256ull << 20;
constexpr uint32_t kTexture2DCount = 8192;
constexpr uint32_t kTexture3DCount = 64;
constexpr uint32_t kTextureCubeCount = 256;
constexpr uint32_t kTexture1DCount = 1024;
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
// True when the vertex or pixel shader key starts with one of the
// comma-separated hex prefixes in `list`.
bool ShaderListed(std::string_view list, uint64_t vs_key, uint64_t ps_key) {
  const std::string vs_hex = fmt::format("{:016X}", vs_key);
  const std::string ps_hex = fmt::format("{:016X}", ps_key);
  for (const auto part : std::views::split(list, ',')) {
    const std::string_view want(part.begin(), part.end());
    if (!want.empty() && (vs_hex.starts_with(want) || ps_hex.starts_with(want))) return true;
  }
  return false;
}

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
constexpr uint32_t kRbStencilRefMaskBf = 0x210C;
constexpr uint32_t kRbStencilRefMask = 0x210D;
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

// Copies big-endian dwords to `dst` as little-endian ones. The upload ring is
// write-combined GPU memory, so it is only ever written: swapping in place
// there reads it back, which is many times slower than the copy.
void CopySwappedDwords(uint8_t* dst, const uint8_t* src, size_t bytes) {
  for (size_t i = 0; i + 3 < bytes; i += 4) {
    uint32_t v;
    std::memcpy(&v, src + i, 4);
    v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    std::memcpy(dst + i, &v, 4);
  }
}

// ---------------------------------------------------------------------------
// Translation tables.

plume::RenderComparisonFunction CompareFunction(uint32_t xenos_func) {
  // xenos::CompareFunction 0..7 is never, less, equal, lequal, greater,
  // notequal, gequal, always: Plume's order after UNKNOWN.
  return static_cast<plume::RenderComparisonFunction>(1 + (xenos_func & 7));
}

plume::RenderStencilOp StencilOp(uint32_t xenos_op) {
  // keep, zero, replace, incr_sat, decr_sat, invert, incr_wrap, decr_wrap:
  // Plume's order after UNKNOWN.
  return static_cast<plume::RenderStencilOp>(1 + (xenos_op & 7));
}

// One face of RB_DEPTHCONTROL's stencil state: function, then the fail,
// depth-pass and depth-fail operations, three bits each from `shift`.
plume::RenderStencilFaceDesc StencilFace(uint32_t depth_control, uint32_t shift) {
  plume::RenderStencilFaceDesc face;
  face.compareFunction = CompareFunction(depth_control >> shift);
  face.failOp = StencilOp(depth_control >> (shift + 3));
  face.passOp = StencilOp(depth_control >> (shift + 6));
  face.depthFailOp = StencilOp(depth_control >> (shift + 9));
  return face;
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
enum class ElementConvert : uint8_t { kCopy, kFloat4, kFloat4Bits, kPacked101111 };

struct ElementSource {
  xenos::VertexFormat format;
  bool is_signed;
  bool is_integer;
  // Declaration swizzle (format word bits 10-21), three bits per output
  // component: 0-3 pick x/y/z/w, 4 is 0, 5 is 1. D3D patches it into the
  // vfetch instructions when it binds a declaration, after the shader cache
  // was keyed, so the host applies it while gathering the stream.
  uint32_t swizzle;
};

uint32_t ComponentCount(xenos::VertexFormat format) {
  switch (format) {
    case xenos::VertexFormat::k_32_FLOAT:
      return 1;
    case xenos::VertexFormat::k_16_16:
    case xenos::VertexFormat::k_16_16_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
      return 2;
    case xenos::VertexFormat::k_10_11_11:
    case xenos::VertexFormat::k_11_11_10:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
      return 3;
    default:
      return 4;
  }
}

// True when the swizzle does more than the host fetch already does (absent
// components read as 0, absent w as 1).
bool SwizzleMatters(const ElementSource& source) {
  const uint32_t n = ComponentCount(source.format);
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t pick = (source.swizzle >> (3 * c)) & 7;
    const uint32_t natural = c < n ? c : (c == 3 ? 5 : 4);
    if (pick < 6 && pick != natural) return true;
  }
  return false;
}

void ApplySwizzle(uint32_t swizzle, float value[4]) {
  float in[4];
  std::memcpy(in, value, sizeof(in));
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t pick = (swizzle >> (3 * c)) & 7;
    if (pick < 4) {
      value[c] = in[pick];
    } else if (pick == 4) {
      value[c] = 0.0f;
    } else if (pick == 5) {
      value[c] = 1.0f;
    }
  }
}

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
  source.swizzle = (format_word >> 10) & 0xFFF;
  if (!REXCVAR_GET(native_decl_swizzle)) {
    // The fetch as is: present components in order, absent ones 0, w 1.
    const uint32_t n = ComponentCount(source.format);
    source.swizzle = 0;
    for (uint32_t c = 0; c < 4; ++c) source.swizzle |= (c < n ? c : (c == 3 ? 5u : 4u)) << (3 * c);
  }
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
  const bool swizzled = SwizzleMatters(source);
  if (raw) {
    if (!swizzled && (source.format == xenos::VertexFormat::k_11_11_10 ||
                      source.format == xenos::VertexFormat::k_10_11_11)) {
      out = F::R32_UINT;
      packed_normal = true;
      return true;
    }
    convert = ElementConvert::kFloat4Bits;
    out = F::R32G32B32A32_UINT;
    out_size = 16;
    return true;
  }
  if (swizzled) {
    convert = ElementConvert::kFloat4;
    out = F::R32G32B32A32_FLOAT;
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
  ApplySwizzle(source.swizzle, value);
  if (convert == ElementConvert::kPacked101111) {
    // XenosRecomp's tfetchR11G11B10: x and y 11-bit, z 10-bit, two's
    // complement over 1024, 1024 and 512.
    auto field = [](float v, float scale, int32_t lo, int32_t hi, uint32_t mask) {
      return uint32_t(std::clamp<int32_t>(int32_t(std::lround(v * scale)), lo, hi)) & mask;
    };
    const uint32_t packed = field(value[0], 1024.0f, -1024, 1023, 0x7FF) |
                            (field(value[1], 1024.0f, -1024, 1023, 0x7FF) << 11) |
                            (field(value[2], 512.0f, -512, 511, 0x3FF) << 22);
    std::memcpy(dst, &packed, 4);
    return;
  }
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

// Xenos fetches of one-component formats return that component in all four
// channels, and of two-component formats return the second one in z and w
// too. Host views of such formats read 0 and 1 there instead, so the fetch
// swizzle's picks are redirected to the components that exist.
uint32_t HostSwizzle(uint32_t swizzle, xenos::TextureFormat format) {
  uint32_t components = 4;
  switch (format) {
    case xenos::TextureFormat::k_8:
    case xenos::TextureFormat::k_8_A:
    case xenos::TextureFormat::k_8_B:
    case xenos::TextureFormat::k_DXT3A:
    case xenos::TextureFormat::k_DXT5A:
    case xenos::TextureFormat::k_16:
    case xenos::TextureFormat::k_16_EXPAND:
    case xenos::TextureFormat::k_16_FLOAT:
    case xenos::TextureFormat::k_32_FLOAT:
      components = 1;
      break;
    case xenos::TextureFormat::k_8_8:
    case xenos::TextureFormat::k_DXN:
    case xenos::TextureFormat::k_16_16:
    case xenos::TextureFormat::k_16_16_EXPAND:
    case xenos::TextureFormat::k_16_16_FLOAT:
    case xenos::TextureFormat::k_32_32_FLOAT:
      components = 2;
      break;
    default:
      return swizzle;
  }
  uint32_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t pick = (swizzle >> (3 * c)) & 7;
    if (pick < 4) pick = std::min(pick, components - 1);
    out |= pick << (3 * c);
  }
  return out;
}

// A resolve leaves 8:8:8:8 and 2:10:10:10 pixels in guest memory in D3D's
// ARGB order, which a fetch reads as BGRA, and the game's fetch swizzle
// (zyxw) turns back into RGBA. The host copy of a resolve keeps the target's
// RGBA order, so for it the swizzle's picks of x and z trade places.
uint32_t ResolvedSwizzle(uint32_t swizzle, xenos::TextureFormat format) {
  if (format != xenos::TextureFormat::k_8_8_8_8 && format != xenos::TextureFormat::k_2_10_10_10) return swizzle;
  uint32_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t pick = (swizzle >> (3 * c)) & 7;
    if (pick == 0) {
      pick = 2;
    } else if (pick == 2) {
      pick = 0;
    }
    out |= pick << (3 * c);
  }
  return out;
}

// A depth resolve's host copy holds depth in its only component, which is
// what the guest's k_24_8 fetch returns first; every pick of a texel
// component reads it.
uint32_t DepthSwizzle(uint32_t swizzle) {
  uint32_t out = 0;
  for (uint32_t c = 0; c < 4; ++c) {
    uint32_t pick = (swizzle >> (3 * c)) & 7;
    if (pick < 4) pick = 0;
    out |= pick << (3 * c);
  }
  return out;
}

// Names one guest image by its texture fetch constant. Clamp and filter
// bits are sampler state and are left out.
// Physical address of a texture base. Objects the game builds keep virtual
// 0xE0000000-range addresses; SetTexture writes the physical one, which is
// 0x1000 further on (the 4 KB physical heap's offset).
uint32_t TexturePhysical(uint32_t base_address_field) {
  const uint32_t address = base_address_field << 12;
  return address >= 0xE0000000u ? (address & 0x1FFFFFFFu) + 0x1000u : address & 0x1FFFFFFFu;
}

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

  // Swapchain size; the guest's 1280x720 frame is scaled to it, and every
  // render target uses the same scale.
  uint32_t width = 0;
  uint32_t height = 0;
  float scale_x = 1.0f;
  float scale_y = 1.0f;

  // Host render targets. Xenos keeps colour and depth in EDRAM, and surfaces
  // that share an EDRAM base share their pixels whatever their MSAA or the
  // other attachment (the title draws its scene at 4x and resolves the same
  // EDRAM through a 1x surface). So colour images are keyed by colour base,
  // format and pitch, depth images by depth base and pitch, and a framebuffer
  // pairs them. MSAA is not emulated: everything is one sample.
  // Surfaces the guest multisamples are drawn into a multisampled twin of
  // the image; `msaa_newer` says the twin holds pixels the single-sample
  // texture does not have yet, so a single-sample surface on the same EDRAM
  // (the title composites through one) resolves it first.
  struct Image {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTexture> msaa;
    uint32_t msaa_samples = 0;
    bool msaa_newer = false;
    uint32_t guest_width = 0;
    uint32_t guest_height = 0;
    uint32_t width = 0;  // host pixels
    uint32_t height = 0;
    // Depth images only: a view of the texture or its multisampled twin in
    // the 2D heap, for the depth resolve pass to read.
    std::unique_ptr<plume::RenderTextureView> depth_view;
    const plume::RenderTexture* depth_view_of = nullptr;
    uint32_t depth_index = 0;
  };
  struct Target {
    Image* color = nullptr;
    Image* depth = nullptr;
    uint32_t samples = 1;
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  };
  std::unordered_map<uint64_t, Image> color_images;
  std::unordered_map<uint64_t, Image> depth_images;
  std::unordered_map<uint64_t, Target> targets;
  Target* current = nullptr;
  plume::RenderTexture* color = nullptr;  // current target's
  uint32_t current_samples = 1;
  plume::RenderTexture* depth = nullptr;
  plume::RenderFramebuffer* framebuffer = nullptr;
  uint32_t target_width = 0;  // current target, host pixels
  uint32_t target_height = 0;
  uint64_t target_switches = 0;

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
    plume::RenderFormat format = kColorFormat;
    // Depth resolves draw into the texture.
    std::unique_ptr<plume::RenderFramebuffer> framebuffer;
  };
  std::unordered_map<uint64_t, Texture> textures;
  uint32_t next_texture2d = 1;  // 0 is white in every heap
  // 1D textures hold per-draw data the CPU rewrites every frame (skinned
  // meshes fetch their bone matrices from one), so they live in a pool that
  // is refilled each frame: slot i is heap index i + 1, and the same texels
  // used again in the frame (shadow, body and outline passes) share a slot.
  struct Texture1DSlot {
    std::unique_ptr<plume::RenderTexture> texture;
    std::unique_ptr<plume::RenderTextureView> view;
    uint32_t width = 0;
    plume::RenderFormat format = plume::RenderFormat::UNKNOWN;
    uint32_t swizzle = 0;
  };
  std::vector<Texture1DSlot> texture1d_slots;
  // Present pass: the frontbuffer through the game's gamma ramp.
  std::unique_ptr<plume::RenderShader> present_vs, present_ps;
  std::unique_ptr<plume::RenderPipeline> present_pipeline;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> swap_framebuffers;
  std::array<uint32_t, 256> gamma_ramp{};
  bool has_gamma_ramp = false;

  // Depth resolve pass: a depth buffer into an R32_FLOAT texture.
  std::unique_ptr<plume::RenderShader> depth_resolve_vs, depth_resolve_ps;
  std::unique_ptr<plume::RenderPipeline> depth_resolve_pipeline;

  void CreateDepthResolvePipeline() {
    depth_resolve_vs = device->createShader(g_depth_resolve_vs, sizeof(g_depth_resolve_vs), "VSMain",
                                            plume::RenderShaderFormat::SPIRV);
    depth_resolve_ps = device->createShader(g_depth_resolve_ps, sizeof(g_depth_resolve_ps), "PSMain",
                                            plume::RenderShaderFormat::SPIRV);
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout.get();
    desc.vertexShader = depth_resolve_vs.get();
    desc.pixelShader = depth_resolve_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = kDepthCopyFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    depth_resolve_pipeline = device->createGraphicsPipeline(desc);
  }

  void CreatePresentPipeline() {
    present_vs = device->createShader(g_present_vs, sizeof(g_present_vs), "VSMain", plume::RenderShaderFormat::SPIRV);
    present_ps = device->createShader(g_present_ps, sizeof(g_present_ps), "PSMain", plume::RenderShaderFormat::SPIRV);
    plume::RenderGraphicsPipelineDesc desc;
    desc.pipelineLayout = layout.get();
    desc.vertexShader = present_vs.get();
    desc.pixelShader = present_ps.get();
    desc.primitiveTopology = plume::RenderPrimitiveTopology::TRIANGLE_LIST;
    desc.cullMode = plume::RenderCullMode::NONE;
    desc.renderTargetCount = 1;
    desc.renderTargetFormat[0] = kColorFormat;
    desc.renderTargetBlend[0] = plume::RenderBlendDesc::Copy();
    present_pipeline = device->createGraphicsPipeline(desc);
  }

  // Bilinear, clamped sampler for scaling the frontbuffer to the window.
  uint32_t PresentSampler() {
    xenos::xe_gpu_texture_fetch_t fetch{};
    fetch.clamp_x = fetch.clamp_y = fetch.clamp_z = xenos::ClampMode::kClampToEdge;
    fetch.mag_filter = fetch.min_filter = xenos::TextureFilter::kLinear;
    return SamplerFor(fetch);
  }

  plume::RenderFramebuffer* SwapFramebuffer(uint32_t image) {
    if (swap_framebuffers.size() != swap_chain->getTextureCount()) {
      swap_framebuffers.clear();
      swap_framebuffers.resize(swap_chain->getTextureCount());
    }
    auto& framebuffer = swap_framebuffers[image];
    if (!framebuffer) {
      const plume::RenderTexture* attachment = swap_chain->getTexture(image);
      plume::RenderFramebufferDesc desc;
      desc.colorAttachments = &attachment;
      desc.colorAttachmentsCount = 1;
      framebuffer = device->createFramebuffer(desc);
    }
    return framebuffer.get();
  }
  // Cube textures, keyed like 2D ones, in the cube heap.
  std::unordered_map<uint64_t, Texture> cube_textures;
  uint32_t next_texture_cube = 1;
  uint32_t next_texture1d = 0;
  std::unordered_map<uint64_t, uint32_t> texture1d_frame;
  std::vector<std::unique_ptr<plume::RenderTexture>> defaults;
  std::unordered_map<uint32_t, uint32_t> sampler_index;
  std::vector<std::unique_ptr<plume::RenderSampler>> samplers;

  std::string last_viewport;
  std::string last_bounds;
  std::map<std::string, uint64_t> viewports;  // VTE_CNTL and viewport per draw, for the log
  std::string dump;  // lines for native_dump_frame
  uint64_t outline_dump = 0;  // present to dump for native_dump_outline
  std::array<uint64_t, 8> cull_modes{};  // triangle draws per PA_SU_SC_MODE_CNTL & 7
  // Vertex streams per (fetch constant endian, element endian) pair.
  std::array<uint64_t, 16> stream_endians{};
  bool Dumping() const {
    return REXCVAR_GET(native_dump_frame) == int32_t(presents + 1) || outline_dump == presents + 1;
  }
  bool recording = false;
  uint64_t clears = 0;
  uint64_t resolves = 0;
  uint64_t reuploads = 0;
  // Guest base address of every resolve destination -> its texture key. The
  // game samples a resolved image through fetch constants that differ from
  // the destination object's (format view, flags), so the address decides.
  // Resolve destinations by physical address. With Xenos off nothing writes
  // the guest memory a resolve targets, so if its first page changes the CPU
  // has put something else there (a texture streamed into freed memory) and
  // the address no longer means the resolved image.
  struct ResolvedAt {
    uint64_t key;
    uint64_t guest;
  };
  std::unordered_map<uint32_t, ResolvedAt> resolved_at;

  static uint64_t GuestPageHash(const uint8_t* base, uint32_t physical) {
    return XXH3_64bits(At(base, PhysicalView(physical)), 4096);
  }

  // A guest D3DRECT {x1, y1, x2, y2} scaled to the host target.
  plume::RenderRect HostRect(const uint8_t* base, uint32_t rect) const {
    auto at = [&](uint32_t i) { return int32_t(Be32(base, rect + 4 * i)); };
    plume::RenderRect out(int32_t(std::lround(at(0) * scale_x)), int32_t(std::lround(at(1) * scale_y)),
                          int32_t(std::lround(at(2) * scale_x)), int32_t(std::lround(at(3) * scale_y)));
    out.left = std::clamp<int32_t>(out.left, 0, int32_t(target_width));
    out.right = std::clamp<int32_t>(out.right, out.left, int32_t(target_width));
    out.top = std::clamp<int32_t>(out.top, 0, int32_t(target_height));
    out.bottom = std::clamp<int32_t>(out.bottom, out.top, int32_t(target_height));
    return out;
  }

  // Guest rows a rect reaches (its bottom), for sizing the target.
  static uint32_t RectBottom(const uint8_t* base, uint32_t rect) {
    return rect ? uint32_t(std::max<int32_t>(0, int32_t(Be32(base, rect + 12)))) : 0;
  }

  // Makes sure `image` holds `rows` guest rows of `pitch` pixels. Returns
  // true when it was (re)created, which drops framebuffers built on it.
  bool EnsureImage(Image& image, uint32_t pitch, uint32_t rows, bool is_depth, uint32_t samples = 1) {
    bool fresh = false;
    if (!image.texture || image.guest_width != pitch || image.guest_height < rows) {
      CreateImage(image, pitch, rows, is_depth);
      fresh = true;
    }
    if (samples > 1 && (fresh || !image.msaa || image.msaa_samples != samples)) {
      const plume::RenderMultisampling multisampling(static_cast<plume::RenderSampleCounts>(samples));
      Retire(image.msaa);
      image.msaa = device->createTexture(
          is_depth ? plume::RenderTextureDesc::DepthTarget(image.width, image.height, kDepthFormat, multisampling)
                   : plume::RenderTextureDesc::ColorTarget(image.width, image.height, kColorFormat, multisampling));
      image.msaa_samples = samples;
      image.msaa_newer = false;
      commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(image.msaa.get(), is_depth
                                                                           ? plume::RenderTextureLayout::DEPTH_WRITE
                                                                           : plume::RenderTextureLayout::COLOR_WRITE));
      fresh = true;
    }
    return fresh;
  }

  // Writes the multisampled twin's pixels into the single-sample texture.
  void ResolveMsaa(Image& image) {
    if (!image.msaa_newer || !image.msaa) return;
    plume::RenderTextureBarrier to_resolve[] = {
        plume::RenderTextureBarrier(image.msaa.get(), plume::RenderTextureLayout::RESOLVE_SOURCE),
        plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::RESOLVE_DEST)};
    commands->barriers(plume::RenderBarrierStage::COPY, to_resolve, 2);
    commands->resolveTexture(image.texture.get(), image.msaa.get());
    plume::RenderTextureBarrier back[] = {
        plume::RenderTextureBarrier(image.msaa.get(), plume::RenderTextureLayout::COLOR_WRITE),
        plume::RenderTextureBarrier(image.texture.get(), plume::RenderTextureLayout::COLOR_WRITE)};
    commands->barriers(plume::RenderBarrierStage::GRAPHICS, back, 2);
    image.msaa_newer = false;
    ++msaa_resolves;
  }

  void CreateImage(Image& image, uint32_t pitch, uint32_t rows, bool is_depth) {
    // The multisampled twin must match the new size, so it is made again
    // the next time a multisampled surface uses the image.
    Retire(image.msaa);
    image.msaa_samples = 0;
    image.msaa_newer = false;
    Retire(image.texture);
    image.guest_width = pitch;
    image.guest_height = std::max(rows, image.guest_height);
    if (pitch == 1280) image.guest_height = std::max<uint32_t>(image.guest_height, 720);
    image.width = std::max(1u, uint32_t(std::lround(image.guest_width * scale_x)));
    image.height = std::max(1u, uint32_t(std::lround(image.guest_height * scale_y)));
    image.texture = device->createTexture(
        is_depth ? plume::RenderTextureDesc::DepthTarget(image.width, image.height, kDepthFormat)
                 : plume::RenderTextureDesc::ColorTarget(image.width, image.height, kColorFormat));
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(image.texture.get(), is_depth
                                                                            ? plume::RenderTextureLayout::DEPTH_WRITE
                                                                            : plume::RenderTextureLayout::COLOR_WRITE));
  }

  // Host samples for the surface the device's RB_SURFACE_INFO describes
  // (bits 16-17: 1, 2 or 4 samples), capped by native_msaa. Draws build their
  // pipeline with it before the target is bound, so both must agree.
  static uint32_t SurfaceSamples(const uint8_t* base, uint32_t guest_device) {
    const uint32_t surface = Reg(base, guest_device, 0x2000);
    const uint32_t guest_samples = 1u << std::min<uint32_t>((surface >> 16) & 3, 2);
    return std::clamp<uint32_t>(guest_samples, 1, uint32_t(std::max<int32_t>(REXCVAR_GET(native_msaa), 1)));
  }

  // Binds the target the device's EDRAM registers describe, creating or
  // growing its images so they hold `rows` guest rows.
  void SelectTarget(const uint8_t* base, uint32_t guest_device, uint32_t rows) {
    const uint32_t surface = Reg(base, guest_device, 0x2000);  // RB_SURFACE_INFO
    const uint32_t color_info = Reg(base, guest_device, 0x2001);
    const uint32_t depth_info = Reg(base, guest_device, 0x2002);
    const uint32_t pitch = std::max<uint32_t>(surface & 0x3FFF, 1);
    const uint32_t samples = SurfaceSamples(base, guest_device);
    rows = std::clamp<uint32_t>(rows, 1, 2048);
    const uint64_t color_key = (uint64_t(pitch) << 32) | (color_info & 0x000F0FFF);
    const uint64_t depth_key = (uint64_t(pitch) << 32) | (depth_info & 0x00010FFF);
    Image& color_image = color_images[color_key];
    Image& depth_image = depth_images[depth_key];
    bool fresh_color = EnsureImage(color_image, pitch, rows, false, samples);
    bool fresh_depth = EnsureImage(depth_image, pitch, rows, true, samples);
    // Both attachments must be the same size.
    if (depth_image.guest_height < color_image.guest_height) {
      fresh_depth |= EnsureImage(depth_image, pitch, color_image.guest_height, true, samples);
    } else if (color_image.guest_height < depth_image.guest_height) {
      fresh_color |= EnsureImage(color_image, pitch, depth_image.guest_height, false, samples);
    }
    if (fresh_color || fresh_depth) {
      for (auto it = targets.begin(); it != targets.end();) {
        if ((fresh_color && it->second.color == &color_image) || (fresh_depth && it->second.depth == &depth_image)) {
          if (current == &it->second) current = nullptr;
          Retire(it->second.framebuffer);
          it = targets.erase(it);
        } else {
          ++it;
        }
      }
    }
    Target& target = targets[(color_key * 1000003u ^ depth_key) * 8 + samples];
    if (!target.framebuffer) {
      target.color = &color_image;
      target.depth = &depth_image;
      target.samples = samples;
      const plume::RenderTexture* attachment =
          samples > 1 ? color_image.msaa.get() : color_image.texture.get();
      plume::RenderFramebufferDesc desc;
      desc.colorAttachments = &attachment;
      desc.colorAttachmentsCount = 1;
      desc.depthAttachment = samples > 1 ? depth_image.msaa.get() : depth_image.texture.get();
      target.framebuffer = device->createFramebuffer(desc);
      current = nullptr;
    }
    if (UseTarget(target)) {
      if (fresh_color) commands->clearColor(0, plume::RenderColor(0.0f, 0.0f, 0.0f, 1.0f));
      if (fresh_depth) commands->clearDepthStencil(true, true, 1.0f, 0);
    }
  }

  // Makes `target` the one draws go to. Returns true when it was not already.
  bool UseTarget(Target& target) {
    Image& color_image = *target.color;
    Image& depth_image = *target.depth;
    if (target.samples > 1) {
      color_image.msaa_newer = true;
    } else {
      ResolveMsaa(color_image);
    }
    if (current == &target) return false;
    ++target_switches;
    current = &target;
    current_samples = target.samples;
    color = target.samples > 1 ? color_image.msaa.get() : color_image.texture.get();
    depth = target.samples > 1 ? depth_image.msaa.get() : depth_image.texture.get();
    framebuffer = target.framebuffer.get();
    target_width = color_image.width;
    target_height = color_image.height;
    BindTarget();
    return true;
  }

  uint64_t presents = 0;
  uint64_t msaa_resolves = 0;
  // Vblanks per flip the game last asked for (2 at 30 Hz, 1 at 60 Hz).
  std::atomic<uint32_t> flip_interval{0};
  uint64_t depth_resolves = 0;

  // GPU objects replaced while a frame is being recorded. The command list
  // may still reference them (a framebuffer, a barrier, a copy), and
  // destroying them invalidates it, which NVIDIA's driver does not survive.
  // They are destroyed once the frame's fence has passed.
  std::vector<std::unique_ptr<plume::RenderTexture>> retired_textures;
  std::vector<std::unique_ptr<plume::RenderTextureView>> retired_views;
  std::vector<std::unique_ptr<plume::RenderFramebuffer>> retired_framebuffers;

  void Retire(std::unique_ptr<plume::RenderTexture>& texture) {
    if (texture) retired_textures.push_back(std::move(texture));
  }
  void Retire(std::unique_ptr<plume::RenderTextureView>& view) {
    if (view) retired_views.push_back(std::move(view));
  }
  void Retire(std::unique_ptr<plume::RenderFramebuffer>& framebuffer) {
    if (framebuffer) retired_framebuffers.push_back(std::move(framebuffer));
  }
  void DestroyRetired() {
    retired_framebuffers.clear();
    retired_views.clear();
    retired_textures.clear();
  }

  std::mutex mutex;  // Draw and Present run on the guest thread, TakeStats on the sampler
  std::map<std::string, uint64_t> skipped;
  uint64_t drawn = 0;
  double ms_pipeline = 0, ms_texture = 0, ms_stream = 0, ms_submit = 0;
  // Wall time inside each entry point, for the 5 s stats.
  double ms_draw = 0, ms_clear = 0, ms_resolve = 0, ms_present = 0;
  // Draw split in sections: setup, streams, state, constants+textures, indices, record, dump.
  std::array<double, 7> ms_lap{};
  // Reused CPU buffers for gathering a stream before it goes to the
  // write-combined upload ring in one copy.
  std::vector<uint8_t> stream_raw, stream_out;
  // Converted vertex streams kept in their own buffers, keyed by the guest
  // bytes and the layout they were converted with. Static geometry is then
  // converted once instead of every frame. A stream gets a buffer the second
  // time it is seen, so data rewritten every frame stays in the ring.
  struct CachedStream {
    std::unique_ptr<plume::RenderBuffer> buffer;
    uint64_t last_used = 0;
    uint32_t seen = 0;
  };
  std::unordered_map<uint64_t, CachedStream> stream_cache;
  uint64_t stream_hits = 0, stream_misses = 0;
  std::chrono::steady_clock::time_point lap_start;
  void Lap(size_t section) {
    const auto now = std::chrono::steady_clock::now();
    ms_lap[section] += std::chrono::duration<double, std::milli>(now - lap_start).count();
    lap_start = now;
  }

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
    scale_x = float(width) / kGuestWidth;
    scale_y = float(height) / kGuestHeight;
    if (const double scale = REXCVAR_GET(native_resolution_scale); scale > 0.0) {
      scale_x = scale_y = float(std::clamp(scale, 0.25, 4.0));
    }
    targets.clear();
    color_images.clear();
    depth_images.clear();
    current = nullptr;
    color = nullptr;
    depth = nullptr;
    framebuffer = nullptr;
    return width > 0 && height > 0;
  }

  void BeginFrame() {
    if (recording) return;
    recording = true;
    upload_used = 0;
    next_texture1d = 0;
    texture1d_frame.clear();
    journal.clear();
    draw_ops.clear();
    clear_ops.clear();
    resolve_ops.clear();
    commands->begin();
    // Targets keep their contents across frames (the game draws before Swap
    // and resolves after it); a new command list only needs them rebound.
    current = nullptr;
  }

  void BindTarget() {
    if (framebuffer) commands->setFramebuffer(framebuffer);
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
    if (layout) {
      CreatePresentPipeline();
      CreateDepthResolvePipeline();
    }
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

  // Texture index in the 1D heap for one fetch constant, 0 (white) when the
  // format is not handled or the pool is full.
  uint32_t Texture1DFor(const uint32_t words[6], const xenos::xe_gpu_texture_fetch_t& fetch,
                        const uint8_t* base, bool& uploaded) {
    const uint32_t format = uint32_t(fetch.format);
    const plume::RenderFormat host = HostTextureFormat(format);
    if (host == plume::RenderFormat::UNKNOWN) {
      Skip(fmt::format("1d texture format {}", format));
      return 0;
    }
    const uint32_t width = fetch.size_1d.width + 1;
    const uint32_t pitch = std::max<uint32_t>(fetch.pitch * 32, (width + 31) & ~31u);
    const uint32_t bytes = gfx::GuestTextureBytesAny(format, width, 1, pitch, fetch.tiled);
    if (bytes == 0 || width > 16384) {
      Skip("1d texture size");
      return 0;
    }
    const uint8_t* guest = At(base, PhysicalView(fetch.base_address << 12));
    const uint64_t frame_key = ImageKey(words) * 0x9E3779B97F4A7C15ull ^ XXH3_64bits(guest, bytes);
    if (auto found = texture1d_frame.find(frame_key); found != texture1d_frame.end()) return found->second;
    if (next_texture1d + 1 >= kTexture1DCount) {
      Skip("1d texture pool full");
      return 0;
    }
    gfx::LinearTexture linear;
    std::string reason;
    if (!gfx::LoadGuestTextureLinear(format, width, 1, pitch, uint32_t(fetch.endianness), fetch.tiled, guest,
                                     bytes, linear, reason)) {
      Skip("1d texture layout");
      return 0;
    }
    const size_t row_bytes = size_t(linear.width_blocks) * linear.block_bytes;
    const size_t row = (row_bytes + 255) & ~size_t(255);
    uint64_t offset = 0;
    if (!Upload(row * linear.height_blocks, 512, offset)) {
      Skip("upload ring full");
      return 0;
    }
    for (uint32_t y = 0; y < linear.height_blocks; ++y) {
      std::memcpy(upload_cpu + offset + y * row, linear.blocks.data() + y * row_bytes, row_bytes);
    }
    if (Dumping() && REXCVAR_GET(native_dump_texture).find("1d") != std::string::npos) {
      const std::string path = fmt::format("logs/texture1d-{}-{:08X}-f{}-w{}.bin", next_texture1d + 1,
                                           fetch.base_address << 12, format, width);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(linear.blocks.data(), 1, linear.blocks.size(), f);
        std::fclose(f);
      }
    }
    if (texture1d_slots.size() <= next_texture1d) texture1d_slots.resize(next_texture1d + 1);
    Texture1DSlot& slot = texture1d_slots[next_texture1d];
    const uint32_t index = next_texture1d + 1;
    const uint32_t swizzle = HostSwizzle(fetch.swizzle, fetch.format);
    if (!slot.texture || slot.width != width || slot.format != host || slot.swizzle != swizzle) {
      Retire(slot.view);
      Retire(slot.texture);
      slot.texture = device->createTexture(plume::RenderTextureDesc::Texture1D(width, 1, host));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture1D(host);
      view.componentMapping = ComponentMapping(swizzle);
      slot.view = slot.texture->createTextureView(view);
      slot.width = width;
      slot.format = host;
      slot.swizzle = swizzle;
    }
    commands->barriers(plume::RenderBarrierStage::COPY,
                       plume::RenderTextureBarrier(slot.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    const uint32_t row_texels = uint32_t(row / linear.block_bytes) * linear.block_width;
    commands->copyTextureRegion(
        plume::RenderTextureCopyLocation::Subresource(slot.texture.get()),
        plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), host, width, 1, 1, row_texels, offset));
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(slot.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    sets[5]->setTexture(index, slot.texture.get(), plume::RenderTextureLayout::SHADER_READ, slot.view.get());
    ++next_texture1d;
    texture1d_frame[frame_key] = index;
    uploaded = true;
    return index;
  }

  // Texture index in the cube heap for one fetch constant, 0 (white) when the
  // format or layout is not handled. Xenos keeps the six faces as array
  // slices of the base level, each aligned like a separate 2D level.
  uint32_t TextureCubeFor(const uint32_t words[6], const xenos::xe_gpu_texture_fetch_t& fetch,
                          const uint8_t* base, bool& uploaded) {
    const uint64_t key = ImageKey(words);
    auto found = cube_textures.find(key);
    if (found != cube_textures.end() && found->second.checked == presents) return found->second.index;
    const uint32_t format = uint32_t(fetch.format);
    const plume::RenderFormat host = HostTextureFormat(format);
    if (host == plume::RenderFormat::UNKNOWN) {
      Skip(fmt::format("cube texture format {}", format));
      return 0;
    }
    const uint32_t width = fetch.size_2d.width + 1;
    const uint32_t height = fetch.size_2d.height + 1;
    const uint32_t pitch = fetch.pitch * 32;
    const auto layout = rex::graphics::texture_util::GetGuestTextureLayout(
        xenos::DataDimension::kCube, fetch.pitch, width, height, 6, fetch.tiled, fetch.format, fetch.packed_mips,
        true, 0);
    const uint32_t face_stride = layout.base.array_slice_stride_bytes;
    const uint32_t face_bytes =
        gfx::GuestTextureBaseBytes(format, width, height, pitch, fetch.tiled, fetch.packed_mips);
    if (face_bytes == 0 || face_stride == 0) {
      Skip("cube texture size");
      return 0;
    }
    const uint8_t* guest = At(base, PhysicalView(fetch.base_address << 12));
    const uint64_t content = XXH3_64bits(guest, size_t(face_stride) * 5 + face_bytes);
    if (found != cube_textures.end()) {
      found->second.checked = presents;
      if (found->second.content == content) return found->second.index;
      ++reuploads;
    } else if (next_texture_cube >= kTextureCubeCount) {
      return 0;
    }
    std::array<gfx::LinearTexture, 6> faces;
    for (uint32_t face = 0; face < 6; ++face) {
      std::string reason;
      if (!gfx::LoadGuestTextureBase(format, width, height, pitch, uint32_t(fetch.endianness), fetch.tiled,
                                     fetch.packed_mips, guest + size_t(face) * face_stride, face_bytes,
                                     faces[face], reason)) {
        Skip("cube texture layout");
        return found != cube_textures.end() ? found->second.index : 0;
      }
    }
    if (found == cube_textures.end()) {
      Texture texture;
      texture.texture = device->createTexture(plume::RenderTextureDesc::Texture(
          plume::RenderTextureDimension::TEXTURE_2D, width, height, 1, 1, 6, host, plume::RenderTextureFlag::CUBE));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::TextureCube(host);
      view.componentMapping = ComponentMapping(HostSwizzle(fetch.swizzle, fetch.format));
      texture.view = texture.texture->createTextureView(view);
      texture.index = next_texture_cube++;
      found = cube_textures.emplace(key, std::move(texture)).first;
    }
    Texture& texture = found->second;
    texture.content = content;
    texture.checked = presents;
    commands->barriers(plume::RenderBarrierStage::COPY,
                       plume::RenderTextureBarrier(texture.texture.get(), plume::RenderTextureLayout::COPY_DEST));
    for (uint32_t face = 0; face < 6; ++face) {
      const gfx::LinearTexture& linear = faces[face];
      const size_t row_bytes = size_t(linear.width_blocks) * linear.block_bytes;
      const size_t row = (row_bytes + 255) & ~size_t(255);
      uint64_t offset = 0;
      if (!Upload(row * linear.height_blocks, 512, offset)) {
        Skip("upload ring full");
        return texture.index;
      }
      for (uint32_t y = 0; y < linear.height_blocks; ++y) {
        std::memcpy(upload_cpu + offset + y * row, linear.blocks.data() + y * row_bytes, row_bytes);
      }
      const uint32_t row_texels = uint32_t(row / linear.block_bytes) * linear.block_width;
      commands->copyTextureRegion(
          plume::RenderTextureCopyLocation::Subresource(texture.texture.get(), 0, face),
          plume::RenderTextureCopyLocation::PlacedFootprint(upload.get(), host, width, height, 1, row_texels,
                                                            offset));
    }
    commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                       plume::RenderTextureBarrier(texture.texture.get(), plume::RenderTextureLayout::SHADER_READ));
    sets[2]->setTexture(texture.index, texture.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                        texture.view.get());
    uploaded = true;
    return texture.index;
  }

  // Texture index in the 2D heap for one fetch constant, 0 (white) when the
  // format is not handled yet. Texels are uploaded untiled and endian-swapped
  // but otherwise as the guest stores them; the fetch swizzle becomes the
  // view's component mapping. Guest memory is hashed the first time a texture
  // is used in a frame, and changed texels (Bink video, CPU-written images)
  // are uploaded again.
  uint32_t Texture2DFor(const uint32_t words[6], const xenos::xe_gpu_texture_fetch_t& fetch,
                        const uint8_t* base, bool& uploaded) {
    const uint32_t physical = TexturePhysical(fetch.base_address);
    if (auto at = resolved_at.find(physical); at != resolved_at.end()) {
      auto resolved = textures.find(at->second.key);
      if (resolved != textures.end() && at->second.guest == GuestPageHash(base, physical)) {
        return resolved->second.index;
      }
      resolved_at.erase(at);
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
    const uint32_t bytes = gfx::GuestTextureBaseBytes(format, width, height, pitch, fetch.tiled, fetch.packed_mips);
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
    if (!gfx::LoadGuestTextureBase(format, width, height, pitch, uint32_t(fetch.endianness), fetch.tiled,
                                   fetch.packed_mips, guest, bytes, linear, reason)) {
      Skip("texture layout");
      return found != textures.end() ? found->second.index : 0;
    }
    const size_t row_bytes = size_t(linear.width_blocks) * linear.block_bytes;
    const size_t row = (row_bytes + 255) & ~size_t(255);
    bool dump_texture = false;
    for (const auto part : std::views::split(std::string_view(REXCVAR_GET(native_dump_texture)), ',')) {
      const std::string want(part.begin(), part.end());
      dump_texture |= !want.empty() && (want == fmt::format("{}x{}", width, height) ||
                                        std::strtoul(want.c_str(), nullptr, 16) == (fetch.base_address << 12));
    }
    if (dump_texture) {
      const std::string path =
          fmt::format("logs/texture-{:08X}-f{}-{}x{}.bin", fetch.base_address << 12, format, width, height);
      if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(linear.blocks.data(), 1, linear.blocks.size(), f);
        std::fclose(f);
      }
    }
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
      view.componentMapping = ComponentMapping(HostSwizzle(fetch.swizzle, fetch.format));
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

  // Frame journal (native_interpolate): what each guest call recorded, in
  // order, so the frame can be recorded a second time. Everything a call read
  // from guest memory is resolved into the op; data lives in the upload ring,
  // which holds until the next BeginFrame.
  struct DrawOp {
    Target* target = nullptr;
    plume::RenderPipeline* pipeline = nullptr;
    uint64_t vs_constants = 0, ps_constants = 0, shared = 0;  // upload ring offsets
    plume::RenderViewport viewport;
    std::array<plume::RenderVertexBufferView, 16> views;
    std::array<plume::RenderInputSlot, 16> slots;
    uint32_t slot_count = 0;
    bool indexed = false;
    uint64_t index_ring = 0;
    uint32_t index_count = 0;
    uint32_t vertex_count = 0;
    bool uploaded = false;  // textures were uploaded first, so rebind
    uint64_t key = 0;       // interp::DrawKey
  };
  struct ClearOp {
    Target* target = nullptr;
    uint32_t flags = 0;
    plume::RenderRect rect;
    plume::RenderColor color;
    float z = 0.0f;
    uint32_t stencil = 0;
  };
  struct ResolveOp {
    Target* target = nullptr;
    enum Kind { kNone, kColor, kDepth } kind = kNone;
    uint64_t key = 0;  // destination in `textures`
    plume::RenderRect src;
    int32_t dst_x = 0, dst_y = 0;
    int32_t copy_w = 0, copy_h = 0;  // depth
    uint64_t constants = 0;          // depth pass constants in the upload ring
    uint32_t clear_flags = 0;        // 0x100 color, 0x200 depth and stencil
    plume::RenderRect clear_region;
    plume::RenderColor clear_color;
    float z = 0.0f;
  };
  struct FrameOp {
    enum Kind { kDraw, kClear, kResolve } kind;
    uint32_t index;
  };
  std::vector<FrameOp> journal;
  std::vector<DrawOp> draw_ops;
  std::vector<ClearOp> clear_ops;
  std::vector<ResolveOp> resolve_ops;
  size_t journal_last = 0;  // ops the last presented frame recorded

  void Record(const DrawOp& op) {
    if (!REXCVAR_GET(native_interpolate)) return;
    journal.push_back({FrameOp::kDraw, uint32_t(draw_ops.size())});
    draw_ops.push_back(op);
  }
  void Record(const ClearOp& op) {
    if (!REXCVAR_GET(native_interpolate)) return;
    journal.push_back({FrameOp::kClear, uint32_t(clear_ops.size())});
    clear_ops.push_back(op);
  }
  void Record(const ResolveOp& op) {
    if (!REXCVAR_GET(native_interpolate)) return;
    journal.push_back({FrameOp::kResolve, uint32_t(resolve_ops.size())});
    resolve_ops.push_back(op);
  }

  void ExecuteDraw(const DrawOp& op) {
    UseTarget(*op.target);
    if (op.uploaded) BindTarget();
    commands->setPipeline(op.pipeline);
    const uint64_t push[3] = {upload_gpu + op.vs_constants, upload_gpu + op.ps_constants, upload_gpu + op.shared};
    commands->setGraphicsPushConstants(0, push, 0, sizeof(push));
    commands->setViewports(&op.viewport, 1);
    const plume::RenderRect scissor(0, 0, int32_t(target_width), int32_t(target_height));
    commands->setScissors(&scissor, 1);
    if (op.slot_count) commands->setVertexBuffers(0, op.views.data(), op.slot_count, op.slots.data());
    if (op.indexed) {
      const plume::RenderIndexBufferView view(upload->at(op.index_ring), op.index_count * 4,
                                              plume::RenderFormat::R32_UINT);
      commands->setIndexBuffer(&view);
      commands->drawIndexedInstanced(op.index_count, 1, 0, 0, 0);
    } else {
      commands->drawInstanced(op.vertex_count, 1, 0, 0);
    }
  }

  void ExecuteClear(const ClearOp& op) {
    UseTarget(*op.target);
    if (op.flags & 0xF) commands->clearColor(0, op.color, &op.rect, 1);
    if (op.flags & 0x30) {
      commands->clearDepthStencil((op.flags & 0x10) != 0, (op.flags & 0x20) != 0, std::clamp(op.z, 0.0f, 1.0f),
                                  op.stencil & 0xFF, &op.rect, 1);
    }
  }

  void ExecuteResolve(const ResolveOp& op) {
    UseTarget(*op.target);
    if (op.kind == ResolveOp::kDepth) {
      Texture& entry = textures[op.key];
      Image& depth_image = *op.target->depth;
      plume::RenderTextureBarrier to_pass[] = {
          plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::DEPTH_READ),
          plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::COLOR_WRITE)};
      commands->barriers(plume::RenderBarrierStage::GRAPHICS, to_pass, 2);
      sets[0]->setTexture(depth_image.depth_index, depth, plume::RenderTextureLayout::DEPTH_READ,
                          depth_image.depth_view.get());
      commands->setFramebuffer(entry.framebuffer.get());
      commands->setGraphicsPipelineLayout(layout.get());
      for (uint32_t i = 0; i < 6; ++i) commands->setGraphicsDescriptorSet(sets[i].get(), i);
      commands->setPipeline(depth_resolve_pipeline.get());
      const uint64_t push[3] = {upload_gpu + op.constants, 0, 0};
      commands->setGraphicsPushConstants(0, push, 0, sizeof(push));
      const plume::RenderViewport viewport{float(op.dst_x), float(op.dst_y), float(op.copy_w), float(op.copy_h)};
      commands->setViewports(&viewport, 1);
      const plume::RenderRect scissor(op.dst_x, op.dst_y, op.dst_x + op.copy_w, op.dst_y + op.copy_h);
      commands->setScissors(&scissor, 1);
      commands->drawInstanced(3, 1, 0, 0);
      plume::RenderTextureBarrier back[] = {
          plume::RenderTextureBarrier(depth, plume::RenderTextureLayout::DEPTH_WRITE),
          plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::SHADER_READ)};
      commands->barriers(plume::RenderBarrierStage::GRAPHICS, back, 2);
      sets[0]->setTexture(entry.index, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                          entry.view.get());
    } else if (op.kind == ResolveOp::kColor) {
      Texture& entry = textures[op.key];
      const bool multisampled = current_samples > 1;
      plume::RenderTextureBarrier to_copy[] = {
          plume::RenderTextureBarrier(color, multisampled ? plume::RenderTextureLayout::RESOLVE_SOURCE
                                                          : plume::RenderTextureLayout::COPY_SOURCE),
          plume::RenderTextureBarrier(entry.texture.get(), multisampled ? plume::RenderTextureLayout::RESOLVE_DEST
                                                                        : plume::RenderTextureLayout::COPY_DEST)};
      commands->barriers(plume::RenderBarrierStage::COPY, to_copy, 2);
      if (multisampled) {
        commands->resolveTextureRegion(entry.texture.get(), uint32_t(op.dst_x), uint32_t(op.dst_y), color, &op.src);
      } else {
        const plume::RenderBox box(op.src.left, op.src.top, op.src.right, op.src.bottom);
        commands->copyTextureRegion(plume::RenderTextureCopyLocation::Subresource(entry.texture.get()),
                                    plume::RenderTextureCopyLocation::Subresource(color), uint32_t(op.dst_x),
                                    uint32_t(op.dst_y), 0, &box);
      }
      plume::RenderTextureBarrier back[] = {
          plume::RenderTextureBarrier(color, plume::RenderTextureLayout::COLOR_WRITE),
          plume::RenderTextureBarrier(entry.texture.get(), plume::RenderTextureLayout::SHADER_READ)};
      commands->barriers(plume::RenderBarrierStage::GRAPHICS, back, 2);
      sets[0]->setTexture(entry.index, entry.texture.get(), plume::RenderTextureLayout::SHADER_READ,
                          entry.view.get());
    }
    BindTarget();
    if (op.clear_flags & 0x100) commands->clearColor(0, op.clear_color, &op.clear_region, 1);
    if (op.clear_flags & 0x200) {
      commands->clearDepthStencil(true, true, std::clamp(op.z, 0.0f, 1.0f), 0, &op.clear_region, 1);
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
      static_cast<plume::RenderWindow>(native_window), kColorFormat,
      REXCVAR_GET(native_interpolate) ? kInterpolationSwapChainBuffers : kSwapChainBuffers));
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
  ScopedMs timer(s.ms_draw);
  s.lap_start = std::chrono::steady_clock::now();
  s.BeginFrame();
  s.UploadDefaults();
  const uint32_t device = draw.device;

  // Shaders.
  const uint32_t vs_object = Be32(base, device + kDeviceVertexShader);
  const uint32_t ps_object = Be32(base, device + kDevicePixelShader);
  const uint64_t vs_key = host::ShadowShaderKey(vs_object, base);
  const uint64_t ps_key = host::ShadowShaderKey(ps_object, base);
  if (ShaderListed(REXCVAR_GET(native_skip_shaders), vs_key, ps_key)) {
    s.Skip("native_skip_shaders");
    return;
  }
  plume::RenderShader* vs = s.Shader(vs_key);
  plume::RenderShader* ps = ps_object ? s.Shader(ps_key) : nullptr;
  if (!vs || (ps_object && !ps)) {
    s.Skip("shader not in cache");
    return;
  }
  if (REXCVAR_GET(native_debug_target) && vs_key == 0x1089333427EFE5F2ull) {
    s.Skip("debug: title composite");
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
  uint32_t index_address = 0;
  if (draw.indexed) {
    const uint32_t ib = Be32(base, device + kDeviceIndices);
    if (ib == 0) {
      s.Skip("index buffer");
      return;
    }
    const bool index32 = (Be32(base, ib) & 0x80000000u) != 0;
    const uint32_t address = PhysicalView(Be32(base, ib + 24));
    index_address = address;
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

  s.Lap(0);
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
  // One spec constant makes the vertex shader unpack NORMAL, TANGENT and
  // BINORMAL alike, so when any of them is packed 11:11:10 the others are
  // repacked that way too.
  if (packed_normals) {
    for (uint32_t i = 0; i < element_count; ++i) {
      if (elems[i].convert == ElementConvert::kFloat4Bits) {
        elems[i].convert = ElementConvert::kPacked101111;
        elems[i].format = plume::RenderFormat::R32_UINT;
        elems[i].out_size = 4;
      }
    }
  }

  std::array<plume::RenderInputElement, 16> elements;
  std::array<plume::RenderInputSlot, 16> slots;
  std::array<plume::RenderVertexBufferView, 16> views;
  uint32_t slot_count = 0;
  std::array<uint32_t, 16> stream_addresses{};
  uint32_t stream_address_count = 0;
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
    if (slot_count == 0) {
      for (const auto part : std::views::split(std::string_view(REXCVAR_GET(native_skip_verts)), ',')) {
        const std::string_view want(part.begin(), part.end());
        if (!want.empty() && want == std::to_string(vertex_count)) {
          s.Skip("native_skip_verts");
          return;
        }
      }
    }
    const uint8_t* guest_stream = At(base, PhysicalView(vf.address * 4 + uint32_t(begin)));
    const size_t out_bytes = size_t(vertex_count) * out_stride;
    XXH3_state_t layout_state;
    XXH3_64bits_reset(&layout_state);
    const uint32_t stream_layout[4] = {stride, out_stride, uint32_t(vf.endian), uint32_t(bytes)};
    XXH3_64bits_update(&layout_state, stream_layout, sizeof(stream_layout));
    for (uint32_t i = 0; i < element_count; ++i) {
      const Element& e = elems[i];
      if (e.stream != stream) continue;
      const uint32_t element_layout[6] = {e.offset, e.format_word, e.out_offset, uint32_t(e.convert),
                                          uint32_t(e.format), e.source.swizzle};
      XXH3_64bits_update(&layout_state, element_layout, sizeof(element_layout));
    }
    const uint64_t stream_key =
        XXH3_64bits_withSeed(guest_stream, size_t(bytes), XXH3_64bits_digest(&layout_state));
    State::CachedStream& cached = s.stream_cache[stream_key];
    cached.last_used = s.presents;
    plume::RenderBufferReference stream_ref;
    if (cached.buffer) {
      ++s.stream_hits;
      stream_ref = cached.buffer->at(0);
    } else {
      ++s.stream_misses;
      std::vector<uint8_t>& raw = s.stream_raw;
      raw.resize(bytes);
      std::memcpy(raw.data(), guest_stream, bytes);
      SwapDwords(raw.data(), raw.size(), uint32_t(vf.endian));
      s.stream_out.assign(out_bytes, 0);
      uint8_t* out = s.stream_out.data();
      for (uint32_t v = 0; v < vertex_count; ++v) {
        const uint8_t* src = raw.data() + size_t(v) * stride;
        uint8_t* dst = out + size_t(v) * out_stride;
        for (uint32_t i = 0; i < element_count; ++i) {
          const Element& e = elems[i];
          if (e.stream != stream) continue;
          if (v == 0) ++s.stream_endians[uint32_t(vf.endian) * 4 + ((e.format_word >> 6) & 3)];
          ConvertElement(src + e.offset, dst + e.out_offset, e.in_size, e.convert, e.source,
                         uint32_t(vf.endian));
        }
      }
      if (++cached.seen >= 2 && out_bytes >= 4096) {
        cached.buffer = s.device->createBuffer(
            plume::RenderBufferDesc::UploadBuffer(out_bytes, plume::RenderBufferFlag::VERTEX));
      }
      if (cached.buffer) {
        void* mapped = cached.buffer->map();
        std::memcpy(mapped, out, out_bytes);
        cached.buffer->unmap();
        stream_ref = cached.buffer->at(0);
      } else {
        uint64_t ring = 0;
        if (!s.Upload(out_bytes, 16, ring)) {
          s.Skip("upload ring full");
          return;
        }
        std::memcpy(s.upload_cpu + ring, out, out_bytes);
        stream_ref = s.upload->at(ring);
      }
    }
    slots[slot_count] = plume::RenderInputSlot(slot_count, out_stride);
    views[slot_count] = plume::RenderVertexBufferView(stream_ref, uint32_t(out_bytes));
    stream_addresses[stream_address_count++] = vf.address;
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

  s.Lap(1);
  // Render state.
  const uint32_t depth_control = Reg(base, device, kRbDepthControl);
  const uint32_t blend_control = Reg(base, device, kRbBlendControl0);
  const uint32_t color_control = Reg(base, device, kRbColorControl);
  uint32_t color_mask = Reg(base, device, kRbColorMask) & 0xF;
  {
    static constexpr uint32_t kTintMasks[] = {0x1, 0x2, 0x4, 0x3};
    uint32_t n = 0;
    for (const auto part : std::views::split(std::string_view(REXCVAR_GET(native_tint_verts)), ',')) {
      const std::string_view want(part.begin(), part.end());
      if (n < 4 && !want.empty() && want == std::to_string(vertex_count) && color_mask != 0) {
        color_mask = kTintMasks[n];
      }
      ++n;
    }
  }
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
  // Culling only applies to triangles; both bits set is treated as none.
  const bool triangles = topology == plume::RenderPrimitiveTopology::TRIANGLE_LIST ||
                         topology == plume::RenderPrimitiveTopology::TRIANGLE_STRIP;
  const uint32_t sc_mode = triangles ? Reg(base, device, kPaSuScModeCntl) & 7 : 0;
  if (triangles) ++s.cull_modes[sc_mode];
  // Anything but no culling or back faces with counter-clockwise front.
  if (REXCVAR_GET(native_dump_outline) && s.outline_dump == 0 && sc_mode != 0 && sc_mode != 2) {
    s.outline_dump = s.presents + 61;
  }
  const uint32_t stencil_ref = (depth_control & 1) ? Reg(base, device, kRbStencilRefMask) & 0xFFFFFF : 0;
  const uint32_t state_key[8] = {(depth_control & 1) ? depth_control : depth_control & 0x77,
                                 blend_control, color_mask, uint32_t(topology), spec, uint32_t(ps_object != 0),
                                 sc_mode, stencil_ref};
  const uint32_t target_samples = State::SurfaceSamples(base, device);
  XXH3_64bits_update(&key_state, &target_samples, sizeof(target_samples));
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
    // PA_SU_SC_MODE_CNTL: bit 0 culls front faces, bit 1 back faces, bit 2
    // makes clockwise triangles the front ones. Characters draw their
    // outline as an inflated shell with front faces culled.
    switch (sc_mode & 3) {
      case 1: desc.cullMode = plume::RenderCullMode::FRONT; break;
      case 2: desc.cullMode = plume::RenderCullMode::BACK; break;
      default: desc.cullMode = plume::RenderCullMode::NONE; break;
    }
    desc.frontFace = (sc_mode & 4) ? plume::RenderFrontFace::CLOCKWISE : plume::RenderFrontFace::COUNTER_CLOCKWISE;
    desc.depthClipEnabled = true;
    desc.depthEnabled = (depth_control & 0x2) != 0 && !REXCVAR_GET(native_debug_target);
    desc.depthWriteEnabled = (depth_control & 0x4) != 0;
    desc.depthFunction = CompareFunction((depth_control >> 4) & 7);
    // Stencil: characters mark their pixels so the outline shell drawn after
    // them only shows outside the body. One reference and read mask serve
    // both faces on the host; Xenos keeps a set per face.
    desc.stencilEnabled = (depth_control & 1) != 0;
    if (desc.stencilEnabled) {
      desc.stencilReference = stencil_ref & 0xFF;
      desc.stencilReadMask = (stencil_ref >> 8) & 0xFF;
      desc.stencilWriteMask = (stencil_ref >> 16) & 0xFF;
      desc.stencilFrontFace = StencilFace(depth_control, 8);
      desc.stencilBackFace = (depth_control & 0x80) ? StencilFace(depth_control, 20) : desc.stencilFrontFace;
    }
    desc.depthTargetFormat = kDepthFormat;
    desc.multisampling = plume::RenderMultisampling(static_cast<plume::RenderSampleCounts>(target_samples));
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
  s.Lap(2);
  CopySwappedDwords(s.upload_cpu + vs_constants, At(base, device + kDeviceVsConstants),
                    gfx::kVertexShaderConstantsBytes);
  CopySwappedDwords(s.upload_cpu + ps_constants, At(base, device + kDevicePsConstants),
                    gfx::kPixelShaderConstantsBytes);

  // Shared constants are built in local memory and copied once.
  alignas(16) uint8_t shared_cpu[gfx::kSharedConstantsBytes] = {};
  auto* indices = reinterpret_cast<uint32_t*>(shared_cpu);
  bool uploaded = false;
  for (uint32_t slot = 0; slot < DESCRIPTOR_SLOT_COUNT; ++slot) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, device + kDeviceFetch + 24 * slot + 4 * w);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words, sizeof(words));
    if (uint32_t(fetch.type) != 2) continue;
    indices[DESCRIPTOR_KIND_SAMPLER * DESCRIPTOR_SLOT_COUNT + slot] = s.SamplerFor(fetch);
    if (uint32_t(fetch.dimension) == 0) {
      indices[DESCRIPTOR_KIND_TEXTURE1D * DESCRIPTOR_SLOT_COUNT + slot] = s.Texture1DFor(words, fetch, base, uploaded);
    } else if (uint32_t(fetch.dimension) == 3) {
      indices[DESCRIPTOR_KIND_TEXTURECUBE * DESCRIPTOR_SLOT_COUNT + slot] = s.TextureCubeFor(words, fetch, base, uploaded);
    } else if (uint32_t(fetch.dimension) == 2) {
      s.Skip("3d texture");
    } else if (uint32_t(fetch.dimension) == 1) {
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
  const uint32_t alpha_func = color_control & 7;
  std::memcpy(shared_cpu + gfx::kSharedAlphaFuncOffset, &alpha_func, 4);
  std::memcpy(s.upload_cpu + shared, shared_cpu, sizeof(shared_cpu));

  s.Lap(3);
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
  const float sx = s.scale_x;
  const float sy = s.scale_y;
  const float xscale = std::fabs(RegFloat(base, device, kPaClVportXScale));
  const float yscale = std::fabs(RegFloat(base, device, kPaClVportYScale));
  const float xoffset = RegFloat(base, device, kPaClVportXOffset);
  const float yoffset = RegFloat(base, device, kPaClVportYOffset);
  const float zscale = RegFloat(base, device, kPaClVportZScale);
  const float zoffset = RegFloat(base, device, kPaClVportZOffset);
  plume::RenderViewport viewport((xoffset - xscale) * sx, (yoffset - yscale) * sy, 2.0f * xscale * sx,
                                 2.0f * yscale * sy, std::clamp(zoffset, 0.0f, 1.0f),
                                 std::clamp(zoffset + zscale, 0.0f, 1.0f));
  s.SelectTarget(base, device, uint32_t(std::ceil(yoffset + yscale)));
  if (!(viewport.width > 0.0f) || !(viewport.height > 0.0f)) {
    viewport = plume::RenderViewport(0.0f, 0.0f, float(s.target_width), float(s.target_height));
  }
  ++s.viewports[fmt::format("vte={:03X} vp={:.0f},{:.0f},{:.0f}x{:.0f} z={:.2f}+{:.2f} surf={:08X} wo={:08X} sc={:08X}-{:08X}",
                              Reg(base, device, 0x2206), xoffset - xscale, yoffset - yscale, 2 * xscale,
                              2 * yscale, zoffset, zscale, Reg(base, device, 0x2000), Reg(base, device, 0x2080),
                              Reg(base, device, 0x200E), Reg(base, device, 0x200F))];
  s.last_viewport = fmt::format("{:.1f},{:.1f} {:.1f}x{:.1f}", xoffset - xscale, yoffset - yscale,
                                2.0f * xscale, 2.0f * yscale);

  s.Lap(4);
  State::DrawOp op;
  op.target = s.current;
  op.pipeline = pipeline;
  op.vs_constants = vs_constants;
  op.ps_constants = ps_constants;
  op.shared = shared;
  op.viewport = viewport;
  op.views = views;
  op.slots = slots;
  op.slot_count = slot_count;
  op.indexed = use_indices;
  op.index_ring = index_ring;
  op.index_count = index_count;
  op.vertex_count = draw.count;
  op.uploaded = uploaded;
  if (REXCVAR_GET(native_interpolate)) {
    uint32_t texture_indices[DESCRIPTOR_SLOT_COUNT];
    for (uint32_t slot = 0; slot < DESCRIPTOR_SLOT_COUNT; ++slot) {
      texture_indices[slot] = indices[DESCRIPTOR_KIND_TEXTURE2D * DESCRIPTOR_SLOT_COUNT + slot];
    }
    op.key = interp::DrawKey(vs_key, ps_key, stream_addresses.data(), stream_address_count,
                             draw.indexed ? index_address : 0, draw.start, draw.count, texture_indices,
                             DESCRIPTOR_SLOT_COUNT);
  }
  s.ExecuteDraw(op);
  s.Record(op);
  s.Lap(5);
  if (s.Dumping()) {
    std::string line = fmt::format("draw {} indexed={} prim={} start={} count={} base={} first={} verts={} "
                                   "vs={:016X} ps={:016X} depth={:08X} blend={:08X} mask={:X} vp={} cull={:08X} "
                                   "clip={:08X} rt={:08X}/{:08X}/{:08X} cc={:08X} aref={:g}",
                                   s.drawn, draw.indexed, draw.prim, draw.start, draw.count, draw.base_vertex,
                                   first, vertex_count, vs_key, ps_key, depth_control, blend_control, color_mask,
                                   s.last_viewport, Reg(base, device, kPaSuScModeCntl), Reg(base, device, 0x2204),
                                   Reg(base, device, 0x2000), Reg(base, device, 0x2001), Reg(base, device, 0x2002),
                                   color_control, alpha_ref);
    for (uint32_t i = 0; i < element_count; ++i) {
      line += fmt::format(" | e{} s{} +{} usage{}.{} type{:08X}", i, elems[i].stream, elems[i].offset,
                          elems[i].usage, elems[i].usage_index, elems[i].format_word);
    }
    if (REXCVAR_GET(native_dump_mesh) > 0 && uint32_t(REXCVAR_GET(native_dump_mesh)) == vertex_count) {
      const std::string path = fmt::format("logs/mesh-{}-{}.csv", s.drawn, vertex_count);
      if (FILE* f = std::fopen(path.c_str(), "w")) {
        for (uint32_t v = 0; v < vertex_count; ++v) {
          std::string row = fmt::format("{}", first + v);
          for (uint32_t i = 0; i < element_count; ++i) {
            const uint32_t fetch_at = device + kDeviceFetch + 8 * (95 - elems[i].stream);
            xenos::xe_gpu_vertex_fetch_t vf;
            vf.dword_0 = Be32(base, fetch_at);
            vf.dword_1 = Be32(base, fetch_at + 4);
            const uint32_t stride = uint32_t(*At(base, device + kDeviceStrides + elems[i].stream)) * 4;
            uint8_t raw[16] = {};
            std::memcpy(raw, At(base, PhysicalView(vf.address * 4 + (first + v) * stride + elems[i].offset)),
                        elems[i].in_size);
            SwapDwords(raw, sizeof(raw), uint32_t(vf.endian));
            if (uint32_t(vf.endian) == 2 && SixteenBitComponents(elems[i].source.format)) {
              for (uint32_t b = 0; b + 3 < sizeof(raw); b += 4) {
                std::swap(raw[b], raw[b + 2]);
                std::swap(raw[b + 1], raw[b + 3]);
              }
            }
            float value[4];
            DecodeElement(elems[i].source, raw, value);
            ApplySwizzle(elems[i].source.swizzle, value);
            row += fmt::format(",u{}.{},{:g},{:g},{:g},{:g}", elems[i].usage, elems[i].usage_index, value[0],
                               value[1], value[2], value[3]);
          }
          std::fputs((row + "\n").c_str(), f);
        }
        std::fclose(f);
      }
    }
    // Decoded positions of the first vertices, and every element of them for
    // the shaders native_dump_constants names, as the vertex shader gets them.
    const bool all_elements = ShaderListed(REXCVAR_GET(native_dump_constants), vs_key, ps_key);
    for (uint32_t i = 0; i < element_count; ++i) {
      if (!all_elements && (elems[i].usage != 0 || elems[i].usage_index != 0)) continue;
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
        // The same 16-bit component order fix as ConvertElement.
        if (uint32_t(vf.endian) == 2 && SixteenBitComponents(elems[i].source.format)) {
          for (uint32_t b = 0; b + 3 < sizeof(raw); b += 4) {
            std::swap(raw[b], raw[b + 2]);
            std::swap(raw[b + 1], raw[b + 3]);
          }
        }
        float pos[4];
        DecodeElement(elems[i].source, raw, pos);
        if (all_elements) {
          ApplySwizzle(elems[i].source.swizzle, pos);
          line += fmt::format(" | v{}.u{}.{} ({:.3f},{:.3f},{:.3f},{:.3f})", first + v, elems[i].usage,
                              elems[i].usage_index, pos[0], pos[1], pos[2], pos[3]);
        } else {
          line += fmt::format(" | v{} ({:.2f},{:.2f},{:.2f},{:.2f})", first + v, pos[0], pos[1], pos[2], pos[3]);
        }
      }
    }
    const float* c = reinterpret_cast<const float*>(s.upload_cpu + vs_constants);
    for (uint32_t r = 0; r < 8; ++r) {
      line += fmt::format(" | c{} ({:g},{:g},{:g},{:g})", r, c[4 * r], c[4 * r + 1], c[4 * r + 2],
                          c[4 * r + 3]);
    }
    if (ShaderListed(REXCVAR_GET(native_dump_constants), vs_key, ps_key)) {
      const float* ps_c = reinterpret_cast<const float*>(s.upload_cpu + ps_constants);
      for (uint32_t r = 8; r < gfx::kVertexShaderConstantsBytes / 16; ++r) {
        if (c[4 * r] == 0 && c[4 * r + 1] == 0 && c[4 * r + 2] == 0 && c[4 * r + 3] == 0) continue;
        line += fmt::format(" | c{} ({:g},{:g},{:g},{:g})", r, c[4 * r], c[4 * r + 1], c[4 * r + 2], c[4 * r + 3]);
      }
      for (uint32_t r = 0; r < gfx::kPixelShaderConstantsBytes / 16; ++r) {
        const float* v = ps_c + 4 * r;
        if (v[0] == 0 && v[1] == 0 && v[2] == 0 && v[3] == 0) continue;
        line += fmt::format(" | p{} ({:g},{:g},{:g},{:g})", r, v[0], v[1], v[2], v[3]);
      }
    }
    for (uint32_t slot = 0; slot < DESCRIPTOR_SLOT_COUNT; ++slot) {
      if (const uint32_t index1d = indices[DESCRIPTOR_KIND_TEXTURE1D * DESCRIPTOR_SLOT_COUNT + slot]) {
        uint32_t fw[6];
        for (uint32_t w = 0; w < 6; ++w) fw[w] = Be32(base, device + kDeviceFetch + 24 * slot + 4 * w);
        xenos::xe_gpu_texture_fetch_t ff;
        std::memcpy(&ff, fw, sizeof(fw));
        line += fmt::format(" | t{}=1d#{}@{:08X}/f{}/w{} pitch{} [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", slot,
                            index1d, ff.base_address << 12, uint32_t(ff.format), ff.size_1d.width + 1,
                            ff.pitch * 32, fw[0], fw[1], fw[2], fw[3], fw[4], fw[5]);
      }
      const uint32_t index = indices[DESCRIPTOR_KIND_TEXTURE2D * DESCRIPTOR_SLOT_COUNT + slot];
      if (index == 0) continue;
      bool resolved = false;
      for (const auto& [k, t] : s.textures) resolved |= t.index == index && t.resolved;
      uint32_t fw[6];
      for (uint32_t w = 0; w < 6; ++w) fw[w] = Be32(base, device + kDeviceFetch + 24 * slot + 4 * w);
      xenos::xe_gpu_texture_fetch_t ff;
      std::memcpy(&ff, fw, sizeof(fw));
      line += fmt::format(" | t{}=#{}{}@{:08X}/f{}/{}x{} [{:08X} {:08X} {:08X} {:08X} {:08X} {:08X}]", slot, index,
                          resolved ? "r" : "", ff.base_address << 12, uint32_t(ff.format), ff.size_2d.width + 1,
                          ff.size_2d.height + 1, fw[0], fw[1], fw[2], fw[3], fw[4], fw[5]);
    }
    s.dump += line + "\n";
  }
  s.Lap(6);
  ++s.drawn;
}

void Renderer::Clear(uint32_t device, uint32_t flags, uint32_t rect, uint32_t color, float z,
                     uint32_t stencil, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  ScopedMs timer(s.ms_clear);
  s.BeginFrame();
  s.UploadDefaults();
  s.SelectTarget(base, device, std::max<uint32_t>(State::RectBottom(base, rect), 1));
  if (REXCVAR_GET(native_debug_target)) return;
  plume::RenderRect host_rect(0, 0, int32_t(s.target_width), int32_t(s.target_height));
  if (rect != 0) {
    host_rect = s.HostRect(base, rect);
    // In a tiled pass the clear is replayed for every band with that band's
    // offset, but the title passes the first band's rect. A full-width rect
    // from the top therefore stands for the whole surface.
    if (host_rect.left == 0 && host_rect.top == 0 && host_rect.right >= int32_t(s.target_width)) {
      host_rect.bottom = int32_t(s.target_height);
    }
  }
  State::ClearOp op;
  op.target = s.current;
  op.flags = flags;
  op.rect = host_rect;
  op.color = plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f);
  if ((flags & 0xF) && color != 0) {
    op.color = plume::RenderColor(BeFloat(base, color), BeFloat(base, color + 4), BeFloat(base, color + 8),
                                  BeFloat(base, color + 12));
  }
  op.z = z;
  op.stencil = stencil;
  s.ExecuteClear(op);
  s.Record(op);
  if (s.Dumping()) {
    s.dump += fmt::format("clear flags={:X} rect={:08X} z={} rt={:08X}/{:08X}/{:08X}\n", flags, rect, z,
                          Reg(base, device, 0x2000), Reg(base, device, 0x2001), Reg(base, device, 0x2002));
  }
  ++s.clears;
}

void Renderer::Resolve(uint32_t device, uint32_t flags, uint32_t rect, uint32_t texture, uint32_t point,
                       uint32_t clear_color, float z, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  ScopedMs timer(s.ms_resolve);
  s.BeginFrame();
  s.UploadDefaults();
  s.SelectTarget(base, device, std::max<uint32_t>(State::RectBottom(base, rect), 1));
  const uint32_t source = flags & 7;
  State::ResolveOp op;
  op.target = s.current;
  if (texture != 0 && source >= 4) {
    // The EDRAM depth into a texture the game samples later (the cutscene
    // depth of field). The host copy holds the depth value as R32_FLOAT.
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, texture + 28 + 4 * w);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words, sizeof(words));
    const uint32_t guest_width = fetch.size_2d.width + 1;
    const uint32_t guest_height = fetch.size_2d.height + 1;
    static bool logged = false;
    if (!logged) {
      logged = true;
      REXLOG_INFO("native: first depth resolve: flags={:X} format={} {}x{} swizzle={:03o} depth_info={:08X} samples={}",
                  flags, uint32_t(fetch.format), guest_width, guest_height, uint32_t(fetch.swizzle),
                  Reg(base, device, 0x2002), s.current_samples);
    }
    const uint32_t width = std::max(1u, uint32_t(std::lround(guest_width * s.scale_x)));
    const uint32_t height = std::max(1u, uint32_t(std::lround(guest_height * s.scale_y)));
    State::Image* depth_image = s.current ? s.current->depth : nullptr;
    const uint64_t key = ImageKey(words);
    State::Texture& entry = s.textures[key];
    bool ok = depth_image && s.depth && s.depth_resolve_pipeline;
    if (ok && (!entry.texture || !entry.resolved || entry.format != kDepthCopyFormat || entry.width != width ||
               entry.height != height)) {
      s.Retire(entry.framebuffer);
      s.Retire(entry.view);
      s.Retire(entry.texture);
      entry.texture = s.device->createTexture(plume::RenderTextureDesc::ColorTarget(width, height, kDepthCopyFormat));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(kDepthCopyFormat);
      view.componentMapping = ComponentMapping(DepthSwizzle(fetch.swizzle));
      entry.view = entry.texture->createTextureView(view);
      const plume::RenderTexture* attachment = entry.texture.get();
      plume::RenderFramebufferDesc framebuffer;
      framebuffer.colorAttachments = &attachment;
      framebuffer.colorAttachmentsCount = 1;
      entry.framebuffer = s.device->createFramebuffer(framebuffer);
      if (entry.index == 0) {
        if (s.next_texture2d >= kTexture2DCount) return;
        entry.index = s.next_texture2d++;
      }
      entry.resolved = true;
      entry.format = kDepthCopyFormat;
      entry.width = width;
      entry.height = height;
    }
    if (ok && (!depth_image->depth_view || depth_image->depth_view_of != s.depth)) {
      s.Retire(depth_image->depth_view);
      depth_image->depth_view = s.depth->createTextureView(plume::RenderTextureViewDesc::Texture2D(kDepthFormat));
      depth_image->depth_view_of = s.depth;
      if (depth_image->depth_index == 0) {
        if (s.next_texture2d >= kTexture2DCount) return;
        depth_image->depth_index = s.next_texture2d++;
      }
    }
    plume::RenderRect src(0, 0, int32_t(s.target_width), int32_t(s.target_height));
    if (rect != 0) src = s.HostRect(base, rect);
    int32_t dst_x = 0, dst_y = 0;
    if (point != 0) {
      dst_x = int32_t(std::lround(int32_t(Be32(base, point)) * s.scale_x));
      dst_y = int32_t(std::lround(int32_t(Be32(base, point + 4)) * s.scale_y));
    }
    const int32_t copy_w = std::min<int32_t>(src.right - src.left, int32_t(width) - dst_x);
    const int32_t copy_h = std::min<int32_t>(src.bottom - src.top, int32_t(height) - dst_y);
    uint64_t constants = 0;
    ok = ok && copy_w > 0 && copy_h > 0 && s.Upload(16, 16, constants);
    if (ok) {
      const int32_t header[4] = {int32_t(depth_image->depth_index), s.current_samples > 1 ? 1 : 0,
                                 src.left - dst_x, src.top - dst_y};
      std::memcpy(s.upload_cpu + constants, header, sizeof(header));
      op.kind = State::ResolveOp::kDepth;
      op.key = key;
      op.dst_x = dst_x;
      op.dst_y = dst_y;
      op.copy_w = copy_w;
      op.copy_h = copy_h;
      op.constants = constants;
      const uint32_t physical = TexturePhysical(fetch.base_address);
      s.resolved_at[physical] = {key, State::GuestPageHash(base, physical)};
      ++s.depth_resolves;
    } else {
      s.Skip("depth resolve");
    }
    if (s.Dumping()) {
      s.dump += fmt::format("resolve depth flags={:X} texture={:08X} @{:08X}/f{} {}x{} key={:016X} index=#{} "
                            "src={},{}-{},{} {} rt={:08X}/{:08X}/{:08X}\n",
                            flags, texture, fetch.base_address << 12, uint32_t(fetch.format), guest_width,
                            guest_height, key, entry.index, src.left, src.top, src.right, src.bottom,
                            ok ? "done" : "skipped", Reg(base, device, 0x2000), Reg(base, device, 0x2001),
                            Reg(base, device, 0x2002));
    }
  }
  if (texture != 0 && source < 4) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, texture + 28 + 4 * w);
    xenos::xe_gpu_texture_fetch_t fetch;
    std::memcpy(&fetch, words, sizeof(words));
    const uint32_t guest_width = fetch.size_2d.width + 1;
    const uint32_t guest_height = fetch.size_2d.height + 1;
    const uint32_t width = std::max(1u, uint32_t(std::lround(guest_width * s.scale_x)));
    const uint32_t height = std::max(1u, uint32_t(std::lround(guest_height * s.scale_y)));
    const uint64_t key = ImageKey(words);
    const uint32_t physical = TexturePhysical(fetch.base_address);
    s.resolved_at[physical] = {key, State::GuestPageHash(base, physical)};
    State::Texture& entry = s.textures[key];
    if (!entry.texture || !entry.resolved || entry.format != kColorFormat || entry.width != width ||
        entry.height != height) {
      s.Retire(entry.framebuffer);
      s.Retire(entry.view);
      s.Retire(entry.texture);
      entry.texture = s.device->createTexture(plume::RenderTextureDesc::Texture2D(width, height, 1, kColorFormat));
      plume::RenderTextureViewDesc view = plume::RenderTextureViewDesc::Texture2D(kColorFormat);
      view.componentMapping = ComponentMapping(ResolvedSwizzle(fetch.swizzle, fetch.format));
      entry.view = entry.texture->createTextureView(view);
      if (entry.index == 0) {
        if (s.next_texture2d >= kTexture2DCount) return;
        entry.index = s.next_texture2d++;
      }
      entry.resolved = true;
      entry.format = kColorFormat;
      entry.width = width;
      entry.height = height;
    }
    plume::RenderRect src(0, 0, int32_t(s.target_width), int32_t(s.target_height));
    if (rect != 0) src = s.HostRect(base, rect);
    int32_t dst_x = 0, dst_y = 0;
    if (point != 0) {
      dst_x = int32_t(std::lround(int32_t(Be32(base, point)) * s.scale_x));
      dst_y = int32_t(std::lround(int32_t(Be32(base, point + 4)) * s.scale_y));
    }
    src.right = std::min<int32_t>(src.right, src.left + int32_t(width) - dst_x);
    src.bottom = std::min<int32_t>(src.bottom, src.top + int32_t(height) - dst_y);
    if (src.right > src.left && src.bottom > src.top) {
      op.kind = State::ResolveOp::kColor;
      op.key = key;
      op.src = src;
      op.dst_x = dst_x;
      op.dst_y = dst_y;
    }
    if (s.Dumping()) {
      s.dump += fmt::format("resolve flags={:X} texture={:08X} @{:08X}/f{} {}x{} key={:016X} index=#{} "
                            "src={},{}-{},{} rt={:08X}/{:08X}/{:08X}\n",
                            flags, texture, fetch.base_address << 12, uint32_t(fetch.format), guest_width,
                            guest_height, key, entry.index, src.left, src.top, src.right, src.bottom,
                            Reg(base, device, 0x2000), Reg(base, device, 0x2001), Reg(base, device, 0x2002));
    }
    ++s.resolves;
  }
  // Xenos clears the EDRAM the resolve read. A tiled pass resolves and clears
  // one band at a time; the host draws the frame once, so only that band may
  // be cleared or the later bands would copy an empty target.
  plume::RenderRect region(0, 0, int32_t(s.target_width), int32_t(s.target_height));
  if (rect != 0) region = s.HostRect(base, rect);
  // A tiled pass resolves each band with a clear, but on the GPU that clear
  // runs per tile inside the replayed command buffer, before the next band is
  // drawn. The title then copies the whole target again (flags 0) and
  // composites that copy, so the host, which draws the frame once, must not
  // clear here.
  if (rect != 0 && (region.right - region.left < int32_t(s.target_width) ||
                    region.bottom - region.top < int32_t(s.target_height))) {
    flags &= ~0x300u;
  }
  if (REXCVAR_GET(native_debug_target)) flags &= ~0x300u;
  op.clear_flags = flags & 0x300u;
  op.clear_region = region;
  op.clear_color = plume::RenderColor(0.0f, 0.0f, 0.0f, 0.0f);
  if ((flags & 0x100) && clear_color != 0) {
    op.clear_color = plume::RenderColor(BeFloat(base, clear_color), BeFloat(base, clear_color + 4),
                                        BeFloat(base, clear_color + 8), BeFloat(base, clear_color + 12));
  }
  op.z = z;
  s.ExecuteResolve(op);
  s.Record(op);
}

void Renderer::Present(uint32_t frontbuffer, const uint8_t* base) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  ScopedMs timer(s.ms_present);
  s.BeginFrame();
  s.UploadDefaults();
  // Swap shows a texture: the last resolve into it when there was one, else
  // whatever the target holds.
  if (s.current) s.ResolveMsaa(*s.current->color);
  plume::RenderTexture* source = s.current ? s.current->color->texture.get() : s.color;
  plume::RenderTextureLayout source_layout = plume::RenderTextureLayout::COLOR_WRITE;
  if (source && (s.target_width != s.width || s.target_height != s.height)) source = nullptr;
  if (frontbuffer != 0 && !REXCVAR_GET(native_debug_target)) {
    uint32_t words[6];
    for (uint32_t w = 0; w < 6; ++w) words[w] = Be32(base, frontbuffer + 28 + 4 * w);
    auto found = s.textures.find(ImageKey(words));
    if (found != s.textures.end() && found->second.resolved &&
        (s.present_pipeline || (found->second.width == s.width && found->second.height == s.height))) {
      source = found->second.texture.get();
      source_layout = plume::RenderTextureLayout::SHADER_READ;
    }
  }
  uint32_t image = 0;
  const bool resize = s.swap_chain->needsResize();
  const bool acquired = !resize && !s.swap_chain->isEmpty() &&
                        s.swap_chain->acquireTexture(s.acquire.get(), &image);
  plume::RenderTexture* target = acquired ? s.swap_chain->getTexture(image) : nullptr;
  uint32_t source_index = 0;
  if (source && source_layout == plume::RenderTextureLayout::SHADER_READ) {
    for (const auto& [key, texture] : s.textures) {
      if (texture.texture.get() == source) source_index = texture.index;
    }
  }
  uint64_t present_constants = 0;
  const bool gamma = target && source_index != 0 && s.present_pipeline &&
                     s.Upload(16 + 256 * 4, 256, present_constants);
  if (target && !source) {
    // Nothing the size of the window to show yet.
    s.commands->barriers(plume::RenderBarrierStage::COPY,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
  } else if (gamma) {
    // Draw the frontbuffer through the gamma ramp, as the display controller
    // shows it.
    const uint32_t header[4] = {source_index, s.PresentSampler(), 0, 0};
    std::memcpy(s.upload_cpu + present_constants, header, sizeof(header));
    if (s.has_gamma_ramp) {
      std::memcpy(s.upload_cpu + present_constants + 16, s.gamma_ramp.data(), 256 * 4);
    } else {
      // No ramp written yet: an identity table.
      std::array<uint32_t, 256> identity;
      for (uint32_t i = 0; i < 256; ++i) {
        const uint32_t v = (i * 1023 + 127) / 255;
        identity[i] = v | (v << 10) | (v << 20);
      }
      std::memcpy(s.upload_cpu + present_constants + 16, identity.data(), 256 * 4);
    }
    s.commands->barriers(plume::RenderBarrierStage::GRAPHICS,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::COLOR_WRITE));
    s.commands->setFramebuffer(s.SwapFramebuffer(image));
    s.commands->setGraphicsPipelineLayout(s.layout.get());
    for (uint32_t i = 0; i < 6; ++i) s.commands->setGraphicsDescriptorSet(s.sets[i].get(), i);
    s.commands->setPipeline(s.present_pipeline.get());
    const uint64_t push[3] = {s.upload_gpu + present_constants, 0, 0};
    s.commands->setGraphicsPushConstants(0, push, 0, sizeof(push));
    const plume::RenderViewport viewport(0.0f, 0.0f, float(s.width), float(s.height));
    s.commands->setViewports(&viewport, 1);
    const plume::RenderRect scissor(0, 0, int32_t(s.width), int32_t(s.height));
    s.commands->setScissors(&scissor, 1);
    s.commands->drawInstanced(3, 1, 0, 0);
    s.commands->barriers(plume::RenderBarrierStage::NONE,
                         plume::RenderTextureBarrier(target, plume::RenderTextureLayout::PRESENT));
    s.current = nullptr;
  } else if (target) {
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
  s.journal_last = s.journal.size();
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
  s.DestroyRetired();
  // The GPU is idle now, so streams unused for a while can go.
  if ((s.presents & 63) == 0) {
    std::erase_if(s.stream_cache, [&](const auto& entry) { return s.presents - entry.second.last_used > 120; });
  }
  s.ms_submit +=std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t_submit).count();
  if (s.Dumping()) {
    const std::string path = fmt::format("logs/native-frame-{}.txt", s.presents + 1);
    if (FILE* f = std::fopen(path.c_str(), "w")) {
      std::fputs(s.dump.c_str(), f);
      std::fprintf(f, "present frontbuffer=%08X target=%ux%u\n", frontbuffer, s.width, s.height);
      for (const auto& [physical, at] : s.resolved_at) {
        auto found = s.textures.find(at.key);
        if (found == s.textures.end()) continue;
        std::fprintf(f, "resolved_at %08X key=%016llX index=#%u %ux%u\n", physical,
                     static_cast<unsigned long long>(at.key), found->second.index, found->second.width,
                     found->second.height);
      }
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
      s.swap_framebuffers.clear();
      s.CreateTarget();
    }
  }
}

void Renderer::SetFlipInterval(uint32_t interval) {
  state_->flip_interval.store(interval, std::memory_order_relaxed);
}

void Renderer::SetGammaRamp(const uint32_t table[256]) {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  std::memcpy(s.gamma_ramp.data(), table, 256 * 4);
  s.has_gamma_ramp = false;
  for (const uint32_t entry : s.gamma_ramp) s.has_gamma_ramp |= entry != 0;
}

std::string Renderer::TakeStats() {
  State& s = *state_;
  std::lock_guard<std::mutex> lock(s.mutex);
  std::string out = fmt::format(
      "drawn={} clears={} resolves={} reuploads={} targets={} switches={} pipelines={} shaders={} textures={} samplers={} ms pipeline={:.0f} texture={:.0f} "
      "stream={:.0f} gpu_wait={:.0f} | draw={:.0f} clear={:.0f} resolve={:.0f} present={:.0f} viewport={}",
      s.drawn, s.clears, s.resolves, s.reuploads, s.color_images.size(), s.target_switches, s.pipelines.size(), s.shaders.size(), s.textures.size(), s.samplers.size(),
      s.ms_pipeline, s.ms_texture, s.ms_stream, s.ms_submit, s.ms_draw, s.ms_clear, s.ms_resolve,
      s.ms_present, s.last_viewport);
  s.ms_pipeline = s.ms_texture = s.ms_stream = s.ms_submit = 0;
  s.ms_draw = s.ms_clear = s.ms_resolve = s.ms_present = 0;
  out += fmt::format(" streams hit={} miss={} cached={} msaa_resolves={} depth_resolves={}", s.stream_hits,
                     s.stream_misses, s.stream_cache.size(), s.msaa_resolves, s.depth_resolves);
  s.msaa_resolves = 0;
  s.depth_resolves = 0;
  if (REXCVAR_GET(native_interpolate)) out += fmt::format(" journal={}", s.journal_last);
  s.stream_hits = s.stream_misses = 0;
  out += " laps";
  for (const double ms : s.ms_lap) out += fmt::format(" {:.0f}", ms);
  s.ms_lap = {};
  out += " cull";
  for (uint32_t m = 0; m < 8; ++m) {
    if (s.cull_modes[m]) out += fmt::format(" {}:{}", m, s.cull_modes[m]);
  }
  s.cull_modes = {};
  out += " endian(fetch/elem)";
  for (uint32_t m = 0; m < 16; ++m) {
    if (s.stream_endians[m]) out += fmt::format(" {}/{}:{}", m / 4, m % 4, s.stream_endians[m]);
  }
  s.stream_endians = {};
  if (s.outline_dump) out += fmt::format(" outline_dump={}", s.outline_dump);
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
  s.target_switches = 0;
  s.skipped.clear();
  return out;
}

}  // namespace broken_bond::render
