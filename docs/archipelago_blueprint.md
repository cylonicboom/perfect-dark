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
| `mission_order` | **shuffle (default)** / campaign | **shuffle** decouples stage access from the vanilla order — every stage is an AP item, placed anywhere in the multiworld; you progress as unlocks arrive. `campaign` keeps the vanilla beat-N-⇒-N+1 chain and shuffles only *within* levels. See §9.2 #1. |
| `difficulties` | agent / +special / +perfect | which mission checks exist (§3.1) and which difficulty unlocks are items (§5/§9.2 #5) |
| `bonus_stages` | off / on | include the 4 bonus stages in the shuffle pool |
| `cheat_time_checks` | off / on / all-difficulties | timed-cheat challenges (§3.2) |
| `objective_checks` | off / on | per-objective granularity (§3.3) |
| `challenge_checks` | off / on (+`challenge_playercount`) | Combat-Sim challenges (§3.4) |
| `firing_range_checks` | off / bronze / all-medals | firing-range medals (§3.5) |
| `milestone_checks` | off / on | kill/room/weapon milestones (§3.6) |
| `weapon_logic` | vanilla / shuffled / starting-pistol-only | how weapon items gate solo play |
| `traps` | off / low / med / high | proportion of trap items |
| `goal` | all-missions (default) / final-stage / percent | victory condition (§4) |
| `death_link` | off / on | AP DeathLink: `pd.on("kill")` on player death → broadcast; inbound kills the player |

**Mission order is shuffled by default.** Each of the (enabled-difficulty) stages
is a progression **item**; the campaign opens up as those items arrive from the
multiworld, so the play order is randomized (you might get Skedar Ruins before
Defection — expected for a randomizer). `mission_order: campaign` is the
opt-in for players who want the canonical story order with only intra-level
shuffle. The default `goal` is **all-missions** (clear every enabled stage on its
highest enabled difficulty), which suits a stage-shuffle seed better than a single
final-stage goal.

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

## 9. Gating — what we *lock* until items arrive

§3-4 cover *checks* (what the player completes) and *items* (what AP sends
back). This section is the other half the design needs to actually be a
randomizer: **the locks** — the engine points where we withhold content so the
player stalls until the matching item is received. Without these, AP items have
nothing to unlock and the seed isn't beatable-in-order.

### 9.1 Two gating strategies

There are two complementary ways to lock content, and the right choice depends on
whether the thing being locked is **systemic** (a whole subsystem) or
**level-scripted** (one event baked into a stage's bytecode).

**A) Engine gate points (C).** A small set of existing decision functions already
answer "is the player allowed to X?". We add **one** AP-aware branch to each:
when an AP run is active, consult an **AP unlock set** instead of (or on top of)
the vanilla condition. Few, well-defined sites; covers the systemic gates
(stage-select, weapon use, device use). The AP unlock set is a new
port-only bitset (e.g. `g_ApUnlocks`), written by the `pd.unlock_*` grants
(§6.4) and read by the gate points; defaults to "all locked" in AP mode.

**B) Action-block overrides (Lua, no engine change).** Per-level scripted
progression — a door that opens after a cutscene, a reinforcement wave, the
`end_level` trigger — lives in the stage's **background (`0x10xx`) /
environmental (`0x14xx`) ailist** (see `docs/ailists.md`). `pd.register_ailist`
already lets us override these on host/solo only (`chraiLuaOverridesAllowed`,
chrai.c). An override that **declines to run** the gated command — `return 1`
(yield) every frame, then delegate to the real list via `ctx:exec` only once the
AP item is in — freezes that scripted event indefinitely. **Zero C changes**, and
it's already server-authoritative. This is the deep, optional layer (intra-level
item logic); strategy A is the spine.

> The override path reuses the **`g_StageFlags`** sentinel system the levels
> already use for their own progression (`set_stage_flag 0x00A1` /
> `if_stage_flag_eq 0x00A3`, chraction.c). An AP override can simply refuse to
> set a stage flag until the item arrives, and every downstream `if_stage_flag_eq`
> in that level's script stays blocked — i.e. we gate *one* flag and the level's
> own logic does the rest.

