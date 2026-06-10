# The Blind Server Compendium — every workaround needed to run the server headless

Status: living reference, compiled 2026-06-10 from a full-tree walk of
`port-net-predict`. This is the **consolidated map** of everything the engine
assumes a *sighted* host provides, what the tree already works around, and what
is still open. Companion docs: `PORT_DEDICATED_SERVER_DEBIAN.md` (deployment),
`PORT_DEDICATED_SERVER_TRIAGE.md` (the bug-by-bug history that produced most of
these fixes), `PORT_HOSTED_SERVER_FINDINGS.md` (slot-0 family),
`PORT_NET_PROP_SYNC_CATALOG.md` (prop lifecycle), `PORT_NET_SOAK.md` (headless
client).

---

## 0. What "blind" actually means

A dedicated server (`--dedicated`, `g_NetDedicatedMode == 1`, optionally the
`DEDICATED_SERVER` build) is blind along **five independent axes**. Every
workaround in this document exists because of one of them:

| Axis | Concretely |
|---|---|
| **A. No render pass** | `lvRender` never runs. Everything it *owns* — gameplay calls and per-frame state population alike — must be mirrored or tolerated as absent. |
| **B. No render-populated state** | `ROOMFLAG_ONSCREEN`, `PROPFLAG_ON*SCREEN*`, `g_Vars.onscreenprops`, `g_MpRoomVisibility[]`, `player->worldtoscreenmtx`/projection matrices, `cam_pos`, viewport dims, per-frame model matrices: all zero/NULL/stale, forever. |
| **C. No local pawn** | The host is a zero-panel spectator. There is no slot-0 local player; every combatant is `isremote` with a `client` backlink, and `g_Vars.currentplayer` cycles through *remote* slots during the server's tick. |
| **D. No platform subsystems** | No SDL window/GL context, no audio device, no input devices, no config-save-on-exit, no vsync pacing. |
| **E. It cannot simulate remote players' weapons** | The wire only carries inputs (`CLC_MOVE` ucmds) plus a partial gset; the server replays gun *state machines* from inputs but cannot reproduce the client's exact aim/tick, so **combat is client-reported** (`CLC_HIT`, `CLC_PROP_HIT`) and movement is trust-client (`bwalkUpdateRemote`). |

The single most important structural fact discovered in this walk: **the
"on-screen" pipeline is one chain**, and on a blind server it is cut at the
root:

```
render pass sets ROOMFLAG_ONSCREEN
   → func0f08e8ac (propobj.c:20663) requires it, so returns false for everything
      → chrTick needsupdate = false on EVERY branch (chr.c:2654-2830)
      → objTickPlayer pass2 = false (propobj.c:11560-11588)
         → PROPFLAG_ONTHISSCREENTHISTICK never set
            → propsSort (prop.c:110) → g_Vars.onscreenprops is EMPTY
            → chr/obj per-frame model matrices (gfxAllocate, chr.c:~2908) never built
            → the "offscreen" chr child path (func0f0706f8) runs instead of chr0f022214
```

Anything downstream of that chain either needs a bypass (alwaystick, active-list
scans), a replacement authority (client-reported hits), or acceptance as a
documented degradation. §9 proposes the systemic repair: a **two-tier
visibility restoration** that replaces most of the per-site seams.

---

## 1. Axis D — platform layer (DONE, catalog)

All in place; listed so nobody re-derives them.

| Site | Workaround |
|---|---|
| `port/src/main.c:138-167` | `--dedicated` / `--dedicated-windowed` / `--headless-client` parsed before subsystem init; `DEDICATED_SERVER` build forces mode 1; pre-silences audio (`g_SndDisabled`) and installs signal handlers. |
| `port/src/video.c:109` | `videoInit` skipped; `wmAPI` NULL-guards on every `video*` entry. |
| `port/src/audio.c:30` | `audioInit` skipped; `#ifdef DEDICATED_SERVER` stubs the SDL body out entirely. |
| `port/src/input.c:804,1146` | `inputInit` skipped; mouse-position div-by-zero guard for `videoGetWidth()==0`. `port/src/dedicated_stubs.c` satisfies the whole input API in the stripped build. |
| `port/src/headless.c` | `headlessPace(60)` wall-clock pacer (no vsync to pace the loop) + POSIX/Windows console-signal handlers for clean shutdown. Called from `mainTick` tail (`pdmain.c:990-995`). |
| `port/src/main.c:111` | skip `inputSaveBinds`/`configSave` at exit (would wipe a shared pd.ini with empty binds). |
| `port/src/mod.c:432` | skip mod texture preload (pool exhaustion on heavy mods before the server even starts). |
| `CMakeLists.txt` (`DEDICATED_SERVER`) | strips SDL3/GL/glad/fast3d-backends from the build; binary links libc+zlib+ENet only. |
| `port/src/system.c` | POSIX paths (`/proc/self/exe`, `$HOME`) replace `SDL_GetBasePath/PrefPath`; fatal errors to stderr not a dialog box. |
| **Trap — do NOT "optimize"**: `src/game/zbuf.c:51` | the z-buffer allocation looks render-only but `bgBuildTables`' lighting init writes through it unconditionally; skipping it crashed the server at stage load. Leave it allocated. |

