# CPU Optimization Candidates (2026-07-31 audit)

**What this is:** the consolidated result of a four-way code audit (sim tick / renderer CPU path /
audio+scheduler / infrastructure) hunting for real CPU gains, under the explicit premise that
**N64 buildability may be sacrificed**. It is a ranked work queue with file:line evidence.

**STATUS (2026-07-31, same-day implementation session): nearly everything below is IMPLEMENTED
and compile-verified at -O2 — runtime PENDING across the board.** Landed: #1 (flags: -O2,
inlining, -fno-math-errno), #2, #3, #4, #5, #6, #7, #8, #9, #10, #11 (all except the luaTick
early-out), #12 (GBI_FLOATS — with a scale_bg2gfx fix the upstream float branches lacked), #14,
#15 (MAX_BUFFERED 4096, texture-rebind skip, dlcache-store gating), #16 (one cached ray per
light), #17+#18 (persistent palettes + generation counter replaces the hash), #19a (matrix
hoist), #20 (search-hint), #21 (insertion sort + per-slot prop index), #23 (level-synchronous
BFS with equivalence proof), #25 (catch-up clamp + queue gate, no-op guards, single-DMA
loaders), #26 (both pacing fixes), #27 (all except the propsTickPlayer timeslot fuse and the
HEADSPOT memo), plus the chr.c FORCETOGROUND fulltick gate from #24's family.

**NOT implemented — deliberately deferred, in recommended order for the next session:**
- **#22 collision spatial index** — the biggest remaining win; needs its own session + soak
  (candidate-order preservation is the determinism bar).
- **#13 gfx_sp_tri1 derived-state cache** — needs an exhaustive dirty-flag net + an A/B toggle;
  a missed flag = stale-texture artifacts everywhere.
- **#24 lv.c sim/viewport split** — highest total win for hosts, largest determinism surface;
  do behind the soak harness and the determinism trace.
- **#19b frustum-plane octree test** (only the 8-corner hoist landed), **#25 envmixer SIMD /
  ADPCM predecode / audio thread**, **#27 propsTickPlayer timeslot fuse** (low impact),
  **#11 luaTick early-out** (never fires when chaos is loaded), **#27 HEADSPOT memo** (modelnode
  pointers proven unstable within a stage — gunmem recycling; would be a corruption class).
- **#24's remaining fulltick gates** in the solo ACT_STAND branch (provably no-op in solo).

