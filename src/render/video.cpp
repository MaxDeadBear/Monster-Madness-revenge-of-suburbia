// See video.h. Ported/adapted from ReOdyssey src/render/video.cpp.

#include "render/video.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <plume_render_interface.h>

#include "render/detile.h"
#include "render/shader_cache_runtime.h"
#include "render/shader_translate.h"  // VertexInputAttr

// The interface factories are defined in plume_d3d12.cpp / plume_vulkan.cpp but
// not declared in the public header; forward-declare them.
namespace plume {
std::unique_ptr<RenderInterface> CreateD3D12Interface();
std::unique_ptr<RenderInterface> CreateVulkanInterface();
}  // namespace plume

using namespace plume;

namespace mm {

uint32_t Video::s_viewportWidth = 1280;
uint32_t Video::s_viewportHeight = 720;

namespace {

constexpr uint32_t kBufferCount = 2;

std::unique_ptr<RenderInterface> g_interface;
std::unique_ptr<RenderDevice> g_device;
std::unique_ptr<RenderCommandQueue> g_queue;
std::unique_ptr<RenderCommandList> g_commandList;
std::unique_ptr<RenderSwapChain> g_swapChain;
std::unique_ptr<RenderCommandFence> g_fence;
std::unique_ptr<RenderCommandSemaphore> g_acquireSemaphore;
std::unique_ptr<RenderCommandSemaphore> g_drawSemaphore;
std::vector<std::unique_ptr<RenderFramebuffer>> g_framebuffers;
std::unique_ptr<RenderBuffer> g_uploadBuffer;  // staging for guest-image blits
uint32_t g_uploadW = 0, g_uploadH = 0;
// Offscreen color target the guest draws render into; blitted to the backbuffer
// at present. Stable 1280x720 regardless of window/backbuffer size.
std::unique_ptr<RenderTexture> g_guestRT;
std::unique_ptr<RenderFramebuffer> g_guestRTFb;
uint32_t g_guestRTW = 0, g_guestRTH = 0;
// Bindless root signature shared by all translated guest pipelines.
std::unique_ptr<RenderPipelineLayout> g_guestPipelineLayout;
// Caches keyed by content hash (shaders) and pipeline key.
std::unordered_map<uint64_t, std::unique_ptr<RenderShader>> g_shaderCache;
std::unordered_map<uint64_t, std::unique_ptr<RenderPipeline>> g_pipelineCache;
bool g_initialized = false;
bool g_commandsInFlight = false;
bool g_frameOpen = false;  // command list is recording a guest frame into the RT

// Per-draw binding resources. All draws in a frame record into one command list that
// executes at Present, so each draw needs its OWN resources or they clobber each other
// (the last write would win for every draw -> flashing). We keep a RING of kDrawRing draw
// slots, each with its own VB/IB + b0/b1/b2 constant buffers + a set-4 (CBV) descriptor
// set; a slot is taken per draw (BeginGuestDraw) and the ring resets at frame start.
constexpr uint64_t kVsCbBytes = 256ull * 16;   // 256 float4 VS constants
constexpr uint64_t kPsCbBytes = 224ull * 16;   // 224 float4 PS constants
constexpr uint64_t kSharedCbBytes = 512;       // shared constants block
constexpr uint32_t kViewportSOByteOffset = 320;  // g_ViewportScaleOffset (c20) in b2
constexpr uint32_t kDrawRing = 64;             // max distinct draws recorded per frame

// Write the window->NDC viewport-transform float4 into a shared (b2) buffer at c20.
inline void WriteViewportSO(void* mapped, float sx, float sy, float bx, float by) {
  float* v = reinterpret_cast<float*>(static_cast<uint8_t*>(mapped) + kViewportSOByteOffset);
  v[0] = sx; v[1] = sy; v[2] = bx; v[3] = by;
}

std::unique_ptr<RenderBuffer> g_vsCb[kDrawRing], g_psCb[kDrawRing], g_sharedCb[kDrawRing];
std::unique_ptr<RenderDescriptorSet> g_set4[kDrawRing];  // per-slot CBV set (space 4)
std::unique_ptr<RenderBuffer> g_drawVB[kDrawRing], g_drawIB[kDrawRing];
uint32_t g_drawVBCap[kDrawRing] = {}, g_drawIBCap[kDrawRing] = {};
uint32_t g_curSlot = 0;    // draw slot in use (set by BeginGuestDraw)
uint32_t g_drawSlot = 0;   // next free draw slot this frame
bool g_ringOverflow = false;

std::unique_ptr<RenderBuffer> g_dummyVB;  // zeroed VB so input-layout draws don't fault
std::unique_ptr<RenderTexture> g_dummyTex;
std::unique_ptr<RenderTextureView> g_dummyView;
std::unique_ptr<RenderSampler> g_dummySampler;
std::unique_ptr<RenderDescriptorSet> g_set[4];  // shared bindless sets (spaces 0-3)

// Bindless SRV heap (set 0): a pool of texture objects keyed by heap slot. Each draw's
// textures take fresh slots (g_texHeapNext, reset per frame; slot 0 = dummy). Heap slots
// recur in the same order each frame, so a pool entry is reused + re-uploaded.
constexpr uint32_t kTexHeapSize = 256;
std::unique_ptr<RenderTexture> g_texPool[kTexHeapSize];
std::unique_ptr<RenderTextureView> g_texPoolView[kTexHeapSize];
std::unique_ptr<RenderBuffer> g_texPoolUpload[kTexHeapSize];
uint32_t g_texPoolW[kTexHeapSize] = {};
uint32_t g_texPoolH[kTexHeapSize] = {};
int g_texPoolFmt[kTexHeapSize] = {};
uint32_t g_texHeapNext = 1;  // next free heap slot this frame (0 = dummy)

bool g_drawResourcesReady = false;
bool g_dummyTransitioned = false;

// Map a Xenos primitive type to a Plume topology (for pipeline state).
RenderPrimitiveTopology XenosPrimToTopology(uint32_t prim) {
  switch (prim) {
    case 1:  return RenderPrimitiveTopology::POINT_LIST;
    case 2:  return RenderPrimitiveTopology::LINE_LIST;
    case 3:  return RenderPrimitiveTopology::LINE_STRIP;
    case 6:  return RenderPrimitiveTopology::TRIANGLE_STRIP;
    default: return RenderPrimitiveTopology::TRIANGLE_LIST;  // 4=tri list, 8=rect, 13=quad
  }
}

uint32_t Rd32LE(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

// One input the VS reads from the input assembler (semantic name + index), parsed
// from the DXIL container's input-signature chunk.
struct VsInputSem {
  std::string name;
  uint32_t index;
  uint32_t componentType = 3;  // DXBC: 1=UINT32, 2=SINT32, 3=FLOAT32
};

// Parse a DXIL (DXBC container) signature chunk into its semantics, skipping
// system-value entries (SV_Position etc.). `output`=false reads the INPUT signature
// (ISGN/ISG1); `output`=true reads the OUTPUT signature (OSGN/OSG1).
bool ParseSignature(const uint8_t* dxil, size_t size, bool output, std::vector<VsInputSem>& out,
                    bool skipSystemValues = true) {
  if (!dxil || size < 32 || std::memcmp(dxil, "DXBC", 4) != 0) return false;
  const char* fcc32 = output ? "OSG1" : "ISG1";
  const char* fcc24 = output ? "OSGN" : "ISGN";
  const uint32_t chunkCount = Rd32LE(dxil + 28);
  if (uint64_t(32) + uint64_t(chunkCount) * 4 > size) return false;
  for (uint32_t i = 0; i < chunkCount; ++i) {
    const uint32_t off = Rd32LE(dxil + 32 + i * 4);
    if (uint64_t(off) + 8 > size) continue;
    const bool isg1 = std::memcmp(dxil + off, fcc32, 4) == 0;
    const bool isgn = std::memcmp(dxil + off, fcc24, 4) == 0;
    if (!isg1 && !isgn) continue;
    const uint32_t chunkSize = Rd32LE(dxil + off + 4);
    const uint8_t* data = dxil + off + 8;
    if (uint64_t(off) + 8 + chunkSize > size || chunkSize < 8) return false;
    const uint32_t count = Rd32LE(data);
    const uint32_t stride = isg1 ? 32u : 24u;        // ISG1 adds Stream + MinPrecision
    const uint32_t nameOffField = isg1 ? 4u : 0u;    // nameOffset is after Stream for ISG1
    const uint32_t sysValField = isg1 ? 12u : 8u;
    for (uint32_t e = 0; e < count; ++e) {
      const uint64_t elBase = 8ull + uint64_t(e) * stride;
      if (elBase + stride > chunkSize) break;
      const uint8_t* el = data + elBase;
      const uint32_t nameOff = Rd32LE(el + nameOffField);
      const uint32_t semIdx = Rd32LE(el + nameOffField + 4);
      const uint32_t sysVal = Rd32LE(el + sysValField);
      const uint32_t compType = Rd32LE(el + sysValField + 4);  // 1=uint,2=sint,3=float
      if (skipSystemValues && sysVal != 0) continue;  // skip SV_Position etc.
      if (nameOff >= chunkSize) continue;
      const char* name = reinterpret_cast<const char*>(data + nameOff);
      if (std::memchr(name, '\0', chunkSize - nameOff) == nullptr) continue;
      out.push_back({std::string(name), semIdx, compType});
    }
    return true;
  }
  return false;
}

// Build the pipeline layout matching the XenosRecomp DXIL contract:
//   set 0 -> space0 : Texture2D    g_Texture2DDescriptorHeap[]      (t0, boundless)
//   set 1 -> space1 : Texture2DArray g_Texture2DArrayDescriptorHeap[] (t0, boundless)
//   set 2 -> space2 : TextureCube  g_TextureCubeDescriptorHeap[]    (t0, boundless)
//   set 3 -> space3 : SamplerState g_SamplerDescriptorHeap[]        (s0, boundless)
//   set 4 -> space4 : cbuffers VertexShaderConstants(b0)/Pixel(b1)/Shared(b2)
// (Plume maps descriptor-set index i to HLSL register space i.)
bool EnsureGuestPipelineLayout() {
  if (g_guestPipelineLayout) return true;
  if (!g_device) return false;

  constexpr uint32_t kHeapSize = 4096;  // bindless heap capacity
  const RenderDescriptorRange tex2dRange(RenderDescriptorRangeType::TEXTURE, 0, 1);
  const RenderDescriptorRange tex2dArrayRange(RenderDescriptorRangeType::TEXTURE, 0, 1);
  const RenderDescriptorRange texCubeRange(RenderDescriptorRangeType::TEXTURE, 0, 1);
  const RenderDescriptorRange samplerRange(RenderDescriptorRangeType::SAMPLER, 0, 1);
  const RenderDescriptorRange cbvRanges[3] = {
      RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 0, 1),
      RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 1, 1),
      RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 2, 1),
  };
  const RenderDescriptorSetDesc setDescs[5] = {
      RenderDescriptorSetDesc(&tex2dRange, 1, true, kHeapSize),
      RenderDescriptorSetDesc(&tex2dArrayRange, 1, true, kHeapSize),
      RenderDescriptorSetDesc(&texCubeRange, 1, true, kHeapSize),
      RenderDescriptorSetDesc(&samplerRange, 1, true, kHeapSize),
      RenderDescriptorSetDesc(cbvRanges, 3, false, 0),
  };
  RenderPipelineLayoutDesc layoutDesc;
  layoutDesc.descriptorSetDescs = setDescs;
  layoutDesc.descriptorSetDescsCount = 5;
  layoutDesc.allowInputLayout = true;  // required: PSOs use an input-assembler vertex layout
  g_guestPipelineLayout = g_device->createPipelineLayout(layoutDesc);
  return g_guestPipelineLayout != nullptr;
}

