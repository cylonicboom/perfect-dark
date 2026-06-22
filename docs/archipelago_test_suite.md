# Archipelago — Complete Test Suite (step by step)

A full, ordered manual test pass for the Archipelago integration that has **landed
so far**: the per-frame `tick` event, the `missioncomplete` / `firingrange` /
`weaponfound` check-detection emitters, the bonus/buff `pd.*` bonus API, the
Perfect Buddy, and the no-server Lua harness (`scripts/ap/test.lua`).

For the overall design and what is *not yet* built (the `ap.*` socket/WS bridge,
the gating layer, and the `cheatunlock` / `challengecomplete` / `objective`
events) see [`archipelago_blueprint.md`](archipelago_blueprint.md). Those are
explicitly **out of scope for this suite** — see §9.

> Requires a ROM and a build of `port-net-predict`. Target **single-player /
> solo** — every bonus and the buddy are server/solo authoritative and are no-ops
> on a net client or with no live player.

Each step has a **Do**, an **Expect**, and a **PASS/FAIL** box. Work top to
bottom; later steps assume earlier ones passed. Keep the console (`~`) open — every
result is a console log line.

---

## 0. Prerequisites & build

| # | Do | Expect | ✅ |
|---|---|---|---|
| 0.1 | Build the branch from the MSYS2 MinGW-x64 shell (`cmake --build build_debug_sdl3 -j`). | Build completes, `pd.x86_64.exe` produced. | ☐ |
| 0.2 | Confirm `scripts/` is next to the working directory you launch from (the runtime loads `scripts/init.lua` from the **CWD**). | `scripts/init.lua` and `scripts/ap/test.lua` present. | ☐ |
| 0.3 | Launch the game with a valid ROM. | Title screen, no crash. | ☐ |

If you launch from a folder **without** `scripts/`, the harness silently won't
load — that's the #1 cause of "nothing happens." Verify 0.2.

---

## 1. Harness load

| # | Do | Expect | ✅ |
|---|---|---|---|
| 1.1 | Press **`~`** to open the console. | Console opens. | ☐ |
| 1.2 | Read the boot log (scroll up with **PageUp** if needed). | A line: `AP test harness loaded: /lua ap.list()  \|  pause menu -> Lua Director` | ☐ |
| 1.3 | Run `/lua reload`. | Scripts re-run; the "AP test harness loaded" line prints again, no errors. | ☐ |
| 1.4 | Run `/lua print(type(ap))`. | `table` | ☐ |

**FAIL → stop here.** If the harness didn't load, nothing else works. Check:
launched from the right CWD (0.2); `scripts/init.lua` actually `require`s/loads
`ap/test.lua`; no Lua parse error printed on boot.

---

## 2. Check board — console tools

The board is seeded with **63 mission checks** (21 stages × 3 difficulties) plus
any firing-range / weapon checks that fire during play.

| # | Do | Expect | ✅ |
|---|---|---|---|
| 2.1 | `/lua ap.list()` | Prints 63 lines `[ ] mission:<Stage>/<Diff>` then `AP: 0 / 63 checks complete`. All start `[ ]`. | ☐ |
| 2.2 | `/lua ap.status()` | `AP: 0 / 63 checks complete` | ☐ |
| 2.3 | `/lua ap.complete("mission:Defection/Agent")` | `AP CHECK: mission:Defection/Agent  (forced)` | ☐ |
| 2.4 | `/lua ap.list()` | `mission:Defection/Agent` now `[x]`; tally `AP: 1 / 63`. | ☐ |
| 2.5 | `/lua ap.complete("Villa")` (substring match) | Completes the first pending `…Villa…` check, logs `(forced (matched))`. | ☐ |
| 2.6 | `/lua ap.complete("Defection/Agent")` again | `AP CHECK (already done): mission:Defection/Agent` (idempotent, no double count). | ☐ |
| 2.7 | `/lua ap.next()` | Completes the next pending check; logs `(forced (next))` and returns its name. | ☐ |
| 2.8 | `/lua ap.complete("ZZZ-nope")` | `AP: no pending check matching 'ZZZ-nope'` (no crash). | ☐ |
| 2.9 | `/lua ap.reset()` | `AP: all checks reset to pending`. | ☐ |
| 2.10 | `/lua ap.list()` | Back to `AP: 0 / 63` (firing-range/weapon checks that appeared also reset to `[ ]`, not removed). | ☐ |

