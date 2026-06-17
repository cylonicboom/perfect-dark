# Port-only Rendering Feature: Display-List Cache (GPU-resident static geometry)

A port-only Fast3D optimization that caches **static room geometry** in persistent
GPU buffers and replays it each frame with a GPU-side model-view-projection
matrix (`uMVP`), instead of the immediate-mode path's per-frame CPU vertex
transform + per-triangle state derivation. Off by default; toggled with
`/dlcache`. GL desktop only; the N64 build and `/dlcache off` are byte-identical
to an unmodified build.

> **Status: Phase 1 + Phase 2 done, plus GPU fog and dynamic vertex lighting.**
> Phase 1 caches a restricted class of static room geometry; Phase 2 makes the cache
> octree-aware (per-vtxbatch culling at replay). 2-cycle, multitexture, grayscale,
> distance fog, and dynamic vertex lighting (shader-side palette) are all cached.
> The remaining un-cached cases — `G_TEXTURE_GEN` (reflective/env-mapped surfaces),
> `G_LIGHTING` (normal-based view-space lighting), and whole-room dyntex exclusion —
> are view-dependent and deferred to Phase 3; see "Roadmap".

---

## Why this works (the key insight)

Fast3D's vertex shader does **no** transform — `gl_Position = aVtxPos`, with
`aVtxPos` already in clip space (the CPU does the whole MVP in `gfx_sp_vertex`).
So you can't cache the post-transform vertex buffer across frames; the camera
bakes into the positions.

But every CPU post-transform fixup is **linear**, so it folds into one matrix:

| Fixup | `gfx_pc.cpp` | Folded into `uMVP` as |
|---|---|---|
| aspect-X | `x' = (aspect_ofs·w + x)·aspect_scale/aspect_ratio` | x-row scaled by `sx`, plus `sx·ofs ×` w-row |
| invert-Y | `y' = -y` (when `clip.invert_y`) | y-row negated |
| z 0→1 | `z' = (z+w)/2` (only when `z_is_from_0_to_1`; **false** on GL desktop) | not needed on GL |

Phase 0 added `uniform mat4 uMVP` to every generated shader and changed the VS to
`gl_Position = uMVP * aVtxPos`. `uMVP` defaults to **identity**, so the immediate
path is unchanged (identity·clip == clip). The cache stores **object-space**
vertices and, at replay, uploads `uMVP = (fixups)·MP_matrix` (the live room
matrix) — reproducing the exact clip-space position the CPU would have computed.

`uMVP` fold lives in `dlcacheReplay` (`gfx_pc.cpp`); the matrix is column-major
(`mvp[col*4 + row]`), uploaded with `glUniformMatrix4fv(..., GL_FALSE, ...)`.

---

## How it hangs together

```
bg.c bgRenderRoomPass LEAF (port-only, /dlcache on, non-dyntex room):
    vis = octree active for this room ? &g_BgCullVisible[startidx] : NULL
    gSPDlCacheBeginEXT(gdl++, vis);            // marker (+ per-frame batch vis)
    gSPDisplayList(gdl++, block->gdl);         // the leaf's geometry
    gSPDlCacheEndEXT(gdl++);                    // marker

renderer (gfx_run_dl):
  G_DLCACHE_BEGIN_EXT: key = leaf gdl pointer peeked from the next gSPDisplayList
     ├─ cache HIT (ready)  → dlcacheReplay(entry); skip the gSPDisplayList
     ├─ cache BAD          → fall through: the gSPDisplayList renders legacy
     └─ cache MISS         → dlcacheBeginRecord(key): the gSPDisplayList renders
                              normally AND tees object-space geometry into a
                              staging buffer (frame-1 is correct)
  G_DLCACHE_END_EXT: dlcacheEndRecord(): upload staging to a STATIC_DRAW VBO,
                     store the segment list keyed by the leaf gdl pointer
```

**Cache key = the leaf `gdl` pointer.** OPA and XLU are separate block trees, so
a leaf gdl belongs to exactly one pass — no pass bit needed. The renderer peeks
it from the `gSPDisplayList` that immediately follows `BEGIN` (no key in the
opcode). When a room reloads its gdl pointer changes → old entry is orphaned and
dropped on the next cache clear (see Invalidation).

