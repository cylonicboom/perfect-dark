# port-net-predict — Files Changed

Per-file breakdown of the changes made on the `port-net-predict` branch (CSP, entity interpolation, lag compensation, sim sync, and supporting infrastructure). Each change is a bullet; the rationale / failure history that was previously inline-bold is broken out into sub-bullets so nothing is lost.

> Scope note: this list covers the netplay-prediction work (net core, message layer, game-logic hooks, console/crash tooling). The separate no-room-culling / draw-slot-limit rendering feature (`constants.h`, `bg.c`, `cheats.c`, `combat.inc`, `netmenu.c`) is documented in [`PORT_NO_CULLING.md`](PORT_NO_CULLING.md).

---

## `port/include/net/net.h`

- new constants, structs (`csp_snapshot`, `lagcomp_snapshot`, `netkillfeedentry`), ring buffer field in `netclient`, extern decls, function decls
- `NET_PROTOCOL_VER 21`
- `netplayermove.animnum` / `.animframe` for player anim sync
- kill-feed buffer + render decls
- promoted CSP / interp / stale-snap `#define`s to extern globals (`g_NetCspCorrFramesMax`, `g_NetCspCorrThreshSq`, `g_NetCspTeleportThreshSq`, `g_NetStaleSnapshotTicks`)
  - so the `/cspframes`, `/cspcorr`, `/cspteleport`, `/stale` console commands can tune them at runtime
  - the original `NET_CSP_*` macro names are kept as aliases so call sites are unchanged
- `netCspReconcile` signature gained `server_theta`
  - so the snap branch can call `chrSetPos`

## `port/include/input.h`

- added `VK_HOME` (74), `VK_PAGEUP` (75), `VK_END` (77), `VK_PAGEDOWN` (78) to the virtkey enum (SDL scancode values)
  - for console scrollback

## `port/include/net/netmsg.h`

- `SVC_CHR_FIRE` (0x44), `SVC_KILL` (0x45), `SVC_SCORE` (0x46) + `netmsgSvcChrFireWrite/Read`, `netmsgSvcKillWrite/Read`, `netmsgSvcScoreWrite/Read` prototypes

## `port/src/net/net.c`

- CSP globals
- 3 new function groups (CSP + lag comp)
- updated `netClientRecordMove`, `netEndFrame`, `netDisconnect`, `netClientEvReceive`
- sim chr prop broadcast in `netEndFrame`
- `netCspReconcile` uses original retarget-only behavior
  - history shift + variable window reverted (caused teleporting at high ping)
- `netCspReconcile` snap branch now calls `chrSetPos(chr, &snap_pos, chr->prop->rooms, server_theta, /*findground=*/true)` instead of a bare `prop->pos = *server_pos`
  - the bare write left `player->vv_manground` / `vv_ground` / `chr->ground` pointing at the pre-snap floor, so the next `bondmovePlayer` tick clamped the player back, producing a stuck-in-place ack/snap loop visible as 30+ consecutive `csp_snap` lines with `err=120…130` and `dy` growing
- `netLagCompBegin/End` patch the root matrix only
  - full-array translation reverted (crash)
- animnum/animframe captured in `netClientRecordMove`
- `netClientNeedMove` memcmp excludes the trailing anim fields
- expanded `netDebugRender` with per-client list, CSP state, lag-comp activity, and kB/s rates
- diagnostic log infrastructure (`Net.Debug.LogPath`) with per-event lines hooked into stage/csp/lagcomp + rate-gated per-tick position dumps for clients and sims
- outgoing latency simulator with `/lag` console command
- runtime tuning knob commands (`/interp`, `/stale`, `/svcrate`, `/clcrate`, `/cspframes`, `/cspcorr`, `/cspteleport`) added to `netConsoleCommand`
  - so CSP/interp/update-rate behavior can be tuned without rebuilding
  - `/netinfo` now prints all knob values too

## `port/src/net/netmsg.c`

