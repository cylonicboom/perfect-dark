# Port-only: Global Lives system (Limits menu)

> **History:** this began as the ninth Combat Sim scenario, "Elimination"
> (`MPSCENARIO_ELIMINATION 8`, mpsetups v8, proto 70) and was generalised to
> a **scenario-independent global feature** before release (proto 71,
> mpsetups v9). The scenario is gone; the retired **v8 tail high-bit scheme**
> it pioneered for scenario ids 8-15 is documented at the bottom — read that
> before ever adding a 9th scenario.

A "limit" like the time / score limits: when **Lives** (Limits menu, **0 =
Off default, 1-9** — the co-op F3 range) is set, every combatant starts with
N lives and every death spends one. Out of lives = **no respawn**
(eliminated). When at most one faction remains, the match ends. Works with
**any scenario** — Combat + Lives is classic last-man-standing; KotH/CTC/
Zones etc. keep their own objectives and scoring with attrition layered on.

## Lives modes

- **Solo** (`elimlivesmode 0`, default): each combatant has their own pool
  of N. Free-for-all and team games alike (a team is out when every member
  is eliminated; with teams off every combatant is their own faction).
- **Team** (`elimlivesmode 1`): one shared pool per team of **exactly N** —
  the setting is the team's total, regardless of member count (deliberately
  NOT co-op's `count × members` scaling). Any member's death spends from the
  pool; while it lasts anyone can respawn; at 0 every dead member is out.
  Degrades to Solo when teams are disabled. The dropdown is greyed out while
  Lives is Off.

## Mechanics

All state lives in `struct elimdata g_ElimData` (`elimination.inc`) —
**deliberately NOT a `g_ScenarioData` union member**: lives run alongside
any scenario, and the union slot belongs to the active one.

- **Hooks**: although not a scenario, the system rides the scenario
  dispatchers in `scenarios.c` (same TU as the `.inc`): `elimReset` from
  `scenarioInitProps` (every stage load, both roles), `elimTick` from
  `scenarioTick`, `elimRenderHud` from `scenarioRenderHud` (with its own
  copy of the hud framing — the active scenario may have no hudfunc). Each
  is internally gated on the Lives setting.
- **Seeding**: lazily on the first tick with combatants present
  (`g_MpAllChrPtrs` isn't populated until `mpStartMatch` finishes). Records
  `startfactions`; the end check only arms when ≥ 2.
- **Spending**: `elimHandleDeath` from `mpstatsRecordDeath` — kills,
  suicides, unattributed deaths all count. Host-only.
- **Respawn gates** (`elimChrCanRespawn`, returns true while Lives is Off):
  players in `player.c`'s MP dead branch (server-authoritative — net clients
  only send `UCMD_RESPAWN`); bots in `chrTickDead`'s corpse-fade respawn —
  an out-of-lives bot stays as a faded corpse, **never deleted** (the chr
  pointer arrays must stay valid).
- **Match end**: `elimShouldEndMatch()` (lives on + started with ≥2 factions
  + ≤1 remains) polled from **`lv.c`'s match-end block** beside the
  score-limit checks. **Gotcha:** `g_NumReasonsToEndMpMatch` is recomputed
  from ZERO there every frame — an increment made from a scenario tick gets
  wiped before the end test reads it (the first build had exactly this bug).
  Never increment it outside that block.
- **Scoring**: none of its own — the active scenario's scoring stands
  (an earlier survival-points design was dropped when lives went global: it
  would have polluted numpoints-based scenarios like KotH/HTB/Graffiti).
- **HUD**: remaining lives (the shared pool in Team mode) **top left** (the
  top centre belongs to scenario HUDs) in the team colour; "OUT" once
  eliminated. Net clients keep auto-spectate-on-death while eliminated.
- **Challenges**: `mpApplyConfig` force-zeroes both fields — the whole-struct
  copy from challenge config data predates the port fields, and a garbage
  value would silently enable lives in challenges.
- Defaults (`elimlives = 0`/Off) live in `func0f187fec`, which covers both
  boot (`mpInit`) and the Limits menu's **Restore Defaults**.

## Persistence (mpsetups v9)