Boot flow (no menus, no agent file): `netDedicatedBootTick`
(`port/src/net/netmenu.c:198-240`), hooked from `playerTickPauseMenu`'s
`MENUROOT_FILEMGR` case (`player.c:2535`), waits on `pakIsGamepakReady`
(`pak.c:546`), loads `gamefileLoadDefaults`, and consumes the host/join latch
directly. Fallback blank-agent auto-create in `filemgr.c:2599`. Auto-start +
post-vote lobby fallback gate on `min_humans_to_start` (`net.c:3090,3149`).

---

## 2. Axis A — what lvRender owns, and the mainTick mirror

`lvRender` (lv.c) is not just drawing; its per-player loop owns gameplay-tier
calls. The headless mirror lives in `port/src/pdmain.c` `mainTick`
(~lines 711-995). Current coverage:

| lvRender-owned call | Where (render path) | Mirrored headless? | Notes |
|---|---|---|---|
| `propsTickPlayer` | lv.c:1452 | ✅ pdmain.c:856 | with `g_Vars.alwaystick = 1` (pdmain.c:826) so the foreground gate (prop.c:2431-2435) passes without `ROOMFLAG_ONSCREEN`. |
| `scenarioTickChr` | lv.c:1453 | ✅ pdmain.c:857 | |
| `propsSort` | lv.c:1454 | ✅ pdmain.c:858 | runs, but produces an **empty list** headless (see §4) — kept for its side-effect-free consistency. |
| `propsTestForPickup` | lv.c:1591 | ✅ pdmain.c:867 | pickups for remote pawns. |
| `currentPlayerInteract(false)` | lv.c:1566/1577 | ✅ pdmain.c:873-876 | gated on `JO_ACTION_ACTIVATE` from UCMDs. |
| death state machine (`playerRenderHud`, player.c:5220-5502: `isdead` 1→2, `deathanimfinished`, `redbloodfinished`, `colourfadetimemax60`) | player.c | ✅ pdmain.c:878-910 | terminal state advanced immediately; without it `numdying` never reaches 0 and the match never ends (lv.c:2402). |
| respawn detect+consume (`dostartnewlife` on `UCMD_RESPAWN` → `playerStartNewLife`) | player.c:5268-5328 / lv.c | ✅ pdmain.c:912-945 | |
| `handsTickAttack` | **lv.c:1456 — only caller in the tree** | ❌ **NOT mirrored** | see §6.1 — this is the biggest open gap. |
| `autoaimTick` | lv.c:1455 | ❌ | server has no aim-assist consumer; cosmetic. OK. |
| `lookingatprop` calc (`propFindAimingAt` QUERY) | lv.c:1464-1497 | ❌ | HUD/aim-track only; interact has its own scan (§4). OK. |
| `lvFindThreats` / tracked props | lv.c | ❌ | threat-detector HUD; OK. |
| `bgTick` / `lightsTick` / `artifactsClear` / `skyRender` / `bgCalculateGlaresForVisibleRooms` | lv.c:1449-1451 | ❌ | render-tier (portal *visibility*, flash lighting, glares). Room traversal/doors are collision+door-state, not portals — nothing gameplay-blocking here. `bgTickPortals` also syncs cheat→renderer globals; meaningless headless. OK. |
| `bgunTickGameplay2` | player.c:5125 (playerRenderHud) | ❌ | vision mode (X-ray/FarSight), Mauler charge tick, RCP120 cloak ammo drain, `bgunTickLoad`, eyespy deselect — all per-player cosmetic/local; cloak *state* arrives via `UCMD_CLOAKED` (bondmove.c:661). Minor; see §6.6. |
| co-op buddy health-steal on revive (player.c:5345-5422) | playerRenderHud | ❌ | only matters if co-op ever runs on a dedicated host; the mirror's respawn path skips the health transfer. §6.7. |
| `bgunLoadAll` (gun model load) | lv.c:1424-1429 | ❌ | server never needs gun *models*; no observed harm across soaks. |

What does **not** need mirroring because it already lives in the tick path
(verified, since several earlier notes got this wrong):

- **Gun state machines for remote players** run on the server:
  `lvTickPlayer → playerTick → bmoveTick → bmoveProcessRemoteInput`
  (bondmove.c:139) calls **`bgunTickGameplay(fireguns)`** (bondmove.c:265) from
  the replayed UCMDs. Reload/weapon-switch/fire timers all advance headless.
- **Remote mine detonation** runs on the server: `bmoveTick` handles
  `movedata.detonating → playerActivateRemoteMineDetonator` (bondmove.c:2537).
- **`weatherTick` / `nbombsTick` / sparks / wallhits / casings** run from
  `lvTick` (lv.c:2845-2860), not render. (Their `cam_pos` reads are in their
  `*Render` functions only — checked nbomb.c:816/944, weather.c:209/1098.)
