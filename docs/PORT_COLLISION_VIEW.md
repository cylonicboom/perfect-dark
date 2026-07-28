# Collision View (port-only debug visualiser)

**Extended Options > Experiments > Collision View**, or `/collision` in the console.

Draws the geometry the **simulation** uses — not the geometry the renderer uses.
That distinction is the whole point: it's the tool for *"why did that shot
miss"*, *"why am I stuck on nothing"*, and *"why can't I walk up this"*, all of
which are mismatches between what you see and what collision actually holds.

Not a cheat, not chaos-gated, and it writes no game state — purely a read-only
overlay, so it is save-safe and net-safe.

**Status: world geometry + props usable. Character hitboxes are BROKEN and
PARKED (default off).**

Runtime testing 2026-07-28 narrowed the "triangles flickering across the
viewport" corruption to the **chr-hitbox layer specifically** — turning that one
layer off clears it entirely, so the world-geometry and object/door layers are
sound. `g_ColViewHitboxes` therefore defaults to **0**; `/collision hitboxes`
still enables it for debugging. See "Open issue: hitbox layer" below before
picking this up.

No colour classification has been confirmed on screen yet either. Run the
checklist at the bottom before trusting anything it draws.

---

## What it draws

| Layer | Source data | Colours |
|---|---|---|
| **World Geometry** | The per-room collision lists in `g_TileFileData` (`GEOTYPE_TILE_I` for BG, `GEOTYPE_TILE_F` for lifts) — the same bytes `cdCollectGeoForCylFromList` walks | floor **green**, ceiling **blue**, wall **red**, slope/ramp/step **amber**, ladder **cyan**, lethal (`GEOFLAG_DIE`) **magenta** |
| **Objects and Doors** | Per-prop volumes via `propUpdateGeometry` — `GEOTYPE_BLOCK` prisms and `GEOTYPE_CYL` cylinders | block **orange**, cylinder **violet** |
| **Character Hitboxes** | Every `MODELNODETYPE_BBOX` node of a chr's model — the literal boxes `modelTestForHit` tests a shot against, carrying a `HITPART_*` id | head **red**, torso/pelvis **yellow**, arms **cyan**, legs **green**, tail **pink**, other **grey** |

Everything is a **solid translucent polygon** (`G_CC_SHADE` — untextured,
vertex-coloured; the hoverbike speed-bar recipe in `bondview.c`), so volumes
read as volumes rather than as a wire salad.

Chr hit boxes additionally get **black edge outlines** ("Hitbox Outlines",
`/collision outlines`, default on) — a chr carries ~17 overlapping boxes and
without edges the translucent fills merge into one coloured mass.

---

## Hitbox outlines

The 12 edges of each box are drawn as thin solid **bars**, not lines. The
fast3d line path is desktop-GL only (see `PORT_WIREFRAME.md`) and would be a
no-op on the SDL_GPU backend; a bar also reads better because it thickens as
you approach.

Each bar is centred **on** its edge, so it straddles the box surface: the outer
half sits proud of the fill (no coplanar z-fighting) and the inner half is
overdrawn on top of it. XLU render modes compare depth but don't write it, so
draw order is what layers them — the fill is emitted first, then the outlines.

Bar thickness is a fraction of the box's **smallest** extent (12%, floor 0.4).
Deriving it from the largest extent is wrong and looks it: limb boxes are long
and thin, so a thickness scaled off the long axis exceeds the short axis, the
bars meet in the middle, and the box becomes a black blob.

Outlines use their own alpha (`fill alpha + 128`, clamped) so they stay crisp
when the fills are set very faint.

**Cost:** 8 verts for the fill + 12 bars × 8 = **104 verts per hit box**, so
~1.7 KB of vertex pool per box and ~28 KB per chr. On the port the 1-player vtx
pool is 512 KB per buffer (`0x10000 × PD_BIG_POOL_SCALE`), and the overlay runs
late in the frame — after the world is already allocated — so this is
affordable for a normal scene and the budget guard trims a crowd. Turn
**Hitbox Outlines** off if you're inspecting a room full of guards.

---

## Gotchas worth knowing before you read a colour

**PD has no ceiling flag.** `GEOFLAG_*` gives you `FLOOR1`, `FLOOR2`, `WALL`,
`LADDER`, `SLOPE`, `STEP`, `DIE`, `BLOCK_SIGHT`, `BLOCK_SHOOT`, `UNDERWATER` —
but nothing for "ceiling". The engine decides floor-vs-ceiling by *approach
direction* (`SURFACE_FLOOR` / `SURFACE_CEILING` passed to
`cdFindClosestVertical`), not by a property of the tile. So this overlay
classifies a tile as horizontal-or-not from its normal, then splits horizontal
into floor and ceiling by **height relative to the camera**. A flat surface
above your eye is drawn as a ceiling; the same polygon seen from above is drawn
as a floor. That is faithful to how the engine treats it.