**Honest limits:** all findings were from static reading, not profiling; the whole implemented
set is compile-verified only. First runtime pass should sanity-check: HUD/menu text (rect
viewport rework), HD-texture stages (texture-rebind skip), splitscreen with one NVG player
(per-viewport palettes), Graffiti/Zones repaints + KoH pulse + chaos room tint (reshade tuple),
MP3 music, reverb stages, dropped-prop pickup order, bot/NPC pathing vs a pre-change build
(BFS steps must be identical), and net play (net.c scan merge cadence + FORCETOGROUND gate are
behaviour-adjacent). scale_bg2gfx stages (any stage where the world scale isn't 1) specifically
exercise the new GBI_FLOATS scale paths.

---

## Tier 0 — the global multiplier

### 1. The entire binary builds at `-Og` with inlining disabled ⚠ FIX FIRST

`CMakeLists.txt:184-187`:

```cmake
# Set Release optimization to -Og until I fix the -O2 issues
if(NOT (CMAKE_BUILD_TYPE STREQUAL "Debug"))
  add_compile_options(-Og)
endif()
```

plus `-fno-inline-functions` at `CMakeLists.txt:198`. Verified in `build/compile_commands.json` /
`flags.make`: the effective command line is `-O2 ... -Og -g -fno-inline-functions ...` — the
directory-level `-Og` lands **after** RelWithDebInfo's `-O2`, and GCC's last `-O` wins. So every
TU in the project — decompiled game code, `port/`, `port/fast3d/*.cpp`, the bundled Lua VM
(`pd_lua`, `CMakeLists.txt:618`) — runs unvectorized, un-inlined, with locals spilled. Origin:
commit `e2b708d44` "port: set default opt level to -Og for now (see #529)".

- **No `-march`, no `-fno-math-errno`, no LTO anywhere.** Every `sqrtf` goes through the
  errno-checking libm wrapper instead of a single `sqrtss` (hot sites: `src/lib/mtx.c:224-453`,
  the per-camera look-at builders).
- **Fix (staged):**
  1. Delete lines 184-187; keep `-fno-strict-aliasing` + `-fwrapv` (load-bearing for decomp UB);
     drop `-fno-inline-functions`. Add `-fno-math-errno`.
  2. If #529-class breakage reappears, pin `-Og` per-file with `set_source_files_properties`
     instead of globally. Likely suspects: register-transliteration TUs
     (`src/lib/modelasm_c.c`, `src/lib/mtx_c.c`, `src/game/mtxf2lbulkasm_c.c`).
  3. Fallback if the decomp fights back: build only `port/` + `port/fast3d/` at `-O2` (new code,
     no matching constraint) and leave `src/` at `-Og` while bisecting.
- **Impact: by far the largest item — plausibly 30-60% of CPU frame time.** Risk: medium-high
  (known `-O2` issues), but a *known* bug to fix.
- **Determinism note:** do NOT add `-ffast-math`/`-funsafe-math-optimizations`. Without them, on
  the SSE2 baseline (no FMA), FP results are IEEE-identical across `-Og`/`-O2`, so old and new
  builds should stay sim-compatible — but treat mixed-build netplay as untested until soaked.

---

## Tier 1 — cheap, results provably identical (do alongside #1)

### 2. `modelasm_c.c` pseudo-registers have external linkage — blocks register allocation

`src/lib/modelasm_c.c:17-51`: ~30 file-scope globals (`f0`-`f23`, `s0`-`s4`, `t3`, `t4`, `gp`, …)
used as MIPS pseudo-registers by `modelasm00018680` (:78), the per-frame skeletal-anim matrix
builder (`src/lib/model.c:1625/1629`, every animated model every frame). External linkage forces
every access to be a memory load/store. **Fix: add `static` to all of them** (grep confirms
nothing outside the TU references them). Pure qualifier change, no renames. Pays off only with
#1 (needs `-O2` + inlining). Impact: high; risk: low.

### 3. `roomGetProps` de-dupes with an O(n²) scan — the inner loop of every collision query

`src/game/prop.c:4083-4131`: each candidate propnum is membership-tested by walking the already-
collected list. Up to 256 props → ~32k s16 compares **per collision call**, and a single chr
movement tick issues 6-12 such calls (`chr0f01f378` at `src/game/chr.c:529` → `cdFindLadder`,
`chrCalculatePushPos`, `cdFindGroundInfoAtCyl`, …; callers across `src/lib/collision.c:999/1298/
1622/3374/3435/3849/4117`, `bot.c:906`, `chraction.c:8143/20913`, `explosions.c:801`).
**Fix: per-prop visit stamp** (`u32 stamp[MAX_PROPS]` + epoch counter, bump epoch per call) —
O(1) membership, output array bit-identical, fully deterministic. ~10 lines. Impact: high;
risk: very low. Also single-handedly defuses the worst caller, `botCheckPickups`
(`src/game/bot.c:891-938`, a 20-room `roomGetProps` per bot per frame).

### 4. Portal bboxes recomputed from vertices on every flood-fill

`src/game/bg.c:7568-7597` (`bgCalculatePortalBbox`): min/max sweep over immutable level data,
redone per portal per `bgFindEnteredRooms` call — and `chrCalculatePushPos` calls that up to 4×
per chr per movement tick (`chr.c:274/340/396/450`), plus `bondmove.c:3057`, `explosions.c:1225`,
`smoke.c:566`. **Fix: precompute `g_PortalBboxes[]` at level load** (the `g_PortalMetrics`
precedent, see `bgTestPosInRoomCheap` at `bg.c:5936`). Impact: medium; risk: near zero.

### 5. Anim header re-walked from part 0 for every joint — O(joints²) on the C path

`src/lib/anim.c:461-651` (`animGetRotTranslateScale`) and friends re-scan the header from part 0
per call; callers in `src/lib/model.c:1128-1165` hit it per POSITION node, ×2 for interpolation,
×2 more when merging. The asm-transliteration path (`modelasm00018680`) avoids this, but the C
fallback is forced whenever chaos `g_ChaosTPose` / `yscale` / `groundmult` are active
(`model.c:1614-1620`). Root-motion catch-up (`model.c:2345-2400`) also decodes every intervening
frame. **Fix: per-part `{bitoffset, ptr}` table computed when `animLoadHeader` installs a slot**
(headers are immutable). Bit-identical memoization. Bonus: replace the 32-entry linear
`animLoadFrame` cache + the `animForgetFrameBirths` full clear per model (`model.c:1674`) with a
small map. Impact: medium (high with chaos scaling active); risk: very low.

### 6. `propsSort` is an O(n²) selection sort, up to 1024 props, per player pass

`src/game/prop.c:81-171`; `MAX_ONSCREEN_PROPS` is 1024 on the port (`src/include/constants.h:75`;
200 on N64); called per player from `lv.c:1775`, plus `pdmain.c:913` (headless) and
`spectator.c:843`. Worst case ~520k compares/player/frame — the cliff that makes 32-sim /
`/octree bigroom` scenes fall over. **Fix: stable insertion sort** (near-sorted frame-to-frame →
~O(n) in practice). MUST be stable descending on depth: `onscreenprops` order feeds
`shotCalculateHits`, so an unstable qsort could change which prop a bullet hits → desync.
Impact: medium (high in bigroom); risk: low if stable.

### 7. `sndTick` copies every playing sound state into a stack array that is never read

`src/lib/snd.c:1976-2004`: `states[i] = *state;` into a ~13-21 KB stack array; grep confirms
`states[]` is never read. Runs once per frame (`lv.c:3307`), one struct copy per active sound
(up to 192). Also no-op `osGetThreadPri`/`osSetThreadPri` ceremony at :1993-1994/:2021.
**Fix: delete.** Impact: small but free; risk: ~0.

### 8. MP3s are decoded TWICE — the N64 software decoder runs, then minimp3 overwrites its output

`src/lib/mp3.c:253-267`: `mp3main0004453c(...)` performs a full MPEG-1 Layer-III decode
(`src/lib/mp3/decoder.c`, 3045 lines) into the stream buffer, then `acmd07` → `aPlayMP3Impl`
(`port/src/mixer.c:694-724`) decodes the same frame with minimp3 into the **same buffer**.
~40 redundant full L3 frame decodes/sec while any MP3 plays. Also: a discarded 0x400-byte
`mp3Dma()` at `mp3.c:277`. **Fix: on non-N64, take frame length / channel count from minimp3's
`mp3dec_frame_info_t` and skip `mp3main0004453c` + drop `src/lib/mp3/*.c` from the port build.**
Preserve the side effects: `stream->unk3ba0` rotor advance + `numchannels` → stereo flag.
Impact: large during MP3 playback; risk: low-medium.

### 9. Audio "DMA" copies resident RAM through a 256-entry cache list, per voice, per subframe

`src/lib/audiodma.c:65-144` (`admaExec`): walks a sorted list of up to 256 windows, memcpys
0x400 bytes of **already-addressable** sfxtbl (resident via `romdata.c:192`), then
`aLoadBufferImpl` copies it again. Up to 96 voices × 2 subframes/frame. **Fix: `#ifndef
PLATFORM_N64`, `admaExec` returns the direct pointer** (`return offset;` effectively); retire the
256×0x400 pool + `admaBeginFrame`/`admaReceiveAll`. Impact: medium-large, very cheap; risk: low
(`mp3.c:412` uses the returned pointer as a bcopy source — still valid).

### 10. Anim frame loads memcpy out of the resident segment through a thrashing 32-slot cache

`src/lib/anim.c:147-155` (`animDma` → bcopy), 289-341 (`animLoadFrame`), 352-393
(`animLoadHeader`), driven per model per frame from `model.c:1658-1674` —
`animForgetFrameBirths` marks all 32 slots evictable after **each model**, so with many chrs the
cache thrashes and most hits become copies. The mod-replacement branch at `anim.c:319-327`
already just points at the data. **Fix: make the stock path do the same — return
`&_animationsSegmentRomStart[segoffset]` directly.** Audit that no caller writes through the
returned pointer first. Impact: medium (scales with chr count); risk: low-medium.

### 11. Assorted dead work per frame (each trivial)

- `modelmgrPrintCounts()` (`src/game/modelmgr.c:37-105`, from lvTick): five full array scans whose
  only consumers are debug HUD + `osSyncPrintf` (compiled to nothing). Gate on `#ifdef DEBUG`.
- `memaPrint()` (`port/src/pdmain.c:1274` → `src/lib/mema.c:292`): the print body is `#ifdef
  DEBUG` but `memaDefragPass` sits OUTSIDE the ifdef and runs every frame. NOT pure waste (the
  merge affects fragmentation; `memaAlloc` relies on defrag fallback) — rate-limit to ~every 30
  frames rather than delete.
- `luaTick()` (`src/game/luaai_api.c:4240-4288`, every frame from `pdsched.c:311`): full event
  dispatch + player-info poll even with zero scripts loaded. Early-out when no handlers + no AP.
- `netDiagLogf` (93 call sites, ~15/frame in `pdmain.c`): non-inlinable call + arg evaluation even
  when disabled. Macro-guard on `g_NetDiagFile`.
- `sysLogPrintf` (`port/src/system.c:186-217`): fopen+fclose+fflush **per line**. Fine until a
  throttle-less per-frame warning fires (`memp.c:182/187/220`, `prop.c:2085` have no throttle) —
  then it's a hard stall. Keep a persistent handle + copy the 1s throttle pattern from
  `prop.c:104-113`.
- `viHandleRetrace` (`src/lib/vi.c:267-307`): ~40 lines of register math feeding four empty
  stubs; only `videoSetWindowOffset` matters. Reduce on non-N64.

---

## Tier 2 — renderer CPU path (port/fast3d + bg.c)

### 12. Define `GBI_FLOATS` — kill the float→s15.16→float round trip per matrix per frame

The support exists and is dead: `gfx_sp_matrix` unpacks fixed-point back to float per `G_MTX`
(`port/fast3d/gfx_pc.cpp:1576-1589`, the `#else` branch is a memcpy); `mtxF2LBulk`
(`src/game/mtxf2lbulkasm_c.c:8-50`) packs per rendered chr (`chr.c:3718/4143`), viewmodel
(`bondgun.c:12598/12610/12718`), props (`propobj.c:14034`), gunfx, rooms, sky, weather.
Thousands of 16-element pack+unpack ops per frame. `Mtxf` is already a `f32/u32` union
(`types.h:43-47`), same 64-byte size; audit confirmed the only writers of `.l[]` on matrices are
the converters themselves (other `.l[` hits are `Lightsn`, unrelated). **Fix: define `GBI_FLOATS`
for the port; add the missing `guMtxL2F` branch (`src/lib/ultra/gu/mtxutil.c:49-70`).** Also
removes s15.16 precision jitter. Impact: moderate-high; risk: medium (check `vi.c:553-681`,
`gfxreplace.c`, anything reading `Mtx` as words). This is the item that formally breaks N64
buildability unless `#ifdef`'d.

### 13. `gfx_sp_tri1` re-derives state per TRIANGLE that only changes on state commands

`port/fast3d/gfx_pc.cpp:1975-2497`: per tri — depth-mode assembly (:2047), ~12 othermode
extractions + CC key build (:2090-2143), texture-dimension derivation with **two integer divides
per texture unit** (:2172-2236), two indirect rapi calls (`shader_get_info` :2266,
`get_clip_parameters` :2268); per vertex — tile shift reads + float divides (:2294-2328),
combiner-input switch with `/255.0f` divides for constant inputs (:2355-2419). **Fix: a
dirty-flagged "derived render state" struct** recomputed only in the ~8 state setters
(`gfx_sp_geometry_mode` :2534, `gfx_sp_texture` :2656, `gfx_dp_set_tile` :2704, set_tile_size
:2740, set_combine_mode :2897, set_other_mode :3214/:3235), caching depth mode, cc key, prg, tm,
tex dims + reciprocals, clip params, and hoisted constant combiner colours. Impact: high
(~100-200 cycles × every immediate-mode triangle); risk: medium — the dirty set must be
exhaustive or you get stale-texture artifacts; land behind a toggle.

### 14. Every HUD/menu rect forces viewport swap → flush → draw call; text is one rect per glyph

`gfx_pc.cpp:3039-3054` swaps in a default viewport and restores it around **every** rectangle,
tripping the memcmp+flush in `gfx_sp_tri1:2061-2088`. PD draws text one texrect per character
(`src/game/game_1531a0.c:2166-2233`), so menu/scoreboard screens are hundreds of forced flushes.
**Fix: don't touch `rdp.viewport`; have `gfx_sp_tri1` select a rect viewport via the existing
`is_rect` flag.** (Font atlas would fix the per-glyph texture flush too — bigger project.)
Impact: moderate on HUD-heavy screens; risk: low-medium (keep the mirror/upside-down/roll scissor
special cases attached).

### 15. Related renderer quickies

- `MAX_BUFFERED 256` (`gfx_pc.cpp:60`) caps every batch at 256 tris → raise to 2-4k (:2494 split,
  98 KB → ~1 MB buffer). Trivial; watch Switch BSS.
- Redundant texture re-binds: `gfx_dp_load_block`/`load_tile`/`load_tlut` set
  `textures_changed` unconditionally → flush + re-import even for the already-bound texture
  (:2176-2179). Compute the `TextureCacheKey` (:1448-1455) BEFORE flushing; skip if equal.
- `gfx_sp_vertex` (:1646-1938): dlcache-recorder fields `ox/oy/oz/colour_index` written
  unconditionally (:1913-1918) — gate on recording. Five chaos-effect globals branched per vertex
  — add a batch-level "no effects" fast path so the 4×4 transform can vectorize.
- Software `G_LIGHTING` + `G_TEXTURE_GEN` (incl. `acosf` per vertex, :1783-1852) is the remaining
  structural CPU-vertex cost — GPU-shaped, matches the dlcache Phase-3 roadmap.

### 16. Port-only glare occlusion: up to 4 full world raycasts per visible light per frame

`src/game/artifact.c:447` inside the corner loop (:401) inside the light loop (:274), from
`lvRender` (`lv.c:1781`), single-viewport modes only (`bg.c:7698`) — i.e. exactly SP and net
co-op. Each `artifactTestLos` → `shotTestLos` (`prop.c:1397`) = portal walk + real triangle
intersection per room + prop scan. 20-30 visible lights = 80-120 raycasts/frame. **Possibly the
single largest per-frame cost in SP after the compiler flags — cheap to confirm: force
`videoGetGlareBrightness()` to 0 and compare frame times.** Fixes, cheapest first: one ray
(origin) instead of 4 corners; round-robin corners across frames; temporal cache keyed on camera
pose; or return to a depth-buffer test (the draw-order fix in PORT_GLARE_OCCLUSION.md re-enables
that option). Impact: potentially high; risk: low (cosmetic effect).

### 17. `roomHighlight` rebuilds whole room vertex palettes per player per frame

`src/game/dlights.c:1717-1886`, called from the bg leaf renderers (`bg.c:4325/4471`); the only
guard is a per-`g_BgFrameCount` stamp, and `g_BgFrameCount` bumps **per player viewport**
(`bg.c:2332`) — splitscreen recomputes identical palettes N×, into a fresh per-frame
`gfxAllocateColours` buffer each time. The result is a pure function of ~6 scalars that
`roomsTickLighting` already dirty-tracks (it even sets `ROOMFLAG_NEEDRESHADE` at `dlights.c:1562`
— which nothing reads). **Fix: persistent per-room palette buffer allocated at `bgLoadRoom`,
recomputed only when the input tuple changes.** Then #18 comes free. Impact: ~0.1-0.2 ms/player/
frame typical; risk: medium (buffer lifetime vs in-flight display list — the port runs `gfx_run`
synchronously, so safe, but verify).

### 18. dlcache dirty check FNV-hashes the whole palette byte-by-byte per room per frame

`src/game/bg.c:4147-4176`, from `bgRenderRoomPass:4372` when `/dlcache` on. Exists only to detect
what #17's input-tuple comparison would tell you for free. Drop once #17 lands (or hash the ~6
inputs instead of the output).

