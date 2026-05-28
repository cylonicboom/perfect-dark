# Port-only Feature: GoldenEye Style

Two activation routes that trigger the same behavioural rule set:

- **`MPOPTION_GOLDENEYE`** (`0x80000000`, `src/include/constants.h`) — Combat Sim toggle in the host's lobby. Applies to the active match only. Wire-synced via `SVC_STAGE_START` so server and clients evaluate every gate identically (`port/src/net/netmsg.c:517` write, `:603` read).
- **`CHEAT_GOLDENEYE`** (45, `src/include/constants.h`) — gameplay cheat (always-unlocked, port-only). Works in any mode — solo, training, MP — so the GE rule set isn't tied to Combat Sim alone.

Every gate site routes through a single helper `goldeneyeStyleActive()` (`src/game/cheats.c`, declared in `src/game/cheats.h`):

```c
bool goldeneyeStyleActive(void)
{
    if (cheatIsActive(CHEAT_GOLDENEYE)) return true;
    if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_GOLDENEYE)) return true;
    return false;
}
```

So adding new GE behaviour means: call `goldeneyeStyleActive()` at the gate. Both routes pick it up automatically.

Everything is gated under `#ifndef PLATFORM_N64` so the N64 build is byte-identical.

---

## Activation surface

| File | Site | Purpose |
|---|---|---|
| `src/include/constants.h` | `MPOPTION_GOLDENEYE 0x80000000` | Bit definition (upper byte is now fully allocated) |
| `src/game/mplayer/scenarios/combat.inc` | inside the existing `#ifndef PLATFORM_N64` block | "GoldenEye Style" checkbox in the Combat menu |
| `port/src/net/netmenu.c` | `s_opts[]` table | Lobby active-options summary string |

---

## Behavioural changes

Each lives in the most natural decompiled file, gated with the canonical pattern:

```c
#ifndef PLATFORM_N64
g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_GOLDENEYE)
#endif
```

### 1. Snap lean (`src/game/bondwalk.c`)

The lean sway code at `bwalk0f0c69b8` interpolates `swayoffset0/swayoffset2` toward target over many frames using a per-frame `spa8` cap. GE mode sets `spa8 = dist * 0.5f` so each frame applies ~50% of the remaining delta — lean reaches near-target in ~3 frames instead of 8–10 (without a true 1-frame pop that breaks the `spcc.f[0] += spb4` movement-delta accumulator at the same site).

### 2. No crouch accuracy bonus (`src/game/bondgun.c`)

`bgunCalculatePlayerShotSpread` skips the `if (CROUCHPOS_SQUAT) spread *= 0.5f` multiplier. `bgunCalculateBotShotSpread` is intentionally **not** gated (sims keep their bonus).

### 3. Lower-and-raise reloads (`src/game/bondgun.c`)

In the `HANDSTATEMINOR_RELOAD_MAIN` block, the gate on the `reload_animation` non-NULL branch fails in GE mode, so every weapon (except combat knife, exclusion preserved) falls through to `HANDSTATEMINOR_RELOAD_LOWER` — the existing arm-down/arm-up sequence used by PP9i / CC13 (their `reload_animation` is `NULL`, `bgunSetArmPitch` does the actual visual lower).

### 4. Invisible-wall ledges (`src/game/bondwalk.c`)

When the player walks off a real cliff (drop > 60 units below the start-of-tick floor, computed as `(bondprevpos.y - vv_height) - vv_ground`), the start-of-tick pose is restored AND the player is nudged a few units back in the direction opposite to their attempted move. The push-back creates breathing room so a held-forward input doesn't keep retriggering the snap every frame and feel like being stuck pinned at the lip. Push magnitude is 4 units, clipped along the attempted-move axis; `bondprevpos.y - vv_height` is used for `vv_manground` (feet position; using `bondprevpos.y` directly would put the engine's feet 159 units above the floor and the player would visibly float).

(Earlier description of this gate moved here for context.)

In `bwalkUpdateVertical`'s falling branch, when `isfalling` is about to transition false → true AND the drop magnitude `(bondprevpos.y - vv_height) - vv_ground > 60.0f`, the player's start-of-tick pose (from `bondprevpos`, cached at the top of every `bwalkTick`) is restored and `isfalling` is left false. Slopes and stairs (per-frame drop below 60 units) fall through to vanilla behavior. `vv_manground` must be set to `bondprevpos.y - vv_height` (feet, not head) — `vv_height = 159` per `types.h`.

