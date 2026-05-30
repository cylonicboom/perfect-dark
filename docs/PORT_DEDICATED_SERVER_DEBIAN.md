# Headless Dedicated Server on Debian (SSH / systemd) — Design & Plan

Status: **Phase 1 complete** (build-time strip validated on Linux). Living
document — update as phases land.

This describes the redesign of the Perfect Dark dedicated server for a GUI-less
Debian box driven over SSH and run as a systemd service, replacing the current
Windows `run-dedicated.bat` workflow. It builds on the existing `--dedicated`
headless mode already present on `port-net-predict`.

---

## 1. Goals & non-goals

**Goals**

- Run a Perfect Dark netplay server on Debian with **no GUI, no video, no audio,
  no input** — nothing that needs X11/Wayland/OpenGL/a sound device.
- **One codebase.** The server is the same source tree as the client, selected by
  a single CMake option. No fork, no copy.
- **Lightest practical footprint**: the server binary should not even *link*
  SDL2, OpenGL, or audio libraries.
- Managed as a **systemd service** an admin can `start`/`stop`/`enable`/`status`
  over SSH, with logs via `journalctl`.
- **Do not regress** the existing Windows/Linux/macOS/Switch *client* builds.

**Non-goals (for this work)**

- Fixing netplay simulation/sync correctness (see §10 — tracked separately in
  `PORT_DEDICATED_SERVER_TRIAGE.md`).
- Live admin tooling (kick/map-change/RCON). Designed here (§9), implemented later.

---

## 2. Architecture: one tree, a build switch

A single CMake option, **`DEDICATED_SERVER`** (default **OFF**), carves a
server-only binary out of the same sources. When OFF, *every existing build is
byte-for-byte unchanged* — the option's `#ifdef DEDICATED_SERVER` regions are
invisible to normal builds. When ON, video/audio/input and the GL/SDL backends
are compiled out.

Build matrix (same source, four artifacts):

| | Default build (`OFF`) | `-DDEDICATED_SERVER=ON` |
|---|---|---|
| **Windows** | `pd.exe` — full client (as today) | `pd-server.exe` — headless server |
| **Linux** | `pd` — full client (as today) | `pd-server` — headless server |

Relationship to the existing **runtime** `--dedicated` flag:

- `--dedicated` (runtime) sets `g_NetDedicatedMode = 1`, which already makes the
  *normal client* run headless (skips render, force-mutes audio, auto-hosts,
  installs signal handlers). This keeps working on the default client build.
- `-DDEDICATED_SERVER=ON` (build time) is the *optimized* version of that: it
  removes the GUI libraries entirely so the box needs none of them installed.
  The stripped build still honours `--dedicated` and the same CLI flags; in the
  server build we default `g_NetDedicatedMode = 1` so the flag is optional but
  harmless.

---

## 3. What `port-net-predict` already provides

The dedicated server is **not** greenfield. Already present:

- `--dedicated` (headless, mode 1) and `--dedicated-windowed` (mode 2) CLI flags,
  parsed in `main.c` before subsystem init; `--dedicated` implies `--host`
  (auto-start the server on boot).
- Server CLI in `port/src/net/net.c`: `--port`, `--maxclients`, `--playlist`,
  `--server-name`, `--master`, `--no-advertise`, `--password`, plus `--rom-file`
  and a `--netdiag` diagnostic log. Master server / browser on port 27100.
- `port/src/headless.c` — wall-clock frame pacer (`headlessPace`) and POSIX
  (`SIGINT`/`SIGTERM`/`SIGHUP`) + Windows console-control signal handlers feeding
  a clean shutdown. **POSIX is already supported.**
- Runtime headless guards already in `video.c`, `audio.c`, `input.c`, `pdmain.c`
  (gated on `g_NetDedicatedMode`), dedicated host runs as a non-combatant
  spectator with zero panels, auto-start gating on `min_humans_to_start`, etc.

