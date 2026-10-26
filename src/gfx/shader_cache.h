// Declarations for the shader cache XenosRecomp writes (shader_cache.cpp in
// the build tree, generated from game/xeshader.sdb; never committed).
//
// Each entry is one unique guest shader container, keyed by XXH3_64bits over
// the container bytes (virtualSize + physicalSize). The SPIR-V blob is
// zstd-compressed as a whole and each shader inside it is smol-v encoded.

#pragma once

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry {
  uint64_t hash;
  uint32_t dxilOffset;
  uint32_t dxilSize;
  uint32_t spirvOffset;
  uint32_t spirvSize;
  uint32_t specConstantsMask;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;

#if defined(_WIN32)
extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;
#endif
