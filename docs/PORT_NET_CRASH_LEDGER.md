# Netplay Client Crash/Hang Ledger (hosted dedicated Combat Sim)

> Running log of every client crash/hang hit while testing the **master client
> against a VPS dedicated server**, so the *pattern* (not just each site) is
> visible and we can converge on the final root. All are CLIENT-side, in a
> hosted Combat Sim match (proto 74). Symbolicate with
> `addr2line -f -C -e build_debug_sdl3/pd.x86_64.exe 0x$(printf %x $((0x140000000+OFFSET)))`.
> See also `docs/PORT_NET_PROP_SYNC_CATALOG.md` and the `propstick-walk-crash` memory.

## Chronological table

| # | Site (fn:file:line) | Fault | Kind | Root | Fix (commit) | Status |
|---|---|---|---|---|---|---|
| 1 | `propsTick` proptick.c | AV `prop->next` NULL (window-drag) | crash | freed prop left in active list | walk NULL-guard (e0bdf3853) | guarded |
| 2 | `propsSort` prop.c | AV corrupt `->next` (0x1) | crash | same, render sort | range-guard (cef719108) | guarded |
| 3 | `objTickPlayer` propobj.c | AV `obj->model` on NULL obj | crash | null-obj corpse in active list | reap (83ede7b9e→aff12448c) | guarded→reaped |
| 4 | `propsRenderBeams` propobj.c | HANG | hang | active-list `->next` **cycle** | cycle cap (65300aeeb) | guarded |
| 5 | `roomsTickLighting` dlights.c | HANG | hang | active-list cycle | **propsHealActiveList** per-frame (1f09d7e0c) | healed |
| 6 | `mempAlloc` memp.c | AV (NULL alloc → deref) | crash | 16MB stage pool exhausted by AIO HD textures | config `Game.MemorySize` 16→256 | **fixed (config)** |
| 7 | `objFreeEmbedmentOrProjectile` propobj.c:2403 | AV read 0xff | crash | client kept `OBJHFLAG_EMBEDDED` (host-only embedment, unserialized) → garbage union ptr, freed in `weaponCreate` recycle | strip flag in `netbufReadHidden` (82dafd81f) | **fixed (source)** |
| 8 | `chrTick`/`func0f0706f8` chr.c:3028 | HANG | hang | chr **child-chain** cycle (separate list) | **chrHealChildList** per-tick (b300a019a) | healed |
| 9 | `explosionTick` explosions.c:1028 | AV read 0x3d4 (NULL `prop->explosion`) | crash | EXPLOSION corpse ticked by `propsTick` (lvTick walk had no reap) | reap in `propsTick` (aa37a077d) | reaped |
| 10 | `projectileFree` propobj.c:1027 | AV read -1 (garbage `obj->projectile`) | crash | corrupt slot force-recycled by `weaponCreate`; PROJECTILE union ptr garbage | pool range-check in `objFreeProjectile` (5900b442f) | guarded |
| 11 | `propIsOfCdType` prop.c:3488 | AV read 0x50 (NULL `obj`, `obj->unkgeo`) | crash | a ticking projectile's COLLISION examines a null-union OBJ/WEAPON **corpse** via a ROOM prop list — the per-tick-walk reaps fire too late (corpse seen before its own tick) | **reap corpses PRE-TICK in `propsHealActiveList`** (bab0b2fcd) | reaped early |

| 12 | `roomsTickLighting` dlights.c:1461 | HANG (self-loop) | hang | **regression** — the #11 pre-tick reap `propFree`d a corpse that was already in the freelist → double-free → `next==self` → infinite walk | heal UNLINKS corpses via trusted `prev` instead of `propFree` (8b2bacb9f) | fixed (regression) |
| 13 | `objFreeEmbedmentOrProjectile` propobj.c:2420 | AV read -1 (garbage `obj->embedment`) | crash | **Family B sibling of #7/#10** — `weaponCreate` force-recycles a full slot (`objFreePermanently` from a `SVC_PROP_SPAWN`); the slot held `OBJHFLAG_EMBEDDED` + a wild `obj->embedment` (0xffff…ffff). The PROJECTILE branch below was pool-range-guarded (#10) but the EMBEDDED branch was not, so `obj->embedment->projectile` derefs -1 | pool-range guard mirroring `objFreeProjectile`: reject `obj->embedment` outside `g_Embedments[..g_MaxEmbedments)`, clear flag + skip | guarded |

