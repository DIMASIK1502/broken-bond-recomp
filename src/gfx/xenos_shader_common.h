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

// Descriptor kinds; index = kind * 32 + fetch slot.
#define DESCRIPTOR_KIND_TEXTURE1D   0
#define DESCRIPTOR_KIND_TEXTURE2D   1
#define DESCRIPTOR_KIND_TEXTURE3D   2
#define DESCRIPTOR_KIND_TEXTURECUBE 3
#define DESCRIPTOR_KIND_SAMPLER     4
#define DESCRIPTOR_SLOT_COUNT       32

// WebGPU sampled-texture budget on this machine's adapter is 48 per stage,
// and the sampler budget is 16. The heap is separate bindings (core WebGPU
// has no binding arrays; plume binds element i at binding + i). A shader
// only keeps the kinds it fetches, so the 2D heap can use most of the budget.
#define BROKEN_BOND_WEB_TEXTURE2D   32
#define BROKEN_BOND_WEB_TEXTURE3D   4
#define BROKEN_BOND_WEB_TEXTURECUBE 8
#define BROKEN_BOND_WEB_TEXTURE1D   16
#define BROKEN_BOND_WEB_SAMPLERS    16
#define BROKEN_BOND_WEB_TEXTURE2D_BINDING   0
#define BROKEN_BOND_WEB_TEXTURE3D_BINDING   32
#define BROKEN_BOND_WEB_TEXTURECUBE_BINDING 36
#define BROKEN_BOND_WEB_TEXTURE1D_BINDING   44

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
// Web shaders read the specialization bits from here. naga rejects the
// OpSpecConstantOp that a vk::constant_id comparison lowers to, and these
// four bytes are padding in the 672-byte block the PC shaders already upload.
inline constexpr uint32_t kSharedSpecConstantsOffset = kDescriptorIndexBytes + 24;
inline constexpr uint32_t kSharedConstantsBytes = kDescriptorIndexBytes + 32;

inline constexpr uint32_t kWebTexture2DCount = BROKEN_BOND_WEB_TEXTURE2D;
inline constexpr uint32_t kWebTexture3DCount = BROKEN_BOND_WEB_TEXTURE3D;
inline constexpr uint32_t kWebTextureCubeCount = BROKEN_BOND_WEB_TEXTURECUBE;
inline constexpr uint32_t kWebTexture1DCount = BROKEN_BOND_WEB_TEXTURE1D;
inline constexpr uint32_t kWebSamplerCount = BROKEN_BOND_WEB_SAMPLERS;
inline constexpr uint32_t kWebTexture2DBinding = BROKEN_BOND_WEB_TEXTURE2D_BINDING;
inline constexpr uint32_t kWebTexture3DBinding = BROKEN_BOND_WEB_TEXTURE3D_BINDING;
inline constexpr uint32_t kWebTextureCubeBinding = BROKEN_BOND_WEB_TEXTURECUBE_BINDING;
inline constexpr uint32_t kWebTexture1DBinding = BROKEN_BOND_WEB_TEXTURE1D_BINDING;

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

#if defined(BROKEN_BOND_WGSL)
// WebGPU has no buffer device address. The three push-constant words are byte
// offsets into g_Upload, the renderer's upload ring bound as a storage buffer.
// Groups: 0 textures as separate bindings (2D t0-t31, 3D t32-t35, cube
// t36-t43, 1D t44-t59), 1 samplers s0-s15, 2 this buffer. Group 3 binding
// 999 is the push-constant block, which plume-web rewrites in.

