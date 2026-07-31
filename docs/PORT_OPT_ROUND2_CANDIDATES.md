# Optimization Candidates, Round 2 — GPU path + vintage game code (2026-07-31 audit)

**What this is:** the consolidated result of five deep audits run after round 1
(`PORT_CPU_OPTIMIZATION_CANDIDATES.md`, implemented same-day): the GL backend, the SDL_GPU
backend, game-side GPU load, the AI/perception layer, and the object/effects/menu layer of the
original 1999 code. File:line refs verified by reading, no profiling. Tags: **[CACHE]** =
bit-identical, ships alone; **[COORD]** = changes sim/RNG behaviour, server+clients deploy
together through the soak harness.

**STATUS (2026-07-31, GPU wave implemented same-day — compile-verified, runtime PENDING):**
Part A landed: A1, A2 (replay value-mirror in dlcacheReplay), A3 (VAO always + generation-counter
uniform caching), A6, A7a (dead fb0 clear deleted) + A7's depth-only-clear subset on SDL_GPU,
A8 (transfer-buffer pool + batched copy pass; texture ring deferred), A10 (+ a stale-depth guard
that closes a pre-existing validation hole), A11, A13, A14 (+ `/octree autobatch N`), A15-lite
(+ an xray sun-scissor fix the trace uncovered), A16 (minimal-safe variant: skip only when the
fill would be black), A17 (both backends), A20's rtU location cache + RT-forces-offscreen-fb,
A21's blend/poly-offset/select-texture shadows + glFlush removal. The beam texSelect item was a
**misread** (mutually exclusive branches) — struck. **Still deferred:** A4 (mapped ring), A5
(dlcache indexing/packing), A9 (submit split), A12 (font atlas), A18 (shader/pipeline caches),
A19 (texture formats), A7 full pending-clear rework, A20's glGet state shadow, and ALL of
Part B (vintage game code — next wave).
Runtime checklist: pause-menu blur on SDL_GPU (**stale first frame CONFIRMED by user, then
FIXED same-day**: a disarmed capture now serves from fb0 directly), `/rt` toggle both backends,
4P splitscreen, HD-pack texture churn, three-point filter visuals, portal-less/custom stages
with glass (draw order), MP explosion spam, `/octree stats` + `autobatch` on big indoor halls,
cloaked chrs in XLU, black-sky indoor stages + wireframe/xray, dynamically-lit rooms with
`/dlcache on` (palette subimage + replay mirror).

**STATUS UPDATE 2 (2026-07-31, Part B implemented same-day — compile-verified, runtime
PENDING):** B1 (per-chr LOS memo, 64-entry pointer-hashed, side-effect replay, gated on
`g_ChrLosMemoEnabled` = `/losmemo on|off` — all netplay peers must match), B2 (per-room
geoflag union in collision.c, guards cdCollectGeoForCyl + ForCylMove — the ladder-probe
killer), B3, B4 (mainOverrideVariable = static-inline no-op in lib/main.h — all 147 sites),
B5 (aiDetectEnemy family + 9 chrGetDistanceToChr sites, special-chrnum fallback preserved),
B6 (chraiGoToLabel lazy label index w/ stack-buffer + Lua-exec safety), B8-safe-half
(menuIsItemDisabled row-height reuse), B9 (debugdoor deleted + idle-door-ring gate preserving
func0f08d460), B10 (radar hoist, wallhitsTick active-list, explosion free-part per-call hint,
pak debug gate + 15-frame hotplug), B11 (suspicious-item break, shares the losmemo toggle),
B12c (text per-glyph hoists + wipe-sqrtf memo), B13 (three binary-search OOB fixes + the
chraiRunLoop 100k-iteration hang cap). **Debunked during implementation:** the darkroom
`ailistFindById` item (already brightness-gated — stale premise) and the scenario-walk hoists
(already optimal). **Skipped with reason:** explosion light-list cache (Perfect-Darkness-off
can re-HEAL lights mid-level — a compacted cache would diverge), B7 (bot.c is dirty with
unrelated work), B8 full memo. Part B runtime checklist: guard-heavy SP stage (Air Base) with
`/losmemo` A/B, ladder stages (the geoflag union), doors-heavy stages (Villa), menus/lobby
feel, MP wallhit-heavy firefights, and a netplay determinism soak (B1 same-setting on all
peers).