void Log(const char* msg) {
  static FILE* f = std::fopen("mm_video.log", "w");
  if (f) {
    std::fprintf(f, "%s\n", msg);
    std::fflush(f);
  }
}

}  // namespace

namespace {
void* g_pendingHwnd = nullptr;
}  // namespace

void Video::Configure(void* nativeWindowHandle, uint32_t width, uint32_t height) {
  g_pendingHwnd = nativeWindowHandle;
  s_viewportWidth = width;
  s_viewportHeight = height;
}

bool Video::Init() { return Init(g_pendingHwnd, s_viewportWidth, s_viewportHeight); }

bool Video::Init(void* nativeWindowHandle, uint32_t width, uint32_t height) {
  if (g_initialized) return true;
  if (!nativeWindowHandle) {
    Log("Video: Init called with no window");
    return false;
  }
  s_viewportWidth = width;
  s_viewportHeight = height;

  g_interface = CreateD3D12Interface();
  if (!g_interface) {
    Log("Video: no D3D12 interface, trying Vulkan");
    g_interface = CreateVulkanInterface();
  }
  if (!g_interface) {
    Log("Video: failed to create a render interface");
    return false;
  }

  g_device = g_interface->createDevice();
  if (!g_device) {
    Log("Video: createDevice failed");
    return false;
  }

  g_queue = g_device->createCommandQueue(RenderCommandListType::DIRECT);
  g_commandList = g_queue->createCommandList();
  g_fence = g_device->createCommandFence();
  g_acquireSemaphore = g_device->createCommandSemaphore();
  g_drawSemaphore = g_device->createCommandSemaphore();

  RenderSwapChainDesc scDesc;
  scDesc.renderWindow = static_cast<RenderWindow>(nativeWindowHandle);
  scDesc.format = RenderFormat::B8G8R8A8_UNORM;
  scDesc.textureCount = kBufferCount;
  g_swapChain = g_queue->createSwapChain(scDesc);
  if (!g_swapChain) {
    Log("Video: createSwapChain failed");
    return false;
  }

  // One framebuffer per backbuffer (for the clear/present pass).
  for (uint32_t i = 0; i < g_swapChain->getTextureCount(); ++i) {
    RenderTexture* tex = g_swapChain->getTexture(i);
    const RenderTexture* colors[] = {tex};
    g_framebuffers.push_back(g_device->createFramebuffer(RenderFramebufferDesc(colors, 1)));
  }

  // Offscreen render target the guest frame is composited into.
  g_guestRTW = width;
  g_guestRTH = height;
  g_guestRT = g_device->createTexture(
      RenderTextureDesc::ColorTarget(g_guestRTW, g_guestRTH, RenderFormat::B8G8R8A8_UNORM));
  {
    const RenderTexture* colors[] = {g_guestRT.get()};
    g_guestRTFb = g_device->createFramebuffer(RenderFramebufferDesc(colors, 1));
  }

  g_initialized = true;
  {
    char m[96];
    std::snprintf(m, sizeof(m), "Video: initialized (Plume) on thread %lu",
                  (unsigned long)GetCurrentThreadId());
    Log(m);
  }
  return true;
}

bool Video::IsInitialized() { return g_initialized; }

// Begin recording a guest frame into the offscreen RT (idempotent within a frame).
// Opens the command list, clears the RT, sets the framebuffer + full viewport so
// subsequent RecordGuestDraw calls render into it.
void EnsureFrameStarted() {
  if (g_frameOpen) return;
  // New frame: reset the per-draw ring + bindless texture-slot allocator.
  g_drawSlot = 0;
  g_curSlot = 0;
  g_texHeapNext = 1;  // slot 0 stays the dummy
  g_commandList->begin();
  g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(g_guestRT.get(), RenderTextureLayout::COLOR_WRITE));
  g_commandList->setFramebuffer(g_guestRTFb.get());
  g_commandList->clearColor(0, RenderColor(0.05f, 0.20f, 0.08f, 1.0f));
  const RenderViewport vp(0.0f, 0.0f, float(g_guestRTW), float(g_guestRTH));
  const RenderRect sc(0, 0, int32_t(g_guestRTW), int32_t(g_guestRTH));
  g_commandList->setViewports(vp);
  g_commandList->setScissors(sc);
  g_frameOpen = true;
}