---

## 3. Real engine check detection (the C emitters)

This is the important part — it proves the engine emits the events from real
gameplay, not just the Lua simulation.

### 3.1 `missioncomplete` — legitimate finish

| # | Do | Expect | ✅ |
|---|---|---|---|
| 3.1.1 | Start any solo mission with **no cheats active**. Reach the exit / complete objectives legitimately. | Mission ends → endscreen. | ☐ |
| 3.1.2 | Open console, read the log. | `AP CHECK: mission:<Stage>/<Diff>  (<secs>s)` — stage and difficulty match what you played. | ☐ |
| 3.1.3 | `/lua ap.list()` | That `mission:<Stage>/<Diff>` cell is `[x]`. | ☐ |

### 3.2 `missioncomplete` — anti-cheese guard (negative test)

| # | Do | Expect | ✅ |
|---|---|---|---|
| 3.2.1 | Start a mission, enable a cheat (e.g. invincibility via the cheats menu, or `/lua ap.invincible()`), finish it. | Mission ends. | ☐ |
| 3.2.2 | Read the log. | Either **no** `AP CHECK: mission:` line, **or** one tagged `CHEATED` — and the cell does **not** count as a legit clear. The engine's "no best time while cheats active" guard must suppress the genuine check. | ☐ |

> The harness shows `CHEATED` in the source tag if the engine passes
> `cheated != 0`; the real AP client will simply ignore cheated completions.

### 3.3 `firingrange` — medals

| # | Do | Expect | ✅ |
|---|---|---|---|
| 3.3.1 | Go to Carrington Institute → Firing Range. Earn a **new** Bronze/Silver/Gold on any weapon (must beat your previous best for that weapon). | Medal awarded. | ☐ |
| 3.3.2 | Console log. | `AP CHECK: firingrange:wN/<Bronze\|Silver\|Gold>  (firing range)` | ☐ |
| 3.3.3 | Re-run the same range **without** beating your score. | **No** new `firingrange` line (only fires on a new best). | ☐ |
| 3.3.4 | `/lua ap.list()` | A `firingrange:wN/<medal>` row now appears, `[x]`. | ☐ |

### 3.4 `weaponfound` — first pickup

| # | Do | Expect | ✅ |
|---|---|---|---|
| 3.4.1 | In a mission, pick up a weapon you have **never found before** in this save. | Weapon picked up. | ☐ |
| 3.4.2 | Console log. | `AP CHECK: weaponfound:wN  (first found)` | ☐ |
| 3.4.3 | Drop and re-pick the **same** weapon. | **No** new `weaponfound` line (first-found only). | ☐ |

### 3.5 `objective` — per-objective completion

| # | Do | Expect | ✅ |
|---|---|---|---|
| 3.5.1 | In a mission, complete a **single objective** (not the whole mission). | Objective-complete HUD message. | ☐ |
| 3.5.2 | Console log. | `AP CHECK: objective:<Stage>/<Diff>/<n>  (objective complete)` — keyed by stage + difficulty + objective index. | ☐ |
| 3.5.3 | `/lua ap.list()` | An `objective:…` row appears `[x]`. (Objectives are added to the board as they fire — not pre-seeded.) | ☐ |
| 3.5.4 | Finish the whole mission. | One `objective:` line per objective that transitioned to complete, plus the `mission:` line. | ☐ |

### 3.6 `cheatunlock` — timed/skill cheat unlocks

