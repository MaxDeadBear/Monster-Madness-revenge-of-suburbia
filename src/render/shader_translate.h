// Native shader path: translate Xbox 360 Xenos shader microcode to host DXIL via
// XenosRecomp (ucode -> HLSL) + DXC (HLSL -> DXIL), for the Plume renderer.
//
// This replaces the SDK's Xenia DXBC translator (heavy emulation model) with
// XenosRecomp's clean HLSL output (the ReOdyssey/Unleashed approach). See
// docs/RENDERER_MAPPING.md (shader extraction finding).

#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace mm {

// One vertex-shader input as the host input assembler must feed it. Sourced from the
// shader container's vertex-element table (D3D usage/index = the semantic) joined with
// the vfetch instruction (Xenos vertex format + position). The host maps `format` to a
// DXGI format and `offsetDwords`/`strideDwords` to byte offsets to build the IA layout.
struct VertexInputAttr {
  uint32_t usage;         // Xenos DeclUsage (0=Position,3=Normal,5=TexCoord,10=Color,...)
  uint32_t usageIndex;    // semantic index (POSITION0 vs POSITION1, etc.)
  uint32_t format;        // raw Xenos VertexFormat (e.g. 57 = k_32_32_32_FLOAT)
  uint32_t offsetDwords;  // attribute offset within the vertex, in dwords
  uint32_t strideDwords;  // binding stride, in dwords (0 on mini-fetches)
};

// Translate a Xenos shader container (as bound by SetVertexShader/SetPixelShader)
// to DXIL. `includeHlsl` is the HLSL prelude the recompiled shader #includes
// (XenosRecomp/XenosRecomp/shader_common.h-derived). Returns empty on failure;
// sets isPixelOut. If `vertexLayoutOut` is non-null and the shader is a vertex
// shader, it receives the parsed input-assembler layout (empty for pixel shaders).
std::vector<uint8_t> TranslateXenosShaderToDxil(const uint8_t* shaderContainer,
                                                std::string_view includeHlsl, bool& isPixelOut,
                                                std::vector<VertexInputAttr>* vertexLayoutOut = nullptr);

// Splice a specialized-variant ucode into a template container and translate to DXIL.
// MM's vertex shader containers (flags bit0 set) carry the rich tables (vertex elements,
// constants) but only a TEMPLATE/stub ucode; the GPU runs a specialized variant whose
// ucode the CP thread sees (host_address). This builds a synthetic container = the
// template's virtual region + `ucode` as the physical region (patching physicalSize +
// Shader.physicalOffset=0 + Shader.size), then translates it. `ucode` is big-endian guest
// dwords (as the CP receives it); `ucodeBytes` must be a multiple of 12. Returns empty on
// failure. Sets isPixelOut; fills vertexLayoutOut (VS) if non-null.
std::vector<uint8_t> TranslateSplicedShaderToDxil(const uint8_t* templateContainer,
                                                  const uint8_t* ucode, uint32_t ucodeBytes,
                                                  std::string_view includeHlsl, bool& isPixelOut,
                                                  std::vector<VertexInputAttr>* vertexLayoutOut);

// Cheap (no-DXC) probe: splice `ucode` into `templateContainer`, run only the ucode->HLSL
// recompile, and return how many vertex input attributes resolved (vfetch instructions that
// matched the template's vertex-element table). >0 means the variant ucode belongs to this
// template. Returns -1 if the recompile failed. Safe to call over many templates (no DXC).
int ProbeSplicedVsAttrs(const uint8_t* templateContainer, const uint8_t* ucode,
                        uint32_t ucodeBytes, std::string_view includeHlsl);

// Safely copy up to `n` bytes from a possibly-bad guest pointer (SEH-guarded).
// Returns bytes copied (0 on access fault). For probing what a shader pointer
// references before we know the object layout.
size_t SafePeek(const uint8_t* src, uint8_t* dst, size_t n);

}  // namespace mm
