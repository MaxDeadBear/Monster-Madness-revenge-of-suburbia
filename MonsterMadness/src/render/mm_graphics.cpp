// See mm_graphics.h.

#include "render/mm_graphics.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/pipeline/shader/dxbc_translator.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/pipeline/shader/translator.h>
#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/system/xmemory.h>  // memory::Memory::virtual_membase()
#include <rex/string/buffer.h>
#include <rex/ui/graphics_provider.h>

#include "render/shader_cache_runtime.h"
#include "render/shader_translate.h"  // TranslateXenosShaderToDxil
#include "render/video.h"

namespace mm {
namespace {

// Raw, spdlog-independent trace for the native draw stream (the SDK file logger
// writes empty files in this config). Plain flushed file next to the exe.
void DrawTracef(const char* fmt, ...) {
  static FILE* f = std::fopen("mm_draws_trace.log", "w");
  static std::mutex m;
  if (!f) return;
  std::lock_guard<std::mutex> lk(m);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fflush(f);
}

void ShaderTracef(const char* fmt, ...) {
  static FILE* f = std::fopen("mm_shaders_trace.log", "w");
  static std::mutex m;
  if (!f) return;
  std::lock_guard<std::mutex> lk(m);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fflush(f);
}

// Diagnostic-only geometry dump (gated by env MM_DUMP_GEO). Separate file so the
// per-draw vertex/format detail doesn't drown the normal trace. Used to chase the
// torn-mesh problem: dumps binding count (multi-stream?), each attribute's Xenos
// vertex format + offset/stride, and decoded POSITION values/bbox for a few live draws.
void GeoDumpf(const char* fmt, ...) {
  static FILE* f = std::fopen("mm_geo_dump.log", "w");
  static std::mutex m;
  if (!f) return;
  std::lock_guard<std::mutex> lk(m);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fflush(f);
}

// Human-readable Xenos vertex format name (xenos::VertexFormat values).
const char* VertexFormatName(uint32_t f) {
  switch (f) {
    case 0:  return "kUndefined";
    case 6:  return "k_8_8_8_8";
    case 7:  return "k_2_10_10_10";
    case 16: return "k_10_11_11";
    case 17: return "k_11_11_10";
    case 25: return "k_16_16";
    case 26: return "k_16_16_16_16";
    case 31: return "k_16_16_FLOAT";
    case 32: return "k_16_16_16_16_FLOAT";
    case 33: return "k_32";
    case 34: return "k_32_32";
    case 35: return "k_32_32_32_32";
    case 36: return "k_32_FLOAT";
    case 37: return "k_32_32_FLOAT";
    case 38: return "k_32_32_32_32_FLOAT";
    case 57: return "k_32_32_32_FLOAT";
    default: return "k_?";
  }
}

// Mirrors video.cpp XenosVertexFormatToRender's coverage: which Xenos vertex formats
// have a real DXGI mapping. UNMAPPED formats fall back to a R32G32B32_FLOAT@packed GUESS
// in the IA layout — wrong bytes/stride => corrupt positions. Prime tear suspect.
bool IsVertexFormatMapped(uint32_t f) {
  switch (f) {
    case 6: case 25: case 26: case 31: case 32: case 33:
    case 34: case 35: case 36: case 37: case 38: case 57:
      return true;
    default:  // 7 (k_2_10_10_10), 16 (k_10_11_11), 17 (k_11_11_10), ... -> fallback guess
      return false;
  }
}

// The HLSL prelude XenosRecomp output is prepended with (shader_common.h), for
// runtime translation of containers not in the precompiled cache.
const std::string& ShaderInclude() {
  static std::string include = [] {
    std::ifstream f(std::string(MONSTER_MADNESS_SOURCE_ROOT) +
                        "/thirdparty/XenosRecomp/XenosRecomp/shader_common.h",
                    std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), {}) : std::string();
  }();
  return include;
}

// A DXBC blob is usable in a graphics PSO only if it carries an input-signature chunk
// (ISG1/ISGN). The precompiled cache was built Unleashed-style as DXIL LIBRARIES (chunks
// SFI0/VERS/RDAT, no ISGN) for per-variant linking — those can't be a graphics VS/PS
// (CreateGraphicsPipelineState -> E_INVALIDARG). Detect that so we fall back to a runtime
// ps_6_0/vs_6_0 translate instead of feeding a library to the PSO.
bool IsGraphicsDxil(const uint8_t* d, uint32_t size) {
  if (!d || size < 36 || std::memcmp(d, "DXBC", 4) != 0) return false;
  uint32_t cc;
  std::memcpy(&cc, d + 28, 4);
  for (uint32_t i = 0; i < cc; ++i) {
    if (uint64_t(32) + uint64_t(i) * 4 + 4 > size) break;
    uint32_t off;
    std::memcpy(&off, d + 32 + i * 4, 4);
    if (uint64_t(off) + 4 > size) continue;
    if (std::memcmp(d + off, "ISG1", 4) == 0 || std::memcmp(d + off, "ISGN", 4) == 0) return true;
  }
  return false;
}

uint64_t HashUcode(const uint32_t* dwords, uint32_t count) {
  uint64_t h = 1469598103934665603ull;  // FNV-1a 64
  for (uint32_t i = 0; i < count; ++i) {
    h = (h ^ dwords[i]) * 1099511628211ull;
  }
  return h;
}

// A CommandProcessor that inherits the full ring-buffer pump, MMIO/write-pointer
// handling, fence/EVENT_WRITE servicing, completion interrupts and read-pointer
// write-back from the base class, but performs no rendering. This is exactly the
// bookkeeping the guest blocks on; rendering is intentionally a no-op for now.
class MMNullCommandProcessor : public rex::graphics::CommandProcessor {
 public:
  using rex::graphics::CommandProcessor::CommandProcessor;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override {
    // Frame boundary (CP worker thread): report what the guest issued this frame,
    // present our Plume frame, then reset.
    DrawTracef("[mm-draw] SWAP frontbuffer=0x%08X %ux%u  (frame had %u draws)\n", frontbuffer_ptr,
               frontbuffer_width, frontbuffer_height, draws_this_frame_);
    if (!std::getenv("MM_GUEST_RENDER")) Video::Present();  // else guest thread presents
    if (geo_draws_this_frame_ > 0) {
      ++geo_frames_dumped_;
      GeoDumpf("==== end geo-dump frame %u (%u draws total) ====\n\n", geo_frames_dumped_,
               draws_this_frame_);
    }
    prev_frame_draws_ = draws_this_frame_;
    draws_this_frame_ = 0;
    traced_this_frame_ = 0;
    geo_draws_this_frame_ = 0;
  }
  void TracePlaybackWroteMemory(uint32_t /*base_ptr*/, uint32_t /*length*/) override {}
  void RestoreEdramSnapshot(const void* /*snapshot*/) override {}