### 19. Octree/room visibility projects all 8 bbox corners through non-inlined calls

`bg.c:4000-4039` (`bgBboxOnScreen`) + `bg.c:2520-2580` (`bgRoomIntersectsScreenBox`):
`bg3dPosTo2dPos` per corner calls `camGetWorldToScreenMtxf()` EIGHT times per bbox (hoist — one
line, zero risk), then a full matrix transform + reciprocal each. Recurses over the whole octree
per room per pass per player; the room test runs for every room on portal-less stages
(`bg.c:7111-7122`). **Fix: (a) hoist the matrix call; (b) replace with centre/half-extent vs
6-plane frustum test** (~6 dot products, no divides) — (b) is not bit-identical to the
`numbehind`/`numfar` semantics, tune against `/octree stats`. Impact: 5-10× on octree traversal;
risk: (a) none, (b) medium.

### 20. dlcache leaf→batch lookup is a linear scan per leaf per frame

`bg.c:4178-4195` (`bgFindLeafBatchStart`), duplicated inside `bgEmitLeafCulled` (:4220-4225),
plus two full GBI-stream passes per partially-culled leaf (:4237-4299). The mapping is static
from `bgFindRoomVtxBatches` (:4571-4643). **Fix: cache leaf→batch index (+ command count) at room
load; invalidate in `bgUnloadRoom` (:3410).** Impact: tens of thousands of compares/pass on big
outdoor rooms; risk: low.

