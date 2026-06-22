// Host presentation via Plume (ported/adapted from ReOdyssey src/render/video.h).
//
// Owns the Plume render interface/device/queue/swapchain and presents frames.
// The native D3D Swap hook drives Video::Present once wired. Until the window is
// handed over from the SDK (config.graphics = null / detached), Init is a no-op
// caller's responsibility.

#pragma once

#include <cstddef>
#include <cstdint>

namespace mm {

struct VertexInputAttr;  // shader_translate.h

struct Video {
  static uint32_t s_viewportWidth;
  static uint32_t s_viewportHeight;

  // Store the window + size from any thread (the app's main thread). The actual
  // Plume init must happen on the CP "GPU Commands" worker thread (see Init).
  static void Configure(void* nativeWindowHandle, uint32_t width, uint32_t height);

  // Create the Plume device/swapchain/RT. MUST be called on the CP worker thread
  // (MMNullCommandProcessor::SetupContext) so all subsequent Plume use is on one
  // thread. Uses the window stored by Configure. Idempotent.
  static bool Init();

  // nativeWindowHandle is the platform window (HWND on Windows).
  static bool Init(void* nativeWindowHandle, uint32_t width, uint32_t height);
  static bool IsInitialized();
  static void Present();       // acquire backbuffer, clear, present
  // Detile a tiled 8.8.8.8 guest surface (e.g. the Swap front buffer / movie
  // frame), upload it, and blit it to the backbuffer. `pitch` = texel pitch.
  static void PresentGuestImage(const uint8_t* tiledData, uint32_t width, uint32_t height,
                                uint32_t pitch);
  // Build the bindless pipeline layout (root signature) matching the XenosRecomp
  // DXIL contract, and validate DXIL shader loading from the cache. Logs results
  // to mm_video.log. Returns true if both succeed. CP-thread only.
  static bool SelfTestPipeline();

  // Create (and cache) a graphics pipeline from a resolved guest VS+PS DXIL pair,
  // using the bindless layout + the offscreen RT format. Keyed by (vsKey,psKey,
  // xenosPrimType). `vtxAttrs` (vtxAttrCount entries) is the VS input-assembler layout
  // parsed from the shader container (real Xenos formats/offsets); when provided the
  // IA layout uses those instead of guessed per-semantic formats. Returns true if a
  // pipeline exists/was created. CP-thread only.
  // `depthState` packs the guest depth state (bit0 z_enable, bit1 z_write, bits2-4 zfunc
  // = Xenos CompareFunction) so a PSO variant is built + keyed per depth state. 0 = depth
  // off (UI/movie). It is part of the pipeline cache key, so Record* must pass the same value.
  static bool GetOrCreateGuestPipeline(const void* vsDxil, uint64_t vsSize, uint64_t vsKey,
                                       const void* psDxil, uint64_t psSize, uint64_t psKey,
                                       uint32_t xenosPrimType,
                                       const VertexInputAttr* vtxAttrs = nullptr,
                                       size_t vtxAttrCount = 0, uint32_t depthState = 0);

  // Record one draw into the offscreen RT using the cached pipeline for (vsKey,psKey,
  // prim,depthState) and the bound constants/descriptors. No-op if no pipeline exists.
  static void RecordGuestDraw(uint64_t vsKey, uint64_t psKey, uint32_t xenosPrimType,
                              uint32_t vertexCount, uint32_t indexCount, uint32_t depthState = 0);

  // Take a fresh per-draw resource slot for the next draw (its own VB/IB + b0/b1/b2 +
  // descriptor set), so draws in a frame don't clobber each other. Call once per draw,
  // before SetDrawConstants/BindGuestTexture/RecordGuestDraw*. The ring resets each frame.
  static void BeginGuestDraw();

  // Window->NDC viewport-transform for the next draw (b2 g_ViewportScaleOffset):
  // oPos.xy = oPos.xy*(sx,sy) + (bx,by)*oPos.w. Pass (1,1,0,0) for identity (the guest's
  // Xenos viewport transform was enabled, so oPos is already NDC); pass the window->NDC
  // scale/bias when the guest disabled it (VTE). Defaults to identity if not called.
  static void SetViewportTransform(float sx, float sy, float bx, float by);