### 5. Classic crosshair on every weapon (`src/game/sight.c`)

At the top of `sightDraw`, when GE mode is on (or the per-player Force Classic Crosshair option) and the sight isn't `SIGHT_NONE` (melee), the sight is forced to `SIGHT_CLASSIC`. `SIGHT_ZOOM` weapons (MagSec 4, AR34, sniper, etc. — see `game_0b0fd0.c:642`) are included; their `sightDrawZoom` wrapper otherwise falls through to `sightDrawDefault` for the under-bracket reticle, so they'd show the default crosshair without the override. Trade-off: zoom-corner brackets and the sniper fullscreen scope are gone in GE mode; the FOV-change on zoom still works. Color comes from `PLAYER_EXTCFG().crosshaircolour`.

### 6. Hide crosshair unless aiming (`src/game/sight.c`)

`sightDraw` early-returns when `!sighton` (player isn't holding aim) AND GE mode is active OR the per-player Hide Crosshair Unless Aiming option is on.

### 7. GoldenEye HUD: half-arc bracket health/shield bars (`src/game/player.c`)

`playerRenderHealthBar` gates to `playerRenderHealthBarGE` when GE mode is on. The GE version draws two 8-segment half-circle "brackets" — a `)` shape on the left and a `(` shape on the right, with the BULGE of each arc resting on the side edge and the ENDPOINTS facing into the screen. Vertically centered on the viewport so the arcs sit *on* the sides, not concentrated in the corners.

- **Health** (left, yellow→red top-to-bottom): warm yellow `0xffd820` at the top endpoint fades through orange to saturated red `0xd01818` at the bottom endpoint.
- **Shield** (right, light-cyan→dark-blue top-to-bottom): light cyan `0x80d0ff` at the top fades to dark blue `0x2050d0` at the bottom.

Segment positions/sizes are hand-tuned in `arc_layout[8]` to approximate a half circle of radius ~90px using axis-aligned rectangles, with dimensions varying along the arc (wide/short at endpoints where the tangent is horizontal, tall/narrow at the bulge where the tangent is vertical) to suggest the rotation of GE's segmented arc HUD. The whole arc is inset ~24px from each side edge so blank space remains between the arc and the screen border. Format per row is `{ x_from_edge, y_off_from_center, w, h }`. Index 0 = bottom endpoint (sits on the side edge, red end, persists longest); index 7 = top endpoint (also on the edge, yellow end, depletes first); index 3/4 = middle segments at the bulge peak (deepest into the screen). The right side mirrors x around `viewright`. Per-segment color comes from `playerLerpRGBA(bot, top, t)` where `t = i / 7`. Empty slots draw at low alpha so max-HP capacity stays visible.

The default PD shield-bar is suppressed entirely. Caller's 2D HUD render state from `func0f0d49c8` is still valid, so no matrix dance is needed. Layout uses fixed pixel sizes — at higher render resolutions the arcs look proportionally smaller; bumping every value in `arc_layout` proportionally is the simplest tune.

### 8. Disable secondary weapon functions (`src/game/bondgun.c`, `src/game/botinv.c`)

Two activation routes go to secondary, both gated:

- **Standard function-change button** (cycles `weaponfunc` via `HANDSTATE_CHANGEFUNC`): `bgunSetState` refuses the transition when current function is `FUNC_PRIMARY` AND `bgunSecondaryFunctionDisabled(weaponnum)` returns true. Switching secondary → primary stays allowed.
- **Dedicated alt-fire button / active-menu function-toggle** (sets `invertgunfunc` / `activatesecondary` for weapons like RCP120, AR34, Laptop Gun, Dragon, Sniper Rifle, Remote Mine): `bgunConsiderToggleGunFunction` (the single choke point for both the `UCMD_SECONDARY` input path and the active-menu `togglefunc` path) returns `USETIMER_STOP` early when `goldeneyeStyleActive() && !bgunIsUsingSecondaryFunction()` — refusing to enter secondary while allowing the toggle-back if the player was already in secondary when GE activated.
- **Bots — weapon-function secondary** (`botinv.c`): `botinvSwitchToWeapon(chr, weaponnum, funcnum)` clamps `funcnum` to `FUNC_PRIMARY` when `goldeneyeStyleActive()` returns true. Bot AI's weapon-scoring code in `botinv.c` (`botinvScoreWeaponByItself` etc.) still considers secondaries when scoring, but the final commit is forced to primary. Covers every AI path that would otherwise select a secondary.
- **Bots — cloak device + RCP120 cloak** (`bot.c`): the AI block that flips `aibot->cloakdeviceenabled` / `aibot->rcp120cloakenabled` based on ammo / target state runs as normal, then a GE post-gate forces both to false. Bots can't go invisible in GE mode. (Cloak is the only bot "alt fire" that lives outside the `funcnum` system — every other bot alt-mode goes through `botinvSwitchToWeapon`.)

### 9. No mid crouch (`src/game/bondwalk.c`, `src/game/bondmove.c`)

Crouch input lives in two files. All four input paths need the gate; `bwalkAdjustCrouchPos` alone is not enough because the keyboard / toggle / hold paths in `bondmove.c` write `crouchpos` directly without going through it.

- **`bondwalk.c` `bwalkAdjustCrouchPos`** — landing on `CROUCHPOS_DUCK` is collapsed to STAND or SQUAT using the input direction. Covers the C-button / analog path.
- **`bondmove.c` `BUTTON_CROUCH_CYCLE` (toggle mode)** — after the decrement that would land on DUCK, jump to SQUAT.
- **`bondmove.c` `BUTTON_HALF_CROUCH` (toggle mode)** — repurposed as a STAND ↔ SQUAT toggle so the Shift key still acts as a crouch.
- **`bondmove.c` `BUTTON_HALF_CROUCH` (hold mode)** — assigns SQUAT instead of DUCK.
- **`bondmove.c` `bmoveProcessRemoteInput` (UCMD_DUCK)** — remote players' DUCK input is collapsed to SQUAT for visual/state consistency.

### 10. Disable dual-wield (`src/game/bondgun.c`)

Three gate sites all use the reusable `bgunDualWieldDisabled()` helper. Three layers because PD has multiple paths that set `dualwielding = true` (cycle forward/back, active menu, REMOTEMINE auto-bump, spawn-with-weapon) and the engine's own resync at the bottom of `bgunTickSwitch2` only fires when `bgunCanFreeWeapon(HAND_LEFT)` allows it — i.e., not mid-attack — so a single gate at the equip path lets the left hand keep firing for a few frames.

- **Top of `bgunTickSwitch2`** — every tick, force `ctrl->dualwielding = false` AND `lefthand->inuse = false`. Kills the left fire path immediately regardless of attack state.
- **Mid `bgunTickSwitch2` (unified equip if-branch)** — after the `REMOTEMINE` bump that sets `dualwielding = true`, force it false again. The existing `if (!ctrl->dualwielding) lefthand->inuse = false` line right below drops the left hand.
- **`bgunEquipWeapon2(HAND_LEFT, …)`** — early-return; refuses left-hand equips and clears both flags up-front, before they get a chance to propagate.

**Cycle navigation fix in `bgunCycleForward` / `bgunCycleBack`** — companion patch, not a gate. The top-of-tick gate forces `lefthand.inuse = false`, so `bgunGetSwitchToWeapon(HAND_LEFT)` returns `WEAPON_NONE`. Without intervention, `invChooseCycleForwardWeapon` then matches the player's own Double inventory item (`weapon1 == current && weapon2 > 0`) and returns it as "the next weapon" — the player gets stuck. The fix: if the player has a DUAL of the current weapon in inventory, pretend `weapon2 == weapon1` for the cycle's lookup so it walks past the DUAL slot instead of into it.

### 12. No blur / dizzy effects (`src/game/chraction.c`, `src/game/chr.c`, `src/game/bot.c`)

GoldenEye 007 didn't have screen-blur or dizzy stagger effects, so GE mode disables both sources in PD:

- **Weapon-induced dizzy** (`chraction.c:~4448`): right after `makedizzy = race != RACE_DRCAROLL && gsetHasFunctionFlags(gset, FUNCFLAG_MAKEDIZZY)`, a GE clamp forces `makedizzy = false`. This stops the downstream player-dizzy block (~4860) from adding to `blurdrugamount` and also stops the chr-dizzy paths further in `chrDamage` — one gate site covers both directions.
- **Poison-induced blur** (`chr.c:~2321`): the per-tick poison code accumulates `blurdrugamount += g_Vars.lvupdate240 * 10`. Gated so it skips the accumulation in GE mode (poison damage itself still applies; just no visual blur).
- **Bot residual wipe** (`bot.c`): the bot's GE post-gate (same site that clamps cloak) also force-zeroes `blurdrugamount` and `blurnumtimesdied` per tick, so flipping the GE cheat mid-game instantly wipes lingering dizziness on bots.

Player residual blur fades naturally (the existing decay in `bot.c:947-957` only runs for bots; the player's blur decays via the same code path's player-equivalent driven by damage absence). If you flip the cheat mid-game with the player currently dizzy, expect a few seconds for the existing blur to fade out — new blur won't accumulate.

### 11. I-frames + damage flash + fire lockout (`src/game/chraction.c`, `src/game/player.c`, `src/game/bondgun.c`)

When GoldenEye Style is on, any chr — player OR sim — that takes damage gets `TICKS(18)` (~300ms at 60Hz) of invulnerability. A repeated hit inside that window is rejected by an early-return at the top of `chrDamage`. Local players also get a one-shot 8-frame full-screen white flash for visual feedback, AND cannot fire weapons while the i-frame window is active.

- **New port-only fields** (in `types.h`):
  - `chrdata.lastdamagetick60` — lvframe60 stamp of the most recent damage. Zero is the "never damaged" sentinel so the first hit always lands; the gate bumps to 1 if `lvframe60` itself is 0. Reset to 0 in `chrInit` (`chr.c:1119`), `botReset` respawn block (`bot.c`), and `playerStartNewLife` (`player.c`) so a stale stamp from a previous chr life (or a recycled chrslot) can't carry over and grant the freshly-spawned chr permanent invulnerability. Gate uses u32 arithmetic so any stamp ahead of current `lvframe60` wraps to a huge unsigned value that correctly fails the `< TICKS(12)` check.
  - `player.damageflashstart60` — lvframe60 stamp of the moment this local player last took damage. Initialised to `-1000000` in `playermgrCreatePlayer` and also reset there in `playerStartNewLife` so no spurious flash on respawn.
- **I-frame gate** at the top of `chrDamage` (`chraction.c`): returns early if GE mode is on AND `lvframe60 - lastdamagetick60 < TICKS(12)`. The gate ONLY reads the stamp; it does not write it.
- **Stamp at the damage-application sites** (not at function entry — that would spuriously fire the window on probe / zero-damage calls like gun/hat hits or hits on already-dead chrs):
  - Player branch (`chraction.c:~4870`, immediately after `bondhealth -= amount / healthscale`): stamps `chr->lastdamagetick60 = lvframe60` unconditionally, but only stamps `currentplayer->damageflashstart60 = lvframe60` when BOTH the previous flash has fully ended (≥ 8 frames ago) AND the chr is not still in its prior i-frame window. This stops the flash from re-stacking / mid-fade-restarting if chained damage calls land close together. `currentplayer` has been switched to the damaged player by the earlier `setCurrentPlayerNum`, so the flash lands on the right viewport.
  - Sim/chr branch (`chraction.c:~5057`, immediately after `chr->damage += damage`): stamps `chr->lastdamagetick60 = lvframe60`.
  - Both sites bump the stamp to 1 if `lvframe60 == 0` so the "never damaged" sentinel can't be re-armed by chance.
- **Flash render** appended to `playerRenderHealthBarGE` (`player.c`): full-screen white `gDPHudRectangle` with a triangular alpha curve over 8 frames (`8 → 22 → 36 → 50 → 50 → 36 → 22 → 8`, peak ~50/255 ≈ 20% for a subtle hit indicator), then suppressed. Sims/remote players never render it because the HUD function only runs for the local viewport.
- **Fire lockout** via reusable helper `bgunCurrentPlayerInIframe()` (`bondgun.c`). Three gate sites cover human-player + AI firing:
  - **Player input gate** — `bgunSetState` refuses new `HANDSTATE_ATTACK` / `HANDSTATE_ATTACKEMPTY` transitions while the helper returns true, so IDLE → ATTACK is blocked.
  - **Player tick gate** — `bgunTickInc` top force-cancels an in-flight attack (snaps the hand back to `HANDSTATE_IDLE`) when the helper returns true, so a held-down auto-fire weapon stops mid-burst instead of continuing through the i-frame window.
  - **Bot / NPC tick gate** — `chrTickShoot` (`chraction.c`) early-returns when `goldeneyeStyleActive() && chr->lastdamagetick60 != 0 && (u32)(lvframe60 - lastdamagetick60) < (u32)TICKS(18)`. Same u32-wrap-safe check as the chrDamage gate. Bot AI's per-shot tick becomes a no-op while the bot is in its own i-frame, so a freshly-hit sim is briefly silenced before returning fire.

### 12. Hide weapon function indicator (`src/game/bondgun.c`)

The HUD normally renders two things to indicate the current weapon function:
- A small **red/yellow square** next to the ammo counter (`bondgun.c:13179`) — red when primary, animates to yellow when secondary, driven by `ctrl->fnfader`.
- A **function-name text overlay** ("Single Shot", "Burst Fire", etc.) below the weapon name (`bondgun.c:13245-13316`).

In GE mode both are gated off. The square is wrapped in a `#ifndef PLATFORM_N64` GE-mode check around the `textSetPrimColour` / `gDPFillRectangleScaled` / `text0f153838` triple; the text block uses the same check on the outer `if (func)`. Matches GE's minimal HUD aesthetic where each weapon has only one function anyway. The weapon-name overlay above it (driven by `guntypetimer`) is left visible.

---

## Reusable helpers (for the upcoming weapon-loadout work)

Both live in `src/game/bondgun.c`, both port-only. The pattern is intentionally identical so future per-weapon loadout options can OR additional conditions in without re-threading the check through every call site.

```c
bool bgunSecondaryFunctionDisabled(s32 weaponnum);  // src/game/bondgun.c
bool bgunDualWieldDisabled(void);                   // src/game/bondgun.c
```

Both currently return true only when `g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_GOLDENEYE)`. When weapon-loadout options that ban per-weapon secondaries / dual-wield arrive, add the `g_LoadoutDisabledSecondary[weaponnum]` (or whatever the loadout API exposes) check inside these helpers — every existing call site automatically picks up the new condition.

The `weaponnum` parameter on `bgunSecondaryFunctionDisabled` is currently ignored but is in the signature so loadout work doesn't need to break the call signature when it lands.

---

## Per-player crosshair options (related, not GE-gated)

These live in `struct extplayerconfig` (`src/include/types.h`), default false in `PLAYER_EXT_CFG_DEFAULT` (`src/game/mplayer/mplayer.c`), config-registered in `port/src/main.c`, and exposed in Options → Extended Game (`port/src/optionsmenu.c`):

| Field | INI key | Menu label |
|---|---|---|
| `crosshairforceclassic` | `Game.PlayerN.CrosshairForceClassic` | "Force Classic Crosshair" |
| `crosshairhideunlessaiming` | `Game.PlayerN.CrosshairHideUnlessAiming` | "Hide Crosshair Unless Aiming" |

Both are also implied by GE mode (the same gates in `sight.c` OR them with the GE check), so enabling GE mode doesn't require touching these.

---

## Gotchas

- **The GoldenEye HUD assumes 2D HUD render state is already set up by the caller** (`menu.c:5533` runs `func0f0d49c8` before `playerRenderHealthBar`). If you rewire callers, set that up or the bars won't render.
- **`bondprevpos` is start-of-tick, not end-of-tick.** `bwalkUpdatePrevPos` runs at the top of every `bwalkTick`, before the horizontal move. The ledge wall uses this to snap back to "before this tick's move."
- **`vv_height = 159` for Jo, constant regardless of crouch state** (per `types.h:2709` comment). Don't try to read it for crouch-aware feet placement; use `vv_manground` directly when you have it.
- **All 4 upper-byte MPOPTION bits are now allocated** (`MPOPTION_NOCULL`, `MPOPTION_NOOMLIMIT`, `MPOPTION_HOSTSPECTATOR`, `MPOPTION_GOLDENEYE`). Future port-only MP options need a new strategy — either repurpose unused lower-byte bits the original game ignores, or widen the field. See `src/include/CLAUDE.md`.
- **N64 build untouched.** All edits live under `#ifndef PLATFORM_N64`, preserving the decompilation contract.
