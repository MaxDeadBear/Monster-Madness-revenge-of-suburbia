// See shader_translate.h.

#include "render/shader_translate.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

// XenosRecomp recompiler core (vendored static lib xenos_recomp). pch.h provides
// the DXC types (dxcapi.h) and the be<> big-endian template its headers rely on.
#include "pch.h"

#include "dxc_compiler.h"
#include "shader_recompiler.h"

namespace mm {

// recompile() can fail two ways: a C++ throw (its own validation, e.g. "invalid constant
// table range") or a genuine SEH access violation. Catch C++ exceptions here (logging the
// message — that's the actionable reason for xlat-FAIL) ...
static bool RecompileCxxGuarded(ShaderRecompiler& recompiler, const uint8_t* data,
                                std::string_view include) {
  try {
    recompiler.recompile(data, include);
    return true;
  } catch (const std::exception& e) {
    if (FILE* f = std::fopen("mm_xlat_fail.log", "a")) {
      std::fprintf(f, "[recompile-exc] %s (phase=%d outBytes=%zu px=%d)\n", e.what(),
                   g_recompPhase, recompiler.out.size(), recompiler.isPixelShader ? 1 : 0);
      std::fclose(f);
    }
    return false;
  } catch (...) {
    return false;
  }
}

// ... and wrap that in SEH so a true AV (malformed/garbage container) can't crash the game.
// (Must be its own function with no locals needing C++ unwinding alongside __try.)
static bool TryRecompile(ShaderRecompiler& recompiler, const uint8_t* data,
                         std::string_view include) {
  __try {
    return RecompileCxxGuarded(recompiler, data, include);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

size_t SafePeek(const uint8_t* src, uint8_t* dst, size_t n) {
  if (!src) return 0;
  __try {
    for (size_t i = 0; i < n; ++i) dst[i] = src[i];
    return n;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

namespace {
uint32_t BeRd32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
void BeWr32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// Build a fully SYNTHETIC XenosRecomp container for a pixel shader whose specialized variant
// ucode matches no RAM container. Layout: ShaderContainer(36) + minimal empty ConstantTable(32)
// + PixelShader{...,interpolators[N]} + the ucode. svPos is parked at r255 so it doesn't collide.
// The interpolator table maps slot i -> register i reading the COMPANION VS's slot-i exported
// semantic (`vsInterp[i]` = (usage<<8)|usageIndex). That's how Xenos links interpolators: slot i
// of the VS feeds slot i of the PS, loaded into PS GPR i (param-gen off). If `vsInterp` is empty
// (VS not yet resolved), fall back to a generic TexCoord{i} table.
std::vector<uint8_t> BuildSyntheticPsContainer(const uint8_t* ucode, uint32_t ucodeBytes,
                                               const std::vector<uint32_t>& vsInterp,
                                               uint32_t outputsMask) {
  if (!ucode || ucodeBytes == 0 || (ucodeBytes % 12) != 0 || ucodeBytes > 4096) return {};
  // Match the VS's interpolator count (slots beyond what the VS exports carry no data); clamp to
  // the hardware max of 16 and to what the 4-bit reg field can address.
  uint32_t kInterp = vsInterp.empty() ? 16u : uint32_t(vsInterp.size());
  if (kInterp > 16) kInterp = 16;
  constexpr uint32_t ctOff = 36;     // after ShaderContainer
  constexpr uint32_t ctSize = 32;    // sizeof(ConstantTableContainer): size + ConstantTable(28)
  constexpr uint32_t shOff = ctOff + ctSize;             // 68
  const uint32_t virtualSize = shOff + 32 + kInterp * 4;  // PixelShader = Shader(24)+8+interps
  std::vector<uint8_t> c(size_t(virtualSize) + ucodeBytes, 0);
  uint8_t* p = c.data();
  BeWr32(p + 0, 0x102A1100u);   // flags (PS: bit0 clear)
  BeWr32(p + 4, virtualSize);   // virtualSize (ucode starts here)
  BeWr32(p + 8, ucodeBytes);    // physicalSize
  BeWr32(p + 16, ctOff);        // constantTableOffset
  BeWr32(p + 24, shOff);        // shaderOffset
  // ConstantTableContainer: outer size=32; ConstantTable.size=28, constants=0, constantInfo=0.
  BeWr32(p + ctOff + 0, ctSize);
  BeWr32(p + ctOff + 4 + 0, ctSize - 4);
  // PixelShader: physicalOffset=0, size=ucode, fieldC=svPos(255)<<8, interpolatorInfo=count<<5,
  // field18=0, outputs=COLOR0. Interpolator i = {usageIndex, usage, reg=i} (Interpolator bitfield
  // usageIndex:4,usage:4,reg:4).
  BeWr32(p + shOff + 4, ucodeBytes);          // Shader.size
  BeWr32(p + shOff + 12, 0xFFu << 8);         // Shader.fieldC -> svPos register 255 (parked)
  BeWr32(p + shOff + 20, kInterp << 5);       // Shader.interpolatorInfo (count<<5)
  BeWr32(p + shOff + 28, outputsMask);        // PixelShader.outputs (COLOR0..3 | DEPTH bits)
  for (uint32_t i = 0; i < kInterp; ++i) {
    uint32_t usage, usageIndex;
    if (i < vsInterp.size()) { usage = (vsInterp[i] >> 8) & 0xF; usageIndex = vsInterp[i] & 0xF; }
    else { usage = 5u; usageIndex = i & 0xF; }  // TexCoord{i} fallback
    BeWr32(p + shOff + 32 + i * 4, usageIndex | (usage << 4) | ((i & 0xF) << 8));
  }
  std::memcpy(p + virtualSize, ucode, ucodeBytes);
  return c;
}

// Build a synthetic container = the template's virtual region + `ucode` as the physical
// region, with Shader.physicalOffset=0, Shader.size=ucodeBytes, physicalSize=ucodeBytes.
// Returns empty on a malformed/unreadable template. SEH-safe on the guest template pointer.
std::vector<uint8_t> BuildSplicedContainer(const uint8_t* templateContainer, const uint8_t* ucode,
                                           uint32_t ucodeBytes) {
  uint8_t hdr[0x20];
  if (mm::SafePeek(templateContainer, hdr, sizeof(hdr)) != sizeof(hdr)) return {};
  const uint32_t flags = BeRd32(hdr + 0);
  if ((flags & 0xFFFFFF00u) != 0x102A1100u) return {};
  const uint32_t virtualSize = BeRd32(hdr + 4);
  const uint32_t shaderOffset = BeRd32(hdr + 0x18);
  if (virtualSize < 36 || virtualSize > (64u * 1024u)) return {};
  if (shaderOffset + 8 > virtualSize) return {};
  if (ucodeBytes == 0 || (ucodeBytes % 12) != 0 || ucodeBytes > 4096) return {};

  std::vector<uint8_t> out(size_t(virtualSize) + ucodeBytes);
  if (mm::SafePeek(templateContainer, out.data(), virtualSize) != virtualSize) return {};
  std::memcpy(out.data() + virtualSize, ucode, ucodeBytes);
  BeWr32(out.data() + 8, ucodeBytes);              // ShaderContainer.physicalSize
  BeWr32(out.data() + shaderOffset + 0, 0);        // Shader.physicalOffset
  BeWr32(out.data() + shaderOffset + 4, ucodeBytes);  // Shader.size
  return out;
}
}  // namespace

std::vector<uint8_t> TranslateSplicedShaderToDxil(const uint8_t* templateContainer,
                                                  const uint8_t* ucode, uint32_t ucodeBytes,
                                                  std::string_view includeHlsl, bool& isPixelOut,
                                                  std::vector<VertexInputAttr>* vertexLayoutOut,
                                                  std::vector<uint32_t>* interpolatorSemanticsOut) {
  isPixelOut = false;
  if (vertexLayoutOut) vertexLayoutOut->clear();
  std::vector<uint8_t> spliced = BuildSplicedContainer(templateContainer, ucode, ucodeBytes);
  if (spliced.empty()) return {};
  // The recompiler reads constant-default values at virtualSize + definition->physicalOffset,
  // which for a spliced container would land in the ucode; vertex shaders have no definition
  // table (dtOff=0) so this is safe. (Guarded inside XenosRecomp regardless.)
  return TranslateXenosShaderToDxil(spliced.data(), includeHlsl, isPixelOut, vertexLayoutOut,
                                    interpolatorSemanticsOut);
}

std::vector<uint8_t> TranslateSyntheticPsToDxil(const uint8_t* ucode, uint32_t ucodeBytes,
                                                std::string_view includeHlsl,
                                                const std::vector<uint32_t>& vsInterp) {
  // The PixelShaderOutput struct is built from the container's `outputs` field, but the ucode
  // emits writes to whatever it exports (oC0..3, oDepth). If those disagree, DXC fails ("no member
  // named 'oDepth'"). So PASS 1: recompile-only (no DXC) to discover which outputs the ucode
  // actually exports, then PASS 2: build the container with that exact mask and translate.
  std::vector<uint8_t> probe = BuildSyntheticPsContainer(ucode, ucodeBytes, vsInterp, 0x1F);
  if (probe.empty()) return {};
  uint32_t outputsMask = 0x1;  // default COLOR0 if the probe yields nothing
  {
    ShaderRecompiler rc;
    if (TryRecompile(rc, probe.data(), includeHlsl) && rc.pixelShaderOutputs != 0)
      outputsMask = rc.pixelShaderOutputs;
  }
  std::vector<uint8_t> synth = BuildSyntheticPsContainer(ucode, ucodeBytes, vsInterp, outputsMask);
  if (synth.empty()) return {};
  bool isPixel = false;
  return TranslateXenosShaderToDxil(synth.data(), includeHlsl, isPixel, nullptr, nullptr);
}

int ProbeSplicedVsAttrs(const uint8_t* templateContainer, const uint8_t* ucode, uint32_t ucodeBytes,
                        std::string_view includeHlsl, std::vector<VertexInputAttr>* attrsOut) {
  if (attrsOut) attrsOut->clear();
  std::vector<uint8_t> spliced = BuildSplicedContainer(templateContainer, ucode, ucodeBytes);
  if (spliced.empty()) return -1;
  ShaderRecompiler recompiler;
  if (!TryRecompile(recompiler, spliced.data(), includeHlsl)) return -1;
  if (recompiler.isPixelShader) return -1;
  if (attrsOut) {
    attrsOut->reserve(recompiler.vertexInputAttributes.size());
    for (const auto& a : recompiler.vertexInputAttributes)
      attrsOut->push_back({a.usage, a.usageIndex, a.format, a.offsetDwords, a.strideDwords});
  }
  return int(recompiler.vertexInputAttributes.size());
}

namespace {
// Why a translation failed (xlat-FAIL), so we can fix the menu/gameplay shader path. DXC's own
// error text is captured separately in mm_dxc_errors.log (dxc_compiler.cpp).
void XlatFail(const char* reason, bool isPixel) {
  static int n = 0;
  if (n++ >= 32) return;
  if (FILE* f = std::fopen("mm_xlat_fail.log", "a")) {
    std::fprintf(f, "[xlat-fail] %-14s %s\n", reason, isPixel ? "PS" : "VS");
    std::fclose(f);
  }
}
void DumpFailHlsl(const std::string& hlsl, bool isPixel) {
  static int n = 0;
  if (n >= 4) return;
  char fn[48];
  std::snprintf(fn, sizeof(fn), "mm_xlatfail_%s_%d.hlsl", isPixel ? "ps" : "vs", n);
  ++n;
  if (FILE* f = std::fopen(fn, "w")) {
    std::fwrite(hlsl.data(), 1, hlsl.size(), f);
    std::fclose(f);
  }
}
}  // namespace

std::vector<uint8_t> TranslateXenosShaderToDxil(const uint8_t* shaderContainer,
                                                std::string_view includeHlsl, bool& isPixelOut,
                                                std::vector<VertexInputAttr>* vertexLayoutOut,
                                                std::vector<uint32_t>* interpolatorSemanticsOut) {
  isPixelOut = false;
  if (vertexLayoutOut) vertexLayoutOut->clear();
  if (interpolatorSemanticsOut) interpolatorSemanticsOut->clear();
  if (!shaderContainer) return {};

  ShaderRecompiler recompiler;
  if (!TryRecompile(recompiler, shaderContainer, includeHlsl)) {  // ucode -> HLSL
    char buf[32];
    std::snprintf(buf, sizeof(buf), "threw@phase%d", g_recompPhase);
    XlatFail(buf, false);
    return {};
  }
  isPixelOut = recompiler.isPixelShader;
  if (recompiler.out.empty()) {
    XlatFail("empty-hlsl", recompiler.isPixelShader);
    return {};
  }

  // Diagnostic (MM_DUMP_GEO): dump the generated VS HLSL so we can see exactly which input
  // `output.oPos` is computed from (the torn-mesh root-cause investigation).
  if (!recompiler.isPixelShader && std::getenv("MM_DUMP_GEO")) {
    static int vn = 0;
    if (vn < 12) {
      char fn[48];
      std::snprintf(fn, sizeof(fn), "mm_vs_%d.hlsl", vn++);
      if (FILE* f = std::fopen(fn, "w")) {
        std::fwrite(recompiler.out.data(), 1, recompiler.out.size(), f);
        std::fclose(f);
      }
    }
  }

  // Hand back the parsed input-assembler layout (vertex shaders only) so the host can
  // build the IA layout with real formats/offsets instead of guessing.
  if (vertexLayoutOut && !recompiler.isPixelShader) {
    vertexLayoutOut->reserve(recompiler.vertexInputAttributes.size());
    for (const auto& a : recompiler.vertexInputAttributes)
      vertexLayoutOut->push_back({a.usage, a.usageIndex, a.format, a.offsetDwords, a.strideDwords});
  }
  // Hand back the per-slot interpolator semantics (a VS exports these; the host uses them to
  // synthesize a matching PS interpolator table for no-container pixel shaders).
  if (interpolatorSemanticsOut) *interpolatorSemanticsOut = recompiler.interpolatorSemantics;

  // Compile as a real vs_6_0/ps_6_0 (NOT lib_6_3) so the DXIL is usable directly in a
  // graphics PSO (a library has no ISGN/PSV0 -> D3D12 PSO E_INVALIDARG). XenosRecomp
  // ties library compilation to spec constants because g_SpecConstants() is declared
  // unresolved (Unleashed links it per-variant at runtime). For direct use we instead
  // define it to a constant (0 = default feature set; per-draw specialization later).
  std::string hlsl = recompiler.out;
  if (recompiler.specConstantsMask != 0) {
    hlsl += "\nuint g_SpecConstants() { return 0u; }\n";
  }

  DxcCompiler dxc;
  IDxcBlob* blob = dxc.compile(hlsl, recompiler.isPixelShader,
                               /*compileLibrary=*/false, /*compileSpirv=*/false);
  if (!blob) {
    XlatFail("dxc-fail", recompiler.isPixelShader);
    DumpFailHlsl(hlsl, recompiler.isPixelShader);  // inspect the shader DXC rejected
    return {};
  }

  const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
  std::vector<uint8_t> dxil(bytes, bytes + blob->GetBufferSize());
  blob->Release();
  return dxil;
}

}  // namespace mm