- **Explosions/smoke damage** ticks via `propsTickPlayer` under `alwaystick`.
- **Client-side netplay apply** (`netChrInterpolate` from `chrTick`, prop
  apply in `netStartFrame`, auditor in `netEndFrame`) is all tick-path — this
  is what makes the `--headless-client` soak mode work at all.

**gfx pool invariant:** even with rendering skipped, `gfxSwapBuffers` must run
every frame (pdmain.c:974-981) because tick-path code still calls
`gfxAllocate` (chr render-prep, see §4) and the pool only resets on swap.

---

## 3. Axis C — the pawnless host (DONE, catalog)

The host being a zero-panel spectator breaks two families of assumptions:

**Spectator/ghost family (fixed):**
- `netStartServer` marks the local client `is_spectator` (net.c:1143);
  `spectatorAllocatePanels` forces **0 panels** in dedicated
  (spectator.c:139) — a 1-panel fallback used to spawn a visible ghost pawn.
- `mainLoop` allocates `numplayers = combatants` only (pdmain.c:507-522);
  `netSyncIdsAllocate` skips the prop-existence swap for a spectator local
  client; `netPlayersAllocate` gives spectators no player backlink.
- `netEndFrame`'s server broadcast block must NOT require a local
  player/prop — the gate is split client/server (net.c:2651). This single gate
  was the original "dedicated servers don't sync at all" root cause.
- `playerTickThirdPerson` early-returns at `g_NetDedicatedMode == 1`
  (player.c:6087); spectator-host skips in interpolation (player.c:3329) and
  viewport-rect math (player.c:6653).

**Orphan-slot family (fixed, defensive):** a mid-match disconnect leaves
`g_Vars.players[N]` alive with `client == NULL` until the next stage
transition rebinds. Four layers skip orphans:
1. `mainTick` lvTickPlayer loop (pdmain.c:775-800, `isremote`-guarded so the
   host itself still ticks),
2. `mainTick` headless props loop (pdmain.c:853-855),
3. `chrIsRoomOffScreen` player iteration (chraction.c:5618-5622: NULL, pool
   bounds, `PROPTYPE_PLAYER`, client),
4. `propPickupByPlayer` early-return (propobj.c:17603-17630 — prevents
   silent server-side consumption that never broadcasts).
The proper fix (rebind player↔netclient at JIP reconnect) remains open.

**Slot-0 family (fixed):** with no slot-0 swap on a spectator host, "local
player == slot 0" is false for every dedicated-server client. Seven sites
fixed (contpads, `allowmlook`, Slayer/eyespy mouse, hudmsg, `playerSndStart`,
death sting) — full table in `PORT_HOSTED_SERVER_FINDINGS.md` §2. Rule:
key on `isremote`/`netPlayerOwnsMouse()`, never on `currentplayernum == 0`.

**Spawn correctness (fixed):** the synced-RNG spawn picker is deterministic
*per local slot*, and every client's local pawn sits at its own slot 0 → all
humans collapsed onto the host's pad. `lvReset` now force-corrects every
remote at first spawn (`UCMD_FL_FORCEPOS|FORCEANGLE|FORCEGROUND` + direct
`forcetick` latch, lv.c). Note `playerChooseSpawnLocation`'s
screen/standby-avoidance terms (`bgRoomIsOnPlayerScreen/Standby`,
player.c:292-296) read `g_MpRoomVisibility` = all-zero headless — the
*visibility* avoidance silently degrades to geometric-distance + RNG only.
Acceptable; documented.

**`playerReset` uninitialized `rooms[]`** (stages with no `INTROCMD_SPAWN`,
i.e. the CI lobby a dedicated server reloads after every match) walked garbage
geometry forever → wedged server. Fixed with `rooms[0] = -1` seed.

---

## 4. Axis B — render-populated state: bypasses and degradations

### 4.1 The prop tick foreground gate — bypassed
`propsTickPlayer` scores props foreground/background; the score normally comes
from `ROOMFLAG_ONSCREEN` (prop.c:2440-2446). Headless uses the engine's own
escape hatch: **`g_Vars.alwaystick = 1`** (pdmain.c:826; consumed at
prop.c:2431-2435 — the code comment "But it never is [set]" predates us).
Players and `OBJHFLAG_PROJECTILE` props are foreground regardless
(prop.c:2448-2474).

### 4.2 Interact — replaced with an active-list scan
`propFindForInteract` normally walks `g_Vars.onscreenprops` (empty headless).
For `NETMODE_SERVER && currentplayer->isremote` it scans the **active prop
list geometrically** (prop.c:1927), and `objTestForInteract` /
`doorTestForInteract` skip the `PROPFLAG_ONTHISSCREENTHISTICK` check under the
same condition (`noscreen`, propobj.c:16386-16428 / 21274-21330) — distance,
room and LoS checks still decide. High-ping door edge case is covered by
`CLC_DOOR_ACTIVATE` (client names the exact door syncid; host runs
`propdoorInteract` as that client).