- all `inmove[0]/[1]` → ring buffer
- CSP reconcile trigger in `SvcPlayerMoveRead`
- `inmove_head` reset in stage start
- projectile rotation auto-derived in `netmsgSvcPropMoveWrite`
- `netbufWritePlayerMove`/`ReadPlayerMove` extended with animnum + animframe
- `netmsgSvcChrFireWrite/Read` implementations added
- `netmsgSvcPropMoveWrite`/`Read` extended with bit-4 chr-state block (actiontype + yrot + animnum + animframe + anim->speed + per-hand weaponnum)
  - for sim sync
- `netmsgSvcStageStartWrite`/`Read` now syncs all `MAX_BOTS` slots of `g_BotConfigsArray`
  - so sims get the right head/body/team/type/difficulty/name on clients
- chr-state apply on client now calls `modelSetRootPosition` and `modelSetChrRotY` directly so position and visible rotation actually update
  - `chrSetRotY` alone doesn't reach the model's chrinfo for aibots
- anim speed pushed into `model->anim->speed`
  - so run/walk cycles play at the server's chosen rate instead of fixed 1.0
- held-weapon nums drive client-side `chrGiveWeapon`
  - so sims have visible guns matching the server (bot AI's weapon spawn loop is server-only)
- `netmsgSvcKillWrite/Read` for the kill-feed and `netmsgSvcScoreWrite/Read` for server-authoritative scoreboard deltas (mpchrconfig: numdeaths, numpoints, placement, rankablescore, killcounts[12])

## `src/game/mpstats.c`

- `mpstatsRecordDeath` gates mpchrconfig writes (numdeaths/killcounts) and aibot stat writes on `g_NetMode != NETMODE_CLIENT`
  - so clients no longer track scores locally
- server broadcasts kill-feed line via SVC_KILL and a 1- or 2-entry SVC_SCORE delta for the changed mpchrs immediately after stats update

## `src/game/bondwalk.c`

- rewrote `bwalkUpdateRemote` for ring-buffer snapshot interpolation
- early-return on server when `pl->ucmd & UCMD_FL_FORCEMASK`
  - prevents `bwalkUpdateRemote` from writing the client's stale death-position `inmove` back into `prop->pos` on frames T+1+ after respawn.
  - The sequence is: frame T runs `lvTickPlayer` (bwalkUpdateRemote, no force set yet) then `lvRender` (`playerStartNewLife`, sets `prop->pos=SPAWN_A` and force flags); frame T+1 runs `lvTickPlayer` (bwalkUpdateRemote sees `prop->pos=SPAWN_A` vs `inmove=DEATH_POS`, triggers the >512-unit snap, writes `DEATH_POS` back); `netEndFrame` then sends `SVC_PLAYER_MOVE(FORCE, DEATH_POS)` so the client `chrSetPos`s to the death point.
  - The fix: return early when the server holds an authoritative force position, so `outmove[0].pos` stays `SPAWN_A` for all subsequent frames until acked.
  - Works together with the `player.c` `playerStartNewLife` fix (client skips `scenarioChooseSpawnLocation` so it doesn't locally override the server's spawn either).

## `src/game/bondmove.c`

- `bmoveProcessRemoteInput` uses ring-buffer indices + snapshot interpolation for angles/speeds
- applies server-side anim state when it differs from local chr's current animnum
- added `#include "lib/model.h"`

## `src/game/player.c`

- 3 `inmove[0]` → `inmove[inmove_head]` fixes for respawn logic
- `playerStartNewLife` skips `scenarioChooseSpawnLocation` on `NETMODE_CLIENT` — uses current `prop->pos` / `rooms` / `vv_theta` instead
  - Without this, the client-triggered `playerStartNewLife` (called from `netmsgSvcPlayerStatsRead` when `SVC_PLAYER_STATS` clears the dead flag) picked a locally-derived spawn.
  - If this ran after the server's force-correction `SVC_PLAYER_MOVE` had already been acked (clearing force flags), the server switched to echo-back mode and reflected the client's own position — CSP error was always zero, the desync was never detected, and the client stayed permanently at the local spawn (often near the death point, because `scenarioChooseSpawnLocation` avoids the server's spawn, making the death area look attractive).
  - Fix: on the client, `playerStartNewLife` keeps the current position; the server's force-correction `SVC_PLAYER_MOVE` is the sole authority on where the client spawns.

## `src/game/lv.c`

- `lvReset` per-player placement loop: after `playerSpawn()`, force remote combatants to the server's authoritative initial spawn. Server-only guard (`g_NetMode == NETMODE_SERVER && currentplayer->isremote && currentplayer->client`); sets `UCMD_FL_FORCEPOS|FORCEANGLE|FORCEGROUND` on `ucmd` and latches `client->forcetick = g_NetTick` directly.
  - Why: spawn pads are picked deterministically per *local slot* (synced RNG → identical pad per slot on every machine), but `netPlayersAllocate` swaps each client's local player to slot 0. So every client's local pawn picked slot-0's pad (the host's), and the trust-client position echo made the server adopt it — collapsing all humans onto one pad at the **first** match spawn. Sims were immune (their `g_MpBotChrPtrs` order is identical on both sides, no swap).
  - The direct `forcetick` latch is required (rather than relying on the `pl->ucmd` auto-latch in `net.c` `netClientRecordMove`): at stage-load timing `bwalkUpdateRemote` runs before the first move-record, so the `bondwalk.c` one-shot clear (`forcetick == 0` path) would wipe a bare FORCEMASK first. Clears itself on client ack via `netmsgClcMoveRead`.
  - Initial-spawn counterpart to the `playerStartNewLife` respawn fix under `src/game/player.c` above.

## `src/game/prop.c`

- `botTick` guarded on `NETMODE_CLIENT`
- sims fall through to `chrTick` on the client (model load + anim tick + render setup; AI stays server-side)
- lag compensation hooks in `shotCalculateHits`

## `src/game/bondgun.c`

- new `bgunPlayGunSound` helper that routes weapon sounds through `psCreate` for remote players
- wired into 6 call sites (shoot/reload/empty/GUNCMD_PLAYSOUND)
- 3 continuous-loop sounds (Reaper spin, laser stream, mauler charge) skipped entirely for remote players

## `src/game/chraction.c`

- `chrUpdateFireslot` broadcasts `SVC_CHR_FIRE(soundnum>0)` on sim shot transitions
- `chrTickShoot` broadcasts `SVC_CHR_FIRE(soundnum=0)` on off-transitions
- `chraTick` skips the per-action dispatch (`chrTickStand` etc.) for sim bots on the client
  - so it can't clobber synced anim/yrot

## `port/src/console.c`

- `/`-prefixed lines route to `netConsoleCommand` for local netplay/debug commands (e.g., `/lag`, `/loss`, `/diag`, `/netinfo`)
- scrollback via PageUp / PageDown / Home / End
  - 80-line ring buffer (`CON_ROWS`), half-page step (`CON_VISROWS / 2`), edge-triggered so a held key doesn't fly through
  - while scrolled the prompt prefix shows `[-N]`
  - closing the console or submitting a line snaps back to the live tail
  - new lines arriving while scrolled keep the view anchored to the same absolute rows (tmux-style) rather than auto-advancing

## `port/src/crash.c`

- fallback to `addr2line` on Windows when DbgHelp lacks symbols
  - so MinGW DWARF debug info still produces function names + file:line on crash

## Host Spectator Mode

Lets the host opt out of being a combatant and instead drive 1-4 panel observer
views (per-player first-person, free flying cam, or 3D overhead). All 8 wire
slots remain available to remote clients and bots — the host's slot is genuinely
freed, not just hidden. Toggled in the lobby; per-panel mode/target cycle via
in-game C-buttons.

### `src/include/constants.h`

- `MPOPTION_HOSTSPECTATOR 0x40000000` — new bit 30, host-only, port-only
  - serialised in `g_MpSetup.options` so it propagates via `SVC_STAGE_START`

### `src/include/types.h`

- `struct player` gained `is_spectator` and `spectator_panel` (both `#ifndef PLATFORM_N64`)
  - drives HUD suppression / lvRender early-continue / lvTickPlayer skip
  - kept at the end of the existing port-only field block so N64 layout is unaffected

### `port/include/net/net.h`

- `NET_PROTOCOL_VER 27`
- `NET_PLAYERNUM_SPECTATOR 0xFE` sentinel for the host's netclient
- `netclient.is_spectator` field
- `netlobbyclient.is_spectator` field (mirrors above for the lobby UI)

### `port/include/spectator.h` (new)

- `SPEC_MAX_PANELS 4`, `SPEC_MODE_PLAYER/SIM/FREECAM/TOPDOWN` constants
- `struct spectatorpanel`: mode, target, cam pose (pos/look/up/yaw/pitch/room)
- `g_SpectatorPanels[4]`, `g_SpectatorPanelCount`, `g_SpectatorActivePanel`
- `spectatorIsActive`, `spectatorAllocatePanels`, `spectatorFreePanels`,
  `spectatorTickPanel`, `spectatorReadInput`, `spectatorRenderPanel`,
  `spectatorCycleTarget`, `spectatorCycleMode`

### `port/src/spectator.c` (new)

- panel state, freecam input integration, per-mode pose updates,
  target-resolution against `g_NetClients[]` / `g_MpBotChrPtrs[]`
- gamepad bindings: C-Left/Right cycle target, C-Up/Down cycle mode,
  Z trigger cycles the active panel, sticks drive freecam (R-trigger boost,
  D-pad up/down adjusts altitude)
- `spectatorRenderPanel` is a minimal world-only render (sky + bg + props)
  that bypasses the chr/HUD-laden per-player body of `lvRender`

### `port/src/net/net.c`

- `netPlayersAllocate` skips spectator clients when assigning sequential
  combatant playernums
  - keeps the host's view struct in `g_Vars.players[0..panelcount-1]` but
    refuses to bind a remote combatant to it (`cl->player = NULL` on spectator
    slot collision)

### `port/src/net/netmsg.c`

- `SVC_STAGE_START` client manifest: one extra byte per client for the
  spectator flag (read on every client, forces `playernum =
  NET_PLAYERNUM_SPECTATOR` when set)
- `SVC_LOBBY_STATE` per-client block: same extra byte so the lobby UI on
  remote clients can mark the host as "(spec)" before stage start
- defensive guards in `SVC_PROP_PICKUP`/`USE`/`DOOR` read handlers — refuse
  to `setCurrentPlayerNum(actcl->playernum)` if `actcl->is_spectator`

### `port/src/net/netmenu.c`

- new "Spectator Mode" toggle + "Spectator Panels" 1-4 slider in the host
  dialog
  - applied in `menuhandlerHostStart`: sets `MPOPTION_HOSTSPECTATOR` on
    `g_MpSetup.options`, `g_NetLocalClient->is_spectator`, and
    `g_SpectatorPanelCount`
- lobby-view player line tags spectators with `(spec)`
- "Host Spectator" added to the options-list dispatcher

### `src/lib/main.c`

- numplayers selection inflated to `g_SpectatorPanelCount` when host is
  spectating, so `playermgrAllocatePlayers` creates 1-4 panel viewports
- viewport loop dispatches `spectatorReadInput` once per frame and
  `spectatorTickPanel` per panel (replaces `lvTickPlayer`, which would
  deref `prop->pos` on a panel with no mpchr)

### `src/game/playermgr.c`

- `spectatorAllocatePanels` called after `playermgrAllocatePlayer` loop and
  before `netPlayersAllocate`, so the `is_spectator` flag is set on the
  panel slots before remote-client slot binding runs

### `src/game/player.c`

- `playerGetLocalCount` returns `g_SpectatorPanelCount` when host has
  `MPOPTION_HOSTSPECTATOR` set, so `LOCALPLAYERCOUNT()` drives the existing
  split-screen quadrant math in `playerGetViewport*()` for the panels

### `src/game/lv.c`

- `lvRender` per-player loop: `is_spectator` early-continue dispatches to
  `spectatorRenderPanel(gdl)` to avoid the chr/HUD-laden body
- `lvTick` `SLOWMOTION_SMART` iteration: skip spectator panels (they have
  no `prop->rooms` to consult)

### `src/game/mplayer/mplayer.c`

- `mpStartMatch`: when `MPOPTION_HOSTSPECTATOR` is set, host doesn't claim
  slot 0; remote clients fill 0..N-1 instead of 1..N
