# Netplay / Online — Performance & Reliability Review

**Date:** 2026-06-06
**Branch reviewed:** `port-net-predict`
**Scope:** `port/src/net/*` + the per-tick engine hooks (`pdsched.c`).
**Lens:** performance + reliability. The memory-safety/security pass is covered
separately in [`netplay-code-review-2026.md`](netplay-code-review-2026.md); this
review deliberately does not rehash it.

## Verdict

The architecture is strong: authoritative server, CSP + reconciliation,
snapshot-ring interpolation with an adaptive jitter buffer, server-side
lag-comp, time-aligned pose/room/anim reconstruction, phase-offset heartbeats,
a clean dedicated/headless split. The one structural weakness is the per-tick
**bandwidth model**, which degrades ungracefully at the player/sim counts the
mode targets, plus a couple of robustness gaps in the receive/transmit and
connectionless paths.

Items marked **[FIXED]** were addressed in the same commit as this document
(no protocol bump). The rest are recommendations.

---

## Reliability

### R1 — Unreliable sim broadcast had no buffer-space guard → deterministic starvation **[FIXED]**
`netEndFrame` (net.c). The co-op NPC/obj loops guard on `g_NetMsg.wp <
NET_BUFSIZE - N` and round-robin a cursor, but the **sim-bot loop** scanned
`0..g_BotCount` writing ~120-byte chr-state blocks into the fixed 1440-byte
unreliable buffer with no space guard. On overflow `netbuf` truncates safely
(no corruption) but `netFlushSendBuffers` still sends the truncated packet, so
the client drops the tail message — and because the scan order is fixed, the
**same high-index sims lose their update every tick**, reading as permanently
laggy while sim 0 stays smooth. Fixed by adopting the co-op loop pattern:
rotating cursor + `NET_BUFSIZE - 340` budget, so loss (when it happens) is
shared and interpolation hides it; under the common low-sim case behaviour is
unchanged. Player moves are inherently bounded (≤ `NET_MAX_CLIENTS`, small) and
reliable ones use the 4× fragmentable buffer, so they were left as-is.

### R2 — Receive demux loop ignored `error` → latent remote hang **[FIXED]**
`netServerEvReceive` / `netClientEvReceive` (net.c). The loop condition was
`while (!rc && netbufReadLeft(...) > 0)`. On a truncated trailing message the
read pointer stops short of `wp` with `error` set; `netbufReadU8` then returns 0
(= `*_NOP`, `rc` stays 0) without advancing `rp`, so the loop could spin forever.
It didn't fire today only because all 48 handlers happen to `return src->error`
or fully consume before any `return 0` — a fragile by-convention invariant.
Fixed by adding `!cl->in.error` to both loop conditions.

### R3 — Server query path was an unauthenticated UDP reflector/amplifier **[FIXED]**
`netServerConnectionlessPacket` answered any `PDQM` datagram with up to a ~1 KB
response to the unverified, spoofable source, with no rate-limit or challenge
(the `NET_QF_CHALLENGE` flag is unrelated — Combat Sim challenge mode). Request
5-6 bytes → ~50-100× amplification, on servers publicly advertised to the master
browser. Fixed with a per-source-IP throttle (LRU, ~4 summary/sec, ~1
details/sec per source) plus a global per-tick response cap as a backstop
against floods spread over many spoofed IPs. A stronger future option is a
challenge cookie gating the large details response.

### R5 — `netClientNeedMove` memcmp'd struct padding **[FIXED]**
`netClientRecordMove` populated `outmove[0]` field-by-field, leaving padding
(e.g. after `s8 weaponnum`) as recycled stack bytes; the change-detection memcmp
in `netClientNeedMove` then depended on that stale padding — a latent
spurious-send / missed-send the moment a field is reordered. Fixed with a
`memset(move, 0, sizeof(*move))` before population.

### R4 — `g_NetTick` advances per render-frame-with-a-tick, not per logical tick (recommendation)
`netStartFrame` runs once when `g_Vars.diffframe60 > 0` (pdsched.c), but the
sim can run `diffframe60` times that frame. On a client dropping below 60 fps
the net clock drifts slower than the sim clock, skewing tick-stamped moves and
CSP/interp timing. The slow-mo work moved the real-time accumulator into
`detPinTimestep`, which may partially compensate; worth an explicit assertion /
diagnostic that `g_NetTick` tracks wall-clock 60 Hz under frame drops.

---

## Performance