### 4.3 Camera matrices — identity fallback
`camGetWorldToScreenMtxf` returns a **static identity** when the player's
`worldtoscreenmtx` is NULL (camera.c:316-336) — projectile/shot broad-phase
degrades to world-space math instead of crashing. `camGetProjectionMtxF` has
no such guard; all its callers are render-tier or sit behind the on-screen
flags (verified) except `playerTickThirdPerson`, which has the explicit
dedicated early-return.

### 4.4 What is *accepted as degraded* (no fix needed, but know it)
- `g_MpRoomVisibility[]` all-zero → `chrIsRoomOffScreen` says "offscreen" for
  everything → sim AI takes its offscreen paths (waypoint "magic" movement,
  `model->anim->average` frame-skipping); spawn visibility-avoidance inert.
- `player->fovy/aspect/viewwidth/viewheight` for remote slots come from
  client-reported settings (`cl->settings.fovy`, bondmove.c) where they matter
  (zoom FOV replay); the rest of viewport math is render-tier.
- `lookingatprop` stays NULL; sight/reticle system inert. No server consumer.
- Continuous-loop gun sounds and pitch-shaped handles are skipped for remote
  players by design (`bgunPlayGunSound`, src/game/CLAUDE.md).

### 4.5 The chr "offscreen forever" consequence — partially OPEN
Because `func0f08e8ac` (propobj.c:20663) requires `ROOMFLAG_ONSCREEN`, every
chr takes `chrTick`'s offscreen exit (chr.c:~3070): no per-frame model-matrix
build (`gfxAllocate` block at chr.c:2853-2908 skipped), no
`ONTHISSCREENTHISTICK`, children handled by `func0f0706f8`. Three knock-on
effects, two of them still live:

1. **`g_Vars.onscreenprops` is empty** → the server's own shot trace
   (`propFindForPlayer`, prop.c:1012-1045) iterates nothing. Damage still
   works because chr/prop damage is client-reported (§5), but **server-side
   hit validation is structurally blind** — see §6.2.
2. **Screen-gated frees** → embedded/stuck mines freed via the on-screen
   branch (`chr0f022214`) are missed on a host that always takes
   `func0f0706f8` → **ghost mines** on clients. *Mitigated* by the 2 Hz
   `SVC_PROP_RECONCILE` backstop; the targeted fix (un-gate the free when
   `g_NetDedicatedMode`) is still open. (`PORT_NET_PROP_SYNC_CATALOG.md` §4/§6.)
3. Model matrices for chrs are per-frame pool allocations; on a blind server
   they are **never rebuilt**, so any code that reads `chr->model->matrices`
   outside the on-screen gates reads a stale pool pointer. Audit rule: never
   touch chr matrices server-side without checking the needsupdate gate.

All three are addressed at the root by §9 Tier 1.

---

## 5. Axis E — client-authoritative combat (structural, by design)

The blind server **cannot** referee what it cannot simulate. Current authority
split, all already implemented:

| Event | Authority | Wire |
|---|---|---|
| Hitscan chr damage | client detects, server applies+broadcasts | `CLC_HIT` (server skips its own `chrHit` for remote shooters — prop.c:1083-1093) |
| Destructible prop / glass | client detects | `CLC_PROP_HIT` (queued, drained in `netEndFrame`) |
| Melee | **each machine handles only its own local player's melee** — `propFindAimingAt` early-returns for remote currentplayers (prop.c:1571-1581) because the trace is hardcoded to the local view | `CLC_HIT` carries the result |
| Movement | trust-client; server adopts reported pos (`bwalkUpdateRemote`), force-corrects via `UCMD_FL_FORCE*` when *it* wants authority (respawn, first spawn) | `CLC_MOVE` / `SVC_PLAYER_MOVE` |
| Doors | client-predicted, host-confirmed | `CLC_DOOR_ACTIVATE` |
| Pickups (Combat Sim floor weapons) | host-validated grant | `CLC_PICKUP_REQUEST` → `SVC_PROP_PICKUP` |
| Timer-detonated networked props | **server-only** `propExplode`; clients get the visual via `SVC_EXPLOSION` (netmsg.c:4862) | |