### Segments and batches

A **segment** is a contiguous run of triangles sharing all GL state **and**
belonging to one octree vtxbatch. It's broken at three boundaries:

- a `gfx_flush` (any GL state change in `gfx_sp_tri1`) — **bumps the state group**;
- a `G_CULL_BOTH` cull-mode change (CPU-only state, not flushed) — bumps the group;
- a new `G_VTX` (a new vtxbatch) — **same** state group, new `batch_index`.

Each segment records: shader program, bound texture ids + sampler params, depth
mode, viewport, scissor, alpha/modulate, cull mode, a `state_group` id (segments
that share it share *all* GL state), the octree `batch_index` (the k-th `G_VTX` in
the leaf), and `[base_float, num_tris]` into the entry's VBO.

`batch_index` is what ties the cache to the octree: the k-th `G_VTX` in the leaf
maps to `vtxbatches[startidx + k]` (the same mapping `bgEmitLeafCulled` uses), so
`vis[batch_index]` is this batch's per-frame visibility.

### Replay: state-group dedup + draw merge

`dlcacheReplay(entry, vis)` walks the segments once, maintaining a pending draw
range. It re-applies GL state only when `state_group` changes (so a state-run that
was split into many per-batch segments costs **one** state-setup), and merges
consecutive **visible** same-group segments into a single `glDrawArrays` (segments
are buffer-contiguous by construction). A group change flushes the pending draw.
With `vis == NULL` (octree off) every batch is visible, so replay collapses to
one draw per state-run — identical work to Phase 1.

**Octree holes are gap-tolerant** (`g_DlCacheGapTris`, default 256, `/dlcache gap
<tris>`): a culled batch (`vis[batch_index] == 0`) no longer splits the merge
unconditionally — that fragmented a leaf's few large draws into many small ones
and made octree+dlcache *slower* than dlcache alone (measured 475 vs 550 fps on a
29k-tri map; the per-draw overhead outweighed the saved triangles). A culled hole
of ≤ gap tris is instead **absorbed** into the surrounding draw — its triangles
are outside the frustum, so the GPU clips them after trivial vertex shading and
no fragments are rasterised. Only a culled span larger than the gap (e.g. the
half of a huge room behind the camera) splits the draw, which is exactly when
splitting pays. Absorption requires the hole's segments to share the pending
draw's shader program (same vertex stride — a foreign-stride hole forces the
split since one draw range can't span it). `0` restores split-on-every-hole.
`/dlcache stats` prints `draws=` (cache_draw calls last frame) to watch the
effect.

---

## Recording: the tee (`gfx_sp_tri1`)

On a cache miss the leaf renders normally **and** each triangle is teed into
`g_DlCacheStaging`: the same per-vertex float layout `buf_vbo` just produced, but
the 4 position floats are replaced by object-space `(ox, oy, oz, 1)` (captured in
`gfx_sp_vertex` into `LoadedVertex.ox/oy/oz`). The remaining floats (texcoords,
clamp, combiner inputs) are camera-independent and copied verbatim — so the replay
shader's attribute layout matches exactly.

**The record frame captures the WHOLE leaf.** The clip-reject and backface-cull
early-outs in `gfx_sp_tri1` are camera-dependent, so during recording they are
*skipped* (`dlcache_capturing`) — otherwise the record-frame camera would bake in:
off-screen / back-facing triangles would be permanently absent when you later turn
to face them. Visibility is instead handled per-frame at replay by GPU frustum
clipping + `GL_CULL_FACE` (mode recorded per segment). The cost is one frame of
record-frame overdraw (back faces drawn once; z-buffer keeps opaque correct, XLU
may briefly look denser the first time a leaf is seen). Phase 2's octree adds back
coarse CPU culling so fully-offscreen leaves aren't replayed at all.

### Restrictions (recorder aborts → leaf marked `bad` → always legacy)