| # | Do | Expect | ✅ |
|---|---|---|---|
| 3.6.1 | Complete a stage on a difficulty/time that **newly unlocks a cheat** (the endscreen shows the cheat-unlocked banner), **no cheats active**. | Cheat-unlock banner. | ☐ |
| 3.6.2 | Console log. | `AP CHECK: cheat:<id>  (cheat unlocked)`. Only fires the first time that cheat's condition is met. | ☐ |

### 3.7 `challengecomplete` — Combat-Sim challenges

| # | Do | Expect | ✅ |
|---|---|---|---|
| 3.7.1 | Complete a **Combat Simulator challenge** (the Carrington challenges), no cheats active. | Challenge marked complete. | ☐ |
| 3.7.2 | Console log. | `AP CHECK: challenge:<index>/<N>p  (challenge complete)` — N = the player count it was beaten with. | ☐ |

---

## 4. Bonus items

Run each in a **live solo mission** (not the title/CI hub menus). Use the console
form; then repeat via the **pause menu → Lua Director** button to confirm both
paths. Watch the on-screen HUD for the effect, and the console for the log line.

| # | Console | Menu button | Expect (HUD) | Expect (log) | ✅ |
|---|---|---|---|---|---|
| 4.1 | `/lua ap.heal()` | AP Bonus: Full HP | health bar → full | `AP bonus: full HP` | ☐ |
| 4.2 | `/lua ap.shield()` | AP Bonus: Full Shield | shield bar → full | `AP bonus: full shield` | ☐ |
| 4.3 | `/lua ap.ammo()` | AP Bonus: Refill Ammo | all ammo topped up | `AP bonus: ammo refilled` | ☐ |
| 4.4 | `/lua ap.grenade()` | AP Bonus: Grenade | grenade count +1 | `AP bonus: grenade` | ☐ |
| 4.5 | `/lua ap.cloak()` | AP Bonus: Cloak | player turns invisible (~30s) | `AP bonus: cloak engaged` | ☐ |
| 4.6 | `/lua ap.invincible()` | — | take damage → no health loss | `AP bonus: invincible (call ap.mortal() to clear)` | ☐ |
| 4.7 | `/lua ap.mortal()` | — | damage hurts again | `AP: invincibility off` | ☐ |
| 4.8 | `/lua ap.weapon(0x1c)` | — | weapon `0x1c` added to inventory | `AP bonus: weapon 28` | ☐ |

Notes:
- **4.5 cloak** calls `give_ammo(0x14, 60*30)` (cloak ammo, ~30s) then
  `device_on(0x31)` (cloaking device). If only the ammo lands but no cloak,
  `device_on` failed — note it.
- **4.6/4.7** toggle `pd.invincible(true/false)` — re-run 3.2 afterwards to make
  sure you cleared it before testing a legit mission check.
- If any line prints `AP: pd.<fn> not available in this build`, that bridge is
  missing from the build you're running — record which one (it means a stale exe).

---

## 5. Perfect Buddy

| # | Do | Expect | ✅ |
|---|---|---|---|
| 5.1 | In a live solo mission, `/lua ap.buddy()` (or menu **AP Bonus: Spawn Perfect Buddy**). | A friendly ally (Dark-Combat / Velvet) spawns next to you holding a Falcon 2. | ☐ |
| 5.2 | Console log. | `AP bonus: Perfect Buddy spawned (chr N)` | ☐ |
| 5.3 | Lead the buddy to enemies. | It **shoots enemies**, not you; ignores your friendly fire. | ☐ |
| 5.4 | Shoot the buddy. | It does not retaliate / die to your fire (NOFRIENDLYFIRE / TEAM_ALLY). | ☐ |
| 5.5 | Spawn several buddies. | Each spawns and fights; no crash. | ☐ |

---

## 6. Per-frame `tick` event (plumbing)

The AP socket loop will eventually run on `tick`. Confirm the event fires every
frame, even on menus.

| # | Do | Expect | ✅ |
|---|---|---|---|
| 6.1 | In the console: `/lua local n=0; pd.on("tick", function() n=n+1 end); ap._tickprobe=function() return n end` | No error. | ☐ |
| 6.2 | Wait ~2 seconds on the **title screen** (no mission), then `/lua print(ap._tickprobe())`. | A number well above 0 and rising each call (~60/s) — proves `tick` fires off-mission too. | ☐ |