| 14 | `menuGetTeamTitlebarColours` menu.c:2542 | AV read ~stack+0xbf4 (`team=0xff`) | crash | **NOT a prop/weapon crash** — the team-titlebar dialog (`MENUROOT_MPSETUP`+TEAMSENABLED, the Host-Online hosting UI rendered over the live match) indexes an 8-entry stack `colours[]` table with `g_PlayerConfigsArray[g_MpPlayerNum].base.team`, which is the `0xff` "no team" sentinel on a Host-Online client → reads ~team*12 off the stack | clamp out-of-range team to row 0 (`#ifndef PLATFORM_N64`, menu.c) | **fixed (source)** |

| 15 | `bg0f1612e4`/`bgTestHitInRoom` bg.c:5471 | AV read `0xbd955c75` (garbage `batch`) | crash | **Family A corruption surfacing in a blood-splat ray-cast.** `propsTickPlayer → splatTickChr → splatsCreate → splat0f149274 → bgTestHitInRoom`: `batch = g_Rooms[roomnum].vtxbatches` is a **truncated/garbage pointer** (high 32 bits zero, `0xbc00xxxx`/`0xbd95xxxx` signature) — client-side prop/heap corruption. `proptick_guard` fired the same tick (WEAPON prop 40 freed mid-walk). **occ=50 this run** (saturation real on long matches; local grew 15→32, held 10→21 = dead sims' lingering hand-weapons) | (1) **B**: dead sims drop weapons in chr-state sync (`chrIsDead → want=-1`, netmsg.c) to cut the local-weapon climb; (2) `numbatches` sanity guard in `bgTestHitInRoom`; (3) census `deadheld`/`proj` breakdown | mitigated (root = open Family A) |

| 16 | `netmsgSvcPropSpawnRead` netmsg.c:3125 | AV **write** at 0x0 | crash | `weaponCreate` returned **NULL** (`*weapon = tmp` writes through it) — all 50 slots are NON-recyclable: census `occ=48 synced=0 local=48 proj=41`. In-flight projectiles + held weapons are excluded from `weaponCreate`'s recycle scan, so a pool full of syncid-0 projectiles → NULL → NULL write | guard the NULL return (free bare prop+model, drop the spawn — reconcile re-sends); root is the projectile/corpse flood | guarded (crash) + root open |

> **#16 ROOT FIX — orphaned weapon-slot reaper (`weaponSlotsReapOrphans`).** The `projdead`
> census (next run: `projdead=14-19` of `proj=26`) confirmed the pool fills with **freed
> projectile props still referenced by their weapon slot** — a free that cleared the prop
> but not `g_WeaponSlots[i].base.prop` (back-pointer). Since both projectile *creation*
> paths are client-gated, these are host-projectiles freed locally without releasing the
> slot. Fix: per-frame, release any weapon slot whose prop's union no longer points back
> (`prop->obj != &slot.base`) — free the (never-freed) model, clear the slot — so
> `weaponCreate` can reuse it. Runs **server + client** (`weaponSlotsReapOrphans`, gated
> `NETMODE != NONE`, called next to `propsHealActiveList` in `lvTick`): the headless server
> hits the same orphan accumulation (silently fails to spawn drops/projectiles when its pool
> saturates, then streams that to everyone). **No proto bump** — drop-in server replacement.
> NEVER touches the freed prop (no propFree/propDelist). The free-without-clear *generator*
> stays open, but this heals it like `propsHealActiveList` heals the active-list cycle.
> **VERIFIED 2026-06-09 (client E57BCD80, idle client, ~10 min 8-sim match):** `projdead=0`
> on every census line (was 14-19), `occ=50` peaks now `synced=45-47`/`proj=0-5` (healthy,
> was `synced=0`/`proj=41`), `proptick_guard=0` (was 832-932), no crash, clean disconnect.
> Reaping the orphan slots eliminated the whole cascade (saturation→NULL→eviction→void→list
> corruption) — the orphans were the upstream propagator. **The verified runs were against
> the NEW VPS server (`pd-server.x86_64` with the server-side reaper already deployed), so
> the result validates the COMBINED client+server fix — "client-side alone suffices" is NOT
> isolated/claimed.** The server-side reaper keeps the host's own pool clean (likely why the
> active-play `occ` peaked at only 34, not 50 — the host streams less churn).
> **ACTIVE-PLAY VERIFIED 2026-06-09 (E57BCD80, ~10 min, 147 respawns):** still `projdead=0`
> every line, `proptick_guard=0`, no crash; `occ` peaked at only **34** (didn't reach the
> cap). The death/respawn path that previously spiked corruption stayed clean. **This run was
> against the NEW VPS server (server-side reaper deployed) — so client + server reaper
> together.** **Hosted Combat-Sim client crash family considered RESOLVED via the reaper
> heal.** The free-without-clear *generator* is still open but fully healed (find it later
> for a true root fix).

