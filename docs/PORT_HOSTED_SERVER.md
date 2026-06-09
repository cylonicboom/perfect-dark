# Host Online Game — master-hosted dedicated instances (port-net-predict)

Port-only feature: a player without a forwarded UDP port can "host" a game by
having the **master server VPS spawn a dedicated instance for them**. The
requester auto-connects to the instance as a client with auto-granted admin
rights and drives the **full Combat Sim hosting UI** — the genuine advanced
setup menus, not the lightweight Admin: Match Setup dialog. Joiners connect
directly to the VPS instance (no packet relaying); everyone's ping is to the
VPS.

Companion docs: [`PORT_MASTER_SERVER.md`](PORT_MASTER_SERVER.md) (master wire
protocol, including the HOST_* opcodes), [`PORT_ADMIN_CONTROL.md`](PORT_ADMIN_CONTROL.md)
(the admin machinery this rides on), [`PORT_ADMIN_GUI_CONFIGURE.md`](PORT_ADMIN_GUI_CONFIGURE.md)
(the crash-class history that shaped the menu-entry design).

No `NET_PROTOCOL_VER` bump: only the PDMS master protocol gained opcodes
(0x06–0x08); every game↔game message is reused unchanged (`CLC_ADMIN`,
`CLC_ADMIN_SETUP`, `SVC_STAGE_START`, `SVC_STAGE_END`, `SVC_ADMIN`).

---

## End-to-end flow

```
player                         pdmaster (VPS)                 instance (pd-server)
  |--- HOST_REQUEST 0x06 --------->|
  |    (proto, name, max, joinpw)  |-- spawn --dedicated --port P
  |                                |         --admin-password <token>
  |                                |         --master 127.0.0.1 ...
  |<-- HOST_GRANT 0x07 ------------|
  |    (addr "pubip:P", token)     |<======= HEARTBEAT (loopback) ========|
  |                                |  (master substitutes its public IP)  |
  |--- netStartClient(addr) ------------------------------------------->|
  |--- CLC_ADMIN "login <token>" + "take"  (reliable, ordered) -------->|
  |--- fresh-lobby reload, full Combat Sim setup opens locally          |
  |--- Begin Match -> CLC_ADMIN_SETUP ---------------------------------->|
  |<-- SVC_STAGE_START (normal client path; joiners too) ---------------|
```

## Game-side pieces

- **Transport** (`port/src/net/netmaster.c`, `netmaster.h`):
  `netHostRequestOpen/Tick/Close` — a standalone non-blocking UDP socket (the
  server-browser pattern; the request happens before any `g_NetHost` exists).
  HOST_REQUEST retransmits every 3 s; ~20 s hard timeout → `NETHOSTREQ_ERROR`
  ("No response from master server" — also what an **old master** that drops
  the unknown opcode produces). Results in `g_NetHostGrantAddr` /
  `g_NetHostGrantToken` / `g_NetHostDenyReason`, state in
  `g_NetHostRequestState`.
- **Menu** (`port/src/net/netmenu.c`): Network Game → **Host Online Game** —
  Server Name / Max Players / Password (the same backing globals as the local
  host menu, so they persist consistently) → **Request Server** → wait dialog
  (`netHostOnlineWaitDialogHandler` pumps the tick). On grant: join password
  copied to `g_NetJoinPassword` (the instance was spawned with `--password`),
  token stashed in `g_NetAutoAdminToken`, `g_NetHostOnlineMode = 1`,
  `netStartClient`, then the ordinary Joining dialog.
