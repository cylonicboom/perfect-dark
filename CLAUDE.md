# Perfect Dark Port — Claude Reference

## Environment Rules (READ FIRST)

**Do not build.** This repo only compiles inside an MSYS2 MinGW x64 shell, which isn't reachable from this Claude environment. Never run `make`, `cmake --build`, `ninja`, or any other compile invocation — the user builds externally and reports back. Reading CMake files, headers, and verifying code by inspection is fine.

## Code Writing Guidelines

When writing or modifying code, write notes constantly and review to ensure:
- The code makes logical sense and is correct for the task
- No superfluous changes or unnecessary complexity are introduced
- Changes are minimal and focused on the actual problem
- Existing patterns and conventions in the codebase are respected

This keeps the work focused and prevents scope creep.

## Repository Overview

A work-in-progress port of the [Perfect Dark N64 decompilation](https://github.com/n64decomp/perfect_dark) to modern platforms (Windows, Linux, macOS, Nintendo Switch). The source ROM must be provided separately.

- **Main branch**: `port` — stable port, no netplay
- **Netplay branch**: `port-net` (remote only, `remotes/origin/port-net`) — highly experimental internet/LAN multiplayer
- **Private fork**: https://github.com/murkantor/perfect_dark_netplay

## Repo Layout

```
CMakeLists.txt          — build system entry point
src/game/               — original N64 game logic (decompiled C)
src/include/            — game headers (types, constants, bss, data)
src/lib/                — original N64 lib code (sched, vi, snd, etc.)
port/src/               — port-specific platform layer
port/include/           — port headers
port/fast3d/            — libultraship fast3d renderer (OpenGL)
port/external/          — bundled third-party libs (minimp3, enet on port-net)
docs/                   — supplemental docs (netplay.md, etc.)
tools/                  — helper scripts
```

## Build System

CMake with `Unix Makefiles`. Does NOT support Visual Studio. Key flags:
- `-DROMID=pal-final` / `-DROMID=jpn-final` for non-NTSC builds
- Target executable: `build/pd.<arch>[.exe]`

**Toolchain (Windows): MSYS2.** Install dependencies once from an MSYS2 shell:

```
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-zlib mingw-w64-x86_64-cmake mingw-w64-x86_64-python3 mingw-w64-i686-toolchain mingw-w64-i686-SDL2 mingw-w64-i686-zlib mingw-w64-i686-cmake mingw-w64-i686-python3 make git
```

Build from the **MSYS2 MinGW x64** shell (not the plain MSYS2 shell). If the exe launches with `0xc000007b`, check DLL bitness — a 32-bit DLL next to a 64-bit exe is the usual cause.

## Key Platform Files (port/src/)

| File | Purpose |
|---|---|
| `main.c` | Entry point; initialises all subsystems in order |
| `pdmain.c` | Game-side init (memory, stage selection) |
| `pdsched.c` | Per-frame scheduler; calls `netStartFrame`/`netEndFrame` |
| `video.c` | SDL2 + OpenGL window/render |
| `input.c` | SDL2 input, key binds |
| `audio.c` / `mixer.c` | Audio mixer |
| `config.c` | INI config (`pd.ini`) |
| `fs.c` | File system / mod support |
| `system.c` | Logging, args |
| `console.c` | In-game dev console (`~`) |
| `mpsetups.c` | Multiplayer setup file save/load |

---

## Netplay Integration (`port-net` branch)

> Status: **highly experimental**. Only Combat Sim (Combat mode) is partially functional. Self-described as "really bad netcode". Not merged into `port`.

### Architecture

- **Client-server** model. The host is always server; one dedicated listening server, up to `MAX_PLAYERS` (8) clients including the host itself.
- **Transport**: [ENet](http://enet.bespin.org) — a reliable-UDP library, bundled as `port/external/enet.c` + `port/include/external/enet.h`.
- Protocol version constant: `NET_PROTOCOL_VER 12` (bumped to 17 on `port-net-predict`)
- Default UDP port: **27100** (configurable via `pd.ini` or `--port` CLI arg)

### Files Added in port-net

| File | Purpose |
|---|---|
| `port/src/net/net.c` | Core: ENet event loop, `netInit`, `netStartServer`, `netStartClient`, `netDisconnect`, `netStartFrame`, `netEndFrame`, `netSend` |
| `port/src/net/netmsg.c` | Message serialization — all SVC_* and CLC_* read/write functions |
| `port/src/net/netmenu.c` | In-game menus: "Host Network Game" and "Join Game" dialogs |
| `port/src/net/netbuf.c` | Byte-level read/write buffer (typed readers/writers for u8, u16, u32, f32, coord, etc.) |
| `port/include/net/net.h` | Public net API; `netclient`, `netplayermove` structs; NETMODE/CLSTATE/UCMD/DISCONNECT constants |
| `port/include/net/netmsg.h` | SVC_* and CLC_* message ID constants; all read/write function declarations |
| `port/include/net/netbuf.h` | `netbuf` struct definition; buffer API |
| `port/include/net/netenet.h` | Thin ENet include wrapper (undefines `bool`, `near`, `far` after inclusion) |
| `port/external/enet.c` | Bundled ENet source |
| `port/include/external/enet.h` | Bundled ENet header |

### Game Loop Integration

`netInit()` is called in `main()` right after `romdataInit()`.

In `pdsched.c` (the per-frame scheduler), for each 60Hz diffframe:
```c
if (g_NetMode) {
    videoCapFramerate(120);  // cap to 120fps during netplay
    netStartFrame();
}
// ... game tick ...
netEndFrame();
```

`netStartFrame()` polls ENet events (connect/disconnect/receive) and writes outgoing buffers.  
`netEndFrame()` writes player move messages and flushes all pending packets to ENet.

### Connection State Machine

```
CLSTATE_DISCONNECTED → CLSTATE_CONNECTING → CLSTATE_AUTH → CLSTATE_LOBBY → CLSTATE_GAME
```

- **Server** starts in `CLSTATE_LOBBY` immediately (no auth needed for local client).
- **Clients** send `CLC_AUTH` + `CLC_SETTINGS` immediately on TCP connect; server sends `SVC_AUTH` back.
- Auth validates: ROM filename match, mod directory match.
- Late joins (after game started) are rejected with `DISCONNECT_LATE`.

### Message Protocol

Two ENet channels:
- `NETCHAN_DEFAULT` (0): gameplay state (moves, props, damage)
- `NETCHAN_CONTROL` (1): auth, chat, settings — always reliable

**Server → Client (SVC_*)**:

| ID | Name | Description |
|---|---|---|
| 0x02 | SVC_AUTH | Auth response; assigns client ID and player slot |
| 0x03 | SVC_CHAT | Chat message |
| 0x10 | SVC_STAGE_START | Level started; client begins game |
| 0x11 | SVC_STAGE_END | Level ended; client returns to lobby |
| 0x20 | SVC_PLAYER_MOVE | Player position, angles, inputs (per-frame) |
| 0x21 | SVC_PLAYER_GUNS | Player gun state |
| 0x22 | SVC_PLAYER_STATS | Player stats (health, shields, etc.) |
| 0x30 | SVC_PROP_MOVE | Prop position update |
| 0x31 | SVC_PROP_SPAWN | New prop spawned |
| 0x32 | SVC_PROP_DAMAGE | Prop took damage |
| 0x33 | SVC_PROP_PICKUP | Prop picked up by player |
| 0x34 | SVC_PROP_USE | Door/lift/etc used |
| 0x35 | SVC_PROP_DOOR | Door state changed |
| 0x36 | SVC_PROP_LIFT | Lift state changed |
| 0x42 | SVC_CHR_DAMAGE | NPC chr took damage |
| 0x43 | SVC_CHR_DISARM | NPC chr disarmed |
| 0x44 | SVC_CHR_FIRE | Sim chr fired (soundnum>0) or stopped firing (soundnum=0) — added on `port-net-predict` |

**Client → Server (CLC_*)**:

| ID | Name | Description |
|---|---|---|
| 0x02 | CLC_AUTH | Auth request (name, ROM filename, mod dir) |
| 0x03 | CLC_CHAT | Chat message |
| 0x04 | CLC_MOVE | Player input + position this tick |
| 0x05 | CLC_SETTINGS | Player settings changed (head, body, FOV, etc.) |

### Player Move Struct (`netplayermove`)

Sent every frame (unreliable) or on important input change (reliable):
```c
struct netplayermove {
    u32 tick;         // g_NetTick when written
    u32 ucmd;         // UCMD_* bitmask (fire, reload, aim, duck, etc.)
    f32 leanofs;      // lean value
    f32 crouchofs;    // crouch offset
    f32 zoomfov;      // zoom FOV (only if aiming)
    f32 movespeed[2]; // forward/sideways input (animation)
    f32 angles[2];    // theta, verta (view angles)
    f32 crosspos[2];  // crosshair position (aiming mode)
    s8  weaponnum;    // weapon switch request
    struct coord pos; // world position at this tick
    s16 animnum;      // chr->model->anim->animnum (port-net-predict; 0 if unknown)
    s16 animframe;    // chr->model->anim->framea (port-net-predict)
};
```

Note: `animnum`/`animframe` are excluded from `netClientNeedMove`'s change detection (otherwise animframe ticking every frame would force a send every tick).

Important `UCMD_*` bits: `FIRE`, `ACTIVATE`, `RELOAD`, `AIMMODE`, `SELECT`, `SELECT_DUAL` trigger reliable sends. `UCMD_FL_FORCE*` bits force position correction.

### Prop Sync

All active props are assigned a `syncid` (u16, 1-based index into `g_Vars.props`) at stage start via `netSyncIdsAllocate()`. Messages reference props by `syncid` only.

### RNG Sync

Server seeds RNG at stage start (`g_NetRngSeeds[2]`). Clients receive seeds via SVC_AUTH/SVC_STAGE_START and apply them in `netClientSyncRng()`.

### Server Query Protocol

When `Net.Server.AllowInfoQuery` is set (default: true), the server responds to connectionless UDP packets starting with magic `PDQM\x01` with a status payload: protocol version, player count, max players, stage, scenario, host name, ROM name, mod dir. Used for server browser / status tools.

### Config Keys in pd.ini

```
Net.LerpTicks              # interpolation ticks (default 3)
Net.Client.LastJoinAddr    # saved last join address
Net.Client.InRate          # client bandwidth in (bytes/s)
Net.Client.OutRate         # client bandwidth out (bytes/s)
Net.Client.UpdateFrames    # client update interval (ticks)
Net.Server.Port            # server UDP port (default 27100)
Net.Server.InRate          # server bandwidth in
Net.Server.OutRate         # server bandwidth out
Net.Server.UpdateFrames    # server update interval (1 = every tick; 2 = every other)
Net.Server.AllowInfoQuery  # respond to server query packets (0/1)
Net.Debug.LogPath          # diagnostic log file path (empty = disabled)
Net.Debug.LogRate          # ticks between per-client/sim pos dumps (default 6, 0 = disabled)
```

### CLI Flags (port-net only)

```
--host              auto-host on startup
--connect <addr>    auto-join address on startup
--port <n>          server port override
--maxclients <n>    max client cap
```

### Debug / Console

- Press `~` to open console (chat during net game by typing and pressing Enter).
- **Scrollback**: PageUp / PageDown step the visible window by half a page; Home jumps to the oldest line in the ring, End back to the live tail. Closing the console or submitting a line snaps back to the tail. While scrolled, the prompt prefix shows `[-N]` so it's obvious you're not at live output. Ring buffer is `CON_ROWS = 80` lines.
- Lines that start with `/` are **local netplay/debug commands** routed through `netConsoleCommand` instead of being broadcast as chat. They work even outside a net session so you can pre-configure things like lag before connecting. Available commands:
  - `/lag <ms>` — artificial outgoing latency added to every packet (chat included). Useful for reproducing high-ping behavior on a LAN. `/lag 0` disables. Capped at 5000 ms.
  - `/loss <N>` — drop ~1 in N unreliable packets (`g_NetSimPacketLoss`). Reliable packets still go through. `/loss 0` disables.
  - `/diag <path>` — open the diagnostic CSV log to the given path (truncates). `/diag` with no arg closes it. See "Diagnostic Log" below.
  - `/diagrate <ticks>` — change `Net.Debug.LogRate` (per-tick position dump interval). 0 disables dumps.
  - `/netinfo` — print current net state (tick, mode, clients, sims, lag/loss settings, diag path) plus the live tuning knob values below.
  - `/spec [name|next|prev|off]` — spectate another player/sim (camera-only; corpse stays put).
  - **Tuning knobs** (promoted from compile-time `#define`s so they can be changed without rebuilding — useful for hunting CSP / interp regressions on the fly):
    - `/interp <ticks>` — entity interpolation lag. Default 3. Backed by `g_NetInterpTicks` (also config key `Net.LerpTicks`).
    - `/stale <ticks>` — snapshot age before `bwalkUpdateRemote` hard-snaps instead of lerping between stale entries. Default 30 (~500 ms). Backed by `g_NetStaleSnapshotTicks`.
    - `/svcrate <n>` / `/clcrate <n>` — server / client update interval, in ticks. 1 = every tick. Back `g_NetServerUpdateRate` / `g_NetClientUpdateRate`.
    - `/cspframes <n>` — CSP smooth-correction window length. Default 10. Backed by `g_NetCspCorrFramesMax` (the in-flight countdown stays in `g_NetCspCorrFrames`).
    - `/cspcorr <units>` — minimum prediction error (world units) that triggers smooth correction. Default 25. Entered in plain units, stored squared in `g_NetCspCorrThreshSq`.
    - `/cspteleport <units>` — error magnitude (world units) that triggers a hard snap instead of smooth correction. Default 120. Stored squared in `g_NetCspTeleportThreshSq`. Should always be > `/cspcorr`.
  - `/help` / `/?` — list commands.
  Both fake-lag and packet-loss settings persist across disconnect/reconnect within the same process run so you can iterate.
- Press `F9` to toggle the net debug overlay. On `port-net-predict` this shows:
  - **Header**: role (`SERVER`/`CLIENT`), client id / slot, current `g_NetTick`, own ping
  - **Bandwidth**: rolling 1-sec `tx` / `rx` in kB/s, plus this frame's reliable / unreliable byte counts and lifetime totals
  - **Topology**: connected clients / cap, active sims (`g_BotCount`), interp ticks (`g_NetInterpTicks`)
  - **Sim line** (only when active): `sim: lag=Nms loss=1/N qdrop=N` — reminder that fake lag / packet loss is enabled
  - **CSP** (client only): pending correction frames remaining + remaining delta vector
  - **Lag-comp** (server only): number of clients rewound on the last shot + how many ticks were rewound
  - **Per-client list** (everyone in `CLSTATE_LOBBY`+): `[id] name STATE  p=ping  in-X out-Y  lerp=Z` then `pos=(x,y,z)  a=anim/frame  [FARD]` flag bits = Fire / Aim / Reload / Duck. The local client is marked with `*`. `in-X` = ticks since their last move reached us; `out-Y` = ticks our last move has been unacked.

---

## Netplay Enhancement: CSP, Entity Interpolation, Lag Compensation

**Branch**: `port-net-predict` (local, based on `remotes/origin/port-net`)

Three systems implemented. They are off on `port` — only compiled in when `#ifndef PLATFORM_N64`.

### Entity Interpolation (`bondwalk.c`, `bondmove.c`)

**Old**: 2 snapshots (`inmove[0]`/`inmove[1]`), lerp toward newest.  
**New**: 8-snapshot ring buffer (`inmove[NET_SNAPSHOT_COUNT]` + `inmove_head`). `bwalkUpdateRemote()` finds two snapshots bracketing `g_NetTick - g_NetInterpTicks` and interpolates smoothly. `bmoveProcessRemoteInput()` mirrors this for angles/speeds.

Key constant: `NET_SNAPSHOT_COUNT 8` in `net.h`.

### Client-Side Prediction (`net.c`, `netmsg.c`)

Local player already runs physics locally (N64 game handles this). CSP reconciliation adds:
- `g_NetCspHistory[64]`: ring buffer of `{tick, pos}` saved each frame in `netClientRecordMove`.
- When server sends our own position back (non-force), `netCspReconcile()` is called.
- Three error tiers:
  - `err² ≤ NET_CSP_CORR_THRESH_SQ` (25 units): ignored, sub-noise.
  - `err² > NET_CSP_CORR_THRESH_SQ` and `≤ NET_CSP_TELEPORT_THRESH_SQ` (25–120 units): smooth-corrected over `NET_CSP_CORR_FRAMES` (10) ticks via `netCspTick`. Retargets if a fresh ack arrives mid-smoothing.
  - `err² > NET_CSP_TELEPORT_THRESH_SQ` (120 units): hard-snap to server pos via `chrSetPos`, cancel any pending smooth correction. 120 is derived from max player movement physics (strafe-run with `MPOPTION_FASTMOVEMENT` ≈ 1.84 normalized, ~25 horizontal + ~50 vertical world units per tick, plus a ramp/fall buffer). Anything bigger isn't physically reachable in a tick — it's a respawn / kill plane / network glitch, and smooth-correcting it would chase a moving target and pinball.
- Force corrections (`UCMD_FL_FORCEMASK`) still hard-teleport (also via `chrSetPos`, with wire-provided rooms) and cancel pending smooth corrections.

**Snap path must go through `chrSetPos`, not a bare `prop->pos` write.** Earlier the teleport branch did `g_NetLocalClient->player->prop->pos = *server_pos;` directly. That fails on `PROPTYPE_PLAYER` because `bondmovePlayer` keeps its own ground/floor reference in `player->vv_manground` / `vv_ground` and `chr->ground` / `manground` / `sumground`. After a bare pos write those are still pointing at the *pre-snap* floor, so the next physics tick reads "pos is way below floor" and clamps the player back to the old ground — producing a permanent stuck-in-place desync where each new ack re-detects the same ~120 unit error and re-snaps, but the snap never sticks. Observed in diag logs as 30+ consecutive `csp_snap` lines with `err=120…130` and `dy` growing. `chrSetPos(chr, &snap_pos, chr->prop->rooms, server_theta, /*findground=*/true)` re-derives ground via `cdFindGroundInfoAtCyl`, updates `chr->floorroom`, re-registers room references if changed, and (for player props) overwrites `player->vv_manground`/`vv_ground`/`vv_theta` + `unk1c64=1` — making the snap actually take. The CSP path passes the wire's `newmove.angles[0]` as `server_theta` (lags local input by ~RTT but angles change smoothly enough that this is invisible). The rooms array is the chr's current (slightly stale) rooms; `cdFindGroundInfoAtCyl` walks the portal graph from there, which handles snap distances up to a few hundred units. Cross-map teleports still go through `UCMD_FL_FORCEMASK` which carries wire-authoritative rooms.

**Reverted on `port-net-predict`**: an "input replay" history-shift and a magnitude-based variable smoothing window. The shift modified all history entries with `tick >= ack_tick` by the error delta; the window scaled smoothing to 2–5 frames for big errors. Both caused exponential teleporting at ≥100 ms ping: shifted history desyncs subsequent ack comparisons (each new ack reports a fresh huge error against the now-wrong history, schedules another shift, etc.), and the variable window snapped large errors aggressively which looked like teleports instead of smooth corrections.

### Lag Compensation (`net.c`, `prop.c`)

- Server records `{tick, pos}` for each remote client each frame via `netLagCompSave()` into `netclient.lagcomp[120]`.
- In `prop.c`'s `shotCalculateHits()`, before the `chrTestHit` loop: `netLagCompBegin(shooter_client)` computes shooter's one-way RTT in ticks and moves all OTHER clients' `prop->pos` and `rootmtx->m[3]` (broad-phase sphere position) to their historical positions.
- After the loop: `netLagCompEnd()` restores everything.
- Only the sphere broad-phase is lag-compensated. **A full-array bone-matrix translation was attempted on `port-net-predict` and reverted** — `chr->model->matrices` is allocated each frame from `gfxAllocate` (a per-frame heap reset by `gfxSwapBuffers`), so the pointer may be stale or already reused for vertex buffers by the time `shotCalculateHits` runs. Writing past matrix[0] crashed the host on disconnect (access violation, `0xc0000005`). Doing this safely would require either re-deriving the matrices on demand or hooking into the model render path.

### Projectile Rotation Fix (`netmsg.c`)

`netmsgSvcPropMoveWrite` now auto-derives visual rotation for all projectiles. Previously `initrot` was always NULL at most call sites, so rockets arrived at clients with an identity-matrix orientation (wrong).

Fix: when `initrot == NULL` and the prop has a projectile, call `mtx4GetRotation(projectile->mtx.m, &derived_rot)` and use that. Flag bit 2 was already handled on the read side (`mtx4LoadRotation`), so the wire format is unchanged; the server just always populates it now.

### Simulant (AI Bot) Position Sync (`net.c`, `prop.c`)

Sims work because both server and client run the same `setup.c` → `botmgrAllocateBot` code path with synced RNG seeds, giving sim chr props identical syncids on both sides.

**Server** (`net.c` `netEndFrame`): after human player move sends, iterates `g_MpBotChrPtrs[0..g_BotCount-1]` and calls `netmsgSvcPropMoveWrite(&g_NetMsg, chr->prop, NULL)` for each. These are broadcast via the normal `netFlushSendBuffers` call. Guarded by `g_Vars.lvmpbotlevel` and `#ifndef PLATFORM_N64`.

**Client** (`prop.c`): both `botTick` call sites (foreground and background prop loops) are guarded with `#ifndef PLATFORM_N64 / if (g_NetMode != NETMODE_CLIENT)`. On clients, sims skip AI entirely and are position-driven by the incoming `SVC_PROP_MOVE` messages.

**Chr-state extension to `SVC_PROP_MOVE`** (`port-net-predict`): flag bit 4 indicates a follow-on block with `actiontype` (s8 — received but discarded, see below), `yrot` (f32), `animnum` (s16), `animframe` (s16), `anim->speed` (f32), and a 2-byte tail of held-weapon nums (`s8` per hand, -1 = empty). Always set for `PROPTYPE_CHR` props. Without the chr-state block sims were stuck in T-pose / spinning / doing splits because the chr's orientation and animation never updated; with anim speed missing they ran walk/run cycles at hardcoded 1.0 regardless of actual movement speed; with weapon nums missing they held the synced "armed" pose with nothing actually in their hands.

Apply path on client (in order, after `prop->pos = pos`):
1. `modelSetRootPosition(chr->model, &pos)` — pushes the wire pos into `rwdata->chrinfo.pos`. Setting `prop->pos` alone isn't enough: rendering uses the model's internal root, not `prop->pos`. Without this the sim's running anim plays in place — the model never moves to the new world position.
2. `chrSetRotY(chr, yrot)` updates `chr->aibot->roty` for the AI-facing accessor `chrGetRotY` to return the right value.
3. `modelSetChrRotY(chr->model, yrot)` updates the chrinfo yrot that rendering actually reads. `chrSetRotY` alone does NOT propagate to the model for aibots — without this call sims stay facing their spawn direction. On the server, `botApplyMovement` (which we don't run client-side) calls `modelSetChrRotY` directly to keep them in sync.
4. Animation: if `animnum` differs from current, `modelSetAnimation(model, animnum, flip, animframe, animspeed, 0.0625f)`. If unchanged, write `model->anim->speed = animspeed` directly so `modelTickAnim` picks up the new playback rate without snapping the cycle. The server side derives `anim->speed` from `playerChooseThirdPersonAnimation`, which scales it to match the chr's movement speed; without this sync, sims' walk/run cycles play at a fixed 1.0 and look sped-up relative to their actual world movement.
5. Held weapons: for each hand, if the wire weaponnum differs from the currently-held weapon's `weaponnum`, mark the current weapon prop `OBJHFLAG_DELETING` and call `chrGiveWeapon(chr, playermgrGetModelOfWeapon(want), want, hand_flag)` with `OBJFLAG_WEAPON_LEFTHANDED` set for the off hand. The new prop is client-allocated with `syncid=0` (no message references it), and the chr child link is enough for it to render attached to the hand bone. Without this the bot AI's `chrGiveWeapon` path (in `bot.c` when `changeguntimer60` elapses) never runs on the client, so sims looked like they were miming with empty hands.

**`actiontype` from the wire is intentionally dropped — `chr->actiontype` on the client is force-set to `ACT_STAND` regardless.** The server's actiontype (ACT_GOPOS, ACT_ATTACK, ACT_PATROL, ACT_THROWGRENADE, etc.) carries per-state data in the `chr->act_*` union that we don't sync. `chrTick*` dispatch reads that union without null/init checks — applying the server's actiontype on the client crashed in e.g. `chrGoPosGetCurWaypointInfoWithFlags` (chraction.c:5448) on uninitialized waypoint data. Forcing `ACT_STAND` keeps `chrTickStand` as the dispatched tick, which is safe with zero-init union data. Visible animation is still driven by the synced `animnum`, so attack/run/etc. anims play correctly — only the tick logic is reduced to "stand".

**Bot configs synced in `SVC_STAGE_START`**: the server writes all `MAX_BOTS` slots (`mpheadnum`, `mpbodynum`, `team`, `type`, `difficulty`, `name`) at the end of the stage-start packet; the client reads them into `g_BotConfigsArray` before `mpStartMatch()` runs. Without this the client uses whatever its local Combat Sim menu was last set to, so sims spawn with wrong heads/bodies/names.

**Sim chrTick on client**: `botTick` is gated to server-only (sim AI doesn't run on the client). Without _any_ tick, sim chrs don't render. `chrTick` is called instead — it handles model load, anim advancement and render setup without making AI decisions. Position comes from `SVC_PROP_MOVE`.

### Positional Weapon Sounds (`bondgun.c`)

`bgunTick*` functions originally called `sndStart(var80095200, ...)` for shoot/reload/empty/cock sounds. `sndStart` is non-positional ("in your head") which is correct for the local player but wrong for remote players whose `bgunTick` runs locally too (driven by inputs received via `SVC_PLAYER_MOVE` after `setCurrentPlayerNum(remotenum)`). Result: every remote shot played at full volume as if the local player fired.

Fix: new static helper `bgunPlayGunSound(soundnum, handle_out, pstype)` in `bondgun.c`. When `currentplayer->isremote && currentplayer->prop`, routes through `psCreate(NULL, pl->prop, ...)` (3D positional, pans/attenuates by listener distance). Otherwise falls back to `sndStart(...)`. Wired into 6 sites: main shoot sound (both hands), reload, empty-fire (Maian water-hit, tranq, default), and the `GUNCMD_PLAYSOUND` animation-triggered path. Pitch-shift effects that depend on the returned `struct sndstate *` handle (e.g. mauler charge) are skipped for remote shots — minor cosmetic loss.

**Continuous-loop sounds skipped for remote** (SFX_805E / Reaper spin, SFX_LASER_STREAM, SFX_MAULER_CHARGE): these store `hand->audiohandle` for ongoing volume/pitch shaping. `psCreate` doesn't return a compatible handle, and calling it every tick where the condition holds (`audiohandle == NULL`) would spam-overlap. The cleanest workaround is to suppress these continuous sounds for remote players — the actual fire sound still plays positionally via `bgunPlayGunSound`.

PLATFORM_N64 build keeps the original `sndStart` path unchanged.

**Known still-broken sounds**: punching (and possibly some other animation-script-driven sounds outside the GUNCMD_PLAYSOUND path) still play first-person for everyone. Source not yet located.

### Sim Chr Fire Broadcast (`netmsg.c`, `chraction.c`, `net.c`)

New `SVC_CHR_FIRE` (0x44) message — `{prop_syncid:u32, handnum:u8, soundnum:u16}`. Reliable channel, matches `SVC_CHR_DAMAGE`/`_DISARM` so on/off pairs can't get unpaired by a drop.

**Why**: client gates `botTick` (sim AI doesn't run client-side), so sim shots played no sound and showed no muzzle flash. The sim's `chr->model->matrices` ticked via `chrTick` but `chrUpdateFireslot` + `chrSetFiring` never ran.

**On-transition** broadcast in `chrUpdateFireslot` (chraction.c) right after the server-side `psCreate(chr->prop, soundnum, ...)`, gated by `chr->aibot && chr->prop->syncid`. Sends current `soundnum`.

**Off-transition** broadcast in `chrTickShoot` right before the `chrSetFiring(chr, handnum, firingthisframe && normalshoot)` call. Compares `weaponIsGunfireVisible(heldprop)` (current visible state) against `firingthisframe && normalshoot` (about to be set). If was-true and will-be-false, sends `soundnum = 0`.

Client `netmsgSvcChrFireRead` plays positional `psCreate(chrprop, soundnum, PSTYPE_CHRSHOOT)` when soundnum > 0, then toggles `weaponSetGunfireVisible(weaponprop, soundnum != 0, ...)` on the held weapon prop.

### Diagnostic Log (`net.c`)

Set `Net.Debug.LogPath` in `pd.ini` (or via console) to a writable file path. When non-empty, `netStartServer`/`netStartClient` opens the file (truncating it) and `netDisconnect` closes it. Every line is one event in the format:

```
tick,realtime_s,event,key=val key=val ...
```

Events emitted:
- `server_start` / `client_start` / `disconnect`
- `stage_start` / `stage_end`
- `csp_recon` — when CSP detects an above-threshold prediction error. Logs `ack`, `err` (magnitude), and `dx`/`dy`/`dz`.
- `lagcomp` — when the server rewinds clients for a shot. Logs `shooter`, `rtt`, `rewind_ticks`.
- `pos_cl` — per-tick (rate-gated) snapshot of every client's authoritative position + ping + look angles. One line per client per dump.
- `pos_sim` — same for sims, also includes `act` (actiontype) and `hp`.

Dump rate for `pos_cl`/`pos_sim` is controlled by `Net.Debug.LogRate` (default 6 ticks ≈ 10 Hz; set to 0 to disable position dumps entirely while keeping event lines). Every line is flushed immediately so a crash doesn't lose the last few events.

The log is greppable / spreadsheet-importable. For teleport hunting: filter to `pos_cl,id=N` for a specific client, diff consecutive `x/y/z`, sort by delta magnitude. For lag-comp validation: cross-reference `lagcomp` and `pos_cl` entries around the same `tick`.

### Player Animation Sync (`net.c`, `netmsg.c`, `bondmove.c`)

`netplayermove` extended with `s16 animnum; s16 animframe`. Captured in `netClientRecordMove` from `pl->prop->chr->model->anim->{animnum, framea}` (0 if any pointer in the chain is null).

Applied in `bmoveProcessRemoteInput` (the remote-player input path, only called when `pl->isremote || controlmode == CONTROLMODE_NA`): if `g_NetMode == NETMODE_CLIENT` and the incoming `animnum != 0` and differs from the local chr's current `animnum`, call `modelSetAnimation(chr->model, animnum, flip, animframe, 1.0f, 0.0625f)`. Same-anim ticks are left alone so we don't fight the local chrTick frame advance — only divergences trigger a snap.

The anim fields are deliberately excluded from `netClientNeedMove`'s change-detection memcmp (the framea field ticking every frame would otherwise force a send on every tick and undo the update-rate gating).

### Files Changed
- `port/include/net/net.h` — new constants, structs (`csp_snapshot`, `lagcomp_snapshot`, `netkillfeedentry`), ring buffer field in `netclient`, extern decls, function decls; `NET_PROTOCOL_VER 21`; `netplayermove.animnum` / `.animframe` for player anim sync; kill-feed buffer + render decls; **promoted CSP / interp / stale-snap `#define`s to extern globals (`g_NetCspCorrFramesMax`, `g_NetCspCorrThreshSq`, `g_NetCspTeleportThreshSq`, `g_NetStaleSnapshotTicks`) so the `/cspframes`, `/cspcorr`, `/cspteleport`, `/stale` console commands can tune them at runtime; the original `NET_CSP_*` macro names are kept as aliases so call sites are unchanged**; `netCspReconcile` signature gained `server_theta` so the snap branch can call `chrSetPos`
- `port/include/input.h` — added `VK_HOME` (74), `VK_PAGEUP` (75), `VK_END` (77), `VK_PAGEDOWN` (78) to the virtkey enum (SDL scancode values) for console scrollback
- `port/include/net/netmsg.h` — `SVC_CHR_FIRE` (0x44), `SVC_KILL` (0x45), `SVC_SCORE` (0x46) + `netmsgSvcChrFireWrite/Read`, `netmsgSvcKillWrite/Read`, `netmsgSvcScoreWrite/Read` prototypes
- `port/src/net/net.c` — CSP globals, 3 new function groups (CSP + lag comp), updated `netClientRecordMove`, `netEndFrame`, `netDisconnect`, `netClientEvReceive`; sim chr prop broadcast in `netEndFrame`; **`netCspReconcile` uses original retarget-only behavior — history shift + variable window reverted (caused teleporting at high ping)**; **`netCspReconcile` snap branch now calls `chrSetPos(chr, &snap_pos, chr->prop->rooms, server_theta, /*findground=*/true)` instead of a bare `prop->pos = *server_pos` — the bare write left `player->vv_manground` / `vv_ground` / `chr->ground` pointing at the pre-snap floor, so the next `bondmovePlayer` tick clamped the player back, producing a stuck-in-place ack/snap loop visible as 30+ consecutive `csp_snap` lines with `err=120…130` and `dy` growing**; **`netLagCompBegin/End` patch the root matrix only — full-array translation reverted (crash)**; animnum/animframe captured in `netClientRecordMove`; `netClientNeedMove` memcmp excludes the trailing anim fields; expanded `netDebugRender` with per-client list, CSP state, lag-comp activity, and kB/s rates; **diagnostic log infrastructure (`Net.Debug.LogPath`) with per-event lines hooked into stage/csp/lagcomp + rate-gated per-tick position dumps for clients and sims**; **outgoing latency simulator with `/lag` console command**; **runtime tuning knob commands (`/interp`, `/stale`, `/svcrate`, `/clcrate`, `/cspframes`, `/cspcorr`, `/cspteleport`) added to `netConsoleCommand` so CSP/interp/update-rate behavior can be tuned without rebuilding; `/netinfo` now prints all knob values too**
- `port/src/net/netmsg.c` — all `inmove[0]/[1]` → ring buffer; CSP reconcile trigger in `SvcPlayerMoveRead`; `inmove_head` reset in stage start; projectile rotation auto-derived in `netmsgSvcPropMoveWrite`; `netbufWritePlayerMove`/`ReadPlayerMove` extended with animnum + animframe; **`netmsgSvcChrFireWrite/Read` implementations added**; **`netmsgSvcPropMoveWrite`/`Read` extended with bit-4 chr-state block (actiontype + yrot + animnum + animframe + anim->speed + per-hand weaponnum) for sim sync**; **`netmsgSvcStageStartWrite`/`Read` now syncs all `MAX_BOTS` slots of `g_BotConfigsArray` so sims get the right head/body/team/type/difficulty/name on clients**; **chr-state apply on client now calls `modelSetRootPosition` and `modelSetChrRotY` directly so position and visible rotation actually update — `chrSetRotY` alone doesn't reach the model's chrinfo for aibots**; **anim speed pushed into `model->anim->speed` so run/walk cycles play at the server's chosen rate instead of fixed 1.0**; **held-weapon nums drive client-side `chrGiveWeapon` so sims have visible guns matching the server (bot AI's weapon spawn loop is server-only)**; **`netmsgSvcKillWrite/Read` for the kill-feed and `netmsgSvcScoreWrite/Read` for server-authoritative scoreboard deltas (mpchrconfig: numdeaths, numpoints, placement, rankablescore, killcounts[12])**
- `src/game/mpstats.c` — **`mpstatsRecordDeath` gates mpchrconfig writes (numdeaths/killcounts) and aibot stat writes on `g_NetMode != NETMODE_CLIENT` so clients no longer track scores locally; server broadcasts kill-feed line via SVC_KILL and a 1- or 2-entry SVC_SCORE delta for the changed mpchrs immediately after stats update**
- `src/game/bondwalk.c` — rewrote `bwalkUpdateRemote` for ring-buffer snapshot interpolation
- `src/game/bondmove.c` — `bmoveProcessRemoteInput` uses ring-buffer indices + snapshot interpolation for angles/speeds; **applies server-side anim state when it differs from local chr's current animnum**; added `#include "lib/model.h"`
- `src/game/player.c` — 3 `inmove[0]` → `inmove[inmove_head]` fixes for respawn logic; **`playerStartNewLife` skips `scenarioChooseSpawnLocation` on `NETMODE_CLIENT`** — uses current `prop->pos` / `rooms` / `vv_theta` instead. Without this, the client-triggered `playerStartNewLife` (called from `netmsgSvcPlayerStatsRead` when `SVC_PLAYER_STATS` clears the dead flag) picked a locally-derived spawn. If this ran after the server's force-correction `SVC_PLAYER_MOVE` had already been acked (clearing force flags), the server switched to echo-back mode and reflected the client's own position — CSP error was always zero, the desync was never detected, and the client stayed permanently at the local spawn (often near the death point, because `scenarioChooseSpawnLocation` avoids the server's spawn, making the death area look attractive). Fix: on the client, `playerStartNewLife` keeps the current position; the server's force-correction `SVC_PLAYER_MOVE` is the sole authority on where the client spawns.
- `src/game/prop.c` — `botTick` guarded on `NETMODE_CLIENT`; sims fall through to `chrTick` on the client (model load + anim tick + render setup; AI stays server-side); lag compensation hooks in `shotCalculateHits`
- `src/game/bondgun.c` — **new `bgunPlayGunSound` helper that routes weapon sounds through `psCreate` for remote players; wired into 6 call sites (shoot/reload/empty/GUNCMD_PLAYSOUND); 3 continuous-loop sounds (Reaper spin, laser stream, mauler charge) skipped entirely for remote players**
- `src/game/chraction.c` — **`chrUpdateFireslot` broadcasts `SVC_CHR_FIRE(soundnum>0)` on sim shot transitions; `chrTickShoot` broadcasts `SVC_CHR_FIRE(soundnum=0)` on off-transitions**; `chraTick` skips the per-action dispatch (`chrTickStand` etc.) for sim bots on the client so it can't clobber synced anim/yrot
- `port/src/console.c` — `/`-prefixed lines route to `netConsoleCommand` for local netplay/debug commands (e.g., `/lag`, `/loss`, `/diag`, `/netinfo`); **scrollback via PageUp / PageDown / Home / End: 80-line ring buffer (`CON_ROWS`), half-page step (`CON_VISROWS / 2`), edge-triggered so a held key doesn't fly through; while scrolled the prompt prefix shows `[-N]`; closing the console or submitting a line snaps back to the live tail; new lines arriving while scrolled keep the view anchored to the same absolute rows (tmux-style) rather than auto-advancing**
- `port/src/crash.c` — fallback to `addr2line` on Windows when DbgHelp lacks symbols, so MinGW DWARF debug info still produces function names + file:line on crash

### Known Limitations

- Only Combat Sim (Combat scenario) works reliably; other scenarios broken.
- Cloaking device not synced.
- Slayer fly-by-wire and FarSight alt-fire don't work on clients.
- High bandwidth usage, especially with 8 players; recommend `Net.Server.UpdateFrames=2`.
- Sim bots are position-driven on clients — `SVC_CHR_FIRE` syncs shoot sound + muzzle-flash on/off, and the chr-state block in `SVC_PROP_MOVE` syncs body rotation and animation. `chr->actiontype` is **not** synced (would crash; see Sim Position Sync section); the client always dispatches sim chrTick as `ACT_STAND`, so visible animation comes only from the synced `animnum` and not from any per-action tick logic. Aim/look direction (head/torso) and partial-body animations (limb-specific layers) are still server-authoritative only.
- Punching and a few weapon-animation sounds still play first-person for every listener — the punch swing/hit goes through a code path outside the GUNCMD_PLAYSOUND hook we patched. Source TBD.
- Lag-comp is broad-phase (sphere) only. Narrow-phase bone matrix rewind was attempted and reverted after crashing the host — see Lag Compensation section. A safer narrow-phase pass needs to re-derive matrices on demand instead of writing into the per-frame `gfxAllocate` buffer.
- CSP is back to the simpler retarget-only behavior. At very high ping the local player may drift slightly behind the authoritative position when constantly diverging from the server, but it's stable — no exponential teleporting. Snap branch now uses `chrSetPos` (re-derives ground/rooms + resets `player->vv_manground`/`vv_ground`/`vv_theta`) so snaps actually stick — fixes the previous "client thinks it's standing, server says it's fallen, ack/snap loop forever" desync.
- Pitch-shift effects on remote players' weapon sounds (e.g., mauler charge) are skipped because `psCreate` returns a channel index, not a `struct sndstate *` handle. Local player still gets the effect via `sndStart`.
- Weapon equip / pickup sounds (the ~30 other `sndStart` sites in `bondgun.c`) are still non-positional for remote players. Less audible than fire sounds, so deferred.
- No build test performed — see "Environment Rules" at the top: this environment can't compile. The user builds externally from MSYS2 MinGW x64.