`elimlivesmode` (1 bit) + `elimlives` (4 bits) at the wad tail, `version >=
9`. v8 files carry the retired 1-bit scenario high-bit at the same offset —
the v8 read consumes and discards it (a saved Elimination-scenario setup
loads as Combat; its lives settings are gone — acceptable, v8 never shipped
beyond dev testing). **The 80-byte block has 1 spare bit (639/640)** — the
next saved field needs `MPSETUP_BLOCKSIZE` enlarged + a version-aware
`mpsetupDeserialize` + import-path tail zeroing.

## Networking (`SVC_ELIM_STATE` 0x57, proto 71; wire keying fixed proto 73)

`{u8 lives[12], u8 teamlives[8], u16 elimmask}` — reliable, on-change
(`g_MpElimDirty`) + 1s heartbeat at phase 25, gated on
`normmplayerisrunning && elimlives > 0` (any scenario). **The per-combatant
slices (lives + the eliminated mask) are wire-keyed** (humans by netclient
ID, bots by mpchr index — `netChrArrayToWire`/`FromWire`, see PORT_RACE.md /
the net CLAUDE.md gotcha): raw local slots differ per machine, and shipping
them raw made clients read the HOST's lives as their own (fixed at proto
73; team pools are team-indexed and were always wire-stable). Client apply
(`elimApplyWireState`) overwrites lives/pools/the eliminated set.
`g_MpSetup.elimlivesmode`/`elimlives` ride `SVC_STAGE_START` and
`CLC_ADMIN_SETUP` after the Zones fields. Proto 71 = same wire fields as 70
but global gate semantics + scenario id 8 removed (mixed versions must not
join).

## Files

| File | Change |
|---|---|
| `src/game/mplayer/scenarios/elimination.inc` | The whole system (Limits-menu handlers, reset/seed/tick, death hook, respawn gate, end predicate, HUD, wire accessors, `g_ElimData`, `g_MpElimDirty`) |
| `src/game/mplayer/scenarios.c` | `elimReset`/`elimTick`/`elimRenderHud` hooked into the scenario dispatchers; `#include` |
| `src/game/mplayer/setup.c` | "Lives" slider + "Lives Mode" dropdown in `g_MpLimitsMenuItems` |
| `src/include/types.h` | `struct elimdata` (standalone); `mpsetup.elimlivesmode`/`elimlives` |
| `src/include/game/mplayer/scenarios.h` | menu handler + `elimReset`/`elimTick`/`elimRenderHud` decls |
| `src/include/constants.h` | scenario-index note (7 = last; hi-bit scheme retired, see below) |
| `src/game/mplayer/mplayer.c` | `func0f187fec` defaults; wad v9 tail; `mpApplyConfig` challenge guard |
| `src/game/mpstats.c` | `elimHandleDeath` hook (gated on `elimlives > 0`) |
| `src/game/lv.c` | `elimShouldEndMatch` polled in the match-end block |
| `src/game/player.c` / `src/game/chraction.c` | respawn vetoes via `elimChrCanRespawn` |
| `port/src/mpsetups.c` | `MPSETUP_VERSION` 9 |
| net (`netmsg.h/.c`, `net.c`, `netmenu.c`, `net.h`) | `SVC_ELIM_STATE`; setup fields in stage-start/admin-setup; proto 71; scenario name tables trimmed back to 8 |

## Appendix: the retired v8 scenario high-bit scheme (for a future 9th scenario)

The mpsetups wad stores the scenario as a **3-bit mid-stream field** and
blocks are **never re-encoded** (saving restamps the file version while old
blocks keep their original layout), so the field cannot be widened — only
**tail appends** are safe. The Elimination scenario (id 8) proved this
scheme before being retired:

- Write `id & 7` in the old field + the 4th bit as a 1-bit tail append.
- At parse time the block dispatches as `id & 7`, so ids 8-15 are only safe
  when the alias row has **no `initfunc` side effects and a NULL
  `readsavefunc`** (8→Combat was safe; 12→KoH and 15→Zones would consume/
  clobber the aliased scenario's save slot). The slot must be captured raw
  in the default branch (`g_ScenarioSaveSlotRaw` — since removed) and
  re-applied after the tail reveals the true id, followed by a re-run of
  `scenarioInit()`.
- `mpsetupfileGetOverview` (the saved-setups list label) parses only the
  block head and shows the aliased name.

With v9 occupying the tail bit, a 9th scenario now ALSO needs the
`MPSETUP_BLOCKSIZE` enlargement. Consider that migration directly instead.
