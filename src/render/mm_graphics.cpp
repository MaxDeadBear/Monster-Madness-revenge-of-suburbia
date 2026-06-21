// See mm_graphics.h.

#include "render/mm_graphics.h"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
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
    draws_this_frame_ = 0;
    traced_this_frame_ = 0;
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
        if (rs.cacheDxil) {
          source = "cache";
        } else {
          bool isPx = false;
          rs.translated =
              mm::TranslateXenosShaderToDxil(container, ShaderInclude(), isPx, &rs.vertexLayout);
          if (!rs.translated.empty()) {
            rs.dxilSize = static_cast<uint32_t>(rs.translated.size());
            source = "xlat";
          } else {
            source = "xlat-FAIL";
          }
        }
      } else if (shader_type == rex::graphics::xenos::ShaderType::kVertex &&
                 !shader->vertex_bindings().empty()) {
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
        // The right template's vertex elements resolve against every vfetch in this ucode,
        // so prefer attrs == the shader's actual attribute count (a full, confident match);
        // fall back to the max otherwise (unordered iteration, so exact-match avoids ties).
        size_t expected = 0;
        for (const auto& b : shader->vertex_bindings()) expected += b.attributes.size();
        const uint8_t* bestTpl = nullptr;
        int bestAttrs = 0;
        for (size_t i = 0; i < nC; ++i) {
          const int attrs = mm::ProbeSplicedVsAttrs(vsC[i], ub, nbytes, ShaderInclude());
          if (attrs > bestAttrs) { bestAttrs = attrs; bestTpl = vsC[i]; }
          if (attrs > 0 && size_t(attrs) == expected) { bestTpl = vsC[i]; break; }
        }
        if (bestTpl) {
          bool isPx = false;
          rs.translated = mm::TranslateSplicedShaderToDxil(bestTpl, ub, nbytes, ShaderInclude(),
                                                           isPx, &rs.vertexLayout);
          if (!rs.translated.empty()) {
            rs.dxilSize = static_cast<uint32_t>(rs.translated.size());
            rs.containerFound = true;  // resolved via splice
            source = "splice";
          } else {
            source = "splice-FAIL";
          }
        } else {
          source = "no-template";
        }
      }
    }
    // Capture before the move (operator[] would insert an empty entry and discard rs).
    const bool haveDxil = rs.dxil() != nullptr;
    const bool found = rs.containerFound;
    const uint32_t dxilSize = rs.dxilSize;
    resolved_[hash] = std::move(rs);  // store the real resolution
    ShaderTracef("[mm-resolve]   container=%s dxil=%s(%s) size=%u\n", found ? "FOUND" : "miss",
                 haveDxil ? "YES" : "no", source, dxilSize);

    // Diagnose why a miss happened (esp. vertex shaders): index coverage + whether a
    // container's ucode is a prefix/suffix of the bound ucode (fetch-shader preamble?).
    if (!found && diag_logged_ < 6) {
      ++diag_logged_;
      size_t total = 0, vs = 0, ps = 0;
      mm::GetUcodeIndexStats(total, vs, ps);
      uint32_t matchedDwords = 0;
      int relation = 0;
      const bool related =
          mm::DiagnoseUcodeMatch(reinterpret_cast<const uint8_t*>(host_address), dword_count,
                                 matchedDwords, relation);
      const char* relStr = relation == 1   ? "exact"
                           : relation == 2 ? "container-is-prefix"
                           : relation == 3 ? "container-is-suffix"
                                           : "none";
      ShaderTracef(
          "[mm-diag]   %s dwords=%u idx{total=%zu vs=%zu ps=%zu} related=%s cDwords=%u (%s)\n",
          is_vertex ? "VERTEX" : "PIXEL", dword_count, total, vs, ps, related ? "YES" : "no",
          matchedDwords, relStr);
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
  void RecordRealDraw(rex::graphics::Shader* vshader, uint64_t vkey, uint64_t pkey, uint32_t prim,
                      uint32_t index_count, IndexBufferInfo* ib) {
    // Take a fresh per-draw resource slot (own VB/IB + b0/b1/b2 + descriptor set) so this
    // draw's geometry/constants/textures don't clobber other draws in the same frame.
    Video::BeginGuestDraw();

    const auto& bindings = vshader->vertex_bindings();
    if (bindings.empty() || !memory_) {
      Video::RecordGuestDraw(vkey, pkey, prim, index_count, index_count);
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
      Video::RecordGuestDraw(vkey, pkey, prim, index_count, index_count);
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
    }

    if (traced_this_frame_ < 8) {
      DrawTracef("[mm-geo]  fetch[%u] addr=0x%08X size=%u stride=%u idx=%u %s\n", b0.fetch_constant,
                 vbByte, vbAvail, stride, index_count, ib ? "indexed" : "auto");
    }

    // Bind the pixel shader's textures (the movie's Y/U/V planes) from its texture fetch
    // constants -> the bindless heap. Linear k_8 / k_8_8_8_8 only (the movie is linear k_8);
    // tiled/other formats are left on the dummy for now.
    if (auto* ps = active_pixel_shader()) {
      for (const auto& tb : ps->texture_bindings()) {
        const auto tf = register_file_->GetTextureFetch(tb.fetch_constant);
        const uint32_t fmtv = uint32_t(tf.format);
        if (tf.tiled || (fmtv != 2 && fmtv != 6)) continue;
        const uint32_t bpp = (fmtv == 6) ? 4u : 1u;
        const uint32_t w = uint32_t(tf.size_2d.width) + 1;
        const uint32_t h = uint32_t(tf.size_2d.height) + 1;
        const uint32_t pitchBytes = (uint32_t(tf.pitch) << 5) * bpp;
        const uint8_t* tsrc =
            memory_->TranslatePhysical<const uint8_t*>(uint32_t(tf.base_address) << 12);
        if (traced_this_frame_ < 8) {
          DrawTracef("[mm-tex]  s%u (binding %zu) fmt=%u %ux%u pitch=%u base=0x%08X\n",
                     tb.fetch_constant, tb.binding_index, fmtv, w, h, pitchBytes,
                     uint32_t(tf.base_address) << 12);
        }
        // The generated shaders index textures/samplers by FETCH-CONSTANT number (sN reads
        // fetch constant N), NOT by the SDK's binding listing order. Keying by binding_index
        // swapped s1/s2 (the U/V chroma planes) -> R/B-swapped movie. Use fetch_constant.
        Video::BindGuestTexture(uint32_t(tb.fetch_constant), tsrc, w, h, pitchBytes, int(fmtv),
                                int(uint32_t(tf.endianness)));
      }
    }

    if (ib && ib->guest_base && ib->count) {
      const uint8_t* ibHost = memory_->TranslatePhysical<const uint8_t*>(ib->guest_base);
      const bool ib32 = ib->format == rex::graphics::xenos::IndexFormat::kInt32;
      Video::RecordGuestDrawIndexed(vkey, pkey, prim, vbHost, vbAvail ? vbAvail : index_count * stride,
                                    stride, ibHost, ib->count, ib32);
    } else {
      uint32_t vbUsed = index_count * stride;
      if (vbAvail && vbUsed > vbAvail) vbUsed = vbAvail;
      if (vbUsed == 0) vbUsed = vbAvail;
      Video::RecordGuestDrawVB(vkey, pkey, prim, vbHost, vbUsed, stride, index_count, 0);
    }
  }

  // First real step of native draw translation: decode the guest draw stream
  // from the register state the base CommandProcessor maintains. Rendering is
  // still a no-op (return true), but we now see exactly what must be translated.
  bool IssueDraw(rex::graphics::xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool /*major_mode_explicit*/) override {
    ++draws_this_frame_;

    // Build a Plume graphics pipeline for the active VS+PS pair, when both resolved
    // to DXIL. This validates the full machinery (root sig links against real
    // translated shaders). Geometry/textures/constants + the actual draw call come
    // next (tasks #4/#5); cached per pair so it runs once.
    auto* vshader = active_vertex_shader();
    auto* pshader = active_pixel_shader();
    if (vshader && pshader) {
      auto vit = resolved_.find(vshader->ucode_data_hash());
      auto pit = resolved_.find(pshader->ucode_data_hash());
      if (vit != resolved_.end() && pit != resolved_.end() && vit->second.dxil() &&
          pit->second.dxil()) {
        const uint64_t vkey = vshader->ucode_data_hash();
        const uint64_t pkey = pshader->ucode_data_hash();
        const uint32_t prim = static_cast<uint32_t>(prim_type);
        if (pipelines_attempted_.insert(vkey ^ (pkey << 1)).second) {
          const auto& vl = vit->second.vertexLayout;
          const bool ok = Video::GetOrCreateGuestPipeline(
              vit->second.dxil(), vit->second.dxilSize, vkey, pit->second.dxil(),
              pit->second.dxilSize, pkey, prim, vl.empty() ? nullptr : vl.data(), vl.size());
          DrawTracef("[mm-draw] pipeline for VS=%016llX PS=%016llX prim=%u attrs=%zu -> %s\n",
                     static_cast<unsigned long long>(vkey), static_cast<unsigned long long>(pkey),
                     prim, vl.size(), ok ? "OK" : "FAIL(see mm_video.log)");
        }
        // Record the actual draw. Default ON now that the movie path is stable; set MM_NO_DRAW
        // to fall back to the safe clear+present (escape hatch if a draw TDRs the GPU, e.g. an
        // unhandled gameplay format).
        static const bool kDrawEnabled = std::getenv("MM_NO_DRAW") == nullptr;
        if (kDrawEnabled) {
          RecordRealDraw(vshader, vkey, pkey, prim, index_count, index_buffer_info);
        }
      }
    }

    // Trace a few draws per frame to characterize the stream without flooding.
    if (traced_this_frame_ < 8) {
      ++traced_this_frame_;
      const auto& regs = *register_file_;
      uint32_t surface_pitch = regs.Get<rex::graphics::reg::RB_SURFACE_INFO>().surface_pitch;
      const char* idx = index_buffer_info ? "indexed" : "auto";
      uint32_t idx_base = index_buffer_info ? index_buffer_info->guest_base : 0;
      DrawTracef("[mm-draw]  draw prim=%u indices=%u %s idx_base=0x%08X surface_pitch=%u\n",
                 static_cast<uint32_t>(prim_type), index_count, idx, idx_base, surface_pitch);
    }
    return true;
  }
  bool IssueCopy() override { return true; }

 private:
  struct ResolvedShader {
    const uint8_t* cacheDxil = nullptr;  // precompiled cache ptr (stable, lifetime)
    std::vector<uint8_t> translated;     // runtime-translated DXIL (owned)
    uint32_t dxilSize = 0;
    uint64_t containerHash = 0;
    bool isPixel = false;
    bool containerFound = false;
    std::vector<mm::VertexInputAttr> vertexLayout;  // VS IA layout (from splice/translate)
    const uint8_t* dxil() const { return !translated.empty() ? translated.data() : cacheDxil; }
  };

  uint32_t draws_this_frame_ = 0;
  uint32_t traced_this_frame_ = 0;
  uint32_t index_rebuilds_ = 0;
  uint32_t diag_logged_ = 0;
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
