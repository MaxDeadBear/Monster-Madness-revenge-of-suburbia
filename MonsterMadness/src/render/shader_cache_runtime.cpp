// See shader_cache_runtime.h. Compiled inside the xenos_recomp lib (which links
// zstd and has shadercache/ on its include path).

#include "render/shader_cache_runtime.h"

#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <xxhash.h>
#include <zstd.h>

#include "render/shader_translate.h"  // mm::SafePeek (SEH-safe guest read)
#include "shader_cache.h"             // generated cache data + ShaderCacheEntry

namespace mm {

namespace {
std::vector<uint8_t> g_dxil;  // decompressed DXIL cache
bool g_inited = false;
}  // namespace

size_t InitShaderCache() {
  if (!g_inited) {
    g_inited = true;
    g_dxil.resize(g_dxilCacheDecompressedSize);
    const size_t n = ZSTD_decompress(g_dxil.data(), g_dxil.size(), g_compressedDxilCache,
                                     g_dxilCacheCompressedSize);
    const bool ok = !ZSTD_isError(n) && n == g_dxilCacheDecompressedSize;
    if (!ok) g_dxil.clear();

    // Self-test: confirm the cache loads, decompresses, and the first entry's
    // DXIL slice is a valid DXBC container.
    if (FILE* f = std::fopen("mm_shadercache.log", "w")) {
      std::fprintf(f, "entries=%zu compressed=%zu decompressed=%zu decompress=%s\n",
                   g_shaderCacheEntryCount, g_dxilCacheCompressedSize, g_dxilCacheDecompressedSize,
                   ok ? "OK" : "FAIL");
      if (ok && g_shaderCacheEntryCount > 0) {
        const ShaderCacheEntry& e = g_shaderCacheEntries[0];
        const uint8_t* d = g_dxil.data() + e.dxilOffset;
        std::fprintf(f, "entry0 hash=%016llX dxil=%u magic=%c%c%c%c\n",
                     (unsigned long long)e.hash, e.dxilSize, d[0], d[1], d[2], d[3]);
      }
      std::fclose(f);
    }
  }
  return g_shaderCacheEntryCount;
}

const uint8_t* LookupShaderDxil(uint64_t hash, uint32_t& sizeOut, uint32_t& specConstantsMaskOut) {
  sizeOut = 0;
  specConstantsMaskOut = 0;
  if (!g_inited) InitShaderCache();
  if (g_dxil.empty()) return nullptr;

  // 186 entries — linear scan is fine.
  for (size_t i = 0; i < g_shaderCacheEntryCount; ++i) {
    const ShaderCacheEntry& e = g_shaderCacheEntries[i];
    if (e.hash != hash) continue;
    if (size_t(e.dxilOffset) + e.dxilSize > g_dxil.size()) return nullptr;
    sizeOut = e.dxilSize;
    specConstantsMaskOut = e.specConstantsMask;
    return g_dxil.data() + e.dxilOffset;
  }
  return nullptr;
}

const uint8_t* GetCacheDxilByIndex(size_t index, uint32_t& sizeOut) {
  sizeOut = 0;
  if (!g_inited) InitShaderCache();
  if (g_dxil.empty() || index >= g_shaderCacheEntryCount) return nullptr;
  const ShaderCacheEntry& e = g_shaderCacheEntries[index];
  if (size_t(e.dxilOffset) + e.dxilSize > g_dxil.size()) return nullptr;
  sizeOut = e.dxilSize;
  return g_dxil.data() + e.dxilOffset;
}

namespace {
uint32_t BeRead32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint64_t SafeXXH3(const uint8_t* p, size_t n) {
  __try {
    return XXH3_64bits(p, n);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}
}  // namespace

const uint8_t* TryShaderContainerLookup(const uint8_t* p, uint32_t& dxilSizeOut, uint32_t& specMaskOut,
                                        uint64_t& hashOut, bool& isPixelOut) {
  dxilSizeOut = 0;
  specMaskOut = 0;
  hashOut = 0;
  isPixelOut = false;
  uint8_t hdr[12];
  if (mm::SafePeek(p, hdr, sizeof(hdr)) != sizeof(hdr)) return nullptr;
  const uint32_t flags = BeRead32(hdr);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return nullptr;
  isPixelOut = (flags & 1u) == 0;
  const uint64_t total = uint64_t(BeRead32(hdr + 4)) + BeRead32(hdr + 8);  // virtual+physical
  if (total < 36 || total > (256u * 1024u)) return nullptr;                // shaders are small
  hashOut = SafeXXH3(p, size_t(total));
  if (hashOut == 0) return nullptr;
  return LookupShaderDxil(hashOut, dxilSizeOut, specMaskOut);
}

namespace {
struct UcodeEntry {
  const uint8_t* container = nullptr;  // rich container in guest RAM (for translation)
  uint64_t containerHash = 0;
  bool isPixel = false;
};
std::unordered_map<uint64_t, UcodeEntry> g_ucodeIndex;
bool g_ucodeIndexBuilt = false;
size_t g_indexVS = 0, g_indexPS = 0;

// FNV-1a 64 over ucode dwords, matching MMNullCommandProcessor::HashUcode (reads
// raw uint32 from memory; both bound ucode and container ucode are guest BE, read
// identically, so the hashes match).
uint64_t FnvUcode(const uint8_t* p, uint32_t dwordCount) {
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0; i < dwordCount; ++i) {
    uint32_t v;
    std::memcpy(&v, p + size_t(i) * 4, 4);
    h = (h ^ v) * 1099511628211ull;
  }
  return h;
}

// Index one container at C (already magic-validated). Adds its ucode-hash entry.
void IndexContainer(const uint8_t* C) {
  uint8_t hdr[0x20];
  if (mm::SafePeek(C, hdr, sizeof(hdr)) != sizeof(hdr)) return;
  const uint32_t flags = BeRead32(hdr + 0x00);
  const uint32_t virtualSize = BeRead32(hdr + 0x04);
  const uint32_t physicalSize = BeRead32(hdr + 0x08);
  const uint32_t shaderOffset = BeRead32(hdr + 0x18);
  const uint64_t total = uint64_t(virtualSize) + physicalSize;
  if (total < 36 || total > (256u * 1024u)) return;
  if (shaderOffset + 8 > virtualSize) return;

  uint8_t sh[8];  // Shader: physicalOffset(+0), size(+4)
  if (mm::SafePeek(C + shaderOffset, sh, 8) != 8) return;
  const uint32_t physicalOffset = BeRead32(sh + 0);
  const uint32_t ucodeSize = BeRead32(sh + 4);
  if (ucodeSize == 0 || (ucodeSize % 4) != 0 || ucodeSize > physicalSize) return;
  if (physicalOffset > physicalSize - ucodeSize) return;

  const uint8_t* ucode = C + size_t(virtualSize) + physicalOffset;
  uint64_t fnv;
  __try {
    fnv = FnvUcode(ucode, ucodeSize / 4);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return;
  }

  UcodeEntry e;
  e.container = C;
  e.isPixel = (flags & 1u) == 0;
  e.containerHash = SafeXXH3(C, size_t(total));
  if (g_ucodeIndex.emplace(fnv, e).second) {
    if (e.isPixel) ++g_indexPS; else ++g_indexVS;
  }
}

// Re-read the container at C and return its ucode FNV (the same value IndexContainer stored),
// or 0 if C is no longer a valid container. Used to detect STALE index pointers: the game frees/
// reuses container memory at the movie->menu transition, so a hash hit may point at garbage —
// translating it makes recompile() read insane offsets/counts (bad_alloc / "invalid range").
uint64_t ComputeContainerUcodeFnv(const uint8_t* C) {
  uint8_t hdr[0x20];
  if (mm::SafePeek(C, hdr, sizeof(hdr)) != sizeof(hdr)) return 0;
  if ((BeRead32(hdr + 0x00) & 0xFFFFFF00u) != 0x102A1100u) return 0;  // magic gone -> stale
  const uint32_t virtualSize = BeRead32(hdr + 0x04);
  const uint32_t physicalSize = BeRead32(hdr + 0x08);
  const uint32_t shaderOffset = BeRead32(hdr + 0x18);
  const uint64_t total = uint64_t(virtualSize) + physicalSize;
  if (total < 36 || total > (256u * 1024u)) return 0;
  if (shaderOffset + 8 > virtualSize) return 0;
  uint8_t sh[8];
  if (mm::SafePeek(C + shaderOffset, sh, 8) != 8) return 0;
  const uint32_t physicalOffset = BeRead32(sh + 0);
  const uint32_t ucodeSize = BeRead32(sh + 4);
  if (ucodeSize == 0 || (ucodeSize % 4) != 0 || ucodeSize > physicalSize) return 0;
  if (physicalOffset > physicalSize - ucodeSize) return 0;
  const uint8_t* ucode = C + size_t(virtualSize) + physicalOffset;
  uint64_t fnv;
  __try {
    fnv = FnvUcode(ucode, ucodeSize / 4);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
  return fnv;
}
}  // namespace

void GetUcodeIndexStats(size_t& totalOut, size_t& vsOut, size_t& psOut) {
  totalOut = g_ucodeIndex.size();
  vsOut = g_indexVS;
  psOut = g_indexPS;
}

const uint8_t* GetIndexedContainer(bool wantPixel) {
  for (const auto& kv : g_ucodeIndex) {
    if (kv.second.isPixel == wantPixel) return kv.second.container;
  }
  return nullptr;
}

size_t GetIndexedContainers(bool wantPixel, const uint8_t** out, size_t maxOut) {
  size_t n = 0;
  for (const auto& kv : g_ucodeIndex) {
    if (kv.second.isPixel != wantPixel) continue;
    if (n >= maxOut) break;
    out[n++] = kv.second.container;
  }
  return n;
}

// Find a container whose ucode is a PREFIX or SUFFIX of `ucode` (or vice versa),
// for diagnosing why an exact FNV match failed (e.g. a vertex-fetch preamble). Sets
// the matched container's ucode dword length + the relationship. Returns true if any
// related container was found. Slow (iterates the index); diagnostic only.
bool DiagnoseUcodeMatch(const uint8_t* ucode, uint32_t dwordCount, uint32_t& matchedDwordsOut,
                        int& relationOut, bool wantPixel) {
  matchedDwordsOut = 0;
  relationOut = 0;
  for (const auto& kv : g_ucodeIndex) {
    if (kv.second.isPixel != wantPixel) continue;  // compare same stage only
    const uint8_t* C = kv.second.container;
    uint8_t hdr[0x20];
    if (mm::SafePeek(C, hdr, sizeof(hdr)) != sizeof(hdr)) continue;
    if ((BeRead32(hdr + 0x00) & 0xFFFFFF00u) != 0x102A1100u) continue;  // stale/garbage -> skip
    const uint32_t virtualSize = BeRead32(hdr + 0x04);
    const uint32_t physicalSize = BeRead32(hdr + 0x08);
    const uint32_t shaderOffset = BeRead32(hdr + 0x18);
    if (uint64_t(virtualSize) + physicalSize > (256u * 1024u) || shaderOffset + 8 > virtualSize)
      continue;
    uint8_t sh[8];
    if (mm::SafePeek(C + shaderOffset, sh, 8) != 8) continue;
    const uint32_t physicalOffset = BeRead32(sh + 0);
    const uint32_t cSize = BeRead32(sh + 4);
    if (cSize == 0 || (cSize % 4) != 0 || cSize > physicalSize || physicalOffset > physicalSize - cSize)
      continue;
    const uint8_t* cu = C + size_t(virtualSize) + physicalOffset;
    const uint32_t cDwords = cSize / 4;
    const uint32_t n = cDwords < dwordCount ? cDwords : dwordCount;
    if (n < 4) continue;
    // compare first n dwords (prefix) and last n dwords (suffix), SEH-guarded
    bool prefix = false, suffix = false;
    __try {
      prefix = std::memcmp(cu, ucode, size_t(n) * 4) == 0;
      suffix = std::memcmp(cu + (cDwords - n) * 4, ucode + (dwordCount - n) * 4,
                           size_t(n) * 4) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      continue;
    }
    if (prefix || suffix) {
      matchedDwordsOut = cDwords;
      relationOut = prefix ? (cDwords == dwordCount ? 1 : 2) : 3;  // 1=exact,2=prefix,3=suffix
      return true;
    }
  }
  return false;
}

namespace {
void ScanAndIndex(const uint8_t* base, uint32_t startAddr, uint32_t endAddr) {
  const uint8_t* lo = base + startAddr;
  const uint8_t* hi = base + endAddr;
  const DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  const uint8_t* addr = lo;
  MEMORY_BASIC_INFORMATION mbi;
  while (addr < hi && VirtualQuery(addr, &mbi, sizeof(mbi)) != 0) {
    const uint8_t* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
    const uint8_t* rend = rbase + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && (mbi.Protect & kReadable) && !(mbi.Protect & PAGE_GUARD)) {
      const uint8_t* s = rbase < lo ? lo : rbase;
      const uint8_t* e = rend > hi ? hi : rend;
      const uint8_t* p = s;
      while (p + 36 < e) {
        const uint8_t* m = static_cast<const uint8_t*>(std::memchr(p, 0x10, size_t(e - 36 - p)));
        if (!m) break;
        if (m[1] == 0x2A && m[2] == 0x11 && m[3] <= 1) {
          IndexContainer(m);
        }
        p = m + 1;
      }
    }
    addr = rend > addr ? rend : addr + 0x1000;
  }
}
}  // namespace

size_t BuildUcodeIndex(const uint8_t* base, uint32_t startAddr, uint32_t endAddr) {
  if (g_ucodeIndexBuilt) return g_ucodeIndex.size();
  g_ucodeIndexBuilt = true;
  InitShaderCache();
  ScanAndIndex(base, startAddr, endAddr);
  return g_ucodeIndex.size();
}

size_t RebuildUcodeIndex(const uint8_t* base, uint32_t startAddr, uint32_t endAddr) {
  // Containers load over time; rescan (additive — emplace keeps existing entries).
  g_ucodeIndexBuilt = true;
  InitShaderCache();
  ScanAndIndex(base, startAddr, endAddr);
  return g_ucodeIndex.size();
}

const uint8_t* LookupContainerByUcodeHash(uint64_t ucodeFnvHash, uint64_t& containerHashOut,
                                          bool& isPixelOut, bool& containerFoundOut) {
  containerHashOut = 0;
  isPixelOut = false;
  containerFoundOut = false;
  auto it = g_ucodeIndex.find(ucodeFnvHash);
  if (it == g_ucodeIndex.end()) return nullptr;
  // Reject a STALE pointer: if the container's ucode no longer hashes to the requested value,
  // the guest reused that memory -> drop the entry and report a miss (the caller re-scans).
  if (ComputeContainerUcodeFnv(it->second.container) != ucodeFnvHash) {
    g_ucodeIndex.erase(it);
    return nullptr;
  }
  containerFoundOut = true;
  isPixelOut = it->second.isPixel;
  containerHashOut = it->second.containerHash;
  return it->second.container;
}

size_t ScanForShaders(const uint8_t* base, uint32_t startAddr, uint32_t endAddr) {
  InitShaderCache();
  size_t found = 0, matched = 0;
  FILE* f = std::fopen("mm_shadercache.log", "a");

  // Walk only committed, readable regions via VirtualQuery — skips the vast
  // unmapped gaps in the 4GB guest reservation instantly (no page faults).
  const uint8_t* lo = base + startAddr;
  const uint8_t* hi = base + endAddr;
  const DWORD kReadable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                          PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
  const uint8_t* addr = lo;
  MEMORY_BASIC_INFORMATION mbi;
  while (addr < hi && VirtualQuery(addr, &mbi, sizeof(mbi)) != 0) {
    const uint8_t* rbase = static_cast<const uint8_t*>(mbi.BaseAddress);
    const uint8_t* rend = rbase + mbi.RegionSize;
    if (mbi.State == MEM_COMMIT && (mbi.Protect & kReadable) && !(mbi.Protect & PAGE_GUARD)) {
      const uint8_t* s = rbase < lo ? lo : rbase;
      const uint8_t* e = rend > hi ? hi : rend;
      // Fast: memchr for the first magic byte (0x10), then verify the rest.
      const uint8_t* p = s;
      while (p + 36 < e) {
        const uint8_t* m = static_cast<const uint8_t*>(std::memchr(p, 0x10, size_t(e - 36 - p)));
        if (!m) break;
        if (m[1] == 0x2A && m[2] == 0x11 && m[3] <= 1) {
          ++found;
          uint32_t dxilSize = 0, mask = 0;
          uint64_t hash = 0;
          bool isPixel = false;
          if (TryShaderContainerLookup(m, dxilSize, mask, hash, isPixel)) {
            ++matched;
            if (matched <= 8 && f)
              std::fprintf(f, "  MATCH guestaddr=0x%08X hash=%016llX %s dxil=%u\n",
                           uint32_t(m - base), (unsigned long long)hash, isPixel ? "PS" : "VS",
                           dxilSize);
          }
        }
        p = m + 1;
      }
    }
    addr = rend > addr ? rend : addr + 0x1000;
  }
  if (f) {
    std::fprintf(f, "scan [0x%08X,0x%08X): found %zu container-magics, %zu matched cache\n",
                 startAddr, endAddr, found, matched);
    std::fclose(f);
  }
  return matched;
}

}  // namespace mm