void Video::Present() {
  if (!g_initialized) return;
  EnsureFrameStarted();  // ensures the RT is cleared even on no-draw frames

  uint32_t index = 0;
  if (!g_swapChain->acquireTexture(g_acquireSemaphore.get(), &index)) {
    return;
  }
  RenderTexture* back = g_swapChain->getTexture(index);

  // Close the render pass, then blit the RT to the acquired backbuffer.
  g_commandList->setFramebuffer(nullptr);
  RenderTextureBarrier toCopy[] = {
      RenderTextureBarrier(g_guestRT.get(), RenderTextureLayout::COPY_SOURCE),
      RenderTextureBarrier(back, RenderTextureLayout::COPY_DEST)};
  g_commandList->barriers(RenderBarrierStage::COPY, toCopy, 2);

  const uint32_t cw = g_guestRTW < g_swapChain->getWidth() ? g_guestRTW : g_swapChain->getWidth();
  const uint32_t ch = g_guestRTH < g_swapChain->getHeight() ? g_guestRTH : g_swapChain->getHeight();
  const RenderBox srcBox(0, 0, int32_t(cw), int32_t(ch), 0, 1);
  g_commandList->copyTextureRegion(RenderTextureCopyLocation::Subresource(back),
                                   RenderTextureCopyLocation::Subresource(g_guestRT.get()), 0, 0, 0,
                                   &srcBox);

  g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(back, RenderTextureLayout::PRESENT));
  g_commandList->end();
  g_frameOpen = false;

  const RenderCommandList* lists[] = {g_commandList.get()};
  RenderCommandSemaphore* waits[] = {g_acquireSemaphore.get()};
  RenderCommandSemaphore* signals[] = {g_drawSemaphore.get()};
  g_queue->executeCommandLists(lists, 1, waits, 1, signals, 1, g_fence.get());
  g_commandsInFlight = true;

  RenderCommandSemaphore* presentWaits[] = {g_drawSemaphore.get()};
  g_swapChain->present(index, presentWaits, 1);

  g_queue->waitForCommandFence(g_fence.get());
  g_commandsInFlight = false;
}

void Video::PresentGuestImage(const uint8_t* tiledData, uint32_t width, uint32_t height,
                              uint32_t pitch) {
  if (!g_initialized || !tiledData) {
    Present();
    return;
  }
  // D3D12 placed-footprint rows must be 256-byte aligned; width*4 is aligned for
  // width multiples of 64 (the 1280-wide front buffer qualifies).
  const uint32_t rowTexels = width;
  const uint64_t uploadSize = uint64_t(rowTexels) * height * 4;
  if (!g_uploadBuffer || g_uploadW != width || g_uploadH != height) {
    g_uploadBuffer = g_device->createBuffer(RenderBufferDesc::UploadBuffer(uploadSize));
    g_uploadW = width;
    g_uploadH = height;
  }
  if (!g_uploadBuffer) {
    Present();
    return;
  }

  // Detile the guest surface straight into the mapped staging buffer.
  if (uint8_t* mapped = static_cast<uint8_t*>(g_uploadBuffer->map())) {
    DetileTexture8888(mapped, tiledData, width, height, pitch);
    g_uploadBuffer->unmap();
  }

  uint32_t index = 0;
  if (!g_swapChain->acquireTexture(g_acquireSemaphore.get(), &index)) {
    return;
  }
  RenderTexture* tex = g_swapChain->getTexture(index);
  const uint32_t scW = g_swapChain->getWidth();
  const uint32_t scH = g_swapChain->getHeight();
  const int32_t cw = int32_t(width < scW ? width : scW);
  const int32_t ch = int32_t(height < scH ? height : scH);

  g_commandList->begin();
  g_commandList->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(tex, RenderTextureLayout::COPY_DEST));
  RenderTextureCopyLocation dst = RenderTextureCopyLocation::Subresource(tex);
  RenderTextureCopyLocation src = RenderTextureCopyLocation::PlacedFootprint(
      g_uploadBuffer.get(), RenderFormat::B8G8R8A8_UNORM, width, height, 1, rowTexels);
  const RenderBox srcBox(0, 0, cw, ch, 0, 1);
  g_commandList->copyTextureRegion(dst, src, 0, 0, 0, &srcBox);
  g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(tex, RenderTextureLayout::PRESENT));
  g_commandList->end();

  const RenderCommandList* lists[] = {g_commandList.get()};
  RenderCommandSemaphore* waits[] = {g_acquireSemaphore.get()};
  RenderCommandSemaphore* signals[] = {g_drawSemaphore.get()};
  g_queue->executeCommandLists(lists, 1, waits, 1, signals, 1, g_fence.get());
  g_commandsInFlight = true;

  RenderCommandSemaphore* presentWaits[] = {g_drawSemaphore.get()};
  g_swapChain->present(index, presentWaits, 1);

  g_queue->waitForCommandFence(g_fence.get());
  g_commandsInFlight = false;
}

bool Video::SelfTestPipeline() {
  char msg[256];
  if (!g_initialized) {
    Log("SelfTestPipeline: Video not initialized");
    return false;
  }

  const bool layoutOk = EnsureGuestPipelineLayout();
  std::snprintf(msg, sizeof(msg), "SelfTestPipeline: bindless pipeline layout %s",
                layoutOk ? "CREATED" : "FAILED");
  Log(msg);

  // Validate DXIL shader loading from the cache (does not validate against the
  // root signature; that happens at pipeline creation once VS/PS pairs resolve).
  const RenderShaderFormat fmt = g_interface->getCapabilities().shaderFormat;
  uint32_t dxilSize = 0;
  const uint8_t* dxil = GetCacheDxilByIndex(0, dxilSize);
  bool shaderOk = false;
  if (dxil && dxilSize) {
    if (auto shader = g_device->createShader(dxil, dxilSize, "shaderMain", fmt)) {
      shaderOk = true;  // shader released at scope end (validation only)
    }
  }
  std::snprintf(msg, sizeof(msg), "SelfTestPipeline: createShader(cache[0], %u bytes, fmt=%d) %s",
                dxilSize, int(fmt), shaderOk ? "OK" : "FAILED");
  Log(msg);

  return layoutOk && shaderOk;
}

