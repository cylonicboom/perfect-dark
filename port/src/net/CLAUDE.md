# Netplay Reference (`port/src/net/`)

> Auto-loads when working under `port/src/net/`. Covers the netplay protocol, game-loop hooks, and the `port-net-predict` CSP / interpolation / lag-comp systems.
>
> See also:
> - [Full list of files changed on `port-net-predict` + rationale](../../../docs/PORT_NET_PREDICT_CHANGES.md) *(placeholder — to be created)*
> - [Known limitations and broken features](../../../docs/PORT_NET_KNOWN_ISSUES.md) *(placeholder — to be created)*
>
> For open prediction-work design questions, see [NOTES_PREDICTION.md](NOTES_PREDICTION.md) (load on demand).

### Files Added in port-net

| File | Purpose |
|---|---|
| `port/src/net/net.c` | Core: ENet event loop, `netInit`, `netStartServer`, `netStartClient`, `netDisconnect`, `netStartFrame`, `netEndFrame`, `netSend` |
| `port/src/net/netmsg.c` | Message serialization — all SVC_* and CLC_* read/write functions |
| `port/src/net/netmenu.c` | In-game menus: "Host Network Game", "Join Game", **"Server Browser"** + details/password dialogs |
| `port/src/net/netmaster.c` | **port-net-predict:** master-server heartbeat + standalone browser socket / list + per-server query parsing |
| `port/src/net/netupnp.c` | **port-net-predict:** client-hosted UPnP port forwarding — SSDP gateway discovery + SOAP Add/DeletePortMapping state machine, pumped from `schedEndFrame` (see `docs/PORT_UPNP.md`) |
| `port/src/net/netbuf.c` | Byte-level read/write buffer (typed readers/writers for u8, u16, u32, f32, coord, etc.) |
| `port/include/net/net.h` | Public net API; `netclient`, `netplayermove` structs; NETMODE/CLSTATE/UCMD/DISCONNECT constants |
| `port/include/net/netmsg.h` | SVC_* and CLC_* message ID constants; all read/write function declarations |
| `port/include/net/netbuf.h` | `netbuf` struct definition; buffer API |
| `port/include/net/netmaster.h` | **port-net-predict:** master/browser constants, `netserverentry`/`netserverdetails`, browser state externs, API (ENet-free so menus can include it) |
| `port/include/net/netupnp.h` | **port-net-predict:** UPnP state constants (`NETUPNP_*`), `g_NetUpnpEnabled`/`g_NetUpnpState`/`g_NetUpnpExternalIP`, API (ENet-free) |
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
| 0x53 | SVC_TIMESCALE | Slow motion / combat boost: server's sim-step halving flag + boost timer (proto 57; see "Slow Motion / Combat Boost Sync") |

**Client → Server (CLC_*)**:

| ID | Name | Description |
|---|---|---|
| 0x02 | CLC_AUTH | Auth request (name, ROM filename, mod dir) |
| 0x03 | CLC_CHAT | Chat message |
| 0x04 | CLC_MOVE | Player input + position this tick |
| 0x05 | CLC_SETTINGS | Player settings changed (head, body, FOV, etc.) |
| 0x0b | CLC_STAGE_COMPLETE | Co-op: client's local sim reached the exit / scripted mission-complete; host ends the stage for all (added on `port-net-predict`) |
| 0x0c | CLC_OBJECTIVE_DONE | Co-op: client completed an objective the host can't witness (trigger room entered, throw-on-object, camera holograph); host latches it into `objectiveCheck` (`g_NetCoopClientObjDone`) and rebroadcasts `SVC_OBJECTIVE` (proto 53) |
| 0x0d | CLC_PICKUP_REQUEST | Co-op: client wants to collect an OBJ/weapon/key prop (by syncid). Clients can't take pickups locally (`objTestForPickup` defers); the host re-validates against the client's synced position via `objTestForPickup` for that player slot and grants authoritatively through `SVC_PROP_PICKUP` (proto 55) |
| 0x0f | CLC_STAGE_READY | Empty body, sent once per stage load from `netSyncIdsAllocate` when the client's world (props + syncids) exists. The server uses it to ship the **JIP catch-up snapshot** to a mid-match joiner (`netServerSendJipSnapshot`, one-shot via `jip_snapshot_sent`) — sending the snapshot back-to-back with the JIP `SVC_STAGE_START` would race the client's deferred stage load and get dropped against the un-loaded world (proto 59) |
| 0x0e | CLC_BOT_CMD | Combat Sim: client orders an own-team simulant — `{botindex:u8, command:u8, targetindex:u8}` (`g_MpAllChrPtrs` indices, wire-stable; target 0xff = none). Server validates teams-enabled + team ownership + command range, then applies via `botApplyAttack` / `botcmdApply` run as the sender (`setCurrentPlayerNum`) so FOLLOW/PROTECT/DEFEND/HOLD anchor to the ordering player. Sent from the active menu (`activemenu.c`) — non-ATTACK slots send directly; ATTACK opens the local pick-target dialog and forwards the chosen target. Reliable control channel (proto 56). The client menu's current-order highlight reads the LOCAL `aibot->command`, which is mirrored back continuously via gunfire-byte bits 3-6 in the chr-state block — so the menu reflects the server's actual state (and confirms the order applied) |

