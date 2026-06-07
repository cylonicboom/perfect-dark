# Port-only Combat Sim mode: "Zones" (`MPSCENARIO_ZONES`)

TimeSplitters 2 style territory control built on the King of the Hill
framework. **Every KoH hillpad on the stage is an active zone simultaneously**
(4-7 per stage — the same `INTROCMD_HILL` pads KotH randomises between).
Standing in a zone flips it to your team's colour; a repeating **score cycle**
counts every zone each time it expires — each team scores **+1 per zone it
owns** — then the countdown resets. The match ends on the **team score limit
or the time limit**.

This is the eighth scenario, appended after Graffiti. Entirely port-only
(`#ifndef PLATFORM_N64`); the N64 build is byte-identical. **It also takes the
last free scenario index** — the wad stores the scenario in a 3-bit field, so
a 9th scenario needs a wad-format migration (noted in `constants.h`).

## How it works

### Zone collection + highlight

`scenarioReset`'s `INTROCMD_HILL` parser feeds `zonesAddHill` (the `kohAddHill`
pattern, but into `g_ScenarioData.zones` — zones and KotH are union members
and can't be active at once). `zonesInitProps` resolves each hillpad's room
(`padUnpack PADFIELD_ROOM`) and highlights **all** of them from match start
via `roomSetLightOp(LIGHTOP_HIGHLIGHT)`. `zonesHighlightRoom` tints neutral
zones the KoH unoccupied-hill green (×0.25, ×1, ×0.25) and owned zones the
owning team's colour (the Graffiti tint math). Owner flips just set
`ROOMFLAG_BRIGHTNESS_DIRTY_TEMP` for the reshade.

### Capture (host-authoritative, the Graffiti pattern)

`zonesTick` tracks each combatant's current zone (`lastzone[12]`, via
`prop->rooms[0]` against the zone rooms — the KotH occupancy test). With
**Capture Time** at 0 (default) a zone flips on **entry**; with a non-zero
time the combatant must **hold** the zone for that many seconds
(`capturetime240[12]`), and the hold timer **pauses KoH-style while another
team is also in the zone** (`zonesZoneContested`) — contested zones are won
by clearing them out. Tracking runs on every machine (clients predict
locally, holding at the threshold until the host's flip arrives); ownership
**writes** (`zonesSetZoneOwner`) are gated to the server. Flips are
deliberately **silent** — with all zones active at once a per-flip cue is
noise; the room re-tint, HUD squares and radar dots carry the information
(only the cycle award plays a sound).

### Score cycle

`zones->cycle240` counts down from the **Score Interval**; on expiry the
server awards `teamscores[t] += zones owned by t`, plays the KoH score cue,
resets the countdown and raises the broadcast flag. Clients tick the
countdown locally for display (clamping at 0) and resync from the broadcast.
Every member's `numpoints` mirrors `teamscores[team]` each tick (the Graffiti
pattern), with:

- `mpCalculateTeamScore` override returning `teamscores[teamnum]` (so the
  team total isn't multiplied by the member count), and
- `mpApplyLimits` forcing the **per-player** score limit unlimited for Zones
  (it would trip at the team value) while keeping the **team score limit and
  time limit** active — those end the match, per the design.

Teams are locked on (`zonesInit` forces `MPOPTION_TEAMSENABLED`;
`menuhandlerMpTeamsEnabled` disables the checkbox like CTC/KoH/Graffiti).

### Options menu

Standard six toggles plus a radar checkbox and two literal-text sliders:

- **Zones on Radar** — reuses `MPOPTION_KOH_HILLONRADAR` (the scenarios are
  mutually exclusive); default on via `mpInit`.
- **Score Interval** — seconds per score cycle, **5..60, default 10**
  (`g_MpSetup.zonescoretime`, sanitized through `zonesGetInterval240`).
- **Capture Time** — seconds to hold a zone before it flips, **0..10,
  default 0 = Instant** (`g_MpSetup.zonecapturetime`).

### HUD + radar

- Score-cycle countdown top centre, whole seconds in the KoH numeric font
  (green), rounded up so it reads the full interval right after a reset.
- **Per-zone ownership squares** bottom centre (the spot Graffiti's coverage
  bars occupy): one 3×3 square per zone in zone-index order, coloured by
  owner; neutral zones use the hill green. Graffiti-bars drawing pattern
  (`textSetPrimColour` + `gDPFillRectangle` with `text0f153838` flushes).
- **Radar dots** (`zonesRadarExtra`, the `kohRadarExtra` pattern): one dot
  per zone at its hillpad position (`zonepos[9]`, captured in
  `zonesInitProps`), owner-coloured, neutral = green.

## Persistence (no wad version bump)

Both sliders pack into the **32-bit per-scenario save slot** — the same slot
KotH uses for `mphilltime` (`scenarioReadSave`/`scenarioWriteSave`, one slot
per saved setup, dispatched to the active scenario). Bits 0-3 =
`zonecapturetime`, bits 4-9 = `zonescoretime`; values are sanitized on load.
`MPSETUP_VERSION` stays at 7 and the 80-byte block budget (6 spare bits) is
untouched.

## Networking (`SVC_ZONES_STATE` 0x56, proto 69)

`{u8 count, u8 owners[count], u16 teamscores[8], u16 cycle240}` — reliable,
broadcast **on change** (`g_MpZonesDirty`: zone flips + cycle awards) plus a
**1s heartbeat** at phase 20 of `NET_HEARTBEAT_INTERVAL` (free slot: KoH 0,
prop-reconcile 10/40, score 15, lobby 30, paint 35, stats 45, timescale 50).
Zone indices are wire-stable because both sides collect the same hillpads
from stage data. The client apply (`zonesApplyWireState`) routes owners
through the shared setter (re-tint + capture cue), overwrites the team scores
(edge-detecting an increase to play the score cue once), and resyncs the
countdown. `g_MpSetup.zonescoretime`/`zonecapturetime` also ride
`SVC_STAGE_START` and `CLC_ADMIN_SETUP` after `paintclaimtime` (menu display +
dedicated-admin pushes; capture/scoring runs host-side only, no determinism
concerns — no RNG is consumed).

## Files

| File | Change |
|---|---|
| `src/game/mplayer/scenarios/zones.inc` | New: the whole scenario (options menu + sliders, init/initprops/addhill/tick, contested test, highlight, HUD, score, save slot, `zonesGetData`/`zonesApplyWireState`/`zonesSetZoneOwner`, `g_MpZonesDirty`) |
| `src/game/mplayer/scenarios.c` | Register vtable row + overview entry; `#include` the `.inc`; "Zones" in `scenarioGetNameText`; `scenarioReset` hillcount clear; `INTROCMD_HILL` → `zonesAddHill` |
| `src/include/constants.h` | `MPSCENARIO_ZONES 7` + last-free-index warning |
| `src/include/types.h` | `struct scenariodata_zones` + union member; `mpsetup.zonescoretime`/`zonecapturetime` |
| `src/include/game/mplayer/scenarios.h` | `g_MpScenarioOverviews[8]` on port |
| `src/game/mplayer/mplayer.c` | `mpInit` defaults (10s / instant); `mpApplyLimits` player-score-limit override; `mpCalculateTeamScore` override |
| `src/game/mplayer/setup.c` | `menuhandlerMpTeamsEnabled`: lock teams on for Zones |
| `port/include/net/netmsg.h` / `port/src/net/netmsg.c` | `SVC_ZONES_STATE` write/read; setup fields in `SVC_STAGE_START` / `CLC_ADMIN_SETUP`; lobby name table |
| `port/src/net/net.c` | dispatch case + on-change/heartbeat send in `netEndFrame` |
| `port/src/net/netmenu.c` | admin scenario dropdown + browser name table (+"Graffiti", which both were missing) |
| `port/include/net/net.h` | `NET_PROTOCOL_VER 69`; `zonesGetData`/`zonesApplyWireState`/`g_MpZonesDirty` decls |

## Notes / limits

- Zone granularity is the **room containing the hillpad** (the KotH occupancy
  model, `rooms[0]` equality). Two hillpads sharing a room would act as one
  zone (first index wins); no stock stage does this.
- No per-zone capture-progress HUD yet (the Graffiti "s.hh" countdown could
  be ported over for non-instant Capture Time).
- Kills do **not** claim zones (unlike Graffiti) — captures are purely
  presence-based, like TS2.