**Tile winding is not consistent**, so only the *magnitude* of the normal's Y is
used (`colviewFlatness`). Never reintroduce a signed-normal floor/ceiling test
here — it will flip colours arbitrarily between tiles.

**Two matrix regimes in one render pass**, and mixing them up will put geometry
in the wrong place:

- World geometry is emitted in **world coordinates** under the plain
  world-to-screen matrix, translation intact.
- Chr hit boxes are emitted in **node-local** space under the model's own
  `model->matrices[]` entry, because those are already model-**to-screen**
  (built during render prep against the current camera — see the
  `isdifferentmtx` re-base in chraction.c's hitpart search). No manual transform
  is needed and the boxes land exactly on the animated limbs.

**Do not switch world geometry back to camera-relative coordinates.** This is
the bug behind the first runtime report (2026-07-28: green triangles flickering
*across the viewport* on the Defection helipad). The first version used the
`lasersightRenderBeam` trick (gunfx.c) — camera-relative coords ×4 under a
translation-zeroed matrix — for sub-unit precision. `Vtx.x/y/z` are `s16`, so
that caps the representable radius at ±8191 units *from the camera*, and one big
outdoor collision tile (a helipad, a hangar floor) spans more than that on its
own. Vertices past the cap wrapped, throwing triangles to arbitrary screen
positions.

> **Reading the symptom:** flicker *across the viewport* means vertex
> corruption — geometry is landing where no real surface is. Flicker *within a
> surface's own plane* means z-fighting. They have completely different causes;
> don't treat "flickering" as one bug.

World coords have no such cap, because `geotilei` vertices are **already stored
as s16 world values** in the tile file; the world-space form is both lossless
and jitter-free, which the camera-relative form never was (it re-quantised every
vertex against a moving fractional camera position each frame).

**Collision surfaces are coplanar with the geometry they describe**, so a
depth-tested translucent fill would z-fight the rendered floor into shimmer.
That is about coincident planes, *not* about thin floors — it happens
identically on thick ones. Pre-empted by `COLVIEW_DEPTHBIAS` (0.995): every
vertex is pulled that fraction of the way toward the camera. Scaling a point
along its own view ray doesn't move it on screen at all — it stays on the same
ray through the eye — so this is a pure depth bias with no positional
distortion. Proportional rather than absolute is the right shape, because depth
precision is finest near the eye, so near surfaces need less offset to win.
(Added alongside the overflow fix; the in-plane shimmer it targets had not been
observed yet, since the corruption was masking everything else.)

**Range-cull by AABB, never by one vertex.** A helipad-sized tile can have its
first vertex outside the draw radius while you stand in the middle of it, so a
single-vertex test made large tiles pop in and out as the camera crossed the
boundary. `colviewAabbInRange` does nearest-point-of-box, which is also how the
collision code broad-phases. For `GEOTYPE_TILE_I` the bounds come from the
tile's own min/max fields — which are **byte offsets into the tile**, read as
`*(s16 *)(tile->xmin + (uintptr_t)tile)`, exactly as
`cdCollectGeoForCylFromList` does.

**`model->matrices` is NULL until a model's first render prep** (`modelInit`
NULLs it), and `modelFindNodeMtx` returns `&matrices[index]` — NULL plus an
offset is a *garbage pointer, not NULL*. `colviewDrawChrHitboxes` checks
`model->matrices` before calling it. Do not remove that check; it is the same
trap that caused the MP-start crash in `bheadReset`.

**`prop->chr` / `prop->obj` / `prop->door` alias one union slot**, so
`prop->chr != NULL` is true for *any* prop. The hitbox pass tests
`prop->type` first. Same rule as the netmsg type-confusion gate.

**Geo-list strides are copied verbatim** from `cdCollectGeoForCylFromList`
(`TILE_I` = `numvertices * 6 + 0xe`, `TILE_F` = `(numvertices - 0x40) * 0xc +
0x310`, `BLOCK` = `0x4c`, `CYL` = `0x18`). This walks the same byte stream, so
it must advance identically or it desyncs and reads garbage. An unknown type
byte breaks the walk rather than guessing a stride.

**Pool safety — this was the real cause of the corruption, and it has three
separate traps.** Read all of them before adding any allocation here.

