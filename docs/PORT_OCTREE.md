# Port-only Rendering Feature: Outdoor-Room Octree Culling

A port-only frustum-culling layer for large outdoor rooms. Rooms flagged
`ROOMFLAG_EX_OCTREE` build a per-room octree of their vtx/tri batches at load
time; each render pass traverses the octree, rejects whole subtrees that fall
outside the view, and submits a **filtered copy** of the room's display list
containing only the surviving batches. Every other room — and the entire N64
build — is unchanged. Implemented entirely under `#ifndef PLATFORM_N64`.

> The N64's only visibility mechanism is portal/room-level culling
> (`bgTickPortals` → draw slots). Once a room is "on screen" all of its geometry
> is drawn, even the half behind the camera. This adds *sub-room* culling for the
> handful of big open rooms where that waste is large.

---

## How it hangs together

```
room load ──► bgFindRoomVtxBatches (existing, builds vtxbatches[] world AABBs)
                     │  (octree-flagged rooms only)
                     ▼
              bgBuildRoomOctree ──► g_Rooms[roomnum].octree   (camera-independent)

render pass (per viewport, per OPA/XLU pass):
  bgRenderRoomOpaque / bgRenderRoomXlu
    ├─ bgCullBeginPass:  visible[] = gfxAllocate(numvtxbatches);
    │                    bgOctreeMarkVisible(frustum) fills visible[];
    │                    arm file-static g_BgCullVisible / g_BgCullRoom
    ├─ bgRenderRoomPass  (unchanged block-tree walk; LEAF case swapped)
    │     └─ bgEmitLeafCulled:  all visible → original gSPDisplayList (zero-copy)
    │                           none visible → emit nothing
    │                           partial → copy survivors+state into gfxAllocate
    │                                     scratch, gSPEndDisplayList, branch to it
    └─ bgCullEndPass:    g_BgCullVisible = NULL
```

The octree is built once per room **load** (it depends only on geometry), and is
freed in `bgUnloadRoom`. Visibility is recomputed every pass because it depends
on the camera; the result lives in the per-frame `gfxAllocate` (vtx) pool, same
lifetime as all other per-frame vertex data.

---

## The flag — `ROOMFLAG_EX_OCTREE` (`src/include/constants.h`)

```c
#define ROOMFLAG_EX_OCTREE  0x0002   // in the port-only extra_flags word
```

Lives in `struct room.extra_flags` (the port-only u16 overflow word, alongside
`ROOMFLAG_EX_WEATHERPROOF 0x0001`), **not** the N64 `flags` word. It is
deliberately separate from `ROOMFLAG_OUTDOORS 0x8000` (which `dlights.c` uses for
lighting) and from `roomHighlight`/`br_flash` (the light-flash effect) — toggling
octree culling must not touch gameplay/lighting state.

`struct room` gains one port-only tail field (`src/include/types.h`):

```c
#ifndef PLATFORM_N64
    u16 extra_flags;
    struct bgoctree *octree;   // NULL unless flagged AND loaded
#endif
```

No room sets the flag by default (there is no setup-data wiring yet), so the octree
is activated at runtime via `/octree mark` / `markall` / `bigroom` (below).
`bgLoadRoom` still rebuilds the octree for any room that already carries the flag
(e.g. one re-flagged on reload after `/octree mark`, or set per-level by
`aiSetRoomOctree` — see "Per-level flagging" below).

---

## Data structures (`src/game/bg.c`, all `#ifndef PLATFORM_N64`)

```c
struct bgoctreenode {
    f32 bbmin[3], bbmax[3];  // UNION AABB of the whole subtree (world space)
    s32 children[8];         // child node indices, -1 if absent
    s32 firstbatch;          // offset into bgoctree.batchindices
    s32 numbatches;          // batches stored directly at THIS node
};
struct bgoctree {
    struct bgoctreenode *nodes;  s32 numnodes;
    s32 *batchindices;           s32 numbatchindices;  // batches grouped by node
    s32 maxdepth;
};
```

It is a **centre-split octree / BVH hybrid**: batches are partitioned into octants
by their bbox centre (each batch lands in exactly one node), but each node stores
the *union* AABB of its whole subtree. That union is what the frustum test uses,
so culling a node never drops a batch that pokes past the geometric octant
boundary. Build params: `BG_OCTREE_LEAF_THRESHOLD 8`, `BG_OCTREE_MAX_DEPTH 6`.

The octree is built over the **existing** `g_Rooms[roomnum].vtxbatches[]` array
(one batch = one `G_VTX` + its following tris, with a world-space AABB), which
`bgFindRoomVtxBatches` already builds for hit detection — no new gdl walking.

---

## Why per-batch culling is sound

`bgPopulateVtxBatchType` assigns one batch per `G_VTX` in gdl order, and
`bgTestHitInVtxBatch` confirms each batch's tris reference **only** that batch's
own `G_VTX` vertices (the tri walk runs until the next `G_VTX`/`G_ENDDL`, skipping
interleaved state commands). So a batch is self-contained: dropping a culled
batch's `G_VTX`+tris cannot break a surviving batch, **provided** the
texture/combine/tile/othermode state commands between batches are preserved.