namespace {

// Xenos DeclUsage -> D3D semantic name. Matches XenosRecomp's USAGE_SEMANTICS, which is
// exactly what the generated DXIL ISGN carries — so we can join a parsed input attribute
// to its ISGN entry by (semantic name, index).
const char* DeclUsageToSemantic(uint32_t usage) {
  switch (usage) {
    case 0: return "POSITION";
    case 1: return "BLENDWEIGHT";
    case 2: return "BLENDINDICES";
    case 3: return "NORMAL";
    case 4: return "PSIZE";
    case 5: return "TEXCOORD";
    case 6: return "TANGENT";
    case 7: return "BINORMAL";
    case 8: return "TESSFACTOR";
    case 9: return "POSITIONT";
    case 10: return "COLOR";
    case 11: return "FOG";
    case 12: return "DEPTH";
    case 13: return "SAMPLE";
    default: return "";
  }
}

// Xenos (DeclUsage, usageIndex) -> Vulkan input location, matching XenosRecomp's
// USAGE_LOCATIONS. Only meaningful for the SPIR-V backend (DXIL matches by semantic and
// ignores location); returns UINT32_MAX when there is no fixed slot.
uint32_t DeclUsageToLocation(uint32_t usage, uint32_t usageIndex) {
  struct L { uint32_t usage, idx, loc; };
  static const L kLoc[] = {
      {0, 0, 0},  {0, 1, 1},  {0, 2, 2},  {0, 3, 3},   // Position 0..3
      {3, 0, 4},  {3, 1, 5},  {3, 2, 6},  {3, 3, 7},   // Normal 0..3
      {6, 0, 8},  {6, 1, 9},  {6, 2, 10}, {6, 3, 11},  // Tangent 0..3
      {7, 0, 12},                                       // Binormal 0
      {5, 0, 13}, {5, 1, 14}, {5, 2, 15}, {5, 3, 16},  // TexCoord 0..3
      {10, 0, 17},                                      // Color 0
      {2, 0, 18},                                       // BlendIndices 0
      {1, 0, 19},                                       // BlendWeight 0
      {5, 5, 20}, {5, 6, 21}, {5, 7, 22},               // TexCoord 5..7
      {10, 1, 23},                                      // Color 1
      {12, 1, 24},                                      // Depth 1
      {6, 4, 25}, {6, 5, 26}, {6, 6, 27}, {6, 7, 28},  // Tangent 4..7
      {7, 1, 29}, {7, 2, 30}, {7, 3, 31},               // Binormal 1..3
  };
  for (const auto& e : kLoc)
    if (e.usage == usage && e.idx == usageIndex) return e.loc;
  return 0xFFFFFFFFu;
}

// Map a raw Xenos vertex format to a Plume RenderFormat + byte size. `uintReg` is true
// when the DXIL input register is integer-typed (e.g. POSITION0, which the shader reads
// as uint4 and bit-casts via tfetchPos3N) -> use the *_UINT variant so the IA delivers
// raw bits; float registers use the float/normalized variant. (Values are
// xenos::VertexFormat; see SDK xenos.h.) Returns false for unmapped formats.
//
// NOTE: the guest VB is uploaded byte-swapped at dword granularity, which is correct for
// 32-bit-per-component formats. Sub-dword packed formats (16/8-bit, 2_10_10_10) need the
// shader's g_Swapped* spec-constant correction, which is 0 in our translate today; they
// map best-effort and are exercised by non-movie draws later.
bool XenosVertexFormatToRender(uint32_t xfmt, bool uintReg, RenderFormat& outFmt,
                               uint32_t& outBytes) {
  switch (xfmt) {
    case 36:  // k_32_FLOAT
      outFmt = uintReg ? RenderFormat::R32_UINT : RenderFormat::R32_FLOAT; outBytes = 4; return true;
    case 37:  // k_32_32_FLOAT
      outFmt = uintReg ? RenderFormat::R32G32_UINT : RenderFormat::R32G32_FLOAT; outBytes = 8; return true;
    case 57:  // k_32_32_32_FLOAT
      outFmt = uintReg ? RenderFormat::R32G32B32_UINT : RenderFormat::R32G32B32_FLOAT;
      outBytes = 12; return true;
    case 38:  // k_32_32_32_32_FLOAT
      outFmt = uintReg ? RenderFormat::R32G32B32A32_UINT : RenderFormat::R32G32B32A32_FLOAT;
      outBytes = 16; return true;
    case 33:  // k_32 (integer)
      outFmt = RenderFormat::R32_UINT; outBytes = 4; return true;
    case 34:  // k_32_32 (integer)
      outFmt = RenderFormat::R32G32_UINT; outBytes = 8; return true;
    case 35:  // k_32_32_32_32 (integer)
      outFmt = RenderFormat::R32G32B32A32_UINT; outBytes = 16; return true;
    case 31:  // k_16_16_FLOAT
      outFmt = RenderFormat::R16G16_FLOAT; outBytes = 4; return true;
    case 32:  // k_16_16_16_16_FLOAT
      outFmt = RenderFormat::R16G16B16A16_FLOAT; outBytes = 8; return true;
    case 25:  // k_16_16 (signed-normalized typical)
      outFmt = RenderFormat::R16G16_SNORM; outBytes = 4; return true;
    case 26:  // k_16_16_16_16
      outFmt = RenderFormat::R16G16B16A16_SNORM; outBytes = 8; return true;
    case 6:   // k_8_8_8_8
      outFmt = RenderFormat::R8G8B8A8_UNORM; outBytes = 4; return true;
    default:  // k_2_10_10_10 (no Plume format) and others -> caller falls back to a guess
      return false;
  }
}

}  // namespace

bool Video::GetOrCreateGuestPipeline(const void* vsDxil, uint64_t vsSize, uint64_t vsKey,
                                     const void* psDxil, uint64_t psSize, uint64_t psKey,
                                     uint32_t xenosPrimType, const VertexInputAttr* vtxAttrs,
                                     size_t vtxAttrCount) {
  if (!g_initialized || !vsDxil || !psDxil) return false;
  if (!EnsureGuestPipelineLayout()) return false;

  const RenderPrimitiveTopology topo = XenosPrimToTopology(xenosPrimType);
  const uint64_t pkey = vsKey * 1099511628211ull ^ (psKey + (uint64_t(xenosPrimType) << 1));
  if (g_pipelineCache.count(pkey)) return true;

  const RenderShaderFormat fmt = g_interface->getCapabilities().shaderFormat;
  auto getShader = [&](uint64_t key, const void* dxil, uint64_t size) -> RenderShader* {
    auto it = g_shaderCache.find(key);
    if (it != g_shaderCache.end()) return it->second.get();
    auto sh = g_device->createShader(dxil, size, "shaderMain", fmt);
    RenderShader* raw = sh.get();
    g_shaderCache.emplace(key, std::move(sh));
    return raw;
  };
  RenderShader* vs = getShader(vsKey, vsDxil, vsSize);
  RenderShader* ps = getShader(psKey, psDxil, psSize);
  if (!vs || !ps) {
    Log("GetOrCreateGuestPipeline: createShader failed");
    return false;
  }

  // Build the input layout from the VS input signature: every non-system input must
  // be supplied or D3D12 fails PSO creation (E_INVALIDARG). Placeholder formats for
  // now (R32G32B32A32_FLOAT, packed slot 0) — enough to validate + bind the pipeline;
  // real per-element formats/offsets come from the guest vertex declaration (task #4
  // correctness pass). Strings must outlive createGraphicsPipeline (kept in `names`).
  std::vector<VsInputSem> sems;
  ParseSignature(static_cast<const uint8_t*>(vsDxil), size_t(vsSize), /*output=*/false, sems);
  {
    char sm[200];
    int n = std::snprintf(sm, sizeof(sm), "GetOrCreateGuestPipeline: VS inputs:");
    for (const auto& s : sems)
      n += std::snprintf(sm + n, sizeof(sm) - n, " %s%u", s.name.c_str(), s.index);
    Log(sm);
  }

  // Diagnostic: compare the VS OUTPUT signature against the PS INPUT signature. D3D12
  // fails PSO linkage (E_INVALIDARG) if a PS input has no matching VS output.
  {
    std::vector<VsInputSem> vsOut, psIn;
    ParseSignature(static_cast<const uint8_t*>(vsDxil), size_t(vsSize), /*output=*/true, vsOut);
    ParseSignature(static_cast<const uint8_t*>(psDxil), size_t(psSize), /*output=*/false, psIn);
    char sm[256];
    int n = std::snprintf(sm, sizeof(sm), "  VS out:");
    for (const auto& s : vsOut) n += std::snprintf(sm + n, sizeof(sm) - n, " %s%u", s.name.c_str(), s.index);
    Log(sm);
    n = std::snprintf(sm, sizeof(sm), "  PS in :");
    for (const auto& s : psIn) n += std::snprintf(sm + n, sizeof(sm) - n, " %s%u", s.name.c_str(), s.index);
    Log(sm);
    // Dump the PS DXIL chunk fourccs (to confirm whether it even has an input sig).
    {
      const uint8_t* pd = static_cast<const uint8_t*>(psDxil);
      if (psSize >= 32 && std::memcmp(pd, "DXBC", 4) == 0) {
        const uint32_t cc = Rd32LE(pd + 28);
        int m = std::snprintf(sm, sizeof(sm), "  PS chunks(%u):", cc);
        for (uint32_t i = 0; i < cc && uint64_t(32) + i * 4 + 4 <= psSize; ++i) {
          const uint32_t co = Rd32LE(pd + 32 + i * 4);
          if (co + 4 <= psSize)
            m += std::snprintf(sm + m, sizeof(sm) - m, " %c%c%c%c", pd[co], pd[co + 1], pd[co + 2],
                               pd[co + 3]);
        }
        Log(sm);
      }
    }
    for (const auto& pi : psIn) {
      bool found = false;
      for (const auto& vo : vsOut)
        if (vo.index == pi.index && vo.name == pi.name) { found = true; break; }
      if (!found) {
        std::snprintf(sm, sizeof(sm), "  *** PS input %s%u has NO matching VS output (PSO linkage fail)",
                      pi.name.c_str(), pi.index);
        Log(sm);
      }
    }
  }
  std::vector<std::string> names;
  names.reserve(sems.size());
  for (const auto& s : sems) names.push_back(s.name);
  // Build the IA layout. For each VS input (from the DXIL ISGN), find the matching
  // parsed attribute (same semantic name + index) to get the REAL Xenos format + byte
  // offset; fall back to a per-semantic guess only when no attribute is available (e.g.
  // a container-less VS). Real offsets/formats are what makes geometry land on-screen
  // (the old guess packed everything at the wrong offsets -> degenerate green quad).
  std::vector<RenderInputElement> inputElements;
  inputElements.reserve(sems.size());
  uint32_t packedOffset = 0;  // fallback packing cursor
  uint32_t maxStrideBytes = 0;
  for (size_t i = 0; i < sems.size(); ++i) {
    const bool uintReg = sems[i].componentType == 1 || sems[i].componentType == 2;
    RenderFormat fmt = RenderFormat::R32G32B32A32_FLOAT;
    uint32_t size = 16;
    uint32_t offset = packedOffset;
    uint32_t location = uint32_t(i);
    bool matched = false;
    for (size_t a = 0; a < vtxAttrCount; ++a) {
      if (DeclUsageToSemantic(vtxAttrs[a].usage) == names[i] &&
          vtxAttrs[a].usageIndex == sems[i].index) {
        RenderFormat f;
        uint32_t bytes;
        if (XenosVertexFormatToRender(vtxAttrs[a].format, uintReg, f, bytes)) {
          fmt = f;
          size = bytes;
          offset = vtxAttrs[a].offsetDwords * 4;
          if (vtxAttrs[a].strideDwords)
            maxStrideBytes = std::max(maxStrideBytes, vtxAttrs[a].strideDwords * 4);
          matched = true;
        }
        const uint32_t loc = DeclUsageToLocation(vtxAttrs[a].usage, vtxAttrs[a].usageIndex);
        if (loc != 0xFFFFFFFFu) location = loc;
        break;
      }
    }
    if (!matched) {
      // Fallback guess (no parsed attribute): common movie-quad packing.
      if (names[i] == "POSITION") { fmt = RenderFormat::R32G32B32_FLOAT; size = 12; }
      else if (names[i] == "TEXCOORD") { fmt = RenderFormat::R32G32_FLOAT; size = 8; }
      offset = packedOffset;
    }
    inputElements.emplace_back(names[i].c_str(), sems[i].index, location, fmt, /*slotIndex=*/0u,
                               offset);
    packedOffset = std::max(packedOffset, offset + size);
  }
  // Slot stride: the binding stride from the vfetch (all attributes share one binding);
  // fall back to the tightly-packed size when the parse didn't yield a stride.
  const uint32_t slotStride = maxStrideBytes ? maxStrideBytes : packedOffset;
  const RenderInputSlot inputSlot(0, slotStride);
  {
    char sm[256];
    int n = std::snprintf(sm, sizeof(sm), "  IA layout (stride=%u attrs=%zu):", slotStride,
                          vtxAttrCount);
    for (const auto& e : inputElements)
      n += std::snprintf(sm + n, sizeof(sm) - n, " %s%u@%u/fmt%d", e.semanticName, e.semanticIndex,
                         e.alignedByteOffset, int(e.format));
    Log(sm);
  }

  // Count the PS render-target outputs (SV_TargetN). D3D12 fails PSO creation if the
  // declared RT count/formats don't cover what the PS writes.
  std::vector<VsInputSem> psOut;
  ParseSignature(static_cast<const uint8_t*>(psDxil), size_t(psSize), /*output=*/true, psOut,
                 /*skipSystemValues=*/false);
  uint32_t rtCount = 0;
  for (const auto& o : psOut) {
    if (o.name.rfind("SV_Target", 0) == 0) rtCount = (o.index + 1 > rtCount) ? o.index + 1 : rtCount;
  }
  if (rtCount == 0) rtCount = 1;
  if (rtCount > 8) rtCount = 8;
  {
    char sm[200];
    int n = std::snprintf(sm, sizeof(sm), "  PS out(rtCount=%u):", rtCount);
    for (const auto& o : psOut) n += std::snprintf(sm + n, sizeof(sm) - n, " %s%u", o.name.c_str(), o.index);
    Log(sm);
  }

  RenderGraphicsPipelineDesc desc;
  desc.pipelineLayout = g_guestPipelineLayout.get();
  desc.vertexShader = vs;
  desc.pixelShader = ps;
  for (uint32_t r = 0; r < rtCount; ++r) {
    desc.renderTargetFormat[r] = RenderFormat::B8G8R8A8_UNORM;
    desc.renderTargetBlend[r] = RenderBlendDesc::Copy();
  }
  desc.renderTargetCount = rtCount;
  desc.primitiveTopology = topo;
  if (!inputElements.empty()) {
    desc.inputElements = inputElements.data();
    desc.inputElementsCount = uint32_t(inputElements.size());
    desc.inputSlots = &inputSlot;
    desc.inputSlotsCount = 1;
  }

  auto pipeline = g_device->createGraphicsPipeline(desc);
  char msg[160];
  std::snprintf(msg, sizeof(msg), "GetOrCreateGuestPipeline: vs=%llu ps=%llu prim=%u inputs=%zu -> %s",
                (unsigned long long)vsKey, (unsigned long long)psKey, xenosPrimType, sems.size(),
                pipeline ? "CREATED" : "FAILED");
  Log(msg);
  if (!pipeline) return false;
  g_pipelineCache.emplace(pkey, std::move(pipeline));
  return true;
}