> **Co-op stage-completion handshake.** Mission-complete is detected per-machine on
> the local player (`func0000e990` → `mainEndStage`). The host ending broadcasts
> `SVC_STAGE_END` (everyone plays the debrief). When a *client* finishes, its local
> `mainEndStage` can't reach the host, so `func0000e990` also sends
> `CLC_STAGE_COMPLETE`; the host's `netmsgClcStageCompleteRead` calls `mainEndStage`
> (→ `netServerStageEnd` → `SVC_STAGE_END` to all). Gated to an in-progress co-op
> game (`coopplayernum >= 0 && !g_MainIsEndscreen`) so it can't end a lobby or a
> Combat Sim match; the echo back to the finisher is a `g_MainIsEndscreen` no-op.

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
    u8  renderbehind; // client's g_NetInterpTicks render offset (proto 63, lag-comp)
};
```

Note: `animnum`/`animframe`/`renderbehind` are excluded from `netClientNeedMove`'s change detection (otherwise animframe ticking every frame would force a send every tick; `renderbehind` is ~constant). The memcmp is bounded at the *address of* `animnum`, so any field appended after it is automatically excluded — don't reintroduce a fixed `sizeof(animnum)+sizeof(animframe)` tail (that breaks the moment a field is appended, as `renderbehind` was).

Important `UCMD_*` bits: `FIRE`, `ACTIVATE`, `RELOAD`, `AIMMODE`, `SELECT`, `SELECT_DUAL` trigger reliable sends. `UCMD_FL_FORCE*` bits force position correction.

### Prop Sync

All active props are assigned a `syncid` (u16, 1-based index into `g_Vars.props`) at stage start via `netSyncIdsAllocate()`. Messages reference props by `syncid` only.

### RNG Sync

Server seeds RNG at stage start (`g_NetRngSeeds[2]`). Clients receive seeds via SVC_AUTH/SVC_STAGE_START and apply them in `netClientSyncRng()`.

### Server Query Protocol

When `Net.Server.AllowInfoQuery` is set (default: true), the server responds to connectionless UDP packets starting with magic `PDQM\x01`. The request may carry one trailing `u8` query type: `0`/absent = **summary**, `1` = **details**.

- **Summary** (`netmsgQuerySummaryWrite`): protocol version, flags byte (`NET_QF_INPROGRESS|PASSWORD|DEDICATED|CHALLENGE`), num clients, max clients, **num sims**, stagenum, scenario, **server name** (`g_NetServerName`, not the player name), ROM name, mod dir. This block is shared verbatim with the master HEARTBEAT.
- **Details** (`netmsgQueryDetailsWrite`, appended for type 1): score/time/team limits, then a live scoreboard — per-player `{name, ping, team, score, deaths}` and per-sim `{name, team, difficulty, score}`.

The browser measures ping by timing the summary round-trip and pulls the scoreboard with a details query. Full wire layout (and the master-server protocol) is in [`docs/PORT_MASTER_SERVER.md`](../../../docs/PORT_MASTER_SERVER.md). `tools/query.py [--details] <addr>` is the reference client.

### Master Server + Server Browser (`netmaster.c` / `netmaster.h`)

Port-only discovery. Servers (listen + dedicated) heartbeat to an external UDP tracker (the VPS) every ~15s via `netMasterTick` (called from `netEndFrame`), sent out of the game socket (`netSendConnectionless`) so the master sees the real `ip:port`. The in-game **Network Game → Server Browser** opens a standalone non-blocking UDP socket (`netBrowserOpen`, driven each frame by the dialog handler's `MENUOP_TICK`), asks the master for the directory (`PDMS\x01` LIST_REQUEST/RESPONSE), then direct-queries each listed server for ping + live counts, and (on the Details view) the live scoreboard. The master is a thin directory — never relays game traffic, never sees passwords. Browser state for the UI: `g_NetServerList[]`, `g_NetServerCount`, `g_NetServerDetails`, `g_NetBrowserState`. The `netmaster.h` header is deliberately ENet-free so `netmenu.c` can include it without pulling in enet.

### Host Online Game (`netmaster.c` transport + `netmenu.c` flow + menutick.c hooks)

Port-only "the master hosts for you": **Network Game → Host Online Game** sends a `PDMS` HOST_REQUEST (0x06; same standalone-socket pattern as the browser, 3s retransmit, 20s timeout) and the master (`pdmaster/instances.go`) spawns a dedicated instance, replying HOST_GRANT (0x07: addr + one-off admin token) or HOST_DENY (0x08: reason). The requester then `netStartClient`s into the instance, auto-sends `CLC_ADMIN "login <token>"` + `"take"` (reliable ordered — no SVC_ADMIN reply parsing) from the Joining dialog's tick, and enters the **full Combat Sim hosting UI** via a fresh-CITRAINING reload + the post-match latch (`var80087260`; never run the title-screen setup-load over the live connected lobby — the shieldhits crash class). "Begin Match" reroutes to `netAdminPushStart()` (CLC_ADMIN_SETUP) at menutick.c's `-5` sentinel; a ~10s watchdog in `netStartFrame` returns to the setup if the SVC_STAGE_START never arrives. Session flag `g_NetHostOnlineMode` (+ token/latches) clears in `netDisconnect`. **No NET_PROTOCOL_VER bump** — master-protocol opcodes only. See `docs/PORT_HOSTED_SERVER.md`.

### Join Password

`g_NetServerPassword` (host, `Server.Password`/`--password`) gates joining: the client sends a trailing `str password` in `CLC_AUTH`; `netmsgClcAuthRead` string-compares and kicks a mismatch with `DISCONNECT_PASSWORD`. Only a `NET_QF_PASSWORD` flag is advertised — the password never goes on the wire as plaintext beyond the join attempt itself (and ENet is unencrypted, so this is access-gating, not strong security). The browser prompts for it before connecting to a flagged server; manual joins set `g_NetJoinPassword` first.

### Host Spectator Mode

Host can opt out of being a combatant in the lobby ("Spectator Mode: On" + "Spectator Panels: 1..4"). The host's `netclient` keeps a sentinel `playernum = NET_PLAYERNUM_SPECTATOR (0xFE)`, its `config`/`player` stay `NULL`, and `netPlayersAllocate` skips it when assigning sequential playernums — all 8 wire slots remain available to remote clients and bots. `MPOPTION_HOSTSPECTATOR (0x40000000)` rides along with `g_MpSetup.options` and is mirrored in `SVC_STAGE_START`'s per-client manifest (one extra byte per client).

Locally the host runs 1-4 panel viewports allocated as `g_Vars.players[0..N-1]` (`is_spectator = 1`). The `LOCALPLAYERCOUNT()` override returns the panel count so the existing split-screen quadrant math in `playerGetViewport*` lays out the panels. `lvRender`'s per-player loop dispatches to `spectatorRenderPanel` for spectator slots, bypassing the chr/HUD-dependent body.

Per-panel mode is one of `SPEC_MODE_PLAYER` (first-person from another client), `SPEC_MODE_SIM` (first-person from a sim), `SPEC_MODE_FREECAM` (free flying cam), or `SPEC_MODE_TOPDOWN` (freecam pinned overhead). Top-down works because the same `playerAllocateMatrices(pos, look, up)` primitive that eyespy uses also accepts an arbitrary high-altitude pose; combine with `MPOPTION_NOCULL` for clean overhead shots on large maps.

In-game controls (host only, when in spectator mode):

- **Left stick** — pan the active panel's freecam in the local horizontal plane
- **Right stick** — yaw / pitch the active panel's freecam
- **R-trigger** — freecam boost (×4 movement)
- **D-pad up / down** — freecam altitude
- **C-Left / C-Right** — cycle target player/sim (when active panel is PLAYER or SIM mode)
- **C-Up / C-Down** — cycle active panel's mode (PLAYER → SIM → FREECAM → TOPDOWN)
- **Z-trigger** — cycle which panel is active (consumes input)

The `/spec` console command (camera-only spectate of one chr from the *local player's* view) is unrelated — it predates spectator mode and still works for non-spectator clients.

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
Net.Master.Addr            # user override; empty = use baked-in NET_MASTER_DEFAULT_ADDR (204.152.192.106)
Net.Master.Port            # master-server UDP port (default 27100, same as the game port)
Net.Master.Advertise       # server registers with the master (0/1, default 1)
Net.UPnP.Enabled           # UPnP-forward the server port on the router when hosting (0/1, default 1; see docs/PORT_UPNP.md)
Server.Password            # host join password (empty = open server)
Net.Debug.LogPath          # diagnostic log file path (empty = disabled)
Net.Debug.LogRate          # ticks between per-client/sim pos dumps (default 6, 0 = disabled)
Game.Egg                   # vanity-egg auto-enable on boot (written as `Egg=` under [Game]): "0" (default) = off, "graslu" / "redvox57" = enable that banner (= the /graslu /redvox57 commands). Applied in netInit (netApplyEggConfig), case-insensitive. NOTE: must be a sectioned key — the config system mangles section-less keys (drops the first char on save).
```

### CLI Flags (port-net only)

```
--host              auto-host on startup
--connect <addr>    auto-join address on startup
--headless-client <addr>  headless soak/test client: headless runtime (no
                    window/audio/input, like --dedicated) but JOINs <addr> as a
                    combatant instead of hosting. The prop-sync apply + the
                    invariant auditor run in the tick path (lvRender is skipped),
                    so it's a valid second machine for the manifest-parity check.
                    Spawns at round boundaries only (server-authoritative), takes
                    neutral input. See docs/PORT_NET_SOAK.md "Headless client".
--port <n>          server port override
--maxclients <n>    max client cap
--master <addr>     master-server host/IP override (Net.Master.Addr)
--no-advertise      don't register this server with the master
--no-upnp           don't UPnP-forward the server port on the router
                    (Net.UPnP.Enabled; see docs/PORT_UPNP.md)
--password <pw>     set the host join password (Server.Password)
--svcrate <ticks>   server state-send interval (= Net.Server.UpdateFrames, the
                    /svcrate console command): 1 = 60Hz, 2 = 30Hz/~half band-
                    width; clamped 1..60. CLI form for dedicated instances (no
                    console). pdmaster spawns instances with --svcrate 2.
--clcrate <ticks>   client upstream-send interval (= Net.Client.UpdateFrames,
                    /clcrate); clamped 1..60.
--netdiag <path>    netplay diagnostic CSV path (alias --diag; = Net.Debug.LogPath;
                    opens at host/join, NOT a boot log — that's --log → pd.log)
```

### Debug / Console

