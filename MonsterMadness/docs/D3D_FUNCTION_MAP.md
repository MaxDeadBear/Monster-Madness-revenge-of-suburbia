# Monster Madness — D3D function map (for the ReOdyssey-style native renderer)

Goal: adopt ReOdyssey's blueprint (native D3D-function hooks + Plume). That needs
MM's `D3DDevice_*` functions named in the config `[functions]` table so codegen
emits `rex_D3DDevice_*` symbols that `REX_HOOK` can target. See
docs/RENDERER_MAPPING.md and the `reodyssey-reference-renderer` memory.

## Derivation method (no FLIRT sigs needed — from MM's own code)

Every command-emitting 360 D3D method reserves push-buffer space via a small
helper and then writes PM4 packets. In MM those helpers are `sub_8220A2B8` and
`sub_822091A8`. So **callers of those helpers = the D3D device command methods**.
`tools/ida_d3d_find.py` lists them with size, caller-count, and the PM4 type-3
opcodes each emits (extracted from immediate constants) to tell Draw / SetConstant
/ etc. apart. Output: `assets/ida_dump/d3d_methods.txt` (54 candidates).

## Confirmed anchors

| addr | role | evidence |
|------|------|----------|
| `0x822122D0` | `D3DDevice_Swap` (present) | calls VdSwap/VdGetSystemCommandBuffer |
| `0x8220A800` | device / ring-buffer init | calls VdInitializeRingBuffer |
| `0x8220A2B8` | push-buffer reserve/wrap helper | called by all command methods |
| `0x822091A8` | push-buffer reserve (variant, used by Swap) | |

## Candidates by PM4 opcode (top, from d3d_methods.txt)

Draw emitters (→ `D3DDevice_Draw*`):
- `0x8265BBA0` (1184b, 10 xref) DRAW_INDX — HIGH-LEVEL: also calls state setters
  (`0x8220D260,0x8220BB68,0x8220C140,0x8220BFC0,0x8220BF00`) then reserve+draw.
  Likely `D3DDevice_DrawIndexedVertices` (or a UE3 RHI draw wrapper).
- `0x8265C098`, `0x8265C5C8`, `0x8265C9B0` — DRAW_INDX + EVENT_WRITE (draw variants).
- `0x8221FE28` (424b, 4 xref) DRAW_INDX_2 only — low-level packet emitter; many
  args incl. `float*`/`double` => likely `DrawVerticesUP`/`BeginVertices` (inline
  vertices).
- `0x8221E5D8` (2200b) DRAW_INDX_2 — composite draw.

State setters (→ `D3DDevice_Set*` / render-state):
- `0x8220BB68`, `0x822024A8`, `0x8220D260`, `0x82211788` — SET_CONSTANT emitters.
- `0x82211DA0`, `0x8220BFC0`, `0x82205B00` — WAIT_REG_MEM (sync/state).

## ReOdyssey target names to map onto these (from reodyssey_config.toml)

Resource: D3DResource_Release/AddRef; D3DDevice_Create{VertexBuffer,IndexBuffer,
Texture,Surface,VertexDeclaration}. Bind: SetVertexShader/SetPixelShader/
SetVertexDeclaration/SetStreamSource/SetIndices/SetTexture/SetSamplerState. Draw:
DrawVertices/DrawIndexedVertices/DrawVerticesUP/BeginVertices. Target/state:
SetRenderTarget/SetViewport + RsAlphaBlendEnable/RsBlendOp/... (D3DRS_*). Present:
D3DDevice_Swap (=0x822122D0). Plus FXeVertexShader_Init/FXePixelShader_Init,
XGSetVertexDeclaration.

## Per-function decompilation results (2026-06-19)

Key refinement: the reserve-caller heuristic reliably isolates **draws** and
**internal state-flush helpers**, but NOT the public binding setters (those are
inline on 360 — set a dirty bit + store, no PM4 at call time — so they never call
the reserve helper).