`bgEmitLeafCulled` therefore copies **every non-geometry command unconditionally**
and only conditionally copies `G_VTX`/`G_TRI1`/`G_TRI4`. It never decodes vertex
indices — it copies whole 8/16-byte `Gfx` words verbatim — so the encoding of tri
commands is irrelevant to correctness.

Batch ↔ leaf matching: the k-th `G_VTX` in `block->gdl` maps to
`vtxbatches[startidx + k]`, where `startidx` is the first batch whose
`.gdl == block->gdl` (batches for one leaf are contiguous and in `G_VTX` order).
This is the same `gdl == block->gdl` pointer identity `bgFindVerticesForGdl` uses.

---

## Memory: filtered lists go to per-frame scratch, not the master DL

The master display list (`g_GfxBuffers`) holds only branch commands; room
geometry lives in the room's own `gfxdata` and is reached via a `gSPDisplayList`
*branch*. Inlining a filtered copy into the master DL would blow its `-mgfx`
sizing, so `bgEmitLeafCulled` allocates the filtered list from `gfxAllocate` (the
per-frame vtx pool, reset by `gfxSwapBuffers`) and emits a single
`gSPDisplayList` branch to it — exactly like the original, just to a filtered
copy. `gfxGetFreeVtx()` guards both the `visible[]` and the scratch allocations:
if the pool is low we fall back to the **unfiltered** original list, so pool
pressure can never produce a broken frame (worst case: that room isn't culled
this frame). If a stage routinely hits the guard, raise `-mvtx` for it.

`gfxGetFreeVtx()` was previously defined in `gfxmemory.c` but **not declared**;
this feature adds its prototype to `src/include/game/gfxmemory.h` (next to
`gfxGetFreeGfx`).

---

## `/octree` console command (`port/src/net/net.c`)

Routed through `netConsoleCommand` like `/wireframe` and `/spec` (works outside a
net session). Drives the port-only globals in `bg.c`:

| Command | Effect |
|---|---|
| `/octree` or `/octree on` / `off` | toggle `g_BgOctreeEnabled` (master cull switch) |
| `/octree forcecull` (alias `cull`) | toggle `g_BgOctreeForceCullAll` — marks every batch culled, so flagged rooms render **black** (props still draw). Proves the filtered list is what's submitted. |
| `/octree stats` | print last-frame `g_BgOctreeStats`: octree passes, nodes tested/culled, batches drawn/culled |
| `/octree mark` | flag the room the player is standing in + build its octree now (`bgOctreeMarkCurrentRoom`). Targeted test without reaching the permanently-flagged rooms. |
| `/octree markall` | toggle `g_BgOctreeMarkAll` — treat **every** loaded room as octree-enabled, building each octree lazily on first render (`bgCullBeginPass`). Lets you test culling in *any* level and walk around (newly-streamed rooms are picked up automatically), e.g. `markall` then `forcecull` blanks the whole world as you move. |
| `/octree bigroom` | toggle `g_BgOctreeBigRoom` — treat the **whole level as one open space**: disables portal room-culling (ORs into `g_BgNoCull` + `g_BgNoDrawSlotLimit` in `bgTickPortals`, so every room renders) *and* octree-culls every room, making the octree the sole visibility mechanism. Best on open levels — there's no occlusion culling, so an indoor level renders everything in the frustum (heavy). Disabling portal culling flags every prop on-screen, so the on-screen-prop buffer was raised to `MAX_ONSCREEN_PROPS` + bounded in `propsSort` to stop an overflow crash — see `PORT_NO_CULLING.md`. |
| `/octree portal` | toggle `g_BgOctreePortalCull` (**default on**). When on, octree nodes are frustum-tested against each room's **portal-clipped draw-slot box** (`bgGetRoomDrawSlot(roomnum)->box` — the screen rectangle the room is actually visible through, already used to scissor it) instead of the full viewport. So a room glimpsed through a doorway only submits the batches visible *through that doorway*, not the whole frustum. Bigroom rooms are unaffected (portals off → their draw-slot box is the whole screen). |
| `/octree unmark` | clear all runtime marks: `g_BgOctreeMarkAll`, `g_BgOctreeBigRoom`, every room's `ROOMFLAG_EX_OCTREE`, and all built octrees (`bgOctreeUnmarkAll`) — back to the unculled path. |

The `mark`/`markall`/`unmark` commands and the lazy build in `bgCullBeginPass`
are test conveniences, but the lazy-build-on-first-render is also the mechanism
the "wiring to setup data" follow-up below would reuse for late-flagged rooms.

`g_BgOctreeEnabled` / `g_BgOctreeForceCullAll` / `g_BgOctreeMarkAll` are game-side `bool` (= `s32`, 4
bytes); net.c reads/writes them through `game/bg.h` where `bool` is also `s32`
(both translation units pull `#define bool s32` from `types.h`), so there is **no**
`bool`-width bridging hazard here — unlike the renderer's 1-byte `gfx_wireframe_mode`
(see `PORT_WIREFRAME.md`).