Checked in `gfx_sp_tri1`; on violation the entry is discarded and future frames
render it legacy without re-recording. Only genuinely un-bakeable state aborts:

- **`G_LIGHTING`** (`GFX_DLC_ABORT_LIGHTING`) — lighting is computed in view space
  (the light dir is transformed by the modelview), so the colour is
  camera-dependent. Baked-vertex-colour rooms (`G_COL`, the common case) are fine.
- **`G_TEXTURE_GEN`** (`GFX_DLC_ABORT_TEXGEN`) — texgen/environment-map UVs are
  computed per-vertex from the view direction, so baking them freezes the
  reflection to the record-frame camera ("textures locked to the view"). Excluded.
- `G_CULL_BOTH` (`GFX_DLC_ABORT_CULLBOTH`) — "draw nothing" cull, no clean GL map; rare.

**Fog is supported** (see "Fog" below), as are 2-cycle, multitexture, and grayscale
— their per-vertex attributes (combiner inputs, second-texture coords, grayscale
colour) are camera-independent and bake correctly. Dyntex rooms are excluded
earlier in `bg.c` via `ROOMFLAG_HASDYNTEX`. Per-frame-varying prim/env/fog **colour**
is **not** detected — it's baked at record time (static-room assumption); documented
limitation, not enforced. (`GFX_DLC_ABORT_FOG` is retained but unused.)

### Fog

PD world geometry almost always has fog, so the cache must handle it. The fog
*colour* is static and baked per-vertex (`aFog.rgb`); only the *factor* matters:

- **constant fog** (blend fog without `G_FOG`) — the factor is `fog_color.a`,
  already static, baked into `aFog.a`. Replay uses it (`uUseVertexFog = 1`).
- **distance fog** (`G_FOG`) — the factor is `z/w`-based, i.e. camera-dependent, so
  the baked value would freeze. The vertex shader **recomputes** it from
  `gl_Position` (`uUseVertexFog = 0` + `uFogMul`/`uFogOff` uniforms): `fz =
  (gl_Position.z / gl_Position.w) * uFogMul + uFogOff`, clamped 0..255, /255 — which
  matches the immediate path because uMVP rows 2,3 (`M[k][2]`/`M[k][3]`) leave z,w
  unchanged, so the shader's `z/w` equals the CPU's `z0/w0`. The factor is computed
  *before* the no-depth-clamp `z*0.3` hack.

`uUseVertexFog` defaults to **1** (baked) and is uploaded on every shader load, so
the immediate path is unchanged (`vFog.a = aFog.a`, identical to the old
`vFog = aFog`). The cache sets it per state group via `gfx_rapi->set_fog_params`,
recording `fog_compute`/`fog_mul`/`fog_off` per segment (a fog-state change breaks
the segment + bumps the state group, like cull).

The shader mirrors `gfx_sp_vertex` **including the near/behind-eye guards** —
clamp `|w| < 0.001` and force max fog for `w < 0` (`winv < 0 → 32767`). These
matter for the room you're standing in: its geometry surrounds the camera, so it
has vertices with tiny/negative `w`; a naive `z/w` gives them garbage fog and
paints the room with the fog/sky colour. For `w > 0.001` it's identical to `z/w`,
so far geometry is unaffected.

`/dlcache stats` reports the OR of abort reasons across all `bad` leaves, so you
can see what's keeping a level from caching (e.g. `bad reasons: fog`).

