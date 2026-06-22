// Native D3D hook surface for Monster Madness (ReOdyssey-style).
//
// REX_HOOKs the named guest D3D functions (named via monster_madness_config.toml,
// emitted by codegen as rex_D3DDevice_*). For now each hook is a passthrough that
// records the call + its first few argument sets, so we can (a) confirm the whole
// surface is hookable and fires, and (b) empirically validate the medium-
// confidence name mappings by observing sane arguments at runtime. The real Plume
// translation replaces these bodies later.
//
// Swap / InitRingBuffer are hooked in mm_render_hooks.cpp (don't redefine here).
// Function -> address map + confidence: docs/D3D_FUNCTION_MAP.md.

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h>  // GetCurrentThreadId

#include <rex/hook.h>

#include "render/shader_translate.h"
#include "render/video.h"

namespace {

std::mutex g_m;
FILE* g_f = nullptr;
uint64_t g_total = 0;

void HookTrace(const char* name, uint32_t r3, uint32_t r4, uint32_t r5, uint32_t r6) {
  std::lock_guard<std::mutex> lk(g_m);
  if (!g_f) g_f = std::fopen("mm_d3d_hooks_trace.log", "w");
  if (!g_f) return;
  // Log the first 3 calls of each distinct function with args (enough to confirm
  // the mapping); thereafter just keep a running per-frame-ish total.
  static std::unordered_map<std::string, uint32_t> seen;
  uint32_t& n = seen[name];
  if (n < 3) {
    std::fprintf(g_f, "[mm-d3d] %-34s r3=%08X r4=%08X r5=%08X r6=%08X\n", name, r3, r4, r5, r6);
    std::fflush(g_f);
  }
  ++n;
  if ((++g_total % 4000) == 0) {
    std::fprintf(g_f, "---- call totals @ %llu ----\n", (unsigned long long)g_total);
    for (auto& [k, c] : seen) std::fprintf(g_f, "    %-34s %u\n", k.c_str(), c);
    std::fflush(g_f);
  }
}

void ShaderTracef(const char* fmt, ...) {
  std::lock_guard<std::mutex> lk(g_m);
  if (!g_f) g_f = std::fopen("mm_d3d_hooks_trace.log", "w");
  if (!g_f) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(g_f, fmt, ap);
  va_end(ap);
  std::fflush(g_f);
}

// The HLSL prelude XenosRecomp's output is prepended with (shader_common.h),
// loaded once from the source tree.
const std::string& ShaderInclude() {
  static std::string include = [] {
    std::ifstream f(std::string(MONSTER_MADNESS_SOURCE_ROOT) +
                        "/thirdparty/XenosRecomp/XenosRecomp/shader_common.h",
                    std::ios::binary);
    return f ? std::string((std::istreambuf_iterator<char>(f)), {}) : std::string();
  }();
  return include;
}

}  // namespace