> **#16 — the saturation source is PROJECTILES, not dead-sim weapons (revises #15's B).**
> `deadheld=0` every census ⇒ B (dead-sim hand-weapons) was the wrong target. The pool
> hog is `proj=26→41` syncid-0 projectile slots, and `synced→0` (host props evicted by
> force-recycle, projectiles excluded so they remain). **Both projectile *creation*
> paths are already client-gated** (`bgunCreateFiredProjectile` :5071, `bgunCreateThrownProjectile`
> :4787 both `return` on `NETMODE_CLIENT`) — so the client does NOT make these; they are
> **freed host-projectiles turned into syncid-0 corpses** (`propFree` clears syncid but the
> g_WeaponSlots slot still references them) and/or live synced projectiles that never free.
> `netmsgSvcPropFreeRead` is already double-free-guarded (`prop->active`), so the corpse is
> a **free-without-clear** (a `propFree`/heal-unlink that leaves `base.prop` set), the
> client's local `projectileTick` (runs for movement — gating it would freeze tracers, the
> reported symptom) racing the host's free, or genuine non-freeing. Added census `projdead`
> (proj slots with `!prop->active`) to decide corpse-flood vs live-flood. `932`
> `proptick_guard` hits/run = heavy active-list corruption; the void/"rooms stopped loading"
> is the room system starved once sync collapses (`synced=0`). **Root still open**; next
> log's `projdead` directs the fix.

> **#15 — saturation IS real on long runs (revises #14).** The short run behind #14
> showed occ≤21, but a ~59s match hit **occ=50** with **local=32, held=21** — dead sims'
> syncid-0 hand-weapons accumulate (the host's chr-state keeps reporting a weapon for a
> corpse, so the client never frees the local one). That refills the 50-slot pool and
> forces `weaponCreate`'s recycle path, feeding Family-A corruption that truncated a
> `g_Rooms[].vtxbatches` pointer and crashed the splat ray-cast. **B** (force a dead chr
> to hold nothing in the chr-state sync) attacks the accumulation; the `weaponslots`
> census now logs `deadheld`/`proj` to show the local-weapon composition and confirm B's
> effect. The `bgTestHitInRoom` guard is a backstop (can't portably validate a truncated
> pointer — the repo has a 32-bit build — but catches a garbage `numbatches`). The
> underlying Family-A "something frees a listed prop mid-walk" root stays OPEN.

> **#14 — and the measurement that reframes Open #2.** The `master.csv` netdiag from this
> run (proto 74, the C1 instrumentation build) shows the client is **not** overloaded:
> `weaponslots occ` peaks at **21/50** (never near saturation — the force-recycle path
> isn't even reached), `tickdrift … fps=60.0` with a **constant** `drift=-47` (a benign
> one-time clock offset from a stage-load hitch, NOT ongoing per-frame loss — net then
> tracks wall 1:1 at a steady 60fps), and **zero** `propsheal`/`proptick_guard` lines.
> So the ledger's central "client runs behind → weapon-slot saturation → force-recycle"
> theory is **DISPROVEN for this session**: there was no saturation, no fps deficit, no
> prop-list corruption that run. The crashes are specific bugs, not load symptoms —
> #14 is the team-colour OOB (now fixed), #13 was a corrupt slot (Family A, guarded).
> **Open #2's load-reduction work (A1/B) was therefore NOT implemented** — the census
> proved it unnecessary. The "bullet trails freeze then crash" symptom = the MPSETUP
> menu opening over the match (freezing the background view), then #14 firing in its
> dialog render (`disconnect,wasingame=0` = died in the menu, not gameplay).