**`bad` is informational, not an error.** A `bad` leaf renders correctly via the
normal (legacy) path — it just isn't GPU-cached. Seeing `bad reasons: lighting
texgen` on a level simply means some surfaces use view-space lighting / env-map
texgen and were left to the legacy path (the static-coloured majority still
caches). Re-record (the dynamic-lighting trick) does **not** help these: unlike
vertex colours — which change only on lighting *events* — `G_LIGHTING`/`texgen`
change every camera move, so they'd re-record every frame. Caching them needs the
shader-side route (GPU lighting/texgen), see Roadmap.

---

## Replay (`dlcacheReplay`)

`set_mvp(folded)` → `cache_replay_begin(buffer)` → per segment: restore
depth/viewport/scissor/alpha/textures/shader + `cache_set_cull` + `cache_draw`
(`glDrawArrays` from the segment's `base_float`) → `set_mvp(identity)` →
`cache_replay_end()`. Replay drives GL directly, so afterwards it forces the
immediate path to re-establish shader (→ attrib pointers back into `opengl_vbo`)
and textures on its next draw, and `cache_replay_end` rebinds `opengl_vbo` +
disables `GL_CULL_FACE` (the immediate path culls on the CPU).

---

## Invalidation (why stored texture ids / shader pointers never dangle)

A segment stores raw GL texture ids and a `ShaderProgram*`. Both are dropped
**before** the underlying objects are freed by clearing the whole cache
(`dlcacheInvalidateAll` — deletes every VBO + clears the map) on any texture-cache
change:

- `gfx_texture_cache_clear` — fires on **stage load** (`texReset` →
  `videoResetTextureCache`), on texture-filter change (`reset_texture_state`,
  which also clears the shader pool), and on gun-mem free
  (`bgunFreeGunMem` → `videoResetTextureCache`). Stage-load clearing is what
  prevents a reused leaf-gdl address from replaying a previous level's geometry.
- `gfx_texture_cache_delete` / `_delete_range` — dyntex / animated-texture
  invalidation.
- LRU eviction in `gfx_texture_cache_lookup` (texture cache full).

Also cleared by `/dlcache off` and `/dlcache clear`.

> **Texture-cache size + LRU refresh (fixed 2026-06-16 — "progressive black
> textures").** `TEXTURE_CACHE_MAX_SIZE` is a **count** cap, not a memory cap (a
> bigger configured memory pool does *not* raise it). It was `1024` (N64-era);
> AIO HD-texture sets exceed that, and because every eviction calls
> `dlcacheInvalidateAll`, a full cache makes the dlcache re-record constantly,
> the per-frame working set thrashes past the cap, and textures bind to
> evicted/reused ids → **progressively black**. Two fixes: (1) the cap is raised
> to `4096` (memory still bounded by the textures actually loaded); (2) **replay
> now refreshes the texture-cache LRU** for every texture it binds
> (`dlcacheReplay`, via a stored `DlCacheSegment::tex_node`). Without (2),
> textures shown *only* through cached replay never touched the LRU, drifted to
> the front, and were evicted **while still on screen** — so the cache evicted
> exactly the hot static-room textures the dlcache depends on. The node pointer
> never dangles: any eviction/delete calls `dlcacheInvalidateAll`, which drops
> every segment, so a live segment's texture is always still in the cache.

> Side effect: a gun switch (gun-mem free) or any dyntex delete clears the
> **whole** cache, forcing a one-frame re-record. Safe, but it means the cache
> doesn't persist across those events yet — a Phase-3 "purge only affected
> textures" change (the existing `bgunFreeGunMem` TODO) would fix it.

> **Black textures with `/dlcache on` (the recorder texture-import amplifier).**
> The record frame draws the **whole leaf with clip-reject disabled** (`gfx_sp_tri1`,
> so off-screen geometry is captured), which means it also runs `import_texture` for
> the leaf's **off-screen** textures — textures the immediate path never loads. On a
> large/HD texture set this inflates the working set past `TEXTURE_CACHE_MAX_SIZE`
> (the **count** cap), so the LRU evicts **on-screen** textures; they bind to
> evicted/reused ids and render **black**. Every eviction also wipes the dlcache
> (`dlcacheInvalidateAll`), so leaves re-record, re-import, and the thrash sustains
> itself — which is why `/dlcache clear` (recording stays on) does **not** recover
> but `/dlcache off` (recording stops) does. Diagnosis: `/dlcache stats` (or
> `/texcache`) shows `tex=used/max`; if it reads `FULL` while surfaces are black,
> this is it. Fix/lever: **`/texcache N`** raises the cap live (`g_TextureCacheMaxSize`,
> default 4096), and **`Video.TextureCacheSize`** in `pd.ini` persists it — raise it
> until the level's working set fits without eviction (memory is still bounded by the
> textures actually loaded). This is distinct from the 2026-06-16 fix below, which
> addressed eviction *churn*; this is eviction caused by the recorder's extra imports.

---

## `/dlcache` console command (`net.c`)

Routed through `netConsoleCommand` like `/octree` / `/wireframe`; works outside a
net session.

| Command | Effect |
|---|---|
| `/dlcache` or `on` / `off` | toggle `g_DlCacheEnabled` (`bg.c`); `off` also clears the cache |
| `/dlcache stats` | cached/bad entry counts, last-frame replayed segments + tris, front-face winding |
| `/dlcache clear` | drop all cached buffers (re-record next frame) |
| `/dlcache ff` (alias `frontface`) | **calibration:** flip the front-face winding used for cached backface culling. If cached geometry shows inside-out / missing faces vs `/dlcache off`, flip this once. Read live at replay (no re-record). **Persist with `Video.DlCacheFrontFace = ccw\|cw` in `pd.ini`, or the "DL Cache Flip Winding" checkbox in Extended > Video** (off = `ccw`, on = `cw`; `menuhandlerDlCacheWinding` → `videoSet/GetDlCacheFlipWinding`, applies live) — the winding is driver-dependent (one user's 2023 Vulkan driver culled cached walls that the default `ccw` kept; `cw` / this flip restored them while keeping culling on for the perf), so an affected machine pins `cw` once instead of re-typing `/dlcache ff` each launch. Default `ccw` is unchanged for everyone else. |
| `/dlcache palette [on\|off]` | **diagnostic isolation:** `off` makes cached replay skip the shader-side GPU palette (the live vertex-shade substitution) and draw with the **baked record-time shade** instead. Geometry + textures stay cached, so it splits "is this bad surface a vertex-shading/palette bug or a geometry/texture bake bug?" Lighting goes **static** while off (no muzzle-flash brightening) — expected. Read live at replay (no re-record). Default on. |
| `/dlcache cull [auto\|off\|back\|front]` | cached backface-cull mode. **`auto` is the default** (per-segment recorded `G_CULL_*` mode + the `ff` winding). The "turn-around → rooms missing" symptom was a baked *scissor* (now fixed), not culling, so culling is back on for the perf win. `off` draws both faces (opaque z-buffer-identical) — an instant live escape if anything still drops; `back`/`front` force a single `glCullFace`. Read live; no re-record. **Persist with `Video.DlCacheCull = auto\|off\|back\|front` in `pd.ini`** (applied at `videoInit`) — needed because some GPU drivers cull cached geometry the immediate (CPU-cull) path doesn't, dropping whole walls (black on GL / see-through on Vulkan); confirmed driver-dependent (one user's 2023 driver dropped Vulkan-cached walls that `cull off` restored, while other machines were unaffected). |

Renderer side is reached via `extern "C"` shims in `gfx_api.h`
(`gfx_dlcache_clear` / `_get_stats` / `_set_frontface` / `_get_frontface`).

### Live overlay (Lua)

`/dlcache stats` is a one-shot console snapshot. For a **live** readout there's a
Lua binding `pd.dlcache_stats()` (in `src/game/luaai_api.c`, `#ifndef PLATFORM_N64`)
returning `{ enabled, cached, bad, batches, tris, fog, lighting, cullboth, empty,
texgen, tex_used, tex_max }` straight off `gfx_dlcache_get_stats` +
`gfx_get_texture_cache_fill` + `g_DlCacheEnabled`.
`scripts/dlcache_overlay.lua` draws those top-right (below the octree + perf
overlays) every frame while `/dlcache` is on — so you can watch `cached` climb as
rooms record, the replayed `batches`/`tris` rise and fall as you move (and as the
octree culls), the **`tex used/max` line turn red `FULL`** the moment the texture
cache overflows (the recorder evicting on-screen textures → black surfaces; raise
with `/texcache` or `Video.TextureCacheSize`), and the `bad:` reason flags appear.
Wired into `scripts/init.lua`; comment that `load(...)` line out to hide it.
Mirrors `scripts/octree_overlay.lua`.