- **Auto-admin handshake** (`netHostOnlineJoiningTick`, pumped from the Joining
  dialog's `MENUOP_TICK`): at `CLSTATE_LOBBY`, sends `CLC_ADMIN "login <token>"`
  then `"take"` — both on the reliable **ordered** control channel, so no
  SVC_ADMIN reply parsing is needed (replies are console text). ~0.5 s later it
  enters the hosting UI.
- **Hosting UI entry** (`netHostOnlineEnterSetup`, netmenu.c): reloads a fresh
  CITRAINING world (the `netDisconnect`-return sequence: `titleSetNextStage` +
  `setNumPlayers(1)` + `mainChangeToStage`) and arms the **post-match menu
  latch** (`var80087260 = 3`). `menutick.c`'s "returning from a multiplayer
  match" block then opens the advanced-setup Combat Sim root on the fresh world
  — with two Host Online twists: a one-shot `mpsetupCopyAllFromPak()` +
  `mpsetupLoadCurrentFile()` (the same load `menuhandlerHostStart` runs;
  `g_NetHostOnlineSetupLoad` latch), and **no** auto-pushed "Joining Game..."
  waiting dialog (that's for ordinary clients — the Host Online admin IS the
  host). Sims are preserved across matches like on a listen server
  (`chrslots & 0xff00` kept).
- **Begin Match interception** (`src/game/menutick.c`, the `-5` sentinel,
  `#ifndef PLATFORM_N64`): instead of the local `mpStartMatch()`, a Host Online
  admin sends `netClientSettingsChanged()` (so its own name/body edits reach
  the server first) + `netAdminPushStart()` (`CLC_ADMIN_SETUP`), stamps
  `g_NetHostOnlinePushTick`, and `menuStop()`s. The server validates
  admin-in-control + lobby, commits, runs `mpStartMatch` → `SVC_STAGE_START`
  brings this client (and all joiners) into the match through the completely
  normal client path.
- **Push watchdog** (`net.c` `netStartFrame`): if the `SVC_STAGE_START` never
  arrives within ~10 s of a push (rejected — control lost, server error), the
  client is returned to the setup UI instead of being stranded in an empty
  lobby world. The stamp is cleared on `CLSTATE_GAME` so it can't misfire on
  the post-match lobby return.
- **Post-match loop**: `SVC_STAGE_END` → MP endscreens → the vanilla
  `MENUROOT_MPENDSCREEN` close path reloads CITRAINING + re-arms the latch →
  the same latch block re-opens the full setup (no waiting dialog) → configure
  → Begin → push. Indefinitely.
- **Session end**: `netDisconnect` clears `g_NetHostOnlineMode`, the token and
  both latches, so a later plain join doesn't auto-login or reroute Begin
  Match.

## Why the fresh-lobby reload (the crash class)

Opening the title-screen Combat Sim entry (whose setup-load runs
`mpsetupCopyAllFromPak → mpInit`) **over a live connected lobby world** is the
documented `shieldhitsTick` crash class — it failed three separate times
historically (see `PORT_ADMIN_GUI_CONFIGURE.md`). The fix here is to only ever
run that load on a **freshly reloaded** CITRAINING world (frame ≥ 4, via the
same latch the post-match return uses). Stage reloads while connected are safe
and proven: the post-match client does exactly this every match
(`lvStop` severs the `g_NetClients[i].player` bindings by design;
`netPlayersAllocate` rebinds at the next stage load).

## pdmaster side

See `pdmaster/instances.go` (manager) + `pdmaster/README.md` (ops). Highlights:

- Spawn-on-demand `os/exec`, port from `-instance-port-min/max`, capped by
  `-max-instances` globally and `-max-instances-per-ip` (default 2) per
  requester source IP.
- **Idempotency**: keyed on **(source IP, nonce)** — HOST_REQUEST carries an
  optional per-process random nonce. A retransmitted/duplicate request
  re-receives the *same* grant (never double-spawns), while a same-IP request
  with a *different* nonce (another player behind a shared/CGNAT address) gets
  its **own** instance instead of the first player's admin token. Nonce-less
  legacy clients keep the old strict per-IP behaviour among themselves.
- **Public-IP substitution**: instances heartbeat from loopback (told
  `--master 127.0.0.1`); `handleHeartbeat` re-keys them under `-public-ip` so
  the browser list and HOST_GRANT addresses are reachable, and exempts them
  from the per-source-IP flood cap (they all share 127.0.0.1).
- **Lifecycle**: killed when the owner never connects within ~2 min of the
  grant, or after ~5 min empty *after having had players* (a populated game
  survives its owner leaving). Process exit frees the port + directory entry.
- Spawned with **no playlist**: the dedicated auto-start/vote machine requires
  `g_NetPlaylist.count > 0` (and no admin holding control), so the instance
  idles in the CITRAINING lobby until the owner pushes a match.
- The instance advertises normally and appears in everyone's Server Browser.

## Security caveats (same trust model as the rest of netplay)

- The admin token is random (128-bit hex) but travels in **plaintext UDP**
  (HOST_GRANT, then `CLC_ADMIN login`) — ENet is unencrypted. Same caveat as
  `Server.AdminPassword` in `PORT_ADMIN_CONTROL.md`: access gating, not strong
  security.
- The token rides the instance's **argv** (`--admin-password`), visible in
  `ps` on the VPS. Acceptable for v1 (anyone who can run `ps` there owns the
  box anyway); an env-var handoff is the noted hardening follow-up.
- HOST_REQUEST source IPs can be spoofed to burn instance slots (the grant
  goes to the spoofed IP, never connects, reaped in ~2 min). The per-IP cap +
  max-instances + reaper bound the damage.
- Two players behind one public IP (CGNAT/household NAT) get **separate
  instances with separate admin tokens** — the request nonce keys the grant,
  so a neighbour's re-request can no longer receive your token. The nonce is
  a disambiguator, not a secret; the admin token remains the credential.

## Known limitations (v1)

- **Backing fully out of the setup menus** leaves you connected in the CI hub
  with no Host Online UI; `/admin configure` (console) is the escape hatch, or
  disconnect and re-request — the per-IP idempotent grant returns the same
  instance with the same token while it's alive.
- **Owner rejoin** after a disconnect works by re-running Host Online Game
  (same instance + token come back while it lives), but there's no dedicated
  "Reconnect" button. The grant is keyed on a **per-process** nonce, so this
  works from the same game run; after restarting the game a re-request gets a
  *fresh* instance (the old one reaps once empty / if it never had players).
- The master is **IPv4-only** (matches the existing pdmaster).

## Local test loop (no VPS needed)

`--dedicated` works in a normal Windows build (`--dedicated-windowed` keeps the
window), so the whole loop runs on one machine:

```sh
cd pdmaster && go build -o pdmaster.exe .
./pdmaster.exe -instance-bin /path/to/pd.x86_64.exe -instance-rom /path/to/pd.ntsc-final.z64 \
    -public-ip 127.0.0.1 -instance-port-min 27101 -instance-port-max 27104 -max-instances 4
```

Game with `--master 127.0.0.1` → Network Game → Host Online Game → Request.
Watch pdmaster log the spawn + grant; the game auto-connects, auto-logs-in
(console shows the SVC_ADMIN replies), reloads into the full Combat Sim setup;
configure and Begin Match. A second game instance (also `--master 127.0.0.1`)
sees the hosted server in the browser and joins directly. `go test ./...` in
`pdmaster/` covers grant idempotency, caps, deny reasons, reap rules, and the
heartbeat substitution; `curl 127.0.0.1:8080/status` shows managed instances.