> **#13:** identical backtrace to the documented Family B chain
> (`netStartFrame → netClientEvReceive → netmsgSvcPropSpawnRead → weaponCreate →
> objFreePermanently → objFree → objFreeEmbedmentOrProjectile`). Log showed the usual
> systemic condition right before death: sustained drift (`tickdrift … drift=-46`),
> sim chrs dying (`hp=-0 act=4/5`), full weapon churn → 50 slots saturate → force-recycle.
> This closes the last un-guarded sub-step of `objFree`'s union handling (model/geo
> frees are still unhardened — see Open #2). Fix is the same shape as 5900b442f.

> **#12 LESSON (important):** a "corpse" (`prop->obj == NULL`) may **already be in the
> freelist** (a free-without-delist put it there while still active-list-referenced).
> Calling `propFree`/`propExecuteTickOperation(TICKOP_FREE)` on it **double-frees** it
> (`prop->next = freeprops`, which is itself → self-loop hang). The heal now **unlinks**
> corpses from the active chain via its own trusted `prev` (and severs `next==self`),
> never re-frees them. Detect already-freed via `next==self` OR `prop->prev != walk_prev`.

> **#11 was real progress:** `propsheal=0` (no cycle that run), `proptick_guard` fired
> **363×** (corpses reaped), ran ~1083 log lines before dying. The corpse handling is
> now consolidated at the single pre-everything point (lvTick-top heal): a corpse can
> no longer be seen by ANY tick / collision / render walk this frame, and it's
> `propDeregisterRooms`'d so room-list consumers (collision) can't reach it either.

## #16 GENERATOR ROOT-CAUSED + fixed at source (2026-06-09)

Found by inspection (not the tripwire — the `projdead` corpses are dropped guns, which
narrowed it). **`propExecuteTickOperation`'s non-regen `TICKOP_FREE` branch (prop.c:1830-1836)
bare-`propFree`s the prop without `objFree`** — so the weaponobj's `base.prop` back-pointer is
never cleared and the model leaks = the orphan-slot generator. It's ORIGINAL decompiled code,
so it only *manifests* in netplay: on a client `objTickPlayer` runs a **dropped gun's fadeout
locally** (not gated), fadeout returns `TICKOP_FREE`, this branch orphans the g_WeaponSlots
slot; under 8-sim dropped-gun churn the orphans (projectile-flagged, excluded from
`weaponCreate`'s recycle scan) saturate the pool → the NULL-`weaponCreate` crash / host-prop
eviction / void. SP/N64 hit the same path but at trivial churn the orphan sits harmless till
stage end. The dedicated server ticks/fades dropped guns too → same generator.

**FIX (final form):** the bare propFree skips ALL of `objFree`'s reference-clearing, not just
the weaponobj backref — also `wallhitsFreeByProp` / `invRemoveProp` / `chrClearReferences` /
`projectilesUnrefOwner` / `shieldhitsRemoveByProp` / embedment + the model. (A first cut that
only cleared the backref+model fixed the slot orphan — verified `orphan_reap=0`, `projdead=0`,
9-min run — but then crashed in **`wallhitFree` (wallhit.c:173), ledger #17**: a wall-hit decal
left pointing at the freed prop, so `wallhitsTick`→`wallhitFree` walked `prop->opawallhits` off
the end → NULL deref at 0x90.) The complete fix **routes the intact-link case through
`objFreePermanently`** (the full teardown) instead of partially replicating it; the already-
detached DELETING case (`prop->obj == NULL`) falls through to the original bare free. Gated
`g_NetMode != NETMODE_NONE`; SP/N64 byte-identical. `weaponSlotsReapOrphans` stays as a backstop
and logs `orphan_reap` if anything else bare-frees a weapon (should stay silent). Client
`EC84E5D8` + `pd-server.x86_64` rebuilt. This is the true root behind the #16/#17 cascade (and
likely fed much of the broader Family-A corruption via recycled-prop type confusion).