`g_BgOctreeStats` is zeroed at the top of `bgRenderScene`, so it reflects the most
recent scene render (the last viewport, in split-screen).

### Live overlay (Lua)

`/octree stats` is a one-shot console snapshot. For a **live** readout there's a Lua
binding `pd.octree_stats()` (in `src/game/luaai_api.c`, `#ifndef PLATFORM_N64`) that
returns a table `{ drawn, culled, nodes, nodesculled, passes }` straight off
`g_BgOctreeStats`. `scripts/octree_overlay.lua` registers a `pd.on("draw", …)` handler
that draws those counters top-right every frame (only while an octree room is rendering,
i.e. `passes > 0`), so you can watch `drawn` rise and fall as geometry leaves the
frustum. It's wired into `scripts/init.lua`; comment that `load(...)` line out to hide
it. The Lua layer is port-only (the binding is N64-guarded since `g_BgOctreeStats` is).

A compile-time `PD_OCTREE_DEBUG` (default `0`) in `bg.c` logs per-room build stats
(`sysLogPrintf(LOG_NOTE, ...)`: batches / nodes / depth) when set to `1`.

---

## Gotchas

- **N64 build untouched.** The flag is an unused `#define`; every struct field,
  function, and call site is `#ifndef PLATFORM_N64`. `bgRenderRoomPass`'s LEAF
  case keeps the original `gSPDisplayList(block->gdl)` on N64.
- **No signature changes to shared functions.** The per-pass visibility array is
  handed to `bgEmitLeafCulled` through a **file-static** (`g_BgCullVisible` +
  `g_BgCullRoom`), not a new parameter on `bgRenderRoomPass` — that function is
  shared with the N64 path. The static is armed/cleared around each top-level
  `bgRenderRoomPass` call (`bgCullBeginPass` / `bgCullEndPass`); the recursion
  inside stays within one room.
- **XLU ordering is preserved.** Because the block-tree walk (with its back-to-
  front PARENT sort) is unchanged and we only filter *within* each leaf, both the
  opaque and translucent passes cull correctly.
- **Identity when nothing culls.** A leaf with all batches visible takes the
  zero-copy fast path (`gSPDisplayList(block->gdl)`), byte-identical to the
  non-octree build. `/octree off` must look identical to an unmodified build.
- **The cull bbox is the subtree UNION, not the geometric octant.** Don't "fix"
  the node bbox to the split box — batches that overhang their octant would then
  pop at the frustum edge.
- **Centre-split degenerates gracefully.** If every batch lands in one octant the
  node becomes a leaf (no infinite recursion); the `BG_OCTREE_MAX_DEPTH` cap and a
  `2n+32` node-pool cap also bound the build. Pool/alloc exhaustion anywhere
  degrades to coarser culling or no octree for that room — never a crash.

---

## Per-level flagging: `aiSetRoomOctree` (setup data)

A level flags its own octree rooms declaratively with the action-block command
**`aiSetRoomOctree(roomnum)`** in its setup file / ailist:

```c
aiSetRoomOctree(0x0060),
```

It's sugar (`commands.h`) over `configure_environment(room, AIENVCMD_ROOM_SETOCTREE, TRUE)`
— AI command `0x01d6` with the new subcommand byte `AIENVCMD_ROOM_SETOCTREE` (`0x10`,
appended after `STOPUFOHUM 0x0f`). Handled port-only in `aiConfigureEnvironment`
(`chraicommands.c`), it just sets `ROOMFLAG_EX_OCTREE` on the room. The octree is
**built lazily** on the room's next render (`bgCullBeginPass`), so the "setup runs at
stage init, octree builds at room load" ordering resolves itself — no rebuild hook
needed, and it's fine for the command to run before the room's geometry streams in.

A *dedicated* subcommand (rather than piggybacking on `AIENVCMD_ROOM_SETOUTDOORS`) keeps
it explicit — not every outdoor room wants an octree, and some indoor open rooms might.
N64 is byte-identical: the `0x10` value and the switch case are unused/absent there.
Runtime `/octree mark` / `markall` / `bigroom` remain for ad-hoc testing.

---

## Files touched

| File | Change |
|---|---|
| `src/include/constants.h` | `ROOMFLAG_EX_OCTREE 0x0002` |
| `src/include/types.h` | fwd-decl `struct bgoctree`; `room.octree` tail field |
| `src/include/game/bg.h` | `struct bgoctreestats`, `g_BgOctree*` externs, build/free protos |
| `src/include/game/gfxmemory.h` | declare existing `u32 gfxGetFreeVtx(void)` |
| `src/game/bg.c` | octree structs/globals, build/free, frustum test, mark-visible, `bgEmitLeafCulled`, LEAF-case swap, OPA/XLU pass wrap, stats reset, manual test flag |
| `port/src/net/net.c` | `/octree` command + `/help` line; `#include "game/bg.h"` |
