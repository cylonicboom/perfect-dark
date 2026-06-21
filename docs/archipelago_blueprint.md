# Archipelago Support — Blueprint

> Status: **design / blueprint**. Nothing here is wired up yet. This document
> maps Perfect Dark onto the [Archipelago](https://archipelago.gg) multiworld
> randomizer: what we expose as **checks** (locations), what we accept back as
> **items**, the player-facing **options** (difficulties / cheat-times /
> challenges), and the exact **Lua + C hooks we need to add** to make it work.
> It builds directly on the shipped Lua layer (see
> [`luascripting.md`](luascripting.md) / [`luascripting_roadmap.md`](luascripting_roadmap.md)).

---

## 1. What Archipelago needs from a game

An Archipelago (AP) "world" is three things:

1. **Locations (checks).** Named spots the player can *reach/complete*. When a
   location is checked, the client tells the AP server, which routes the item
   that was placed there to whoever owns it (you or another player's world).
2. **Items.** Things the AP server sends *to* the game — these unlock the game's
   own content (weapons, levels, abilities, cheats). In a randomizer your own
   progression items are scattered across everyone's worlds, so the game must be
   able to **gate** content on "have I received item X?" rather than "did I earn
   it normally?".
3. **A goal / completion condition.** When met, the client reports victory.

Perfect Dark is an unusually good fit: it already has a rich, *persistent,
condition-gated* progression system (mission unlocks, timed cheat unlocks,
Combat-Sim challenges, firing-range medals, weapon discovery). Most of the work
is **surfacing** that existing state to a client, plus a thin **gate layer** so
AP can decide what's available.

---

## 2. Architecture

### 2.1 Where the client lives

The Lua sandbox in `src/game/luaai.c` deliberately **omits `io`, `os`,
`package`, `require`, and `debug`** (`luaai_open_safe_libs`, luaai.c:224-249) —
so a pure-Lua script **cannot open a socket or a file**. AP's reference protocol
is a WebSocket/TCP JSON link to the AP server. Therefore the connection itself
**must be a small C bridge**; the policy/logic can stay in Lua.

Recommended layout:

| Piece | Where | Role |
|---|---|---|
| `src/game/luaai_ap.c` (**new**) | auto-compiled (`src/game/*.c` glob, CMakeLists) | C socket + JSON-frame bridge; exposes an `ap.*` Lua table |
| `scripts/ap/client.lua` (**new**) | loaded from `scripts/init.lua` | AP protocol state machine, check/item mapping, written in Lua |
| `scripts/ap/data.lua` (**new**) | required by `client.lua` | the generated location/item id tables (mirrors the AP `apworld`) |
| New `pd.*` events + accessors | `luaai_api.c` / bridges | the missing hooks enumerated in §6 |

`ap.*` (C) only does transport + framing; `scripts/ap/*.lua` does everything
else, so the protocol can iterate without rebuilding the engine — matching the
existing "generate, don't hand-maintain / keep logic in Lua" principle.

### 2.2 The poll point

`luaTick()` (luaai_api.c:784) runs once per frame from `schedEndFrame`
(pdsched.c) at ~60 Hz, and already `luaaiEnsureState()`s so scripts run on the
title screen too. AP polling and item draining happen here via a new
`pd.on("tick", …)` event (today only `"draw"` fires every frame, and only while
the HUD renders — see §6.1). The C bridge does non-blocking socket service; Lua
reacts.

### 2.3 Item application vs. earned progress

Two distinct state surfaces, both already in `struct gamefile` (types.h:4114):

- **`g_GameFile.besttimes[stage][diff]`** — non-zero ⇒ stage completed on that
  difficulty (also the value the cheat-unlock checks read). Writing a sentinel
  time here is how you *grant a level/cheat unlock* from an AP item.
- **Combat-Sim feature flags** (`g_MpFeaturesUnlocked[80]`,
  `g_MpFeaturesForceUnlocked[40]`, challenge.c) + `firingrangescores[9]` +
  `weaponsfound[6]`.

AP must own a **gate** so received items map onto these without the player having
to legitimately earn them, while *checks* still fire only on legitimate
completion. Keep the two directions separate:

- **Check (game → AP):** fired by the new completion events (§6), guarded by the
  game's own "not cheated" rule (`endscreen.c` already only records a best time
  when `g_CheatsActiveBank0/1 == 0`, endscreen.c:1600). This means **you cannot
  cheat your way to a check** — important for AP integrity.
- **Item (AP → game):** a new `pd.grant_*` family (§6.4) writes the unlock
  state, flagged as AP-granted so it doesn't masquerade as an earned time on the
  file-select thumbnail.

### 2.4 Netplay interaction

AP is **single-world-per-save / solo-campaign oriented**. The campaign is
single-player or co-op; co-op is host-authoritative for objectives already
(`CLC_OBJECTIVE_DONE`, see net `CLAUDE.md`). Rule: **the AP client runs only on
the machine that owns the save** — in co-op that's the host. The bridge refuses
to send checks on a net client (`g_NetMode == NETMODE_CLIENT`), mirroring the
"server-authoritative for anything affecting game state" principle. AP traffic
uses its **own** socket, never the ENet game socket, so it can't perturb the
netcode.

---

## 3. Checks (Locations) — the catalog

Grouped by source. Counts assume NTSC-final. Each group is independently
toggleable in the AP options (§5) so a player can size the multiworld.

### 3.1 Mission completion (core) — up to 63

The 21 solo stages (`SOLOSTAGEINDEX_*`, constants.h:4046; `NUM_SOLOSTAGES`).
One check per **(stage, difficulty)** the player enabled.

| Granularity | Locations | Option |
|---|---|---|
| Complete each stage on **Agent** | 21 | always on |
| …also on **Special Agent** | +21 | `difficulties` |
| …also on **Perfect Agent** | +21 | `difficulties` |

Detection: new `"missioncomplete"` event (§6.2) at the best-time write site
(endscreen.c:1628). The 17 main + 4 bonus stages all flow through it. The
"cheats off" guard at endscreen.c:1600 already gates this.

> The 4 bonus stages (Skedar Ruins, MBR, Maian SOS, WAR, Duel,
> `SOLOSTAGEINDEX_SKEDARRUINS..DUEL`) are great optional checks behind a
> `bonus_stages` toggle.

### 3.2 Cheat-time checks ("challenges") — up to 25

Perfect Dark unlocks most cheats by completing a *specific stage on a specific
difficulty under a target time* (`g_Cheats[]`, cheats.c:36-137;
`struct cheat`, types.h:3155). These are the game's built-in speed/skill
challenges and make ideal optional checks.

- ~25 timed/completion cheat conditions (`CHEAT_HURRICANEFISTS`..`CHEAT_PSYCHOSISGUN`,
  excluding firing-range and port-only "Experiment" cheats).
- A check fires when the cheat's **unlock condition is newly satisfied** —
  `cheatIsUnlocked(id)` flips false→true (the engine already computes this at
  the endscreen, endscreen.c:1657).

Detection: new `"cheatunlock"` event (§6.2) emitted where the endscreen detects
a new unlock (`cheatinfo` flags 0x200/0x2000 paths, endscreen.c:1656-1664).

Option `cheat_time_checks`: off / on. When on, also exposes a per-cheat
sub-option to require **all three difficulties' timed cheats** for completionists.

### 3.3 Per-objective checks (granular) — variable

Each stage has up to 5 objectives, per-difficulty
(`struct objective.difficulties`, types.h:1774; `objectivesCheckAll`,
objectives.c:488). Firing a check per objective makes early game far more
check-dense (good for big multiworlds).

Detection: needs the **`"objective"` event** that the roadmap explicitly leaves
open (luascripting_roadmap.md §4b) — completion is spread across criteria types
(`criteria_roomentered`, `criteria_throwinroom`, `criteria_holograph`, …). The
clean single firing point is `objectivesCheckAll` when an entry transitions to
`OBJECTIVE_COMPLETE` (§6.3). This is the one genuinely new piece of detection
logic; everything else is a one-line emit at an existing site.

Option `objective_checks`: off / on (default off — it's the heaviest group).

### 3.4 Combat Simulator challenges — up to 30

`g_MpChallenges[]` (challenge.c:29; `struct challenge`, types.h:4302) — 30
predefined Combat-Sim scenarios, each completable at 1–4 players, that unlock MP
features/characters/stages.

Detection: new `"challengecomplete"` event (§6.2) at
`challengeConsiderMarkingComplete()` (challenge.c:860).

Option `challenge_checks`: off / on. Sub-option `challenge_playercount` to
require a specific player count (default: any).

### 3.5 Firing-range medals — up to 96

32 firing-range weapons (training.c:236-274), each with Bronze/Silver/Gold
(`FRDIFFICULTY_*`, constants.h:1093). A check per **(weapon, medal)** earned.

Detection: new `"firingrange"` event (§6.2) at `frSaveScoreIfBest()`
(training.c:84), passing weapon index + medal.

Option `firing_range_checks`: off / bronze-only (32) / all-medals (96).

### 3.6 Other easy wins (suggested extras)

| Check idea | Source / hook | Notes |
|---|---|---|
| **Weapon first-found** | `frSetWeaponFound()` (training.c:170), `weaponsfound[6]` | A check the first time each weapon is picked up in-game — natural exploration progression. |
| **Co-op stage clears** | `coopcompletions[3]` bitfield (gamefile.c) | One per (stage, coop-difficulty); only when co-op enabled. |
| **Special-weapon discoveries** | `GAMEFILEFLAG_FOUND*` (gamefile.c:386) for Timed/Proximity/Remote mines | Hidden-weapon checks. |
| **Carrington Institute training** | CI device/hologram/firing-range tutorials (training.c) | Tutorial-completion checks, very early-game. |
| **Cumulative kill milestones** | `pd.on("kill")` (already shipped) + a Lua counter | e.g. checks at 50/100/250/500 enemy kills across the run. Pure Lua, no engine change. |
| **Shots-fired / accuracy milestones** | `pd.on("weaponfire")` (shipped) | Pure-Lua counters. |
| **Room-discovery checks** | `pd.on("roomenter")` (shipped) | Mark a curated set of "landmark" rooms per stage as checks — pure Lua, zero engine change. |
| **Holograph / data-uplink objectives** | `criteria_holograph` (objectives.c:637) | Falls out of §3.3 once the `objective` event exists. |
| **Difficulty "first clear of the campaign"** | derived in Lua from `besttimes` | Milestone checks (beat all 17 on Agent, etc.). |

The **kill/weaponfire/roomenter** milestone checks are the cheapest possible
additions: they need **no engine changes at all** because those events already
ship — they're pure `scripts/ap/*.lua`. Worth landing first as a proof of life.

---

## 4. Items (AP → game)

What the AP server can hand back. Each maps to a `pd.grant_*` accessor (§6.4).

| Item | Effect | Backing state |
|---|---|---|
| **Stage unlock** | Makes a mission selectable | `besttimes[stage][DIFF_A]` sentinel (the menu gates on this, mainmenu.c) |
| **Difficulty unlock** | Special/Perfect Agent become selectable | gate flag (see §6.4) |
| **Weapon unlock (solo)** | Weapon available in solo loadout / found set | `weaponsfound[6]` / FNFLAG gates |
| **Cheat as item** | Grant a cheat directly (e.g. Cloaking Device, FarSight, All Guns) | `cheatActivate()` / unlock via `besttimes` sentinel |
| **Classic firing-range weapon** | PP9i/CC13/… unlocked | `firingrangescores` set to gold for the gating weapons, or a direct unlock flag |
| **Combat-Sim feature** | character / scenario / 8-bots / stage | `g_MpFeaturesUnlocked[]` |
| **Trap items** (optional) | DK Mode, Small Jo, Marquis (one-hit), Enemy Rockets, Perfect Darkness | `cheatActivate()` for a timed nuisance — classic AP "trap" flavour |
| **Filler** | extra ammo / shield top-up on receipt | `pd.spawn` / `pd.chr_set_shield` (shipped) |

Traps are a natural fit because PD's cheat list already contains
self-sabotaging modes (`CHEAT_DKMODE`, `CHEAT_SMALLJO`, `CHEAT_MARQUIS`,
`CHEAT_ENEMYROCKETS`, `CHEAT_PERFECTDARKNESS`). `cheatActivate(id)` (cheats.c:279)
flips them on live.

**Goal options:** beat a chosen final stage on a chosen difficulty
(default: Skedar Ruins / Special Agent), or "all 17 main missions on Agent", or
"N% of all checks". Victory reported from Lua when the goal predicate over
`besttimes`/checks is satisfied.

---

## 5. Player options (the AP `.yaml`)

These ride in the AP world definition (the `apworld`, authored separately) and
are read by `scripts/ap/client.lua` from the server's `slot_data`:

| Option | Values | Drives |
|---|---|---|
| `difficulties` | agent / +special / +perfect | which mission checks exist (§3.1) |
| `bonus_stages` | off / on | include the 4 bonus stages |
| `cheat_time_checks` | off / on / all-difficulties | timed-cheat challenges (§3.2) |
| `objective_checks` | off / on | per-objective granularity (§3.3) |
| `challenge_checks` | off / on (+`challenge_playercount`) | Combat-Sim challenges (§3.4) |
| `firing_range_checks` | off / bronze / all-medals | firing-range medals (§3.5) |
| `milestone_checks` | off / on | kill/room/weapon milestones (§3.6) |
| `weapon_logic` | vanilla / shuffled / starting-pistol-only | how weapon items gate solo play |
| `traps` | off / low / med / high | proportion of trap items |
| `goal` | final-stage / full-campaign / percent | victory condition (§4) |
| `death_link` | off / on | AP DeathLink: `pd.on("kill")` on player death → broadcast; inbound kills the player |

`death_link` is almost free: outbound uses the existing player-death path; inbound
needs a `pd.kill_player([n])` accessor (a thin wrapper over `playerDie`).

---

## 6. Lua API additions required ("add it if it's missing")

This is the concrete list of engine-side additions. Everything is **additive,
port-only (`#ifndef PLATFORM_N64`), cosmetic/local except where it writes
gamefile state (server-only)**, and follows the existing emitter pattern in
`luaai_api.c`.

### 6.1 `"tick"` event (every frame, everywhere)

Today the only per-frame Lua callback is `"draw"`, fired inside `luaHudRender`
(luaai_api.c:839) — i.e. only when the HUD renders. AP polling must run even on
menus/loading. Add a `"tick"` event dispatched from `luaTick()` (luaai_api.c:784,
which already runs unconditionally each frame).

```c
/* luaTick(), after luaaiEnsureState(): */
luaEventDispatchInts("tick", 0, NULL);
```

Handler: `pd.on("tick", function() … end)`. ~2 lines.

### 6.2 New completion events (one-line emits at existing sites)

Each is a `luaEmit*` added to `luaai_api.c` (declared in `luaai.h`) plus a single
call at the engine site. Server-only where it reflects authoritative progress.

| Event | Args | Emit site |
|---|---|---|
| `"missioncomplete"` | `(stageindex, difficulty, secs, cheated)` | endscreen.c:1628 (best-time write) |
| `"cheatunlock"` | `(cheatid)` | endscreen.c:1657 (new-unlock detection) |
| `"challengecomplete"` | `(challengeindex, numplayers)` | `challengeConsiderMarkingComplete()`, challenge.c:860 |
| `"firingrange"` | `(weaponindex, medal)` | `frSaveScoreIfBest()`, training.c:84 |
| `"weaponfound"` | `(weaponnum)` | `frSetWeaponFound()`, training.c:170 |

Pattern (mirrors `luaEmitKill`, luaai_api.c:748):

```c
void luaEmitMissionComplete(s32 stageindex, s32 difficulty, s32 secs, s32 cheated)
{
    lua_Integer a[4] = { stageindex, difficulty, secs, cheated };
    luaEventDispatchInts("missioncomplete", 4, a);
}
```

### 6.3 `"objective"` event (the one new detection point)

The roadmap defers this because completion is scattered (luascripting_roadmap.md
§4b). The clean approach: in `objectivesCheckAll()` (objectives.c:488), which
already iterates every objective each frame and tracks status transitions, emit
when an objective's status goes `!= OBJECTIVE_COMPLETE` → `OBJECTIVE_COMPLETE`:

```c
/* inside objectivesCheckAll, when status transitions to complete: */
luaEmitObjective(objindex, OBJECTIVE_COMPLETE);
```

Args `(objectiveindex, status)`. Track previous status in a small static/array
keyed by index so it's edge-triggered. This also feeds §3.3 and the holograph /
throw-in-room checks for free. Medium effort (needs the transition latch), but
contained to one function.

### 6.4 Gamefile read + grant accessors (the gate layer)

Read-only queries (any machine) and server-only grants. Backed by small bridges
next to the existing `chraiLua*` accessors so `luaai_api.c` stays struct-free.

```lua
-- reads (decisions / goal predicate / check derivation)
pd.mission_time(stageindex, difficulty)   -> secs (0 = not done)
pd.mission_done(stageindex, difficulty)   -> bool
pd.cheat_unlocked(cheatid)                -> bool        -- wraps cheatIsUnlocked
pd.cheat_active(cheatid)                  -> bool        -- wraps cheatIsActive
pd.challenge_done(index, numplayers)      -> bool
pd.firing_medal(weaponindex)              -> 0..3
pd.weapon_found(weaponnum)                -> bool
pd.mp_feature_unlocked(featureid)         -> bool
pd.difficulty()                           -> 0..3        -- wraps lvGetDifficulty
pd.stage_index()                          -> current SOLOSTAGEINDEX

-- grants (AP item application; server-only, AP-flagged)
pd.grant_mission(stageindex, difficulty)             -- besttimes sentinel
pd.grant_cheat(cheatid)                               -- unlock + optional activate
pd.activate_cheat(cheatid) / pd.deactivate_cheat(id)  -- cheatActivate/Deactivate (live traps)
pd.grant_firing_weapon(weaponnum)
pd.grant_weapon_found(weaponnum)
pd.grant_mp_feature(featureid)
pd.kill_player([n])                                   -- DeathLink inbound
```

Notes:
- Grants that write `besttimes` must set an **AP-granted** marker (a new bit in
  `g_GameFile.flags[10]`, which has spare bits) so the file-select thumbnail and
  "first completion" logic don't treat an AP unlock as an earned time.
- `cheatActivate`/`Deactivate` already exist (cheats.c:279/325) — the Lua
  wrappers are thin and enable live trap items.
- All grants gated `g_NetMode != NETMODE_CLIENT`.

### 6.5 The `ap.*` transport bridge (`luaai_ap.c`, new file)

Minimal non-blocking socket surface; JSON stays in Lua (the AP text protocol is
line/JSON framed — parse with a tiny pure-Lua JSON lib in `scripts/ap/`).

```lua
ap.connect(host, port)   -> bool      -- non-blocking connect
ap.status()              -> "disconnected"|"connecting"|"connected"
ap.poll()                -> string|nil -- next inbound frame (call from "tick")
ap.send(text)            -> bool       -- queue an outbound frame
ap.disconnect()
```

Platform sockets behind `#ifdef _WIN32` (winsock) / POSIX, same split the port
already uses elsewhere. ENet is **not** reused (keeps AP isolated from netplay).
WebSocket vs. raw TCP: the AP server speaks WebSocket; either implement a tiny
WS handshake+framing in C here, or connect through a local text proxy — decide at
implementation time (a ~150-line WS client in C is the self-contained option).

---

## 7. Implementation phases (value-to-effort)

1. **Proof of life (pure Lua, no engine change).** `scripts/ap/` with the
   milestone checks (kills/rooms/weaponfire) using the **already-shipped** events,
   plus a stub `ap.*` that logs to console. Proves the mapping/state-machine
   design end to end before any C.  *(zero risk)*
2. **`"tick"` event + `ap.*` socket bridge** (`luaai_ap.c`). Real connection to a
   local AP server; send/receive frames; drain items in `"tick"`. §6.1, §6.5.
3. **Mission + cheat checks.** `"missioncomplete"` / `"cheatunlock"` events +
   `besttimes` read/grant accessors. Covers the core randomizer (§3.1, §3.2, §4).
4. **Challenges + firing range + weapons-found.** §3.4, §3.5, §3.6 events +
   accessors. Combat-Sim feature grants.
5. **Per-objective checks.** The `"objective"` transition event (§6.3) — the only
   piece needing new detection logic.
6. **Polish:** DeathLink, traps, goal variants, co-op host-only gating, the
   AP-granted thumbnail marker.

Each phase is independently shippable and testable (phase 1 needs no ROM at all,
matching the `tools/luaai_test` "validate without the ROM" philosophy).

---

## 8. Risks / open questions

- **WebSocket in C.** The single largest unknown. A minimal WS client (RFC 6455
  handshake + text frames, no TLS for a LAN/`localhost` AP host) is ~150 lines;
  TLS (for `archipelago.gg`-hosted rooms) would need an SSL dep. Mitigation:
  support `ws://` + localhost first; document a proxy for remote rooms.
- **Save coupling.** AP grants write `g_GameFile`; we must not corrupt vanilla
  saves. Use the spare `flags[10]` bit to mark AP runs and consider a separate
  save slot for AP playthroughs.
- **Co-op authority.** Only the host runs the client; clients must not double-fire
  checks. Gate on `g_NetMode != NETMODE_CLIENT` (already the rule for state
  writes).
- **Cheat integrity.** Keep the existing "no best time while cheats active" guard
  so AP checks can't be cheesed — but allow **AP-granted** cheats (items) to coexist
  without blocking checks (they belong in the ENABLED-only / Experiment-style
  bank that never sets `g_CheatsActiveBank*`, cheats.c). Decide which received
  cheats count as "active" for the guard.
- **ID stability.** Location/item ids in `scripts/ap/data.lua` must match the
  `apworld`; generate both from one source (cheats/stage/challenge tables) so they
  can't drift — same rule as `gen_aicommands.py`.

---

## 9. File-change summary (for the eventual PR)

| File | Change |
|---|---|
| `src/game/luaai_api.c` | `"tick"` dispatch; `luaEmit{MissionComplete,CheatUnlock,ChallengeComplete,FiringRange,WeaponFound,Objective}`; register `pd.*` read/grant fns |
| `src/include/game/luaai.h` | declare the new emitters + bridge accessors |
| `src/game/endscreen.c` | emit `missioncomplete` + `cheatunlock` at the existing best-time/unlock sites |
| `src/game/objectives.c` | edge-triggered `objective` emit in `objectivesCheckAll` |
| `src/game/challenge.c` | emit `challengecomplete` in `challengeConsiderMarkingComplete` |
| `src/game/training.c` | emit `firingrange` + `weaponfound` |
| `src/game/cheats.c` / `gamefile.c` | grant helpers + AP-granted marker bit |
| `src/game/luaai_ap.c` | **new** — `ap.*` socket/WS bridge |
| `scripts/init.lua` | load `scripts/ap/client.lua` |
| `scripts/ap/*.lua` | **new** — client state machine, id tables, JSON |
| `docs/archipelago_blueprint.md` | this document |

---

*Authored against `port-net-predict`. Cross-references:
[`luascripting.md`](luascripting.md), [`luascripting_roadmap.md`](luascripting_roadmap.md),
`port/src/net/CLAUDE.md`, `src/include/CLAUDE.md`.*
