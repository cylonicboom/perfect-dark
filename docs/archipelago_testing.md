# Archipelago — Testing Guide

How to exercise the Archipelago integration that's landed so far (the
check-detection events, the bonus/buff items, the Perfect Buddy, and the no-server
Lua test harness). See [`archipelago_blueprint.md`](archipelago_blueprint.md) for
the overall design and what's still planned.

> Needs a ROM and a build of this branch. Target single-player/solo — the
> bonuses and the buddy are server/solo authoritative.

## 1. Build & launch

Build as usual (MSYS2 MinGW x64) and launch from a directory that contains the
repo's `scripts/` folder — the runtime loads `scripts/init.lua` from the **working
directory**, which now also loads `scripts/ap/test.lua`.

## 2. Confirm the harness loaded

Press **`~`** to open the console. On boot you should see:

```
[lua] AP test harness loaded: /lua ap.list()  |  pause menu -> Lua Director
```

`/lua reload` re-runs the scripts without restarting the game.

## 3. Check board — manual tools (console)

| Command | Effect |
|---|---|
| `/lua ap.list()` | print the full board (21 stages × 3 difficulties + any firing-range/weapon checks seen) with `[x]`/`[ ]` and a done/total tally |
| `/lua ap.complete("Defection/Agent")` | force-complete a check (substring match works, e.g. `ap.complete("Villa")`) |
| `/lua ap.next()` | complete the next pending check |
| `/lua ap.status()` | done/total count |
| `/lua ap.reset()` | set all checks back to pending |

Each completion logs `AP CHECK: mission:<stage>/<diff> (...)` to the console.

## 4. Real check detection (the engine events)

This proves the C emitters fire from real gameplay:

- **Mission complete** — finish any mission legitimately (no active cheats) →
  `AP CHECK: mission:<stage>/<diff> (<secs>s)`, and the cell flips to `[x]` in
  `ap.list()`. (With a cheat active it will **not** fire — the anti-cheese guard.)
- **Firing range** — earn a new Bronze/Silver/Gold in the CI firing range →
  `AP CHECK: firingrange:wN/<medal>`.
- **Weapon first-found** — pick up a weapon you haven't found before →
  `AP CHECK: weaponfound:wN`.

## 5. Bonus items

Via console, or the **pause menu → Lua Director** buttons (open the pause menu in
a mission):

| Console | Menu button | Expect |
|---|---|---|
| `/lua ap.heal()` | AP Bonus: Full HP | health bar full |
| `/lua ap.shield()` | AP Bonus: Full Shield | shield bar full |
| `/lua ap.ammo()` | AP Bonus: Refill Ammo | all ammo topped up |
| `/lua ap.grenade()` | AP Bonus: Grenade | a grenade added |
| `/lua ap.cloak()` | AP Bonus: Cloak | invisible (~30s) |
| `/lua ap.invincible()` / `/lua ap.mortal()` | — | god mode on / off |
| `/lua ap.weapon(0x1c)` | — | weapon added to inventory |

## 6. Perfect Buddy

`/lua ap.buddy()` or the **"AP Bonus: Spawn Perfect Buddy"** menu button spawns a
friendly ally (Dark-Combat / Velvet) next to you with a Falcon 2, on `TEAM_ALLY`.
Verify it shoots **enemies** (not you) and ignores friendly fire. Console logs
`Perfect Buddy spawned (chr N)`.

## 7. Edge cases worth a glance

- Bonuses and the buddy are **no-ops on a net client** and on the title screen
  (no live player) — `ap.buddy()` logs `buddy spawn failed` there. Test them in a
  real solo mission.
- The mission-complete check fires only on a **legit** finish (no active cheats).

## Verification status

The engine C is verified by **CI compile** (client non-regression + dedicated
headless, Linux + Windows) and the Lua scripts are **parse-checked** against the
vendored Lua 5.4. The in-game behaviour above is what to confirm on a real ROM —
it has not been runtime-proven yet.
