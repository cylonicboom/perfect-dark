# Prediction Work — Open Notes

Working file for client-side prediction design questions, determinism gaps, and existing TODO debt that touches the netplay layer. Load on demand — not part of the auto-loaded CLAUDE.md set.

For approaches that have already been tried and reverted, see [`../../../docs/PORT_NET_REVERTED_EXPERIMENTS.md`](../../../docs/PORT_NET_REVERTED_EXPERIMENTS.md). Do not re-attempt those without first addressing the documented root cause.

## Open Design Questions

- [ ] **Narrow-phase lag compensation** — broad-phase sphere rewind works; bone-matrix rewind crashed (see reverted experiments). Re-derive matrices on demand inside `shotCalculateHits`, or hook into the model render path and cache them?
- [ ] **High-ping CSP drift** — current retarget-only correction is stable but the local player visibly lags behind authoritative position at ≥150 ms. Bounded input replay (only N frames back, no history rewrite), or accept the drift?
- [ ] **Aim/look direction for sims** — head/torso aim is server-authoritative only. Worth adding a bit-packed delta to `SVC_PROP_MOVE`'s chr-state block, or defer?
- [ ] **`chr->actiontype` subset sync** — currently force-`ACT_STAND` on clients because the server's actiontype crashed on uninitialised union data. Open: sync a whitelist of safe actiontypes (`ACT_GOPOS`, `ACT_PATROL`, ...) with explicit init, leave the rest server-only?
- [ ] **Partial-body anim layers** — limb-specific anim layers (upper-body aim over lower-body run) only update server-side. Same question as actiontype.
- [ ] **Cloaking device sync** — listed in known issues. Probably a per-frame visibility bit in `SVC_PLAYER_STATS`; needs a wire-format decision.
- [ ] **Spectator authority** — `/spec` works locally but the server doesn't know who's spectating whom. Should it?

## Determinism Violations (server vs client drift)

The only point where RNG seeds are synced is `netClientSyncRng()` at stage start (`port/src/net/net.c:1056`). After that, server and client each advance `g_RngSeed` based on their own game events, so any divergent RNG consumer accumulates drift.

- [ ] **`g_RngSeed` not re-synced mid-game.** Every AI random decision, weapon spread, damage roll, and dropped item type pulls from `g_RngSeed` (defined `src/lib/rng_c.c:8`, declared `src/include/data.h:516`). Server and client diverge as soon as one side rolls and the other doesn't. Music RNG is correctly split out (`g_NetMusicRngSeed`, `port/src/net/net.c:166`); everything else shares one seed.
- [ ] **`g_Rng2Seed` same problem.** Second seed (declared `src/include/data.h:517`) only re-synced at stage start.
- [ ] **`osGetCount()` as game-state input.** `chraicommands.c:9873` reads `osGetCount() * 64 / 3000` inside `aiIfMusicEventQueueIsEmpty` (queryable by AI scripts). `camdraw.c:1093` writes `osGetCount() * 0.0000001f` into camera state. On port, `osGetCount()` is wall-clock (`port/src/libultra.c:42`), not a fixed game tick — server and client read different values.
- [ ] **Initial seed is wall-clock.** `pdmain.c:347` seeds via `rngSetSeed(osGetCount())` at startup. Mitigated by `netClientSyncRng()` at stage start, but RNG consumption between boot and stage start (menu nav, demo playback) starts diverged.
- [ ] **Floating-point determinism not enforced.** No strict-FP flags in `CMakeLists.txt`. Different platforms / compilers can reorder, fuse, or round differently. Long-running float accumulations (chr velocity integration, anim frame counters) drift even with identical inputs.
- [ ] **Per-chr `rand` field.** `struct chrdata` has a `rand` member used by AI commands (`if_morale_lt_random` etc., `src/include/commands.h:1326`). Seeded from `g_RngSeed` — drift follows the seed drift above.

## TODO / HACK Comments in Netcode

Verbatim from `port/src/net/`:

- [ ] `net.c:581` — `// TODO: this will indicate coop/anti/etc` (game-mode bits in client info)
- [ ] `net.c:1471` — `// TODO: backup the player configs or something` (around disconnect / state-restore)
- [ ] `net.c:1526` — `// HACK: when we're a client, we'll need to swap our player and server player's props`
- [ ] `netmsg.c:146` — `// TODO: make a map or something` (syncid handling)
- [ ] `netmsg.c:181` — `// TODO: use a CRC or something` (ROM identity; currently filename match in auth)
- [ ] `netmsg.c:183` — `// TODO: number of local players` (split-screen / multi-local support)
- [ ] `netmsg.c:511` / `netmsg.c:587` — `// TODO: coop, anti` (scenario-type byte, write + read sides)
- [ ] `netmsg.c:1859` — `// TODO: eventually remove this message completely` (deprecated message still on the wire)

## How to extend this file

Add a `[ ]` line under the appropriate section with file:line evidence when you open a new investigation. When something is settled, remove the line or move the explanation into a CLAUDE.md and reference it. If an attempt fails, the failure write-up belongs in `docs/PORT_NET_REVERTED_EXPERIMENTS.md`, not here.
