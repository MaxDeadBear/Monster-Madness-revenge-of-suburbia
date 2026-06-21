// Runtime access to the precompiled XenosRecomp DXIL shader cache.
// Decompresses the embedded cache and resolves a guest-shader hash to its DXIL.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

// Decompress the embedded DXIL cache (idempotent). Returns the entry count.
size_t InitShaderCache();

// Look up DXIL for a guest shader hash (XXH3-64). Returns a pointer into the
// decompressed cache (valid for process lifetime) + size, or nullptr if absent.
const uint8_t* LookupShaderDxil(uint64_t hash, uint32_t& sizeOut, uint32_t& specConstantsMaskOut);

// Get the DXIL blob for cache entry `index` (for validation/self-test). Returns a
// pointer into the decompressed cache + size, or nullptr if index is out of range.
const uint8_t* GetCacheDxilByIndex(size_t index, uint32_t& sizeOut);

// If `p` is a Xenos shader container (magic 0x102A11xx), hash it (XXH3 of
// virtualSize+physicalSize bytes, matching XenosRecomp) and look up its DXIL.
// Returns DXIL or nullptr; sets hashOut/isPixelOut. SEH-safe on bad pointers.
const uint8_t* TryShaderContainerLookup(const uint8_t* p, uint32_t& dxilSizeOut,
                                        uint32_t& specMaskOut, uint64_t& hashOut, bool& isPixelOut);

// Build (idempotent) an index of every Xenos shader container in guest memory,
// keyed by the FNV-1a hash of its ucode region (matching the CP's HashUcode). The
// ucode the GPU runs is copied away from its rich container, so we match by ucode
// content, not address. Returns the number of indexed containers.
size_t BuildUcodeIndex(const uint8_t* base, uint32_t startAddr, uint32_t endAddr);

// Re-scan to pick up containers loaded since the last build (additive). Call on a
// lookup miss; throttle at the call site (a full scan is not free).
size_t RebuildUcodeIndex(const uint8_t* base, uint32_t startAddr, uint32_t endAddr);

// Diagnostics: indexed container counts, and a prefix/suffix probe for why an exact
// ucode FNV match failed (relation: 1=exact,2=container-is-prefix,3=container-is-suffix).
void GetUcodeIndexStats(size_t& totalOut, size_t& vsOut, size_t& psOut);

// Return the first indexed container of the requested stage (for a pipeline self-test
// with real game shaders), or nullptr. Feed to TranslateXenosShaderToDxil.
const uint8_t* GetIndexedContainer(bool wantPixel);

// Collect up to `maxOut` indexed containers of the requested stage into `out`. Returns
// the number written. Used to find the template container for a specialized-variant
// ucode (the variant's hash never matches a container, so we splice + test-translate).
size_t GetIndexedContainers(bool wantPixel, const uint8_t** out, size_t maxOut);

bool DiagnoseUcodeMatch(const uint8_t* ucode, uint32_t dwordCount, uint32_t& matchedDwordsOut,
                        int& relationOut);

// Find the rich container (in guest RAM) for a bound shader's ucode FNV hash (the
// value the CP computes in LoadShader). Returns the container pointer (feed to
// TranslateXenosShaderToDxil) or nullptr; sets containerHash/isPixel/found.
const uint8_t* LookupContainerByUcodeHash(uint64_t ucodeFnvHash, uint64_t& containerHashOut,
                                          bool& isPixelOut, bool& containerFoundOut);

// One-time scan of guest memory [startAddr,endAddr) for shader containers; hashes
// each and checks the cache. Logs found/matched to mm_shadercache.log. Returns
// the number that matched a cache entry. Proves runtime find+hash+lookup works.
size_t ScanForShaders(const uint8_t* base, uint32_t startAddr, uint32_t endAddr);

}  // namespace mm