- `0x8265BBA0..0x8265C9B0` — a **cluster of 4 draw functions** (BBA0/C098/C5C8/
  C9B0), matching ReOdyssey's 4 hooked draws (DrawVertices / DrawIndexedVertices /
  DrawVerticesUP / BeginVertices). `0x8265BBA0(device, primType, count, stride)`
  flushes dirty state (calls the flush helpers below), allocates a vertex buffer
  via `sub_822096C8`, emits draw-initiator `primType|(count<<16)|0x80` (auto
  index) => **DrawVerticesUP-shaped**. The DRAW_INDX (index-buffer) members are the
  DrawIndexedVertices variants. (d3d9.lib spans ~0x8220xxxx..0x8265xxxx in MM; the
  Draw* family is clustered at 0x8265Bxx-Cxx, like ReOdyssey's Set* cluster.)
- `0x8221FE28(device, count, vtxptr, color[4], z, ...)` — specialized **2D
  colored-primitive draw**: builds 21 floats/prim (3 verts x pos+color), half-pixel
  offset; an internal UI/line draw helper, not the clean public draw API.
- `0x8220BB68(device, dirtyMask, ...)`, `0x822024A8(device,...)`, `0x8220D260`,
  `0x8220BF00` — **internal render-state flush helpers** (write register blocks per
  dirty bits, return remaining mask). Called by the draw functions. NOT hook
  targets, but they reveal which device-struct offsets hold which render state.

CONFIDENCE: Swap (0x822122D0) and device-init (0x8220A800) high; the 0x8265 draw
cluster = the Draw* family high; exact 1:1 within the cluster medium (needs arg/
packet confirmation per function); flush helpers identified (not hookable).

## Draw-family 1:1 (call-graph-CONFIRMED, 2026-06-19)

Resolved via the call graph + the pending-field (3357..3361) read/write scan
(`tools/ida_find_offset_refs.py` -> offset_refs.txt), which is the reliable
disambiguation. Corrected earlier best-effort guesses.

| addr | confirming evidence | name | conf |
|------|---------------------|------|------|
| `0x8265C5C8` | `(dev, primType, startVtx, count)`; auto-index (src_sel 0x80), **NO buffer alloc**, draws from the bound stream; prim-split loop | **D3DDevice_DrawVertices** | high |
| `0x8265C9B0` | prim-split loop, draws from the **bound** index buffer, DRAW_INDX_2 | **D3DDevice_DrawIndexedVertices** | high |
| `0x8265C050` | `(dev, type, count, pData, stride)`; calls BeginVertices(0x8265BBA0), **memcpy's user data** (sub_827B17A0(buf,pData,count*stride)), then EndVertices | **D3DDevice_DrawVerticesUP** | high |
| `0x8265BBA0` | allocates 1 vtx buffer, returns it, stores pending (3357/8/60); **called by DrawVerticesUP as its begin step** | **D3DDevice_BeginVertices** | high |
| `0x8265C098` | allocates **vertex+index** buffers, returns both via out-params, stores pending (3357..3361) | **D3DDevice_BeginIndexedVertices** | med-high |
| `0x8265C040` | 12 bytes: `device+48 = device+13428` — commits the pending push pointer | **D3DDevice_EndVertices** | high |

KEY CORRECTION: 0x8265BBA0 is BeginVertices (not DrawVerticesUP); DrawVerticesUP
is 0x8265C050 (it calls Begin + memcpy + End). Distinguisher: DrawVertices/
DrawIndexedVertices draw from BOUND streams with no alloc; Begin* allocate and
return buffers; DrawVerticesUP copies user data.

STILL MISSING: EndIndexedVertices (pairs with BeginIndexedVertices; likely near
0x8265C040, reads index pending fields) and possibly DrawIndexedVerticesUP.
ReOdyssey's 4 hooked draws (DrawVertices/DrawIndexedVertices/DrawVerticesUP/
BeginVertices) are all now covered.

## Binding setters (derived via slot writers, 2026-06-19)

Method that worked: the setters store a resource into a device-struct slot + set
a dirty bit, no PM4 (so absent from the reserve-caller list). Find the slot a draw
READS, then its WRITER is the setter (`tools/ida_find_offset_refs.py` with the
slot offset). DrawIndexedVertices reads the index descriptor at device+0x308C
(idx 3107) -> its RMW writer is SetIndices. SetIndices sits in a setter cluster
(~0x82204D38..0x82205FC8); decompiling the cluster gives the rest.