### 21. `bgRenderScene` details

- Draw-slot bubble sort (`bg.c:1182-1200`), 255-slot arrays on the port — O(n²) in bigroom mode.
  `draworder` is u8-range → **256-bucket counting sort** (stable, matching bubble semantics).
- `propsRender` rescans the ENTIRE onscreen-prop list once per draw slot per pass
  (`prop.c:764-799`; call sites `bg.c:1326-1398`) — O(slots × props × 3). The bucketing info
  (`roomnumsbyprop`) is already built at `bg.c:1286-1307`. **Fix: per-slot linked lists built in
  one O(props) pass**; reproduce both iteration directions + the `firstroomnum` double-call
  exactly or XLU order changes.

---

## Tier 3 — structural (big wins, bigger surface — plan individually)

### 22. Per-room collision has NO spatial index — every query walks the room's whole geo stream

`src/lib/collision.c:897-963` (`cd00026a04`), :1164-1317, :2872, :3086: variable-stride pointer
walk over every tile in the room, per query, and a chr movement tick issues 6-12 queries over up
to 20 rooms. The octree indexes RENDER batches only; collision was never indexed. **Fix: per-room
XZ uniform grid (or flat sorted AABB array) built at level load; MUST preserve candidate
iteration order** (byte-stream order) — tie-breaks in `cdFindGroundFromList` (:1805), first-hit
early-outs, and the `maxcollisions` truncation all depend on it; order preserved = pure cache,
order changed = desync risk. Impact: high; risk: medium-low done carefully.