So "redesign for Debian/SSH" is mostly a **deployment + footprint** layer plus a
small build-time strip — not an engine rewrite.

---

## 4. Dependency strip (what makes the binary light)

Timing is already POSIX (`gettimeofday` / `nanosleep` in `system.c`). SDL/GL are
almost entirely confined to a few files. To reach a binary that links only
**libc + zlib + ENet + stdc++/m**:

| Concern | Location | Action under `DEDICATED_SERVER` |
|---|---|---|
| SDL2 window backend | `port/fast3d/gfx_sdl2.cpp` | Exclude from build |
| OpenGL renderer | `port/fast3d/gfx_opengl.cpp` + `glad/` | Exclude from build |
| Audio device | `port/src/audio.c` | `#ifdef` the SDL body out (already runtime-guarded) |
| Input | `port/src/input.c` | `#ifdef` the SDL body out (already runtime-guarded) |
| Video backend wiring | `port/src/video.c` | `#ifdef` out the body that references `&gfx_sdl` / `&gfx_opengl_api` (those symbols won't exist) |
| Frame pacing | `port/src/headless.c` → `SDL_Delay` (1 call) | Replace with `nanosleep` / `sysSleep` |
| Exe & home paths | `port/src/system.c` → `SDL_GetBasePath`/`SDL_GetPrefPath` | Replace with POSIX (`/proc/self/exe`, `$HOME`/`getpwuid`) |
| Fatal-error dialog | `system.c` / `crash.c` → `SDL_ShowSimpleMessageBox` | Replace with stderr log |

Build wrinkle: sources are gathered by `GLOB_RECURSE port/*.c`/`*.cpp`, so the
two fast3d files + `glad/` are removed from the list via `list(REMOVE_ITEM ...)`
when `DEDICATED_SERVER` is ON, and `find_package(SDL2)` / GL are skipped and
dropped from `LIBS`.

---

## 5. Phase plan

- **Phase 0 — Baseline.** Build the normal Linux client (toolchain check), then
  build/run the existing `--dedicated` mode on Debian as-is. Proves the engine
  runs headless on Linux before anything is stripped.
- **Phase 1 — `-DDEDICATED_SERVER=ON` compile-out. ✅ DONE.** CMake option + the
  source changes in §4. Validated on Linux x86_64: the server binary
  (`pd-server.x86_64`) links only `libz/libstdc++/libm/libgcc_s/libc` — **no SDL,
  OpenGL, X11 or audio** (`ldd` confirmed); it boots headless, resolves paths via
  POSIX (no SDL), and reports fatal errors to stderr (no GUI dialog). The default
  client build (`-DDEDICATED_SERVER=OFF`) is unchanged and still links SDL2/GL.
- **Phase 2 — Debian build & data.** apt deps, documented build recipe, ROM/asset
  placement, launch wrapper + sample `server_playlist.ini`.
- **Phase 3 — systemd service.** Dedicated user, install layout, hardened unit,
  journald logging, `ExecStart` from the `.bat` flags (§7–8).
- **Phase 4 — Admin control (designed now, built later).** Control-socket seam in
  `headless.c` (§9).
- **Phase 5 — CI guard.** Add a `DEDICATED_SERVER` build job to
  `.github/workflows/c-cpp.yml` so client and server both build on every commit.

---

## 6. Build recipes

**Client (unchanged, for reference):**
```sh
sudo apt install build-essential cmake libsdl2-dev zlib1g-dev   # + GL dev
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
```

**Server (new):**
```sh
sudo apt install build-essential cmake zlib1g-dev   # NO SDL2 / GL / audio dev
cmake -B build-server -DDEDICATED_SERVER=ON -DROMID=ntsc-final -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-server -j
# -> build-server/pd-server
```

**Windows server**: the same `-DDEDICATED_SERVER=ON` build works on Windows
(MSYS2/MinGW) — a console app linking no SDL/GL (verified by cross-compile + the
CI Windows job). Build/run/service instructions: `dist/windows/server/README.md`.

## 7. ROM / data on a GUI-less box

The port loads `pd.<ROMID>.z64` (e.g. `pd.ntsc-final.z64`) from its data dir
(`fsFullPath("")`), overridable with `--rom-file <path>`. Same requirement as the
client. On Debian, place the ROM next to the server binary (mirroring the Windows
`%~dp0` layout) or point `--rom-file` at it. `scp` the ROM + `server_playlist.ini`
to the box; no GUI needed.

## 8. Operator config: `.bat` → systemd

Current Windows launch (`run-dedicated.bat`):
```bat
pd.x86_64.exe --dedicated --port 27100 --maxclients 8 --server-name "Perfect Dark Dedicated" --playlist server_playlist.ini
```

Maps 1:1 onto the service.

**Privilege model: never root.** Root is used only for *install* (copying files
into `/opt`, dropping the unit). The server *process* runs as an unprivileged,
service-only user. Rationale: the server is network-facing (UDP from untrusted
peers), so a netcode bug under root would be full box compromise; under a
sandboxed service user it's boxed into a throwaway account.

Default user model: **`DynamicUser=yes`** — systemd fabricates a transient
`pd-server` user at start and tears it down at stop. No manual `useradd`, no
leftover account; persistent state lives in `StateDirectory=pd-server`
(`/var/lib/pd-server`). Alternative: an explicit `useradd --system pdserver`
account if a stable UID / pre-owned files are preferred.

Layout: install root `/opt/pd-server` (binary + ROM + `server_playlist.ini`,
read-only to the service); writable state `/var/lib/pd-server`.

The **master server** (`netmaster.c`, browser/matchmaker on 27100) is an
independent service with its **own** user/unit if self-hosted; the dedicated
game server only *registers* with one via `--master` (or runs `--no-advertise`).

`/etc/systemd/system/pd-server.service` (draft):
```ini
[Unit]
Description=Perfect Dark Dedicated Server
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
DynamicUser=yes
StateDirectory=pd-server
WorkingDirectory=/opt/pd-server
ExecStart=/opt/pd-server/pd-server --dedicated --port 27100 --maxclients 8 \
          --server-name "Perfect Dark Dedicated" \
          --playlist /opt/pd-server/server_playlist.ini
Restart=on-failure
RestartSec=3
# Sandboxing
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
# StateDirectory grants RW to /var/lib/pd-server automatically
# UDP game/master port — open 27100/udp in the firewall

[Install]
WantedBy=multi-user.target
```

Admin workflow over SSH:
```sh
sudo systemctl enable --now pd-server
sudo systemctl status pd-server
journalctl -u pd-server -f          # live server log
sudo systemctl restart pd-server
```

`server_playlist.ini` is read from `--playlist` (or `$S/server_playlist.ini`
by default). The supplied playlist (11 entries across COMBAT/HTB/CTC/KOH/HTM/
POPACAP plus RANDOM wildcards, `min_humans_to_start = 1`, 20s/3-candidate votes)
works unchanged on Linux — only the path needs to resolve, which `--playlist`
makes explicit.

## 8a. Running multiple instances on one machine

Use a **single templated unit** `pd-server@.service`, enabled once per instance —
not copies of a unit. The port already exposes the flags needed for isolation:

- `--basedir <path>` — read-only install dir (ROM + assets), **shared**.
- `--savedir <path>` — writable dir (own `pd.ini`, saves, diag log, future stats),
  **per instance**. Distinct savedirs avoid any shared-`pd.ini` contention.
- `--port` / `--server-name` / `--playlist` — per instance.

```ini
# /etc/systemd/system/pd-server@.service
[Service]
Type=simple
DynamicUser=yes
StateDirectory=pd-server/%i          # -> /var/lib/pd-server/%i (persistent)
WorkingDirectory=/opt/pd-server
EnvironmentFile=/etc/pd-server/%i.conf
ExecStart=/opt/pd-server/pd-server --dedicated \
          --basedir /opt/pd-server --savedir /var/lib/pd-server/%i \
          --port ${PORT} --maxclients ${MAXCLIENTS} \
          --server-name "${NAME}" --playlist ${PLAYLIST} ${EXTRA}
Restart=on-failure
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
PrivateTmp=true
[Install]
WantedBy=multi-user.target
```

Per-instance config, e.g. `/etc/pd-server/dm.conf`:
```
PORT=27100
MAXCLIENTS=8
NAME=PD Deathmatch
PLAYLIST=/opt/pd-server/playlists/dm.ini
EXTRA=
```

```sh
sudo systemctl enable --now pd-server@dm pd-server@objective
journalctl -u pd-server@dm -f
```

Each `%i` gets its own UDP port, its own persistent `StateDirectory`, its own
**distinct DynamicUser UID** (full isolation between instances), and its own
playlist/rules; all share one read-only `/opt/pd-server`.

Sizing: each instance is a full 60 Hz sim (bots are the main CPU cost; the pacer
sleeps when idle). Budget ~1 core per busy instance; open the assigned UDP range
(e.g. `27100-27103/udp`) in the firewall.

## 8b. Per-server persistent state & stat tracking (future)

Each instance's `StateDirectory` (`/var/lib/pd-server/%i`, passed as `--savedir`)
is **persistent across restarts/reboots and naturally per-server** — the right
home for future per-server stat tracking. The storage is provisioned by the unit
today; the stat feature is code on top (write e.g. a `stats.db` SQLite file or
JSON into the savedir at match end, keyed by player; optionally surface via the
control socket in §9).

Notes:
- `DynamicUser=yes` + `StateDirectory` is supported for persistent state —
  systemd re-applies ownership to the transient UID each start, so the server
  always reads/writes its own stats. **But** the files end up owned by a floating
  system UID. If external tooling (web leaderboard, cron export, manual edits)
  will touch the stats DB directly, prefer a **fixed `pdserver` user** for stable
  ownership. This is the concrete use-case that may tip the §11 user-model choice.
- Per-server vs global: per-server stats fall out for free (one DB per
  StateDirectory). A cross-server leaderboard is additive — aggregate the
  per-server DBs on a schedule, or point instances at a shared stats path with
  concurrency handling.

## 9. Admin control (future seam)

Lifecycle is covered immediately by systemctl/journalctl. Live in-game admin ops
(kick, force map/scenario, say, reload playlist) slot in as a **control thread in
`headless.c`** listening on a Unix-domain socket (e.g.
`/run/pd-server/control.sock`, root/admin-only perms) and dispatching commands
into `net.c` / `playlist.c`. A thin `pd-serverctl` CLI talks to the socket. This
keeps admin access off the network entirely and authorized by filesystem perms.
Designed now; implemented in a later phase.

## 10. Out of scope / known issue

`PORT_DEDICATED_SERVER_TRIAGE.md` documents an unresolved "clients don't sync
with the dedicated host" bug (remote players/sims out of sync, no pickups,
respawn issues). That is a **netplay correctness** problem independent of this
deployment work — the Debian service can be built and validated around it, but a
*playable* server depends on it being fixed. Tracked separately.

## 11. Decisions & open questions

Decided:
- **Privilege:** never root. Service runs unprivileged via `DynamicUser=yes`
  (transient `pd-server` user); explicit `pdserver` account is the alternative.
- **Master server:** separate service / own user if self-hosted; the game server
  only registers with one.

Open:
- Install root: `/opt/pd-server` vs `/srv` vs `/var/games` — default `/opt`.
- Firewall: confirm `27100/udp` (game + master query) is the only port to open.
- Master advertising: keep default-on, or run private with `--no-advertise`?