---

## 7. Edge cases / negative tests

| # | Do | Expect | ✅ |
|---|---|---|---|
| 7.1 | On the **title screen** (no live player): `/lua ap.buddy()` | `AP: buddy spawn failed (no live player? net client?)` — no crash. | ☐ |
| 7.2 | On the title screen: `/lua ap.heal()` | No effect / safe no-op (no live player). No crash. | ☐ |
| 7.3 | As a **net client** (join someone's game): `/lua ap.buddy()` / `ap.heal()` | No-ops on the client (server/solo authoritative). No crash, no desync. | ☐ |
| 7.4 | `/lua ap.complete()` (no arg) | Usage hint: `usage: ap.complete("mission:Defection/Agent")`. | ☐ |
| 7.5 | `/lua reload` mid-mission | Harness reloads, board state **resets** (it's session-only), no crash. | ☐ |

---

## 8. Full command & expected-log reference

Console (all prefixed `/lua`):

```
ap.list()                  -> board + "AP: d / t checks complete"
ap.status()                -> "AP: d / t checks complete"
ap.complete("name")        -> "AP CHECK: name  (forced[ (matched)])"
ap.next()                  -> completes next pending, returns name
ap.reset()                 -> "AP: all checks reset to pending"
ap.heal()                  -> "AP bonus: full HP"
ap.shield()                -> "AP bonus: full shield"
ap.ammo()                  -> "AP bonus: ammo refilled"
ap.grenade()               -> "AP bonus: grenade"
ap.cloak()                 -> "AP bonus: cloak engaged"
ap.invincible()/ap.mortal()-> "AP bonus: invincible …" / "AP: invincibility off"
ap.weapon(n)               -> "AP bonus: weapon <n>"
ap.buddy()                 -> "AP bonus: Perfect Buddy spawned (chr N)"
```

Engine-emitted (from real gameplay):

```
AP CHECK: mission:<Stage>/<Diff>  (<secs>s[ CHEATED])
AP CHECK: firingrange:w<N>/<Bronze|Silver|Gold>  (firing range)
AP CHECK: weaponfound:w<N>  (first found)
```

Landed `pd.*` bridges this suite exercises: `player_heal`, `player_set_shield`,
`refill_ammo`, `give_ammo`, `give_weapon`, `device_on`, `invincible`,
`spawn_ally`, `menu_add`, `on`, `log`. Landed events: `tick`, `missioncomplete`,
`firingrange`, `weaponfound`.

---

## 9. Explicitly NOT covered (not yet implemented)

Do **not** test-fail on these — they are unbuilt per the blueprint:

- **`ap.*` socket / WebSocket bridge** (`ap.connect/poll/send/status`) — no real
  AP server link yet.
- **Gating** (`pd.ap_mode`, `pd.unlock/lock/is_unlocked`, stage/weapon/device
  locks) — nothing is locked.
- **Grant accessors** (`pd.grant_*`), DeathLink, traps, goal predicate.

The `objective`, `cheatunlock`, and `challengecomplete` events are now wired
(§3.5–3.7). Remaining detection gaps are only the pure-Lua milestone ideas
(kill/room counters) and the gamefile read accessors.

---

## 10. Result summary

| Section | Pass? | Notes |
|---|---|---|
| 0 Build & launch | ☐ | |
| 1 Harness load | ☐ | |
| 2 Check board tools | ☐ | |
| 3 Real check detection | ☐ | |
| 4 Bonus items | ☐ | |
| 5 Perfect Buddy | ☐ | |
| 6 `tick` event | ☐ | |
| 7 Edge cases | ☐ | |

> **Verification status (pre-this-pass):** engine C verified by CI compile only;
> Lua parse-checked against vendored Lua 5.4. This suite **is** the first runtime
> proof — record any FAIL with the exact console line and the step number.