struct PushConstants
{
    // Three little-endian uint64 offsets. WGSL has no u64, so each offset is
    // the low word and the high word is padding. The renderer still writes
    // three uint64s.
    uint VertexShaderConstants;
    uint VertexShaderConstantsHigh;
    uint PixelShaderConstants;
    uint PixelShaderConstantsHigh;
    uint SharedConstants;
    uint SharedConstantsHigh;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

ByteAddressBuffer g_Upload : register(t0, space2);

// Copied from the push constants at the start of main. Using the pointer in a
// later block makes naga spill it into a local, which WGSL cannot store.
static uint g_WebVertexBase;
static uint g_WebPixelBase;
static uint g_WebSharedBase;

#define UPLOAD_U32(BYTE) g_Upload.Load(uint(BYTE))
#define DESCRIPTOR_INDEX(KIND, SLOT) UPLOAD_U32(g_WebSharedBase + ((KIND) * DESCRIPTOR_SLOT_COUNT + (SLOT)) * 4)

#define g_Booleans                 UPLOAD_U32(g_WebSharedBase + 640)
#define g_SwappedTexcoords         UPLOAD_U32(g_WebSharedBase + 644)
#define g_HalfPixelOffset          asfloat(g_Upload.Load2(g_WebSharedBase + 648))
#define g_AlphaThreshold           asfloat(UPLOAD_U32(g_WebSharedBase + 656))
#define g_AlphaFunc                UPLOAD_U32(g_WebSharedBase + 660)
#define g_SpecConstants()          UPLOAD_U32(g_WebSharedBase + 664)

#elif defined(__spirv__)

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

#endif

#define TEXTURE1D_INDEX(SLOT)   DESCRIPTOR_INDEX(DESCRIPTOR_KIND_TEXTURE1D, SLOT)
#define TEXTURE2D_INDEX(SLOT)   DESCRIPTOR_INDEX(DESCRIPTOR_KIND_TEXTURE2D, SLOT)
#define TEXTURE3D_INDEX(SLOT)   DESCRIPTOR_INDEX(DESCRIPTOR_KIND_TEXTURE3D, SLOT)
#define TEXTURECube_INDEX(SLOT) DESCRIPTOR_INDEX(DESCRIPTOR_KIND_TEXTURECUBE, SLOT)
#define SAMPLER_INDEX(SLOT)     DESCRIPTOR_INDEX(DESCRIPTOR_KIND_SAMPLER, SLOT)

#if defined(BROKEN_BOND_WGSL)
Texture2D<float4> g_WebT2D_0 : register(t0, space0);
Texture2D<float4> g_WebT2D_1 : register(t1, space0);
Texture2D<float4> g_WebT2D_2 : register(t2, space0);
Texture2D<float4> g_WebT2D_3 : register(t3, space0);
Texture2D<float4> g_WebT2D_4 : register(t4, space0);
Texture2D<float4> g_WebT2D_5 : register(t5, space0);
Texture2D<float4> g_WebT2D_6 : register(t6, space0);
Texture2D<float4> g_WebT2D_7 : register(t7, space0);
Texture2D<float4> g_WebT2D_8 : register(t8, space0);
Texture2D<float4> g_WebT2D_9 : register(t9, space0);
Texture2D<float4> g_WebT2D_10 : register(t10, space0);
Texture2D<float4> g_WebT2D_11 : register(t11, space0);
Texture2D<float4> g_WebT2D_12 : register(t12, space0);
Texture2D<float4> g_WebT2D_13 : register(t13, space0);
Texture2D<float4> g_WebT2D_14 : register(t14, space0);
Texture2D<float4> g_WebT2D_15 : register(t15, space0);
Texture2D<float4> g_WebT2D_16 : register(t16, space0);
Texture2D<float4> g_WebT2D_17 : register(t17, space0);
Texture2D<float4> g_WebT2D_18 : register(t18, space0);
Texture2D<float4> g_WebT2D_19 : register(t19, space0);
Texture2D<float4> g_WebT2D_20 : register(t20, space0);
Texture2D<float4> g_WebT2D_21 : register(t21, space0);
Texture2D<float4> g_WebT2D_22 : register(t22, space0);
Texture2D<float4> g_WebT2D_23 : register(t23, space0);
Texture2D<float4> g_WebT2D_24 : register(t24, space0);
Texture2D<float4> g_WebT2D_25 : register(t25, space0);
Texture2D<float4> g_WebT2D_26 : register(t26, space0);
Texture2D<float4> g_WebT2D_27 : register(t27, space0);
Texture2D<float4> g_WebT2D_28 : register(t28, space0);
Texture2D<float4> g_WebT2D_29 : register(t29, space0);
Texture2D<float4> g_WebT2D_30 : register(t30, space0);
Texture2D<float4> g_WebT2D_31 : register(t31, space0);
Texture3D<float4> g_WebT3D_0 : register(t32, space0);
Texture3D<float4> g_WebT3D_1 : register(t33, space0);
Texture3D<float4> g_WebT3D_2 : register(t34, space0);
Texture3D<float4> g_WebT3D_3 : register(t35, space0);
TextureCube<float4> g_WebTCube_0 : register(t36, space0);
TextureCube<float4> g_WebTCube_1 : register(t37, space0);
TextureCube<float4> g_WebTCube_2 : register(t38, space0);
TextureCube<float4> g_WebTCube_3 : register(t39, space0);
TextureCube<float4> g_WebTCube_4 : register(t40, space0);
TextureCube<float4> g_WebTCube_5 : register(t41, space0);
TextureCube<float4> g_WebTCube_6 : register(t42, space0);
TextureCube<float4> g_WebTCube_7 : register(t43, space0);
Texture1D<float4> g_WebT1D_0 : register(t44, space0);
Texture1D<float4> g_WebT1D_1 : register(t45, space0);
Texture1D<float4> g_WebT1D_2 : register(t46, space0);
Texture1D<float4> g_WebT1D_3 : register(t47, space0);
Texture1D<float4> g_WebT1D_4 : register(t48, space0);
Texture1D<float4> g_WebT1D_5 : register(t49, space0);
Texture1D<float4> g_WebT1D_6 : register(t50, space0);
Texture1D<float4> g_WebT1D_7 : register(t51, space0);
Texture1D<float4> g_WebT1D_8 : register(t52, space0);
Texture1D<float4> g_WebT1D_9 : register(t53, space0);
Texture1D<float4> g_WebT1D_10 : register(t54, space0);
Texture1D<float4> g_WebT1D_11 : register(t55, space0);
Texture1D<float4> g_WebT1D_12 : register(t56, space0);
Texture1D<float4> g_WebT1D_13 : register(t57, space0);
Texture1D<float4> g_WebT1D_14 : register(t58, space0);
Texture1D<float4> g_WebT1D_15 : register(t59, space0);
SamplerState g_WebS_0 : register(s0, space1);
SamplerState g_WebS_1 : register(s1, space1);
SamplerState g_WebS_2 : register(s2, space1);
SamplerState g_WebS_3 : register(s3, space1);
SamplerState g_WebS_4 : register(s4, space1);
SamplerState g_WebS_5 : register(s5, space1);
SamplerState g_WebS_6 : register(s6, space1);
SamplerState g_WebS_7 : register(s7, space1);
SamplerState g_WebS_8 : register(s8, space1);
SamplerState g_WebS_9 : register(s9, space1);
SamplerState g_WebS_10 : register(s10, space1);
SamplerState g_WebS_11 : register(s11, space1);
SamplerState g_WebS_12 : register(s12, space1);
SamplerState g_WebS_13 : register(s13, space1);
SamplerState g_WebS_14 : register(s14, space1);
SamplerState g_WebS_15 : register(s15, space1);
float4 webSample2D(uint textureIndex, uint samplerIndex, float2 texCoord, float2 offset) {
    if (textureIndex == 0) {
        uint2 dimensions;
        g_WebT2D_0.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_0.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_0.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_0.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_0.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_0.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_0.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_0.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_0.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_0.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_0.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_0.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_0.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_0.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_0.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_0.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_0.Sample(g_WebS_15, uv);
        else return g_WebT2D_0.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 1) {
        uint2 dimensions;
        g_WebT2D_1.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_1.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_1.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_1.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_1.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_1.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_1.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_1.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_1.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_1.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_1.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_1.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_1.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_1.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_1.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_1.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_1.Sample(g_WebS_15, uv);
        else return g_WebT2D_1.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 2) {
        uint2 dimensions;
        g_WebT2D_2.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_2.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_2.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_2.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_2.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_2.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_2.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_2.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_2.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_2.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_2.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_2.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_2.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_2.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_2.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_2.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_2.Sample(g_WebS_15, uv);
        else return g_WebT2D_2.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 3) {
        uint2 dimensions;
        g_WebT2D_3.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_3.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_3.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_3.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_3.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_3.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_3.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_3.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_3.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_3.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_3.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_3.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_3.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_3.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_3.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_3.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_3.Sample(g_WebS_15, uv);
        else return g_WebT2D_3.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 4) {
        uint2 dimensions;
        g_WebT2D_4.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_4.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_4.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_4.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_4.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_4.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_4.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_4.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_4.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_4.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_4.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_4.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_4.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_4.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_4.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_4.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_4.Sample(g_WebS_15, uv);
        else return g_WebT2D_4.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 5) {
        uint2 dimensions;
        g_WebT2D_5.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_5.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_5.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_5.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_5.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_5.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_5.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_5.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_5.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_5.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_5.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_5.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_5.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_5.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_5.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_5.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_5.Sample(g_WebS_15, uv);
        else return g_WebT2D_5.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 6) {
        uint2 dimensions;
        g_WebT2D_6.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_6.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_6.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_6.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_6.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_6.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_6.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_6.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_6.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_6.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_6.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_6.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_6.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_6.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_6.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_6.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_6.Sample(g_WebS_15, uv);
        else return g_WebT2D_6.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 7) {
        uint2 dimensions;
        g_WebT2D_7.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_7.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_7.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_7.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_7.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_7.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_7.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_7.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_7.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_7.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_7.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_7.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_7.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_7.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_7.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_7.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_7.Sample(g_WebS_15, uv);
        else return g_WebT2D_7.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 8) {
        uint2 dimensions;
        g_WebT2D_8.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_8.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_8.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_8.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_8.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_8.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_8.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_8.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_8.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_8.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_8.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_8.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_8.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_8.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_8.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_8.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_8.Sample(g_WebS_15, uv);
        else return g_WebT2D_8.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 9) {
        uint2 dimensions;
        g_WebT2D_9.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_9.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_9.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_9.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_9.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_9.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_9.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_9.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_9.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_9.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_9.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_9.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_9.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_9.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_9.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_9.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_9.Sample(g_WebS_15, uv);
        else return g_WebT2D_9.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 10) {
        uint2 dimensions;
        g_WebT2D_10.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_10.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_10.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_10.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_10.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_10.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_10.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_10.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_10.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_10.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_10.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_10.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_10.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_10.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_10.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_10.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_10.Sample(g_WebS_15, uv);
        else return g_WebT2D_10.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 11) {
        uint2 dimensions;
        g_WebT2D_11.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_11.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_11.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_11.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_11.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_11.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_11.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_11.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_11.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_11.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_11.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_11.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_11.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_11.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_11.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_11.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_11.Sample(g_WebS_15, uv);
        else return g_WebT2D_11.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 12) {
        uint2 dimensions;
        g_WebT2D_12.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_12.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_12.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_12.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_12.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_12.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_12.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_12.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_12.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_12.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_12.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_12.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_12.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_12.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_12.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_12.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_12.Sample(g_WebS_15, uv);
        else return g_WebT2D_12.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 13) {
        uint2 dimensions;
        g_WebT2D_13.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_13.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_13.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_13.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_13.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_13.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_13.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_13.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_13.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_13.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_13.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_13.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_13.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_13.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_13.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_13.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_13.Sample(g_WebS_15, uv);
        else return g_WebT2D_13.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 14) {
        uint2 dimensions;
        g_WebT2D_14.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_14.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_14.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_14.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_14.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_14.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_14.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_14.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_14.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_14.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_14.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_14.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_14.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_14.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_14.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_14.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_14.Sample(g_WebS_15, uv);
        else return g_WebT2D_14.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 15) {
        uint2 dimensions;
        g_WebT2D_15.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_15.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_15.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_15.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_15.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_15.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_15.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_15.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_15.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_15.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_15.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_15.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_15.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_15.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_15.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_15.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_15.Sample(g_WebS_15, uv);
        else return g_WebT2D_15.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 16) {
        uint2 dimensions;
        g_WebT2D_16.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_16.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_16.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_16.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_16.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_16.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_16.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_16.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_16.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_16.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_16.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_16.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_16.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_16.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_16.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_16.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_16.Sample(g_WebS_15, uv);
        else return g_WebT2D_16.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 17) {
        uint2 dimensions;
        g_WebT2D_17.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_17.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_17.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_17.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_17.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_17.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_17.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_17.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_17.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_17.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_17.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_17.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_17.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_17.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_17.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_17.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_17.Sample(g_WebS_15, uv);
        else return g_WebT2D_17.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 18) {
        uint2 dimensions;
        g_WebT2D_18.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_18.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_18.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_18.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_18.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_18.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_18.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_18.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_18.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_18.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_18.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_18.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_18.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_18.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_18.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_18.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_18.Sample(g_WebS_15, uv);
        else return g_WebT2D_18.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 19) {
        uint2 dimensions;
        g_WebT2D_19.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_19.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_19.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_19.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_19.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_19.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_19.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_19.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_19.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_19.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_19.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_19.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_19.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_19.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_19.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_19.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_19.Sample(g_WebS_15, uv);
        else return g_WebT2D_19.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 20) {
        uint2 dimensions;
        g_WebT2D_20.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_20.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_20.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_20.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_20.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_20.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_20.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_20.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_20.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_20.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_20.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_20.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_20.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_20.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_20.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_20.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_20.Sample(g_WebS_15, uv);
        else return g_WebT2D_20.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 21) {
        uint2 dimensions;
        g_WebT2D_21.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_21.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_21.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_21.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_21.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_21.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_21.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_21.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_21.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_21.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_21.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_21.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_21.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_21.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_21.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_21.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_21.Sample(g_WebS_15, uv);
        else return g_WebT2D_21.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 22) {
        uint2 dimensions;
        g_WebT2D_22.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_22.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_22.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_22.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_22.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_22.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_22.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_22.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_22.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_22.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_22.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_22.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_22.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_22.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_22.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_22.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_22.Sample(g_WebS_15, uv);
        else return g_WebT2D_22.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 23) {
        uint2 dimensions;
        g_WebT2D_23.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_23.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_23.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_23.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_23.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_23.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_23.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_23.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_23.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_23.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_23.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_23.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_23.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_23.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_23.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_23.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_23.Sample(g_WebS_15, uv);
        else return g_WebT2D_23.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 24) {
        uint2 dimensions;
        g_WebT2D_24.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_24.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_24.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_24.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_24.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_24.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_24.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_24.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_24.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_24.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_24.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_24.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_24.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_24.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_24.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_24.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_24.Sample(g_WebS_15, uv);
        else return g_WebT2D_24.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 25) {
        uint2 dimensions;
        g_WebT2D_25.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_25.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_25.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_25.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_25.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_25.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_25.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_25.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_25.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_25.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_25.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_25.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_25.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_25.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_25.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_25.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_25.Sample(g_WebS_15, uv);
        else return g_WebT2D_25.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 26) {
        uint2 dimensions;
        g_WebT2D_26.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_26.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_26.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_26.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_26.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_26.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_26.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_26.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_26.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_26.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_26.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_26.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_26.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_26.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_26.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_26.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_26.Sample(g_WebS_15, uv);
        else return g_WebT2D_26.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 27) {
        uint2 dimensions;
        g_WebT2D_27.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_27.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_27.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_27.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_27.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_27.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_27.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_27.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_27.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_27.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_27.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_27.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_27.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_27.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_27.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_27.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_27.Sample(g_WebS_15, uv);
        else return g_WebT2D_27.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 28) {
        uint2 dimensions;
        g_WebT2D_28.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_28.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_28.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_28.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_28.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_28.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_28.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_28.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_28.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_28.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_28.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_28.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_28.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_28.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_28.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_28.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_28.Sample(g_WebS_15, uv);
        else return g_WebT2D_28.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 29) {
        uint2 dimensions;
        g_WebT2D_29.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_29.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_29.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_29.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_29.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_29.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_29.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_29.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_29.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_29.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_29.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_29.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_29.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_29.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_29.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_29.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_29.Sample(g_WebS_15, uv);
        else return g_WebT2D_29.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 30) {
        uint2 dimensions;
        g_WebT2D_30.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_30.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_30.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_30.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_30.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_30.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_30.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_30.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_30.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_30.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_30.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_30.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_30.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_30.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_30.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_30.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_30.Sample(g_WebS_15, uv);
        else return g_WebT2D_30.Sample(g_WebS_0, uv);
    }
    else if (textureIndex == 31) {
        uint2 dimensions;
        g_WebT2D_31.GetDimensions(dimensions.x, dimensions.y);
        float2 uv = texCoord + offset / dimensions;
        if (samplerIndex == 0) return g_WebT2D_31.Sample(g_WebS_0, uv);
        else if (samplerIndex == 1) return g_WebT2D_31.Sample(g_WebS_1, uv);
        else if (samplerIndex == 2) return g_WebT2D_31.Sample(g_WebS_2, uv);
        else if (samplerIndex == 3) return g_WebT2D_31.Sample(g_WebS_3, uv);
        else if (samplerIndex == 4) return g_WebT2D_31.Sample(g_WebS_4, uv);
        else if (samplerIndex == 5) return g_WebT2D_31.Sample(g_WebS_5, uv);
        else if (samplerIndex == 6) return g_WebT2D_31.Sample(g_WebS_6, uv);
        else if (samplerIndex == 7) return g_WebT2D_31.Sample(g_WebS_7, uv);
        else if (samplerIndex == 8) return g_WebT2D_31.Sample(g_WebS_8, uv);
        else if (samplerIndex == 9) return g_WebT2D_31.Sample(g_WebS_9, uv);
        else if (samplerIndex == 10) return g_WebT2D_31.Sample(g_WebS_10, uv);
        else if (samplerIndex == 11) return g_WebT2D_31.Sample(g_WebS_11, uv);
        else if (samplerIndex == 12) return g_WebT2D_31.Sample(g_WebS_12, uv);
        else if (samplerIndex == 13) return g_WebT2D_31.Sample(g_WebS_13, uv);
        else if (samplerIndex == 14) return g_WebT2D_31.Sample(g_WebS_14, uv);
        else if (samplerIndex == 15) return g_WebT2D_31.Sample(g_WebS_15, uv);
        else return g_WebT2D_31.Sample(g_WebS_0, uv);
    }
    return 0;
}
float4 webSample1D(uint textureIndex, uint samplerIndex, float texCoord, float offset) {
    if (textureIndex == 0) {
        uint width;
        g_WebT1D_0.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_0.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_0.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_0.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_0.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_0.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_0.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_0.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_0.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_0.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_0.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_0.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_0.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_0.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_0.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_0.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_0.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_0.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 1) {
        uint width;
        g_WebT1D_1.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_1.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_1.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_1.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_1.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_1.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_1.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_1.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_1.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_1.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_1.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_1.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_1.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_1.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_1.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_1.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_1.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_1.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 2) {
        uint width;
        g_WebT1D_2.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_2.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_2.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_2.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_2.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_2.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_2.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_2.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_2.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_2.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_2.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_2.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_2.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_2.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_2.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_2.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_2.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_2.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 3) {
        uint width;
        g_WebT1D_3.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_3.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_3.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_3.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_3.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_3.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_3.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_3.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_3.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_3.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_3.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_3.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_3.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_3.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_3.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_3.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_3.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_3.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 4) {
        uint width;
        g_WebT1D_4.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_4.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_4.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_4.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_4.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_4.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_4.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_4.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_4.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_4.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_4.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_4.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_4.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_4.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_4.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_4.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_4.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_4.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 5) {
        uint width;
        g_WebT1D_5.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_5.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_5.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_5.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_5.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_5.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_5.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_5.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_5.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_5.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_5.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_5.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_5.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_5.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_5.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_5.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_5.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_5.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 6) {
        uint width;
        g_WebT1D_6.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_6.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_6.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_6.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_6.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_6.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_6.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_6.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_6.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_6.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_6.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_6.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_6.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_6.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_6.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_6.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_6.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_6.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 7) {
        uint width;
        g_WebT1D_7.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_7.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_7.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_7.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_7.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_7.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_7.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_7.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_7.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_7.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_7.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_7.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_7.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_7.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_7.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_7.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_7.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_7.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 8) {
        uint width;
        g_WebT1D_8.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_8.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_8.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_8.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_8.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_8.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_8.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_8.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_8.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_8.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_8.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_8.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_8.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_8.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_8.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_8.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_8.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_8.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 9) {
        uint width;
        g_WebT1D_9.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_9.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_9.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_9.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_9.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_9.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_9.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_9.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_9.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_9.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_9.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_9.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_9.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_9.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_9.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_9.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_9.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_9.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 10) {
        uint width;
        g_WebT1D_10.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_10.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_10.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_10.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_10.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_10.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_10.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_10.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_10.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_10.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_10.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_10.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_10.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_10.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_10.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_10.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_10.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_10.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 11) {
        uint width;
        g_WebT1D_11.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_11.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_11.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_11.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_11.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_11.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_11.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_11.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_11.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_11.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_11.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_11.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_11.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_11.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_11.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_11.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_11.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_11.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 12) {
        uint width;
        g_WebT1D_12.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_12.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_12.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_12.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_12.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_12.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_12.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_12.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_12.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_12.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_12.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_12.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_12.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_12.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_12.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_12.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_12.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_12.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 13) {
        uint width;
        g_WebT1D_13.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_13.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_13.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_13.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_13.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_13.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_13.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_13.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_13.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_13.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_13.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_13.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_13.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_13.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_13.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_13.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_13.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_13.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 14) {
        uint width;
        g_WebT1D_14.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_14.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_14.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_14.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_14.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_14.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_14.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_14.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_14.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_14.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_14.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_14.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_14.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_14.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_14.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_14.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_14.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_14.SampleLevel(g_WebS_0, u, 0);
    }
    else if (textureIndex == 15) {
        uint width;
        g_WebT1D_15.GetDimensions(width);
        float u = texCoord + offset / width;
        if (samplerIndex == 0) return g_WebT1D_15.SampleLevel(g_WebS_0, u, 0);
        else if (samplerIndex == 1) return g_WebT1D_15.SampleLevel(g_WebS_1, u, 0);
        else if (samplerIndex == 2) return g_WebT1D_15.SampleLevel(g_WebS_2, u, 0);
        else if (samplerIndex == 3) return g_WebT1D_15.SampleLevel(g_WebS_3, u, 0);
        else if (samplerIndex == 4) return g_WebT1D_15.SampleLevel(g_WebS_4, u, 0);
        else if (samplerIndex == 5) return g_WebT1D_15.SampleLevel(g_WebS_5, u, 0);
        else if (samplerIndex == 6) return g_WebT1D_15.SampleLevel(g_WebS_6, u, 0);
        else if (samplerIndex == 7) return g_WebT1D_15.SampleLevel(g_WebS_7, u, 0);
        else if (samplerIndex == 8) return g_WebT1D_15.SampleLevel(g_WebS_8, u, 0);
        else if (samplerIndex == 9) return g_WebT1D_15.SampleLevel(g_WebS_9, u, 0);
        else if (samplerIndex == 10) return g_WebT1D_15.SampleLevel(g_WebS_10, u, 0);
        else if (samplerIndex == 11) return g_WebT1D_15.SampleLevel(g_WebS_11, u, 0);
        else if (samplerIndex == 12) return g_WebT1D_15.SampleLevel(g_WebS_12, u, 0);
        else if (samplerIndex == 13) return g_WebT1D_15.SampleLevel(g_WebS_13, u, 0);
        else if (samplerIndex == 14) return g_WebT1D_15.SampleLevel(g_WebS_14, u, 0);
        else if (samplerIndex == 15) return g_WebT1D_15.SampleLevel(g_WebS_15, u, 0);
        else return g_WebT1D_15.SampleLevel(g_WebS_0, u, 0);
    }
    return 0;
}
float4 webSample3D(uint textureIndex, uint samplerIndex, float3 texCoord) {
    if (textureIndex == 0) {
        if (samplerIndex == 0) return g_WebT3D_0.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebT3D_0.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebT3D_0.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebT3D_0.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebT3D_0.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebT3D_0.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebT3D_0.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebT3D_0.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebT3D_0.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebT3D_0.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebT3D_0.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebT3D_0.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebT3D_0.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebT3D_0.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebT3D_0.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebT3D_0.Sample(g_WebS_15, texCoord);
        else return g_WebT3D_0.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 1) {
        if (samplerIndex == 0) return g_WebT3D_1.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebT3D_1.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebT3D_1.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebT3D_1.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebT3D_1.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebT3D_1.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebT3D_1.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebT3D_1.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebT3D_1.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebT3D_1.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebT3D_1.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebT3D_1.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebT3D_1.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebT3D_1.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebT3D_1.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebT3D_1.Sample(g_WebS_15, texCoord);
        else return g_WebT3D_1.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 2) {
        if (samplerIndex == 0) return g_WebT3D_2.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebT3D_2.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebT3D_2.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebT3D_2.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebT3D_2.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebT3D_2.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebT3D_2.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebT3D_2.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebT3D_2.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebT3D_2.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebT3D_2.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebT3D_2.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebT3D_2.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebT3D_2.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebT3D_2.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebT3D_2.Sample(g_WebS_15, texCoord);
        else return g_WebT3D_2.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 3) {
        if (samplerIndex == 0) return g_WebT3D_3.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebT3D_3.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebT3D_3.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebT3D_3.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebT3D_3.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebT3D_3.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebT3D_3.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebT3D_3.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebT3D_3.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebT3D_3.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebT3D_3.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebT3D_3.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebT3D_3.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebT3D_3.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebT3D_3.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebT3D_3.Sample(g_WebS_15, texCoord);
        else return g_WebT3D_3.Sample(g_WebS_0, texCoord);
    }
    return 0;
}
float4 webSampleCube(uint textureIndex, uint samplerIndex, float3 texCoord) {
    if (textureIndex == 0) {
        if (samplerIndex == 0) return g_WebTCube_0.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebTCube_0.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebTCube_0.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebTCube_0.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebTCube_0.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebTCube_0.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebTCube_0.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebTCube_0.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebTCube_0.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebTCube_0.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebTCube_0.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebTCube_0.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebTCube_0.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebTCube_0.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebTCube_0.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebTCube_0.Sample(g_WebS_15, texCoord);
        else return g_WebTCube_0.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 1) {
        if (samplerIndex == 0) return g_WebTCube_1.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebTCube_1.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebTCube_1.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebTCube_1.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebTCube_1.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebTCube_1.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebTCube_1.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebTCube_1.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebTCube_1.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebTCube_1.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebTCube_1.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebTCube_1.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebTCube_1.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebTCube_1.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebTCube_1.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebTCube_1.Sample(g_WebS_15, texCoord);
        else return g_WebTCube_1.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 2) {
        if (samplerIndex == 0) return g_WebTCube_2.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebTCube_2.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebTCube_2.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebTCube_2.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebTCube_2.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebTCube_2.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebTCube_2.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebTCube_2.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebTCube_2.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebTCube_2.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebTCube_2.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebTCube_2.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebTCube_2.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebTCube_2.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebTCube_2.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebTCube_2.Sample(g_WebS_15, texCoord);
        else return g_WebTCube_2.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 3) {
        if (samplerIndex == 0) return g_WebTCube_3.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebTCube_3.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebTCube_3.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebTCube_3.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebTCube_3.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebTCube_3.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebTCube_3.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebTCube_3.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebTCube_3.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebTCube_3.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebTCube_3.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebTCube_3.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebTCube_3.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebTCube_3.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebTCube_3.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebTCube_3.Sample(g_WebS_15, texCoord);
        else return g_WebTCube_3.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 4) {
        if (samplerIndex == 0) return g_WebTCube_4.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebTCube_4.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebTCube_4.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebTCube_4.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebTCube_4.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebTCube_4.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebTCube_4.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebTCube_4.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebTCube_4.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebTCube_4.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebTCube_4.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebTCube_4.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebTCube_4.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebTCube_4.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebTCube_4.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebTCube_4.Sample(g_WebS_15, texCoord);
        else return g_WebTCube_4.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 5) {
        if (samplerIndex == 0) return g_WebTCube_5.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebTCube_5.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebTCube_5.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebTCube_5.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebTCube_5.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebTCube_5.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebTCube_5.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebTCube_5.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebTCube_5.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebTCube_5.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebTCube_5.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebTCube_5.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebTCube_5.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebTCube_5.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebTCube_5.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebTCube_5.Sample(g_WebS_15, texCoord);
        else return g_WebTCube_5.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 6) {
        if (samplerIndex == 0) return g_WebTCube_6.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebTCube_6.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebTCube_6.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebTCube_6.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebTCube_6.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebTCube_6.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebTCube_6.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebTCube_6.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebTCube_6.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebTCube_6.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebTCube_6.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebTCube_6.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebTCube_6.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebTCube_6.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebTCube_6.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebTCube_6.Sample(g_WebS_15, texCoord);
        else return g_WebTCube_6.Sample(g_WebS_0, texCoord);
    }
    else if (textureIndex == 7) {
        if (samplerIndex == 0) return g_WebTCube_7.Sample(g_WebS_0, texCoord);
        else if (samplerIndex == 1) return g_WebTCube_7.Sample(g_WebS_1, texCoord);
        else if (samplerIndex == 2) return g_WebTCube_7.Sample(g_WebS_2, texCoord);
        else if (samplerIndex == 3) return g_WebTCube_7.Sample(g_WebS_3, texCoord);
        else if (samplerIndex == 4) return g_WebTCube_7.Sample(g_WebS_4, texCoord);
        else if (samplerIndex == 5) return g_WebTCube_7.Sample(g_WebS_5, texCoord);
        else if (samplerIndex == 6) return g_WebTCube_7.Sample(g_WebS_6, texCoord);
        else if (samplerIndex == 7) return g_WebTCube_7.Sample(g_WebS_7, texCoord);
        else if (samplerIndex == 8) return g_WebTCube_7.Sample(g_WebS_8, texCoord);
        else if (samplerIndex == 9) return g_WebTCube_7.Sample(g_WebS_9, texCoord);
        else if (samplerIndex == 10) return g_WebTCube_7.Sample(g_WebS_10, texCoord);
        else if (samplerIndex == 11) return g_WebTCube_7.Sample(g_WebS_11, texCoord);
        else if (samplerIndex == 12) return g_WebTCube_7.Sample(g_WebS_12, texCoord);
        else if (samplerIndex == 13) return g_WebTCube_7.Sample(g_WebS_13, texCoord);
        else if (samplerIndex == 14) return g_WebTCube_7.Sample(g_WebS_14, texCoord);
        else if (samplerIndex == 15) return g_WebTCube_7.Sample(g_WebS_15, texCoord);
        else return g_WebTCube_7.Sample(g_WebS_0, texCoord);
    }
    return 0;
}
uint2 webDimensions2D(uint textureIndex) {
    uint2 dimensions;
    if (textureIndex == 0) g_WebT2D_0.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 1) g_WebT2D_1.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 2) g_WebT2D_2.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 3) g_WebT2D_3.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 4) g_WebT2D_4.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 5) g_WebT2D_5.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 6) g_WebT2D_6.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 7) g_WebT2D_7.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 8) g_WebT2D_8.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 9) g_WebT2D_9.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 10) g_WebT2D_10.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 11) g_WebT2D_11.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 12) g_WebT2D_12.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 13) g_WebT2D_13.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 14) g_WebT2D_14.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 15) g_WebT2D_15.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 16) g_WebT2D_16.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 17) g_WebT2D_17.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 18) g_WebT2D_18.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 19) g_WebT2D_19.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 20) g_WebT2D_20.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 21) g_WebT2D_21.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 22) g_WebT2D_22.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 23) g_WebT2D_23.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 24) g_WebT2D_24.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 25) g_WebT2D_25.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 26) g_WebT2D_26.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 27) g_WebT2D_27.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 28) g_WebT2D_28.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 29) g_WebT2D_29.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 30) g_WebT2D_30.GetDimensions(dimensions.x, dimensions.y);
    else if (textureIndex == 31) g_WebT2D_31.GetDimensions(dimensions.x, dimensions.y);
    else g_WebT2D_0.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}
