// Contract between Broken Bond's translated shaders and the host renderer.
//
// XenosRecomp pastes this file at the top of every HLSL shader it writes
// (third argument of the tool), and the host includes it from C++ for the
// layout constants below. Adapted from XenosRecomp's shader_common.h:
//
//   MIT License. Copyright (c) 2025 hedge-dev and contributors.
//   Permission is hereby granted, free of charge, to any person obtaining a copy
//   of this software and associated documentation files (the "Software"), to deal
//   in the Software without restriction, including without limitation the rights
//   to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
//   copies of the Software, and to permit persons to whom the Software is
//   furnished to do so, subject to the following conditions: the above copyright
//   notice and this permission notice shall be included in all copies or
//   substantial portions of the Software. THE SOFTWARE IS PROVIDED "AS IS",
//   WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED.
//
// Differences from upstream, all made for this game:
// - Texture and sampler descriptor indices are kept per texture fetch constant
//   slot (0..31), not per constant-table name. 1312 of BB's 8156 shaders fetch
//   from a slot their constant table does not name.
// - 1D textures exist: 271 vertex shaders fetch a 1D texture from slot 16.
// - Shared constants start after the 160 descriptor indices (640 bytes).

#ifndef XENOS_SHADER_COMMON_H_INCLUDED
#define XENOS_SHADER_COMMON_H_INCLUDED

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
// Not a specialization constant: XenosRecomp sets it in the cache entry of a
// vertex shader whose fetches take a computed index. Such a shader has no
// vertex inputs and reads the fetch table (kSharedFetchTableOffset).
#define SPEC_CONSTANT_INDEXED_FETCH     (1 << 8)
// Not specialization constants either: a vertex shader whose constant table
// names these registers (frame interpolation blends them). Bits 9-14 in
// this order are interp::LerpMotion's `motion` mask.
#define SPEC_CONSTANT_SKIN_MATRICES     (1 << 9)  // g_aSkinMatrices c42-c137
#define SPEC_CONSTANT_WORLD_VIEW_PROJ   (1 << 10) // g_mWorldViewProj c0-c3
#define SPEC_CONSTANT_WORLD_VIEW        (1 << 11) // g_mWorldView c4-c6
#define SPEC_CONSTANT_INV_TRANS_WORLD   (1 << 12) // g_mInvTransWorld c8-c11
#define SPEC_CONSTANT_WORLD             (1 << 13) // g_mWorld c12-c14
#define SPEC_CONSTANT_VIEWER_POSITION   (1 << 14) // g_vViewerPosition c21
#define SPEC_CONSTANT_MOTION_SHIFT      9

// Descriptor kinds; index = kind * 32 + fetch slot.
#define DESCRIPTOR_KIND_TEXTURE1D   0
#define DESCRIPTOR_KIND_TEXTURE2D   1
#define DESCRIPTOR_KIND_TEXTURE3D   2
#define DESCRIPTOR_KIND_TEXTURECUBE 3
#define DESCRIPTOR_KIND_SAMPLER     4
#define DESCRIPTOR_SLOT_COUNT       32

#if defined(__cplusplus) && !defined(__INTELLISENSE__)

#include <cstdint>

namespace broken_bond::gfx {

// Shared constant buffer: uint descriptor_index[5][32], then these fields.
inline constexpr uint32_t kDescriptorIndexBytes = 5 * DESCRIPTOR_SLOT_COUNT * 4;
inline constexpr uint32_t kSharedBooleansOffset = kDescriptorIndexBytes + 0;
inline constexpr uint32_t kSharedSwappedTexcoordsOffset = kDescriptorIndexBytes + 4;
inline constexpr uint32_t kSharedHalfPixelOffsetOffset = kDescriptorIndexBytes + 8;
inline constexpr uint32_t kSharedAlphaThresholdOffset = kDescriptorIndexBytes + 16;
// RB_COLORCONTROL alpha function: 0 never ... 7 always, as D3DCMP_* minus one.
inline constexpr uint32_t kSharedAlphaFuncOffset = kDescriptorIndexBytes + 20;
// GPU address of the fetch table, for SPEC_CONSTANT_INDEXED_FETCH shaders:
// uint vertex_base (added to the host vertex index to give the guest one),
// three pad words, then per vertex location {uint64 address, uint count,
// uint pad} of `count` decoded float4s.
inline constexpr uint32_t kSharedFetchTableOffset = kDescriptorIndexBytes + 24;
inline constexpr uint32_t kFetchTableBytes = 16 + 16 * 16;
inline constexpr uint32_t kSharedConstantsBytes = kDescriptorIndexBytes + 32;

inline constexpr uint32_t kVertexShaderConstantsBytes = 256 * 16;
inline constexpr uint32_t kPixelShaderConstantsBytes = 224 * 16;

// D3DDECLUSAGE values as the shader container stores them.
enum class DeclUsage : uint32_t {
  kPosition = 0,
  kBlendWeight = 1,
  kBlendIndices = 2,
  kNormal = 3,
  kTexCoord = 5,
  kTangent = 6,
  kBinormal = 7,
  kColor = 10,
};

struct VertexLocation {
  DeclUsage usage;
  uint32_t usage_index;
  uint32_t location;
};

// Must match USAGE_LOCATIONS in patches/xenosrecomp (shader_recompiler.cpp).
inline constexpr VertexLocation kVertexLocations[] = {
    {DeclUsage::kPosition, 0, 0},     {DeclUsage::kNormal, 0, 1},
    {DeclUsage::kTangent, 0, 2},      {DeclUsage::kBinormal, 0, 3},
    {DeclUsage::kTexCoord, 0, 4},     {DeclUsage::kTexCoord, 1, 5},
    {DeclUsage::kTexCoord, 2, 6},     {DeclUsage::kTexCoord, 3, 7},
    {DeclUsage::kTexCoord, 4, 8},     {DeclUsage::kColor, 0, 9},
    {DeclUsage::kColor, 1, 10},       {DeclUsage::kColor, 2, 11},
    {DeclUsage::kColor, 3, 12},       {DeclUsage::kBlendIndices, 0, 13},
    {DeclUsage::kBlendWeight, 0, 14}, {DeclUsage::kPosition, 1, 15},
};

}  // namespace broken_bond::gfx