*Nothing bounds-checks.* `gfxAllocateVertices`, `gfxAllocateMatrix` and
`gfxAllocateColours` all just bump `g_GfxMemPos` and return the old value; the
master display list just advances `gdl`. Overrunning either pool writes into the
buffer the GPU is currently displaying. A garbage matrix or garbage vertex data
throws geometry to arbitrary screen positions — which is why the symptom looked
like a coordinate bug and survived a coordinate-space rewrite. This is the same
failure gfxmemory.c documents for `/octree bigroom` ("triangles out of order"),
and the reason `PD_BIG_POOL_SCALE` exists.

*There are TWO pools, and this overlay is command-hungry, not vertex-hungry.*
One hit box is 8 vertices but a whole run of draw commands. Checking only
`gfxGetFreeVtx()` — as the first version did — misses the pool that actually
runs out. `gfxGetFreeGfx(gdl)` counts remaining display-list entries and must be
checked too. `colviewCanEmit(gdl, verts, cmds)` does both; every allocation goes
through it, including the matrices, which the first version allocated unchecked.

*`gfxGetFreeVtx()` returns an unsigned pointer difference.* Once the pool has
already overrun it reports a huge positive number, so an unsigned comparison
passes forever — the guard inverts exactly when it is needed. The signed casts
in `colviewCanEmit` are load-bearing. (It also returns **bytes**, not a vertex
count.)

Command count is kept down by packing triangles four to a command with
`gSPTri4` (`gbiex.h`) rather than one per `gSP1Triangle` — a box drops from 13
commands to 4, a hit box with outlines from ~170 to ~53. Note `gSPTri4` indices
are **4-bit nibbles**, so a vertex batch may not exceed 16; and a triangle whose
three indices are all zero is silently skipped, so index 0 must never be all
three corners of a real triangle.

If the overlay looks incomplete in a huge room, that's the reserve doing its
job — lower **Draw Distance** or turn off **Hitbox Outlines** rather than
shrinking the reserves (`COLVIEW_VTXRESERVE` 32 KB, `COLVIEW_GFXRESERVE` 4096
commands).

---

## Files

| File | Role |
|---|---|
| `src/game/collisionview.c` | Everything: globals, geo-list walk, hitbox walk, the draw. Port-only file living in a **game** TU (the `luaai_api.c` precedent) so it can use gbi macros, `gfxAllocate*`, and the collision/model internals directly |
| `src/include/game/collisionview.h` | `g_ColView*` externs + `colviewRender` |
| `src/game/bondgun.c` | Hook: `colviewRender(gdl)` in `playerRenderHud`, right after `lasersightRenderBeam` |
| `port/src/optionsmenu.c` | The `g_ExtendedColViewMenuDialog` page + its handlers, and the entry on the Experiments page |
| `port/src/net/net.c` | The `/collision` command |

### Why the toggles are `s32` and not `bool`

`optionsmenu.c` and `net.c` are **port** translation units; `collisionview.c` is
a **game** one, and `bool` is a different width on the two sides of that seam —
this is the bug that made VR controller buttons read as permanently held. Every
shared global here is `s32`. Do not "tidy" them to `bool`.

### Why the render hook is where it is

`colviewRender` must run **before** `bgunRender`: at that point the world is
rendered and the depth buffer is still intact, and `bgunRender` clears depth for
the viewmodel. Putting the overlay after it would depth-test the world geometry
against an empty buffer. This is the same seam the glare-occlusion fix and the
`G_RTRESOLVE_EXT` marker sit on — see `PORT_GLARE_OCCLUSION.md` and
`PORT_RAYTRACING.md`.

---

## Console

```
/collision                 toggle master on/off
/collision on | off
/collision geo             toggle the world-geometry layer
/collision props           toggle the object/door layer
/collision hitboxes        toggle the chr-hitbox layer (alias: hit)
/collision outlines        toggle the black hitbox edges (alias: edges)
/collision xray            toggle depth testing off (draw through walls)
/collision alpha N         fill opacity 8..255
/collision range N         draw radius in world units, 200..8000
/collision status          print current state
```

Alias `/col`. Every subcommand prints the full state afterwards.

---

## Open issue: hitbox layer (PARKED 2026-07-28)