- Press `~` to open console (chat during net game by typing and pressing Enter).
- **Scrollback**: PageUp / PageDown step the visible window by half a page; Home jumps to the oldest line in the ring, End back to the live tail. Closing the console or submitting a line snaps back to the tail. While scrolled, the prompt prefix shows `[-N]` so it's obvious you're not at live output. Ring buffer is `CON_ROWS = 80` lines.
- Lines that start with `/` are **local netplay/debug commands** routed through `netConsoleCommand` instead of being broadcast as chat. They work even outside a net session so you can pre-configure things like lag before connecting. Available commands:
  - `/lag <ms>` — artificial outgoing latency added to every packet (chat included). Useful for reproducing high-ping behavior on a LAN. `/lag 0` disables. Capped at 5000 ms.
  - `/loss <N>` — drop ~1 in N unreliable packets (`g_NetSimPacketLoss`). Reliable packets still go through. `/loss 0` disables.
  - `/diag <path>` — open the diagnostic CSV log to the given path (truncates). `/diag` with no arg closes it. See "Diagnostic Log" below.
  - `/diagrate <ticks>` — change `Net.Debug.LogRate` (per-tick position dump interval). 0 disables dumps.
  - `/upnp [on|off|retry]` — client-hosted UPnP port forwarding (netupnp.c): no arg = status (state, gateway, active mapping, external IP), `on`/`off` toggles `Net.UPnP.Enabled` (`off` deletes an active mapping), `retry` re-runs discovery + mapping while hosting. See `docs/PORT_UPNP.md`.
  - `/netinfo` — print current net state (tick, mode, clients, sims, lag/loss settings, diag path) plus the live tuning knob values below.
  - `/slomo` — dump the slow-motion / combat-boost decision chain: type + option bits + challenge unlock, the per-frame engage flag, tick-pin state, live `lvupdate240/60/rem`, and the speedpill (boost) state. Run on both machines to pinpoint where a net slow-mo failure sits (options missing vs flag not set vs step not halved vs client not applying).
  - `/igtick` — print the LOCAL machine's in-game tick rate. First call records `lvframe60` and the wall-clock timestamp; second+ calls report `(lvframe60_now - lvframe60_then) / elapsed_seconds` so you can see whether the local game loop is actually advancing at 60 tps. Also dumps the local player chr's GE i-frame stamp + age + window so you can debug whether the gate is firing. Diagnostic counterpart to `/netinfo`, which only reports the server / wire tick.
  - `/spec [name|next|prev|off|toggle]` — spectate another player/sim. **Player** targets now render the target's *own* viewport (full first-person + HUD + aim, via the `lvRender` slot redirect), not a camera — see `docs/PORT_CLIENT_SPECTATOR.md`. **Sim** targets use a camera at their eyes (yaw-only, no HUD). Auto-engages on death (`netSpectateAutoUpdate`); `toggle` enters/leaves manually. `g_NetSpectateChr` is dangling-guarded (cleared on disconnect + at `mainEndStage`; never dereferenced in the redirect).
  - **Tuning knobs** (promoted from compile-time `#define`s so they can be changed without rebuilding — useful for hunting CSP / interp regressions on the fly):
    - `/interp <ticks>` — entity interpolation lag. Default 3. Backed by `g_NetInterpTicks` (also config key `Net.LerpTicks`).
    - `/stale <ticks>` — snapshot age before `bwalkUpdateRemote` hard-snaps instead of lerping between stale entries. Default 30 (~500 ms). Backed by `g_NetStaleSnapshotTicks`.
    - `/svcrate <n>` / `/clcrate <n>` — server / client update interval, in ticks. 1 = every tick. Back `g_NetServerUpdateRate` / `g_NetClientUpdateRate`.
    - `/lagcomp [exact|legacy]` — server-side hit-rewind mode (`g_NetLagCompExact`, default exact/proto 63). `exact` rewinds targets to `inmovetick − renderbehind` (the precise server-tick the shooter was displaying); `legacy` uses the old `RTT/2 + interp_lag` symmetric-latency estimate. Live A/B for hit-registration feel; the `lagcomp` diag line shows the active mode.
    - `/relevancy [on|off|dist N]` — server-side per-client sim/NPC relevancy cull (`g_NetRelevancy`, default on; P2). `on` sends each remote client only the chrs near/sharing a room with its pawn; `off` broadcasts every chr to all (legacy). `dist N` sets the cull radius (`g_NetRelevancyDist`, default 9000) for a chr not sharing the pawn's room. Config: `Net.Server.Relevancy` / `Net.Server.RelevancyDist`. Flip `off` if a far chr ever pops in. Small/open maps may prefer `off` (per-client packets cost more there than they save).
    - `/cspframes <n>` — CSP smooth-correction window length. Default 10. Backed by `g_NetCspCorrFramesMax` (the in-flight countdown stays in `g_NetCspCorrFrames`).
    - `/cspcorr <units>` — minimum prediction error (world units) that triggers smooth correction. Default 25. Entered in plain units, stored squared in `g_NetCspCorrThreshSq`.
    - `/cspteleport <units>` — error magnitude (world units) that triggers a hard snap instead of smooth correction. Default 120. Stored squared in `g_NetCspTeleportThreshSq`. Should always be > `/cspcorr`.
  - `/wireframe …` (alias `/wf`) — toggle the Wireframe cheat (`CHEAT_WIREFRAME`) live, no stage reload, and configure its appearance. Subcommands: no-arg toggles; `on`/`off`; `bg RRGGBB` sky backdrop colour (`g_WireframeBgColour`, read by `sky.c`, default black); `wire RRGGBB` flat wire colour / `wire off` natural; `thick N` line width px (1..16); `vomit`/`trip` gag mode (animate bg/wire hue + width; `g_WireframeAnimSpeed` in `bgTickPortals`, trip = 4× slower); `save`/`load` persist sky/wire colour + thickness to `pd.ini` (`[Wireframe]` section; shadow vars `g_WfCfg*` registered lazily with `config.c`, sky colour packed `u8[3]`→`u32`); bare `RRGGBB` = `bg` shortcut. Hex takes optional leading `#`, parsed by `netParseHexColour`. On/off flips the cheat's active+enabled bits in `g_CheatsActiveBank1`/`g_CheatsEnabledBank1` (→ `gfx_wireframe_mode` via `bgTickPortals`); wire/thick write the renderer globals `gfx_wireframe_wire_color*` / `gfx_wireframe_line_width`. Unrelated to netplay but lives here because all `/`-commands route through `netConsoleCommand`. See `docs/PORT_WIREFRAME.md`.
  - `/octree [on|off|auto|forcecull|stats|mark|markall|bigroom|portal|unmark]` — port-only outdoor-room octree frustum culling. `portal` toggles `g_BgOctreePortalCull` (default on): cull each room's octree nodes against its portal-clipped draw-slot box (`bgGetRoomDrawSlot(roomnum)->box`) instead of the full viewport, so a room seen through a doorway only submits what shows through it (bigroom rooms unaffected — their box is the whole screen). `bigroom` toggles `g_BgOctreeBigRoom`: disables portal room-culling (ORs into `g_BgNoCull`/`g_BgNoDrawSlotLimit`) + octree-culls every room, so the whole level renders as one open space with the octree as sole visibility (best on open levels — no occlusion culling). `on`/`off` = master toggle (`g_BgOctreeEnabled`); `forcecull` marks every batch culled (flagged rooms render black — proves the filtered list is what's submitted); `stats` prints last-frame counters (`g_BgOctreeStats`); `mark` flags the room you're standing in and builds it now; `markall` toggles `g_BgOctreeMarkAll` (treat *every* loaded room as octree-enabled, lazy-built — test in any level); `auto`/`outdoor` toggles `g_BgOctreeAutoOutdoor` (markall filtered to outdoor rooms only — `room->flags & ROOMFLAG_OUTDOORS` from level data — so outdoor rooms auto-cull with no manual mark; default off, forces master on); `unmark` clears all runtime marks. Drives `bg.c`; works outside a net session. See `docs/PORT_OCTREE.md`.
  - `/padtest caps|led R G B|rumble S MS|trig S MS|hp` — debug aid for the SDL3 gamepad extras (RGB-LED player colours / trigger rumble, `docs/PORT_SDL3_EXTRAS.md`). Body is `inputPadTest` in `input.c` (needs the `SDL_Gamepad` handles); no-op stub in `dedicated_stubs.c`. Unrelated to netplay but routes through `netConsoleCommand` like `/wireframe`.
  - `/gpu` (alias `/renderer`) — print the active rendering backend; with the SDL_GPU backend also the driver (vulkan/direct3d12/metal), shader format (SPIR-V/DXBC/MSL), applied vs requested MSAA, vsync interval and disk shader-cache entry count. Body is `videoGetRendererInfo` (`video.c`) → `gfx_sdlgpu_get_info`. See `docs/PORT_SDLGPU.md`.
  - `/gyro [on|off|sens X [Y]|status]` — gyro aim from pad 1's gyroscope (SDL3 gamepad sensors; merged into the mouse-delta pipeline; `docs/PORT_SDL3_EXTRAS.md`). Body is `inputGyroCommand` in `input.c`; no-op stub in `dedicated_stubs.c`. Config: `Input.GyroAim`, `Input.GyroSpeedX/Y`.
  - `/tonal [on|off]` — toggle the Tonal Inversion cheat (`CHEAT_TONALINVERSION`) live: music note pitches reflected around middle C in the sequence player (strict melodic inversion; SFX unaffected). Cosmetic-only à la `/mirror` (ENABLED bank only). See `docs/PORT_TONAL_INVERSION.md`.
  - `/fps [on|off]` / `/mem [on|off]` — toggle the render-time and memory Lua overlays (`scripts/perf_overlay.lua`, drawn on the right just below the octree overlay — kept near the top because the lo-res screen is only ~220 tall, so y≳224 is off the bottom). They flip port-only globals `g_LuaShowFps` / `g_LuaShowMem` (defined in `luaai_api.c`); the script reads them + the data via the `pd.perf()` binding (`{ fps, frame_ms, vtx_used, vtx_total, show_fps, show_mem }`). FPS comes from `videoGetAverageFPS()`; "memory" is the per-frame **vtx scratch pool** used/total (`gfxGetFreeVtx`/`gfxGetVtxPoolSize`) — the pool the No-Cull/bigroom whole-level render stresses (process RSS wouldn't move, the pools are pre-allocated).
  - `/proplog [syncid]` — dump the networked-prop **lifecycle ring** (`netprop.c`): no arg = the newest ~40 events of any prop; with a syncid = every buffered event for that prop (activate/pause/delist/free/wire-spawn-tx/rx/drop/netfree/gated-explode/hardfree). Reset per stage. The "what touched this prop, in what order" tool for ghost-gun / slot-orphan / double-free hunts. See `docs/PORT_NET_PROP_LIFECYCLE.md`.
  - `/audit [on|off|now|rate N]` — the **prop-sync invariant auditor** (Phase 2 soak harness, `netprop.c`). Runs once a second on both roles (default on), re-deriving the invariants by pool iteration (cycle-immune) and emitting a parseable `audit:` line to the diag log (dupes/corpses/orphan/overcap + heal/reap/orphreap fire counts + a `manifest` digest of the networked syncid set); prints a `LOG_WARNING` to the console only on a non-PASS cycle. `now` runs one cycle immediately; `rate N` sets the cadence (ticks). Feed the diag CSV(s) to `tools/netsoak.py` for a PASS/FAIL verdict + server↔client manifest parity. See `docs/PORT_NET_SOAK.md`.
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
- In `prop.c`'s `shotCalculateHits()`, before the `chrTestHit` loop: `netLagCompBegin(shooter_client)` rewinds all OTHER clients' `prop->pos` and `rootmtx->m[3]` (broad-phase sphere position) to their historical positions at the tick the shooter was displaying.
- After the loop: `netLagCompEnd()` restores everything.

**Exact rewind tick (proto 63).** The rewind target is `target_tick = shooter->inmovetick − shooter->renderbehind`: `inmovetick` is the client's own net-clock stamp on its last applied move (so `g_NetTick − inmovetick` already IS the true upstream staleness — no RTT estimate needed), and `renderbehind` is the client's `g_NetInterpTicks` carried on the move (how far behind its clock it renders other entities). This indexes the lagcomp ring directly (same server-tick epoch). It replaced the old `RTT/2 + (g_NetInterpTicks + interp_lag)` estimate, which was correct only under symmetric latency. The legacy estimate is retained as the fallback when the shooter has no applied move yet (`inmovetick == 0`), and is selectable live via `/lagcomp exact|legacy` (`g_NetLagCompExact`, default 1) for A/B'ing hit feel. The `lagcomp` diag line logs `mode=exact|legacy` + the rewind/target ticks.
- Only the sphere broad-phase is lag-compensated. **A full-array bone-matrix translation was attempted on `port-net-predict` and reverted** — `chr->model->matrices` is allocated each frame from `gfxAllocate` (a per-frame heap reset by `gfxSwapBuffers`), so the pointer may be stale or already reused for vertex buffers by the time `shotCalculateHits` runs. Writing past matrix[0] crashed the host on disconnect (access violation, `0xc0000005`). Doing this safely would require either re-deriving the matrices on demand or hooking into the model render path.

### Projectile Rotation Fix (`netmsg.c`)

`netmsgSvcPropMoveWrite` now auto-derives visual rotation for all projectiles. Previously `initrot` was always NULL at most call sites, so rockets arrived at clients with an identity-matrix orientation (wrong).

Fix: when `initrot == NULL` and the prop has a projectile, call `mtx4GetRotation(projectile->mtx.m, &derived_rot)` and use that. Flag bit 2 was already handled on the read side (`mtx4LoadRotation`), so the wire format is unchanged; the server just always populates it now.

### Simulant (AI Bot) Position Sync (`net.c`, `prop.c`)

Sims work because both server and client run the same `setup.c` → `botmgrAllocateBot` code path with synced RNG seeds, giving sim chr props identical syncids on both sides.

**Server** (`net.c` `netEndFrame`): after human player move sends, iterates `g_MpBotChrPtrs[0..g_BotCount-1]` and writes `netmsgSvcPropMoveWrite(..., chr->prop, NULL)` for each. With **relevancy off** (`/relevancy off`) these go into the shared `g_NetMsg` and broadcast via `netFlushSendBuffers`; with **relevancy on (default, P2)** they're built per-client into `g_NetRelevBuf` (only the chrs relevant to each client) and `netSend`d unicast. Guarded by `g_Vars.lvmpbotlevel` and `#ifndef PLATFORM_N64`.

**Per-client relevancy cull (P2, `g_NetRelevancy`, default on — `docs/netplay-perf-review-2026.md`).** The sim + co-op-NPC chr-state broadcast is the bandwidth driver; relevancy sends each remote client only the chrs it cares about. `netChrRelevantTo(chr, cl)` is **conservative** (cull is default-on, untestable here): always relevant if the chr **shares a room** with the client's pawn (`netRoomsShareAny` over `prop->rooms` — same-room visibility at any range), else relevant only within `g_NetRelevancyDist` (default 9000 world units, ≫ `LV_SMART_SLOMO_RANGE` 1500). Both-far-and-unrelated-rooms ⇒ culled; interpolation + the 2Hz reconcile absorb the brief re-entry pop. A no-pawn client (spectator/JIP) gets everything. Each client keeps its own byte-budgeted round-robin cursor (preserves R1 fairness). Players (`SVC_PLAYER_MOVE`) are **not** culled — they're few and stay in the `g_NetMsg` broadcast. Client read path is unchanged (a `SVC_PROP_MOVE` is the same whether it arrived in the broadcast or a unicast packet). **Tradeoff:** on a small/open map where every chr is relevant to everyone, the per-client path is slightly worse than one broadcast (same payload, N× packets + serialization) — `/relevancy off` there. Delta encoding (step 3) is still deferred.

**Client** (`prop.c`): both `botTick` call sites (foreground and background prop loops) are guarded with `#ifndef PLATFORM_N64 / if (g_NetMode != NETMODE_CLIENT)`. On clients, sims skip AI entirely and are position-driven by the incoming `SVC_PROP_MOVE` messages.

**Chr-state extension to `SVC_PROP_MOVE`** (`port-net-predict`): flag bit 4 indicates a follow-on block, in wire order: `actiontype` (s8 — received but discarded, see below), `yrot` (f32), `animnum` (s16), `animframe` (s16), `anim->speed` (f32), held-weapon nums (`s8` per hand, -1 = empty; right then left), aim properties (`aimupback`, `aimsideback`, `aimuplshoulder`, `aimuprshoulder` — all f32), `angleoffset` (f32), a per-hand **gunfire-visible byte** (u8; bit0 = right, bit1 = left — proto 38), and **authoritative shield + health** (`shield` u8 quantised 0..8, then `chr->damage` f32 — proto 39). **Proto 62 quantizes the pose fields to s16** (bandwidth — `docs/netplay-perf-review-2026.md` P1): `yrot` and `angleoffset` ride as periodic angles (s16 over `[-π,π)`, wrap), the four `aim*` joints as clamped s16 over `[-π,π]`, and `anim->speed` as clamped s16 over `[-16,16]` — via `netbufWriteAngleQ`/`netbufWriteBoundedQ` and their readers in `netmsg.c`. `pos` (coord) and `chr->damage` (f32) stay full-precision. Always set for `PROPTYPE_CHR` props. Without the chr-state block sims were stuck in T-pose / spinning / doing splits because the chr's orientation and animation never updated; with anim speed missing they ran walk/run cycles at hardcoded 1.0 regardless of actual movement speed; with weapon nums missing they held the synced "armed" pose with nothing actually in their hands; with aim/angleoffset missing their arms pointed straight forward regardless of target; with the gunfire byte missing the muzzle flash could stick on (see the muzzle-flash note below).

Apply path on client (in order, after `prop->pos = pos`):
1. `modelSetRootPosition(chr->model, &pos)` — pushes the wire pos into `rwdata->chrinfo.pos`. Setting `prop->pos` alone isn't enough: rendering uses the model's internal root, not `prop->pos`. Without this the sim's running anim plays in place — the model never moves to the new world position.
2. `chrSetRotY(chr, yrot)` updates `chr->aibot->roty` for the AI-facing accessor `chrGetRotY` to return the right value.
3. `modelSetChrRotY(chr->model, yrot)` updates the chrinfo yrot that rendering actually reads. `chrSetRotY` alone does NOT propagate to the model for aibots — without this call sims stay facing their spawn direction. On the server, `botApplyMovement` (which we don't run client-side) calls `modelSetChrRotY` directly to keep them in sync.
4. Animation: if `animnum` differs from current, `modelSetAnimation(model, animnum, flip, animframe, animspeed, 16.0f)` — **merge time 16 matches the host's chr transitions** (`playerChooseThirdPersonAnimation`, player.c:6186) so sim anim switches cross-fade like the base game instead of popping (was `0.0625f`, ~256× too short = effectively instant). If unchanged, write `model->anim->speed = animspeed` directly so `modelTickAnim` picks up the new playback rate without snapping the cycle. The server side derives `anim->speed` from `playerChooseThirdPersonAnimation`, which scales it to match the chr's movement speed; without this sync, sims' walk/run cycles play at a fixed 1.0 and look sped-up relative to their actual world movement.
5. Held weapons: for each hand, if the wire weaponnum differs from the currently-held weapon's `weaponnum`, mark the current weapon prop `OBJHFLAG_DELETING` and call `chrGiveWeapon(chr, playermgrGetModelOfWeapon(want), want, hand_flag)` with `OBJFLAG_WEAPON_LEFTHANDED` set for the off hand. The new prop is client-allocated with `syncid=0` (no message references it), and the chr child link is enough for it to render attached to the hand bone. Without this the bot AI's `chrGiveWeapon` path (in `bot.c` when `changeguntimer60` elapses) never runs on the client, so sims looked like they were miming with empty hands.

**`actiontype` from the wire is intentionally dropped — `chr->actiontype` on the client is force-set to `ACT_STAND` regardless.** The server's actiontype (ACT_GOPOS, ACT_ATTACK, ACT_PATROL, ACT_THROWGRENADE, etc.) carries per-state data in the `chr->act_*` union that we don't sync. `chrTick*` dispatch reads that union without null/init checks — applying the server's actiontype on the client crashed in e.g. `chrGoPosGetCurWaypointInfoWithFlags` (chraction.c:5448) on uninitialized waypoint data. Forcing `ACT_STAND` keeps `chrTickStand` as the dispatched tick, which is safe with zero-init union data. Visible animation is still driven by the synced `animnum`, so attack/run/etc. anims play correctly — only the tick logic is reduced to "stand".

**Bot configs synced in `SVC_STAGE_START`**: the server writes all `NET_MAX_BOTS` (8) slots — net games are capped there; the offline-only 32-sim extras (docs/PORT_OFFLINE_32_SIMS.md) never ride the wire — (`mpheadnum`, `mpbodynum`, `team`, `type`, `difficulty`, `name`) at the end of the stage-start packet; the client reads them into `g_BotConfigsArray` before `mpStartMatch()` runs. Without this the client uses whatever its local Combat Sim menu was last set to, so sims spawn with wrong heads/bodies/names.

**Sim chrTick on client**: `botTick` is gated to server-only (sim AI doesn't run on the client). Without _any_ tick, sim chrs don't render. `chrTick` is called instead — it handles model load, anim advancement and render setup without making AI decisions. Position comes from `SVC_PROP_MOVE`.

### Sim Chr Fire Broadcast (`netmsg.c`, `chraction.c`, `net.c`)

New `SVC_CHR_FIRE` (0x44) message — `{prop_syncid:u32, handnum:u8, soundnum:u16}`. Reliable channel, matches `SVC_CHR_DAMAGE`/`_DISARM` so on/off pairs can't get unpaired by a drop.

**Why**: client gates `botTick` (sim AI doesn't run client-side), so sim shots played no sound and showed no muzzle flash. The sim's `chr->model->matrices` ticked via `chrTick` but `chrUpdateFireslot` + `chrSetFiring` never ran.

**On-transition** broadcast in `chrUpdateFireslot` (chraction.c) right after the server-side `psCreate(chr->prop, soundnum, ...)`, gated by `chr->aibot && chr->prop->syncid`. Sends current `soundnum`.

**Off-transition** broadcast in `chrTickShoot` right before the `chrSetFiring(chr, handnum, firingthisframe && normalshoot)` call. Compares `weaponIsGunfireVisible(heldprop)` (current visible state) against `firingthisframe && normalshoot` (about to be set). If was-true and will-be-false, sends `soundnum = 0`.

Client `netmsgSvcChrFireRead` plays positional `psCreate(chrprop, soundnum, PSTYPE_CHRSHOOT)` when soundnum > 0, then toggles `weaponSetGunfireVisible(weaponprop, soundnum != 0, ...)` on the held weapon prop.

**Muzzle-flash visibility is now CONTINUOUS, not edge-driven (proto 38).** The on/off `SVC_CHR_FIRE` events above are fragile for *visibility*: the off-edge can be missed (single-frame trigger release, ammo-out stop, death, weapon swap between on and off), which left the flash stuck on — a recurring user-reported bug. The fix appends a **per-hand gunfire-visible byte** to the `SVC_PROP_MOVE` chr-state block (bit0 = right hand `HAND_RIGHT`, bit1 = left hand `HAND_LEFT`), written from `chrIsGunfireVisible(chr, h)` and reconciled every snapshot on the read side (after the weapons-held loop, so `chrGetHeldProp` resolves the current prop). Because the chr-state block is broadcast every state-send tick (svcrate-gated since the P3b cadence pass; settled ACT_DEAD corpses ride a slower ~8x throttle), a stuck flash now self-clears within one snapshot interval. `SVC_CHR_FIRE` is retained for the **crisp single-frame onset + positional sound** (reliable channel); the continuous byte only guarantees the OFF can't be missed. The edge off-transition in `chrTickShoot` is left in place as a faster-than-one-snapshot OFF but is no longer load-bearing for correctness.

**Sim leg animation is now TIME-ALIGNED with the interpolated body.** Previously `animnum`/`framea`/`speed` were applied at *receive time* (current), while `netChrInterpolate` rendered the body position *in the past* (interp delay). When the server transmits a near-zero `anim->speed` — which `playerChooseThirdPersonAnimation` produces in the soft-turn band (`speed = 2.0 * turnspeed`) as a bot decelerates to fire — the old receive-time apply pinned the legs to that **current** ~0 speed while the body rendered a **delayed**, still-moving position: frozen legs under a gliding body, worst when stopping to fire. The fix buffers `animnum`/`framea`/`speed` in the **existing snapshot ring** (`struct netchrpose` + `chrdata.netsnap[]`; no wire change — these were always on the wire) and reconstructs the anim for the **same past instant** as the body in `netChrInterpolate`: the older bracket snapshot's discrete `animnum`/`framea` plus a lerped `speed`, so motion and cadence share one time domain and always agree. The client still free-runs the frame via its own `chrTick` (prop.c routes client sims to `chrTick` not `botTick`; the ACT_STAND branch in chr.c advances `chr0f0220ec`→`modelTickAnimQuarterSpeed` each fulltick at this speed). The **receive-time anim apply in `netmsgSvcPropMoveRead` is now gated behind `!g_NetChrInterp`** — it's the fallback for `/chrinterp off`; with interp on, `netChrInterpolate` is the sole anim driver (applying the current speed there too would re-introduce the mismatch). No anim classification and no protocol bump were needed. `/chrinterp off` reverts to the legacy receive-time behaviour.

**Anim-switch hysteresis.** A firing bot that hovers at the `turnspeed < 0.05` stand<->soft-turn-walk threshold makes the server toggle its animnum almost every snapshot; faithfully replaying that flip-flopped the client between a walk and a standstill. `netChrInterpolate` now only adopts a new discrete animnum when **both bracketing snapshots agree** on it (an `animstable` flag) — a genuine transition is picked up within ~1 snapshot, but a 1-snapshot blip is held through (the continuous `speed` still updates so the held anim doesn't freeze). A freshly-spawned sim (animnum 0) bypasses the hysteresis so it can't get stuck pose-less.

**Body-yaw wire field fix (the "sim faces ~90 deg off while firing" bug).** The chr-state block sends the chr's body yaw, applied client-side via `modelSetChrRotY` with the synced `angleoffset` twisting the waist on top (`chrHandleJointPositioned`: waist yrot = `aimsideback + aibot->angleoffset`). The server's RENDERED body yaw is `angle2 = lookangle - angleoffset`, set by `botApplyMovement` via `modelSetChrRotY(chr->model, angle2)` (bot.c) — so the server's upper body ends up at `angle2 + angleoffset = lookangle` (the target). The write side originally sent **`chrGetRotY(chr)` = `aibot->roty`, the bot's separate MOVEMENT facing**, which differs from the rendered `angle2` by up to ~`angleoffset` whenever the body is turned away from the aim (a stationary bot twisting to track a player, or a strafing bot). The client then based the whole body on that wrong yaw and added `angleoffset` on top, rotating the entire sim away from the target. Fix: send **`modelGetChrRotY(chr->model)`** (the rendered `angle2`) instead, so the client reproduces the server's exact body+waist pose. It was a genuine sync gap, not PD's aim approximation. Same f32 field, no protocol bump.

### Sim Health & Shield Sync (`netmsg.c`, proto 39)

**Problem:** clients reconstructed a sim's HP/shield *entirely* by replaying `SVC_CHR_DAMAGE` through `chrDamage`. That only stays correct if the client's starting values match AND every HP/shield change flows through a replayed damage event AND `chrDamage` is deterministic — none of which held:

- **Shield is set in the server-only bot path** and never replayed: spawn/respawn shield for TURTLE/SHIELD bots and shielded Dark sims (`botReset`, bot.c:215/227), the global `mpHasShield` option, and **shield pickups** (`chrSetShield(chr, shield->amount * 8)`, bot.c:479). Shield is a real second HP pool — a hit is fully absorbed (`damage = 0`) while shield lasts (chraction.c:4709-4721). A client sim with shield = 0 therefore takes to health what the host's shielded sim shrugs off.
- **RNG divergence:** the headshot multiplier is `damage *= 1..6` via `rngRandom()` (chraction.c:4666-4672). The client's RNG stream has diverged from the host's (the server runs bot AI every tick consuming RNG; the client runs none — `botTick` is server-gated), so a replayed headshot scales by a *different* random factor → a host kill becomes a client non-kill or vice-versa.
- **Respawn / health pickups** reset or raise `chr->damage` outside any replayed event.

Net symptom: no shield-hit flash on the client and damage spilling into health early, so a sim reads as "won't die" to a client shooter; HP drifts permanently.

**Fix:** the chr-state block now carries the **authoritative** `cshield` (u8, 0..8, 1/32-unit precision) + `chr->damage` (raw f32 — an armoured chr's damage is negative, so not quantised). The client **overwrites** both every snapshot (`g_NetMode == NETMODE_CLIENT`); the `SVC_CHR_DAMAGE` replay is left running purely for effects (blood, shield flash, knockback, sound), no longer load-bearing for HP. **Death visuals are unaffected** — they're animnum-driven (the client force-sets `ACT_STAND` in the chr-state apply, so `chrIsDead`, which keys on `ACT_DIE/ACT_DEAD`, never trips client-side anyway), so no client-side `chrDie` is needed. Protocol bumped 38 → 39.

### Cloak Sync — Players + Sims + NPCs (no proto bump)

Cloaking (`chr->hidden & CHRHFLAG_CLOAKED`) was fully unsynced: the cloak
DECISION inputs are owner-local (a sim's `aibot->cloakdeviceenabled` /
`rcp120cloakenabled` come from server-only bot AI; a player's `devicesactive` +
cloak ammo are never replicated), so remote machines not only never engaged a
cloak — `chrUpdateCloak`'s decision branches read "nothing active" and would
instantly *un*cloak any flag that got set. Three pieces fix it:

- **Owner publishes state, never the decision.** Players: `UCMD_CLOAKED`
  (bit 11) is set in the local ucmd composition (`bondmove.c`) from the own
  chr's flag, riding every CLC_MOVE/SVC_PLAYER_MOVE (verbatim ucmd
  pass-through). Sims/co-op NPCs: bit 2 of the existing **gunfire byte** in the
  `SVC_PROP_MOVE` chr-state block (spare bit — wire size unchanged, no protocol
  bump; covers script-cloaked NPCs too since the writer reads `chr->hidden`).
  Gunfire-byte layout is now: bits 0-1 = per-hand muzzle flash, bit 2 = cloak,
  bits 3-6 = current `aibot->command` (+1 biased, 0 = absent — drives the
  client active menu's order highlight, see CLC_BOT_CMD), bit 7 spare.
- **Remote machines mirror the flag, edge-detected** (so the on/off SFX plays
  once per transition, positionally): players in `bmoveProcessRemoteInput`
  (`bondmove.c`, the eyesshut/crouch pattern); sims/NPCs in the chr-state
  cloak reconcile (`netmsgSvcPropMoveRead`, right after the muzzle-flash
  reconcile). Continuous state — a dropped packet self-heals next snapshot.
- **`chrUpdateCloak`'s decision region is skipped for wire-driven chrs**
  (`chr.c`): remote players (`isremote`, any netmode — the server doesn't know
  other players' devices either) and client-side synced chrs (`NETMODE_CLIENT`
  + `syncid`). Only the flag-driven `cloakfadefrac` fade tail runs for them, so
  the shimmer still animates locally.

Transient divergence (e.g. `chrUncloakTemporarily` firing on a remote machine's
replayed shot) converges within one move/snapshot via the continuous reconcile.

### Sim Position Speed Cap Removed (`netmsg.c`)

The receive-time chr-state apply (`netmsgSvcPropMoveRead`) used to gate its 50% smoothing blend behind `dist_sq < 80*80` (80 units / server update, "above what AI movement can produce") and **hard-snap** above it — the same guard reused for the yrot, aim-shoulder and angleoffset blends. That assumption is false for high-speed (Dark) sims, which legitimately move >80 units/update, so the cap mis-fired and snapped them every update. **All four blends are now unconditional.** The blend always converges (each packet halves the remaining error), so a genuine respawn/teleport just slides over a few packets instead of getting stuck — and `prop->pos` is still committed every packet so it can't freeze (the original stuck-at-death-location concern). This is the `/chrinterp off` fallback path only; with `/chrinterp on` (default) `netChrInterpolate` already drove position from the raw, **un-capped** snapshot ring, so it never had a speed cap. No wire/protocol change.

### Sim Room Membership Time-Alignment (`netmsg.c`, `net.c`, `types.h`, `net.h`)

`prop->rooms` is read by room culling AND by `func0f08e8ac` (the per-tick visibility/update gate, called every chr tick in the ACT_STAND branch), both of which test it *together with* `prop->pos`. On a client those two were in **different time domains**: `netmsgSvcPropMoveRead` registered the **current** wire rooms immediately, while `netChrInterpolate` renders `prop->pos` ~interp-delay ticks **in the past** and never touched `prop->rooms`. They only diverge at a room boundary — a **ledge or doorway** — so a sim that darts off a ledge and quickly back flips `prop->rooms` to the lower room while `prop->pos` is still rendered up on the ledge (or vice-versa); the visibility gate / cull then misfires and the sim **freezes or vanishes**.

Fix: carry the wire rooms in the snapshot ring (`RoomNum rooms[8]` added to `chrdata.netsnap[]` in `types.h` — inside the existing port-only block — and to `netchrpose` in `net.h`) and re-register `prop->rooms` **time-aligned** with the interpolated pos inside `netChrInterpolate`, from the **older bracket snapshot** (the same instant the discrete `animnum` comes from), or the source/head snapshot in the single/extrapolation case. The receive-time path now **skips** registering rooms for interpolated chrs (`interp_owns_rooms = g_NetChrInterp && client && PROPTYPE_CHR`) precisely so the interp path owns it; projectiles, objects, and chrs under `/chrinterp off` still register immediately (`netChrInterpolate` early-returns there). A local `netChrRoomsEqual` skips the deregister/register churn when the time-aligned rooms are unchanged (they only change at boundaries). No wire/protocol change.

### Sim "Under the Map" / Invisible Fix — Local Gravity (`chr.c`)

Symptom: on the client, some sims become **invisible while their radar blip stays correct** ("they're under the map"). The position sync is *not* at fault — `prop->pos` (what radar/collision/targeting read) is correct. The chr's **rendered vertical** comes from `chr->manground` (`chr0f01f378`: `arg2->y += manground`), which the client integrates with **local gravity** (`fallspeed`/`manground` via `cdFindGroundInfoAtCyl`) and which the position sync never corrects — `netChrInterpolate` pins `prop->pos` but not the ground state. For a sim the client momentarily treats as airborne (e.g. just after running off a ledge — the user's earlier "off-ledge-and-back" clue), `manground` runs away downward and the model sinks below the floor (invisible), accelerating over ~a minute. It hits only *some* sims because it depends on the local ground-find at each sim's spot.

The runaway is in `chr0f01f378`: it finds ground at the **anim-driven model pos** (`arg2`), so once that drifts even slightly the ground-find drifts with it and `manground` free-falls. (`CHRCFLAG_FORCETOGROUND` does **not** fix it — it's consumed *inside* `chr0f01f378` *after* that bad ground-find, so it just re-snaps to the wrong, under-map ground.)

Fix (`chrTick`, right after `netChrInterpolate`, client-driven aibot sims only): **keep the engine's own gravity** (`chr0f01f378` — natural ledge falls, slopes, landing) and add **one clamp** that breaks the runaway: never let `manground` fall *below* the floor found at the **synced `prop->pos`** X/Z (`cdFindGroundInfoAtCyl`). A genuine fall keeps `manground` *above* the floor (descending toward it), so the clamp is a no-op for real gravity — the bot still arcs off the ledge and lands naturally. It only fires on the runaway; re-seating `manground` at the floor puts `arg2` (= `anim_local + manground`) back above the floor so the next floor-find is correct again — so in practice the sink can't even start. On the clamp, reset `sumground`/`fallspeed` too.

  *Rejected earlier attempts:* (a) `CHRCFLAG_FORCETOGROUND` — consumed inside `chr0f01f378` *after* the bad floor-find, re-snaps to the wrong ground. (b) pin `manground` to the local floor every tick — stopped the sink but **snapped airborne bots to the floor** (no fall). (c) pin `manground` to the interpolated wire `prop->pos.y` — followed the fall arc but **floated bots**, because `prop->pos.y` rides a fixed offset (`anim_local`, the root ride-height) above the floor. The clamp keeps the engine's correct vertical and only guards the failure mode. No wire/protocol change; `#ifndef PLATFORM_N64` + `g_NetMode == NETMODE_CLIENT` gated, so N64 / server / single-player are unaffected.

### Prop Reconciliation Backstop (`SVC_PROP_RECONCILE`, proto 41)

Event-based prop removal (`SVC_PROP_FREE`) is the primary path, but it can be
missed — the embedded-mine-on-a-chr free is **screen-gated** on the host
(`chr0f022214` on-screen vs `func0f0706f8` off-screen processing of a chr's
child props), so a mine stuck to an on-screen sim wasn't reliably freed/broadcast,
leaving a **ghost** on clients. The reconciliation backstop heals that class
regardless of cause: the server broadcasts, twice a second (`net.c` `netEndFrame`,
`g_NetTick % (NET_HEARTBEAT_INTERVAL/2) == 10`, reliable channel), the syncids of
every networked **weapon/obj** prop it currently holds (`netmsgSvcPropReconcileWrite`
— terminator-delimited u16 list). The client (`netmsgSvcPropReconcileRead`) builds
a bitmap and `objFreePermanently`s any weapon/obj synced prop it holds that the
host's set doesn't list — a ghost the host already freed. The reliable ordered
channel means that when the client processes the message it has applied every
prior spawn/free, so an absent prop is genuinely a ghost (no in-flight skew); a
truncated message sets `src->error` and the removal pass is skipped. Existence-only
(weapon/obj scoped) — it heals ghosts, not identity-mismap (that relies on the
deterministic positional syncid pool kept consistent by symmetric spawn/free).
See `docs/PORT_COOP_PREP.md` for the design rationale and the checksum optimization
for scale.

### The Uninitialised-netsnap Crash (root cause) + Wire Hardening (no proto bump)

**Root cause of the "client leaves after a match ended" crash family** (`0xc0000005`
in `netChrInterpolate` reading a garbage `netsnaphead`; `portal00018148`
wild-writing off garbage `prop->rooms`): the port-appended `chrdata.netsnap[]` /
`netsnaphead` were **never initialised** — `chrInit` sets fields one by one (no
memset) and chr slots are recycled stage-pool memory. First sessions worked
because fresh OS pages are zero; after a match, the next stage's chrs inherit the
old pool bytes. While still connected (`NETMODE_CLIENT` — e.g. the front-end
stage loading after `SVC_STAGE_END`, before the user disconnects), every chr runs
`netChrInterpolate`, which trusts `netsnap[head].tick != 0`: garbage head = OOB
read crash; in-range head with garbage tick = interpolating a garbage pose AND
`roomsCopy`ing garbage snapshot rooms into `prop->rooms` → `propRegisterRooms` /
`portal00018148` index with them (wild write, lib_17ce0.c:214). A host-side kick
never crashed because `netDisconnect` clears `g_NetMode` *before* the front-end
stage loads — the voluntary-leave path loads it while still connected. **Fix:
`chrInit` (chr.c) clears `netsnaphead` + every `netsnap[].tick`** (same recycled
chrslot lesson as the GE `lastdamagetick60` reset, PORT_GOLDENEYE.md gotchas).

Defense-in-depth added during the same hunt (kept because they close real, if
narrower, corruption paths):

- **Type-confusion gate** (`netmsgSvcPropMoveRead`): `prop->chr` / `prop->obj` /
  `prop->door` alias one union slot, so `prop->chr != NULL` is true for ANY prop.
  The write side was already type-gated (`has_obj`, after a past crash); the read
  side wasn't. With co-op runtime chr FREE replication recycling syncids, a stale
  **unreliable** chr-state move (`flags` bit 4) arriving after its chr's syncid was
  reused by a weapon/obj prop wrote a full chrdata of state (the `netsnap` ring at
  `chr+0x3d4`+) over a much smaller objdata — trashing neighbouring stage-pool
  allocations. Both the chr-state apply and the obj/projectile section are now
  gated on `prop->type` (mirroring the writer); wire bytes are still consumed so
  the stream stays aligned. Mismatches log "recycled syncid?" warnings (tick-throttled).
  Reliable-channel chr messages (`SVC_CHR_DAMAGE`/`_FIRE`/…) don't need the gate —
  they're ordered against the reliable chr FREE.
- **Room sanitization** (`netbufReadRooms`): room numbers index `g_Rooms[]`, the
  per-room prop lists (`propRegisterRooms`) and portal tables on **writes** as well
  as reads, so garbage wire rooms = memory corruption. All five read sites now
  truncate at the first invalid entry (valid = `1..roomcount-1`) and force `-1`
  termination (an unterminated 8-entry array would make `roomsCopy` overrun).
  This also covers the server side (`CLC_MOVE` rooms → `chrSetPos`).

`netChrInterpolate` keeps a logged invariant check on `netsnaphead` (and
`netChrRecordSnapshot` re-seats a corrupt head) so any *future* corruption logs
and skips instead of crashing or silently freezing.

### Co-op Drop-in (proto 61)

Mid-mission co-op join/leave/rejoin. Net co-op always allocates
`NET_COOP_MAX_SLOTS` (4) player slots (`netCoopEnterStage`) so every machine
keeps the deterministic prop/syncid layout; unbound slots park dormant
(`player.isdormant`, dead+hidden, `netCoopDormantInit` from
`netSyncIdsAllocate`). A joiner's `CLC_STAGE_READY` triggers the JIP snapshot
then `netServerCoopClaim`: name-reserved slot first (reclaim) else first
dormant; seat via `netCoopSeatClient` (shared with the client-side apply),
revive via `dostartnewlife` → `playerStartNewLife` + spawn force-snap;
broadcast **`SVC_COOP_CLAIM 0x54`** `{clientid, playernum, name, bodybit}`
(`clientid = NET_NULL_CLIENT` = release → slot parks dormant, reserved under
the leaver's name). `SVC_PROP_RECONCILE` also lists **chr** syncids in co-op
so a joiner's fresh-loaded NPCs that the host already freed get reaped
(`CHRHFLAG_DELETING`). Gotchas: NO mid-mission slot-0 swap — the claimant
stays at wire slot N (lv.c render rollback keys on the local order now;
contpads restored via `netRestoreLocalProfile`); the all-out mission-fail
loop and respawn-input reads skip `isdormant` slots. See
`docs/PORT_COOP_DROPIN.md`.

### JIP v2 — Combat Sim catch-up + spectate-while-waiting (proto 59)

A mid-match joiner (JIP) is seated as a spectator and spawns at the next round
boundary (`mpStartMatch` clears `jip_pending_unspectate`). v2 makes the wait
correct and watchable:

- **Catch-up snapshot** (`netServerSendJipSnapshot`, net.c): when the joiner's
  world exists (it sends `CLC_STAGE_READY` from `netSyncIdsAllocate`), the
  server replays, to that client only: runtime-spawned weapon/obj props
  (`syncid >= g_NetFirstDynamicSyncId`, recorded by `netSyncIdsAllocate`) via
  `SVC_PROP_SPAWN`; doors off their default state via `SVC_PROP_DOOR` (which
  now carries `frac` — an open-and-idle door is mode IDLE + frac=maxfrac, so
  mode alone can't reproduce it); and full lift state via `SVC_PROP_LIFT`.
  Everything else heals via the existing 1s heartbeats (score, player stats,
  prop-reconcile, KoH, timescale). Log line: `NET: JIP snapshot to client …`.
- **Auto-spectate**: a playerless JIP client auto-engages the client-spectator
  redirect on the first live PLAYER target (`netSpectateAutoUpdate` playerless
  branch; sims are excluded — the sim camera path needs a local player). The
  lvRender redirect gained a `jipspec` variant that substitutes on the LAST
  render order (no own viewport iteration exists) and forces the full-screen
  viewport via `playerGetViewport*`.
- **Leaver pawn**: a Combat Sim client disconnecting mid-match has its pawn
  killed through `playerDie(true)` (the kill-plane path) in
  `netServerEvDisconnect`, so it corpses + drops weapons instead of standing
  as an orphan statue. Co-op leavers are excluded (pawn kept for future
  reclaim-on-rejoin).

### Server Weapon / Function Bans + fn-flag sync (proto 58)

Playlist `[server]` `banned=` key: whole-weapon bans (slot → `MPWEAPON_NONE`,
the pad never spawns) and `:pri`/`:sec` function bans (OR'd into
`g_MpSlotFnFlags[]`, the weapon-preset FNFLAG system). Enforced once per match
by `playlistApplyWeaponBans()` (playlist.c), called from `mpStartMatch` after
the option strips / weapon re-roll — so preset picks, admin pushes and lobby
changes are all filtered before pads spawn and before `SVC_STAGE_START` ships
the slots. **Proto 58 also syncs `g_MpSlotFnFlags[6]` right after the weapons
block in `SVC_STAGE_START`** (closing the documented preset fn-flag client
gap — clients now enforce the same `bgun*FunctionDisabled` gates locally).
`/playlist list` prints the active bans. See `docs/PORT_SERVER_WEAPON_BANS.md`.

### Slow Motion / Combat Boost Sync (`SVC_TIMESCALE`, proto 57)

PD's slow motion (the Combat Sim option and the Combat Boost pickup) works by
shrinking `g_Vars.lvupdate240` in `lvTick`. Under netplay that was dead:
`detPinTimestep` pins the sim step to a fixed 4 (1/60) *after* lvTick's
halving, overwriting it every frame. (It was also broken in **local** play at
high fps — plain integer halving of a delta that's mostly 1 does nothing; fixed
separately with a remainder-carrying halver, `lvSlomoScaleTick` in lv.c.)

Mechanism:
- `lvTick` (server/local) decides engagement each frame → `g_LvSlomoEngaged`
  (slow-mo option active, SMART proximity, or combat boost — the same branches
  that cap `lvupdate240`). When the tick pin is active (`detTickPinActive()`),
  the lvTick-level halver stands down and `detPinTimestep` halves the **pinned**
  step instead (4 → 2; remainder-carried for odd `/forcetick` steps).
- **The `g_NetTick` cadence is unchanged** — still one tick per real 1/60s on
  both machines. Only the sim time advanced per tick halves, so interp /
  lag-comp / CSP (which all time in ticks) are unaffected.
- Server mirrors the flag to clients via `SVC_TIMESCALE` `{engaged:u8,
  speedpillwant:u8, speedpilltime:s32}` — on change + a heartbeat heal at phase
  50 (reliable channel, `netEndFrame`, the `g_StageFlags` pattern). Clients
  apply it in `netmsgSvcTimescaleRead` (the wire owns `g_LvSlomoEngaged` on
  clients; their own lvTick decision is gated off). The speedpill fields keep
  the client's boost HUD meter / activation transition cosmetically in sync.
- Resets: `lvReset` zeroes the flag every stage load (both roles);
  `netDisconnect` zeroes flag + last-sent so a client leaving mid-slow-mo
  doesn't stay at half tick rate offline.
- Transition skew is bounded by RTT (client engages a few ticks after the
  server); CSP smooth-correction absorbs the resulting small position error.
- **The dead-file / fps-scaling root cause (fixed):** `src/lib/main.c` is NOT
  COMPILED into the port build — `port/src/pdmain.c` replaces it wholesale,
  and pdmain's `mainTick` runs `lvTick` once per RENDER frame with no
  fixed-step loop (the `mainnsteps` accumulator only ever existed in the dead
  file). So the netplay pin made sim speed scale with fps/60: at a 116fps VRR
  cap the net sim ran ~1.93× real time, and the slow-mo halving yielded
  ~0.97× — "slow motion looks like normal speed". Fixed by moving the
  real-time accumulator INTO `detPinTimestep`: whole 1/60 steps are emitted
  at real-time rate; render-only frames pin `lvupdate240 = 0` (paused-frame
  semantics). This also makes net sim speed correct at any fps and makes
  `/forcetick` behave as documented for the first time. NOTE:
  `detFrameBegin`/`detEndTick` (record/replay hooks) are still only called
  from the dead file — det record/replay needs rewiring into pdmain.c.
- **The lvupdate240rem gotcha (the "repeats one tick" softlock, fixed):**
  `detPinTimestep` used to zero `g_Vars.lvupdate240rem` every frame. With the
  halved step of 2, lv.c's derivation `lvupdate60 = (step + rem) >> 2` then
  yields **0 forever** — every integer-tick consumer (timers, anim fullticks,
  match clock) freezes while rendering continues. The pin now zeroes the
  remainder only for 4-aligned steps; non-aligned steps (slow-mo's 2, odd
  `/forcetick` rates) accumulate naturally so `lvupdate60` alternates 0/1 and
  averages correctly. Never reintroduce an unconditional `rem = 0` there.
- **SLOWMOTION_SMART uses a distance test in net games** (`lvTick`,
  `LV_SMART_SLOMO_RANGE` = 1500 units): the vanilla test ("player A's room on
  player B's screen", `g_MpRoomVisibility`) needs per-player render traversals
  that are stale/garbage for remote players on the server (and the bitmask is
  only 4 players wide), so SMART never fired in net games. Splitscreen keeps
  the vanilla room-visibility test for player pairs.
- **SMART also counts simulants now** (`lvSlomoSimNearby`, both local and
  net): vanilla only ever checked human players, so a 1-human + bots match —
  the common Combat Sim case — could never engage SMART at all ("activates if
  an enemy chr is nearby" was the stated intent, but chrs were never checked).
  A living sim within `LV_SMART_SLOMO_RANGE` of a living player engages it.
- Known gap: on a **dedicated** server the boost activation transition
  (`speedpillchange`, advanced in `lvRender`, `currentplayernum == 0`) never
  runs headless, so a *combat-boost* pickup won't flip `speedpillon` there;
  the slow-motion *option* path doesn't depend on it and works (user opted to
  keep dedicated unfixed for now — listen servers only).

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

Dump rate for `pos_cl`/`pos_sim` is controlled by `Net.Debug.LogRate` (default 6 ticks ≈ 10 Hz; set to 0 to disable position dumps entirely while keeping event lines). Event/bracket lines are flushed immediately so a crash never loses the lines that bracket it; the high-rate `pos_cl`/`pos_sim` dumps are left in the stdio buffer (flushed on the next event line, or on close) to cut synchronous `fflush` load on the game thread.

The log is greppable / spreadsheet-importable. For teleport hunting: filter to `pos_cl,id=N` for a specific client, diff consecutive `x/y/z`, sort by delta magnitude. For lag-comp validation: cross-reference `lagcomp` and `pos_cl` entries around the same `tick`.

### Player Animation Sync (`net.c`, `netmsg.c`, `bondmove.c`)

`netplayermove` extended with `s16 animnum; s16 animframe`. Captured in `netClientRecordMove` from `pl->prop->chr->model->anim->{animnum, framea}` (0 if any pointer in the chain is null).

Applied in `bmoveProcessRemoteInput` (the remote-player input path, only called when `pl->isremote || controlmode == CONTROLMODE_NA`): if `g_NetMode == NETMODE_CLIENT` and the incoming `animnum != 0` and differs from the local chr's current `animnum`, call `modelSetAnimation(chr->model, animnum, flip, animframe, 1.0f, 0.0625f)`. Same-anim ticks are left alone so we don't fight the local chrTick frame advance — only divergences trigger a snap.

The anim fields are deliberately excluded from `netClientNeedMove`'s change-detection memcmp (the framea field ticking every frame would otherwise force a send on every tick and undo the update-rate gating).
