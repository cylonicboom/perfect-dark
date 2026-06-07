# Port-only Combat Sim mode: "Graffiti" (`MPSCENARIO_PAINTROOM`; formerly "Paint the Map")

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

- `s32 claimtime240[12]` — per-combatant time (lvupdate240 units) spent in
  `lastroom`, for the timed-claim option below. Reset on room change, death
  and claim.

### Detection (host-authoritative)

`paintTick` (the scenario `tickfunc`, run every frame on all machines from
`lvTick`) iterates **all** combatants `g_MpAllChrPtrs[0..g_MpNumChrs-1]` — the
same all-chr scan KoH uses, so it covers local + remote players + bots uniformly
(unlike `tickchrfunc`, which only reaches the local player and bots). With the
Claim Time slider at 0 (default), rooms are claimed on **entry only**: each
chr's last `rooms[0]` is tracked in `scenariodata_paint.lastroom[12]` (reset to
-1 on death, so a respawn counts as an entry) and a claim fires only when it
changes; with a non-zero Claim Time the chr must remain in the room for that
many seconds first (see the Claim Time section below). Continuous standing-in-room
claiming was removed — it flipped contested rooms every frame to whoever
iterated last in `g_MpAllChrPtrs` (the sims), so a human could never hold a
room a sim occupied. **Kills also claim**: `paintHandleDeath` (called from
`mpstatsRecordDeath` beside the `pacHandleDeath` hook) claims the killer's
current room for their team — the winner of a contested-room fight takes the
room. Suicides/unattributed deaths claim nothing. The ownership write is
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
most floor first. Because every member mirrors the same team tally,
`mpCalculateTeamScore` has a port-only override for `MPSCENARIO_PAINTROOM` that
returns the collective `teamcounts[teamnum]` as the team score instead of the
member sum (which multiplied the score by the member count). The match is
**time-limited**: `mpApplyLimits` forces the score and team-score limits to
unlimited for this scenario (`MPSCENARIO_PAINTROOM`) so a team owning many rooms
doesn't end the round early. Teams are locked on (forced in `paintInit`; the
teams checkbox is disabled like CTC/KoH in `menuhandlerMpTeamsEnabled`).

## "Claim Time" slider (`mpsetup.paintclaimtime`, proto 68, mpsetups v7)

A slider in the Graffiti options menu (below Owned Room Spawn): seconds
(**0..10, default 0**) a combatant must remain in a room before it flips to
their team. `0` = instant claim on entry — byte-identical to the original
entry-only behaviour. With a non-zero time, `paintTick` accumulates
`claimtime240[i] += lvupdate240` while the chr stays in the room (room change,
death and a successful claim reset it to 0) and claims when the threshold
(`paintclaimtime * TICKS(240)`) is reached. A room **already owned by the
chr's team** does not accumulate.

**Contested pause (the KoH dual-occupancy rule):** while living combatants
from more than one team are in the room (`paintRoomContested`), nobody's
timer advances — progress is held, not reset, exactly like KoH's hill timer.
A contested room is therefore won by clearing it out: **kill claims stay
instant** (`paintHandleDeath` is unchanged), so the winner of the fight takes
the room on the spot.

**HUD countdown ("s.hh", top centre):** with a non-instant Claim Time,
`paintRenderHud` shows the time remaining before the local player's current
room flips — only while alive in a claimable room (valid + not already owned
by their team); it visibly freezes while the room is contested. To feed it on
net clients, the room/timer **tracking** in `paintTick` runs on every machine
(clients predict the timers off replicated positions); only the ownership
**writes** stay server-gated. When a client's local timer crosses the
threshold it holds at 0.00 until the host's claim arrives via
`SVC_PAINT_STATE` (so it can't wrap back to full and flicker). Client timers
can drift slightly from the host's (interp delay on others' positions, and
synced chrs never read as dead client-side so a corpse can pause a client's
display) — cosmetic only; ownership is authoritative.

Plumbing (the `kohstatichill` pattern):

- `u8 paintclaimtime` appended to `struct mpsetup` (`types.h`, port-only).
- Wire: appended as a `u8` after `htmstaticpad` in `SVC_STAGE_START` and
  `CLC_ADMIN_SETUP` (`NET_PROTOCOL_VER` 68). Claiming is host-only, so the
  sync is for client menu display + dedicated-admin pushes, not determinism
  (no RNG is consumed). `SVC_LOBBY_STATE` doesn't carry it (that message is
  display-only and doesn't carry the other static pins either).
- Wad: 4 bits at the setup-block tail (`mpsetupfileLoadWad/SaveWad`,
  `version >= 7`; `MPSETUP_VERSION` 6→7). The 80-byte block had 10 spare
  bits; 6 remain.

## "Owned Room Spawn" option (`MPOPTION_OWNEDROOMSPAWN`, bit 33)

A Combat Sim option toggle (the high-word checkbox in the Paint options menu,
`menuhandlerMpCheckboxPortOption`). **Default on** — the bit is set in
`mpInit`'s port-only defaults (beside `MPOPTION_FRIENDLYFIRE`), so fresh
setups start with it enabled; saved setups keep whatever they stored. When on,
`paintChooseSpawnLocation` (the
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
| `src/game/mplayer/scenarios/paintroom.inc` | New: the whole scenario (options menu incl. the Claim Time slider, init/initprops/tick + `paintRoomContested`, highlight, HUD, score, spawn, shared `paintSetRoomOwner`/`paintGetRoomOwner`, `g_MpPaintDirty`) |
| `src/game/mplayer/scenarios.c` | Register the scenario + overview entry; `#include` the `.inc`; `scenarioGetNameText` literal-name helper (port-only modes have no ROM lang string); always-show Teamwork group |
| `src/include/constants.h` | `MPSCENARIO_PAINTROOM 6`, `MPOPTION_OWNEDROOMSPAWN` (bit 33) |
| `src/include/types.h` | `struct scenariodata_paint` + port-only union member; `mpsetup.paintclaimtime` |
| `src/include/game/mplayer/scenarios.h` | `g_MpScenarioOverviews[7]` on port |
| `src/game/mplayer/mplayer.c` | `mpApplyLimits`: force score/team-score limits unlimited for paint; `mpInit`: OWNEDROOMSPAWN default on; wad tail: `paintclaimtime` (v7) |
| `src/game/mplayer/setup.c` | `menuhandlerMpTeamsEnabled`: lock teams on for paint |
| `port/src/mpsetups.c` | `MPSETUP_VERSION` 6→7 (paintclaimtime tail field) |
| `port/include/net/netmsg.h` / `port/src/net/netmsg.c` | `SVC_PAINT_STATE` write/read; `paintclaimtime` in `SVC_STAGE_START` / `CLC_ADMIN_SETUP` |
| `port/src/net/net.c` | dispatch case + on-change/heartbeat send in `netEndFrame` |
| `port/include/net/net.h` | `NET_PROTOCOL_VER 66` (paint state) / `68` (claim time); `paintGetRoomOwner`/`paintSetRoomOwner`/`g_MpPaintDirty` decls |

## Notes / limits

- Per-room, not per-tile. Per-tile Splatoon painting would need vertex splitting
  and dynamic vertex buffers — a much larger renderer lift, deferred.
- A room, once painted, never returns to unpainted (only re-owned), which is what
  makes the full-list wire format idempotent and removal-free.
- The HUD shows the local player's own team's room tally at the bottom centre
  (just below the per-team coverage bars), and — with a non-instant Claim
  Time — the current room's capture countdown ("s.hh") at the top centre; the
  in-game scoreboard and end-of-round ranking show every team's coverage via
  `numpoints`.