| addr | evidence | name | conf |
|------|----------|------|------|
| `0x82205060` | stores arg into device[12428] (idx slot 0x308C) after releasing old; `(device, pIndexData)` | **D3DDevice_SetIndices** | high |
| `0x82204EB8` | **indexed by stream a2**; writes vertex buffer (+24/+28) + stride into stream slot `4*(a2+3113)`, sets dirty (device+24 \|= bit) | **D3DDevice_SetStreamSource** | high |
| `0x82204D38` | reads `(x,y,w,h)` ints + minZ/maxZ floats (device+12644/12652) | **D3DDevice_SetViewport** | med-high |
| `0x82204FD8` | reads stream slot `4*(a2+3113)`, refcounts, out-params | **D3DDevice_GetStreamSource** | med |
| `0x82205770` | builds state block via sub_8220ADA8, compares/updates device+3745 | **D3DDevice_SetVertexShader** (paired) | med |
| `0x82205838` | builds state block via sub_8220AF88, parallel to 0x82205770 | **D3DDevice_SetPixelShader** (paired) | med |

NOT setters: `0x82205EA8` = D3DResource AddRef (atomic refcount); `0x82205FC8` =
format/type query helper.

### Sampler / render-state setter family + resource mgmt (2026-06-19)

Indexed-by-stage sampler entries live at `device + 24*(stage+48)` (24 bytes each).
The per-state setters are a FAMILY of small functions (one per state field, like
ReOdyssey's RENDER_STATE_HOOK macro), each modifying a field + setting the
per-stage dirty bit `device[24] |= 1<<(stage+32)`:

| addr | role | conf |
|------|------|------|
| `0x822046C0` | SetSamplerState (writes sampler entry field, per-stage dirty) | med |
| `0x82204758` | SetSamplerState (another state field; uses dword_820C5498[val]) | med |
| `0x82204AC0` | GetSamplerState (`(entry>>10)&7`) | med |
| `0x82204B10` | GetSamplerState (`entry>>13`) | med |

Resource management (NOT setters, but needed by the renderer):
`0x82205EA8` AddRef (atomic +1), `0x82205D90` Release (atomic -256),
`0x82205F20` refcount -1, `0x82205900` resource free, `0x82205FA0` GetDevice
(VdGlobalDevice @0x101BE + addref), `0x82204FD8` GetStreamSource,
`0x82205738` GetRenderTarget (reads device+14808).

### SetTexture / SetVertexDeclaration / SetRenderTarget (2026-06-19)

Found via the fetch-constant builder (sub_8220C2A0 copies fetch constants from
device+240=vertex, device+752=texture) + CONFIRMED by the state-reset function
sub_82219718, which calls them with null args:

| addr | evidence | name | conf |
|------|----------|------|------|
| `0x822050F8` | `(device, stage, texture)`: stores texture into per-stage slot `4*(stage+3108)`, builds fetch constant from texture+28. Reset func calls it as SetTexture(dev,stage,0) for stages 0..3 | **D3DDevice_SetTexture** | high |
| `0x82205460` | stores decl ptr into device+12448 (single slot, next to SetIndices' 12428). Reset func calls it as SetVertexDeclaration(dev,0) | **D3DDevice_SetVertexDeclaration** | high |
| `0x82219460` | `(device, surface)`: switches on surface format (a2[16]), builds RB_SURFACE state, writes RT slot device+14808 (a1[3702]) | **D3DDevice_SetRenderTarget** | med |

SetRenderTarget caveat: signature is `(device, surface)` with no explicit RT
index, so 0x82219460 may be the primary-RT bind or SetDepthStencilSurface;
device slots a1[3700/3701/3702] (14800/04/08) are the RT/depth surface slots
(all released together by the reset func sub_82219718). Confirm color-vs-depth /
index by call-site.

NOTE: 0x82205770/0x82205838 (paired state-block builders vs device+3745) are the
SetVertexShader/SetPixelShader candidates but remain med confidence (haven't
confirmed the block holds shader ucode).

## Remaining work to finish the map

1. Decompile each candidate; match to a ReOdyssey name by its PM4 opcode + arg
   signature + callees (Draw funcs call reserve+draw; Set funcs are small,
   write one register block). The `Set{Vertex,Pixel}Shader`/`SetTexture`/
   `SetStreamSource`/`SetIndices` are small device methods — find them among the
   non-draw callers and via shader/stream register writes.
2. Resource-creation funcs (`Create*`) don't emit PM4 (they allocate) — find via
   the D3D allocation patterns / xrefs from the Create call sites, not this list.
3. Write the `[functions]` table into MM's config with `rex_D3DDevice_*` names;
   re-run codegen.
4. Vendor `plume` + `XenosRecomp` (thirdparty), set `config.graphics = null`,
   port `src/render/` from ReOdyssey, REX_HOOK the named functions.
