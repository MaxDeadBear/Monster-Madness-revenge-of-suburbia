// See shader_translate.h.

#include "render/shader_translate.h"

#include <cstring>
#include <string>

// XenosRecomp recompiler core (vendored static lib xenos_recomp). pch.h provides
// the DXC types (dxcapi.h) and the be<> big-endian template its headers rely on.
#include "pch.h"

#include "dxc_compiler.h"
#include "shader_recompiler.h"

namespace mm {

// SEH-guarded so a malformed/garbage shader container can't crash the game (the
// recompiler walks the container; mirrors XenosRecomp main.cpp's tryRecompile).
// Must be its own function with no locals needing C++ unwinding.
static bool TryRecompile(ShaderRecompiler& recompiler, const uint8_t* data,
                         std::string_view include) {
  __try {
    recompiler.recompile(data, include);
    return true;
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
                                                  std::vector<VertexInputAttr>* vertexLayoutOut) {
  isPixelOut = false;
  if (vertexLayoutOut) vertexLayoutOut->clear();
  std::vector<uint8_t> spliced = BuildSplicedContainer(templateContainer, ucode, ucodeBytes);
  if (spliced.empty()) return {};
  // The recompiler reads constant-default values at virtualSize + definition->physicalOffset,
  // which for a spliced container would land in the ucode; vertex shaders have no definition
  // table (dtOff=0) so this is safe. (Guarded inside XenosRecomp regardless.)
  return TranslateXenosShaderToDxil(spliced.data(), includeHlsl, isPixelOut, vertexLayoutOut);
}

int ProbeSplicedVsAttrs(const uint8_t* templateContainer, const uint8_t* ucode, uint32_t ucodeBytes,
                        std::string_view includeHlsl) {
  std::vector<uint8_t> spliced = BuildSplicedContainer(templateContainer, ucode, ucodeBytes);
  if (spliced.empty()) return -1;
  ShaderRecompiler recompiler;
  if (!TryRecompile(recompiler, spliced.data(), includeHlsl)) return -1;
  if (recompiler.isPixelShader) return -1;
  return int(recompiler.vertexInputAttributes.size());
}

std::vector<uint8_t> TranslateXenosShaderToDxil(const uint8_t* shaderContainer,
                                                std::string_view includeHlsl, bool& isPixelOut,
                                                std::vector<VertexInputAttr>* vertexLayoutOut) {
  isPixelOut = false;
  if (vertexLayoutOut) vertexLayoutOut->clear();
  if (!shaderContainer) return {};

  ShaderRecompiler recompiler;
  if (!TryRecompile(recompiler, shaderContainer, includeHlsl)) return {};  // ucode -> HLSL
  isPixelOut = recompiler.isPixelShader;
  if (recompiler.out.empty()) return {};

  // Hand back the parsed input-assembler layout (vertex shaders only) so the host can
  // build the IA layout with real formats/offsets instead of guessing.
  if (vertexLayoutOut && !recompiler.isPixelShader) {
    vertexLayoutOut->reserve(recompiler.vertexInputAttributes.size());
    for (const auto& a : recompiler.vertexInputAttributes)
      vertexLayoutOut->push_back({a.usage, a.usageIndex, a.format, a.offsetDwords, a.strideDwords});
  }

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
  if (!blob) return {};

  const auto* bytes = static_cast<const uint8_t*>(blob->GetBufferPointer());
  std::vector<uint8_t> dxil(bytes, bytes + blob->GetBufferSize());
  blob->Release();
  return dxil;
}

}  // namespace mm
