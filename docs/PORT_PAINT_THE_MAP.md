# Port-only Combat Sim mode: "Paint the Map" (`MPSCENARIO_PAINTROOM`)

A team game inspired by Splatoon / Tony Hawk graffiti. While a living combatant
stands in a room, that room's floor is tinted their team's colour. Rooms are
**re-paintable** — the last team to cross a room owns it. At the **time limit**
the team owning the most rooms (floor area) wins. Room-level granularity (the
whole room's floor flips at once); per-tile painting is intentionally out of
scope.

This is the seventh scenario, appended after Capture the Case. It is entirely
port-only (`#ifndef PLATFORM_N64`); the N64 build is byte-identical.

## How it works

Ownership reuses the existing scenario **room-highlight** pipeline — the same
one Capture the Case uses to tint static team base rooms — so no renderer work
was needed. The only difference is the owned set is dynamic instead of fixed:

- `paintIsRoomHighlighted(room)` / `paintHighlightRoom(room, &r,&g,&b)` are the
  scenario vtable callbacks (`scenarios.c` `g_MpScenarios[]`), consumed by the
  dynamic-lighting reshade in `dlights.c` (`scenarioHighlightRoom` →
  `g_Rooms[i].highlightfrac_*`). `paintHighlightRoom` multiplies room brightness
  by `g_TeamColours[owner-1]` (copied from `ctcHighlightRoom`).
- A room becomes highlighted via `roomSetLightOp(room, LIGHTOP_HIGHLIGHT, …)`,
  and is marked `ROOMFLAG_BRIGHTNESS_DIRTY_TEMP` on every owner change so a
  team-flip re-tints (the flash-lighting reshade pattern).

### State

Per-room ownership lives in `g_ScenarioData.paint` (`struct scenariodata_paint`,
port-only union member in `types.h`):

- `u8 *roomowner` — per-stage array (`MEMPOOL_STAGE`, size `g_Vars.roomcount`,
  modelled on `g_MpRoomVisibility`), allocated in `paintInitProps`. `roomowner[r]`
  = owning team **+1**, `0` = unpainted. Once painted a room never reverts to
  unpainted — only its owning team changes.
- `s32 roomcount`, `s32 teamcounts[8]` — live per-team owned-room tally.

`paintInitProps` runs from `setup.c`'s unconditional `scenarioInitProps()` (so it
runs even though the mode spawns no props), on host and client alike, re-zeroing
ownership each stage.

### Detection (host-authoritative)

`paintTick` (the scenario `tickfunc`, run every frame on all machines from
`lvTick`) iterates **all** combatants `g_MpAllChrPtrs[0..g_MpNumChrs-1]` — the
same all-chr scan KoH uses, so it covers local + remote players + bots uniformly
(unlike `tickchrfunc`, which only reaches the local player and bots). For each
living chr it paints `chr->prop->rooms[0]` with its team. The ownership write is
gated to the **server** (`g_NetMode != NETMODE_CLIENT`); clients receive
ownership over the wire. Counts and each player's `numpoints` (= their team's
owned-room count) are recomputed on every machine from the synced ownership.

### Networking (`SVC_PAINT_STATE`, proto 66)

`SVC_PAINT_STATE` (0x55) carries the full list of owned rooms:
`{u16 count, [u16 roomnum, u8 owner] × count}`. Because rooms are only ever
(re)painted, applying the present entries idempotently is sufficient — there are
no removals to replay. The server broadcasts it (reliable) **on change**
(`g_MpPaintDirty`, raised by `paintSetRoomOwner`) plus a **1 s heartbeat** at
`g_NetTick % 60 == 35` in `netEndFrame`, so dropped packets and mid-match joiners
heal within a second. `netmsgSvcPaintStateRead` applies each pair via the shared
`paintSetRoomOwner` (which sanitizes the room number and sets the highlight, and
never raises the dirty flag on a client).

### Scoring / win

`paintCalculatePlayerScore` returns `score = numpoints` (the team's owned-room
count), `deaths = numdeaths`, so `mpGetPlayerRankings` ranks the team with the
most floor first. The match is **time-limited**: `mpApplyLimits` forces the score
and team-score limits to unlimited for this scenario (`MPSCENARIO_PAINTROOM`) so a
team owning many rooms doesn't end the round early. Teams are locked on (forced in
`paintInit`; the teams checkbox is disabled like CTC/KoH in
`menuhandlerMpTeamsEnabled`).

## "Owned Room Spawn" option (`MPOPTION_OWNEDROOMSPAWN`, bit 33)

A Combat Sim option toggle (the high-word checkbox in the Paint options menu,
`menuhandlerMpCheckboxPortOption`). When on, `paintChooseSpawnLocation` (the
scenario `spawnfunc`, invoked from every respawn path via
`scenarioChooseSpawnLocation`) prefers to respawn a killed player in:

1. a room their team **owns**, else
2. a room **adjacent** to owned territory (`bgRoomGetNeighbours`), else
3. falls through (`return false`) to the stock random spawn.

It reuses `playerChooseSpawnLocation` with a team-filtered pad subset (capped at
24 — the picker's internal arrays are `[24]`), so the existing enemy-avoidance +
shortlist randomisation still apply ("more likely", not always the same corner).
Both host and client pick from the same wire-synced ownership; the host's
authoritative position correction reconciles any RNG divergence in the exact pad
chosen (the established net-spawn behaviour, `player.c:557`).

## Files

| File | Change |
|---|---|
| `src/game/mplayer/scenarios/paintroom.inc` | New: the whole scenario (options menu, init/initprops/tick, highlight, HUD, score, spawn, shared `paintSetRoomOwner`/`paintGetRoomOwner`, `g_MpPaintDirty`) |
| `src/game/mplayer/scenarios.c` | Register the scenario + overview entry; `#include` the `.inc`; `scenarioGetNameText` literal-name helper (port-only modes have no ROM lang string); always-show Teamwork group |
| `src/include/constants.h` | `MPSCENARIO_PAINTROOM 6`, `MPOPTION_OWNEDROOMSPAWN` (bit 33) |
| `src/include/types.h` | `struct scenariodata_paint` + port-only union member |
| `src/include/game/mplayer/scenarios.h` | `g_MpScenarioOverviews[7]` on port |
| `src/game/mplayer/mplayer.c` | `mpApplyLimits`: force score/team-score limits unlimited for paint |
| `src/game/mplayer/setup.c` | `menuhandlerMpTeamsEnabled`: lock teams on for paint |
| `port/include/net/netmsg.h` / `port/src/net/netmsg.c` | `SVC_PAINT_STATE` write/read |
| `port/src/net/net.c` | dispatch case + on-change/heartbeat send in `netEndFrame` |
| `port/include/net/net.h` | `NET_PROTOCOL_VER 66`; `paintGetRoomOwner`/`paintSetRoomOwner`/`g_MpPaintDirty` decls |

## Notes / limits

- Per-room, not per-tile. Per-tile Splatoon painting would need vertex splitting
  and dynamic vertex buffers — a much larger renderer lift, deferred.
- A room, once painted, never returns to unpainted (only re-owned), which is what
  makes the full-list wire format idempotent and removal-free.
- The HUD shows the local player's own team's room tally at the top of the
  screen; the in-game scoreboard and end-of-round ranking show every team's
  coverage via `numpoints`.