---

## Dynamic vertex lighting (shader-side GPU palette)

PD lights rooms by **per-vertex colour**: `gfx_sp_vertex` resolves each vertex's
colour from the room's palette, and dynamic lighting — shooting a light out (room
darkens), a muzzle flash / spark / explosion brightening nearby surfaces — works by
`dlights.c`'s `roomHighlight()` **rebuilding `g_Rooms[roomnum].colours` every
frame**. The cache bakes each vertex's resolved shade colour at record time, so a
naive cache would freeze a room's lighting.

**Change detection.** `bg.c` FNV-hashes the room's colours once per room per frame
(`bgDlCacheRoomColoursDirty`, cached in the port-only `room.dlcolourhash`/
`dlcolourhashframe`/`dlcolourdirty` fields) and passes a 1-bit "dirty" flag in the
`BEGIN` opcode (`w0` low bit). A *static* room hashes the same every frame; an
*actively-lit* room flags dirty. The hash only runs while `/dlcache` is on.

**Fix — shader-side GPU palette (desktop GL).** Each cached vertex stores the
palette **index** (`v->colour>>2`) as a trailing float; the leaf's single `G_COL`
palette is uploaded as an N×1 RGBA8 texture. At replay the **vertex shader** looks
up `shade = palette[index]` and routes it into the combiner's shade input slots
(per a per-draw `uShadeRoute`, derived from `comb->shader_input_mapping`). On the
**dirty** bit the renderer just **re-uploads the tiny palette texture** — the
geometry VBO stays put, so dynamic lighting runs at **cache speed** (no re-record).
When colours are static, the lookup reproduces the baked shade exactly
(byte-identical). Gated by `uPaletteEnable` (default 0 → immediate path unchanged).