### 9.2 The gate catalog

Ordered by value-to-effort. "Vector" = which strategy; "Lever" = the exact
field/function to flip.

| # | Gate | Vector | Lever (engine site) | Notes |
|---|---|---|---|---|
| 1 | **Stage + difficulty access** | A | `isStageDifficultyUnlocked()` (mainmenu.c:1026) | **The spine — full stage shuffle is the default.** Vanilla derives access from the `besttimes` chain ("beat N ⇒ N+1 unlocks"); in the default `mission_order: shuffle` AP mode this function returns true iff the AP unlock set holds this `(stage,difficulty)`, so each stage becomes a placed item and the play order is randomized. One added branch gates the whole campaign. (`mission_order: campaign` leaves the vanilla chain intact and shuffles only intra-level.) |
| 2 | **Weapon use** | A | `objTestForPickup()` (propobj.c:18042) to refuse the pickup; **and/or** `bgunPrimaryFunctionDisabled()` (bondgun.c:3456) to deny fire | Pickup-deny = "can't even hold it"; fire-deny = "holds but can't shoot". Pick one per `weapon_logic` option. Both already exist as choke points. |
| 3 | **Starting loadout** | A | intro-weapon loop in `playerLoadDefaults()` (player.c:705) | Skip `INTROCMD_WEAPON` grants for not-yet-unlocked guns ⇒ start missions with pistol only. |
| 4 | **Gadgets / devices** | A | `currentPlayerSetDeviceActive()` (game_0b0fd0.c:388) — refuse to set the `devicesactive` bit | Single point for all 11 devices (Night Vision, IR/X-Ray, Cloak, Eyespy, R-Tracker, …). Locked device just won't toggle on. |
| 5 | **Difficulty selection** | A | same `isStageDifficultyUnlocked` path / difficulty menu | Special/Perfect Agent as progressive items (`difficulties` option). |
| 6 | **Doors (key/lock)** | A or B | `doorIsUnlocked()` / `door->keyflags` (propobj.c:19669); padlock via `doorIsPadlockFree` | Treat PD's own `keyflags` as AP keys: AP can hold a door locked (`keyflags != 0` + no key) until the "keycard" item arrives, then clear it. Or do it from a Lua override (B). |
| 7 | **Lifts / elevators** | A or B | `OBJFLAG_LIFT_TRIGGERDISABLE` on the `liftobj` (propobj.c) | Set the flag to make a lift refuse calls; clear on item. Good for sectioning a level. |
| 8 | **Scripted events** (spawns, cutscenes, scripted door opens) | B | override the stage `0x10xx`/`0x14xx` ailist; suppress `open_door`/`try_spawn_chr_at_pad`/`enable_object`/`set_stage_flag` | The deep per-level layer; needs per-stage authoring but no engine change. |
| 9 | **Level exit / mission end** | B | suppress `end_level` (`0x00DC`) / the all-objectives-complete path in the stage ailist | PD has **no physical exit prop** to lock — completion is state-driven, so the only way to gate the *exit itself* is via the script (B). Usually unnecessary: gating stage *access* (#1) already controls order; gate the exit only for "collect N before you may leave" seeds. |
| 10 | **Objectives** | B | refuse the `set_stage_flag` / objective-criteria path in the override | Lets AP require an item before an objective can be completed (e.g. "no Data Uplink ⇒ uplink objective can't finish"). Pairs naturally with the gadget gate (#4). |

### 9.3 What should be *progression* vs. *useful* vs. *filler*

AP logic needs each item classified so the seed stays solvable:

- **Progression** (gates real advancement): stage/difficulty unlocks (#1/#5),
  campaign-critical weapons & gadgets that an objective requires (#2/#4/#10),
  keycards (#6). These must be in logic.
- **Useful** (helps but not required): most weapons, scanners, Combat Boost,
  extra ammo capacity.
- **Filler / traps**: ammo top-ups, the self-sabotage cheats (§4). Safe to place
  anywhere.

The `apworld`'s logic rules then read like: *"Skedar Ruins/Perfect Agent
requires `Stage:SkedarRuins` + `Difficulty:Perfect` + (objective-gating items
for that stage)."*

### 9.4 New Lua surface for gating

Strategy A needs a tiny set of grants/queries beyond §6.4 (the same
`g_ApUnlocks` set):

```lua
pd.ap_mode(on)                 -- enable AP gating (flips gates to consult the unlock set)
pd.unlock(category, id)        -- AP item arrived: add to the unlock set
pd.lock(category, id)          -- (rarely) revoke
pd.is_unlocked(category, id)   -- query (also used by Lua override gates, strategy B)
-- categories: "stage" | "difficulty" | "weapon" | "device" | "key" | "feature"
```

Strategy B needs nothing new — it's `pd.register_ailist` + `pd.is_unlocked` +
the already-shipped `ctx`/`ai.*` helpers. Each engine gate point (#1-7) gets a
one-line `if (apMode && !apIsUnlocked(cat,id)) return locked;` guard reading the
same set, so Lua and C agree on a single source of truth.

> **Integrity:** the gates are **server/solo only** (`g_NetMode != NETMODE_CLIENT`)
> for the same reason overrides are — a net client must not make its own
> access decisions. In co-op the host's unlock set governs everyone.

---

## 10. File-change summary (for the eventual PR)

| File | Change | Half |
|---|---|---|
| `src/game/luaai_api.c` | `"tick"` dispatch; `luaEmit{MissionComplete,CheatUnlock,ChallengeComplete,FiringRange,WeaponFound,Objective}`; register `pd.*` read/grant/lock fns | both |
| `src/include/game/luaai.h` | declare the new emitters + bridge accessors | both |
| `src/game/endscreen.c` | emit `missioncomplete` + `cheatunlock` at the existing best-time/unlock sites | checks |
| `src/game/objectives.c` | edge-triggered `objective` emit in `objectivesCheckAll` | checks |
| `src/game/challenge.c` | emit `challengecomplete` in `challengeConsiderMarkingComplete` | checks |
| `src/game/training.c` | emit `firingrange` + `weaponfound` | checks |
| `src/game/cheats.c` / `gamefile.c` | grant helpers + AP-granted marker bit + `g_ApUnlocks` set | both |
| `src/game/mainmenu.c` | AP-aware branch in `isStageDifficultyUnlocked` (stage/difficulty gate #1/#5) | gating |
| `src/game/propobj.c` | AP gate in `objTestForPickup` (weapon #2) + optional door `keyflags`/lift gates (#6/#7) | gating |
| `src/game/bondgun.c` | optional AP branch in `bgunPrimaryFunctionDisabled` (weapon-fire #2) | gating |
| `src/game/player.c` | AP filter in the `playerLoadDefaults` intro-weapon loop (loadout #3) | gating |
| `src/game/game_0b0fd0.c` | AP gate in `currentPlayerSetDeviceActive` (gadgets #4) | gating |
| `src/game/luaai_ap.c` | **new** — `ap.*` socket/WS bridge | transport |
| `scripts/init.lua` | load `scripts/ap/client.lua` | both |
| `scripts/ap/*.lua` | **new** — client state machine, id tables, JSON, per-level override gates | both |
| `docs/archipelago_blueprint.md` | this document | — |

All engine edits are additive, port-only (`#ifndef PLATFORM_N64`), and the gates
are inert unless an AP run is active (`pd.ap_mode`), so non-AP play is byte-for-byte
unchanged.

---

*Authored against `port-net-predict`. Cross-references:
[`luascripting.md`](luascripting.md), [`luascripting_roadmap.md`](luascripting_roadmap.md),
`port/src/net/CLAUDE.md`, `src/include/CLAUDE.md`.*
