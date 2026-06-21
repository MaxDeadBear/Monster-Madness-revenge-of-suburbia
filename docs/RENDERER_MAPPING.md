# Monster Madness — Native Renderer Mapping

Goal: a native (Unleashed-Recompiled-style) renderer for the recomp, i.e. the
SDK's emulated GPU is disabled and the game's own D3D path is driven by host
code via `REX_HOOK`.

## Mode

`src/monster_madness_app.h` overrides `OnPreSetup()` to `config.graphics.reset()`,
which:
- drops the Xenia-derived `GraphicsSystem` (emulated GPU),
- turns the `Vd*` kernel imports into no-ops (they guard on a null graphics
  system),
- routes `ReXApp::SetupPresentation` down the detached branch ->
  `OnCreateImmediateDrawer()` (currently returns nullptr = headless baseline).

Verified building: `monster_madness.exe` links clean with the null-GPU change.

To get the emulated GPU back as a reference oracle, delete the `OnPreSetup`
override.

## How host code attaches to guest functions

`REX_HOOK(sub_XXXXXXXX, HostFn)` replaces a recompiled function body and
auto-marshals PPC regs -> host C++ types. `MidAsmHook` (manifest) handles
mid-function injection. This is the Unleashed `PPC_FUNC`-override equivalent.

## GPU seam — confirmed hook anchors (from Vd* call-graph)

Derived by tracing callers of the `Vd*` kernel-import thunks
(`tools/ida_xrefs.py` -> `assets/ida_dump/xrefs.txt`).

| guest func   | calls (Vd*)                                              | role (inferred)                |
|--------------|----------------------------------------------------------|--------------------------------|
| `sub_8220A800` | VdInitializeRingBuffer, VdEnableRingBufferRPtrWriteBack, VdSetSystemCommandBufferGpuIdentifierAddress | **D3D device / ring-buffer init** |
| `sub_822122D0` | VdSwap, VdGetSystemCommandBuffer                         | **Present / Swap**             |
| `sub_82219858` | VdInitializeEngines, VdSetGraphicsInterruptCallback     | early GPU/engine bring-up      |
| `sub_82219AB8` | VdSetGraphicsInterruptCallback, VdSetSystemCommandBufferGpuIdentifierAddress | GPU init helper |
| `sub_8220FD48` | VdInitializeScalerCommandBuffer                         | scaler command buffer setup    |

First bring-up targets: hook `sub_8220A800` (stand up host device) and
`sub_822122D0` (drive host present).

## D3D runtime internals (statically linked; identified via strings)

Generally NOT hooked individually — listed for orientation
(`assets/ida_dump/strings.txt`):

- `sub_82847DB0` — Xenos microcode assembler
- `sub_828CC990`, `sub_82838E38` — HLSL/FX compiler (D3DX) + #define setup
- `sub_828F75F8` — D3DX Effect API (`ID3DXEffect::SetPixelShader` etc.)
- `sub_8220E038` / `sub_8220E2B0` — D3D GPU-hang / device debug handler
- `sub_82A74A28` — GPU number-format / fetch-descriptor formatting
- `sub_82B5C968` — shader IL decode / interpolator validation

Implication: the title statically links the full MS D3D9 + D3DX + HLSL compiler,
so it can compile shaders at runtime (`D3DXCompileShader`).

## Overlay / early-screen shaders (good "hello triangle" hook targets)

Embedded HLSL source is in the binary:
- `sub_827AE890` — overlay/ImGui font shaders (`FontVertexShader`/`FontPixelShader`)
- `sub_822E3A18` — splash-screen shaders (`SplashVertexShader`/`SplashPixelShader`)

## What's still needed for the full D3DDevice_* hook surface

The thin/inline `D3DDevice_Draw*` / `Set*` entry points carry no strings, so
string-xref won't find them cleanly. Two ways forward:
1. **XDK `d3d9.lib` FLIRT signatures** applied in IDA -> auto-names the whole
   `D3DDevice_*` surface (best, needs the XDK .sig/.lib).
