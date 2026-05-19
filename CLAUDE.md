# Perfect Dark Port — Claude Reference

## Repository Overview

A work-in-progress port of the [Perfect Dark N64 decompilation](https://github.com/n64decomp/perfect_dark) to modern platforms (Windows, Linux, macOS, Nintendo Switch). The source ROM must be provided separately.

- **Main branch**: `port` — stable port, no netplay
- **Netplay branch**: `port-net` (remote only, `remotes/origin/port-net`) — highly experimental internet/LAN multiplayer

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
- Protocol version constant: `NET_PROTOCOL_VER 12` (bumped to 13 on `port-net-predict`)
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
};
```

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
- Press `F9` to toggle net debug overlay (nettick, ping, bytes sent/recv, reliable/unreliable frame lengths).

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
- When server sends our own position back (non-force), `netCspReconcile()` is called. If error² > `NET_CSP_CORR_THRESH_SQ` (25 units), a smooth correction is scheduled.
- `netCspTick()` (called in `netEndFrame`) applies `1/N` of the correction delta each frame over `NET_CSP_CORR_FRAMES` (10) ticks.
- Force corrections (`UCMD_FL_FORCEMASK`) still hard-teleport and cancel pending smooth corrections.

### Lag Compensation (`net.c`, `prop.c`)

- Server records `{tick, pos}` for each remote client each frame via `netLagCompSave()` into `netclient.lagcomp[120]`.
- In `prop.c`'s `shotCalculateHits()`, before the `chrTestHit` loop: `netLagCompBegin(shooter_client)` computes shooter's one-way RTT in ticks and moves all OTHER clients' `prop->pos` and `rootmtx->m[3]` (broad-phase sphere position) to their historical positions.
- After the loop: `netLagCompEnd()` restores everything.
- Only the sphere broad-phase is lag-compensated; narrow-phase model matrices are left as-is (a known limitation of this test implementation).

### Projectile Rotation Fix (`netmsg.c`)

`netmsgSvcPropMoveWrite` now auto-derives visual rotation for all projectiles. Previously `initrot` was always NULL at most call sites, so rockets arrived at clients with an identity-matrix orientation (wrong).

Fix: when `initrot == NULL` and the prop has a projectile, call `mtx4GetRotation(projectile->mtx.m, &derived_rot)` and use that. Flag bit 2 was already handled on the read side (`mtx4LoadRotation`), so the wire format is unchanged; the server just always populates it now.

### Simulant (AI Bot) Position Sync (`net.c`, `prop.c`)

Sims work because both server and client run the same `setup.c` → `botmgrAllocateBot` code path with synced RNG seeds, giving sim chr props identical syncids on both sides.

**Server** (`net.c` `netEndFrame`): after human player move sends, iterates `g_MpBotChrPtrs[0..g_BotCount-1]` and calls `netmsgSvcPropMoveWrite(&g_NetMsg, chr->prop, NULL)` for each. These are broadcast via the normal `netFlushSendBuffers` call. Guarded by `g_Vars.lvmpbotlevel` and `#ifndef PLATFORM_N64`.

**Client** (`prop.c`): both `botTick` call sites (foreground and background prop loops) are guarded with `#ifndef PLATFORM_N64 / if (g_NetMode != NETMODE_CLIENT)`. On clients, sims skip AI entirely and are position-driven by the incoming `SVC_PROP_MOVE` messages.

`g_NetNumSims` global added (`net.c`, declared in `net.h`) and reset to 0 in `netDisconnect`. `CLFLAG_SIM (1 << 0)` defined in `net.h` for potential future use. `NET_PROTOCOL_VER` bumped to 13.

### Files Changed
- `port/include/net/net.h` — new constants, structs (`csp_snapshot`, `lagcomp_snapshot`), ring buffer field in `netclient`, extern decls, function decls; `NET_PROTOCOL_VER 13`, `CLFLAG_SIM`, `g_NetNumSims` extern
- `port/src/net/net.c` — CSP globals, 3 new function groups (CSP + lag comp), updated `netClientRecordMove`, `netEndFrame`, `netDisconnect`; `g_NetNumSims` global; sim chr prop broadcast in `netEndFrame`
- `port/src/net/netmsg.c` — all `inmove[0]/[1]` → ring buffer; CSP reconcile trigger in `SvcPlayerMoveRead`; `inmove_head` reset in stage start; projectile rotation auto-derived in `netmsgSvcPropMoveWrite`
- `src/game/bondwalk.c` — rewrote `bwalkUpdateRemote` for ring-buffer snapshot interpolation
- `src/game/bondmove.c` — `bmoveProcessRemoteInput` uses ring-buffer indices + snapshot interpolation for angles/speeds
- `src/game/player.c` — 3 `inmove[0]` → `inmove[inmove_head]` fixes for respawn logic
- `src/game/prop.c` — `botTick` guarded on `NETMODE_CLIENT` so sims are position-driven on clients; lag compensation hooks in `shotCalculateHits`

### Known Limitations

- Only Combat Sim (Combat scenario) works reliably; other scenarios broken.
- Cloaking device not synced.
- Slayer fly-by-wire and FarSight alt-fire don't work on clients.
- High bandwidth usage, especially with 8 players; recommend `Net.Server.UpdateFrames=2`.
- Sim bots are position-driven on clients — animations play but AI responses (shooting, dodging) are server-authoritative only.
- No build test performed on `port-net-predict` — MinGW not available in this environment.