 protected:
  // Runs on the CP "GPU Commands" worker thread before the ring is drained. This is
  // the correct thread to create + own all Plume objects (device/swapchain/RT), so
  // that IssueDraw/IssueSwap can record + present on the same thread.
  bool SetupContext() override {
    // In guest-thread render mode the D3D hooks own Video (init + present on the
    // guest thread); the CP must not touch Plume.
    if (std::getenv("MM_GUEST_RENDER")) {
      DrawTracef("[mm-draw] MM_GUEST_RENDER: Video owned by guest-thread D3D hooks\n");
      return true;
    }
    if (Video::Init()) {
      DrawTracef("[mm-draw] Video (Plume) initialized on CP worker thread\n");
      const bool ptest = Video::SelfTestPipeline();
      DrawTracef("[mm-draw] pipeline self-test: %s (see mm_video.log)\n", ptest ? "PASS" : "see-log");
    } else {
      DrawTracef("[mm-draw] Video init FAILED (continuing without present)\n");
    }
    return true;
  }
  void ShutdownContext() override { Video::Shutdown(); }

  // Capture, cache, and analyze each Xenos shader the game binds. Returns a real
  // analyzed Shader (the base CP tracks it as the active shader). This is the
  // shader-translation entry point: we now hold the parsed ucode, vertex/texture
  // bindings, and Xenos disassembly that a host shader must reproduce. (DXBC/
  // SPIR-V emission via the SDK translator is the next sub-step.)
  rex::graphics::Shader* LoadShader(rex::graphics::xenos::ShaderType shader_type,
                                    uint32_t guest_address, const uint32_t* host_address,
                                    uint32_t dword_count) override {
    const uint64_t hash = HashUcode(host_address, dword_count);
    auto it = shaders_.find(hash);
    if (it != shaders_.end()) {
      return it->second;
    }

    auto* shader = new rex::graphics::Shader(shader_type, hash, host_address, dword_count);
    rex::string::StringBuffer disasm;
    shader->AnalyzeUcode(disasm);
    shaders_.emplace(hash, shader);

    const bool is_vertex = shader_type == rex::graphics::xenos::ShaderType::kVertex;
    ShaderTracef("[mm-shader] #%zu %s hash=%016llX dwords=%u vtx_bindings=%zu tex_bindings=%zu\n",
                 shaders_.size(), is_vertex ? "VERTEX" : "PIXEL", static_cast<unsigned long long>(hash),
                 dword_count, shader->vertex_bindings().size(), shader->texture_bindings().size());

    // Resolve this bound shader to DXIL (task #6). The GPU's ucode is copied away from
    // its rich container, so we match by ucode content: index all containers in guest
    // RAM by ucode FNV hash, find the container for `hash`, then take its precompiled
    // DXIL from the cache or runtime-translate it (covers shaders not in the cache).
    ResolvedShader rs{};
    const char* source = "no-container";
    if (memory_) {
      const uint8_t* mem = memory_->virtual_membase();
      mm::BuildUcodeIndex(mem, 0x10000u, 0xF0000000u);
      const uint8_t* container =
          mm::LookupContainerByUcodeHash(hash, rs.containerHash, rs.isPixel, rs.containerFound);
      if (!rs.containerFound && index_rebuilds_ < 32) {
        ++index_rebuilds_;  // containers may have loaded since the last scan
        mm::RebuildUcodeIndex(mem, 0x10000u, 0xF0000000u);
        container =
            mm::LookupContainerByUcodeHash(hash, rs.containerHash, rs.isPixel, rs.containerFound);
      }
      if (container) {
        uint32_t mask = 0;
        rs.cacheDxil = mm::LookupShaderDxil(rs.containerHash, rs.dxilSize, mask);
        // Only use the cache if it's a graphics shader; the cache holds DXIL LIBRARIES which
        // are unusable in a PSO -> fall back to a runtime ps_6_0/vs_6_0 translate.
        if (rs.cacheDxil && IsGraphicsDxil(rs.cacheDxil, rs.dxilSize)) {
          source = "cache";
        } else {
          rs.cacheDxil = nullptr;
          rs.dxilSize = 0;
          bool isPx = false;
          rs.translated = mm::TranslateXenosShaderToDxil(container, ShaderInclude(), isPx,
                                                         &rs.vertexLayout, &rs.vsInterp);
          if (!rs.translated.empty()) {
            rs.dxilSize = static_cast<uint32_t>(rs.translated.size());
            source = "xlat";
          } else {
            source = "xlat-FAIL";
          }
        }
      } else if (shader_type == rex::graphics::xenos::ShaderType::kVertex) {
        // Specialized-variant VS: the GPU runs ucode whose hash matches no container, but
        // a TEMPLATE container (stub ucode + the real vertex-element/constant tables) does
        // exist. Find that template (its vertex elements resolve against THIS ucode's
        // vfetch instructions), then splice the real ucode into it and translate. Probe is
        // recompile-only (no DXC) so scanning all templates is cheap; one DXC compile runs
        // for the chosen template (same cost as a normal translate, no burst).
        const uint8_t* vsC[256];
        const size_t nC = mm::GetIndexedContainers(/*wantPixel=*/false, vsC, 256);
        const uint8_t* ub = reinterpret_cast<const uint8_t*>(host_address);
        const uint32_t nbytes = (dword_count - dword_count % 3) * 4;  // XenosRecomp needs %12==0
        // Pick the template whose recompiled layout REPRODUCES the authoritative vfetch layout
        // (the SDK's vertex_bindings: each attribute's real Xenos format + dword offset), NOT just
        // the attribute COUNT. A count-only match can pick a template that mis-maps the
        // vertex-element semantics so the POSITION vfetch is misread/dropped -> the VS computes
        // oPos from a register never loaded from input -> oPos=(0,0,0,0) -> w=0 ->
        // perspective-divide-by-zero -> screen-spanning torn triangles. Reproducing the
        // float3/float4 position attribute (format 57=k_32_32_32_FLOAT / 38=k_32_32_32_32_FLOAT)
        // is the decisive signal that the position vfetch was read correctly, so it dominates.
        struct AuthAttr { uint32_t format; uint32_t offset; };
        std::vector<AuthAttr> sdkAttrs;
        for (const auto& b : shader->vertex_bindings())
          for (const auto& a : b.attributes)
            sdkAttrs.push_back({uint32_t(a.fetch_instr.attributes.data_format),
                                uint32_t(a.fetch_instr.attributes.offset)});
        const int expected = static_cast<int>(sdkAttrs.size());
        auto isFloatPos = [](uint32_t fmt) { return fmt == 57u || fmt == 38u; };
        const uint8_t* bestTpl = nullptr;
        long bestScore = -1;
        int bestPhantom = 1 << 30, bestMatched = 0, bestFloat = 0;
        std::vector<mm::VertexInputAttr> probe;
        if (expected == 0) {
          // 0-binding fullscreen/post-process VS (e.g. the title backdrop pass D84D879E, which
          // otherwise falls through to "no-container" and is skipped -> black backdrop). It has no
          // vertex inputs to map, and oPos is auto-detected from the ucode export, so ANY template
          // that recompiles to a no-input VS works. Pick the first such template.
          for (size_t i = 0; i < nC; ++i)
            if (mm::ProbeSplicedVsAttrs(vsC[i], ub, nbytes, ShaderInclude(), &probe) == 0) {
              bestTpl = vsC[i];
              break;
            }
        } else
        for (size_t i = 0; i < nC; ++i) {
          const int attrs = mm::ProbeSplicedVsAttrs(vsC[i], ub, nbytes, ShaderInclude(), &probe);
          if (attrs <= 0) continue;
          int matched = 0, floatMatched = 0;
          for (const auto& sa : sdkAttrs) {
            bool hit = false;
            for (const auto& pa : probe)
              if (pa.format == sa.format && pa.offsetDwords == sa.offset) { hit = true; break; }
            if (hit) { ++matched; if (isFloatPos(sa.format)) ++floatMatched; }
          }
          const long score = long(matched) + 1000L * long(floatMatched);
          const int delta = int(probe.size()) - expected;
          const int phantom = delta < 0 ? -delta : delta;  // prefer no phantom/missing attrs
          if (score > bestScore || (score == bestScore && phantom < bestPhantom)) {
            bestScore = score; bestPhantom = phantom; bestTpl = vsC[i];
            bestMatched = matched; bestFloat = floatMatched;
          }
          if (matched == expected && attrs == expected) break;  // confident full layout match
        }
        if (bestTpl) {
          bool isPx = false;
          rs.translated = mm::TranslateSplicedShaderToDxil(bestTpl, ub, nbytes, ShaderInclude(),
                                                           isPx, &rs.vertexLayout, &rs.vsInterp);
          if (!rs.translated.empty()) {
            rs.dxilSize = static_cast<uint32_t>(rs.translated.size());
            rs.containerFound = true;  // resolved via splice
            source = "splice";
            ShaderTracef("[mm-splice] vkey=%016llX matched=%d/%d floatPos=%d score=%ld\n",
                         static_cast<unsigned long long>(hash), bestMatched, expected,
                         bestFloat, bestScore);
          } else {
            source = "splice-FAIL";
          }
        } else {
          source = "no-template";
        }
      } else if (shader_type == rex::graphics::xenos::ShaderType::kPixel) {
        // Specialized-variant PS that matches no container. Unlike the VS case there is no
        // template to splice, so we synthesize a minimal container around the variant ucode. But
        // the PS's interpolator table (which VS-exported semantic each PS register reads) must
        // match the COMPANION VERTEX SHADER, which we only know at draw time. So DEFER the synth:
        // mark pending and build the DXIL in IssueDraw once the active VS pair is known.
        rs.pendingSynth = true;
        rs.containerFound = true;  // treat as resolved; DXIL is filled at first draw
        source = "synth-ps(deferred)";
      }
    }
    // Capture before the move (operator[] would insert an empty entry and discard rs).
    const bool haveDxil = rs.dxil() != nullptr;
    const bool found = rs.containerFound;
    const uint32_t dxilSize = rs.dxilSize;
    resolved_[hash] = std::move(rs);  // store the real resolution
    ShaderTracef("[mm-resolve]   container=%s dxil=%s(%s) size=%u\n", found ? "FOUND" : "miss",
                 haveDxil ? "YES" : "no", source, dxilSize);

    // Diagnose why a miss happened: index coverage only. (Removed DiagnoseUcodeMatch — it
    // read each indexed container's header without validating the 0x102A11 magic, and by the
    // movie->menu transition those indexed pointers are STALE: the game freed/reloaded that
    // guest memory, so the header gives garbage virtualSize/physicalOffset -> a wild
    // dereference that crashed at ~29s. The splice path validates the magic, so it's safe.)
    if (!found && diag_logged_ < 20) {
      ++diag_logged_;
      size_t total = 0, vs = 0, ps = 0;
      mm::GetUcodeIndexStats(total, vs, ps);
      // Does a same-stage base container's ucode prefix/suffix-match this bound (variant) ucode?
      // A strong match => a splice target exists (the GPU runs a specialized variant of a base
      // container, like the VS case). relation: 1=exact 2=container-prefix 3=container-suffix.
      uint32_t matched = 0;
      int relation = 0;
      const bool related =
          mm::DiagnoseUcodeMatch(reinterpret_cast<const uint8_t*>(host_address), dword_count,
                                 matched, relation, /*wantPixel=*/!is_vertex);
      ShaderTracef("[mm-diag]   %s dwords=%u idx{vs=%zu ps=%zu} match=%s rel=%d cDwords=%u\n",
                   is_vertex ? "VERTEX" : "PIXEL", dword_count, vs, ps, related ? "YES" : "no",
                   relation, matched);
    }
    // Dump full Xenos disassembly for the first few unique shaders as evidence.
    if (shaders_.size() <= 4) {
      ShaderTracef("---- ucode disassembly (hash %016llX) ----\n%s\n----\n",
                   static_cast<unsigned long long>(hash), shader->ucode_disassembly().c_str());
    }
    return shader;
  }