// Passthrough trace-hook: define the strong override + call the original __imp__.
#define MM_TRACE_HOOK(fn)                                                  \
  extern "C" REX_FUNC(__imp__##fn);                                        \
  REX_HOOK_RAW(fn) {                                                       \
    HookTrace(#fn, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);        \
    __imp__##fn(ctx, base);                                               \
  }

// --- Draws ---
// r4 = D3DPRIMITIVETYPE for the explicit Draw* calls (maps to our topology). Record
// the draw on the guest thread (no-op unless MM_GUEST_RENDER + both shaders set).
namespace {
void GuestRecordDraw(uint32_t prim, uint32_t startVertex, uint32_t vertexCount,
                     const uint8_t* base);
void GuestRecordDrawIndexed(uint32_t prim, uint32_t baseIndex, uint32_t indexCount,
                            const uint8_t* base);
bool GuestRenderEnabled();
uint32_t PeekBE32(const uint8_t* base, uint32_t guest_addr);
}  // namespace

extern "C" REX_FUNC(__imp__rex_D3DDevice_DrawVertices);
REX_HOOK_RAW(rex_D3DDevice_DrawVertices) {
  HookTrace("rex_D3DDevice_DrawVertices", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  // sub_8265C5C8(device, primType=r4, startVertex=r5, vertexCount=r6).
  GuestRecordDraw(ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, base);
  __imp__rex_D3DDevice_DrawVertices(ctx, base);
}

extern "C" REX_FUNC(__imp__rex_D3DDevice_DrawIndexedVertices);
REX_HOOK_RAW(rex_D3DDevice_DrawIndexedVertices) {
  HookTrace("rex_D3DDevice_DrawIndexedVertices", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  // sub_8265C9B0(device, primType=r4, ?, baseIndex=r6, indexCount=r7).
  GuestRecordDrawIndexed(ctx.r4.u32, ctx.r6.u32, ctx.r7.u32, base);
  __imp__rex_D3DDevice_DrawIndexedVertices(ctx, base);
}

MM_TRACE_HOOK(rex_D3DDevice_DrawVerticesUP)
MM_TRACE_HOOK(rex_D3DDevice_BeginVertices)
MM_TRACE_HOOK(rex_D3DDevice_BeginIndexedVertices)
MM_TRACE_HOOK(rex_D3DDevice_EndVertices)

// --- Binding setters ---
namespace {
// Real geometry captured on the guest thread (stream 0 VB + the index buffer).
uint32_t g_vbAddr = 0, g_vbStride = 0, g_vbSize = 0;
uint32_t g_ibAddr = 0;
bool g_ib32 = false;

// Mask a Xenos GPU address (carries the 0xE0000000 tiled-alias + page bits) to a real
// guest offset; mirrors the device methods' transform.
uint32_t MaskGpuAddr(uint32_t v) { return (((v >> 20) + 512) & 0x1000) + (v & 0x1FFFFFFF); }
}  // namespace

// SetStreamSource(stream=r4, pVB=r5, offset=r6, stride=r7). VB data = *(pVB+24)+offset,
// size = *(pVB+28)-offset (decompiled sub_82204EB8).
extern "C" REX_FUNC(__imp__rex_D3DDevice_SetStreamSource);
REX_HOOK_RAW(rex_D3DDevice_SetStreamSource) {
  HookTrace("rex_D3DDevice_SetStreamSource", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  if (GuestRenderEnabled() && ctx.r4.u32 == 0 && ctx.r5.u32) {
    const uint32_t vb = ctx.r5.u32, off = ctx.r6.u32;
    const uint32_t vbBase = PeekBE32(base, vb + 24), vbEnd = PeekBE32(base, vb + 28);
    g_vbAddr = MaskGpuAddr(vbBase + off);
    g_vbStride = ctx.r7.u32;
    g_vbSize = vbEnd > off ? vbEnd - off : 0;
  }
  __imp__rex_D3DDevice_SetStreamSource(ctx, base);
}

// SetIndices(pIB=r4). IB data = *(pIB+24); 32-bit if *(pIB+0) high bit set (sub_82205060
// stores it; format read in DrawIndexedVertices sub_8265C9B0).
extern "C" REX_FUNC(__imp__rex_D3DDevice_SetIndices);
REX_HOOK_RAW(rex_D3DDevice_SetIndices) {
  HookTrace("rex_D3DDevice_SetIndices", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  if (GuestRenderEnabled() && ctx.r4.u32) {
    const uint32_t ib = ctx.r4.u32;
    g_ibAddr = MaskGpuAddr(PeekBE32(base, ib + 24));
    g_ib32 = (PeekBE32(base, ib + 0) & 0x80000000u) != 0;
  }
  __imp__rex_D3DDevice_SetIndices(ctx, base);
}

MM_TRACE_HOOK(rex_D3DDevice_SetTexture)
MM_TRACE_HOOK(rex_D3DDevice_SetVertexDeclaration)
MM_TRACE_HOOK(rex_D3DDevice_SetViewport)
MM_TRACE_HOOK(rex_D3DDevice_SetRenderTarget)

// CORRECTION: the "SetVertexShader/SetPixelShader" candidates 0x82205770/838 are
// actually SetGammaRamp (sub_8220ADA8 = sRGB gamma table). The real shader load is
// sub_8220CF00 (IM_LOAD): it reads a shader descriptor at *(a3 + 8*(a8+112)) whose
// +872 = microcode data offset, +876 = size. Probe it to capture the real
// container. (We can REX_HOOK the raw sub_ alias directly.)
namespace {
uint32_t PeekBE32(const uint8_t* base, uint32_t guest_addr) {
  uint8_t b[4] = {};
  if (mm::SafePeek(base + guest_addr, b, 4) != 4) return 0;
  return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}
}  // namespace

extern "C" REX_FUNC(__imp__sub_8220CF00);
REX_HOOK_RAW(sub_8220CF00) {
  static int n = 0;
  if (n < 5) {
    ++n;
    const uint32_t a3 = ctx.r5.u32, a8 = ctx.r10.u32;
    const uint32_t desc = PeekBE32(base, a3 + 8u * (a8 + 112u));
    const uint32_t dataOff = desc ? PeekBE32(base, desc + 872u) : 0;
    const uint32_t size = desc ? PeekBE32(base, desc + 876u) : 0;
    uint8_t hdr[24] = {};
    if (desc) mm::SafePeek(base + desc, hdr, sizeof(hdr));
    char hex[sizeof(hdr) * 3 + 1] = {};
    for (size_t i = 0; i < sizeof(hdr); ++i) std::snprintf(hex + i * 3, 4, "%02X ", hdr[i]);
    ShaderTracef("[mm-imload] a3=%08X a8=%u desc=%08X dataOff=%08X size=%u deschdr=[%s]\n", a3, a8,
                 desc, dataOff, size, hex);
  }
  __imp__sub_8220CF00(ctx, base);
}

// Shader setters: trace only. NOTE: these config-named entry points are mis-mapped (the
// real shader setters are sub_822137F8 / sub_82213450, hooked below). They do NOT carry
// shader pointers in r4/r5, so we must NOT treat the args as shader addresses — an earlier
// probe (TranslateShaderOnce) fed `base + r4` into SafePeek + a full DXC translate, which
// crashed at the movie->menu transition (garbage pointer -> std::bad_alloc in DXC). Real
// shader resolution happens on the CP thread (mm_graphics LoadShader + the splice).
extern "C" REX_FUNC(__imp__rex_D3DDevice_SetVertexShader);
REX_HOOK_RAW(rex_D3DDevice_SetVertexShader) {
  HookTrace("rex_D3DDevice_SetVertexShader", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  __imp__rex_D3DDevice_SetVertexShader(ctx, base);
}

extern "C" REX_FUNC(__imp__rex_D3DDevice_SetPixelShader);
REX_HOOK_RAW(rex_D3DDevice_SetPixelShader) {
  HookTrace("rex_D3DDevice_SetPixelShader", ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  __imp__rex_D3DDevice_SetPixelShader(ctx, base);
}

// --- Guest-thread native renderer (the Unleashed model) ---
// The real shader setters (via IDA): sub_822137F8 writes the VS object to
// device+12688, sub_82213450 writes the PS object to device+12684 (r4 = object).
// Each object embeds the rich XenosRecomp container (VS at obj+872, PS at obj+40).
// SetVertexShader/SetPixelShader/SetTexture/Draw*/Swap all run on this one guest
// render thread in order, so we translate + bind + draw + present here (Video moves
// to this thread). Gated behind MM_GUEST_RENDER so the default path stays on the
// CP-thread clear+present until this is complete.
namespace {

bool GuestRenderEnabled() {
  static const bool on = std::getenv("MM_GUEST_RENDER") != nullptr;
  return on;
}

std::unordered_map<uint32_t, std::vector<uint8_t>> g_shaderDxil;  // shader obj -> DXIL (cached)
std::unordered_map<uint32_t, std::vector<mm::VertexInputAttr>> g_shaderLayout;  // obj -> IA layout
const std::vector<uint8_t>* g_curVS = nullptr;
const std::vector<uint8_t>* g_curPS = nullptr;
const std::vector<mm::VertexInputAttr>* g_curVSLayout = nullptr;
uint64_t g_curVSKey = 0, g_curPSKey = 0;

// Translate (once, cached) the container embedded in a shader object to DXIL. Also
// caches the parsed IA layout (vertex shaders) in g_shaderLayout[obj].
const std::vector<uint8_t>* TranslateShaderObject(const uint8_t* base, uint32_t obj) {
  if (!obj) return nullptr;
  auto it = g_shaderDxil.find(obj);
  if (it != g_shaderDxil.end()) return it->second.empty() ? nullptr : &it->second;
  uint32_t magicAt = 0;
  for (uint32_t off = 0; off < 0x1000; off += 4) {
    uint8_t m[4];
    if (mm::SafePeek(base + obj + off, m, 4) != 4) break;
    if (m[0] == 0x10 && m[1] == 0x2A && m[2] == 0x11 && m[3] <= 1) {
      magicAt = obj + off;
      break;
    }
  }
  std::vector<uint8_t> dxil;
  std::vector<mm::VertexInputAttr> layout;
  if (magicAt) {
    bool isPixel = false;
    dxil = mm::TranslateXenosShaderToDxil(base + magicAt, ShaderInclude(), isPixel, &layout);
  }
  g_shaderLayout[obj] = std::move(layout);
  auto& slot = g_shaderDxil[obj];
  slot = std::move(dxil);
  // One-shot: dump the DXBC header (magic + 16-byte hash). A zero hash => UNSIGNED
  // DXIL, which D3D12 rejects with E_INVALIDARG at PSO creation.
  static int dumped = 0;
  if (!slot.empty() && slot.size() >= 20 && dumped < 4) {
    ++dumped;
    char hex[16 * 3 + 1] = {};
    for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 3, 4, "%02X ", slot[4 + i]);
    bool zero = true;
    for (int i = 0; i < 16; ++i)
      if (slot[4 + i]) zero = false;
    ShaderTracef("[mm-dxil] size=%zu magic=%c%c%c%c hash=[%s] %s\n", slot.size(), slot[0], slot[1],
                 slot[2], slot[3], hex, zero ? "UNSIGNED" : "signed");
  }
  return slot.empty() ? nullptr : &slot;
}

// Pin all Plume use to the thread that issues draws (MM uses several D3D threads).
unsigned long g_videoThread = 0;

// Record a non-indexed draw on the (pinned) render thread. Uses the real bound stream
// (g_vbAddr) when available, else a degenerate dummy.
void GuestRecordDraw(uint32_t prim, uint32_t startVertex, uint32_t vertexCount,
                     const uint8_t* base) {
  if (!GuestRenderEnabled() || !g_curVS || !g_curPS) return;
  const unsigned long tid = GetCurrentThreadId();
  if (g_videoThread == 0) g_videoThread = tid;  // first draw thread owns Video
  if (tid != g_videoThread) return;             // drop draws from other threads
  if (!mm::Video::Init()) return;               // lazy init on the pinned thread; idempotent
  // The Swap hook (a different thread) requested a present: finish the previous frame
  // here, on the pinned thread, before starting this frame's draws.
  if (mm::Video::ConsumePresentRequest()) mm::Video::Present();
  static const bool kNoRecord = std::getenv("MM_NO_RECORD") != nullptr;
  if (kNoRecord) return;
  const mm::VertexInputAttr* vsa = g_curVSLayout ? g_curVSLayout->data() : nullptr;
  const size_t vsaCount = g_curVSLayout ? g_curVSLayout->size() : 0;
  mm::Video::GetOrCreateGuestPipeline(g_curVS->data(), g_curVS->size(), g_curVSKey,
                                      g_curPS->data(), g_curPS->size(), g_curPSKey, prim,
                                      vsa, vsaCount);
  if (g_vbAddr && g_vbStride && vertexCount && vertexCount <= 1000000u) {
    mm::Video::RecordGuestDrawVB(g_curVSKey, g_curPSKey, prim, base + g_vbAddr, g_vbSize,
                                 g_vbStride, vertexCount, startVertex);
  } else {
    const uint32_t vc = (vertexCount == 0 || vertexCount > 6) ? 3u : vertexCount;
    mm::Video::RecordGuestDraw(g_curVSKey, g_curPSKey, prim, vc, vc);
  }
}

// Indexed draw with the REAL captured geometry (stream 0 VB + index buffer).
void GuestRecordDrawIndexed(uint32_t prim, uint32_t baseIndex, uint32_t indexCount,
                            const uint8_t* base) {
  if (!GuestRenderEnabled() || !g_curVS || !g_curPS) return;
  const unsigned long tid = GetCurrentThreadId();
  if (g_videoThread == 0) g_videoThread = tid;
  if (tid != g_videoThread) return;
  if (!mm::Video::Init()) return;
  if (mm::Video::ConsumePresentRequest()) mm::Video::Present();
  static const bool kNoRecord = std::getenv("MM_NO_RECORD") != nullptr;
  if (kNoRecord) return;
  if (g_vbAddr == 0 || g_ibAddr == 0 || g_vbStride == 0 || indexCount == 0) return;
  const mm::VertexInputAttr* vsa = g_curVSLayout ? g_curVSLayout->data() : nullptr;
  const size_t vsaCount = g_curVSLayout ? g_curVSLayout->size() : 0;
  mm::Video::GetOrCreateGuestPipeline(g_curVS->data(), g_curVS->size(), g_curVSKey,
                                      g_curPS->data(), g_curPS->size(), g_curPSKey, prim,
                                      vsa, vsaCount);
  const uint32_t idxSize = g_ib32 ? 4u : 2u;
  const uint8_t* vbHost = base + g_vbAddr;
  const uint8_t* ibHost = base + g_ibAddr + baseIndex * idxSize;
  mm::Video::RecordGuestDrawIndexed(g_curVSKey, g_curPSKey, prim, vbHost, g_vbSize, g_vbStride,
                                    ibHost, indexCount, g_ib32);
}

}  // namespace

extern "C" REX_FUNC(__imp__sub_822137F8);  // real SetVertexShader
REX_HOOK_RAW(sub_822137F8) {
  const uint32_t obj = ctx.r4.u32;
  if (GuestRenderEnabled()) {
    g_curVS = TranslateShaderObject(base, obj);
    auto lit = g_shaderLayout.find(obj);
    g_curVSLayout = (lit != g_shaderLayout.end()) ? &lit->second : nullptr;
    g_curVSKey = obj;
  }
  __imp__sub_822137F8(ctx, base);
}

extern "C" REX_FUNC(__imp__sub_82213450);  // real SetPixelShader
REX_HOOK_RAW(sub_82213450) {
  const uint32_t obj = ctx.r4.u32;
  if (GuestRenderEnabled()) {
    g_curPS = TranslateShaderObject(base, obj);
    g_curPSKey = obj;
  }
  __imp__sub_82213450(ctx, base);
}