---

## Part A — GPU path

### Tier A1: biggest wins

- **A1. SDL_GPU: front-buffer snapshot blit every frame** (`gfx_sdlgpu.cpp:1639-1674`): a
  full-screen read+write+render-pass per frame (8 MB@1080p → ~132 MB@4K HDR) that exists only to
  emulate GL's front-buffer read for the pause-menu blur — consumed a handful of times per
  session (`:1934-1937`; callers `pdsched.c:444`, `menugfx.c:134`, both event-gated). Make it
  demand-driven (a `front_wanted` flag armed by copy_framebuffer, kept alive a few frames).
  **High / low-med risk / small effort.**
- **A2. dlcache replay: zero state-change detection** (`gfx_pc.cpp:3497-3581`, both backends):
  every state-group boundary re-issues depth mode, both textures, 8-10 sampler params, shader,
  cull, fog, shade routing unconditionally — ~20-30 GL calls × N groups × leaves × viewports.
  Keep an applied-state mirror in `dlcacheReplay` and skip unchanged fields; state-group ids are
  a monotonic counter (`:752`, `:2509`) so identical states never coalesce today. **High / low.**
- **A3. GL: no VAO in the shipping compat profile + 11 uniforms per shader bind**
  (`gfx_opengl.cpp:137-254`; VAO only in core/ES per `:1470-1476` while `gfx_sdl.cpp:190-197`
  requests 3.0 compat): ~35 GL calls per program switch. VAO-per-program + split uniforms into
  frame-constants vs per-bind (or one shared UBO). **High / low-med.**
- **A4. GL: variable-size `glBufferData` orphaning per flush** (`gfx_opengl.cpp:1089-1091`):
  worst-case streaming; replace with a persistent-mapped ring (`GL_ARB_buffer_storage` +
  fences) — the SDL_GPU backend's vtx ring (`gfx_sdlgpu.cpp:139`, `:1598`) is the working
  reference; writing tris directly into the mapping also deletes a full memcpy. **High / med.**