This is also the **GPU per-vertex-lighting hook**: a mod that drives a room's
palette (or the shade lookup) gets live per-vertex lighting on cached geometry.

**Fallback — invalidate-on-re-record.** The palette path needs **desktop GL with
GLSL ≥ 130** (the shade routing uses integer bitwise ops + `texelFetch`, and GL-ES
has no guaranteed vertex-shader texture units). `cache_create_palette` returns 0
otherwise → `palette_ok` false. Non-palette leaves (ES, GLSL < 130, or a leaf with
>1 `G_COL`) fall back to dropping + re-recording the entry on dirty — correct, just
rebuilds that leaf's VBO each changed frame.

> Implementation note: the per-input shade routing generates a lot of vertex-shader
> source, so `vs_buf` was enlarged (2048 → 8192). Undersizing it overruns the stack
> and crashes at the first shader compile with no error.

> Particle/sprite effects (sparks, muzzle flash, tracers) are separate props, not
> cached, so they render regardless — this is specifically about their *illumination
> of room surfaces*.

> **Diagnosing a dark / "texture-not-loading" surface that also won't flash when
> shot near it.** A surface that the muzzle flash *doesn't* brighten is the
> signature of the **vertex-shade path**, not texture loading: the flash brightens
> walls only by re-uploading the room palette (or re-recording), so a wall that
> never flashes is one whose shade isn't being driven live — most likely its shade
> stays frozen at the (possibly dark) record-time baked value because the
> per-segment shade routing missed its combiner slot, or its `G_COL`/colour index
> doesn't resolve to the palette the flash updates. **Isolate it with `/dlcache
> palette off`:** that forces *every* cached leaf to draw the baked record-time
> shade (geometry + textures unchanged, lighting static everywhere).
> - If the dark surface now renders **correctly** (just with static lighting like
>   the rest of the room) → it's the **GPU palette / shade-routing** path. Look at
>   `g_DlCacheSegShadeRoute` derivation (`gfx_pc.cpp`, the `shader_input_mapping`
>   scan) and the `aShadeIdx`/`uShadeRoute` lookup in the VS (`gfx_opengl.cpp`).
> - If it's **still dark/untextured** with palette off → the bug is in the
>   **geometry/texture bake** (texture id binding, a `bad` leaf, or a culled batch),
>   *not* vertex shading — pursue `/dlcache stats`, `/dlcache cull off`, and the
>   texture-cache LRU instead.
>
> This split is exactly what `/dlcache palette off` exists for; it's a live,
> reversible replay-time switch (no re-record).

This is also the hook a "per-vertex lighting" mod would use: poke a room's colours
and the affected leaves re-record and show it. The perf-preserving, true-GPU
alternative (store the colour **index** per cached vertex + live palette UBO +
shader lookup) is still in the Roadmap; it needs combiner-aware codegen because the
shade colour is folded into generic combiner-input slots, so its identity is lost at
the shader level.

## Gotchas

- **GL uniforms default to 0, not identity.** `uMVP` is explicitly seeded to
  identity at shader creation and re-uploaded in `gfx_opengl_set_uniforms` on
  every shader load, so the immediate path is always identity.
- **Viewport + scissor are taken LIVE at replay, never baked.** PD scissors each
  room to its portal-clipped draw-slot box (`bg.c` `bgScissorWithinViewportF(thing->
  box)`), which is view-dependent — the screen rectangle moves/shrinks as the camera
  turns. `dlcacheReplay` sets viewport+scissor once from `rdp.viewport`/`rdp.scissor`
  (set per-room by the draw-slot loop before the `BEGIN` marker each frame). Baking
  them (an earlier bug) clipped each cached room to where it *used* to be on screen,
  so rooms vanished when you turned. Everything else recorded per segment
  (shader/textures/depth/combine) is genuinely view-independent. Anything new that's
  view-dependent must likewise be read live, not recorded.
- **Cull is AUTO by default; the earlier "missing rooms" was the baked scissor, not
  culling.** A single `glFrontFace` plus the per-segment `G_CULL_*` mode reconciles
  the N64 cross-product cull (the invert_y winding flip is global), so one winding
  should be correct for all geometry; testing found `CCW` correct. If a clean test
  (octree off) still drops geometry with cull on, suspect the octree batch-vis
  mapping first, then the winding — `/dlcache cull off` is the instant escape, and
  `/dlcache cull back|front` + `/dlcache ff` characterise it. **Caveat with cull
  off:** one-sided *translucent* surfaces draw both faces and can look denser.
- **Front-face calibration.** Whether the N64 cull maps to GL `GL_CCW`/`GL_CW`
  front depends on `invert_y`; `/dlcache ff` settles it empirically. Default `CCW`.
- **Wireframe interaction.** `/wireframe` operates in `draw_triangles`
  (`glPolygonMode`); cached replay uses its own `cache_draw`, so cached rooms
  don't render as wireframe while `/dlcache on`. Cosmetic; not addressed.
- **Octree composition (Phase 2).** When `/dlcache` is on it takes precedence over
  the CPU-filter octree path (`bgEmitLeafCulled`) and absorbs culling: the bracket
  passes `&g_BgCullVisible[startidx]` so cached replay skips culled batches. The
  CPU-filter path now only runs when `/dlcache` is **off** but `/octree` is on.
  Note: `/octree stats` batch counters are only updated by the CPU-filter path, so
  they read zero when `/dlcache` is also on — use `/dlcache stats` instead.
- **Master DL size.** Each cached leaf adds 2 `Gfx` words (BEGIN/END) to the room
  pass. Modest (visible leaves are draw-slot-bounded), but on a very large level
  with `/dlcache on` watch for `-mgfx` pressure.

---

## Roadmap

- **Phase 2 — octree integration. ✅ done.** Cache at `struct vtxbatch` granularity;
  the per-frame octree `visible[]` (`g_BgCullVisible`, see `PORT_OCTREE.md`) selects
  which cached batches replay — culled batches cost neither CPU transform nor a draw
  call. Validate: `/octree markall` (or `bigroom`) + `/dlcache on`, then `/octree
  forcecull` → the cached room goes black (replay honours culling).
- **Fog on the GPU. ✅ done.** Distance fog (`G_FOG`) is recomputed in the vertex
  shader from `gl_Position`; constant fog bakes. See "Fog" above. This is what makes
  the cache actually engage on real (fogged) PD levels.
- **Dynamic vertex lighting + shader-side GPU palette. ✅ done.** Colour index per
  vertex + N×1 palette texture + in-shader shade lookup/routing; dynamic lighting at
  cache speed (dirty → re-upload palette, not re-record). Desktop GL; ES uses the
  re-record fallback. See "Dynamic vertex lighting" above.
- **Phase 3 — remaining coverage.** The leaves that still fall back to the legacy
  path, in rough order of payoff for static rooms:
  - **`G_TEXTURE_GEN`** (`GFX_DLC_ABORT_TEXGEN`) — reflective / environment-mapped
    surfaces. UVs are currently generated per-vertex from the view direction; baking
    freezes them to the record-frame camera. Needs per-vertex normals captured into
    the cache buffer + a texgen matrix uniform so the VS regenerates UVs at replay.
  - **`G_LIGHTING`** (`GFX_DLC_ABORT_LIGHTING`) — normal-based view-space lighting
    (rarer on static room geometry than on props). Same prerequisite: normals in the
    cache + a normal/light uniform set computed in the shader.
  - **Dyntex rooms** (`ROOMFLAG_HASDYNTEX`, excluded whole-room at the `bg.c`
    bracket) — scrolling/animated textures. Needs the cache to tolerate per-frame
    texture-id changes for the animated tiles only (texture pinning, below).
  - Supporting work: per-room palette texture sharing (currently per-leaf); dynamic
    prim/env/fog **colour** (still baked); texture pinning so gun switches / dyntex
    don't clear the whole cache; auto-cache rooms flagged for octree; later, dynamic
    models (per-object `uMVP`).

---

## Files touched

| File | Change |
|---|---|
| `port/fast3d/gfx_opengl.cpp` | `uMVP` + `set_mvp`; GPU fog (`set_fog_params`); GPU palette VS (`aShadeIdx`/`uPalette`/`uPaletteEnable`/`uShadeRoute`, shade lookup+routing, desktop-only) + palette texture + `set_palette_enable`/`set_shade_routing`; persistent-buffer replay/`cache_draw` (stride+1)/`cache_set_cull`; vtable entries |
| `port/fast3d/gfx_rendering_api.h` | `set_mvp` + `set_fog_params` + `cache_*` + palette (`cache_create/delete/upload/bind_palette`, `set_palette_enable`, `set_shade_routing`) rapi entries |
| `port/fast3d/gfx_pc.cpp` | cache structs/globals; record tee (`gfx_sp_vertex` obj-pos + colour index, `gfx_sp_tri1` shade routing, `gfx_flush`); `G_COL` palette capture; `G_DLCACHE_*_EXT` dispatch (dirty → palette re-upload); `dlcacheReplay` (uMVP fold + palette); invalidation; `extern "C"` API |
| `port/fast3d/gfx_api.h` | `extern "C"` `gfx_dlcache_*` declarations |
| `src/include/gbiex.h` | `G_DLCACHE_BEGIN_EXT 0x46` / `G_DLCACHE_END_EXT 0x47` + `gSPDlCache*EXT` macros (`BEGIN` carries vis ptr + dirty bit) |
| `src/include/types.h` | port-only `room.dlcolourhash` / `dlcolourhashframe` / `dlcolourdirty` (dynamic-lighting re-record) |
| `src/game/bg.c` | `g_DlCacheEnabled`; cache bracket in `bgRenderRoomPass` LEAF (precedence over `bgEmitLeafCulled`); `bgFindLeafBatchStart` (octree vis slice); `bgHashColours`/`bgDlCacheRoomColoursDirty` (dynamic-lighting dirty) |
| `src/include/game/bg.h` | `extern bool g_DlCacheEnabled` |
| `port/src/net/net.c` | `/dlcache` command + `/help` line |
| `src/game/luaai_api.c` | `pd.dlcache_stats()` Lua binding (live overlay) |
| `scripts/dlcache_overlay.lua` (new) + `scripts/init.lua` | live on-screen cache stats overlay |
