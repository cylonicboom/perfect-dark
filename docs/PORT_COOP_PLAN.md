# Port Co-op Implementation Plan

> The actionable build plan for campaign co-op. Design rationale and the
> foundation inventory live in [`PORT_COOP_PREP.md`](PORT_COOP_PREP.md); read that
> first. This file is the sequenced "what to build" with concrete hook points.

## Locked decisions

- **First milestone:** one-level **vertical slice** — a single combat-light level
  fully co-op playable end-to-end (NPCs replicated, objectives complete, stage
  advances) before generalizing.
- **Players:** target **up to 4** (but build 2 first — see the player-model risk).
- **Death:** **checkpoint respawn** — a dead player respawns at the last
  checkpoint; the mission continues.

## Foundation (already built, reuse as-is)

The Combat-Sim chr-replication stack is generic on `chrdata`, so campaign NPCs
ride the same rails:
- Position interp (`netChrInterpolate`), chr-state block (pos/yaw/aim/anim/weapons/
  gunfire), anim cross-fade (merge 16), vertical grounding, HP/shield sync,
  `SVC_CHR_DAMAGE`/`_FIRE`, dropped-item spawn (`objDrop`), `SVC_PROP_FREE` +
  `SVC_PROP_RECONCILE` backstop.
- Deterministic positional syncids: campaign NPCs spawn at `setup.c` load on both
  sides, so they get identical syncids like sims do.

## The linchpin: NPC AI gate

NPC AI runs `chrTick` (chr.c:2538) → `chraTick` (chraction.c:13947) →
`chraiExecute` (chrai.c:1044, the ailist bytecode). **It is not net-gated.** Sims
avoid AI on clients by skipping `botTick` (prop.c:2163); NPCs have no such split —
`chrTick` does render *and* AI. Co-op must skip the AI for client-driven NPCs the
same way, keeping render.

- **Gate point:** `chraTick(chr)` at chr.c:2538 — wrap in
  `#ifndef PLATFORM_N64` / `if (g_NetMode != NETMODE_CLIENT || <not a synced chr>)`.
- **CRITICAL RISK:** client *sims* already run `chrTick` → `chraTick` today
  (apparently harmless because the chr-state apply forces `ACT_STAND` and wire
  state dominates). Gating `chraTick` must be verified to not regress sims —
  determine what `chraTick` provides to a client sim (anim advance? timers?) that
  must keep running, and gate only the AI decision path (`chraiExecute` + action
  selection), not the whole function, if needed.

## Phases

### Phase 0 — Co-op session plumbing
- Lobby: a "Campaign Co-op" mode (stage picker = solo stages, not Combat Sim
  arenas). Reuse the existing netmenu host/join flow.
- `SVC_STAGE_START`: carry the campaign stagenum + a co-op flag (today it carries
  the MP stage). Clients load the same solo stage.
- Player allocation: reuse Combat-Sim's N-player allocation (`g_Vars.players[0..N]`)
  to seat up to 4, spawning at the level's co-op spawn pads.
- **Start with 2 players** (`g_Vars.bond` + `g_Vars.coop`) to match the engine's
  built-in co-op model, then widen (Phase 5).

### Phase 1 — NPC chr replication (core)
- **Host broadcast:** generalize the sim loop in `net.c` `netEndFrame` (line 1649,
  `g_MpBotChrPtrs`) to iterate **all active NPC chrs** (`g_ChrSlots`,
  `chrsGetNumSlots`) and `netmsgSvcPropMoveWrite` the chr-state for each
  (skip players). Gate to campaign/co-op so Combat Sim is unchanged.
- **Client AI gate:** the linchpin above — skip NPC AI, render-only, force
  `ACT_STAND` (chr-state apply already does this).
- **Widen the `aibot`-gated fixes** to all client-driven chrs (audit list in
  `PORT_COOP_PREP.md`): the `chrTick` vertical-ground re-pin, `SVC_CHR_FIRE`
  on-transition (`chr->aibot && syncid`), etc.

### Phase 2 — Combat correctness
- Client→NPC hits: `CLC_HIT` carries the gset already; verify the host's
  `chrDamage` path (disarm, death, knockback) replicates for non-aibot chrs the
  same as sims. HP/shield + `SVC_CHR_DAMAGE` already handle the numbers.
- NPC death + dropped weapons already covered (`objDrop` spawn + free/reconcile).
- NPC **targeting** among multiple players: host-authoritative (clients just
  render whatever the host's AI picked) — but see the 2-vs-4 player-model note.

### Phase 3 — Objectives + stage flow (the slice's "win")
- **Objective state:** a new `SVC_OBJECTIVE` (status per objective index). Host is
  authoritative; broadcast on change + a heartbeat heal.
- Doors/lifts/switches: `SVC_PROP_DOOR/LIFT/USE` exist — verify they fire on the
  campaign interactables the chosen level uses.
- **Stage complete:** host detects all objectives done → `SVC_STAGE_END` → clients
  show mission-complete / advance.
- **Checkpoint respawn:** on a player's death the host respawns them at the last
  checkpoint and force-positions via the existing player-teleport path
  (`chrSetPos` + `UCMD_FL_FORCEMASK`). For the slice, checkpoint = level start (or
  one mid-level checkpoint); generalize later.

### Phase 4 — Scripted events (only what the slice needs)
- Alarm/alertness global state, triggered reinforcement spawns (host-authoritative
  `SVC_PROP_SPAWN` with host-assigned syncids), and cutscene handling
  (skip/serialize). Scope strictly to the chosen level — don't sync the whole
  campaign scripting surface yet.

### Phase 5 — Scale to 4 players (heaviest)
- **The engine's co-op model is 2-player:** `g_Vars.coop` is a single player,
  `coopplayernum` is singular (playermgr.c:114-119), and campaign AI/objectives
  reference `g_Vars.bond`/`g_Vars.coop`. Supporting 4 net co-op players means
  widening those assumptions (player iteration in NPC targeting, objective "all
  players", HUD/spawns) — this is the biggest single chunk.
- Recommended: land the slice at 2 players (Phases 0-4), then do 4-player as its
  own pass so a regression here can't block a playable slice.

## Suggested first level

A combat level with simple objectives and minimal scripted set-pieces — e.g.
**dataDyne Defection** (guards + reach-objective flow) or a comparably light early
level. Avoid anything with heavy cutscene/script dependencies for the slice. Final
pick once we scan the candidate's objective/`AICMD_*` complexity.

## Top risks (carry into every phase)
1. **`chraTick` gate regressing sims** — verify first (Phase 1 blocker).
2. **`actiontype` forced to `ACT_STAND`** loses scripted NPC actions — fine for
   ambient combat NPCs, weak for set-pieces (revisit in Phase 4 if needed).
3. **4-player co-op** vs the engine's 2-player model (Phase 5, biggest scope).
4. **Dynamic spawn determinism** for reinforcements — host-authoritative syncids.
5. **Objective/script surface creep** — hard-scope to the slice's level.

## First concrete steps
1. Verify the `chraTick`/`chraiExecute` client gate doesn't regress sims (read +
   reason, then a guarded gate).
2. Generalize the host chr-state broadcast loop to `g_ChrSlots`, co-op gated.
3. Pick the slice level; enumerate its objectives + interactables to size Phase 3.