#else
Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space1);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space2);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space3);
Texture1D<float4> g_Texture1DDescriptorHeap[] : register(t0, space5);
#endif

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
#if defined(BROKEN_BOND_WGSL)
    return webSample1D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset);
#else
    Texture1D<float4> texture = g_Texture1DDescriptorHeap[resourceDescriptorIndex];
    uint width;
    texture.GetDimensions(width);
    return texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / width, 0);
#endif
}

float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
#if defined(BROKEN_BOND_WGSL)
    return webSample2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset);
#else
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return texture.Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord + offset / getTexture2DDimensions(texture));
#endif
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
#if defined(BROKEN_BOND_WGSL)
    uint2 dimensions = webDimensions2D(resourceDescriptorIndex);
#else
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    uint2 dimensions = getTexture2DDimensions(texture);
#endif
    return select(isnan(texCoord), 0.0, frac(texCoord * dimensions + offset - 0.5));
}

float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord)
{
#if defined(BROKEN_BOND_WGSL)
    return webSample3D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord);
#else
    return g_Texture3DDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], texCoord);
#endif
}

struct CubeMapData
{
    float3 cubeMapDirections[2];
    uint cubeMapIndex;
};

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, inout CubeMapData cubeMapData)
{
#if defined(BROKEN_BOND_WGSL)
    return webSampleCube(resourceDescriptorIndex, samplerDescriptorIndex, cubeMapData.cubeMapDirections[texCoord.z]);
#else
    return g_TextureCubeDescriptorHeap[resourceDescriptorIndex].Sample(g_SamplerDescriptorHeap[samplerDescriptorIndex], cubeMapData.cubeMapDirections[texCoord.z]);
#endif
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
#if defined(BROKEN_BOND_WGSL)
    return (swappedTexcoords & (1u << semanticIndex)) != 0 ? value.yxwz : value;
#else
    return (swappedTexcoords & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
#endif
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