  // Host viewport/scissor for the next recorded draw, recovered from the guest viewport
  // scale/offset (x=xo-xs, y=yo+ys, w=2*xs, h=-2*ys). The title renders elements to several
  // sub-viewports/offscreen RTs; without this they all blow up to full-screen and overlap.
  static void SetGuestViewport(float x, float y, float w, float h);

  // Real Xenos float constants for the next recorded draw. `vsConst`/`psConst` point at
  // the VS/PS float-register region of the register file (host-endian float bits, NOT
  // byte-swapped); they are copied into b0/b1. shared (b2) carries the texture/sampler
  // descriptor indices (set by BindGuestTexture). Pass null to leave a buffer zeroed.
  // NOTE: single shared cbuffers, so within one frame the LAST draw's constants win for
  // every draw (correct for the movie's final fullscreen blit; multi-draw is future work).
  static void SetDrawConstants(const void* vsConst, uint32_t vsFloat4Count, const void* psConst,
                               uint32_t psFloat4Count);

  // Upload a guest texture for pixel-shader slot `shaderSlot` (s0,s1,...) into the bindless
  // SRV heap and record the descriptor index into the shared cbuffer (b2) so the shader's
  // tfetch finds it. `src` is host memory (guest physical, linear); `srcPitchBytes` is the
  // row stride; `xenosFormat` is the Xenos TextureFormat (2=k_8, 6=k_8_8_8_8). For the
  // movie this is the Y/U/V planes (k_8). Must be called on the CP thread before the draw
  // that samples them (it ensures the frame is open and records the upload + barrier).
  // `guestBase` is the texture's guest physical base (fetch base_address<<12). If it names a
  // resolved render target (a prior ResolveColorTo dest), the GPU snapshot is sampled and the
  // `src` upload is skipped — this is the EDRAM resolve->sample link for multi-pass scenes.
  // `srcPitchTexels` is the texture row pitch in TEXELS (fetch pitch<<5). `tiled` selects Xenos
  // 2D detiling. Handles linear k_8 / k_8_8_8_8 and DXT1/DXT2_3/DXT4_5 (BC1/2/3, normally tiled).
  static void BindGuestTexture(uint32_t shaderSlot, const void* src, uint32_t width,
                               uint32_t height, uint32_t srcPitchTexels, int xenosFormat,
                               int xenosEndian, uint32_t guestBase = 0, bool tiled = false);

  // EDRAM resolve emulation: snapshot the current color RT into a texture keyed by `destBase`
  // (RB_COPY_DEST_BASE), so a later draw that samples that address gets the rendered content.
  // Called for kCopy draws (edram_mode 6) instead of recording them as geometry. CP-thread.
  static void ResolveColorTo(uint32_t destBase);

  // True if `guestBase` (fetch base_address<<12) names a resolved render target. Lets the
  // caller bind it (via BindGuestTexture with that base) even for tiled/odd formats that the
  // normal upload path would skip.
  static bool IsResolvedBase(uint32_t guestBase);

  // Record an indexed draw with REAL guest geometry. vbHost/ibHost are host pointers
  // into guest RAM; data is big-endian and byte-swapped on upload. CP/guest-thread.
  static void RecordGuestDrawIndexed(uint64_t vsKey, uint64_t psKey, uint32_t xenosPrimType,
                                     const void* vbHost, uint32_t vbSize, uint32_t stride,
                                     const void* ibHost, uint32_t indexCount, bool ib32,
                                     uint32_t depthState = 0);

  // Non-indexed draw with REAL guest geometry (auto-index draws / the movie quad).
  static void RecordGuestDrawVB(uint64_t vsKey, uint64_t psKey, uint32_t xenosPrimType,
                                const void* vbHost, uint32_t vbSize, uint32_t stride,
                                uint32_t vertexCount, uint32_t startVertex, uint32_t depthState = 0);

  // Cross-thread present coordination: MM issues D3D from multiple threads, but Plume
  // command lists are single-thread. The Swap hook (any thread) calls RequestPresent;
  // the draw/render thread consumes it (ConsumePresentRequest) and presents there, so
  // all Plume use stays on one thread.
  static void RequestPresent();
  static bool ConsumePresentRequest();

  static void WaitForGPU();
  static void Shutdown();
};

}  // namespace mm