namespace {
uint64_t PipelineKey(uint64_t vsKey, uint64_t psKey, uint32_t prim) {
  return vsKey * 1099511628211ull ^ (psKey + (uint64_t(prim) << 1));
}

// Create the per-draw binding resources once: the shared bindless sets (0-3) + dummy
// texture/sampler, and the per-slot ring (b0/b1/b2 + a set-4 CBV descriptor set each).
bool EnsureDrawResources() {
  if (g_drawResourcesReady) return true;
  if (!g_device || !EnsureGuestPipelineLayout()) return false;
  Log("EnsureDrawResources: begin");

  g_dummyVB = g_device->createBuffer(RenderBufferDesc::VertexBuffer(4096, RenderHeapType::UPLOAD));
  if (g_dummyVB) { if (void* p = g_dummyVB->map()) { std::memset(p, 0, 4096); g_dummyVB->unmap(); } }

  g_dummyTex = g_device->createTexture(
      RenderTextureDesc::Texture2D(1, 1, 1, RenderFormat::R8G8B8A8_UNORM));
  g_dummyView = g_dummyTex->createTextureView(
      RenderTextureViewDesc::Texture2D(RenderFormat::R8G8B8A8_UNORM));
  RenderSamplerDesc samplerDesc;
  samplerDesc.minFilter = RenderFilter::LINEAR;
  samplerDesc.magFilter = RenderFilter::LINEAR;
  samplerDesc.addressU = RenderTextureAddressMode::WRAP;
  samplerDesc.addressV = RenderTextureAddressMode::WRAP;
  samplerDesc.addressW = RenderTextureAddressMode::WRAP;
  g_dummySampler = g_device->createSampler(samplerDesc);
  Log("EnsureDrawResources: buffers + dummy tex/sampler ok");

  constexpr uint32_t kSamplerHeapSize = 64;   // D3D12 sampler heap max is 2048
  const RenderDescriptorRange t2d(RenderDescriptorRangeType::TEXTURE, 0, 1);
  const RenderDescriptorRange t2dArr(RenderDescriptorRangeType::TEXTURE, 0, 1);
  const RenderDescriptorRange tCube(RenderDescriptorRangeType::TEXTURE, 0, 1);
  const RenderDescriptorRange samp(RenderDescriptorRangeType::SAMPLER, 0, 1);
  const RenderDescriptorRange cbv[3] = {
      RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 0, 1),
      RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 1, 1),
      RenderDescriptorRange(RenderDescriptorRangeType::CONSTANT_BUFFER, 2, 1)};
  g_set[0] = g_device->createDescriptorSet(RenderDescriptorSetDesc(&t2d, 1, true, kTexHeapSize));
  g_set[1] = g_device->createDescriptorSet(RenderDescriptorSetDesc(&t2dArr, 1, true, kTexHeapSize));
  g_set[2] = g_device->createDescriptorSet(RenderDescriptorSetDesc(&tCube, 1, true, kTexHeapSize));
  g_set[3] = g_device->createDescriptorSet(RenderDescriptorSetDesc(&samp, 1, true, kSamplerHeapSize));
  if (!g_set[0] || !g_set[1] || !g_set[2] || !g_set[3]) {
    Log("EnsureDrawResources: createDescriptorSet(shared) failed");
    return false;
  }
  g_set[0]->setTexture(0, g_dummyTex.get(), RenderTextureLayout::SHADER_READ, g_dummyView.get());
  g_set[3]->setSampler(0, g_dummySampler.get());

  // Per-draw ring: a b0/b1/b2 triple + a set-4 CBV descriptor set per slot.
  for (uint32_t k = 0; k < kDrawRing; ++k) {
    g_vsCb[k] = g_device->createBuffer(RenderBufferDesc::UploadBuffer(kVsCbBytes));
    g_psCb[k] = g_device->createBuffer(RenderBufferDesc::UploadBuffer(kPsCbBytes));
    g_sharedCb[k] = g_device->createBuffer(RenderBufferDesc::UploadBuffer(kSharedCbBytes));
    if (void* p = g_vsCb[k]->map()) { std::memset(p, 0, kVsCbBytes); g_vsCb[k]->unmap(); }
    if (void* p = g_psCb[k]->map()) { std::memset(p, 0, kPsCbBytes); g_psCb[k]->unmap(); }
    if (void* p = g_sharedCb[k]->map()) {
      std::memset(p, 0, kSharedCbBytes);
      WriteViewportSO(p, 1.0f, 1.0f, 0.0f, 0.0f);  // identity until SetViewportTransform
      g_sharedCb[k]->unmap();
    }
    g_set4[k] = g_device->createDescriptorSet(RenderDescriptorSetDesc(cbv, 3, false, 0));
    if (!g_vsCb[k] || !g_psCb[k] || !g_sharedCb[k] || !g_set4[k]) {
      Log("EnsureDrawResources: ring slot alloc failed");
      return false;
    }
    g_set4[k]->setBuffer(0, g_vsCb[k].get(), kVsCbBytes);
    g_set4[k]->setBuffer(1, g_psCb[k].get(), kPsCbBytes);
    g_set4[k]->setBuffer(2, g_sharedCb[k].get(), kSharedCbBytes);
  }

  g_drawResourcesReady = true;
  Log("EnsureDrawResources: created (shared sets + per-draw ring)");
  return true;
}

