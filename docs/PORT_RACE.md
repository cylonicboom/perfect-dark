# Port-only Combat Sim mode: "Race" (`MPSCENARIO_RACE`)

Checkpoint racing over the King of the Hill setup. The stage's KoH hillpads
become an **ordered checkpoint course**: racers must touch hill 1, 2, 3, …,
N in stage-data order; touching the last wraps back to hill 1 — that's one
lap. First to the configured lap count takes finishing position 1; the match
keeps running until **every human has finished OR the "Finish Timer"** (the
pity timer, started by the first finisher) expires. **Bots do not race** —
they fight as usual and sit at the bottom of the scoreboard.

This is the ninth scenario and the **first real user of the hi-bit wad
scheme** (id 8 — the scheme pioneered by the retired Elimination scenario;
see `docs/PORT_ELIMINATION.md`'s appendix and the `constants.h` MPSCENARIO
note).

## Options (Race Options menu)

- **Laps** — 1-10, default 3. A lap = touching all N checkpoints in order.
- **Finish Timer** — 0-120 s, default 30. Seconds the rest of the field gets
  after the first finisher before the match force-ends; **0 = "Instant"**
  (the match ends with the winner).

Plus the standard six toggles. Teams are not forced (it's a free-for-all
race; team scoring over the synthetic scores is meaningless — see Notes).

## Mechanics

- **Course**: `raceAddHill` collects the hillpads via the `INTROCMD_HILL`
  dispatch (the kohAddHill/zonesAddHill pattern, into
  `g_ScenarioData.race`); `raceInitProps` resolves rooms + positions and
  highlights every checkpoint room in the KoH hill green. The **radar always
  shows a dot on YOUR next checkpoint** (per-player — room tints are global
  state shared across split-screen, so they can't mark "next").
- **Progress** (host-authoritative, `raceTick`): per human chr, when
  `rooms[0]` equals the next checkpoint's room (the KoH occupancy test) the
  checkpoint advances; passing the last wraps to 0 and increments the lap;
  reaching the lap count assigns the next finishing position. The first
  finisher starts the finish timer. The dead can't pass checkpoints (respawn
  and continue — progress survives death); finishers stop advancing.
- **Match end**: `raceShouldEndMatch()` — someone finished AND (all humans
  finished OR finish timer expired) — **polled from lv.c's match-end block**
  (`g_NumReasonsToEndMpMatch` is recomputed from zero there each frame; a
  tick-side increment would be wiped — the documented elimShouldEndMatch
  rule). Time limit stays as a backstop.
- **Scoring**: synthetic ranking values from `raceCalculatePlayerScore` —
  finishers `2000 - finishpos` (1st > 2nd > …), racers
  `laps*20 + checkpoint` (course progress), bots ~0. `mpApplyLimits` forces
  the score/team-score limits unlimited for Race so these values can't trip
  an early end. The scoreboard numbers look synthetic; ranking is what
  matters.
- **HUD** (top centre): `Lap 1/3 - CP 2/5` while racing, `Finished #1` when
  done; ` | Ns` (ceil seconds) appends while the finish timer runs and
  someone's still racing.
- **Seeding**: lazy on the first tick (`humancount` = chrs with
  `aibot == NULL`; `g_MpAllChrPtrs` isn't populated until `mpStartMatch`
  finishes).

## Persistence (mpsetups v10 — the hi-bit scheme, now live)

- The wad's 3-bit scenario field stores `id & 7` (= 0, Combat) and the 4th
  bit rides the **v10 tail** (1 bit — **the block is now 100% full,
  640/640**; the next saved field needs the `MPSETUP_BLOCKSIZE` migration).
- At parse time a Race block dispatches as Combat (no initfunc, NULL
  readsavefunc), so its scenario save slot is consumed by
  `scenarioReadSave`'s default branch — captured raw in
  `g_ScenarioSaveSlotRaw` (restored in scenarios.c) — and
  `mpsetupfileLoadWad` fixes up `scenario |= 8`, re-runs `scenarioInit()`
  and re-applies the slot via `raceApplySaveSlot`.
- The slot packs `racelaps` (bits 0-3) + `racepitytime` (bits 4-10);
  `raceWriteSave` dispatches normally (write-time scenario is the real 8).
- `mpsetupfileGetOverview` (saved-setups list label) shows "Combat" for Race
  setups (it parses only the block head) — cosmetic; they load fine.

## Networking (`SVC_RACE_STATE` 0x58, proto 72)

`{[u8 nextcp, u8 lapsdone, u8 finishpos] × 12, u8 finishcount,
u8 humancount, u8 pitystarted, u16 pity240}` — reliable, on-change (every
checkpoint pass / finish, `g_MpRaceDirty`) + 1s heartbeat at phase 5.
Clients tick the finish timer locally between heartbeats; the client apply
(`raceApplyWireState`) bounds `nextcp` against the local hillcount.
`g_MpSetup.racelaps`/`racepitytime` ride `SVC_STAGE_START` /
`CLC_ADMIN_SETUP` after the lives fields (display + admin pushes; detection
is host-only, no determinism concerns).

## Files

| File | Change |
|---|---|
| `src/game/mplayer/scenarios/race.inc` | New: the whole scenario (options menu, course collection, init/tick, end predicate, HUD, radar, score, save-slot pack/apply, wire accessors, `g_MpRaceDirty`) |
| `src/game/mplayer/scenarios.c` | vtable row + overview + `#include`; "Race" name; `scenarioReset` hillcount clear; `INTROCMD_HILL` → `raceAddHill`; `g_ScenarioSaveSlotRaw` capture restored |
| `src/include/constants.h` | `MPSCENARIO_RACE 8` + the hi-bit aliasing rules (updated) |
| `src/include/types.h` | `struct scenariodata_race` + union member; `mpsetup.racelaps`/`racepitytime` |
| `src/include/game/mplayer/scenarios.h` | overviews `[9]`; capture + `raceApplySaveSlot` decls |
| `src/game/mplayer/mplayer.c` | defaults (3 laps / 30 s); wad v10 hi-bit write + read/fix-up/slot re-apply; `mpApplyLimits` score-limit override |
| `src/game/lv.c` | `raceShouldEndMatch` polled in the match-end block (beside the Lives poll) |
| `port/src/mpsetups.c` | `MPSETUP_VERSION` 10 |
| net (`netmsg.h/.c`, `net.c`, `netmenu.c`, `net.h`) | `SVC_RACE_STATE`; setup fields in stage-start/admin-setup; proto 72; name tables ("Race") |

## Notes / limits

- Bots don't race by design (they fight; their synthetic score is ~0).
- Checkpoint granularity is the **room containing the hillpad** (the KoH
  occupancy model). Two consecutive checkpoints sharing a room would both
  advance in quick succession; no stock stage does this.
- **Players spawn at checkpoint 1** (`raceChooseSpawnLocation`, the Graffiti
  spawn-tier pattern: pads in its room → pads in a neighbouring room → stock
  random fallback). Applies to the start grid AND respawns (death sends you
  back to the start area; progress is kept). Bots fall through to random
  spawns so they don't pile on the start line.
- Teams aren't locked off; in a team game the team scoreboard sums synthetic
  scores (meaningless but harmless). Race is intended FFA.
- Listed under the Teamwork group header in team games (groups are
  index-ranged; cosmetic).
- No per-checkpoint pass sound/message yet; the HUD line + radar dot are the
  feedback.
