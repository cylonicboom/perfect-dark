# Port Co-op Prep (design notes / pre-plan)

> Status: **prep notes**, not an implementation. This captures the chr-replication
> foundation that Combat-Sim sim sync already gives us and enumerates what campaign
> **co-op** needs on top, so it can be turned into a concrete plan. Nothing here is
> wired up yet.
>
> Read alongside `port/src/net/CLAUDE.md` (sim sync, CSP/interp/lag-comp) and
> `docs/PORT_NET_KNOWN_ISSUES.md`.

## Goal

Two players (host + one client, possibly more) play the **solo campaign**
co-operatively over the existing ENet client-server netplay. The host is
authoritative for the whole world (AI, objectives, scripts); clients render and
send input, exactly as in Combat Sim. PD already has a latent co-op concept
(`g_Vars.coopplayernum`, `antiplayernum` for counter-op) the engine references in
places — co-op netplay drives those from the wire.

## The chr-replication foundation (already built for Combat-Sim sims)

Campaign NPCs are `PROPTYPE_CHR` props just like sims, so most of the sim-sync
stack applies directly. What exists today (all client-side, host stays the
authoritative N64 game logic):

| Concern | Where | Reusable for co-op? |
|---|---|---|
| Position broadcast (per sim, per tick) | `net.c` `netEndFrame` sim loop; `netmsgSvcPropMoveWrite` chr-state block | Yes — same message, just a wider set of chrs |
| Entity interpolation (8-snapshot ring, time-aligned pose) | `netChrInterpolate` (`net.c`), `chrdata.netsnap[]` | Yes |
| Body yaw / waist twist / aim joints sync | chr-state block | Yes |
| Held-weapon sync (per hand) | chr-state block read (`netmsg.c`) | Yes (NPCs carry guns) |
| Muzzle-flash continuous reconcile + `SVC_CHR_FIRE` | chr-state gunfire byte; `netmsgSvcChrFire*` | Yes |
| Anim sync + **cross-fade (merge 16)** | chr-state animnum/framea/speed; `modelSetAnimation(…,16)` | Yes |
| **Room membership time-alignment** | `netsnap[].rooms`, `netChrInterpolate` | Yes |
| **Vertical grounding** (no local-gravity sink under the map) | `chrTick` re-pin to `cdFindGroundInfoAtCyl(&prop->pos,…)` | Yes — **but currently gated `chr->aibot`** (see below) |
| HP + shield absolute sync | chr-state shield(u8)+damage(f32), proto 39 | Yes |
| Damage replay for effects | `SVC_CHR_DAMAGE` | Yes |
| Death visuals (animnum-driven; client forces `ACT_STAND`) | `netmsg.c` | Partly — see actiontype below |
| Deterministic syncids | `netSyncIdsAllocate` (positional in `g_Vars.props`), synced RNG seeds | Yes, **if** spawn order stays deterministic across host/client |

## What co-op adds beyond Combat-Sim sims

### 1. Chr variety — most campaign NPCs are NOT `aibot`s
MP sims are `aibot`s (driven by `botTick`). Campaign NPCs are ordinary `chrdata`
driven by `chrTick` + **AI command lists** (`chr->ailist`/`aioffset`, the
`AICMD_*`/`AILIST` bytecode), not by `botTick`. Consequences:

- The client AI gate is currently *`botTick`-shaped* (`prop.c` skips `botTick`
  for sims). Campaign NPCs don't call `botTick`; their AI is the ailist
  interpreter run inside `chrTick`. Co-op needs a **client-side gate for ailist
  processing** (run model/anim/render, skip AI decisions) — a new guard, not the
  existing one.
- Several sim-only fixes are gated `chr->aibot` and **must widen to all
  client-driven chrs**. The most important is the **vertical-grounding re-pin**
  in `chrTick` (`g_NetMode == NETMODE_CLIENT && chr->aibot && …`): campaign NPCs
  would sink under the map without it. Audit every `chr->aibot &&` net guard
  before co-op.

### 2. The `actiontype` problem, at much greater scope
Clients force `chr->actiontype = ACT_STAND` because the per-state `chr->act_*`
union isn't synced and `chrTick*` dispatchers deref it without init checks
(crash). For sims this is fine (visuals come from animnum). Campaign NPCs use a
far richer action set (patrol, go-to-pos, cover, cower, surrender, throw,
converse, scripted set-pieces). Options to evaluate in the plan:

- Keep forcing `ACT_STAND` and rely on animnum for visuals (cheapest, what sims
  do) — acceptable for ambient NPCs, weak for scripted set-pieces.
- Sync a **whitelisted subset** of actiontypes whose union we *do* serialize.
- Sync the union for specific high-value actions only.

### 3. Objectives, triggers, scripted events
Campaign has objective state, scripted AI (`AICMD_*` intro/triggers), timed
events, cutscene/`chr` set-pieces, alarm state, etc. — none of which Combat Sim
exercises. The host runs them; the client must be told the *outcomes*:

- Objective completion / failure state (new `SVC_*`).
- Scripted door/lift/switch already partly covered (`SVC_PROP_DOOR/LIFT/USE`).
- Alarm / `chrflags` global state (e.g. guards alerted).
- Triggered spawns (reinforcements) — must reuse `SVC_PROP_SPAWN` + keep
  syncids deterministic, or assign authoritatively from the host.

### 4. Stage flow
Combat Sim is one flat arena. Campaign has briefing → intro cutscene → play →
outro → next stage, plus solo-only assumptions. Co-op needs: synchronized stage
load/advance (extend `SVC_STAGE_START/END`), agreement on `g_Vars.coopplayernum`,
handling the briefing/solfiles menu for 2 players, and skipping/serializing
cutscenes.