  // Bind the REAL guest geometry for this draw and record it. The vertex buffer comes
  // from the vertex FETCH CONSTANT (the GPU register block) named by the VS's first
  // vertex binding: address (in dwords) + size (in words) + the binding's stride. For an
  // indexed draw the index buffer comes from index_buffer_info. Falls back to a degenerate
  // draw if the binding/fetch is unusable. (Constants + textures are still defaults; this
  // is the geometry step.)
  // Dump one draw's geometry (gated by MM_DUMP_GEO) to chase torn meshes. For a torn sign
  // draw vs a clean character draw, compare: binding count (multi-stream lives in a 2nd
  // binding we ignore), every attribute's Xenos vertex format + offset/stride, whether the
  // POSITION format is MAPPED (else the IA reads a wrong fallback format -> corrupt position),
  // the decoded POSITION of the first verts + bbox, and the index range.
  void DumpDrawGeometry(rex::graphics::Shader* vshader, uint64_t vkey, uint32_t prim,
                        uint32_t index_count, IndexBufferInfo* ib, const uint8_t* vbHost,
                        uint32_t vbAvail, uint32_t stride) {
    auto beF32 = [](const uint8_t* p) -> float {
      const uint32_t u =
          (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
      float f;
      std::memcpy(&f, &u, sizeof(f));
      return f;
    };

    const auto& bindings = vshader->vertex_bindings();
    // Which texture is bound (first PS fetch) — helps correlate a dump line to the on-screen
    // mesh (e.g. the yellow sign texture vs a character skin).
    uint32_t tex0Base = 0;
    if (auto* ps = active_pixel_shader()) {
      const auto& tbs = ps->texture_bindings();
      if (!tbs.empty())
        tex0Base = uint32_t(register_file_->GetTextureFetch(tbs.front().fetch_constant).base_address)
                   << 12;
    }
    GeoDumpf("---- draw#%u vkey=%016llX prim=%u idx=%u %s stride=%u vbAvail=%u bindings=%zu tex0=0x%08X\n",
             geo_draws_this_frame_, (unsigned long long)vkey, prim, index_count,
             ib ? "INDEXED" : "auto", stride, vbAvail, bindings.size(), tex0Base);

    // Per-binding fetch + raw attribute formats (the unjoined source of truth).
    for (size_t bi = 0; bi < bindings.size(); ++bi) {
      const auto& b = bindings[bi];
      const auto f = register_file_->GetVertexFetch(b.fetch_constant);
      GeoDumpf("  binding%zu: fetch=%u stride_words=%u addr=0x%08X size=%u endian=%u attrs=%zu\n", bi,
               b.fetch_constant, b.stride_words, f.address << 2, f.size << 2, uint32_t(f.endian),
               b.attributes.size());
      for (const auto& a : b.attributes) {
        const auto& at = a.fetch_instr.attributes;
        const uint32_t fmt = uint32_t(at.data_format);
        GeoDumpf("      attr fmt=%u(%s) off=%d(dw) stride=%u(dw) signed=%d int=%d expadj=%d %s\n", fmt,
                 VertexFormatName(fmt), at.offset, at.stride, at.is_signed ? 1 : 0,
                 at.is_integer ? 1 : 0, at.exp_adjust,
                 IsVertexFormatMapped(fmt) ? "MAPPED" : "UNMAPPED->fallback-guess");
      }
    }

    // The semantic-joined layout the IA actually used (usage 0 = Position).
    uint32_t posOff = 0, posFmt = 0xFFFFFFFFu;
    bool havePos = false;
    auto it = resolved_.find(vkey);
    if (it != resolved_.end() && !it->second.vertexLayout.empty()) {
      for (const auto& a : it->second.vertexLayout) {
        GeoDumpf("  layout: usage=%u idx=%u fmt=%u(%s) off=%u(dw) stride=%u(dw) %s\n", a.usage,
                 a.usageIndex, a.format, VertexFormatName(a.format), a.offsetDwords, a.strideDwords,
                 IsVertexFormatMapped(a.format) ? "MAPPED" : "UNMAPPED->fallback-guess");
        if (a.usage == 0 && !havePos) {  // 0 = Position
          posOff = a.offsetDwords * 4;
          posFmt = a.format;
          havePos = true;
        }
      }
    }

    if (!vbHost || stride == 0) {
      GeoDumpf("  (no VB host/stride)\n\n");
      return;
    }
    const uint32_t vbVerts = vbAvail ? (vbAvail / stride) : index_count;

    // Hex of the first 2 whole vertices, so ANY format can be decoded by hand.
    for (uint32_t v = 0; v < 2 && v < vbVerts; ++v) {
      char hex[160];
      int n = 0;
      const uint32_t bytes = std::min<uint32_t>(stride, 48);
      for (uint32_t k = 0; k < bytes; ++k)
        n += std::snprintf(hex + n, sizeof(hex) - n, "%02X", vbHost[size_t(v) * stride + k]);
      GeoDumpf("  v%u raw[%u]: %s\n", v, bytes, hex);
    }

    // Decode POSITION as big-endian float3 (exact for k_32_32_32_FLOAT; a hint otherwise — the
    // hex + format name above pin down non-float formats) plus a bounding box over the verts.
    if (havePos) {
      const bool isF3 = (posFmt == 57 || posFmt == 38);  // k_32_32_32(_32)_FLOAT
      GeoDumpf("  POSITION off=%u fmt=%u(%s) %s\n", posOff, posFmt, VertexFormatName(posFmt),
               IsVertexFormatMapped(posFmt) ? "MAPPED" : "UNMAPPED->fallback-guess");
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      const uint32_t cap = std::min<uint32_t>(vbVerts, 2048);
      for (uint32_t v = 0; v < cap; ++v) {
        const uint8_t* p = vbHost + size_t(v) * stride + posOff;
        if (uint32_t((p - vbHost) + 12) > vbAvail) break;
        for (int c = 0; c < 3; ++c) {
          const float val = beF32(p + c * 4);
          mn[c] = std::min(mn[c], val);
          mx[c] = std::max(mx[c], val);
        }
        if (v < 6)
          GeoDumpf("    v%u pos(beF32)= %.4f %.4f %.4f\n", v, beF32(p), beF32(p + 4), beF32(p + 8));
      }
      GeoDumpf("  POS bbox(beF32) min=(%.3f %.3f %.3f) max=(%.3f %.3f %.3f) over %u verts %s\n", mn[0],
               mn[1], mn[2], mx[0], mx[1], mx[2], cap, isF3 ? "" : "(non-float3: hint only)");
    }

    // Index range — degenerate/huge indices => out-of-range vertex fetch => tears/black gaps.
    if (ib && ib->guest_base && ib->count && memory_) {
      const uint8_t* ibHost = memory_->TranslatePhysical<const uint8_t*>(ib->guest_base);
      const bool ib32 = ib->format == rex::graphics::xenos::IndexFormat::kInt32;
      char idx[200];
      int n = 0;
      const uint32_t showN = std::min<uint32_t>(ib->count, 18);
      uint32_t mnI = 0xFFFFFFFFu, mxI = 0;
      for (uint32_t i = 0; i < ib->count; ++i) {
        const uint8_t* p = ibHost + size_t(i) * (ib32 ? 4 : 2);
        const uint32_t v = ib32 ? ((uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                                   (uint32_t(p[2]) << 8) | uint32_t(p[3]))
                                : ((uint32_t(p[0]) << 8) | uint32_t(p[1]));
        mnI = std::min(mnI, v);
        mxI = std::max(mxI, v);
        if (i < showN) n += std::snprintf(idx + n, sizeof(idx) - n, "%u ", v);
      }
      GeoDumpf("  idx[%u %s]: %s... range=[%u,%u] (vbVerts=%u)\n", ib->count, ib32 ? "u32" : "u16",
               idx, mnI, mxI, vbVerts);
    }
    GeoDumpf("\n");
  }

  void RecordRealDraw(rex::graphics::Shader* vshader, uint64_t vkey, uint64_t pkey, uint32_t prim,
                      uint32_t index_count, IndexBufferInfo* ib, uint32_t depthState) {
    // Take a fresh per-draw resource slot (own VB/IB + b0/b1/b2 + descriptor set) so this
    // draw's geometry/constants/textures don't clobber other draws in the same frame.
    Video::BeginGuestDraw();

    const auto& bindings = vshader->vertex_bindings();
    if (bindings.empty() || !memory_) {
      Video::RecordGuestDraw(vkey, pkey, prim, index_count, index_count, depthState);
      return;
    }
    // Single interleaved stream (slot 0) — what the spliced movie/UI shaders use. The
    // layout packs every attribute into one binding; multi-stream is future work.
    const auto& b0 = bindings.front();
    const rex::graphics::xenos::xe_gpu_vertex_fetch_t fetch =
        register_file_->GetVertexFetch(b0.fetch_constant);
    const uint32_t vbByte = fetch.address << 2;     // address is in dwords
    const uint32_t vbAvail = fetch.size << 2;       // size is in words (dwords)
    const uint32_t stride = b0.stride_words * 4;
    if (vbByte == 0 || stride == 0) {
      Video::RecordGuestDraw(vkey, pkey, prim, index_count, index_count, depthState);
      return;
    }

    // Fetch-constant addresses are guest PHYSICAL addresses (a different host base than the
    // virtual membase). TranslatePhysical maps them (& 0x1FFFFFFF + physical_membase_).
    const uint8_t* vbHost = memory_->TranslatePhysical<const uint8_t*>(vbByte);

    // Real Xenos float constants -> b0/b1. The 512-entry float register file is split
    // DX9-style: VS reads c0-c255, PS reads c256-c511 (XenosRecomp offsets PS by 256).
    // Values are already host-endian in the register file, so no byte swap.
    {
      const uint32_t* cregs = &(*register_file_)[rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X];
      Video::SetDrawConstants(cregs, 256, cregs + 256 * 4, 224);
    }

    // Viewport-transform (VTE) emulation. When the guest disabled the Xenos viewport scale
    // (PA_CL_VTE_CNTL bits: 0=VPORT_X_SCALE_ENA, 2=VPORT_Y_SCALE_ENA), the VS emits window-space
    // xy and the GPU skips NDC->screen; our host always does NDC->screen, so convert window->NDC
    // in the VS via g_ViewportScaleOffset: ndc = win*(1/scale) + (-offset/scale). Identity when
    // the guest transform was enabled (oPos already NDC). (Full-RT viewport assumed.)
    {
      const auto& rf = *register_file_;
      const uint32_t vte = rf[rex::graphics::XE_GPU_REG_PA_CL_VTE_CNTL];
      const float xs = rf.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE);
      const float xo = rf.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XOFFSET);
      const float ys = rf.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YSCALE);
      const float yo = rf.Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YOFFSET);
      const bool xEna = (vte & 0x1u) != 0;
      const bool yEna = (vte & 0x4u) != 0;
      const float sx = (xEna || xs == 0.0f) ? 1.0f : 1.0f / xs;
      const float bx = (xEna || xs == 0.0f) ? 0.0f : -xo / xs;
      const float sy = (yEna || ys == 0.0f) ? 1.0f : 1.0f / ys;
      const float by = (yEna || ys == 0.0f) ? 0.0f : -yo / ys;
      Video::SetViewportTransform(sx, sy, bx, by);
      // Host viewport = the guest viewport rect (x=xo-xs, y=yo+ys, w=2*xs, h=-2*ys). With the
      // window->NDC conversion above this places window-space correctly for both VTE modes, and
      // stops sub-viewport draws (854x854 portrait, 1024x256 banner, ...) from blowing up to
      // full-screen and overlapping. Gated by MM_NO_VP to A/B test.
      static const bool kNoVp = std::getenv("MM_NO_VP") != nullptr;
      if (!kNoVp && xs > 0.0f && ys != 0.0f)
        Video::SetGuestViewport(xo - xs, yo + ys, 2.0f * xs, -2.0f * ys);
      // Log EVERY DISTINCT viewport state seen on live (title) frames, so a sub-viewport that
      // clips/mis-scales the missing planes shows up (the "cutoff planes" the splash had).
      // D3D: xs=W/2, xo=X+W/2; ys=-H/2, yo=Y+H/2 -> recover the guest viewport rect.
      {
        static std::unordered_set<uint64_t> seenVp;
        const uint64_t key = (uint64_t(vte) << 40) ^ (uint64_t(int32_t(xs)) << 20) ^
                             (uint64_t(int32_t(xo)) << 10) ^ uint64_t(uint16_t(int32_t(ys))) ^
                             (uint64_t(uint16_t(int32_t(yo))) << 30);
        if (prev_frame_draws_ > 150 && seenVp.insert(key).second) {
          DrawTracef("[mm-vte] vte=0x%X xEna=%d yEna=%d | xs=%.1f xo=%.1f ys=%.1f yo=%.1f | "
                     "guestVP x=%.0f y=%.0f w=%.0f h=%.0f\n",
                     vte, int(xEna), int(yEna), xs, xo, ys, yo, xo - xs, yo + ys, 2.0f * xs,
                     -2.0f * ys);
        }
      }
    }