#else

#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#define DESCRIPTOR_INDEX(KIND, SLOT) vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + ((KIND) * DESCRIPTOR_SLOT_COUNT + (SLOT)) * 4)

#define g_Booleans                 vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 640)
#define g_SwappedTexcoords         vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 644)
#define g_HalfPixelOffset          vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 648)
#define g_AlphaThreshold           vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 656)
#define g_AlphaFunc                vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 660)
#define g_FetchTable               vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 664, 8)
#define g_FetchVertexBase          vk::RawBufferLoad<uint>(g_FetchTable)

// Vertex fetch by a computed index: element `index` of the decoded stream
// bound to `location`, clamped to the stream. Xenos truncates the index
// unless the fetch asks for rounding.
float4 vfetchIndexed(uint location, float index, bool rounded)
{
    uint64_t entry = g_FetchTable + 16 + location * 16;
    uint count = vk::RawBufferLoad<uint>(entry + 8);
    if (count == 0)
        return float4(0.0, 0.0, 0.0, 1.0);
    float i = clamp(floor(rounded ? index + 0.5 : index), 0.0, float(count - 1));
    return vk::RawBufferLoad<float4>(vk::RawBufferLoad<uint64_t>(entry, 8) + uint(i) * 16, 16);
}

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#else

#define DEFINE_DESCRIPTOR_INDICES() \
    uint4 g_DescriptorIndices[40] : packoffset(c0);

#define DESCRIPTOR_INDEX(KIND, SLOT) g_DescriptorIndices[((KIND) * DESCRIPTOR_SLOT_COUNT + (SLOT)) / 4][((KIND) * DESCRIPTOR_SLOT_COUNT + (SLOT)) % 4]

#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c40.x); \
    uint g_SwappedTexcoords : packoffset(c40.y); \
    float2 g_HalfPixelOffset : packoffset(c40.z); \
    float g_AlphaThreshold : packoffset(c41.x); \
    uint g_AlphaFunc : packoffset(c41.y);

uint g_SpecConstants();

// The fetch table is SPIR-V only.
#define g_FetchVertexBase 0u
float4 vfetchIndexed(uint location, float index, bool rounded)
{
    return float4(0.0, 0.0, 0.0, 1.0);
}

#endif

#define TEXTURE1D_INDEX(SLOT)   DESCRIPTOR_INDEX(DESCRIPTOR_KIND_TEXTURE1D, SLOT)
#define TEXTURE2D_INDEX(SLOT)   DESCRIPTOR_INDEX(DESCRIPTOR_KIND_TEXTURE2D, SLOT)
#define TEXTURE3D_INDEX(SLOT)   DESCRIPTOR_INDEX(DESCRIPTOR_KIND_TEXTURE3D, SLOT)
#define TEXTURECube_INDEX(SLOT) DESCRIPTOR_INDEX(DESCRIPTOR_KIND_TEXTURECUBE, SLOT)
#define SAMPLER_INDEX(SLOT)     DESCRIPTOR_INDEX(DESCRIPTOR_KIND_SAMPLER, SLOT)

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
Texture1D<float4> g_Texture1DDescriptorHeap[] : register(t0, space5);

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

// getGradients: the coarse screen-space derivatives of two values,
// (ddx(v.x), ddy(v.x), ddx(v.y), ddy(v.y)).
float4 gradients(float2 v)
{
    return float4(ddx_coarse(v.x), ddy_coarse(v.x), ddx_coarse(v.y), ddy_coarse(v.y));
}

// Xenos alpha test: keep the pixel when alpha compares true against the
// reference with RB_COLORCONTROL's function (bit 0 less, bit 1 equal, bit 2
// greater). A macro, since the DXIL constant buffer is declared after this
// header.
#define alphaTest(ALPHA) \
    if (!((((g_AlphaFunc) & 1) != 0 && (ALPHA) < g_AlphaThreshold) || \
          (((g_AlphaFunc) & 2) != 0 && (ALPHA) == g_AlphaThreshold) || \
          (((g_AlphaFunc) & 4) != 0 && (ALPHA) > g_AlphaThreshold))) \
        discard

float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float texCoord, float offset)
{
    Texture1D<float4> texture = g_Texture1DDescriptorHeap[resourceDescriptorIndex];
    uint width;
    texture.GetDimensions(width);
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / width, 0);
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture));
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
}

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
        return asfloat(value);
    }
}

float4 tfetchTexcoord(uint swappedTexcoords, float4 value, uint semanticIndex)
{
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 cube(float4 value, inout CubeMapData cubeMapData)
{
    uint index = cubeMapData.cubeMapIndex;
    cubeMapData.cubeMapDirections[index] = value.xyz;
    ++cubeMapData.cubeMapIndex;

    return float4(0.0, 0.0, 0.0, index);
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

float computeMipLevel(float2 pixelCoord)
{
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#endif

#endif