// Take the next draw slot for the frame. Returns false if the ring is exhausted (drop the
// draw rather than clobber an earlier slot). Zeroes the slot's shared cbuffer (b2) so stale
// texture descriptor indices from a previous frame don't leak in.
bool BeginDrawSlot() {
  if (g_drawSlot >= kDrawRing) {
    if (!g_ringOverflow) { Log("draw ring overflow (frame has too many draws)"); g_ringOverflow = true; }
    return false;
  }
  g_curSlot = g_drawSlot++;
  if (g_sharedCb[g_curSlot]) {
    if (void* p = g_sharedCb[g_curSlot]->map()) {
      std::memset(p, 0, kSharedCbBytes);
      WriteViewportSO(p, 1.0f, 1.0f, 0.0f, 0.0f);  // identity until SetViewportTransform
      g_sharedCb[g_curSlot]->unmap();
    }
  }
  return true;
}
}  // namespace

void Video::BeginGuestDraw() {
  if (!g_initialized || !EnsureDrawResources()) return;
  BeginDrawSlot();
}

void Video::SetDrawConstants(const void* vsConst, uint32_t vsFloat4Count, const void* psConst,
                             uint32_t psFloat4Count) {
  if (!g_initialized || g_curSlot >= kDrawRing) return;
  if (!EnsureDrawResources()) return;
  // Xenos float registers are already host-endian (the CP swapped them from the ring), and
  // a DXIL cbuffer float4 is host-endian too, so copy straight through (no byte swap).
  if (vsConst && g_vsCb[g_curSlot]) {
    uint64_t bytes = uint64_t(vsFloat4Count) * 16;
    if (bytes > kVsCbBytes) bytes = kVsCbBytes;
    if (void* p = g_vsCb[g_curSlot]->map()) {
      std::memcpy(p, vsConst, size_t(bytes));
      g_vsCb[g_curSlot]->unmap();
    }
  }
  if (psConst && g_psCb[g_curSlot]) {
    uint64_t bytes = uint64_t(psFloat4Count) * 16;
    if (bytes > kPsCbBytes) bytes = kPsCbBytes;
    if (void* p = g_psCb[g_curSlot]->map()) {
      std::memcpy(p, psConst, size_t(bytes));
      g_psCb[g_curSlot]->unmap();
    }
  }
}

void Video::SetViewportTransform(float sx, float sy, float bx, float by) {
  if (!g_initialized || g_curSlot >= kDrawRing || !g_sharedCb[g_curSlot]) return;
  if (void* p = g_sharedCb[g_curSlot]->map()) {
    WriteViewportSO(p, sx, sy, bx, by);
    g_sharedCb[g_curSlot]->unmap();
  }
}

void Video::BindGuestTexture(uint32_t shaderSlot, const void* src, uint32_t width, uint32_t height,
                             uint32_t srcPitchBytes, int xenosFormat, int xenosEndian) {
  if (!g_initialized || shaderSlot >= 16 || g_curSlot >= kDrawRing || !src || width == 0 ||
      height == 0)
    return;
  if (width > 4096 || height > 4096) return;  // sanity
  if (!EnsureDrawResources()) return;
  if (g_texHeapNext >= kTexHeapSize) return;  // bindless heap full this frame

  RenderFormat fmt;
  uint32_t bpp;
  // k_8_8_8_8 on the 360 is D3DFMT_A8R8G8B8 (ARGB) stored big-endian; the fetch's endian
  // (2 = 8in32) byte-reverses each dword to BGRA, which B8G8R8A8_UNORM reads correctly.
  switch (xenosFormat) {
    case 2: fmt = RenderFormat::R8_UNORM; bpp = 1; break;        // k_8 (YUV plane, single byte)
    case 6: fmt = RenderFormat::B8G8R8A8_UNORM; bpp = 4; break;  // k_8_8_8_8 (ARGB)
    default: return;                                              // unsupported -> keep dummy
  }
  // Per-dword byte reverse when the fetch endian is 8in32 (k_8_8_8_8 ARGB-BE -> BGRA-LE). k_8
  // (single byte) and endian 0 need no swap. (16in32 / 8in16 not needed by the movie/splash.)
  const bool swap4 = (bpp == 4) && (xenosEndian == 2);
  const uint32_t rowBytes = (width * bpp + 255u) & ~255u;  // staging rows are 256-byte aligned

  // Take a fresh bindless heap slot for this draw's texture (so concurrent draws don't share
  // one texture object). Pool entries recur in the same order each frame, so reuse + reupload.
  const uint32_t heapSlot = g_texHeapNext++;
  if (!g_texPool[heapSlot] || g_texPoolW[heapSlot] != width || g_texPoolH[heapSlot] != height ||
      g_texPoolFmt[heapSlot] != xenosFormat) {
    g_texPool[heapSlot] =
        g_device->createTexture(RenderTextureDesc::Texture2D(width, height, 1, fmt));
    g_texPoolView[heapSlot] =
        g_texPool[heapSlot]->createTextureView(RenderTextureViewDesc::Texture2D(fmt));
    g_texPoolUpload[heapSlot] =
        g_device->createBuffer(RenderBufferDesc::UploadBuffer(uint64_t(rowBytes) * height));
    g_texPoolW[heapSlot] = width;
    g_texPoolH[heapSlot] = height;
    g_texPoolFmt[heapSlot] = xenosFormat;
  }
  if (!g_texPool[heapSlot] || !g_texPoolUpload[heapSlot]) return;

  // Copy guest rows (linear, src pitch) into the 256-aligned staging rows. For k_8_8_8_8 with
  // 8in32 endian, reverse each dword's bytes (ARGB-BE -> BGRA). SEH-guarded against a bad src.
  if (uint8_t* dstp = static_cast<uint8_t*>(g_texPoolUpload[heapSlot]->map())) {
    const uint8_t* s = static_cast<const uint8_t*>(src);
    const uint32_t copyBytes = width * bpp;
    __try {
      for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* sr = s + size_t(y) * srcPitchBytes;
        uint8_t* dr = dstp + size_t(y) * rowBytes;
        if (swap4) {
          for (uint32_t x = 0; x < copyBytes; x += 4) {
            dr[x] = sr[x + 3]; dr[x + 1] = sr[x + 2]; dr[x + 2] = sr[x + 1]; dr[x + 3] = sr[x];
          }
        } else {
          std::memcpy(dr, sr, copyBytes);
        }
      }
    } __except (1 /*EXCEPTION_EXECUTE_HANDLER*/) {
    }
    g_texPoolUpload[heapSlot]->unmap();
  }

  EnsureFrameStarted();
  RenderTexture* tex = g_texPool[heapSlot].get();
  g_commandList->barriers(RenderBarrierStage::COPY,
                          RenderTextureBarrier(tex, RenderTextureLayout::COPY_DEST));
  RenderTextureCopyLocation dst = RenderTextureCopyLocation::Subresource(tex);
  RenderTextureCopyLocation srcLoc = RenderTextureCopyLocation::PlacedFootprint(
      g_texPoolUpload[heapSlot].get(), fmt, width, height, 1, rowBytes / bpp);
  g_commandList->copyTextureRegion(dst, srcLoc, 0, 0, 0);
  g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                          RenderTextureBarrier(tex, RenderTextureLayout::SHADER_READ));

  g_set[0]->setTexture(heapSlot, tex, RenderTextureLayout::SHADER_READ, g_texPoolView[heapSlot].get());
  // Descriptor indices into THIS draw's b2 so the shader's tfetch(sN) finds it (shader_common.h:
  // sN texture index @ byte N*4, sN sampler index @ byte 192+N*4). Sampler 0 = dummy linear.
  if (g_sharedCb[g_curSlot]) {
    if (uint8_t* p = static_cast<uint8_t*>(g_sharedCb[g_curSlot]->map())) {
      *reinterpret_cast<uint32_t*>(p + shaderSlot * 4) = heapSlot;
      *reinterpret_cast<uint32_t*>(p + 192 + shaderSlot * 4) = 0;
      g_sharedCb[g_curSlot]->unmap();
    }
  }
}