- **A5. dlcache geometry: non-indexed 60-byte vertices** (recorder `gfx_pc.cpp:2541-2553`,
  draw `gfx_opengl.cpp:1225-1247`, both backends): ~3× redundant verts (no index buffer, no
  post-transform cache) × oversized attributes (colour as 4 floats, constant fog rgb per
  vertex, full-float UVs) ≈ **6× vertex bandwidth**. Index buffer first (mechanical; wireframe
  cheat's `gl_VertexID%3` needs the legacy fallback), attribute packing second. **High / med.**
  **DONE (both halves, same-day):** indexing = `/dlcache indexed` (per-run bit-equality dedup,
  u32 indices, nullable `cache_*_index_*` rapi entries); packing = `/dlcache packed`
  (fog/grayscale/inputs as normalized u8x4 — lossless, all u8-sourced; per-entry layout latch,
  `cache_set_packed` rapi entry, SDL_GPU pipeline-key bit 15; UVs/pos stay float). Typical
  vertex 60 → 40 B before dedup. Compile-verified, runtime PENDING.
- **A6. Room palette texture reallocated per frame** (`gfx_pc.cpp:3798-3805` →
  `gfx_opengl.cpp:1200-1212`): `glTexImage2D` respecify + 4 param sets per dynamically-lit room
  per frame → allocate once, `glTexSubImage2D` per refresh. Verify the SDL_GPU twin
  (`gfx_sdlgpu.cpp:2296`) while there. **Med-high / very low.**
- **A7. Clear economics** (both backends): (a) the fb0 clear at `gfx_pc.cpp:4405` is fully
  overwritten by the present blit every frame — delete; (b) SDL_GPU runs every clear as a
  standalone empty render pass (`gfx_sdlgpu.cpp:1863-1900`) — fold into the next pass's
  load-ops via pending-clear flags (`color_virgin` machinery already half-exists); a depth-only
  clear still round-trips the colour attachment (`:1877-1882`), once per player per frame;
  (c) colour and depth clears are split (`gfx_pc.cpp:4362` vs `:4093`) forfeiting combined fast
  clear; (d) the per-viewport depth clear lost its N64 scissor (`zbuf.c:131-159`) — 4P
  splitscreen does 4 full-res depth clears where quarter-screen ones would do. **Med-high /
  low-med.**
- **A8. SDL_GPU: per-upload texture + transfer-buffer creation + private copy pass**
  (`gfx_sdlgpu.cpp:869-953`, also `:2296`, `:2114`): every upload allocates a fresh GPU texture
  AND transfer buffer and opens its own copy pass; steady-state LRU churn = tens of
  `vkCreateImage`/frame during texture pressure. Pool transfer buffers by size class, batch one
  copy pass per flush point, ring 2-3 textures per TexEntry. **High for hitches / low-med.**
- **A9. SDL_GPU: everything submits after a blocking swapchain wait** (`:1583-1584`,
  `:1676-1722`): the GPU idles through the whole CPU frame, and the CPU blocks on the swapchain
  before submitting the frame's real work. Split the present into its own small CB; submit
  render work as soon as the last game pass ends; consider `SDL_SetGPUAllowedFramesInFlight(3)`
  for VRR. **Med-high / med.**
- **A10. SDL_GPU: depth buffers always created SAMPLER-usage** (`:1486-1491`, `:1818-1830`)
  even with RT off — costs depth compression (HTILE/Z-compression) on every depth-tested draw
  for everyone. Gate on `gfx_rt_enabled` + recreate framebuffers when it flips. **Med / low-med.**

### Tier A2: game-side GPU load

- **A11. Portal-less stages render rooms in arbitrary index order** (`bg.c:7228-7240` stamps
  draworder 0 for all) — both an overdraw problem AND a cross-room XLU ordering defect. The
  bigroom branch directly above (`:7208-7225`) already contains the distance-bucket fix with a
  comment explaining why; apply it to the portal-less branch. Port-only. **Med-high / low.**
  Related: portal stages take draworder = MAX portal depth over paths (`bg.c:343-344`), so near
  rooms can draw late; a separate opaque near-to-far key (or min instead of max) is the deeper
  fix but perturbs XLU ordering — A/B on Villa/Chicago glass. **[COORD]-adjacent visually.**
- **A12. Font atlas** (`game_1531a0.c:2167-2172` + `:2180-2239`): still 1 draw + 1-2 texture
  imports **per glyph** (the round-1 rebind-skip can't help — every glyph is a different
  texture; the 2-cycle font combiner makes both tiles live so it imports twice). Menu pages are
  1000+ glyphs → 1000+ draws. Build one atlas per `struct font` at load, emit texrects with
  atlas UVs. Gotchas: ext_tex per-glyph replacements (`ext_tex.c:51-52` — bake or fall back),
  JPN dynamic glyphs can't pre-atlas, 1px gutters. **High on menus/HUD / med.**
- **A13. Explosion render: 15-bucket texture loop regardless of occupancy**
  (`explosions.c:1406-1430`): 30 `gDPLoadBlock`s + up to 15 forced flushes per explosion per
  frame; pre-count parts per bucket in one O(40) pass and skip empty buckets (~15 lines,
  behaviour-identical). Optional fill lever: skip sub-pixel parts. **Med-high in MP / ~0.**
- **A14. Octree only auto-engages on ROOMFLAG_OUTDOORS** (`bg.c:4203-4208`): big indoor halls
  submit everything. Add a size heuristic (numvtxbatches > 64 or AABB diagonal) or tag rooms
  via the existing `aiSetRoomOctree`. **Med-high on specific rooms / low.**
- **A15. Scissor alternation splits batches** — trailing `bgScissorToViewport` emitted even
  when a props pass drew nothing (`prop.c` propsRender tails), full↔room-box twice per draw
  slot (`bg.c` opaque loop), plus per-chr/smoke/explosion scissors (`chr.c:3937`,
  `smoke.c:649`, `explosions.c:1356`). Skip-when-empty + hoist + last-emitted-box memo ≈
  50-300 saved draw splits/frame. **Med / low-med.**
- **A16. Indoor sky: full-viewport opaque fill over an already-cleared framebuffer**
  (`sky.c:284-334`, most environments have clouds off) — plumb the sky colour into the frame
  clear for the single-viewport case and skip the quad. **Small-med / low-med.**
- **A17. Three-point filter mode generates mipmaps that are provably never sampled** — BOTH
  backends (`gfx_opengl.cpp:972-977` min_filters all-NEAREST; `gfx_sdlgpu.cpp:886-901` vs
  `:969-983`, where it also forces COLOR_TARGET usage and a blit chain per upload). One-line
  fix each. **Large in that mode, else nil / very low.**
- **A18. Shader/pipeline warm-up**: GL has no program-binary disk cache (compile hitches
  mid-gameplay; `gfx_sdlgpu_shader.cpp:51-241` is the pattern to mirror via
  `GL_ARB_get_program_binary`; also defer `GL_COMPILE_STATUS` checks —
  `ARB_parallel_shader_compile`). SDL_GPU still pays lazy `SDL_CreateGPUGraphicsPipeline`
  mid-frame per new state combo (D3D12 PSO = multi-ms) — pre-create common combos post-shader
  load; log pipeline count in `/gpu`. **Smoothness, not FPS / low.**
- **A19. Texture upload formats**: everything expands to RGBA8888 on CPU
  (`gfx_pc.cpp:1163-1531`) — I4/I8→`GL_R8`+swizzle, IA8→`GL_RG8`, RGBA16→`GL_RGB5_A1` direct
  upload (deletes the conversion loop) = 2-8× less upload bandwidth + VRAM; also makes the
  4096-entry cache viable with HD packs (less eviction → fewer dlcache invalidation storms).
  Chaos flat-texture path assumes RGBA32 — keep old path there. **Med / med.**
- **A20. RT suite (when enabled)**: ~24 `glGet*` state queries per resolve per player
  (`gfx_rt.cpp:120-146`) → pass a state shadow from gfx_opengl instead; scene-colour copy runs
  even in AO/shadow-only configs (`gfx_sdlgpu.cpp:2846-2856`) → gate on consumers; LOADOP_LOAD
  on 7 full-res targets that don't need history → DONT_CARE when full-rect
  (`:2734-2763`); `rtU()` does `glGetUniformLocation` by string at draw time
  (`gfx_rt.cpp:492-513`) → cache at link; fbo==0 capture path double `glCopyTexSubImage2D`
  (`:604-618`) → force `game_renders_to_framebuffer` when RT on. **Med when RT on / low.**

### Tier A3: trivia + verified non-findings

- Blend-func/polygon-offset/select-texture redundant-set shadows (`gfx_opengl.cpp:1076-1087`,
  `:1018-1058`, `:965-970`); dead `set_mvp` uploads bracketing replay (`gfx_pc.cpp:3431`,
  `:3625` — make setters record-only); `glFlush` before SwapBuffers (`:1510-1512`); menu-blur
  capture reads GL_FRONT = hard stall (`video.c:940-944` use_back=false → pass true); GL
  sampler objects (kills the texture-object sampler churn AND the stale-wrap bug class
  documented at `gfx_pc.cpp:3519-3536`); `GL_RGB8` fb → try `GL_RGBA8` (`:1517-1550`);
  MSAA STOREOP_RESOLVE instead of RESOLVE_AND_STORE when nothing reads the MS surface
  (`gfx_sdlgpu.cpp:1985-2039`); beam triple-`texSelect` (`gunfx.c:581-593`); cloak/shield
  per-bbox-node texture load hoist (`chr.c:6931-6947`); 96 MB SDL_GPU vtx rings sizing + the
  stale 24 MB doc note (`PORT_SDLGPU.md:266`).
- **Verified free / no action:** fog start/stop toggling (no flush, no shader change —
  `gfx_pc.cpp:2597/3000`); weather is CPU-bound not GPU (1-2 draw calls); closed doors already
  occlude (`bg.c:6900-6902`); VRR/vsync logic correct (`video.c:481-516`); per-scanline HUD
  effects coalesce fine (`bondview.c`); dlcache replay uniform pushes on SDL_GPU already lean.

---

## Part B — vintage game code (1999)

### Tier B1: the big ones

- **B1. AI LOS raycasts completely uncached** — the LOS family
  (`chraction.c:7036-7251`) issues a full portal-flood + geo-walk + prop-sweep raycast per AI
  command per chr per tick, frequently with IDENTICAL args several times in one tick (the three
  `chrTryAttack*` at `:7554/7583/7606`; `aiIf*` commands `chraicommands.c:1348/1364/...`).
  Fix: per-chr per-frame LOS memo keyed {frame, target, variant}. **Highest AI win. [COORD]**
  (cdTestAToB leaves global hit state; success writes `lastvisibletarget60` which the memo
  preserves but timing must be proven) — soak + toggle.
- **B2. Per-room geoflag-union early-out [CACHE]** — precompute OR of all `geo->flags` per room
  at load (the `g_PortalMetrics` pattern); `cdCollectGeoForCyl*` and the LOS tests skip rooms
  that can't contain matching geometry. Kills the 2×-per-player-per-tick ladder probes
  (`bondwalk.c:1081-1096` → `collision.c:2142-2168`) on ladder-less stages and cheapens ALL
  geoflag-filtered walks. Cheap down-payment on round-1's deferred #22 spatial index, stacks
  with it. **Med-high / very low.**
- **B3. `func0f068fc8` computes both brightness averages, discards one [CACHE]**
  (`propobj.c:1548-1603`; the door branch at `:1565` already gates correctly): runs for every
  visible prop+chr per player per frame via `propCalculateShadeColour`. ~4-line fix, halves the
  hottest per-prop lighting call. **Med-high / zero.**
- **B4. `mainOverrideVariable` = 147 real calls to an external empty function [CACHE]** — not
  elided without LTO. Per-prop-per-player pair in `propCalculateShadeColour`
  (`propobj.c:1619-1622`, plus dead `scol/salp` branch at `:1749`), per-door, per-frame weather/
  wallhit/healthbar (21 calls!)/lasersight/pak sites. Make it a `static inline` no-op macro on
  the port — fixes all sites at once. Also hoist `cheatIsActive(CHEAT_PERFECTDARKNESS)`
  (`propobj.c:1653`) into a frame flag. **Med / zero.**
- **B5. `aiDetectEnemy` family [CACHE parts]** (`chraicommands.c:6342-6511`): per candidate per
  tick — 3 redundant binary searches (fix: use the chr pointer already in hand; `chr->prop -
  g_Vars.props` for the prop index) + an uncached raycast (B1 covers). Plus the same
  `chrGetDistanceToChr` double-lookup at 9 team/squadron loop sites (`chraicommands.c:6211/
  6386/6468/6534/7005/7195/7258/7447/7508`). **Med / near-zero.**
- **B6. `chraiGoToLabel` linear bytecode re-scan per taken branch [CACHE]**
  (`chrai.c:658-674`, ~120 callers): O(list length) per branch per chr per tick; ailists are
  immutable — build a lazy per-list sorted {offset,label} index + CMD_END boundaries (the
  `s_lenlist` memo at `:684-692` is the precedent). **Med / low.**
- **B7. `botFindPickup` walks all active props per sim per tick** (`bot.c:2033-2460`, reached
  from 6 action states + `botCanDoCriticalPickup:2464`): [CACHE] part — weaponnum→slot reverse
  map kills the 6× and 19×6 inner scans; [COORD] part — round-robin the whole check across bots
  (changes RNG stream, big win, soak). **High in Combat Sim.**
- **B8. Menus re-measure every string twice per frame [CACHE]**
  (`menu.c:4673-4675` → `dialogCalculateContentSize:1271-1362` + `menuIsItemDisabled:1436`
  recomputing item sizes just to test height==0, called per item from 5 render sites;
  `textMeasure` = per-glyph kerning lookups): zero-risk first step — reuse the height already in
  `menu->rows[]`; then a {textptr, scale, langid}→size memo. Pairs with A12 (atlas) for the
  menu-frame double win. **High on menu frames / low-med.**

### Tier B2: solid, smaller

- **B9. `doorTick`/`doorsCalcFrac` full sibling-ring machinery for idle closed doors**
  (`propobj.c:8034-8041` — the lastcalc60 gate passes EVERY frame; `:21378-21568`): idle-ring
  early-out, keeping the `lastcalc60` write + `func0f08d460` restore (`:20474-20487`) or doors
  visually regress. Delete the live `debugdoor` block (`:7949-7966`, discarded sqrtf) outright.
  **Med on door stages / low-med (gate), zero (debugdoor).**
- **B10. Whole-list walks that should be scoped [CACHE unless noted]:** HTM/HTB token self-heal
  walks all active props per frame with no break (`hackthatmac.inc:455-467`,
  `holdthebriefcase.inc:396-409`); KoH hill census ditto (`kingofthehill.inc:440-461` —
  room-scoping needs an iteration-order proof); R-Tracker radar walk calls `cheatIsActive` PER
  PROP (`radar.c:473-521` — hoist is one line); `wallhitsTick` scans all 360 slots while
  `g_ActiveWallhits` already exists (`wallhit.c:475-614`; unlink-safe iteration);
  `explosionInflictDamage` rescans every light in every touched room every frame of the
  explosion's life (`explosions.c:710-798` — cache the shrinking candidate list per explosion);
  explosion free-part O(parts²) scan (`explosions.c:1126-1221` — forward-moving hint form is
  [CACHE]); `explosions.c:1225` + `smoke.c:566` are additional per-frame `bgFindEnteredRooms`
  callers (round-1's portal-bbox precompute already landed, so these are now cheap).
- **B11. Predicate ordering [COORD]:** `chrTryAttack*` evaluate the raycast before the
  ~119/120-fail cooldown (`chraction.c:7554-7607`) — reorder skips ~99% of those raycasts but
  stops `lastvisibletarget60` updates that `chraTickBg:19564` reads; take B1's memo instead or
  deploy coordinated. `aiIfSeesSuspiciousItem` keeps raycasting after pass=true
  (`chraicommands.c:1582-1615`) — add break (behaviour-adjacent).
- **B12. Trivia [CACHE]:** `pakExecuteDebugOperations` runs per frame outside the paksTick gate
  (`lv.c:3286` → `pak.c:4465-4539`: 7 override calls + pak sweeps; rate-limit hotplug to ~15
  frames); OBJTYPE if-else ladders → switch (`propobj.c:11376/11721/11780` — verify -O2 doesn't
  already table them); per-glyph `viGetWidth/Height` calls + `1024/var8007fad0` divides + up to
  2 sqrtf/char during menu wipes (`game_1531a0.c:2139-2239`, `:2118-2137`); `chraTick` darkroom
  `ailistFindById` per chr per tick ungated (`chraction.c:19351-19356`); `chrsCheckForNoise`
  hoist `noiseradius` math out of the slot loop (`chr.c:5541-5584`); `chrsTriggerProxies` merge
  into the future fused chr sweep (`propobj.c:19378`).
- **B13. Correctness notes (not perf):** one-element OOB read in `chrFindByLiteralId`
  (`chr.c:5589`), `chrFindById` (`chraction.c:20414`), `ailistFindById` (`ailist.c:20/39`) —
  upper bound should be count-1; will trip ASan, can flip results on garbage reads.
  `chraiRunLoop` has no iteration cap (`chrai.c:1035-1049`) — a non-yielding ailist cycle hangs
  the sim; a high cap (10k) + log is cheap insurance [COORD-technically].

### Cleared (don't re-investigate)

Tag/obj/chr/ailist lookups are already O(1)/binary-search (`objectives.c:134-143`,
`ailist.c:11`, `chr.c:5586`, `chraction.c:20399`); `objGetTagNum`/`setupGetCmdByIndex` walks
are cold-path only; mpstats is event-driven; hudmsg/sight/boltbeams/splats ticks are small;
particle sims (weather/sparks/shards/smoke/casings) are cheap on the CPU side — their render
halves (Part A) dominate; AI dispatch is already a function-pointer table (`chrai.c:18-506`).

---

## Suggested implementation waves

**Wave R2-a (bit-identical trivia, one session):** B3, B4, B5, B12's hoists, B10's radar/
wallhit/explosion-hint items, A13, A7a (dead fb0 clear), A17, A6, A20's rtU cache, A3-lite
(VAO always + uniform split), B8's menuIsItemDisabled reuse, A15's skip-when-empty scissor.

**Wave R2-b (contained mediums):** A2 (replay state mirror), A1 (front-snapshot gating),
A11 (portal-less draworder), A14 (octree heuristic), B2 (geoflag union), B6 (label index),
B9 (idle doors), A8 (SDL_GPU upload pooling), A10 (depth SAMPLER gate), B7's map fix.

**Wave R2-c (projects):** A12 font atlas, A4 mapped ring, A5 dlcache indexing/packing,
A9 submit split, A18 shader caches, A19 texture formats, B1 LOS memo (+soak), B8 full memo.

**Still parked from round 1:** collision spatial index (#22 — B2 here is its down-payment),
gfx_sp_tri1 derived-state cache (#13 — A2 here is the replay-side sibling), lv.c sim/viewport
split (#24), audio off-thread/SIMD/predecode (#25).