2. **Continue the call-graph walk** from `sub_8220A800` / `sub_822122D0`
   (callees + the D3D device object's vtable) -> extend `tools/ida_xrefs.py`.

## Device object (`a1` in the seam funcs) — struct offsets

From decompiling `sub_8220A800` / `sub_822122D0` (`assets/ida_dump/decompiled/`).
`a1` is the guest D3DDevice. The game also stores it to the `VdGlobalDevice`
kernel variable (xboxkrnl ordinal 0x01BE), so it can be read back host-side.

| offset | meaning |
|--------|---------|
| +48    | current push-buffer write pointer |
| +52/+56| ring-buffer end / limit (wrap via `sub_8220A2B8`) |
| +10896 | system command buffer base |
| +10900 | secondary buffer |
| +10908 | gpu mode/state (`3` after init) |
| +14812/+14816 | ring-buffer physical allocations |
| +14880/+14884 | ring base / end |
| +16536 | frame/swap counter (incremented at top of Swap) |
| +13520/+13524 | display width / height |
| +21516/+21520/+21524 | display info (w/h/actual-w) |

These are PM4-packet builders: the `D3DDevice_*` calls write packet dwords into
`+48`. Confirms the native hook must intercept at the `D3DDevice_*` method level
(semantic intent) rather than below it.

## Hook mechanism (verified)

Codegen emits each `sub_X` as `__attribute__((alias("__imp__sub_X"))) REX_WEAK_FUNC`
— i.e. a WEAK alias to the real body `__imp__sub_X`
(`generated/default/monster_madness_init.h` `DEFINE_REX_FUNC`). A `REX_HOOK` /
`REX_HOOK_RAW` defines `sub_X` as a STRONG symbol the linker prefers; the original
stays callable as `__imp__sub_X` for passthrough. No manifest change needed.
`VdSwap` writes only a PM4 swap token (no `graphics_system` deref), so calling
through the original Swap under null-GPU is safe = identical to headless baseline.

## Status (2026-06-17)

- `src/render/mm_render_hooks.cpp` — passthrough observation hooks on
  `sub_822122D0` (Present) and `sub_8220A800` (device init), wired into
  `CMakeLists.txt`. **Builds + links clean** (strong override of the weak alias
  works — mechanism proven).
- App launches in native mode and creates a responsive window (no early crash).
- Mode A oracle confirmed working in an earlier run (`logs/monster_madness_011.log`,
  Jun 3): "Translated 399 shaders", "Created 394 graphics pipelines".
- The SDK async/early-buffered file logger writes 0-byte logs in native mode
  (pre-existing since Jun 6, not caused by the null-GPU change; `--log_flush_interval`
  did not help — the early->file drain never completes). WORKAROUND: the hooks
  also write a raw `mm_render_trace.log` (plain fopen+fflush) next to the exe.

### VERIFIED (2026-06-17): native interception fires at runtime

Raw trace from a native-mode run:
```
[mm-render] D3D device init device=0x4004AD00 params=0x7010FB68
[mm-render] Present #0 device=0x4004AD00
```
- Recomp boots in native mode and reaches D3D device init + first present.
- `REX_HOOK` weak-override works at RUNTIME, not just link time.
- Live device pointer = `0x4004AD00` (the VdGlobalDevice; matches the
  SetInterruptCallback user_data seen in the Jun 3 emulated log).
- Present reaches only `#0` (not `#60`) in 15s => the guest render loop STALLS
  after the first swap, because in null-GPU mode nothing consumes/acks the ring
  buffer (no GPU-completion / vsync signal). This is the next thing a host
  renderer must provide: drive frame completion so the loop advances.

### Attempt: synthetic vblank driver (insufficient — important finding)

`src/render/mm_render_hooks.cpp` spawns an `XHostThread` that ticks the guest GPU
interrupt callback (`0x8220A600`, args `{source=0 vblank, user_data=device}`) at
~60 Hz via `FunctionDispatcher::ExecuteInterrupt`, mirroring
`GraphicsSystem::MarkVblank`. Verified the thread runs with the correct
`user_data=0x4004AD00` (captured at Present entry, before the passthrough clobbers
the register file).

RESULT: Present still stalls at `#0`. So **vblank alone does not unblock the
loop.** The guest waits on GPU *ring-buffer consumption*, not vblank:
- `sub_8220A800` arms `VdEnableRingBufferRPtrWriteBack` — the GPU is expected to
  write the ring read-pointer back to guest memory; the CPU polls it. In null
  mode it never advances, so once the game needs ring space / GPU progress it
  blocks.
- The decompiled Present `sub_822122D0` has a spin-loop gated on a GPU-updated
  counter (`a1+16544` vs the frame counter `a1+16536`).

NEXT (the real work): a **minimal command-buffer consumer** is required, not just
vblank. It must (a) advance the ring read-pointer and write it back to the
RPTR-writeback address, (b) service fence / EVENT_WRITE packets the game polls,
(c) fire the source-1 completion interrupt (cf. command_processor.cpp:1060
`DispatchInterruptCallback(1, n)`). Only then will Present advance to `#60`+.

STRATEGIC NOTE: the SDK's emulated `command_processor` already does all of this
correctly AND renders (Mode A: 399 shaders / 394 pipelines). Two viable routes:
1. Build the minimal CP consumer above (true native path; most work).
2. Keep Mode A's GPU running and layer interception there instead of fully
   nulling — lets the game run immediately while we redirect rendering piecemeal.

### SOLVED (2026-06-17): minimal command consumer — game runs frame-by-frame

Implemented `src/render/mm_graphics.{h,cpp}`:
- `MMNullCommandProcessor : rex::graphics::CommandProcessor` — inherits the full
  ring pump / MMIO write-pointer / fence + EVENT_WRITE servicing / completion
  interrupts / read-pointer write-back from the base class; stubs only rendering
  (`IssueDraw`->true, `IssueCopy`->true, `IssueSwap`/`LoadShader`/context/trace =
  no-op/nullptr/true).
- `MMGraphicsSystem : rex::graphics::d3d12::D3D12GraphicsSystem` — reuses the
  D3D12 provider/presenter/MMIO/vsync plumbing; overrides `CreateCommandProcessor`
  (return the null CP) and `name()` (base name() downcasts the CP to
  D3D12CommandProcessor, which ours is not).

`monster_madness_app.h` OnPreSetup now does
`config.graphics = mm::CreateNullConsumerGraphicsSystem();` (instead of resetting
graphics / the DIY vblank driver, both removed).

RESULT (verified run): Present advances `#0,#60,#120,...,#360` continuously at
~real-time. The guest boots, runs its render loop frame-by-frame; window is black
(no rendering yet). This is the foundation: rendering is now an incremental fill
of `IssueDraw`/`IssueSwap` (or of the D3D `sub_*` hooks) — no more hangs.

### Native draw-stream decode (2026-06-17) — first step of translation

`MMNullCommandProcessor::IssueDraw`/`IssueSwap` now decode the guest draw stream
from `register_file_` (the base CP maintains it) and trace to `mm_draws_trace.log`.
Rendering is still a no-op; this proves we see exactly what must be translated.

Verified output:
- `SWAP frontbuffer=0x1F260000..0x1F5F8000 1280x720` — real 720p front buffer.
- prim types per Xenos enum: `1`=PointList (boot frames), `4`=TriangleList
  (`indices=6` => quad), `8`=TriangleStrip-ish, `13`=RectangleList (clears/blits).
- `surface_pitch` 1280 / 640 (render-target geometry); per-frame draw counts
  (26 on first frame, then 2-3 for loading/UI). ~1300 events in 16 s, continuous.

REMAINING native-translation work (the large build), per draw:
1. Shaders: translate the bound Xenos vertex/pixel microcode to host (DXBC/SPIR-V).
   The SDK already has `pipeline/shader/{dxbc,spirv}_translator` — reusable even
   in a custom CP. Shader base addrs live in the register file (SQ_PROGRAM_CNTL +
   shader base regs).

   PROGRESS (2026-06-17): `MMNullCommandProcessor::LoadShader` now captures, caches
   (FNV-1a of ucode), and analyzes every bound shader into a real
   `rex::graphics::Shader` (returned so the base CP tracks it), tracing to
   `mm_shaders_trace.log`. Verified: 8 unique boot shaders disassembled, e.g.
   VERTEX with `vfetch_full vf95 FMT_32_32_32_FLOAT Stride=7` (vertex layout!),
   PIXEL `alloc colors; max oC0,r0,r0`. We now have ucode + vertex/texture bindings
   + Xenos disasm per shader. NEXT sub-step: emit DXBC/SPIR-V via
   `DxbcShaderTranslator` (construct translator + per-draw Modification from the
   register file; see PipelineCache::GetCurrent{Vertex,Pixel}ShaderModification).
2. Geometry: read vertex/index buffers from guest memory via the vertex-fetch
   constants; convert RectangleList/quad prims to host primitives.
3. State: blend/depth/raster from registers; bind render targets (EDRAM surface)
   to host textures.
4. Present: implement IssueSwap for real — fill the presenter's
   D3D12GuestOutputRefreshContext resource (UAV-capable ID3D12Resource) from the
   final front buffer (needs a host D3D12 command context; D3D12CommandProcessor
   is the template, src/graphics/d3d12/command_processor.cpp:1918).

## Tooling (works around the instance-only IDA MCP)

- `tools/ida_dump.py` — functions/strings/imports -> `assets/ida_dump/`
- `tools/ida_xrefs.py` — caller edges for GPU-seam anchors -> `xrefs.txt`
- Run in batch: `set IDA_DUMP_BATCH=1 & idat.exe -A -S<script> <db_copy.i64>`
  (run on a copy of the `.i64`; the live DB is locked).

### Reference frame from RenderDoc (Mode A emulated, 2026-06-19)

Captured the SDK's emulated D3D12 render of the real game (MM_MODE_A=1 +
renderdoccmd + in-app TriggerCapture) and analyzed it WITHOUT the MCP replay
(which fails error 16) via `renderdoccmd convert -c zip.xml` -> parse the XML
chunk stream. assets/mm_capture_frame242.rdc (a loading/UI frame).