void Video::RecordGuestDraw(uint64_t vsKey, uint64_t psKey, uint32_t xenosPrimType,
                            uint32_t vertexCount, uint32_t indexCount) {
  if (!g_initialized) return;
  auto pit = g_pipelineCache.find(PipelineKey(vsKey, psKey, xenosPrimType));
  if (pit == g_pipelineCache.end()) return;  // no pipeline for this pair
  if (!EnsureDrawResources()) return;

  EnsureFrameStarted();
  static const bool kNoDesc = std::getenv("MM_NO_DESC") != nullptr;
  // Transition the dummy texture to SHADER_READ once (recorded in the open frame).
  if (!kNoDesc && !g_dummyTransitioned) {
    g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                            RenderTextureBarrier(g_dummyTex.get(), RenderTextureLayout::SHADER_READ));
    g_dummyTransitioned = true;
  }

  static int dbg = 0;
  const bool log = dbg < 2;
  if (log) {
    ++dbg;
    char m[80];
    std::snprintf(m, sizeof(m), "RecordGuestDraw: frame started ok, thread %lu",
                  (unsigned long)GetCurrentThreadId());
    Log(m);
  }

  if (log) {
    char m[96];
    std::snprintf(m, sizeof(m), "RecordGuestDraw: layout=%p pipeline=%p", (void*)g_guestPipelineLayout.get(), (void*)pit->second.get());
    Log(m);
  }
  g_commandList->setGraphicsPipelineLayout(g_guestPipelineLayout.get());
  if (log) Log("RecordGuestDraw: setGraphicsPipelineLayout ok");
  g_commandList->setPipeline(pit->second.get());
  if (log) Log("RecordGuestDraw: pipeline set ok");
  if (!kNoDesc) {
    static const bool kCbvOnly = std::getenv("MM_CBV_ONLY") != nullptr;
    // Bind Texture2D(0), Sampler(3), cbuffers(4); skip the unpopulated Texture2DArray(1)
    // and TextureCube(2) heaps (binding an empty boundless set appears to fault). The
    // movie PS only uses Texture2D + a sampler. MM_CBV_ONLY = set 4 only (isolation).
    static const bool kNoSampler = std::getenv("MM_NO_SAMPLER") != nullptr;
    const uint32_t sets[] = {0u, 3u, 4u};
    for (uint32_t s : sets) {
      if (kCbvOnly && s != 4u) continue;
      if (kNoSampler && s == 3u) continue;
      // Set 4 (the CBVs) is the per-draw ring slot; sets 0-3 are the shared bindless sets.
      RenderDescriptorSet* ds = (s == 4u) ? g_set4[g_curSlot].get() : g_set[s].get();
      g_commandList->setGraphicsDescriptorSet(ds, s);
    }
  }
  if (log) Log("RecordGuestDraw: descriptor sets bound ok");

  // Bind a zeroed dummy vertex buffer so the input-assembler doesn't fault (no guest
  // geometry uploaded yet — task #4 correctness). Stride 64 covers up to 4 float4
  // inputs; degenerate geometry, but the draw executes safely.
  (void)indexCount;
  static const bool kNoVB = std::getenv("MM_NO_VB") != nullptr;
  if (!kNoVB) {
    const RenderVertexBufferView vbv(g_dummyVB.get(), 4096);
    const RenderInputSlot vslot(0, 64);
    g_commandList->setVertexBuffers(0, &vbv, 1, &vslot);
  }
  static const bool kNoDrawCall = std::getenv("MM_NO_DRAWCALL") != nullptr;
  if (!kNoDrawCall) g_commandList->drawInstanced(vertexCount ? vertexCount : 3, 1, 0, 0);
  if (log) Log("RecordGuestDraw: drawInstanced recorded ok");
}

void Video::RecordGuestDrawIndexed(uint64_t vsKey, uint64_t psKey, uint32_t xenosPrimType,
                                   const void* vbHost, uint32_t vbSize, uint32_t stride,
                                   const void* ibHost, uint32_t indexCount, bool ib32) {
  if (!g_initialized || !vbHost || !ibHost || vbSize == 0 || stride == 0 || indexCount == 0) return;
  if (vbSize > (8u << 20) || indexCount > 2000000u) return;  // sanity bounds
  auto pit = g_pipelineCache.find(PipelineKey(vsKey, psKey, xenosPrimType));
  if (pit == g_pipelineCache.end()) return;
  if (!EnsureDrawResources()) return;

  if (g_curSlot >= kDrawRing) return;
  const uint32_t slot = g_curSlot;
  const uint32_t idxSize = ib32 ? 4u : 2u;
  const uint32_t ibBytes = indexCount * idxSize;
  if (!g_drawVB[slot] || g_drawVBCap[slot] < vbSize) {
    g_drawVBCap[slot] = (vbSize + 0xFFFFu) & ~0xFFFFu;
    g_drawVB[slot] =
        g_device->createBuffer(RenderBufferDesc::VertexBuffer(g_drawVBCap[slot], RenderHeapType::UPLOAD));
  }
  if (!g_drawIB[slot] || g_drawIBCap[slot] < ibBytes) {
    g_drawIBCap[slot] = (ibBytes + 0xFFFFu) & ~0xFFFFu;
    g_drawIB[slot] =
        g_device->createBuffer(RenderBufferDesc::IndexBuffer(g_drawIBCap[slot], RenderHeapType::UPLOAD));
  }
  if (!g_drawVB[slot] || !g_drawIB[slot]) return;

  // Upload, byte-swapping big-endian guest data to little-endian. VB swapped as
  // float32 (4-byte) units — correct for all-float vertex formats (movie quad).
  if (uint8_t* p = static_cast<uint8_t*>(g_drawVB[slot]->map())) {
    const uint8_t* s = static_cast<const uint8_t*>(vbHost);
    const uint32_t n = vbSize & ~3u;
    __try {
      for (uint32_t i = 0; i < n; i += 4) {
        p[i] = s[i + 3]; p[i + 1] = s[i + 2]; p[i + 2] = s[i + 1]; p[i + 3] = s[i];
      }
    } __except (1 /*EXCEPTION_EXECUTE_HANDLER*/) {
    }
    g_drawVB[slot]->unmap();
  }
  if (uint8_t* p = static_cast<uint8_t*>(g_drawIB[slot]->map())) {
    const uint8_t* s = static_cast<const uint8_t*>(ibHost);
    __try {
      if (ib32) {
        for (uint32_t i = 0; i < ibBytes; i += 4) {
          p[i] = s[i + 3]; p[i + 1] = s[i + 2]; p[i + 2] = s[i + 1]; p[i + 3] = s[i];
        }
      } else {
        for (uint32_t i = 0; i < ibBytes; i += 2) { p[i] = s[i + 1]; p[i + 1] = s[i]; }
      }
    } __except (1 /*EXCEPTION_EXECUTE_HANDLER*/) {
    }
    g_drawIB[slot]->unmap();
  }

  EnsureFrameStarted();
  static const bool kNoDesc = std::getenv("MM_NO_DESC") != nullptr;
  if (!kNoDesc && !g_dummyTransitioned) {
    g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                            RenderTextureBarrier(g_dummyTex.get(), RenderTextureLayout::SHADER_READ));
    g_dummyTransitioned = true;
  }
  g_commandList->setGraphicsPipelineLayout(g_guestPipelineLayout.get());
  g_commandList->setPipeline(pit->second.get());
  if (!kNoDesc) {
    const uint32_t sets[] = {0u, 3u, 4u};
    for (uint32_t s : sets) {
      RenderDescriptorSet* ds = (s == 4u) ? g_set4[slot].get() : g_set[s].get();
      g_commandList->setGraphicsDescriptorSet(ds, s);
    }
  }
  const RenderVertexBufferView vbv(g_drawVB[slot].get(), vbSize);
  const RenderInputSlot vslot(0, stride);
  g_commandList->setVertexBuffers(0, &vbv, 1, &vslot);
  const RenderIndexBufferView ibv(g_drawIB[slot].get(), ibBytes,
                                  ib32 ? RenderFormat::R32_UINT : RenderFormat::R16_UINT);
  g_commandList->setIndexBuffer(&ibv);
  g_commandList->drawIndexedInstanced(indexCount, 1, 0, 0, 0);
}

