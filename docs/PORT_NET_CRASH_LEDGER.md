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

> **#11 was real progress:** `propsheal=0` (no cycle that run), `proptick_guard` fired
> **363×** (corpses reaped), ran ~1083 log lines before dying. The corpse handling is
> now consolidated at the single pre-everything point (lvTick-top heal): a corpse can
> no longer be seen by ANY tick / collision / render walk this frame, and it's
> `propDeregisterRooms`'d so room-list consumers (collision) can't reach it either.

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
2. **Reduce client load** so slots stop saturating: `Net.Server.Relevancy=1`,
   consider an SVC send-rate cap; investigate the `-44` drift / sub-60fps directly.
3. **Decide guard philosophy**: the heal+reap now make Families A/B *survivable*
   (recover + log, don't die). If runtime confirms "logs fire but no crash/hang", that
   is a viable shipping state while the generators are hunted one by one.

> Keep appending here on every new crash: site, fault, root, fix, status. The table is
> the map; the pattern section is the territory.
