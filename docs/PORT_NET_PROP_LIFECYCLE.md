# Networked-Prop Lifecycle Consolidation (netprop.c) — 2026-06-10

> **Status: implemented, compile-verified (Linux dedicated build), NOT yet
> runtime-tested.** This is Phase 0 + Phase 1 + the quick wins of the prop-sync
> consistency plan — all **no-wire-change** work (no `NET_PROTOCOL_VER` bump;
> old and new builds interoperate). The runtime test checklist is at the bottom
> — run it on the next test session before trusting any of this.
>
> Pairs with: `docs/PORT_NET_PROP_SYNC_CATALOG.md` (the gap analysis this
> implements against), `docs/PORT_NET_CRASH_LEDGER.md` (the corruption families
> the choke point + tripwires guard), `port/src/net/CLAUDE.md` (protocol).

## What this is

The June crash work proved the prop bugs all live in one seam: **prop teardown
had no single shape** (seven different free paths, each clearing a different
subset of references) and **spawn broadcasting was copy-pasted** across six game
code sites with inconsistent gating. This change consolidates both into one
port-side module — `port/src/net/netprop.c` / `port/include/net/netprop.h` —
and adds the audit trail that makes the *next* corruption generator a one-log
diagnosis instead of a gdb session.

## The pieces

### 1. `netPropFreeSynced(prop, reason)` — the teardown choke point

Every **port-added** free of a networked weapon/obj prop now routes through one
function: the `SVC_PROP_FREE` apply, the reconcile ghost reap, the chr-state
held-weapon swap, `propExecuteTickOperation`'s netplay branch (the ledger #16
root fix), the client force-recycle victim, and the new spawn-replace path. It:

- refuses non-weapon/obj props (the union type-confusion class) and props whose
  `obj->prop` backlink is already broken (the double-free → freelist self-loop
  class, ledger #12) — logging loudly instead of corrupting;
- runs the engine's FULL `objFreePermanently` teardown (detach, embedment,
  wallhits, inventory, chr refs, model, rooms, delist, free);
- **post-free tripwire**: scans `g_WeaponSlots` for any slot still
  back-referencing the freed prop. `objFree` clears it, so a hit means a NEW
  free-without-clear generator exists — and the log line carries the reason
  code, naming the caller.

Decompiled-code free paths are untouched (decomp contract); only port-era code
migrated.

### 2. `netSyncPropSpawn(prop)` — the one spawn broadcast

Replaces five hand-rolled `netmsgSvcPropSpawnWrite + netmsgSvcPropMoveWrite`
blocks (bondgun.c ×3, propobj.c ×2) and `netSyncSpawnProjectile` (chraction.c —
renamed, same body). Uniform gate everywhere now: `NETMODE_SERVER` + syncid
assigned + local client `CLSTATE_GAME` (the old bondgun sites had no
syncid/state check at all). Adds:

- **the autogun owner guard** the live broadcast path never had (the spawn
  writer derefs `g_Vars.players[owner]->client->id`, NULL between a mid-match
  disconnect and the next stage — the JIP snapshot has guarded this since
  proto 59; now the live path skips + warns instead of crashing the server);
- **a same-syncid dedupe window (4 frames)** — see "the double-spawn bug"
  below.

### 3. The double-spawn bug (found during consolidation) + "latest spawn wins"

`objDrop` broadcasts a spawn from its own tail (the sim-disarm/death-drop fix),
**and** two of its callers (`bondgun.c` player drop, `weaponCreateForPlayerDrop`)
broadcast again right after calling it. Every player weapon drop whose prop had
`OBJHFLAG_PROJECTILE` at drop time therefore sent **two `SVC_PROP_SPAWN`s for
the same syncid**, and the client allocated **two props**: moves/frees resolve
the first match, so the duplicate was driven by nothing — and the reconcile
could not reap it because its syncid IS in the host's set. A permanent
client-side ghost gun, manufactured on ordinary weapon drops. Two independent
fixes:

- **Write side**: the dedupe window in `netSyncPropSpawn` swallows a same-syncid
  re-broadcast within 4 frames (a legitimate re-spawn after pickup/free is
  seconds away).
- **Read side** (`netmsgSvcPropSpawnRead`): **one prop per syncid** is now an
  enforced invariant — a spawn for a syncid the client already holds frees the
  existing weapon/obj copy through the choke point (`NETPROP_FREE_RESPAWN`) and
  rebuilds from the newer spawn. This is also the groundwork for a future
  server re-send/NAK path (re-spawn over an existing copy = clean replace).

### 4. Client ownership gating — "wire owns the lifetime"

On a client, a **synced** (`syncid != 0`) weapon/obj prop no longer makes
lifecycle decisions; client-local (`syncid == 0`) props are unaffected:

- **`propExplode` early-returns false** (`propobj.c`): no local blast, no local
  damage. The host's `SVC_EXPLOSION` is the visual; its `SVC_PROP_FREE` is the
  removal. This kills the double-explosion + client-side-damage divergence and
  answers the Phase-2a projdiag question by construction (clients cannot
  self-detonate). Conditional mine/timer callers (`if (propExplode(...))`)
  simply don't free locally — the wire free lands ~RTT later; a rocket may
  visibly penetrate a wall for that long (bounded, authority-consistent).
  Unconditional grenade callers still set `OBJHFLAG_DELETING` and reap cleanly
  (timers run identically on both sides), with the wire blast as the visual.
- **Hard-free fade is server-only for synced props** (`weaponTick`): the client
  ran the 20-on-screen fade on its own screen budget, freeing dropped guns the
  host still tracks — and *nothing re-spawns a missing prop* (the reconcile
  only reaps extras), so every client-side hard-free was a permanent hole. It
  was also the fadeout path that generated ledger #16's orphan slots. The host
  hard-frees on its own budget and broadcasts the free.

### 5. Dedicated-server hard-free budget (`#ifdef DEDICATED_SERVER`)

Headless servers have no render pass, so `PROPFLAG_ONTHISSCREENTHISTICK` is
never set and the vanilla "20 hard-freeable guns on screen" despawn **never
fired at all** — dropped guns accumulated unboundedly, which is exactly the
weapon-slot churn/saturation environment behind crash-ledger families A/B. On
dedicated builds every `CANHARDFREE` gun now counts toward the budget (i.e. it
becomes "20 total dropped guns", slightly stricter than a listen host's "20
visible" but bounded, and each hard-free is broadcast normally). Listen hosts
unchanged.

### 6. Combat Sim client pickups (catalog §5.1)

The client-side hard bail in `objTestForPickup` and the co-op-only gate in
`netmsgClcPickupRequestRead` are gone. All clients now run the same read-only
pickup checks; at the pickup point:

- **co-op clients** keep the existing behaviour (take locally + tell the host;
  the host's echo back is skipped);
- **Combat Sim clients** send `CLC_PICKUP_REQUEST` and take **nothing** locally
  — the host validates against their latest reported position and the
  `SVC_PROP_PICKUP` echo is the only give (`netmsgSvcPropPickupRead` already
  applies own pickups in Combat Sim). Costs ~1 RTT of pickup latency.

The host's own proximity scan for remote pawns (`propsTestForPickup`'s
latest-pos swap) remains as the second detection path. Double-grants are
impossible: the first grant frees/`DELETING`-flags the prop; the loser's
request resolves a dead syncid to NULL.

### 7. The syncid diet + reconcile coverage

`propAllocate` no longer assigns syncids (it can't — `prop->type` isn't set
yet, so it burned an id on **every** server allocation, explosions and smoke
included). Assignment moved to `netPropAssignSyncId()`, called from
`propActivate` / `propActivateThisFrame` / `propPause` — every path into the
world — skipping `PROPTYPE_EXPLOSION`/`PROPTYPE_SMOKE` (never wire-referenced;
they were the id burn). Verified orderings: explosions/smoke set their type
before activating; runtime co-op chr spawns activate (chraction.c:15957) before
broadcasting (15987); `bgun0f09ebcc` activates before every projectile
broadcast site.

Consequences:
- the id counter now grows at networked-prop rate (~hundreds/match, not
  thousands/minute);
- `NET_RECONCILE_MAXSYNCID` raised 4096 → 65536 (8 KB bitmap): the reconcile
  backstop previously **stopped covering new props mid-session** once the
  counter passed 4096 — a silent coverage cliff. Now unreachable. The writer
  also skips over-cap ids so a truncated u16 can never alias another entry.
- a tripwire in `netbufWritePropPtr` logs `"wire ref to syncid-0 prop"` if the
  server ever wire-references a prop that slipped past the assignment hooks
  (a diet coverage gap — should never print).

No wire change: dynamic ids were always server-assigned opaque values; only
*which* ids get consumed changed.

### 8. The lifecycle ring + `/proplog`

A 4096-entry global event ring (`netprop.c`, ~48 KB, idle outside net sessions)
records every `propActivate`/`propActivateThisFrame`/`propPause`/`propDelist`/
`propFree`, every wire spawn TX/RX/drop (with reason), every choke-point free
(with reason), every gated explode/hard-free, and every syncid assignment.
Reset per stage (`netSyncIdsAllocate`).

- `/proplog` — newest 40 events, any prop.
- `/proplog <syncid>` — every buffered event for that prop, oldest first.

This is the "what touched this prop, in what order" tool the ledger entries
each took days to reconstruct by hand.

### 9. Phase-0 projectile gate diagnostic (temporary, like the other projdiags)

For the open "client rockets invisible" WIP: `objTickPlayer` now logs, once a
second on clients, the full gate state for a synced projectile:

```
projdiag: gate sid=N fulltick=F anim=A active=V bg=B sliding=S pos=(x,y,z)
```

How to read it against the existing `projdiag: SPAWN ok ... active=` line:

| Observation | Meaning |
|---|---|
| `SPAWN ok` but **no `gate` line ever** | the prop never reaches `objTickPlayer` — check `SPAWN ok`'s `active=` (0 = parked in pausedprops at spawn) and `/proplog <sid>` for a pause/delist |
| `gate ... active=0` | paused — the spawn's wire `active` bit was 0; fix is force-activating projectile spawns on read |
| `gate ... bg=1` | backgrounded — propstate scheduling starves the tick |
| `gate ... fulltick=1 anim=1` | the `model->anim == NULL` gate blocks `projectileTick` (the committed prime suspect — confirmed) |
| `gate ... fulltick=1 anim=0` and still no flight | the blocker is past the gate chain — instrument `projectileTick` itself |

Note: for **sim-owned** projectiles `fulltick` was already true before the WIP
client override (the player-owner gate at propobj.c only applies when
`ownerprop` is a *player* prop), so the override is load-bearing only for
remote-player projectiles. If sim rockets log `fulltick=1` and still freeze,
the answer is one of the other columns.

## Files touched

| File | Change |
|---|---|
| `port/include/net/netprop.h` | **new** — API + event/reason enums |
| `port/src/net/netprop.c` | **new** — ring, choke point, spawn helper, diet |
| `src/game/prop.c` | diet (propAllocate), assign+ring hooks (activate ×2 / pause / delist / free), TICKOP route |
| `src/game/propobj.c` | objDrop + weaponCreateForPlayerDrop spawn sites → helper; hard-free client gate + dedicated budget; propExplode client gate; pickup gate removal + Combat Sim defer; force-recycle route; Phase-0 gate diag |
| `src/game/bondgun.c` | 3 spawn sites → helper |
| `src/game/chraction.c` | `netSyncSpawnProjectile` → `netSyncPropSpawn` |
| `port/src/net/netmsg.c` | spawn-read replace-on-duplicate + ring events; free/reconcile/weapon-swap → choke point; reconcile cap 65536 + writer guard; PropPtr syncid-0 tripwire; pickup-request co-op gate removed |
| `port/include/net/netmsg.h` | `netSyncSpawnProjectile` decl removed |
| `port/src/net/net.c` | `/proplog` command + help; ring reset in `netSyncIdsAllocate` |

N64 build: all game-code changes are `#ifndef PLATFORM_N64`; the hard-free
restructure preprocesses to the original statements.

## Runtime test checklist (next session)

All of this is compile-verified only (Linux dedicated build). Suggested single
session, listen host + 1 client + 6 sims, weapons set to launchers/mines:

1. **Boot + host + join** — no startup regressions; `/proplog` prints events.
2. **Drop a weapon (switch-away drop or death drop)** on the host — client sees
   exactly ONE gun on the floor (double-spawn fix). `/proplog <sid>` on the
   client should show `spawn_rx` once (a second spawn would show
   `netfree x=6` = RESPAWN replace — also fine; two `spawn_rx` with different
   propidx and no netfree = regression).
3. **Combat Sim client pickup**: client walks over a dropped gun → gets it
   (~1 RTT delay is expected). Watch for double-give (ammo counted twice = the
   echo-skip assumption is wrong — report).
4. **Mine/grenade detonation near the client**: ONE explosion visual (was
   sometimes two), no client-side damage mismatch. A rocket may briefly enter
   a wall before vanishing on the client — expected (wire-owned lifetime).
5. **>20 dropped guns on the client's screen** (mass sim deaths): guns no
   longer vanish client-side while the host keeps them (hard-free gate); the
   HOST's fades still remove them everywhere.
6. **Dedicated server soak (~10 min, 8 sims)**: `weaponslots` census should
   stay healthy like the post-reaper runs; additionally `orphan_reap` should
   stay SILENT (the choke point covers the generators) and any
   `netprop: POST-FREE` line names a new generator — grab `/proplog` + pd.log.
7. **Sim rockets (the WIP)**: read the new `projdiag: gate` lines per the
   table above — this decides the next fix without another build round.
8. **Co-op smoke test** (if time): drop-in still works; NPC corpses still reap
   (chr paths in free/reconcile unchanged).

Expected-silent tripwires (any print = finding): `netprop: POST-FREE`,
`netprop: free reason=... skipped`, `NET: wire ref to syncid-0 prop`,
`orphan_reap`, `propsheal`, `proptick_guard`.