### 23. Pathfinding "BFS" is repeated full-graph scans (Bellman-Ford-shaped)

`src/game/padhalllv.c:318-361` (waygroups), :462-515 (waypoints): O(steps × |V|) instead of
O(V+E) — a 20-hop route rescans the whole array 20 times, at both levels. Re-path triggers are
already throttled (one bot/frame `bot.c:3664`; NPC TTL `chraction.c:6147`), but
`waypointFindClosestToPos` does 10 LOS collision tests per call (`padhalllv.c:159-183`) and gets
called 2× per re-path (`chraction.c:20800-21032`). **Fix: real FIFO BFS assigning identical step
values** (reconstruction re-reads `->step`, so same-steps ⇒ same paths; don't perturb discovery
order — `*ChooseNeighbour` at :258/:402 consumes RNG during reconstruction). Cache
`waypointFindClosestToPos` per chr with short TTL. Raising `MAX_CHRWAYPOINTS` (6, re-path every
3 waypoints via `chrGoPosAdvanceWaypoint` `chraction.c:6184-6198`) is a BEHAVIOUR change —
server+client together only. Impact: medium-high on NPC-heavy levels.

### 24. The lv.c player loop runs the full world tick + DL build per player slot, then discards

`src/game/lv.c:1519-1543` + :2318-2327: in netplay the loop runs for EVERY player including
remotes; the display list is rewound for non-local slots but the CPU work (bgTick, lightsTick,
`propsTickPlayer` walking every active prop, propsSort, autoaim, glares, DL generation) already
happened. AI is fulltick-gated, but render prep (per-chr `gfxAllocate` + `modelSetMatricesWithAnim`
at `chr.c:3128/3202`) multiplies by N. **Fix: split `propsTickPlayer` into a once-per-frame sim
half and a per-viewport render half; skip the render half for slots that won't display.** The sim
half mutates shared timeslicing state (`propstates`, `runstateindex`, per-prop `lvupdate240`
splicing at `prop.c:2617-2633`) — moving it N→1 passes CHANGES sim timeslicing ⇒ server+client
deploy together, validate with the determinism trace. The render-half split alone is low-risk and
already most of the win on a listen host. Impact: highest total lever for hosts/many-player
matches; risk: medium-high — do last, behind the soak harness.

