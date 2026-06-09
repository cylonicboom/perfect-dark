# Host Online Game — Findings & Fix Catalog (networking-online-review)

Companion to [`PORT_HOSTED_SERVER.md`](PORT_HOSTED_SERVER.md) (the feature design).
This document catalogs every defect found and fixed while bringing the
master-hosted dedicated-server flow ("Host Online Game") to a working state,
2026-06-07 → 2026-06-08. **Most findings are NOT host-online-specific** — the
hosted topology (a headless spectator server where *every* player is a remote
client at slot ≥ 1, running as a third process on the test machine) acted as a
stress test that exposed latent bugs in the dedicated-server path, the port's
slot-0 assumptions, and the netcode's clock model. All fixes are on
`networking-online-review`, uncommitted at the time of writing.

**End state: hosted play verified working by runtime test** — host → match →
quit → reconfigure → new match loops cleanly; both clients have full input;
no teleporting; direct listen-host play unchanged-perfect.

---

## 1. The headline fix: net clock rate (R4) — affects ALL netplay

**Symptom:** in hosted play, BOTH players teleported continuously and weapons
looked like they reloaded non-stop; direct listen-host play on the same build
was perfect.

**Root cause:** `g_NetTick` advanced **+1 per `netStartFrame` call** (one per
rendered frame), not per 1/60 of real time. Any machine sustaining <60fps
loses net-ticks against its peers **without bound**. The headless instance ran
at ~57fps (3 processes on one box) → its clock drifted −1 tick/sec, −396 ticks
over a 6.5-minute match (instance diag: `tickdrift` −31 @ 8s → −396 @ 393s).
Once the clock gap exceeded the staleness window (`g_NetStaleSnapshotTicks`,
~0.5s), every rebroadcast looked stale to receivers and
`bwalkUpdateRemote`'s stale-snapshot **snap** path (a designed-rare fallback)
fired continuously → constant teleports of all remote pawns + stale-move anim
replays. The defect was **documented but deliberately unfixed**: the perf
review's R4 comment in `netStartFrame` shipped a drift *monitor* marked
"DIAGNOSTIC ONLY (no behaviour change)".

**Fix (net.c `netStartFrame`):** `g_NetTick += diffframe60` (min 1) — the
engine's own elapsed-real-time step measure (the sim and audio loops already
consume it). The net clock now tracks real time at any framerate on every
machine. Tick stamps stay monotonic; consumers already tolerate gaps.

**Why the direct setup never showed it:** both windowed clients genuinely hold
60fps. The CSP/interp/lag-comp stack was never stressed against a peer whose
clock *rate* differed.

**Follow-ups (open):**
- Exact-phase heartbeat checks (`g_NetTick % INTERVAL == phase` for lobby
  state, KoH keepalive, timescale heal, prop reconcile, …) can now *skip* a
  phase on a multi-step frame → heal rates halve on sub-60fps machines.
  Tolerable, but consider `>=`-style phase tracking.
- The lag-comp ring (`lagcomp[120]`) indexes by tick; skipped ticks leave
  1-tick-stale entries. Cosmetic for rewinds.
- WHY the headless instance only made ~57fps at 9% CPU was never diagnosed
  (pacing interplay between `headlessPace`'s absolute schedule, the
  `mininc60` cycle gate in `mainLoop`, and `diffframe60` accounting. The
  clock fix makes the rate harmless, but a slow instance still halves its
  send rates).
- **Likely also explains** `PORT_DEDICATED_SERVER_TRIAGE.md` Finding 1
  (2026-05-29): the un-root-caused "17-second CLC_MOVE stall" has the same
  signature as accumulated clock drift crossing the staleness window.

## 2. The slot-0 assumption family — affects every dedicated-server client

On a spectator-host (dedicated) server there is **no slot-0 swap**: client N
plays at local slot N (`netPlayersAllocate` skips the spectator host when
assigning playernums). The port repeatedly assumed "the local player is slot
0" — true for listen hosts and splitscreen only. Five sites fixed:

| # | Site | Symptom | Fix |
|---|------|---------|-----|
| 1 | `mpReset` (mplayer.c): `config[i].contpad1 = i` | Slot-1 client read **pad 1** (= a physical gamepad; K/M is pad 0) → frozen pawn, or raw-gamepad-only control ("N64 1.1 feel"). The existing `netRestoreLocalProfile` heal ran in `netPlayersAllocate` but `mpReset` runs **after** it in `mainLoop`'s stage init and re-stomped. | `netMpConfigFixLocalPads(slot)` called per-slot from mpReset; pins the local seat's `contpad1` to **0 always** (the port's local input only ever arrives on pad 0); `netRestoreLocalProfile` hard-pins too. |
| 2 | `bondmove.c:1066` `allowmlook = (currentplayernum == 0)` | Mouse aim dead for slot ≥ 1 clients (kbd/pad fine — they route via contpad). | `netPlayerOwnsMouse()` (net.c): net = the local non-remote pawn at any slot; non-net = player 0. |
| 3 | `player.c` Slayer rocket mouse gate | Same class. | `netPlayerOwnsMouse()`. |
| 4 | `bondeyespy.c` eyespy mouse gate | Same class. | `netPlayerOwnsMouse()`. |
| 5 | `hudmsg.c:1031` port guard `if (g_NetMode && currentplayernum != 0) return;` | Slot ≥ 1 clients received **no HUD messages at all** (weapon pickup toasts included). | Keyed on `currentplayer->isremote` instead. |
| 6 | `player.c` `playerSndStart`: `if (g_NetMode && playernum != 0 ...)` routes through `psGetTheoreticalVolPan` | The slave's **own first-person sounds were 3D-attenuated to silence** — all pickup stings (weapon/ammo/mine/knife/keycard/**shield**) via the `propobj.c` pickup helpers, plus the choke/strangle sound (`chraction.c:4081`). HUD pickup text showed (site 5 fixed) but the sound didn't — the symptom that exposed this. | Keyed on `pl->isremote` instead of `playernum != 0`. One chokepoint fixes every caller. |
| 7 | `music.c:524` `musicStartMpDeath`: `if (... \|\| (g_NetMode && currentplayernum != 0)) return;` | The slave heard **no MP death sting on its own death** (slot N≠0 early-returned); a slot-0 *remote's* death would have wrongly played it. | Keyed on `currentplayer->isremote`. |

**Lesson:** when any per-player feature dies only for dedicated-server
clients, grep `currentplayernum == 0` / `!= 0` first (and `playernum != 0` in
shared helpers). Co-op drop-in claimants (bind at wire slot N) benefit from the
same fixes. Sites 6–7 found 2026-06-08 while the user runtime-tested pickups —
the HUD-vs-sound split (site 5 fixed, site 6 not) is the tell that a *second*
slot-0 gate sits on the same event. NOT the same bug: `lv.c:1986`
(`currentplayernum != 0`) is co-op/anti shared-viewport render restore, gated
on `playerHasSharedViewport()` — never runs in hosted Combat Sim.

## 3. Stale-input replay family (the "constant reloading")

Two independent drivers, both fixed:

- **Server rebroadcast carried one-shot input bits forever** (net.c outmove
  composition): the newest *received* move's ucmd bits were OR'd into every
  outgoing rebroadcast; an idle client sends no new moves (change-detection),
  so a stale RELOAD tap re-applied on observers once per rebroadcast move,
  indefinitely. Fix: one-shot bits (`RELOAD`/`SELECT`) forwarded **once per
  received move** (`cl->oneshot_fwd_tick`, local state, no proto bump).
  **Gotcha discovered en route: `UCMD_SELECT_DUAL` is HELD state** (set while
  dual-wielding), not a tap — it must stay in the level-carried set
  (`FIRE|AIMMODE|EYESSHUT|SELECT_DUAL`) or it flaps in the stream.
- **Observer weapon-switch apply ran per-frame, not per-move**
  (`bmoveProcessRemoteInput`): `UCMD_SELECT → bgunEquipWeapon` was not gated
  by `handled` (the reload apply was), and the server's rebroadcasts carry
  SELECT for the whole ~1s switch window (`switchtoweaponnum` span) — the
  equip anim restarted every frame ("constantly reloading" viewmodel), and
  forever on a stale move (death idle). Fix: `!handled` gate + only equip when
  the target differs from both the held weapon and the in-flight
  `switchtoweaponnum`.

## 4. Crash/hang fixes

- **Drain-loop NULL-host crash (net.c `netStartFrame`)**: a client-side
  DISCONNECT event mid-drain runs `netClientEvDisconnect → netDisconnect`,
  which destroys `g_NetHost` — the multi-event drain loop then handed NULL to
  `enet_host_check_events` (AV read at +0x58, `&host->dispatchQueue`). ANY
  rejection (files/version/password/kick) crashed the client. Fix:
  `if (!g_NetHost) break;` after the event switch. (The old
  one-event-per-frame loop never hit it — the destroy landed between frames.)
- **`playerReset` uninitialized `rooms[]` — eternal collision walk** (caught
  live in gdb): `rooms[8]` is only written inside `if (g_NumSpawnPoints > 0)`;
  a stage whose intro defines no `INTROCMD_SPAWN` (the CI lobby — playerReset
  zeroes the count and parses `g_StageSetup.intro` itself) passed pure stack
  garbage to `cdFindGroundInfoAtCyl`. Boot survives by stack luck; the
  dedicated instance's post-endmatch lobby reload reliably formed a wild rooms
  list (`roomnum=-32768`) → `cdCollectGeoForCylFromList` walked garbage
  geometry forever → **server wedged deaf after every endmatch** (looked like
  "second Begin Match hangs"). Fix: port-guarded `rooms[0] = -1` (empty
  terminated list = benign no-hit) before the spawn block.

## 5. Auth / wire fixes

- **Mod-dir auth compared resolved ABSOLUTE paths** (`fsGetModDir()` returns
  `fsFullPath()` output; netmsg.c strcasecmp'd it raw): could never match
  across installs, mismatched even locally on path-form differences, and
  leaked the local filesystem path (often a username) to the server, the
  master, and every browsing client. Fix: new `netModDirName()` (basename
  only) used in `CLC_AUTH` write/read and the query-summary/heartbeat.
  **Both sides must run this build** (one sends basename, old sends path).
- Server-side `CLC_ADMIN_SETUP` rejects (`not admin` / `not in lobby`) were
  silent in the server log (`netAdminReply` only *sends* to remote admins) —
  both now `sysLogPrintf`.

## 6. Host Online flow fixes (feature-specific)

- **Admin's "End Game" disconnected instead of ending the match**
  (`menuhandlerMpEndGame` took the plain-client `netDisconnect` branch →
  main menu, dead session). Fix: host-online admin sends `/admin endmatch`
  (server ends match for everyone, `SVC_STAGE_END`), stays connected; the
  post-match latch returns them to the Combat Sim setup.
- **Begin-Match-after-endmatch race**: the (AIO-modded) instance takes ~5s to
  reload its lobby; a quick second push hit the silent "end the current match
  first" gate. Fix: the push watchdog **re-pushes every ~3s (×5)** while in
  `CLSTATE_LOBBY` (idempotent — the server starts at most once) before
  falling back to the setup UI.
- **Endscreen ESC appeared dead**: vanilla `menutick.c` "press B to re-open
  the endscreen" (an N64 multi-local-player review feature) re-pushed the
  scoreboard the same frame ESC closed it. Fix: skip the reopen under
  `g_NetMode` (one local player — dismissal should advance).
- **`--log` was a boolean** (always `pd.log`) — pdmaster's per-instance log
  path was silently ignored and the instance shared/clobbered `pd.log` with
  the clients. Fix: `--log [path]` takes an optional path.
- **pdmaster orphan instances**: Ctrl-C'ing pdmaster orphaned spawned
  instances, which kept their UDP port bound and answered later sessions with
  a stale config/token ("files differ" forever). Fixes: pdmaster kills its
  children on SIGINT/SIGTERM (`killAll`), and **probe-binds** each UDP port
  before granting (`udpPortFree`) so squatters are skipped.
- pdmaster `-instance-args` (whitespace-split passthrough) for mod flags —
  the game's auth requires the instance's `--moddir` to match the clients'.
- Instances are spawned with a deliberately nonexistent `--playlist` (a
  save-dir `server_playlist.ini` would otherwise auto-start a rotation before
  the owner connects) and their own `--log pdinst_<port>.game.log` +
  `--netdiag pdinst_<port>.diag.csv`.

## 7. Diagnostic playbook (hard-won)

- **`--netdiag` persists as `Net.Debug.LogPath` in pd.ini** → every process
  without an explicit flag writes the SAME diag file with independent offsets
  (mutual clobbering — poisoned two rounds of forensics). Always pass an
  explicit, unique `--netdiag` per client; instances self-override now.
  Diag files **truncate at every host/join** — collect before re-running.
- **Crash symbolication:** module offsets in `pd.crash.log` +
  `addr2line -f -C -e pd.x86_64.exe (ImageBase 0x140000000 + offset)`.
- **Live hang diagnosis:** `gdb -p <pid>` works from MSYS2 on the running
  instance (debug build). `bt` alone shows gdb's injected break-in thread —
  use `info threads; thread 1; bt`. Pick the *windowless* pd process
  (`Get-Process` + `MainWindowTitle`). Error 87 on attach = dead PID.
- **Liveness probes:** `tools/query.py [--details] 127.0.0.1:<port>` (PDQM),
  pdmaster `curl 127.0.0.1:8080/status` (heartbeat age = is the instance's
  netEndFrame alive; `instances` array = manager state).
- **Diag event semantics:** `bwalkrem_exit_force` = the snap branch of
  `bwalkUpdateRemote` (stale snapshot / FL force bits / >512u drift), 1Hz
  throttled — NOT necessarily a server force; a dead/idle player pins
  benignly. `tickdrift net/wall/drift` = net clock vs wall (now should hover
  ~0; growing magnitude = the R4 class). `npa` = netPlayersAllocate
  (`hpnum` = local seat slot, `hbound` = local player bound).
  `force_move_write` also fires client-side for the force-ACK echo.
- The Windows client build is a **GUI app — stdout goes nowhere**; only
  `--log` output survives. The stdout-capture file pdmaster creates
  (`pdinst_<port>.log`) is expected to be empty on Windows.

## 8. Open items (plan candidates)

1. **Commit strategy**: items in §1–§5 are general netplay/port fixes that
   should land independently of (and before) the Host Online feature commit;
   §6 + pdmaster are the feature. The R4 clock fix affects all net modes —
   consider runtime-testing direct + co-op once more after commit split.
2. Phase-tracking for the exact-phase heartbeats (post-R4 skip class, §1).
3. Why the headless instance only paces ~57fps at 9% CPU (§1) — worth one
   instrumented run; a slow instance halves its broadcast rates even with a
   correct clock.
4. Re-test the May "Finding 1" CLC_MOVE-stall scenario on the fixed clock —
   likely closes `PORT_DEDICATED_SERVER_TRIAGE.md`'s open thread.
5. Host Online v1 gaps (see `PORT_HOSTED_SERVER.md`): no Reconnect button
   (re-request returns the same instance+token), backing fully out of the
   setup leaves no re-entry UI (`/admin configure` is the escape hatch),
   admin token rides instance argv (env-var hardening), master is IPv4-only.
6. VPS deployment: Linux `pd-server` build as `-instance-bin`, mod dirs +
   `-instance-args` on the VPS, firewall for the instance port range,
   `-public-ip`, systemd unit update, and the basename-auth **both-sides
   build requirement** for the community rollout.
7. The spectator/observer ammo model: remote pawns' local clip is simulated
   from replayed inputs and drifts; benign now (no auto-reload loops), but a
   `SVC_PLAYER_GUNS` implementation (the 0x21 stub) would make spectated
   viewmodels honest.

## 9. pdmaster deploy-readiness review (2026-06-08, follow-up session)

A full review of pdmaster against the "two players direct vs hosted" parity
question, then fixes (18/18 go tests; game side rebuilt clean). Closed:

- **LIST_RESPONSE pages could exceed the game's 1024-byte browser buffer**
  (`maxRespBytes` was 1200; bundled ENet *drops* oversized datagrams, -1/-2)
  → silently invisible servers at ~12+ entries. Now 950 + a page-size
  contract test; `PORT_MASTER_SERVER.md` corrected.
- **Orphan loopback heartbeats were published as `127.0.0.1:port`** — now
  dropped when hosting is enabled (kept when disabled: single-box test loop).
- **`pdmaster.service` was incompatible with `-instance-bin`** (children share
  the cgroup: `MemoryMax=128M` OOM, `ProtectSystem=strict` blocks writes) —
  new `pdmaster-hosting.service` + warning in the old unit.
- **CGNAT admin-token leak (security)**: grants were idempotent per source IP
  alone, so a second player behind the same public IP received the first's
  admin token. HOST_REQUEST now carries an optional trailing per-process
  nonce (`netHostRequestNonce`, netmaster.c — no NET_PROTOCOL_VER bump,
  PDMS-only); the master keys grants on (IP, nonce), caps per IP via
  `-max-instances-per-ip` (default 2). Backward compatible both directions.
- **LIST_REQUEST amplification reflector**: per-source-IP token bucket
  (burst 8, 1/s refill, fail-closed at 4096 tracked IPs, 60s idle sweep).
- **Verify-before-list** (`-verify`, default on): heartbeated servers are
  probed with a PDQM query from the master socket and only listed after a
  CRC-valid reply — spoofed registrations and unreachable game ports never
  hit the browser. Managed instances exempt. Consequence: a server with
  `Net.Server.AllowInfoQuery=0` is not listed.
- **Settings-change respawn**: a re-request with changed name/slots/password
  on a never-populated instance replaces it (params were previously silently
  ignored); populated instances keep the old grant. `waitInstance` cleanup is
  now identity-gated (also closes a pre-existing port-reuse race).
- **Learned protocol**: with `-instance-proto 0`, the master learns the proto
  from its own instances' heartbeats and denies mismatched requests with a
  clear reason (previously stale clients got an unjoinable grant).
- **Per-instance save dirs**: instances spawn with `--savedir inst_<port>`
  (dir auto-created) — isolated pd.ini/saves, no cross-instance config races,
  operator's install config untouched. Pre-seed `inst_<port>/pd.ini` to tune.
- **Heartbeat names sanitized** (control chars stripped) before the directory
  and `/status` JSON.
- **Browser mod visibility** (game side): the Details view now shows the
  server's mod dir + a "mismatch, can't join" warning (`netserverdetails.mod`
  / `.modmatch`, computed in netmaster.c's parse against `netModDirName()` —
  netmenu.c can't include netmsg.h, the bool/include-order trap). The mod was
  always in the PDQM summary; it was parsed and discarded.

**Build the dedicated target too — `strcasecmp` portability (caught at review).**
The browser mod-match (netmaster.c `netBrowserParseQuery`) used `strcasecmp`,
which compiled fine on the MinGW SDL3 client (transitive include) but **failed
the Linux `DEDICATED_SERVER` build** (`implicit declaration of strcasecmp`) —
and the instance binary IS the dedicated build, so this would have bricked the
VPS deploy. Fix: `#include <strings.h>` (POSIX; the pattern net.c / netmsg.c /
config.c / system.c already follow). **Lesson: any game-side net change must be
built against BOTH `build_debug_sdl3` (client) and `build-server-debian`
(`-DDEDICATED_SERVER=ON`, the instance binary) before deploy** — the dedicated
build has a stricter/leaner include set and a headless stub layer the client
build hides. A `cmake | tail` pipe also masks the real exit code (tail's status
wins) — grep the full log for `error:`.

**Runtime smoke (master side) PASSED** — `pdmaster/pdmaster_smoke.py` drives
the real binary over loopback UDP (sleeping-stub instance binary): 19/19 checks
across heartbeat/ACK/verify-probe, verify-before-list (incl. bad-CRC reject),
LIST rate limit, nonce grants + CGNAT separation, per-IP cap, settings
respawn, per-instance savedirs, `/status`, and killAll-on-SIGTERM. Run it
before every deploy. The in-*game* smoke (real client nonce, browser mod line)
still needs the user.

### 30Hz network rate for hosted instances (the "drop to 30 tick" ask)

`-instance-svcrate` (default **2**) spawns instances with `--svcrate 2`: the
server sends state every other tick (30Hz), ~halving per-client downstream
bandwidth — the relevant VPS cost, since each instance feeds up to 8 clients
over the box's uplink. New game-side CLI flags `--svcrate` / `--clcrate`
(net.c netInit) are the dedicated-server form of the existing `/svcrate` /
`/clcrate` console commands + `Net.Server/Client.UpdateFrames` keys (clamp
1..60). Chosen over seeding the savedir pd.ini so it can't fight an operator's
per-instance pd.ini and is visible in `ps`. Snapshots are tick-stamped so the
8-entry interp ring handles 30Hz unchanged; clients may prefer `Net.LerpTicks`
4. **Note (findings §1):** the headless instance was *pacing*-bound (~9% CPU),
not CPU-bound, so the win here is bandwidth, not CPU — a dedicated-only 30Hz
*sim* rate (option 2: a `Net.Server.SimHz` key pinning `detPinTimestep` to
`rate=30` server-side) is feasible via the same accumulator but deferred until
a populated instance is actually CPU-measured on the VPS.

Still open from the review: master restart kills live hosted matches
(state-file + re-adopt would fix); admin token on instance argv (env-var
handoff); IPv4-only; combat-boost slow-mo on dedicated; SVC_PLAYER_GUNS (item
7 above); the option-2 dedicated-only 30Hz *sim* rate (above).
