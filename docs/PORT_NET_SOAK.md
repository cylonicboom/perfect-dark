# Netplay Prop-Sync Soak Harness (Phase 2) — 2026-06-10

> **Status: implemented, compile-verified (Linux dedicated build); the offline
> tool is unit-tested on synthetic logs. The end-to-end soak itself needs a
> ROM-provisioned build + a connecting client to actually run — there is no
> headless client.** Phase 2 of the prop-sync consistency plan.
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
| `out/` | generated CSVs/logs (gitignored). |

## How to run a soak (needs a ROM + a client)

The dedicated server is headless but still ROM-gated, and there is **no
headless client**, so a soak needs your real (windowed) client to connect and
provide the second half of the manifest-parity check. Minimal procedure:

1. **Build the dedicated server** (and your normal client) from this branch:
   ```
   cmake -DDEDICATED_SERVER=ON -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Release ..
   make -j
   ```
2. **Launch the server** (provision its ROM/assets as usual for a dedicated
   instance), e.g. for a 30-minute capped run:
   ```
   tools/soak/run_server.sh build_ded/pd-server.x86_64 27100 30
   ```
   It writes `tools/soak/out/server_<stamp>.csv`.
3. **Connect a client** to `host:27100`. On the client, open the console (`~`)
   and turn on its own diag + audit:
   ```
   /diag C:\path\to\client_soak.csv
   /audit on
   ```
   (or set `Net.Debug.LogPath` in the client's `pd.ini` before connecting; the
   auditor is on by default). The match auto-starts; leave it running. For a
   harsher test, add latency/loss on the client: `/lag 120` and `/loss 20`.
4. **Get the verdict** from both logs:
   ```
   tools/netsoak.py tools/soak/out/server_<stamp>.csv client_soak.csv
   ```

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

- **Headless client** — the missing piece for a *fully unattended* soak (server
  + scripted bot-driven client in CI). It's a real lift (the client couples to
  the render path; cf. the dedicated-server stubbing in `dedicated_stubs.c`),
  deferred deliberately. Until then the soak is "launch server, connect a real
  client, walk away, read the verdict."
- **In-session parity SVC** — parity is currently an offline log compare (no
  wire change). A live `SVC`/console readout of host-vs-client manifest delta
  would make divergence visible in-game, but that's a proto-bump item for the
  Phase 4 wire era.