    if (traced_this_frame_ < 8) {
      DrawTracef("[mm-geo]  fetch[%u] addr=0x%08X size=%u stride=%u idx=%u %s\n", b0.fetch_constant,
                 vbByte, vbAvail, stride, index_count, ib ? "indexed" : "auto");
    }

    // MM_DUMP_GEO: dump full per-draw geometry for a bounded number of draws on live-render
    // frames (>150 draws = not the movie/menu), for a couple of frames, then stop. Compare a
    // torn sign draw to a clean character draw in mm_geo_dump.log.
    static const bool kDumpGeo = std::getenv("MM_DUMP_GEO") != nullptr;
    if (kDumpGeo && prev_frame_draws_ > 150 && geo_frames_dumped_ < 2 &&
        geo_draws_this_frame_ < 80) {
      DumpDrawGeometry(vshader, vkey, prim, index_count, ib, vbHost, vbAvail, stride);
      ++geo_draws_this_frame_;
    }

    // *** RenderDoc-diagnosed tear fix (2026-06-22, frame4620 ground truth) ***
    // The visible title-screen "tear" is NOT the big meshes (those project correctly): it's a
    // few DEPTH-DISABLED "frustum volume" draws (light/projector cookies) whose far vertices sit
    // at ~2.6e5 world units. Their VS transform + vertex decode are correct, but the game renders
    // these volumes with effects we don't emulate (additive/stencil), so drawn as opaque tris they
    // project to off-screen-crossing garbage triangles = the diagonal shards over the scene. No
    // real MM mesh has coordinates near that magnitude, so skip any draw whose float3/float4
    // POSITION is absurdly large. Decode is big-endian (guest VB order). Gated by MM_KEEP_HUGE.
    {
      static const bool kSkipHugePos = std::getenv("MM_KEEP_HUGE") == nullptr;
      auto rit = kSkipHugePos ? resolved_.find(vkey) : resolved_.end();
      if (rit != resolved_.end()) {
        uint32_t posOff = 0xFFFFFFFFu;
        for (const auto& a : rit->second.vertexLayout)
          if (a.format == 57u || a.format == 38u) {  // k_32_32_32(_32)_FLOAT = a real float position
            posOff = a.offsetDwords * 4u;
            break;
          }
        if (posOff != 0xFFFFFFFFu) {
          const uint32_t nverts = vbAvail ? (vbAvail / stride) : index_count;
          const uint32_t nchk = std::min<uint32_t>(nverts, 64u);
          float maxMag = 0.0f;
          for (uint32_t v = 0; v < nchk; ++v) {
            const uint8_t* p = vbHost + size_t(v) * stride + posOff;
            if (uint32_t((p - vbHost) + 12) > vbAvail) break;
            for (int c = 0; c < 3; ++c) {
              const uint8_t* q = p + c * 4;
              const uint32_t u = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) |
                                 (uint32_t(q[2]) << 8) | uint32_t(q[3]);
              float f;
              std::memcpy(&f, &u, sizeof(f));
              const float m = f < 0.0f ? -f : f;
              if (m == m && m > maxMag) maxMag = m;  // m==m drops NaN
            }
          }
          if (maxMag > 50000.0f) {
            if (traced_this_frame_ < 8)
              DrawTracef("[mm-skip] huge-pos volume vkey=%016llX maxMag=%.0f -> skipped\n",
                         static_cast<unsigned long long>(vkey), maxMag);
            return;  // don't paint the off-screen garbage shards
          }
        }
      }
    }

    // Bind the pixel shader's textures (the movie's Y/U/V planes) from its texture fetch
    // constants -> the bindless heap. Linear k_8 / k_8_8_8_8 only (the movie is linear k_8);
    // tiled/other formats are left on the dummy for now.
    if (auto* ps = active_pixel_shader()) {
      for (const auto& tb : ps->texture_bindings()) {
        const auto tf = register_file_->GetTextureFetch(tb.fetch_constant);
        const uint32_t base = uint32_t(tf.base_address) << 12;
        // Resolved render target? Bind the GPU snapshot (regardless of format/tiling, which the
        // upload path can't handle). This is the EDRAM resolve->sample link for multi-pass.
        if (Video::IsResolvedBase(base)) {
          if (traced_this_frame_ < 8)
            DrawTracef("[mm-tex]  s%u <- RESOLVED RT base=0x%08X\n", tb.fetch_constant, base);
          Video::BindGuestTexture(uint32_t(tb.fetch_constant), nullptr, 0, 0, 0, 0, 0, base);
          continue;
        }
        const uint32_t fmtv = uint32_t(tf.format);
        // Linear k_8 (YUV plane), k_8_8_8_8 (ARGB), and DXT1/DXT2_3/DXT4_5 (BC1/2/3). 360 game
        // art is almost all tiled DXT, so we detile + endian-swap in BindGuestTexture now.
        if (fmtv != 2 && fmtv != 6 && fmtv != 18 && fmtv != 19 && fmtv != 20) {
          // Log distinct UNSUPPORTED texture formats we drop to the dummy (black). The title's
          // sky/backdrop fullscreen quad may sample one of these -> black backdrop.
          static std::unordered_set<uint64_t> seenSkip;
          if (seenSkip.insert((uint64_t(fmtv) << 32) ^ base).second)
            DrawTracef("[mm-texskip] fmt=%u %ux%u tiled=%u endian=%u base=0x%08X (-> dummy/black)\n",
                       fmtv, uint32_t(tf.size_2d.width) + 1, uint32_t(tf.size_2d.height) + 1,
                       uint32_t(tf.tiled), uint32_t(tf.endianness), base);
          continue;
        }
        const uint32_t w = uint32_t(tf.size_2d.width) + 1;
        const uint32_t h = uint32_t(tf.size_2d.height) + 1;
        const uint32_t pitchTexels = uint32_t(tf.pitch) << 5;  // fetch pitch is in 32-texel units
        const uint8_t* tsrc = memory_->TranslatePhysical<const uint8_t*>(base);
        if (traced_this_frame_ < 8) {
          DrawTracef("[mm-tex]  s%u (binding %zu) fmt=%u %ux%u pitchTx=%u tiled=%u base=0x%08X\n",
                     tb.fetch_constant, tb.binding_index, fmtv, w, h, pitchTexels,
                     uint32_t(tf.tiled), base);
        }
        // The generated shaders index textures/samplers by FETCH-CONSTANT number (sN reads
        // fetch constant N), NOT by the SDK's binding listing order. Keying by binding_index
        // swapped s1/s2 (the U/V chroma planes) -> R/B-swapped movie. Use fetch_constant.
        Video::BindGuestTexture(uint32_t(tb.fetch_constant), tsrc, w, h, pitchTexels, int(fmtv),
                                int(uint32_t(tf.endianness)), base, bool(tf.tiled));
      }
    }

    if (ib && ib->guest_base && ib->count) {
      const uint8_t* ibHost = memory_->TranslatePhysical<const uint8_t*>(ib->guest_base);
      const bool ib32 = ib->format == rex::graphics::xenos::IndexFormat::kInt32;
      Video::RecordGuestDrawIndexed(vkey, pkey, prim, vbHost, vbAvail ? vbAvail : index_count * stride,
                                    stride, ibHost, ib->count, ib32, depthState);
    } else {
      uint32_t vbUsed = index_count * stride;
      if (vbAvail && vbUsed > vbAvail) vbUsed = vbAvail;
      if (vbUsed == 0) vbUsed = vbAvail;
      Video::RecordGuestDrawVB(vkey, pkey, prim, vbHost, vbUsed, stride, index_count, 0, depthState);
    }
  }

  // First real step of native draw translation: decode the guest draw stream
  // from the register state the base CommandProcessor maintains. Rendering is
  // still a no-op (return true), but we now see exactly what must be translated.
  bool IssueDraw(rex::graphics::xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool /*major_mode_explicit*/) override {
    ++draws_this_frame_;

    static const bool kDrawEnabledG = std::getenv("MM_NO_DRAW") == nullptr;
    // Handle non-color EDRAM modes independent of shader resolution. kCopy(6) is an
    // EDRAM->memory RESOLVE: snapshot the color RT to RB_COPY_DEST_BASE so a later pass that
    // samples that address gets the rendered scene. kDepthOnly(5) is a depth pre-pass (skip;
    // our color pass does its own depth). Both consume the draw without rendering geometry.
    {
      const uint32_t edramMode =
          uint32_t(register_file_->Get<rex::graphics::reg::RB_MODECONTROL>().edram_mode);
      if (edramMode == 6u) {
        if (kDrawEnabledG) {
          const auto cc = register_file_->Get<rex::graphics::reg::RB_COPY_CONTROL>();
          if (uint32_t(cc.copy_src_select) < 4u) {  // color resolve only (4 = depth)
            const uint32_t destBase = (*register_file_)[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE];
            if (copies_logged_ < 24) {
              ++copies_logged_;
              DrawTracef("[mm-resolve] kCopy destBase=0x%08X srcSel=%u\n", destBase,
                         uint32_t(cc.copy_src_select));
            }
            Video::ResolveColorTo(destBase);
          }
        }
        return true;
      }
      if (edramMode == 5u) return true;  // depth pre-pass: skip
    }

    // Build a Plume graphics pipeline for the active VS+PS pair, when both resolved
    // to DXIL. This validates the full machinery (root sig links against real
    // translated shaders). Geometry/textures/constants + the actual draw call come
    // next (tasks #4/#5); cached per pair so it runs once.
    auto* vshader = active_vertex_shader();
    auto* pshader = active_pixel_shader();
    if (vshader && pshader) {
      auto vit = resolved_.find(vshader->ucode_data_hash());
      auto pit = resolved_.find(pshader->ucode_data_hash());
      // Deferred PS synthesis: now that the true companion VS is known, build the no-container PS's
      // DXIL with an interpolator table matching this VS's exported semantics (slot i -> reg i).
      if (vit != resolved_.end() && pit != resolved_.end() && vit->second.dxil() &&
          pit->second.pendingSynth && !pit->second.dxil()) {
        const uint32_t dc = static_cast<uint32_t>(pshader->ucode_dword_count());
        const uint32_t ndw = dc - dc % 3;  // XenosRecomp needs ucodeBytes % 12 == 0
        // Shader::ucode_dwords() is host (little-endian) order; XenosRecomp reads the container's
        // ucode as big-endian (like the raw guest bytes the splice path passes). Swap back to BE.
        const uint32_t* le = pshader->ucode_dwords();
        std::vector<uint8_t> be(size_t(ndw) * 4);
        for (uint32_t i = 0; i < ndw; ++i) {
          const uint32_t v = le[i];
          be[i * 4 + 0] = uint8_t(v >> 24); be[i * 4 + 1] = uint8_t(v >> 16);
          be[i * 4 + 2] = uint8_t(v >> 8);  be[i * 4 + 3] = uint8_t(v);
        }
        pit->second.translated = mm::TranslateSyntheticPsToDxil(be.data(), ndw * 4, ShaderInclude(),
                                                                vit->second.vsInterp);
        pit->second.dxilSize = static_cast<uint32_t>(pit->second.translated.size());
        pit->second.pendingSynth = false;
        if (diag_logged_ < 24) {
          ++diag_logged_;
          ShaderTracef("[mm-synth] PS=%016llX <- VS=%016llX interp=%zu dxil=%s size=%u\n",
                       static_cast<unsigned long long>(pshader->ucode_data_hash()),
                       static_cast<unsigned long long>(vshader->ucode_data_hash()),
                       vit->second.vsInterp.size(), pit->second.dxil() ? "YES" : "no",
                       pit->second.dxilSize);
        }
      }
      if (vit != resolved_.end() && pit != resolved_.end() && vit->second.dxil() &&
          pit->second.dxil()) {
        const uint64_t vkey = vshader->ucode_data_hash();
        const uint64_t pkey = pshader->ucode_data_hash();
        const uint32_t prim = static_cast<uint32_t>(prim_type);
        // Pack the guest depth state (RB_DEPTHCONTROL): bit0 z_enable, bit1 z_write, bits2-4
        // zfunc. 0 = depth off. Drives a per-state PSO variant so 3D draws depth-test/write.
        const auto dc = register_file_->Get<rex::graphics::reg::RB_DEPTHCONTROL>();
        static const bool kNoDepth = std::getenv("MM_NO_DEPTH") != nullptr;  // diagnostic
        const uint32_t depthState =
            (dc.z_enable && !kNoDepth)
                ? (1u | (dc.z_write_enable ? 2u : 0u) | ((uint32_t(dc.zfunc) & 7u) << 2))
                : 0u;
        // (kDepthOnly/kCopy modes were already consumed above; this is a color draw.)
        const uint64_t attemptKey =
            vkey ^ (pkey << 1) ^ (uint64_t(prim) << 40) ^ (uint64_t(depthState) << 48);
        if (pipelines_attempted_.insert(attemptKey).second) {
          const auto& vl = vit->second.vertexLayout;
          const bool ok = Video::GetOrCreateGuestPipeline(
              vit->second.dxil(), vit->second.dxilSize, vkey, pit->second.dxil(),
              pit->second.dxilSize, pkey, prim, vl.empty() ? nullptr : vl.data(), vl.size(),
              depthState);
          DrawTracef("[mm-draw] pipeline VS=%016llX PS=%016llX prim=%u depth=%u attrs=%zu -> %s\n",
                     static_cast<unsigned long long>(vkey), static_cast<unsigned long long>(pkey),
                     prim, depthState, vl.size(), ok ? "OK" : "FAIL(see mm_video.log)");
        }
        // Record the actual draw. Default ON now that the movie path is stable; set MM_NO_DRAW
        // to fall back to the safe clear+present (escape hatch if a draw TDRs the GPU).
        if (kDrawEnabledG) {
          RecordRealDraw(vshader, vkey, pkey, prim, index_count, index_buffer_info, depthState);
        }
      }
    }

    // Trace a few draws per frame to characterize the stream + the render-target state, so we
    // can see whether the menu is multi-pass (RT switches + resolves) or direct-to-main.
    if (traced_this_frame_ < 8) {
      ++traced_this_frame_;
      const auto& regs = *register_file_;
      const auto si = regs.Get<rex::graphics::reg::RB_SURFACE_INFO>();
      const auto ci = regs.Get<rex::graphics::reg::RB_COLOR_INFO>();
      const auto di = regs.Get<rex::graphics::reg::RB_DEPTH_INFO>();
      const auto mc = regs.Get<rex::graphics::reg::RB_MODECONTROL>();
      DrawTracef("[mm-rt]  prim=%u idx=%u %s | colorBase=%u fmt=%u pitch=%u msaa=%u depthBase=%u "
                 "edramMode=%u\n",
                 static_cast<uint32_t>(prim_type), index_count,
                 index_buffer_info ? "indexed" : "auto", uint32_t(ci.color_base),
                 uint32_t(ci.color_format), uint32_t(si.surface_pitch), uint32_t(si.msaa_samples),
                 uint32_t(di.depth_base), uint32_t(mc.edram_mode));
    }
    return true;
  }
  bool IssueCopy() override {
    // EDRAM resolve (RT -> system memory). Log the destination so we can see whether the menu
    // resolves its scene to a texture that later draws sample (multi-pass) — the key to whether
    // RT tracking needs resolve emulation.
    if (copies_logged_ < 24) {
      ++copies_logged_;
      const auto& regs = *register_file_;
      const auto cc = regs.Get<rex::graphics::reg::RB_COPY_CONTROL>();
      const uint32_t destBase = regs[rex::graphics::XE_GPU_REG_RB_COPY_DEST_BASE];
      const uint32_t destPitch = regs[rex::graphics::XE_GPU_REG_RB_COPY_DEST_PITCH];
      const auto ci = regs.Get<rex::graphics::reg::RB_COLOR_INFO>();
      DrawTracef("[mm-resolve] destBase=0x%08X destPitch=0x%08X srcSel=%u cmd=%u colorBase=%u "
                 "fmt=%u\n",
                 destBase, destPitch, uint32_t(cc.copy_src_select), uint32_t(cc.copy_command),
                 uint32_t(ci.color_base), uint32_t(ci.color_format));
    }
    return true;
  }

 private:
  struct ResolvedShader {
    const uint8_t* cacheDxil = nullptr;  // precompiled cache ptr (stable, lifetime)
    std::vector<uint8_t> translated;     // runtime-translated DXIL (owned)
    uint32_t dxilSize = 0;
    uint64_t containerHash = 0;
    bool isPixel = false;
    bool containerFound = false;
    bool pendingSynth = false;  // PS: synthesize DXIL at draw time using the companion VS's interp
    std::vector<mm::VertexInputAttr> vertexLayout;  // VS IA layout (from splice/translate)
    std::vector<uint32_t> vsInterp;  // VS: exported interpolator semantics (slot order) for PS synth
    const uint8_t* dxil() const { return !translated.empty() ? translated.data() : cacheDxil; }
  };

  uint32_t draws_this_frame_ = 0;
  uint32_t traced_this_frame_ = 0;
  uint32_t index_rebuilds_ = 0;
  uint32_t diag_logged_ = 0;
  uint32_t copies_logged_ = 0;
  // MM_DUMP_GEO geometry dump: arm only on live-render frames (>150 draws, not the
  // movie/menu), dump a bounded number of draws per frame for a couple of frames.
  uint32_t prev_frame_draws_ = 0;
  uint32_t geo_draws_this_frame_ = 0;
  uint32_t geo_frames_dumped_ = 0;
  bool pipeline_selftest_done_ = false;
  std::unordered_map<uint64_t, rex::graphics::Shader*> shaders_;
  std::unordered_map<uint64_t, ResolvedShader> resolved_;  // ucode hash -> DXIL
  std::unordered_set<uint64_t> pipelines_attempted_;
};

// Reuse the D3D12 graphics system for its provider/presenter/MMIO/vsync plumbing,
// but swap in the null command processor. name() is overridden because the base
// D3D12 name() downcasts the command processor to D3D12CommandProcessor (which
// ours is not) for the window-title text.
class MMGraphicsSystem : public rex::graphics::d3d12::D3D12GraphicsSystem {
 public:
  std::string name() const override { return "Monster Madness (native consumer)"; }

  // Skip presenter creation: we present via Plume (mm::Video), not the SDK's
  // presenter. SetupGuestGpu (the ring-consuming command processor) still runs,
  // so the game advances; with no presenter, SetupPresentation in rex_app leaves
  // the window free for Plume's swapchain (see rex_app.cpp: neither overlay
  // branch runs when graphics_system exists but presenter() is null).
  rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* /*app_context*/) override {
    return 0;  // X_STATUS_SUCCESS
  }

 protected:
  std::unique_ptr<rex::graphics::CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<MMNullCommandProcessor>(this, kernel_state_);
  }
};

}  // namespace

std::unique_ptr<rex::system::IGraphicsSystem> CreateNullConsumerGraphicsSystem() {
  return std::make_unique<MMGraphicsSystem>();
}

}  // namespace mm
