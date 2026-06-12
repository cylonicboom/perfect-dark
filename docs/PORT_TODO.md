# Port / Netplay — To-Do (priority order)

Snapshot of outstanding work, captured 2026-06. Pulled from PR #8's open list, the
co-op plan (`PORT_COOP_PLAN.md`), and `PORT_NET_KNOWN_ISSUES.md`. Items already
fixed this session (objective-complete notifications + rendering, client objective
event-witnessing `CLC_OBJECTIVE_DONE`, client-local proximity pickups, pickup-toast
mirror, prop-float heartbeat, body type, lives toast, ECM message/sound spam, co-op
lobby window) are **not** repeated here.

## Co-op campaign (active focus)

1. **End-of-mission flow** — *biggest remaining gap.* Client's exit condition differs
   from the host's (e.g. enter a trigger vs. open the elevator), and the outro
   cutscene doesn't sync both ways (host doesn't follow when the client exits; host's
   outro doesn't end for the client). Needs an exit-detection + outro-cutscene
   (`SVC_CUTSCENE` / `SVC_STAGE_END`) symmetry pass. Without it, finishing a mission
   together is broken.
2. **Scripted player-give remapping ("option 3")** — Cassandra's necklace and similar
   single-target `aiGiveObjectToChr` → `CHR_COOP` gives land on the wrong player on
   the client (BOND/COOP map to different physical players per machine). Item never
   enters the client's inventory + no bottom-left pickup toast. Not mission-blocking
   (the gate it unlocks is host-driven + mirrored). Full rationale + dead-ends +
   proper fix in `PORT_NET_KNOWN_ISSUES.md`. Deferred.
3. **High-ping prop interpolation** — physics props model-vs-hitbox desync at ~250ms;
   client "can get stuck." Parked (likely needs prop interpolation, not a quick fix).
4. **Checkpoint respawn polish** — lives/respawn exist; checkpoint *position* may still
   be level-start only (co-op plan Phase 3).
5. **Phase 4 — scripted events** (reinforcement spawns, alarm/alertness state), scoped
   per chosen level.
6. **Phase 5 — 4-player co-op** (heaviest). Engine co-op is 2-player; widening
   `g_Vars.coop` / `coopplayernum` / objective "all players" / HUD / spawns.

## Netplay general (non-coop, from KNOWN_ISSUES)

- Cloaking device not synced.
- Slayer fly-by-wire on clients — IMPLEMENTED proto 76 (compile-verified, runtime test pending). FarSight alt-fire still doesn't work on clients.
- Punching + some weapon-anim sounds play first-person for every listener.
- Lag-comp is broad-phase (sphere) only; narrow-phase bone rewind reverted (crashed).

## Larger staged workstream

- **Determinism foundation** (`~/.claude/plans/...` determinism plan): same-inputs →
  identical-state-hash harness, cosmetic-RNG isolation, finish runtime player count.
  Prerequisite for any future peer/rollback netcode; nothing built on it yet.

## Housekeeping

- Confirm PR #8 CI is green on the latest commit (the `hudmsg.c` build break is fixed).

## Port features (non-netplay)

- **Mirror cheat — bug tidy-up (IN PROGRESS).** Known incomplete: Phase 2 (draw the
  gun as a normal model in the LEFT hand instead of mirror-imaging the viewmodel) is
  not implemented. Plus assorted bugs (see `PORT_MIRROR.md`).