Frame structure (how the SDK translates Xenos -> D3D12):
- 13 DrawInstanced (Xenos auto-index draws), 7 Dispatch (compute — vertex/
  memexport/EDRAM ops), 18 SetPipelineState (PSOs), 5 OMSetRenderTargets (~5
  passes) + 5 RSSetViewports/ScissorRects, 4 CopyTextureRegion + 2 CopyBufferRegion
  (EDRAM resolves), 20 ResourceBarrier, 1 ClearDepthStencilView. Topology: triangle
  (list + strip).
- RT/texture formats: color R8G8B8A8_UNORM + R10G10B10A2_UNORM (360 2_10_10_10),
  depth D24_UNORM_S8_UINT, swapchain B8G8R8A8_UNORM (== our Plume swapchain
  format), EDRAM/typed buffers R32G32B32A32_UINT, vertex R32G32_FLOAT, font/mask
  R8_UNORM.
- Shaders: DXBC bytecode embedded in the PSO creates (extractable from the
  zip.xml blobs; the SDK's Xenos->DXBC translations = a reference for the native
  shader path).

Implications for the native (Plume) renderer:
- Plume swapchain B8G8R8A8_UNORM is correct for final present (confirmed).
- Need intermediate RTs in R8G8B8A8 / R10G10B10A2 + D24S8; EDRAM modeled as
  render-to-texture + resolve/copy.