| 17 | `wallhitFree` wallhit.c:173 | AV read 0x90 (NULL `iter`) | crash | a wall-hit decal whose `objprop` points at a prop freed via the bare-propFree path (refs not cleared) — `wallhitFree`'s unlink walk runs off `prop->opawallhits` since the wallhit isn't in it | same root as #16; final fix routes the free through `objFreePermanently` (clears wallhits too) | fixed (source) |
| 18 | `chrGetShield` chraction.c:4097 | AV read 0x1cc (NULL `chr`) | crash | **NOT prop corruption** — `lvTick → lvUpdateSoloHandicaps → playerGetShieldFrac` passes `currentplayer->prop->chr`, NULL for a pawn-less net client (Host-Online lobby/CI between matches, spectator/JIP shells). 2026-06-10 20:11 vs VPS instance, client build of that evening | NULL guard in `chrGetShield` (`#ifndef PLATFORM_N64`) — covers all callers | fixed (source) |
| 19 | `modelSetChrRotY` model.c:549 | AV read 0x0 (`model->definition` NULL/zeroed) | crash | **SERVER-side** (headless soak, match start): `setupCreateProps → botmgrAllocateBot → bodyAllocateModel` returned a model with a NULL/zeroed `definition` → `chrAllocate → chrSetLookAngle` deref. Hit 2/3 full-speed runs, NEVER reproduced under gdb (slow) — timing/pressure-dependent, NOT random body choice; suspected file-cache reclaim of the modeldef while `g_HeadsAndBodies[]` still points at it. Memory pools healthy (256MB, zero failed allocs) | guard in `bodyAllocateModel`: NULL definition/rootnode → loud log naming body/head + return NULL (`botmgrAllocateBot` skips the bot) | guarded (root open — next guard log line names the body) |
| 20 | `bheadAdjustAnimation` bondhead.c:293 | AV **write** 0x1d8 (NULL `chr`) | crash | client stress test vs VPS instance (2026-06-10 20:54, drift −43): `playerTick → bmoveTick → bwalkTick → bmoveUpdateHead` ran for the LOCAL player with a torn-down pawn (`prop`/`prop->chr` NULL while `isdead` false — round-transition race under load; "syncid N does not exist" warnings for static-range ids right before = old/new-world message skew). `chr->oldframe` write through NULL. Same pawn-less family as #18, different consumer | NULL-fetch + early return in `bheadAdjustAnimation` (sibling head fns touch only `player->` fields) | guarded (the pawn-less-tick ROOT — why bmove runs at all — still open) |
| 21 | `chrGetRotY` chraction.c:9567 | AV write 0xb10000ea (GARBAGE `chr`) | crash | third family member same hour (21:04, same stress test): `playerTick → bmoveTick → bwalkTick → bmove0f0cc19c` — this time the pawn pointers are **garbage, not NULL** (the player prop freed AND recycled mid-tick), so leaf NULL checks can't converge | **family root choke point**: `bmoveTick` entry validates the pawn once (prop non-NULL + still `PROPTYPE_PLAYER` + chr non-NULL) and skips the movement tick with a throttled log naming the state. Covers #18's bmove cousins, #20, #21 and future members | guarded at choke point (WHY the pawn dies mid-tick = open; the old VPS server's churn is the suspected feeder — retest after deploy) |
| 22 | `modelPromoteNodeOffsetsToPointers` model.c:3862 / `modelPromoteOffsetsToPointers` model.c:3961 | SIGSEGV (wild node walk) | crash | **#19's ROOT, found via a deterministic Linux repro** (containerised soak, 2026-06-10: 2/2 crashes ~50s in, at the FIRST playlist rotation, on both HEAD and the pre-§9 base — pre-existing). `fileLoadToNew` reuses `g_FileInfo[].loadedsize` across loads, but after a load `romdataFilePreprocess` rewrites it to the ACTUAL post-preprocess size — no estimate margin (`romdataFileGetEstimatedSize` 64-bit factor + 0x20), no 0x8000 EXTRAMEM rwdata slack. `bodiesReset` NULLs every `g_HeadsAndBodies[].modeldef` each stage (lv.c:410), so rotation reloads every bot body into the stale TIGHT buffer → rzip end-of-buffer scratch / in-place 64-bit expansion collide → promote walks the corrupted image. Explains #19's "timing/pressure-dependent, never under gdb" (margin-dependent layout). `body0f02ce8c`'s MP-head `loadedsize = 0` reset was the historical dodge for exactly this | `fileLoadToNew` re-derives the size for every EXTRAMEM load (db48c0c50, port-only; N64 reuse was safe — no preprocess). Unfixed crashed 2/2 at the FIRST rotation; fixed survived 3 rotations (31→60→60) then **a second shape fired at the stage-41/PIPES rotation** — same promote walk (model.c:3961), so a remaining corruption source exists beyond the stale-size reload | **partial** — first shape fixed; second shape (PIPES rotation, model.c:3961) under live investigation; #19's guard stays as backstop |
| 23 | `fclose(NULL)` via `fsFileFree` (fs.c) ← `mpsetupOpenFile` | SIGSEGV in glibc `_IO_new_fclose` | crash | boot-time, not match: `mpsetupOpenFile`'s create-if-missing path does `fsFileFree(fsFileOpenWrite(...))` without checking the open; with a bad/unresolvable save dir (seen with a relative `--savedir`, `$S` resolves wrong) fopen returns NULL → `fclose(NULL)` faults inside `netDedicatedBootTick → menuhandlerHostStart → mpsetupLoadCurrentFile` | NULL guard in `fsFileFree` (554ebe1ef) — choke point covers all callers | fixed (source) |