void Video::RecordGuestDrawVB(uint64_t vsKey, uint64_t psKey, uint32_t xenosPrimType,
                              const void* vbHost, uint32_t vbSize, uint32_t stride,
                              uint32_t vertexCount, uint32_t startVertex) {
  if (!g_initialized || !vbHost || vbSize == 0 || stride == 0 || vertexCount == 0) return;
  if (vbSize > (8u << 20) || vertexCount > 1000000u) return;
  auto pit = g_pipelineCache.find(PipelineKey(vsKey, psKey, xenosPrimType));
  if (pit == g_pipelineCache.end()) return;
  if (!EnsureDrawResources()) return;

  if (g_curSlot >= kDrawRing) return;
  const uint32_t slot = g_curSlot;
  if (!g_drawVB[slot] || g_drawVBCap[slot] < vbSize) {
    g_drawVBCap[slot] = (vbSize + 0xFFFFu) & ~0xFFFFu;
    g_drawVB[slot] =
        g_device->createBuffer(RenderBufferDesc::VertexBuffer(g_drawVBCap[slot], RenderHeapType::UPLOAD));
  }
  if (!g_drawVB[slot]) return;
  if (uint8_t* p = static_cast<uint8_t*>(g_drawVB[slot]->map())) {
    const uint8_t* s = static_cast<const uint8_t*>(vbHost);
    const uint32_t n = vbSize & ~3u;
    __try {
      for (uint32_t i = 0; i < n; i += 4) {
        p[i] = s[i + 3]; p[i + 1] = s[i + 2]; p[i + 2] = s[i + 1]; p[i + 3] = s[i];
      }
    } __except (1 /*EXCEPTION_EXECUTE_HANDLER*/) {
    }
    g_drawVB[slot]->unmap();
  }

  EnsureFrameStarted();
  static const bool kNoDesc = std::getenv("MM_NO_DESC") != nullptr;
  if (!kNoDesc && !g_dummyTransitioned) {
    g_commandList->barriers(RenderBarrierStage::GRAPHICS,
                            RenderTextureBarrier(g_dummyTex.get(), RenderTextureLayout::SHADER_READ));
    g_dummyTransitioned = true;
  }
  g_commandList->setGraphicsPipelineLayout(g_guestPipelineLayout.get());
  g_commandList->setPipeline(pit->second.get());
  if (!kNoDesc) {
    const uint32_t sets[] = {0u, 3u, 4u};
    for (uint32_t s : sets) {
      RenderDescriptorSet* ds = (s == 4u) ? g_set4[slot].get() : g_set[s].get();
      g_commandList->setGraphicsDescriptorSet(ds, s);
    }
  }
  const RenderVertexBufferView vbv(g_drawVB[slot].get(), vbSize);
  const RenderInputSlot vslot(0, stride);
  g_commandList->setVertexBuffers(0, &vbv, 1, &vslot);

  // kQuadList (13): every 4 verts = a quad = 2 triangles. D3D12 has no quad topology (the PSO
  // uses TRIANGLE_LIST), so expand to a triangle index buffer (0,1,2, 0,2,3 per quad) and draw
  // indexed — otherwise only the first triangle renders (the diagonal-half artifact).
  if (xenosPrimType == 13 && vertexCount >= 4) {
    const uint32_t quads = vertexCount / 4;
    const uint32_t idxCount = quads * 6;
    const uint32_t idxBytes = idxCount * 2;  // 16-bit (quad verts are few)
    if (vertexCount <= 0xFFFFu) {
      if (!g_drawIB[slot] || g_drawIBCap[slot] < idxBytes) {
        g_drawIBCap[slot] = (idxBytes + 0xFFFFu) & ~0xFFFFu;
        g_drawIB[slot] = g_device->createBuffer(
            RenderBufferDesc::IndexBuffer(g_drawIBCap[slot], RenderHeapType::UPLOAD));
      }
      if (g_drawIB[slot]) {
        if (uint16_t* ip = static_cast<uint16_t*>(g_drawIB[slot]->map())) {
          for (uint32_t q = 0; q < quads; ++q) {
            const uint16_t b = uint16_t(q * 4);
            uint16_t* t = ip + q * 6;
            t[0] = b; t[1] = b + 1; t[2] = b + 2; t[3] = b; t[4] = b + 2; t[5] = b + 3;
          }
          g_drawIB[slot]->unmap();
        }
        const RenderIndexBufferView ibv(g_drawIB[slot].get(), idxBytes, RenderFormat::R16_UINT);
        g_commandList->setIndexBuffer(&ibv);
        g_commandList->drawIndexedInstanced(idxCount, 1, 0, int32_t(startVertex), 0);
        return;
      }
    }
  }
  g_commandList->drawInstanced(vertexCount, 1, startVertex, 0);
}

namespace {
std::atomic<bool> g_presentRequested{false};
}
void Video::RequestPresent() { g_presentRequested.store(true, std::memory_order_relaxed); }
bool Video::ConsumePresentRequest() {
  return g_presentRequested.exchange(false, std::memory_order_relaxed);
}

void Video::WaitForGPU() {
  if (g_initialized && g_commandsInFlight) {
    g_queue->waitForCommandFence(g_fence.get());
    g_commandsInFlight = false;
  }
}

void Video::Shutdown() {
  if (!g_initialized) return;
  WaitForGPU();
  for (auto& s : g_set) s.reset();
  g_dummySampler.reset();
  g_dummyView.reset();
  g_dummyTex.reset();
  g_dummyVB.reset();
  for (uint32_t k = 0; k < kDrawRing; ++k) {
    g_vsCb[k].reset();
    g_psCb[k].reset();
    g_sharedCb[k].reset();
    g_set4[k].reset();
    g_drawVB[k].reset();
    g_drawIB[k].reset();
  }
  for (uint32_t k = 0; k < kTexHeapSize; ++k) {
    g_texPool[k].reset();
    g_texPoolView[k].reset();
    g_texPoolUpload[k].reset();
  }
  g_drawResourcesReady = false;
  g_pipelineCache.clear();
  g_shaderCache.clear();
  g_guestPipelineLayout.reset();
  g_guestRTFb.reset();
  g_guestRT.reset();
  g_uploadBuffer.reset();
  g_framebuffers.clear();
  g_swapChain.reset();
  g_drawSemaphore.reset();
  g_acquireSemaphore.reset();
  g_fence.reset();
  g_commandList.reset();
  g_queue.reset();
  g_device.reset();
  g_interface.reset();
  g_initialized = false;
}

}  // namespace mm
