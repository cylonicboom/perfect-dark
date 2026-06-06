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

## Performance (recommendations — these need a protocol bump)

### P1 — Chr-state block is the bandwidth driver: 119 bytes, 16 raw f32, every tick
`netmsgSvcPropMoveWrite`. Most of the 16 floats are angles/normalized poses
(`yrot`, `angleoffset`, the four `aim*`, `anim->speed`) that quantize losslessly
to s16/u8 — roughly a 25-30% cut to the dominant per-entity cost with no quality
change. Highest-leverage perf win; mechanical but needs a proto bump.

### P2 — Unreliable broadcast has no relevancy filtering
`enet_host_broadcast` ships one identical all-entity packet to every client with
no PVS/room-distance interest management. The real scaling answer (per-client out
buffers + delta-compression against a per-client acked baseline) is a project,
not a patch, but worth a design note.

### P3 — `g_NetServerUpdateRate` defaults to 1 (full state every tick)
Given P1/P2, consider defaulting to 2 (or adapting to connected client count)
and letting the existing interpolation hide it — a near-free bandwidth halving.

---

## Smaller / lower-priority (recommendations)

- **`netStartFrame` event pump** does one `enet_host_service(…, 1)` per tick
  after draining queued events; a burst of inbound packets is dispatched
  one-per-frame (≤ 16 ms added latency under load). Loop the service call
  (bounded) until it returns 0.
- **`netDiagLogf` flushes every line** when a diag log is open; the 10 Hz
  per-entity `pos_cl`/`pos_sim` dumps are a lot of synchronous `fflush` on the
  game thread. Keep strictly opt-in (it is) and consider buffered writes flushed
  only on event lines.
- **Lag-comp rewind uses `interp_lag` as a proxy** for the shooter's render-tick
  (honestly documented). Correct only under symmetric latency; an exact fix
  needs the shooter to send its render-tick (proto bump). Acceptable ceiling.