**Symptom:** green and black triangles flickering across the viewport (not
within any surface's own plane). **Isolated to the chr-hitbox layer** — the
world-geometry and prop layers render without it. Survived two rounds of fixes,
so do not assume the obvious.

Already ruled out, don't re-walk these:

- **Vertex coordinate overflow.** Fixed by moving world geometry to world
  coordinates; the hitbox layer never used the camera-relative form anyway (it
  emits node-local coords under the model's own matrix), so this was never its
  cause.
- **Triangle index encoding.** `gSP1Triangle` encodes `v*10` and gfx_pc.cpp's
  `G_TRI1` handler divides by 10 — they match. `gSPTri4` uses raw nibbles and
  `gfx_sp_tri4` reads raw nibbles. Both are correct.
- **Colour palette indexing.** `Vtx.colour` is a byte offset; gfx_pc.cpp does
  `v->colour >> 2`. The 15-entry palette is fine, and `gfx_sp_set_vertex_colors`
  just stores a pointer with no size limit.
- **Pool exhaustion (partly).** Both pools are now budgeted with signed
  comparisons via `colviewCanEmit`, including the matrices. This was a real bug
  and had to be fixed, but it did not fully clear the symptom.

Leading hypotheses, in the order worth testing:

1. **The modelview matrix is never restored.** Each hit box emits
   `gSPMatrix(..., G_MTX_LOAD | G_MTX_MODELVIEW)`, replacing the modelview with
   a bone matrix, and nothing puts the world matrix back afterwards. Anything
   rendered later in the frame that assumes the previous matrix would be
   transformed by a bone. Cheapest test: re-emit the world matrix after the
   hitbox pass and see if the corruption stops.
2. **Out-of-range matrix index on attached models.** `modelFindNodeMtx` returns
   `&model->matrices[index]` with no bounds check. A chr's head is a *separate
   modeldef* attached at a headspot; a BBOX node belonging to the attached head
   may carry a matrix index valid for its own modeldef but past the end of the
   body's `matrices[]` array — a garbage transform, which throws that box
   anywhere. The walk already skips headspot *children*, but not sibling or
   headspot nodes themselves. Test by logging `modelFindNodeMtxIndex` against
   the model's matrix count.
3. **Node walk escaping the model.** The `node->parent` unwind assumes the root
   has a NULL parent. If an attached-head tree is spliced in, the walk could
   climb into another model's nodes and read foreign rodata.

Note the effect is *per chr model*, so the natural first diagnostic is to draw
only one hit box (e.g. `HITPART_HEAD`) and see whether corruption scales with
the number of boxes or is specific to certain joints.

## Honest limits

- **Compile-verified only.** No colour, position, or classification below has
  been seen on screen yet.
- **Render seams (polygon edges) are NOT implemented.** That was the fourth ask
  and is the next phase — it needs a walk of the room display lists
  (`g_Rooms[].gfxdata` roomblock chains) to recover triangle edges, which is a
  separate piece of work from the collision walk above. The `bg.c` octree code
  (`bgFindRoomVtxBatches` / `bgPopulateVtxBatchType`) already walks room gdls
  and is the pattern to reuse.
- Room selection is a **camera-to-room-bbox distance test** over all loaded
  rooms, capped at `COLVIEW_MAXROOMS` (48). It is not the renderer's visibility
  set, so on a level with many small rooms in range the cap can clip the far
  ones. Symptom would be collision missing at distance while near geometry is
  fine.
- Props are capped at `COLVIEW_MAXPROPS` (256) per frame.
- The block cap draws the **top** face only; the bottom is almost always buried
  in the floor and doubling the fill just muddies the view.
- Only the first 8 vertices of a collision polygon are drawn. `geotilei` allows
  up to 64; tiles that large are rare, but a truncated one would render as a
  partial fan.
- N64 build is untouched (`#ifndef PLATFORM_N64` throughout, and the hook is
  inside such a block).

---

## Runtime test checklist

1. Start any solo mission. `/collision` — geometry should fill in around you.
2. Look down: the floor you're standing on should be **green**. Look up: the
   ceiling should be **blue**. Walk under a low overhang and confirm the same
   surface doesn't flicker between the two.
3. Walk to a wall — **red**, and it should line up with where you actually stop,
   not with the visible wall surface if those differ.
4. Find a ramp or a staircase: **amber**. Find a ladder: **cyan**.
5. Stand near a crate/table: **orange** prism matching where you bump into it.
6. Look at a guard: coloured boxes on head/torso/arms/legs, tracking the
   animation, each framed in black. Confirm the limb boxes read as outlined
   boxes and not as black blobs (that's the bar-thickness check). Shoot the
   head box and confirm you get a headshot — that's the proof the boxes are
   the real hit volumes and not merely near the model.
6b. `/collision outlines` off and on, and check the difference in a pile of
   overlapping boxes (a guard's arm against his torso).
7. `/collision xray` — everything should draw through walls.
8. `/collision range 8000` in a large open level; watch `/fps` memory. The
   overlay should thin out rather than the world losing geometry.
9. Menu path: Extended > Experiments > Collision View — toggles and both
   sliders should track the console state and vice versa.
10. `/collision off`, then confirm zero visual and performance difference from
    before (the whole thing is one branch when disabled).