## Root-cause fixes landed (not just guards)
- **aff12448c** — `propsTickPlayer` REAPS null-obj corpses (TICKOP_FREE) instead of skipping.
- **758fe1ec1** — **the big one**: the client chr-state weapon swap now FREES the
  swapped-out sim hand-weapon cleanly (`objFreePermanently`) instead of mark-DELETING
  + orphan; `propReparent` made idempotent (detach-before-attach). Closed the
  **syncid-0 hand-weapon** corruption source.
- **82dafd81f** — strip `OBJHFLAG_EMBEDDED` on the client (host-only state).
- Config — `Game.MemorySize` 16→256MB.

## The pattern (→ the final cause)

Every entry is one of two families, and both trace to **one systemic condition**:

- **Family A — prop-list corruption** (#1-5, #8, #9): cycles and null-union corpses in
  `g_Vars.activeprops` and chr child chains. A prop's `->next` is **dual-use** (active
  list vs. child sibling chain), so a stray re-link / re-activate / free-without-delist
  bridges or loops the chains. Surfaces at *every* walk site.
- **Family B — force-recycle free crashes** (#7, #10): when the 50 weapon slots
  (`g_MaxWeaponSlots`) fill, `weaponCreate` force-frees the oldest *live* slot; if that
  slot's obj union is stale/garbage, `objFree` AVs.

**The single systemic condition feeding both:** the client cannot cleanly keep up with
the dedicated server's high-churn prop/weapon stream. It runs **behind** (sustained
`-44` net-tick drift, sub-60fps under the AIO HD-texture renderer), so:
1. spawned/dropped weapons + sim hand-weapon swaps churn faster than the client frees
   them → the 50 slots fill → the dangerous **force-recycle** path runs (Family B);
2. the same churn + the move-before-spawn race + freed-but-referenced props corrupt the
   lists (Family A).

So the "final cause" is **not one line** — it's the client's local prop/weapon lifecycle
breaking down under sustained churn it can't drain in time. 758fe1ec1 removed the
biggest *generator* (the orphaned hand-weapon). The remaining live generators, by the
heal logs:
- a **synced-weapon (#) + explosion** active-list cycle (heal logged back-edge
  `WEAPON syncid 121 → EXPLOSION` on 2026-06-08 17:14) — likely a weapon that explodes
  (creates an explosion, frees itself) re-linking around the explosion. **Next target.**
- weapon-slot saturation forcing the recycle path — would be relieved by draining the
  client faster (relevancy ON to cut prop-move load; or capping per-tick spawns) and/or
  hardening every `objFree` sub-step (model/geo frees) the way #7/#10 hardened the union.

## Open / next steps (priority)
1. **Synced-weapon + explosion cycle source** — use the next `propsheal` line
   (it names the back-edge prop id/type/syncid) to pin the explode→free→relink path.
2. ~~**Reduce client load** so slots stop saturating~~ — **ANSWERED BY MEASUREMENT (see #14).**
   The C1 `weaponslots` census proved the client does NOT saturate (occ ≤ 21/50) and runs
   at a steady 60fps; the `-47` drift is a benign constant offset, not sub-60fps loss. No
   load-reduction (A1/B) was needed. The crashes are specific corruption/OOB bugs, not a
   load breakdown — keep hardening individual sites (A3) + fixing root bugs as they surface.
3. **Decide guard philosophy**: the heal+reap now make Families A/B *survivable*
   (recover + log, don't die). If runtime confirms "logs fire but no crash/hang", that
   is a viable shipping state while the generators are hunted one by one.

> Keep appending here on every new crash: site, fault, root, fix, status. The table is
> the map; the pattern section is the territory.