Consequences a blind server accepts (document, don't "fix" piecemeal): no
server-side verification of fire rate, ammo, damage values, speed, or
wall-clipping. `Net.Server.HitValidate` (net.c:232, modes 0/1/2) exists as the
enforcement seam — but see §6.2 before enabling it on a dedicated host.

Lag compensation itself is headless-safe: the rewind ring stores pos+tick only
and patches `prop->pos` + root matrix translation (prop.c:1015-1019), needing
no render data.

---

## 6. REMAINING GAPS — the workarounds still required

Ordered by impact. Items 1-3 are the substantive engineering work; the rest
are small.

### 6.1 `handsTickAttack` mirror — IMPLEMENTED (compile-verified, runtime-unproven)

> Status 2026-06-10: mirrored in the pdmain.c headless loop after `propsSort`
> (lvRender's order), gated on the §9 Tier 2 camera prime — the shot trace
> projects through the player's matrices and `objHit` derefs
> `camGetProjectionMtxF` with no NULL guard, so it must never run on an
> unprimed slot. Soak checklist: remote grenade/rocket/mine visible to all
> clients, Phoenix explosive-shell explosions, uplink (HTM), no
> double-detonation (DETONATE is reachable from both `bmoveTick` and this
> dispatcher — same as a listen host), spawn-pos sanity on thrown projectiles.
>
> **Quirk inherited from listen hosts (pre-existing, now shared):** a remote
> shooter's prop hit applies damage TWICE server-side — the server's own
> trace (`objHit → objTakeGunfire → objDamage`) plus the shooter's
> `CLC_PROP_HIT` report (`netmsgClcPropHitRead` enqueues unconditionally, no
> dedupe). Invisible for one-hit glass; mild inflation on HP destructibles.
> Fix direction: mirror the chrHit pattern — skip the local `objDamage` for
> remote shooters on the server and let the client report be the single
> application (plus record for validation). Deliberately NOT bundled with the
> mirror commit so the mirror is pure listen-host parity.

Original analysis (kept for the record):
`handsTickAttack` (prop.c:1833) → `handTickAttack` (prop.c:1736) is the
**attack dispatcher**: `HANDATTACKTYPE_SHOOT` (shotCreate), `_MELEE`,
`_DETONATE`, `_UPLINK`, `_BOOST`, `_SHOOTPROJECTILE`
(`bgunCreateFiredProjectile` — rockets), `_THROWPROJECTILE`
(`bgunCreateThrownProjectile` — grenades/mines/nbombs). Its **only caller is
lvRender (lv.c:1456)**. On a listen host it runs per player (including
remotes) because the host renders everyone; on a blind server it runs for
**nobody**.

Per-type fallout on a dedicated server (static analysis; runtime-confirm next
soak with active input):
- `SHOOT`/`MELEE`: covered — damage is client-reported (§5); the lost
  server-side `shotCreate` only cost the validation trace (§6.2).
- `DETONATE`: covered — also reachable via `bmoveTick` (bondmove.c:2537).
- `THROWPROJECTILE`/`SHOOTPROJECTILE`: **NOT covered.** Dynamic prop spawns
  are host-owned (`SVC_PROP_SPAWN`); if the host never runs the spawn, a
  client's grenade/rocket/mine exists only on the throwing client — invisible
  and harmless to everyone else. This is consistent with the
  projectile-sync WIP noted in `PORT_NET_KNOWN_ISSUES.md` (58fed026e).
- `UPLINK`/`BOOST`: not covered; uplink matters for HTM/co-op objectives.

**Workaround direction:** call `handsTickAttack()` in the pdmain.c headless
loop after `currentPlayerInteract` for each bound combatant. It is
input-driven (reads `bgunIsFiring`/hand state advanced by the tick-path
`bgunTickGameplay`), and the damage paths are already remote-safe: `chrHit` is
skipped for remote shooters (prop.c:1083), melee early-returns
(prop.c:1579), and `gsetPopulateFromCurrentPlayer` reads the synced gset.
Risks to check at runtime: `shotCreate`'s broad-phase walks the empty
onscreenprops list (harmless — finds nothing), and projectile spawn position
derives from hand matrices that may be identity/stale headless —
`bgunCreateThrownProjectile2` takes pos/rooms/velocity computed from the gun
matrix, so validate spawn pos sanity (fall back to `prop->pos` + facing if
garbage). Alternatively (heavier, cleaner long-term): client-spawn +
`CLC_PROP_SPAWN_REQUEST` with host validation, which the projectile-sync WIP
is already heading toward.

### 6.2 Server-side hit validation is blind (onscreenprops empty + no chr matrices)
`netServerRecordDetectedHit` (net.c:3780) is only fed by the server's own
`shotCreate` trace, which (a) never runs headless (§6.1) and (b) would find
no chr targets anyway because the trace walks `onscreenprops` and chrs never
enter it (§4.5). Net effect: on a dedicated host, `Net.Server.HitValidate=1`
logs every hit as undetected and `=2` would **reject all legitimate hits**.
Today's default (0 = off) is the only mode that works blind.

**Workaround direction:** §9 fixes both halves — Tier 1 restores
`onscreenprops` + chr matrices; Tier 2 supplies the *per-shooter* candidate
set ("what the shooting client sees"), which is the semantically correct
input for validating that shooter's `CLC_HIT`. Until then, document
HitValidate as listen-host-only.

### 6.3 Screen-gated frees → ghost mines (mitigated, not fixed)
The embedded/stuck-prop FREE path picks `chr0f022214` (on-screen) vs
`func0f0706f8` (off-screen); a blind host always takes the off-screen branch
and can miss the free → ghosts on clients. The reconcile backstop papers over
it at 2 Hz. **Fix:** §9 Tier 1 routes the blind host down the on-screen branch
unconditionally (a free is gameplay GC, not rendering — it must never be
gated on whether anyone is looking). See `PORT_NET_PROP_SYNC_CATALOG.md` §4.

### 6.4 Dropped-item physics owner-gates (known issue, prop-sync WIP)
`objTickPlayer`'s projectile fulltick gates (propobj.c ~11250-11286) assume an
owner pawn iteration that a dedicated host + disconnects can starve → dropped
crates/guns freeze mid-air on some machines. Tracked in
`PORT_NET_KNOWN_ISSUES.md`; root the gates when projectile-sync resumes.

### 6.5 `explosionCreate` BULLETHOLE LOD reads `currentplayer->cam_pos` (explosions.c:253-262)
On the server `currentplayer` is a remote slot whose `cam_pos` is never
populated → the within-4m flame-vs-smoke choice is computed against garbage.
Cosmetic-asymmetric (affects which *visual* the server thinks it spawned, and
`smokeCreateSimple` consumes an RNG draw on one path — a determinism foot-gun
if anything downstream compares). **Workaround:** on
`NETMODE_SERVER && currentplayer->isremote`, use the shooter pawn's
`prop->pos` instead of `cam_pos`, or skip the LOD fork headless.

### 6.6 `bgunTickGameplay2` not run for remote pawns (player.c:5125)
Per-pawn vision mode, Mauler charge tick, RCP120 cloak **ammo drain**, eyespy
deselect. Each client runs this for itself, cloak *state* syncs via
`UCMD_CLOAKED`, and ammo is client-authoritative — so today this costs
nothing the architecture didn't already concede. Revisit only if server-side
ammo accounting ever becomes a goal (it would belong in the §6.1 mirror).

### 6.7 Co-op revive health-steal lives in `playerRenderHud` (player.c:5345-5422)
The headless respawn mirror calls `playerStartNewLife` but not the
buddy-health-transfer block. Irrelevant for Combat Sim playlists; becomes a
required mirror item the day co-op runs on a dedicated host
(`PORT_COOP_*.md` track).

### 6.8 The headless-*client* local-pawn render-prep seam (unproven)
`--headless-client` puts a local **combatant** pawn on a headless build — a
combination the dedicated server never exercises (its host is pawnless). The
soak doc's standing caveat holds: no tick-path crash has been seen, but the
seam (tick code deref'ing viewport/camera/matrix state lvRender primes for a
local pawn) is **compile-verified only**; mid-round respawn for the local
pawn is render-gated off for clients (round-boundary spawns only). Watch the
first long soak with movement scripts.

### 6.9 Prop pause/unpause machinery is visibility-driven (audit needed)
`bgUnpausePropsInRoom` is called from the room-visibility walk (bg.c, the same
pass that maintains `ROOMFLAG_STANDBY`/`LOADCANDIDATE`) — which never runs on
a blind server. `g_Vars.alwaystick` only force-foregrounds props on the
**active** list; props parked in the paused segment (`g_Vars.pausedprops`)
are never iterated by `propsTickPlayer` at all. Dedicated servers visibly run
roaming bots, so something keeps the relevant props live in practice, but the
pause/unpause lifecycle has never been audited headless. Audit alongside the
§9 work (Tier 1 makes the question concrete: if everything is "on-screen",
nothing should ever pause).

### 6.10 Residual cosmetic/known items (no action planned)
- Punch/animation-script sounds still play first-person everywhere
  (src/game/CLAUDE.md "known still-broken").
- Continuous-loop gun sounds skipped for remotes (design tradeoff).
- `autoaimTick`/threat detector/`lookingatprop` inert on the server (no
  consumer).

---

## 7. Operational requirements (the non-code workarounds)

- **ROM + assets** next to the binary or `--rom-file` (`fsFullPath("")`); the
  server needs the same data files as a client.
- **`Game.MemorySize=256`** in the savedir's pd.ini — default 16MB pools crash
  the 8-bot playlist's body modeldef load at first rotation.
- **`--savedir` per instance** (own pd.ini/saves/diag log); `--basedir`
  shared read-only. systemd template + hardening in
  `PORT_DEDICATED_SERVER_DEBIAN.md` §8/8a.
- **`--no-advertise`** for private/test instances or they heartbeat the public
  master.
- **60 Hz floor**: the server must hold tick rate; the R4 net-clock fix
  (`g_NetTick += diffframe60`) keeps the *clock* honest below 60fps but a slow
  server still degrades every client (crash-ledger family B was fed by churn
  outpacing a client; the server has the same exposure).
- Logging: `--netdiag <path>`; one writer per file (PID-suffix if running
  multiple instances against one dir).
- **IPv6-less kernels/netns work** (hardened Debian, containers): the
  vendored ENet always opened an AF_INET6 dual-stack socket and aborted
  with "could not create ENet host" when `socket(PF_INET6)` returned
  EAFNOSUPPORT. The vendored IPv4-only fallback (enet.h, documented in its
  commit) arms automatically; real IPv6 peers are unreachable in that mode.
- **`--savedir` must be an absolute path** (or resolvable from the CWD the
  process actually runs in) — a wrong `$S` used to `fclose(NULL)`-crash at
  boot (ledger #23, now guarded), and still means configs/saves land
  somewhere unintended.

---

## 8. Rules of thumb when adding code (the checklist)

1. **Never gate gameplay on render state.** If you need "is this visible",
   ask whether the *server* must answer it; if yes, compute it geometrically
   (rooms/portals/LoS), not from `ROOMFLAG_ONSCREEN`/`PROPFLAG_ON*SCREEN*`/
   `onscreenprops`/`g_MpRoomVisibility`.
2. **Anything added to lvRender's per-player loop needs a pdmain.c mirror
   decision** — mirrored, tolerated, or explicitly listed in §6.
3. **No `currentplayernum == 0`** — use `isremote` / `netPlayerOwnsMouse()`.
4. **`g_Vars.currentplayer` on the server is a remote pawn**: never read local
   input/config/camera (`cam_pos`!) through it in tick-path code.
5. **chr `model->matrices` don't exist headless** — guard with the needsupdate
   gates or use pos/cylinder math.
6. **Frees and GC must not be screen-gated** — the blind host is permanently
   "off-screen".
7. **Per-frame `gfxAllocate` consumers must tolerate the headless pool** (it
   resets via `gfxSwapBuffers`, which must keep running).
8. New authority decisions: remember the server can replay *inputs* but not
   *aim ticks* — anything needing the client's exact view must be
   client-reported + server-validated (the `CLC_HIT` pattern), and the
   validation itself must use blind-safe data (§6.2).

---

## 9. Two-tier visibility restoration (the systemic fix) — IMPLEMENTED, compile-verified

> Status 2026-06-10: both tiers are in the tree (Tier 1: propobj.c
> `func0f08e8ac`/`posIsInDrawDistance`; Tier 2: pdmain.c headless loop).
> **Linux soak: `OVERALL: PASS`** (2026-06-10, containerised headless
> server + headless client, 8-bot churn playlist, 15-min window): **18
> stage_starts** with Tier 1+2 + the §6.1 mirror active, auditor PASS on
> both roles, **98.8% manifest parity**, heal/reap/orphreap fires **= 0**
> (the ghost-mine watch item), peak weapon slots 10/50, no gfx-pool
> overflow at 256MB. The bring-up ALSO root-caused and fixed two
> pre-existing crashers (crash ledger #22 — BOTH shapes of the rotation
> modeldef corruption, including the real #19 root: port-added mod model
> files with no data source promote an uninitialized buffer when a bot
> rolls that body on an install without the mod data dir; #23 =
> `fclose(NULL)` on a bad savedir) and the ENet IPv6-only socket gap
> (§7). Outstanding gates: long (multi-hour) soaks, and a real
> multi-human session — bot churn does not exercise the human
> `handsTickAttack` dispatch or spawn-avoidance perception.

The per-site seams in §4 treat symptoms of one cut chain. The systemic repair
is **two complementary mechanisms**, combined — they answer different
questions and share no consumers, so neither conflicts with the other:

### Tier 1 — "should the authoritative sim run this?" → always YES

Short-circuit the visibility **predicates** — not the room flag — on the
blind server: `func0f08e8ac` and `posIsInDrawDistance` (propobj.c) return
`true` under `#ifndef PLATFORM_N64` + `g_NetDedicatedMode == 1 &&
g_NetMode == NETMODE_SERVER`. (The NETMODE gate keeps the `--headless-client`
soak build behaving like a real client — masking the §6.4 client fulltick
gates there would make soaks less representative.)

Why the predicate and not `ROOMFLAG_ONSCREEN`: forcing the flag still dies one
call later — `camIsPosInFovAndVisibleRoom` (camera.c:615) reads
`bgGetRoomDrawSlot(room)->box` (render-pass-populated screen boxes) and runs a
screen-space frustum test against camera state that doesn't exist headless.
The predicate is the single choke point where the chain can be restored
deterministically.

What Tier 1 buys, all at once:
- `chrTick needsupdate = true` → full anim + joint positioning + **chr model
  matrices exist server-side** (closes §4.5 item 3);
- `objTickPlayer pass2 = true` → obj matrices + `ONTHISSCREENTHISTICK`;
- `g_Vars.onscreenprops` populates → server shot traces have candidates
  (half of §6.2) and the §6.1 `handsTickAttack` mirror becomes fully viable;
- embedded/stuck-prop frees take the on-screen branch → **ghost-mine class
  (§6.3) fixed at the root** instead of leaning on the reconcile backstop;
- subsumes the interact `noscreen` seams (§4.2) over time.

Cost is provisioned: the port already scales the per-frame gfx pools by
`PD_BIG_POOL_SCALE` for exactly the "whole level, culling off" case
(gfxmemory.c), and `MAX_ONSCREEN_PROPS` is 1024 on the port (vs 200 N64) with
the `/octree bigroom` overflow guard already in `propsSort`. Sims taking the
full-fidelity "someone is watching" paths is *more* correct (matches a listen
host), but it changes the dedicated baseline — soak before trusting.

Residual `cam_pos` garbage inside the newly-enabled blocks (chrTick anim-LOD
~chr.c:2920, `explosionCreate` bullethole §6.5): zero the spectator host's
`cam_pos` or patch per-site.

**Do NOT extend Tier 1 to `g_MpRoomVisibility`** — all-ones there would mark
every spawn pad "seen" and poison the spawn-avoidance shortlist. All-zero is
the correct *blind* value; Tier 2 is the correct *sighted* one.

### Tier 2 — "does a human actually see this?" → derive it from client pose

Clients do **not** send a visibility set; `netplayermove` carries the view
*pose* (pos, `angles[2]`, `zoomfov`, `crosspos`) plus `settings.fovy`. But the
visibility machinery is CPU-only: `g_MpRoomVisibility` is just the per-player
OR of `ROOMFLAG_ONSCREEN/STANDBY` after the portal flood (bg.c:6797-6815),
and the flood is portal-graph + frustum math.

**As implemented** (pdmain.c headless loop, per bound combatant, before
`propsTickPlayer` — full rate, mirroring lvRender's per-player order): the
heavy lifting was already done by the tick path —

- `playerTick` (tick path, runs headless) maintains
  `cam_pos/cam_look/cam_up/cam_room` per pawn via `playerSetCamProperties`;
- `bmoveProcessRemoteInput` keeps `player->fovy` tracking the client's real
  (zoomed) FOV via `playerTweenFovY`;

so the synthetic pass is four mirrored calls: `viSetFovAspectAndSize` (vi
state from the player's live fov/aspect), `vi0000b1d0` (perspective matrix →
`camSetMtxF1754` — **must precede the next call**, the documented
`spectatorRenderPanel` ordering trap; its gdl writes land in the throwaway
master display list), `playerAllocateMatrices` (real world-to-screen +
projection — replacing the §4.3 identity fallback with *correct* matrices for
that player), then `g_CamRoom = cam_room; bgTickPortals()` (called directly,
skipping `bgTick`'s `bgTickRooms` room-graphics load/unload, which is
render-tier).

What Tier 2 buys:
- **Spawn avoidance works again** (player.c:292-296): "don't spawn where a
  player is looking" — the one consumer whose semantics neither all-zero nor
  all-ones can satisfy;
- **AI LOD matches listen-host feel**: `chrIsRoomOffScreen` (MP path) goes
  cheap only when *no client* is watching;
- **Per-shooter validation candidates** (§6.2): "what the shooting client
  sees" is the semantically correct set for validating that client's
  `CLC_HIT` — tighter than Tier 1's everything.

Design rule: **derive, never trust.** The pose is already trust-client for
movement, so deriving visibility from it adds no new attack surface. A
client-*sent* visibility list would add a lie vector ("I see nothing") for
anything gated on it — rejected.

Implementation cautions:
- The walk's side effects (`ROOMFLAG_STANDBY`/`LOADCANDIDATE` bookkeeping,
  `bgUnpausePropsInRoom` — see §6.9) now run per combatant, deliberately —
  `bgUnpausePropsInRoom` is gameplay GC and *should* run; `LOADCANDIDATE`
  flags are cleared at the top of each `bgTickPortals` pass and never
  consumed headless (room-graphics loading lives in the render path).
- The client renders `renderbehind` ticks in the past, so derived visibility
  slightly leads the client's true view — irrelevant for spawn/LOD; lag-comp
  already owns the time offset for hits.
- The synthetic viewport is the headless default (100×100, fov/aspect live) —
  self-consistent for the flood, but a slightly different window shape than
  the client's real one. Acceptable; revisit only if avoidance feels off.
- **Pre-existing 4-player packing**: `g_MpRoomVisibility` packs 4 onscreen
  bits (low nibble) + 4 standby bits (high nibble); `bgRoomIsOnPlayerScreen`
  uses `1 << playernum`, so combatant slots ≥ 4 alias the standby nibble.
  An 8-combatant dedicated match needs this widened (u8 → u16 per room)
  before per-player visibility is trustworthy for slots 4-7. Engine-wide
  constraint, predates this work.
- After the last combatant's pass, `ROOMFLAG_ONSCREEN` holds that player's
  view for the rest of the frame — same as a listen host, where the last
  rendered player's flags persist; Tier 1 keeps tick consumers independent
  of it.

### Why the combination is coherent

| Question | Mechanism | Consumers |
|---|---|---|
| Should the sim tick/build/free X? | Tier 1: predicate forced true | prop foreground gate, chr/obj matrices, `onscreenprops`, screen-gated frees |
| Does a player see X? | Tier 2: pose-derived `g_MpRoomVisibility` | spawn avoidance, `chrIsRoomOffScreen` AI LOD, per-shooter hit-validation candidates |

Tier 1 reads `ROOMFLAG_ONSCREEN`-family state through `func0f08e8ac`; Tier 2
writes `g_MpRoomVisibility`. No consumer reads both for the same decision, so
the tiers compose without interference — and together they subsume §4.2's
interact seams, §4.5's degradations, §6.2's blind validation, and §6.3's
ghost mines. The §6.1 `handsTickAttack` mirror rides on top (gated on the
Tier 2 camera prime) — with it, the headless per-player loop is a faithful
gameplay-tier image of lvRender's.
