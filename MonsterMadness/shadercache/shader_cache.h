// Struct + externs for the XenosRecomp-generated DXIL/SPIR-V shader cache
// (mm_shader_cache.cpp). Field order matches XenosRecomp main.cpp's emitter:
// { hash, dxilOffset, dxilSize, spirvOffset, spirvSize, airOffset, airSize,
//   specConstantsMask, filename }.

#pragma once

#include <cstddef>
#include <cstdint>

struct ShaderCacheEntry {
  uint64_t hash;            // XXH64 of the guest Xenos shader
  uint32_t dxilOffset;      // into the decompressed DXIL cache
  uint32_t dxilSize;
  uint32_t spirvOffset;     // into the decompressed SPIR-V cache
  uint32_t spirvSize;
  uint32_t airOffset;       // Metal AIR (unused on Windows)
  uint32_t airSize;
  uint32_t specConstantsMask;
  const char* filename;
};

extern ShaderCacheEntry g_shaderCacheEntries[];
extern const size_t g_shaderCacheEntryCount;

extern const uint8_t g_compressedDxilCache[];
extern const size_t g_dxilCacheCompressedSize;
extern const size_t g_dxilCacheDecompressedSize;

extern const uint8_t g_compressedSpirvCache[];
extern const size_t g_spirvCacheCompressedSize;
extern const size_t g_spirvCacheDecompressedSize;