### 5. Player count & co-op slots
Drive `g_Vars.coopplayernum` (and `antiplayernum` if counter-op is ever wanted)
from the lobby. NPC **targeting** must choose among 2+ players consistently
(host-authoritative — clients already render whatever the host's AI picked).
Splitscreen-vs-net player allocation interacts with the existing
`playerGetLocalCount`/spectator plumbing — audit it.

### 6. Death / respawn semantics
Campaign death ≠ Combat-Sim respawn. Co-op needs a policy: revive-on-teammate,
checkpoint respawn, or mission-fail-on-any-death. Whatever it is, it's
host-authoritative and pushed to clients (the sim HP/death machinery is the
substrate, but the *policy* is new).

## Choke points to reuse (don't reinvent)

- `netmsgSvcPropMoveWrite` / `…Read` chr-state block — the one place chr pose
  serializes. Widening "which chrs get broadcast" is a server-loop change
  (`net.c` `netEndFrame`), not a format change.
- `netChrInterpolate` — the single client-side pose driver. Already generic on
  `chrdata` (not `aibot`); designed with "campaign NPCs once online co-op lands"
  in mind.
- The `chrTick` client guard region (right after `netChrInterpolate`) — where
  per-tick client-driven-chr fixups live (vertical re-pin today).
- `netSyncIdsAllocate` — positional syncids; co-op's biggest determinism risk is
  any host-only prop spawn shifting the array. Prefer host-authoritative syncid
  assignment for dynamically spawned campaign props.

## Prop-lifecycle sync: events + a reconciliation backstop (hardening)

The prop world syncs by **events** — `SVC_PROP_SPAWN` (add), `SVC_PROP_FREE`
(remove, proto 40), `SVC_PROP_MOVE` (pose) — over the reliable channel, on top of
deterministic positional syncids (`prop − g_Vars.props + 1`). This is correct *if
complete*: the recent lingering-mine bugs were a missing `SVC_PROP_FREE` and then
an incomplete client teardown (a bare `propFree` instead of the full
`objFreePermanently`, which left a stuck mine in its parent chr's child list).
Co-op multiplies the surface: NPCs dropping weapons, destructibles, scripted
spawn/despawn — every one needs symmetric add **and** remove, running the engine's
real teardown on both sides, or the client world drifts.

**Determinism is the real guarantee, not a heartbeat.** Syncids are positional, so
the scary failure is the two `freeprops` pools diverging: then syncid *N* points at
different props on each side and everything downstream mismaps. Symmetric
spawn+free is what keeps the pools locked together — get that right first.

**Add a cheap reconciliation backstop, not a full snapshot.** A full per-prop
state snapshot every N ticks is too heavy (hundreds of props in a campaign level).
Instead, periodically (the existing score/stats/KoH heartbeat cadence) send a
**compact signature of the host's active synced-prop set** — a count plus an
XOR/rolling checksum of active syncids, ideally over a *window* of the syncid range
each heartbeat to amortize. The client compares; on a match (the common case) it
does nothing; on a mismatch the host re-broadcasts the reconciling spawns/frees.
Two correctness rules:
- **Ignore in-flight props:** never remove a client prop whose `SVC_PROP_SPAWN`
  could still be in transit — stamp props with a spawn tick and skip anything
  younger than ~RTT, or the heal will delete legitimately-new props.
- **It heals *existence*, not *identity-mismap*.** An existence checksum catches
  ghosts (client has it, host freed it) and orphans (client missing it), but not
  "both sides have syncid N pointing at different props" — that only comes from a
  diverged pool, which the symmetric-free discipline above prevents.

Sequencing: (1) complete the event model; (2) add the rolling checksum heartbeat
as a backstop; (3) only invest in identity-level reconciliation if checksums keep
mismatching in practice.

## `aibot`-gated net logic to widen for co-op (audit list)

Grep `chr->aibot` in net-touched paths and decide per-site whether co-op NPCs
need it. Known so far:
- **`chrTick` vertical re-pin** (`chr.c`) — must widen to all client-driven chrs.
- `SVC_CHR_FIRE` on-transition broadcast (`chraction.c`) is gated
  `chr->aibot && syncid` — NPCs fire too.
- `chrDie` client weapon-drop guard already keys on `NETMODE_CLIENT`, not aibot —
  OK, but re-verify for NPC weapon props.

## Gaps inherited from sim work (carry into co-op testing)

- Interp delay (`g_NetInterpTicks`) positional lag scales with chr speed; fine
  for walking NPCs.
- `actiontype` forced to `ACT_STAND` (above).
- Extrapolation reversal on sharp direction changes (`g_NetExtrapMaxTicks`).
- See `docs/PORT_NET_KNOWN_ISSUES.md` for the live list.

## Open questions for the plan

1. NPC AI client gate: where exactly to short-circuit ailist execution in
   `chrTick`/the AI interpreter without breaking model/anim setup?
2. actiontype policy: force-STAND everywhere, or serialize a whitelist?
3. Objective/script sync surface: how many new `SVC_*` messages, and which
   campaign subsystems are in scope for a first playable (one level vertical
   slice vs. all)?
4. Dynamic spawn determinism: host-authoritative syncid assignment vs. trusting
   RNG-synced spawn order.
5. Stage-flow / cutscene handling for 2 players.
6. Death/respawn policy.

## Recommended first slice

One simple combat-light level, ambient guards only, host-authoritative
everything, NPCs rendered via the existing sim stack with the `aibot` gates
widened and ailist execution skipped on clients. No scripted set-pieces, no
cutscene sync — prove chr replication + objective-complete + stage-advance
end-to-end, then layer scripting.