Related, smaller behaviour fixes in the same family (each mirrors an existing port fix — see the
gate added at `chr.c:2960-2970`): `chr0f0220ec` calls missing the `fulltick` gate at
`chr.c:2926-2928` and :2944-2952 (chrs move N× per frame in multi-viewport). Behaviour change —
both sides together.

### 25. Audio: bigger structural options

Current shape: the whole N64 audio microcode emulation runs **synchronously on the main thread**
(`pdsched.c:334` → `amgrFrame` → `n_alAudioFrame`; the mixer.h macros execute `a*Impl` inline —
there is no command list; `osCreateThread` is a stub so `amgrMain` is dead code). Options, in
ascending effort:
- **Clamp `schedAudioFrame`'s catch-up loop** (`pdsched.c:262-272`: runs `diffframe60` full
  synth passes after a hitch — 6× the most expensive subsystem in one frame). Clamp to 2 + check
  `audioGetSamplesBuffered() < queueLimit` BEFORE synthesis, not after (`audio.c:687` currently
  throws away completed synth work). Trivial, kills hitch amplification.
- **Skip no-op work:** `aPoleFilterImpl`/`aDisableImpl` are stubs but callers still compute
  `sqrtf`/`atan2f` args per voice per subframe (`n_resample2.c:15-43`, `n_auxbus.c:30-41`,
  `n_reverb.c:334`, `n_mainbus.c:35-48`). Guard out (perf) or implement aPoleFilter (fidelity —
  the reverb damping lowpass is currently silently missing).
