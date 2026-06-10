# Netplay Prop-Sync Soak Harness (Phase 2) — 2026-06-10

> **Status: RUNTIME-PROVEN on Windows (2026-06-10 evening): repeated unattended
> two-process soaks (headless server + headless client, one box) ran matches,
> rotated the playlist, and produced `OVERALL: PASS` with 100% manifest parity
> and zero heal/reap fires.** One-click entry point: `tools\soak\soak_oneclick.bat
> [minutes]` (default 30) — opens the server in its own window, runs the client
> in the launching console, prints both verdicts + the combined parity verdict.
>
> What the bring-up fixed (all required for the first PASS):
> - **Direct dedicated boot** (`netDedicatedBootTick`, netmenu.c, hooked from
>   `playerTickPauseMenu`'s `MENUROOT_FILEMGR` case): headless processes used to
>   stall FOREVER on the agent file-select with a fresh save dir (no agent file
>   for the `--profile` auto-select, no one to drive New Agent, and the host/join
>   latch is only consumed by the main-menu tick). Headless now hosts/joins
>   directly with `gamefileLoadDefaults` — no agent file, no menus. (A blank-slot
>   auto-create fallback also exists in filemgr.c's auto-select block.)
> - **`--savedir tools/soak/save`** in both run scripts: its pd.ini sets
>   `Game.MemorySize=256`. With no pd.ini the engine defaults to 16MB pools,
>   which crashed the 8-bot playlist's bot-body modeldef load at the first match
>   rotation (`modelPromoteOffsetsToPointers` read at -1).
> - **`timeout --kill-after=15`** in both run scripts: on MSYS2/Windows the INT
>   never reaches the native exe, so timed runs sailed past the cap; audit lines
>   are line-flushed so the hard kill loses nothing that matters.
> - **`--no-advertise`** on the soak server: it otherwise heartbeats to the
>   public master and shows up in everyone's server browser.
>
> Honest residual limits: client spawns at round boundaries only, neutral input
> (unchanged); 1-2 min windows only exercise short-soak behaviour — long-soak
> (30+ min) churn is the next milestone. One open lead from the first runs: a
> single `wire ref to syncid-0 prop (type 4)` diet-gap warning (~1 per 2 min);
> the tripwire now logs prop idx/objtype/weaponnum to identify it next run.
> Phase 2 of the prop-sync consistency plan.
>
> Pairs with `docs/PORT_NET_PROP_LIFECYCLE.md` (Phase 0/1 — the auditor checks
> the invariants that work established) and `docs/PORT_NET_CRASH_LEDGER.md`.

## Why

Every crash-ledger root cause cost days of live repro against the VPS with
bespoke throttled printfs. The binding constraint on this project isn't code
quality — it's **verification cost**. The soak harness makes the prop-sync
invariants *continuously self-checking* so a regression announces itself with a
loud, named log line in any session, and a long run reduces to one exit code.

The harness is three parts:

1. **In-engine invariant auditor** (`netprop.c`, `/audit`) — the durable asset.
   Compiled into the dedicated server AND the client; rides along in *every*
   net session, not just harness runs.
2. **Offline verdict tool** (`tools/netsoak.py`) — turns the auditor's CSV
   output into PASS/FAIL + a server↔client manifest-parity check.
3. **Orchestration** (`tools/soak/`) — a churn-maximising playlist + a
   server-launch script that wires the above together.

## 1. The in-engine auditor (`/audit`)

Once a second on both roles, `netPropAuditTick()` (called from `netEndFrame`)
runs `netPropAudit()`, which re-derives the prop-sync invariants from current
state by **iterating the prop pool directly** — it never walks the `->next`
chain, so it cannot hang on the cyclic-list corruption it's looking for (that's
`propsHealActiveList`'s job; the auditor just counts how often the heal fired).

It emits one machine-parseable line to the diag log every cycle:

```
audit: role=S result=PASS netprops=12 manifest=0x9f3a21c4 dupes=0 corpses=0 \
       overcap=0 slots_occ=18 synced=50 local=6 proj=3 projdead=0 orphan=0 \
       heal=0 reap=0 orphreap=0
```

Fields:

| field | meaning | healthy |
|---|---|---|
| `role` | `S` server/host, `C` client | — |
| `result` | `PASS` / `FAIL` for this cycle | PASS |
| `netprops` | live networked weapon/obj props | — |
| `manifest` | order-independent xor-hash of the networked syncid SET | matches peer |
| `dupes` | two props sharing a syncid (double-spawn class) | 0 |
| `corpses` | listed weapon/obj/door/expl/smoke prop with NULL union | 0 |
| `overcap` | networked syncids past the reconcile coverage cap | 0 |
| `slots_occ` / `synced` | weapon-slot occupancy / `g_MaxWeaponSlots` | occ ≪ max |
| `proj` / `projdead` | projectile-flagged slots / those whose prop is freed | projdead 0 |
| `orphan` | slots whose prop backlink doesn't point home (ledger #16) | 0 |
| `heal` / `reap` / `orphreap` | heal-layer FIRES since the last cycle | 0 |

When a cycle is not PASS the auditor ALSO prints a `LOG_WARNING` to the console
(`AUDIT FAIL …` / `AUDIT WARN …`), so a regression is visible even with no diag
file open. `standing` corruption (dupes/corpses/orphan/overcap) is a hard FAIL;
heal/reap/orphreap fires are a WARN (corruption occurred and was masked — the
heal layers kept it alive, but it's still a finding to chase).

Console control:
- `/audit` or `/audit now` — run one cycle immediately, print PASS/FAIL.
- `/audit on` | `/audit off` — toggle the per-second tick (default on).
- `/audit rate N` — cycle every N ticks (default 60 = 1 s; min 1, max 600).

## 2. The verdict tool (`tools/netsoak.py`)

```
tools/netsoak.py SERVER.csv [CLIENT.csv ...]
```

- **One log**: checks that machine's own invariants over the whole run —
  zero FAIL cycles, zero standing corruption, zero heal fires, slot occupancy
  bounded. Prints peaks and totals.
- **Server + client logs**: ALSO checks **manifest parity** — for each client
  audit at tick `T`, the client's `manifest` digest must equal the server's at
  some tick in `[T-window, T]` (clients lag the host by the reliable-channel +
  interp delay; window default 180 ticks/3 s). ≥95 % of client cycles must
  match (the odd miss is a spawn/free straddling the 1 Hz sample). A sustained
  mismatch is the "host and client disagree on which props exist" failure the
  whole sync layer exists to prevent — it pinpoints the first divergence tick.

Exit code: `0` PASS, `1` FAIL, `2` usage/no-data. Suitable for CI-ish gating
once a headless client exists.

The tool is validated on synthetic logs (healthy → PASS/0; injected dupes+heal
→ FAIL/1; divergent client manifest → FAIL/1 with the divergence located).

## 3. Orchestration (`tools/soak/`)

| file | purpose |
|---|---|
| `playlist_soak.txt` | churn-maximising playlist: 8 MEANSIM bots, launcher/mine loadouts, short 3-min rounds (frequent stage-reload cold paths), 3 maps of differing prop density. `min_humans_to_start = 1`. |
| `run_server.sh` | launches `pd-server.x86_64 --dedicated --playlist … --netdiag … --svcrate 2`; auto-starts the match when one client connects; on a timed run, prints the verdict at the end. |
| `run_client.sh` | launches the headless client (`--headless-client <addr> --netdiag …`); on a timed run prints the client-side verdict + the parity command. |
| `out/` | generated CSVs/logs (gitignored). |

## Headless client (`--headless-client <addr>`)

The headless client reuses the dedicated build's headless runtime (no window/
audio/input, 60 Hz pacing) but **joins** a server instead of hosting. It's the
second machine the manifest-parity check needs, and it makes a **fully
unattended** soak possible (server + client as two processes on one box, no GUI).

**Why it works headless.** Everything the soak depends on runs in the game-TICK
path, which executes headless; only `lvRender` is skipped (that's the point —
no visuals):
- client prop apply — `netmsgSvcPropMoveRead`/`netmsgSvcPropSpawnRead`/`…FreeRead`
  run in the net-receive path (`netStartFrame`);
- chr interpolation — `netChrInterpolate` is called from `chrTick` (chr.c), a
  tick-path function, not from render;
- client prop physics / GC — `objTickPlayer`, `propsHealActiveList`,
  `weaponSlotsReapOrphans` all run from `lvTick`/`propsTickPlayer`;
- the auditor — `netPropAuditTick` runs from `netEndFrame`.

**Honest limits (what's NOT verified / by-design degraded).** This is the novel
combo — a local *combatant* pawn on a *headless* build (the dedicated server is
always a pawnless spectator), so these seams are unproven until a real run:
- **Round-boundary respawn only.** The local-pawn spawn (`playerStartNewLife`,
  lv.c) lives in `lvRender` and is already gated off for clients
  (`g_NetMode != NETMODE_CLIENT`) — client spawning is server-authoritative via
  `mpStartMatch` at each round start (tick-path, headless-safe). So a headless
  client spawns at round start, and after death **stays dead until the next
  round** (no mid-round respawn). That's fine for a soak — prop churn comes from
  the bots, and the auditor runs regardless of the client's pawn state.
- **Neutral input** — the pawn stands still (stubbed input returns neutral), so
  it's an easy kill. That's *good* churn (frequent deaths → drops), just not
  representative movement.
- **Render-prep with a local pawn headless is unproven.** No frame is rendered,
  but if some tick-path code derefs a viewport/camera/matrix that `lvRender`
  normally primed for the local player, it could misbehave. The first real run
  (watch for crashes / `AUDIT FAIL` storms in the client log) is the test.
- **Protocol match.** The server it joins must run THIS branch's build
  (`NET_PROTOCOL_VER`), or auth is rejected `DISCONNECT_VERSION`. Pointing it at
  the live VPS only works once the VPS is on this branch.

If the headless client proves unstable, the soak still runs with a **real
(windowed) client** as the second machine — the auditor + parity work identically
either way; you just lose the unattended/CI property.

## How to run a soak — Windows (MSYS2), the current primary path

Only the VPS runs Debian, so soaks run on the Windows dev box for now. Both
processes are headless console apps, so this is just two MSYS2 shell windows.
The scripts are plain bash and run as-is in the **MSYS2 MinGW x64 shell**
(coreutils `timeout`/`tee` and python are present there); the binary name
autodetects the `.exe` suffix. Still ROM-gated — see the asset note below.

> **Shell gotcha (the #1 failure mode):** it must be the **MinGW x64** shell —
> the prompt says `MINGW64`, not `MSYS`. In the plain MSYS shell,
> `/mingw64/bin` is off PATH, so the exe's runtime DLLs (`libwinpthread-1`,
> `libgcc_s_seh-1`, `zlib1`) don't resolve and Windows kills it **before
> `main()` — instantly, with zero output and no diag CSV** (the run looks like
> it "elapsed" immediately). The scripts now preflight this with `ldd` and
> refuse to launch, and they fail loudly after the run if no CSV was written.
> (The CI artifact ships those three DLLs next to the exe for this reason; a
> local build dir doesn't have them.)

1. **Build the headless target** (MSYS2 MinGW x64 shell, repo root):
   ```
   mkdir -p build_ded && cd build_ded
   cmake -G "Unix Makefiles" -DDEDICATED_SERVER=ON -DCMAKE_BUILD_TYPE=Release ..
   make -j
   cd ..
   ```
   Produces `build_ded/pd-server.x86_64.exe` — a console app (Ctrl-C / closing
   the window is a clean shutdown). It can host OR join. CI builds this exact
   target ("Build dedicated server (x86_64 windows, headless)"). The dir name
   is yours to pick (CI uses `build-server/`); pass the binary path as the
   scripts' BINARY arg if it isn't the `build_ded/` default.
   **Assets:** provision the ROM/`data` for `build_ded` the same way as your
   other build dirs (the headless build discovers them identically).

2. **Shell window 1 — server** (30-minute capped run):
   ```
   tools/soak/run_server.sh build_ded/pd-server.x86_64.exe 27100 30
   ```
   (The first arg is optional — the default resolves the `.exe` itself.)
   Writes `tools/soak/out/server_<stamp>.csv`.

3. **Shell window 2 — headless client**:
   ```
   tools/soak/run_client.sh 127.0.0.1:27100 '' 30
   ```
   Writes `tools/soak/out/client_<stamp>.csv`. The match auto-starts as soon as
   the client connects (playlist `min_humans_to_start = 1`).
   - **Or** connect your real (windowed) client instead — console (`~`),
     `/diag <path>` (auditor is on by default), optionally `/lag 120` +
     `/loss 20` to stress the link. Use this if the headless client proves
     unstable — and note the lag/loss knobs are console-only, so link-stress
     runs need the windowed client anyway.

4. **Verdict**:
   ```
   python3 tools/netsoak.py tools/soak/out/server_<stamp>.csv tools/soak/out/client_<stamp>.csv
   ```
   (`python` works too if MSYS2 has no `python3` alias.)

### Linux / VPS (later)

The same three commands work unchanged on Debian (binary
`build_ded/pd-server.x86_64`, no `.exe`). To reproduce a **live VPS** prop
issue, run only the client side against it:
`tools/soak/run_client.sh <vps>:27100 '' 30` — but only once the VPS instance
runs this branch's build (protocol match; otherwise `DISCONNECT_VERSION`). The
VPS's own `--netdiag` CSV is then the `role=S` log for the parity check.

### What a clean run looks like
`OVERALL: PASS`, zero FAIL cycles on both roles, manifest parity ≥ 99 %, and
the console silent of `AUDIT`, `propsheal`, `proptick_guard`, `orphan_reap`,
and `netprop:` lines for the whole run.

### What to do with a failure
- `dupes>0` → a syncid is shared; `/proplog <syncid>` on the offending machine
  shows the two spawns (the lifecycle ring from Phase 1).
- `orphan>0` / `orphreap>0` → a weapon slot leaked; the `netprop: POST-FREE`
  line (if any) names the free reason; otherwise a new bare-free generator.
- `corpses>0` / `reap>0` → a freed prop left listed; `/proplog` its syncid.
- manifest parity drop → note the first-divergence tick from the tool, then
  grep both CSVs around that tick for `spawn`/`free`/`audit` to see which side
  has the extra/missing prop.

## Not yet done (future Phase 2 work)

- **Runtime-validate the headless client** — it's wired + compile-verified but
  unproven live (see "Headless client" limits). First real run: watch the
  client log for crashes / `AUDIT FAIL` storms / the pawn never spawning.
- **Scripted client movement** — the headless client takes neutral input
  (stationary pawn). Driving it with a simple movement/fire script (or a Lua
  hook) would make it a more representative second combatant; currently the
  bots provide the churn and the client is a passive auditor + parity peer.
- **In-session parity SVC** — parity is currently an offline log compare (no
  wire change). A live `SVC`/console readout of host-vs-client manifest delta
  would make divergence visible in-game, but that's a proto-bump item for the
  Phase 4 wire era.