- Per draw: bind translated VS/PS, vertex buffers (R32G32_FLOAT etc.), triangle
  topology, blend/depth state, then a host triangle draw; composite final color
  RT into the swapchain at present.
- Tooling note: `renderdoccmd convert` -> zip.xml is the working way to inspect
  captures here since the MCP replay backend can't init (error 16).

### Shader extraction + disassembly (2026-06-19) — KEY direction finding

Extracted DXBC from the capture WITHOUT replay: unzip mm_capture_frame242.zip ->
numbered blobs; DXBC blobs start with magic "DXBC"; disassemble with x64 fxc
(`fxc /nologo /dumpbin <blob> /Fc <out>`). Saved: assets/ref_shaders/{vs,ps,cs}_xenia.asm.

Shader types this frame: vs_5_1, ps_5_1, cs_5_1 (the 7 compute Dispatches).
Disassembly header says "Generated by Xenia" — the SDK's shader translator IS
Xenia's DxbcShaderTranslator, and it emits the full EMULATION model:
- `dcl_input_sgv v0.x, vertex_id` — NO input layout; vertices fetched in-shader by
  SV_VertexID + `xe_fetch_constants[48]` (cbuffer) from a raw `xe_shared_memory`
  SRV/UAV (all guest memory as a byte buffer). Includes explicit Xenos vertex-index
  ENDIAN swap (0x00ff00ff masks / bfi 16,16).