- **Single-DMA the 5 snd loaders** (`snd.c:1089-1302`): each DMAs twice + checksum-compares to
  defeat a hardware race that can't occur; plus a 1546-entry linear cache-index scan per miss
  (`snd.c:1454-1461`).
- **SIMD `aEnvMixerImpl`** (`mixer.c:518-603`, the author's own `// TODO: sse/neon` at :534) —
  the only mixer op without a SIMD path, ~150-500k scalar ops/frame.
- **Pre-decode ADPCM to PCM** (lazily, cached): sfxtbl is 5 MB ADPCM → ~18 MB PCM; eliminates
  aLoadADPCM + admaExec + aLoadBuffer + aADPCMdec per voice per subframe.
- **Move synthesis off-thread** — the endgame; naudio's thread-priority protocol marks the sync
  points (`snd.c:1993/2021`), but g_SndCache + admaItem + sndTick need real locking. High risk.

### 26. Frame pacing burns up to 1.5 ms of a core per frame busy-spinning

`port/fast3d/gfx_sdl.cpp:408-432`: sleeps to 1.5 ms before deadline then busy-spins — ~9% of a
core at 60fps, on the game thread. Plus `frametimeCalculate` (`src/game/timing.c:40-53`) has an
**unconditional** 100 µs sysSleep inside the do-body even when the exit condition is already met.
**Fix: shrink the spin window to ~200-300 µs** (a high-res waitable timer is already created,
`system.c:121-129`); move the timing.c sleep so it only fires when the loop will iterate.
Impact: medium (latency + a core); risk: medium — pacing is netplay-visible (`g_NetTick` advances
per render frame), test under net.

### 27. Sweep-the-world housekeeping loops

- `roomsTickLighting` (`dlights.c:1319-1630`): 3-4 full room-array sweeps per frame even when
  nothing is dirty → keep an active-lightop list + dirty queue.
- `chraTickBg` (`chraction.c:19499`): three full chr-slot sweeps per frame (census, engagement,
  corpse fade) → merge into one pass; keep the RNG-consuming corpse-fade block's inputs identical.
- `netEndFrame` (`port/src/net/net.c:3105-3200`): three independent full `maxprops` scans per
  tick → merge into one pass with three predicates.
- `propsTickPlayer` timeslot redistribution (`prop.c:2934-3081`): four full active-list walks →
  one fused walk (categories are disjoint ⇒ behaviour-identical; verify with determinism trace).
- `modelGetNodeRwData` (`model.c:383-426`): parent-chain walk per node per frame → precompute
  "has HEADSPOT ancestor" on the immutable modeldef.
- `mema` (`src/lib/mema.c`): `MAX_SPACES` 124 while the port grew the heap — `memaMakeSlot`'s
  ~15k-op saturation path is MORE likely on the port. Raise MAX_SPACES, cap the outer loop.
- `vtxstoreTick` (`src/game/vtxstore.c:103-130`): O(n²) with memaFree in the inner loop when the
  OBJVTX store drops below 25% free — a cliff, not a steady cost.

---

## Cleared — checked and NOT worth touching

- `sinf`/`cosf`/`sqrtf` already resolve to native libm (the `.s` versions aren't in the build);
  `sins`/`coss` table lookups have only 7 call sites. Only missing piece is `-fno-math-errno` (#1).
- `rngRandom` (`rng_c.c:13-19`) is already optimal and determinism-critical — DO NOT TOUCH.
- `memp` is a bump allocator; `gfxAllocate*`/`gfxSwapBuffers` are bump+reset — no per-frame cost.
- The 240 Hz substep loops (`lvupdate240`/`lvupdate60` consumers) are genuine sim fidelity and
  netplay-deterministic — high risk, no reward. `lvupdate240` is a delta, not an inner loop.
- The sequencer is event-driven, not per-frame (`n_synthesizer.c:125-130`) — no win there.
- `ailistFindById` is a binary search; bot re-path is 1 bot/frame; NPC re-path is TTL-gated;
  `portal00018148` memoizes per traversal; `gfx_lookup_or_create_color_combiner` has a working
  one-entry memo; `bgGetPortalScreenBbox` is frame-cached; rmon/profile/VI stubs are empty (their
  call overhead disappears with #1); scheduler task-queue scaffolding is inert.

---

## Recommended first wave (MVP)

1. **#1 compiler flags** — delete the `-Og` override + `-fno-inline-functions`, add
   `-fno-math-errno`; per-file `-Og` pinning as the fallback for #529-class breakage.
2. **#2 `static` the modelasm pseudo-registers** — 30 lines of qualifiers.
3. **#3 `roomGetProps` visit stamp** — ~10 lines, bit-identical.
4. **#4 portal bbox precompute** + **#7 sndTick dead copy** + the #11 trivia.
5. Build, deploy, **re-profile** (F9 overlay / an external profiler) — then re-rank Tiers 2-3
   against measured numbers before investing in the structural items.

Netplay rule of thumb throughout: an optimization is safe alone if its outputs are bit-identical
(pure caches, order-preserving rewrites). Anything that changes sim behaviour (fulltick gates,
timeslicing, path shapes, MAX_CHRWAYPOINTS) ships to server and clients TOGETHER and goes through
the soak harness first.
