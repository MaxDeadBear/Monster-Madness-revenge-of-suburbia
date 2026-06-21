// Native-mode render hooks for Monster Madness.
//
// The render path is driven by a null-consumer GraphicsSystem (see mm_graphics.*,
// installed in monster_madness_app.h OnPreSetup): its CommandProcessor drains the
// ring and services the bookkeeping the guest waits on, so the game runs
// frame-by-frame without the SDK doing any actual rendering. These hooks observe
// the D3D seams (and will later take over drawing).
//
// Interception mechanism: codegen emits each guest function `sub_X` as a WEAK
// alias to its real body `__imp__sub_X`. REX_HOOK_RAW defines `sub_X` STRONG so
// the linker prefers it; the original stays callable as `__imp__sub_X`.
//
// Seam addresses + device-struct offsets: docs/RENDERER_MAPPING.md

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <windows.h>

#include <rex/hook.h>

#include "render/renderdoc_app.h"
#include "render/shader_cache_runtime.h"
#include "render/shader_translate.h"  // mm::SafePeek
#include "render/video.h"

namespace {
// Read a big-endian u32 from guest memory (SEH-safe).
uint32_t GuestBE32(const uint8_t* base, uint32_t addr) {
  uint8_t b[4];
  if (mm::SafePeek(base + addr, b, 4) != 4) return 0;
  return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
}
}  // namespace

namespace {

// Raw, spdlog-independent trace (the SDK async file logger writes empty files in
// native mode); plain flushed file next to the exe.
void RawTracef(const char* fmt, ...) {
  static FILE* f = std::fopen("mm_render_trace.log", "w");
  static std::mutex m;
  if (!f) return;
  std::lock_guard<std::mutex> lk(m);
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fflush(f);
}

// RenderDoc in-app capture: when MM runs under RenderDoc (renderdoccmd inject),
// auto-trigger a frame capture so we can inspect the real frame via the MCP.
RENDERDOC_API_1_4_1* g_rdoc = nullptr;
void RdocInit() {
  HMODULE mod = GetModuleHandleA("renderdoc.dll");
  if (!mod) return;
  auto getApi = reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(mod, "RENDERDOC_GetAPI"));
  if (getApi && getApi(eRENDERDOC_API_Version_1_4_1, reinterpret_cast<void**>(&g_rdoc)) == 1 &&
      g_rdoc) {
    g_rdoc->SetCaptureFilePathTemplate("G:\\rex\\MonsterMadness\\assets\\mm_capture");
    RawTracef("[mm-render] RenderDoc API loaded; captures -> assets\\mm_capture_*.rdc\n");
  }
}

}  // namespace

// Original (weak) recompiled bodies, reachable via their __imp__ symbol. These
// are now named via monster_madness_config.toml (was sub_822122D0/sub_8220A800).
extern "C" REX_FUNC(__imp__rex_D3DDevice_Swap);          // D3DDevice::Swap / Present
extern "C" REX_FUNC(__imp__rex_D3DDevice_InitRingBuffer);  // ring-buffer / device init

// Present / Swap. r3 = guest D3D device object pointer (capture before passthrough).
REX_HOOK_RAW(rex_D3DDevice_Swap) {
  const uint32_t device = ctx.r3.u32;
  static uint32_t frame = 0;
  if (frame == 0 || (frame % 60) == 0) {
    RawTracef("[mm-render] Present #%u device=0x%08X\n", frame, device);
  }

  // Decode the Swap front-buffer Xenos texture fetch (6 BE dwords at a2+28) so we
  // can present the guest image (the XMV intro movies render into this surface).
  const uint32_t a2 = ctx.r4.u32;
  const uint32_t d0 = GuestBE32(base, a2 + 28), d1 = GuestBE32(base, a2 + 32),
                 d2 = GuestBE32(base, a2 + 36);
  // The fetch base carries the 0xE0000000 tiled-memory alias flag; mask it off
  // to get the real guest physical offset (0xFF25F000 -> 0x1F25F000, matching the
  // PM4 SWAP packet's frontbuffer pointer).
  const uint32_t fbAddr = ((d1 >> 12) << 12) & 0x1FFFFFFFu;  // base address (4KB granular)
  const uint32_t fmt = d1 & 0x3F;                 // 6 = 8.8.8.8
  const uint32_t tiled = (d0 >> 31) & 1;
  const uint32_t pitchTiles = (d0 >> 22) & 0x1FF; // pitch in 32-texel tiles
  const uint32_t fbW = (d2 & 0x1FFF) + 1;
  const uint32_t fbH = ((d2 >> 13) & 0x1FFF) + 1;
  if (frame < 4) {
    RawTracef("[mm-fb] fb=0x%08X fmt=%u tiled=%u pitch=%u %ux%u (d0=%08X d1=%08X d2=%08X)\n",
              fbAddr, fmt, tiled, pitchTiles, fbW, fbH, d0, d1, d2);
  }
  if (frame == 1) RdocInit();

  // Scan guest memory for loaded Xenos shader containers at several points (the
  // game plays XMV intro movies first, so the game shaders load later). Confirms
  // their XXH3 hashes match our precompiled DXIL cache. Fast (VirtualQuery-based).
  if (frame == 120 || frame == 600 || frame == 1500 || frame == 3000) {
    size_t m = mm::ScanForShaders(base, 0x00010000u, 0xF0000000u);
    RawTracef("[mm-render] shader scan @frame %u: %zu matched the cache\n", frame, m);
  }
  if (g_rdoc && frame == 240) {
    g_rdoc->TriggerCapture();  // captures the next presented frame -> .rdc
    RawTracef("[mm-render] RenderDoc TriggerCapture @ frame 240\n");
  }
  ++frame;
  __imp__rex_D3DDevice_Swap(ctx, base);

  // Guest-thread render mode (MM_GUEST_RENDER): MM issues D3D from multiple threads, so
  // we only SIGNAL a present here (this Swap may be a different thread than the draws);
  // the pinned render thread performs it on its next draw. Else the CP thread presents.
  static const bool kGuestRender = std::getenv("MM_GUEST_RENDER") != nullptr;
  if (kGuestRender) {
    mm::Video::RequestPresent();
  }
}

// D3DDevice ring-buffer / device init. r3 = device, r4 = params block.
REX_HOOK_RAW(rex_D3DDevice_InitRingBuffer) {
  RawTracef("[mm-render] D3D device init device=0x%08X params=0x%08X\n", ctx.r3.u32, ctx.r4.u32);
  RawTracef("[mm-render] shader cache: %zu entries\n", mm::InitShaderCache());
  __imp__rex_D3DDevice_InitRingBuffer(ctx, base);  // passthrough
}