### P1 — Chr-state block is the bandwidth driver **[DONE — proto 62]**
`netmsgSvcPropMoveWrite` ships ~119 bytes per networked chr every server tick.
The seven pose fields that don't need 32-bit precision now ride as s16: body yaw
and `angleoffset` as periodic angles (`[-π,π)` wrap), the four `aim*` joints as
clamped `[-π,π]`, and `anim->speed` as clamped `[-16,16]`. The quantization step
(~1e-4 rad / ~5e-4 speed-units) is below what rendering or interpolation resolves.
Saves **14 bytes/chr/tick**. Position stays a full coord (precision feeds
hit/visual) and `chr->damage` stays f32 (wide, sometimes-negative). Realised cut
is ~12% of the block, not the 25-30% first estimated — position is the other big
chunk but quantizing it well is map-range-dependent and was judged too risky to
land untested. Protocol bumped 61 → 62.

### P3 — adaptive player-move cadence **[DONE]**
`g_NetServerUpdateRate` defaulted to 1 (every-tick SVC_PLAYER_MOVE). The global
send gate now stretches to every-other-tick once a match has **>4 combatants**,
halving per-tick player-move bandwidth in large games; 2-4 player matches are
untouched (no feel change) and interpolation hides the 30Hz cadence. An operator
override (`/svcrate`, `Net.Server.UpdateFrames > 1`) still wins via `max()`.

### P2 — Per-client relevancy + delta compression **[DEFERRED — design below]**
`enet_host_broadcast` ships one identical all-entity packet to every client with
no PVS/room-distance interest management, and every field is absolute (no delta).
This is the real scaling answer but it is an **architectural rewrite of the core
send path**, and this environment cannot compile or run the game — shipping an
untested rewrite of the working broadcast path risks regressing netplay for a gain
that can't be measured here. Deferred with a concrete plan rather than blind-coded.

**Plan (do this in a build/test-capable environment):**
1. **Per-client out buffers.** Replace the single shared `g_NetMsg` unreliable
   broadcast in `netEndFrame` with a per-client write: loop clients, build each
   one's packet into `cl->out` (already exists), `netSend(cl, …)` individually.
   Keep `g_NetMsg` only for genuinely global reliable events. Cost: N× the
   serialization work on the host — acceptable up to 8 clients, and the relevancy
   cull below reduces per-packet size to compensate.
2. **Relevancy cull.** For each (client, entity) decide inclusion by room/PVS
   first (reuse `prop->rooms` ∩ the client's player rooms, or a coarse
   room-adjacency test) then a distance fallback. The local player's own move must
   always be included (CSP reconciliation depends on the echo). Start with sims +
   campaign NPCs (the bulk); players are few and usually relevant.
3. **Delta baselines.** Per client, keep the last *acked* value of each replicated
   field (the move ring already carries acks). Send a per-entity changed-field
   bitmask + only the changed fields; full state on the first send after an entity
   becomes relevant. This is the big win but also the most error-prone — gate it
   behind a config flag and ship relevancy (steps 1-2) first.
4. **Protocol + tooling.** New proto bump; extend `tools/query.py` / the F9 overlay
   to show per-client packet sizes so the cull/delta benefit is measurable.

Land steps 1-2 first (relevancy, no delta) — they give most of the scaling benefit
with far less desync risk than delta encoding, and each is independently testable.

---

## Smaller / lower-priority (recommendations)

- **`netStartFrame` event pump** **[FIXED]** — did one `enet_host_service(…, 1)`
  per tick after draining queued events, processed that single serviced event and
  exited, so the rest of the burst `enet_host_service` had queued waited a frame
  (≤ 16 ms added latency under load). Now re-drains the queue via
  `enet_host_check_events` after each service and re-services the socket up to
  `maxservices` (8) times, so a full inbound burst is handled the same frame while
  a sustained flood still can't stall the loop. No wire change.
- **`netDiagLogf` flushes every line** **[FIXED]** — when a diag log was open the
  10 Hz per-entity `pos_cl`/`pos_sim` dumps were a lot of synchronous `fflush` on
  the game thread. Now the per-line flush is skipped for `pos_cl`/`pos_sim` only;
  every event/bracket line (`server_start`, `stage_start`, `csp_recon`, `lagcomp`,
  `tick`, `disconnect`, `proptick_guard`, …) still flushes immediately, so crash
  brackets are never lost. Buffered position lines reach disk on the next event
  line's flush (and on close via `fclose`). Opt-in as before. No wire change.
- **Lag-comp rewind uses `interp_lag` as a proxy** for the shooter's render-tick
  (honestly documented). Correct only under symmetric latency; an exact fix
  needs the shooter to send its render-tick (proto bump). Acceptable ceiling.