- Huge `xe_system_cbuffer` (NDC scale/offset, EDRAM state, blend, alpha-test...).

DIRECTION FINDING: this Xenia model is heavy (shared-memory fetch, manual endian,
EDRAM-in-shader) and is NOT what the native (ReOdyssey/Unleashed) path wants.
ReOdyssey vendors **XenosRecomp**, which translates Xenos microcode to CLEAN HLSL
with real vertex inputs / samplers (no shared-memory emulation). So for MM's native
Plume renderer: use XenosRecomp for shaders (next: vendor thirdparty/XenosRecomp),
NOT the SDK/Xenia translator. The captured Xenia shaders are still a useful
reference for the shader LOGIC + the constant/fetch model per draw.

### MM UE3 shader storage investigation (2026-06-19)

Where MM's Xenos shaders live, for the XenosRecomp input (option A, offline dump):
- Game data: assets/MonsterGame (UE3 layout: Config/Xenon/Cooked, CookedXenon, ...).
- Shaders are inside the 1293 cooked packages assets/MonsterGame/CookedXenon/*.xxx
  (UE3 console packages; .xxx = cooked-for-Xenon). NO standalone GlobalShaderCache
  file exists.
- Packages are COMPRESSED: header shows the UE3 compression signature 0x9E2A83C1
  (bytes C1 83 2A 9E) with a chunk table; `strings` finds no FShaderType names
  (data is compressed). 360 cooker typically uses LZX (or zlib/LZO) chunk
  compression.
- Engine/core packages (Core.xxx 64KB, Engine.xxx 1.9MB, EngineMaterials.xxx,
  EngineResources.xxx) likely hold the global/engine shaders; level packages hold
  material shaders.

Extraction path (to produce XenosRecomp input):
1. Parse the UE3 fully-compressed/chunked package header; decompress chunks
   (match the compression method — likely LZX for 360).
2. Parse the decompressed UE3 package: name table + export table.
3. Find the ShaderCache export(s) (UShaderCache / global+material shaders); parse
   the FShaderCache map -> per-shader compiled bytecode = the Xenos shader
   container (the 6-BE-uint32 header + reflection + ucode XenosRecomp wants).
4. Dump those containers; run XenosRecomp (offline) -> DXIL cache; embed/load it;
   bind by hash in the shader hooks.

Tooling: none exists here (reDAHM is a different game, KronosGame, no shader
extraction; UnleashedRecomp's path is Hedgehog-Engine-specific .ar). Needs a UE3
decompressor + FShaderCache parser (UE3 source is widely available as reference;
UE Viewer/umodel handles UE3 package decompression but not shader extraction).
This is a distinct multi-step sub-project — the last mile of the native shader path.
