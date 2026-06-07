# Port-only Feature: GoldenEye Style + Classic Options

> **Classic Options update (2026-06).** The GE rule set was broken into 12 individually
> selectable "Classic Options". Each behaviour now has its own cheat (`CHEAT_CLASSIC_*`
> 49-60) and its own per-match MP option (`MPOPTION_CLASSIC_*`, high-word bits 34-45,
> `ULL` constants), with **GoldenEye Style kept as the master** — a behaviour is active when
> the master OR its own toggle is set, on either route. The per-behaviour choke point is:
>
> ```c
> bool classicOptionActive(s32 cheat_id, u64 mpoption)
> {
>     if (cheatIsActive(CHEAT_GOLDENEYE) || cheatIsActive(cheat_id)) return true;
>     if (g_Vars.normmplayerisrunning && (g_MpSetup.options & (MPOPTION_GOLDENEYE | mpoption))) return true;
>     return false;
> }
> ```
>
> Every gate site listed in this doc now calls `classicOptionActive(CHEAT_CLASSIC_X,
> MPOPTION_CLASSIC_X)` instead of `goldeneyeStyleActive()` (which remains, master-only,
> for diagnostics). The behaviour → toggle mapping:
>
> | Classic option | Cheat (idx) | MP bit | Behaviours (doc sections below) |
> |---|---|---|---|
> | Snap Lean | `CHEAT_CLASSIC_SNAPLEAN` 49 | 34 | §1 snap lean |
> | No Crouch Accuracy | `CHEAT_CLASSIC_NOCROUCHACC` 50 | 35 | §2 no crouch accuracy bonus |
> | Classic Reloads | `CHEAT_CLASSIC_RELOAD` 51 | 36 | §3 lower-and-raise reloads |
> | Ledge Walls | `CHEAT_CLASSIC_LEDGEWALL` 52 | 37 | §4 invisible-wall ledges |
> | Classic Crosshair | `CHEAT_CLASSIC_SIGHT` 53 | 38 | §5 classic crosshair |
> | Hide Crosshair Unless Aiming | `CHEAT_CLASSIC_HIDESIGHT` 54 | 39 | §6 hide-unless-aiming |
> | GoldenEye HUD | `CHEAT_CLASSIC_GEHUD` 55 | 40 | §7 arc HUD **+ the damage flash** (the flash renders inside `playerRenderHealthBarGE`, so it's a GEHUD cosmetic — its `damageflashstart60` stamp in `chrDamage` is GEHUD-gated) |
> | No Secondary Functions | `CHEAT_CLASSIC_NOSECONDARY` 56 | 41 | §8 secondary-function bans (`bgunSecondaryFunctionDisabled`), bot cloak disable, fn-indicator HUD hides |
> | No Mid-Crouch | `CHEAT_CLASSIC_NOMIDCROUCH` 57 | 42 | §9 all five crouch-input sites |
> | No Dual Wield | `CHEAT_CLASSIC_NODUALWIELD` 58 | 43 | §10 (`bgunDualWieldDisabled`) |
> | Damage Invulnerability | `CHEAT_CLASSIC_IFRAMES` 59 | 44 | §11 i-frames + fire lockout (`bgunCurrentPlayerInIframe`, `lastdamagetick60` stamps) — **no flash unless GEHUD is also on** |
> | No Blur Effects | `CHEAT_CLASSIC_NOBLUR` 60 | 45 | §12 dizzy/poison blur/bot blur wipe |
>
> Menus: cheats in **Extended Options > Experiments > Classic Options** (master on top);
> MP options on the **"Classic Options" carousel page** (`g_MpClassicOptionsMenuDialog`,
> `src/game/mplayer/setup.c`), third sibling after "More Options"
> (`g_ExtGameOptionsMenuDialog.nextsibling`), shared by every scenario. High-word rows use
> `menuhandlerMpCheckboxPortOption` (`param3 = BIT >> 32`). Playlist names: `CLASSIC_*`
> (`port/src/net/playlist.c`). `NET_PROTOCOL_VER` bumped to 67 (wire format unchanged —
> options already ride as u64 — but gate semantics differ across builds).

Two activation routes that trigger the same behavioural rule set:

- **`MPOPTION_GOLDENEYE`** (`0x80000000`, `src/include/constants.h`) — Combat Sim toggle in the host's lobby. Applies to the active match only. Wire-synced via `SVC_STAGE_START` so server and clients evaluate every gate identically (`port/src/net/netmsg.c:517` write, `:603` read).
- **`CHEAT_GOLDENEYE`** (45, `src/include/constants.h`) — gameplay cheat (always-unlocked, port-only). Works in any mode — solo, training, MP — so the GE rule set isn't tied to Combat Sim alone. Toggled from **Extended Options > Experiments > Classic Options** (it was moved out of the Cheats > Gameplay menu).

Every gate site routes through the per-behaviour helper above; the master-only helper remains for diagnostics:

```c
bool goldeneyeStyleActive(void)
{
    if (cheatIsActive(CHEAT_GOLDENEYE)) return true;
    if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_GOLDENEYE)) return true;
    return false;
}
```

So adding new GE/Classic behaviour means: pick (or add) a `CHEAT_CLASSIC_*`/`MPOPTION_CLASSIC_*` pair and call `classicOptionActive(...)` at the gate. Both routes — and the master — pick it up automatically.

Everything is gated under `#ifndef PLATFORM_N64` so the N64 build is byte-identical.

---

## Activation surface

| File | Site | Purpose |
|---|---|---|
| `src/include/constants.h` | `MPOPTION_GOLDENEYE 0x80000000` + `MPOPTION_CLASSIC_*` bits 34-45; `CHEAT_GOLDENEYE 45` + `CHEAT_CLASSIC_*` 49-60 | Bit/index definitions |
| `src/game/mplayer/setup.c` | `g_MpClassicOptionsMenuDialog` | "Classic Options" carousel page (master + 12 toggles), third sibling after "More Options" |
| `port/src/optionsmenu.c` | `g_ExtendedClassicMenuDialog` | Extended Options > Experiments > Classic Options (cheat checkboxes) |
| `port/src/net/netmenu.c` | `s_opts[]` table (u64 flags) | Lobby active-options summary string |
| `port/src/net/playlist.c` | `s_options[]` | Playlist `options=` names (`GOLDENEYE`, `CLASSIC_*`) |

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

Both route through `goldeneyeStyleActive()` for the GE arm. `bgunSecondaryFunctionDisabled` additionally consults a static `mpSlotFlagsForWeapon(weaponnum)` helper that walks `g_MpSetup.weapons[]` against `g_MpSlotFnFlags[]` for the per-slot `FNFLAG_SECONDARY_DISABLED` bit — this is the Custom Weapon Presets hook (see [`PORT_WEAPON_PRESETS.md`](PORT_WEAPON_PRESETS.md)). A separate `bgunPrimaryFunctionDisabled(weaponnum)` exists for the mirror axis; it currently only consults the FNFLAG bits (GE mode doesn't disable primary functions).

The `weaponnum` parameter on `bgunSecondaryFunctionDisabled` *is* used now (by `mpSlotFlagsForWeapon`). If you're porting GE Style alone and skipping the Custom Weapon Presets system, the parameter can remain unused in your reduced helper — but keep it in the signature so the call sites don't have to change when presets land later.

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
- **The low-word upper byte is fully allocated** (`MPOPTION_NOCULL`, `MPOPTION_NOOMLIMIT`, `MPOPTION_HOSTSPECTATOR`, `MPOPTION_GOLDENEYE` at `0x10000000`–`0x80000000`). `g_MpSetup.options` has since been **widened to `u64`**, so new port-only options take a high-word bit (32-63, written `0x...ULL`) — `MPOPTION_NODOORS 0x0000000100000000ULL` is the worked example. See `src/include/CLAUDE.md` and `PORTING_HOWTO.md` §3.
- **N64 build untouched.** All edits live under `#ifndef PLATFORM_N64`, preserving the decompilation contract.

---

## Porting checklist

This is a large feature (12 behavioural rules across ~14 files), so porting in order matters — the foundation needs to be in place before any individual rule's gates can call `goldeneyeStyleActive()`. See [`PORTING_HOWTO.md`](PORTING_HOWTO.md) for the cross-cutting methodology (guard patterns, MPOPTION budget, helper choke points) this checklist sits on top of.

### Foundation (must come first — every gate site below depends on these)

1. **Constants** (`src/include/constants.h`, port-only block at the bottom):
   - `CHEAT_GOLDENEYE 45` — append after the existing port-only cheats. Use the same numeric value as this branch; cheat indices are array indices into `g_Cheats[]` and the literal-name table.
   - `MPOPTION_GOLDENEYE 0x80000000` — use the same bit value as this branch (save/wire compat depends on bit positions).

2. **Reusable cheat infrastructure** (`src/game/cheats.c`) — skip if your branch already has it from [`PORT_NO_CULLING.md`](PORT_NO_CULLING.md):
   - `s_cheat_literal_names[]` table + `cheatGetName` helper + `CHEATFLAG_ALWAYSUNLOCKED` short-circuit in `cheatIsUnlocked` + marquee early-return. See PORT_NO_CULLING for the exact shape.
   - Add `[CHEAT_GOLDENEYE] = "GoldenEye Style"` to the literal-name table.

3. **The single helper** — `bool goldeneyeStyleActive(void)` in `src/game/cheats.c`, prototype in `src/include/game/cheats.h` (both under `#ifndef PLATFORM_N64`):
   ```c
   bool goldeneyeStyleActive(void)
   {
       if (cheatIsActive(CHEAT_GOLDENEYE)) return true;
       if (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_GOLDENEYE)) return true;
       return false;
   }
   ```
   Every GE gate site routes through this — do **not** inline the `(MPOPTION | CHEAT)` check.

4. **`g_Cheats[]` entry** for `CHEAT_GOLDENEYE` with `flags = CHEATFLAG_ALWAYSUNLOCKED`. Append to `g_CheatsGameplayMenuItems[]` so it shows in the cheat menu.

5. **MP option menu entry** in `src/game/mplayer/scenarios/combat.inc` inside the existing port-only block of `g_MpCombatOptionsMenuItems[]`:
   - `"GoldenEye Style"` → `MPOPTION_GOLDENEYE`, `menuhandlerMpCheckboxOption`.
   Mirror to other scenario `.inc` files (KoH does this on this branch).

6. **Reusable gate helpers** in `src/game/bondgun.c` + `src/include/game/bondgun.h` (prototypes), all port-only:
   - `bool bgunSecondaryFunctionDisabled(s32 weaponnum)` — returns true on GE active OR `FNFLAG_SECONDARY_DISABLED` for the weapon's slot.
   - `bool bgunPrimaryFunctionDisabled(s32 weaponnum)` — currently FNFLAG-only (no GE term).
   - `bool bgunDualWieldDisabled(void)` — returns true on GE active.
   - `bool bgunCurrentPlayerInIframe(void)` — reads the GE i-frame stamp on the current player's chr.
   - The static `mpSlotFlagsForWeapon(weaponnum)` helper for the FNFLAG axis (only needed if you're also porting Custom Weapon Presets — see [`PORT_WEAPON_PRESETS.md`](PORT_WEAPON_PRESETS.md)).

### Per-rule gates (any order — each depends only on the foundation above)

Each step below corresponds to a numbered section earlier in this doc. Open that section for the exact file:line and code snippet to add. The order between these steps doesn't matter — they're independent gates.

7.  **§1 Snap lean** — `src/game/bondwalk.c` `bwalk0f0c69b8`, raise the per-frame interpolation cap when `goldeneyeStyleActive()`.
8.  **§2 No crouch accuracy bonus** — `src/game/bondgun.c` `bgunCalculatePlayerShotSpread`, skip the `CROUCHPOS_SQUAT` spread multiplier. Note: `bgunCalculateBotShotSpread` is intentionally NOT gated (sims keep their bonus).
9.  **§3 Lower-and-raise reloads** — `src/game/bondgun.c` `HANDSTATEMINOR_RELOAD_MAIN` block, fail the `reload_animation` non-NULL gate so every weapon (except combat knife — exclusion preserved) falls through to `HANDSTATEMINOR_RELOAD_LOWER`.
10. **§4 Invisible-wall ledges** — `src/game/bondwalk.c` `bwalkUpdateVertical`, restore start-of-tick pose when drop > 60 units and the falling transition is about to fire. Also nudge back along the attempted-move axis.
11. **§5 Classic crosshair on every weapon** — `src/game/sight.c` `sightDraw` top, force `SIGHT_CLASSIC` when GE active (or per-player Force Classic Crosshair, see related section) AND sight != `SIGHT_NONE`.
12. **§6 Hide crosshair unless aiming** — `src/game/sight.c` `sightDraw`, early-return when `!sighton` AND GE active (or per-player Hide Crosshair Unless Aiming).
13. **§7 GoldenEye HUD** — `src/game/player.c` `playerRenderHealthBar` dispatches to `playerRenderHealthBarGE` when GE active. New function `playerRenderHealthBarGE` draws the 8-segment half-arc bracket pair (left = health, right = shield) plus the GE damage flash. Vanilla shield bar suppressed inside the GE branch.
14. **§8 Disable secondary functions** — multi-site:
    - `bondgun.c` `bgunSetState` HANDSTATE_CHANGEFUNC block: refuse primary→secondary when `bgunSecondaryFunctionDisabled(weaponnum)`.
    - `bondgun.c` `bgunConsiderToggleGunFunction`: return `USETIMER_STOP` early when `goldeneyeStyleActive() && !bgunIsUsingSecondaryFunction()`.
    - `botinv.c` `botinvSwitchToWeapon`: clamp `funcnum` to `FUNC_PRIMARY` when GE active. (This is the single AI choke point.)
    - `bot.c` post-tick block: force `aibot->cloakdeviceenabled = false` and `aibot->rcp120cloakenabled = false` when GE active.
15. **§9 No mid-crouch** — all four crouch input paths:
    - `bondwalk.c` `bwalkAdjustCrouchPos`: collapse landings on `CROUCHPOS_DUCK` to STAND/SQUAT.
    - `bondmove.c` BUTTON_CROUCH_CYCLE (toggle mode), BUTTON_HALF_CROUCH (toggle and hold modes).
    - `bondmove.c` `bmoveProcessRemoteInput` UCMD_DUCK: collapse to SQUAT for remote-player visual parity.
16. **§10 Disable dual-wield** — `bondgun.c` three sites:
    - Top of `bgunTickSwitch2`: force `ctrl->dualwielding = false` and `lefthand->inuse = false` per-tick.
    - Mid `bgunTickSwitch2` REMOTEMINE auto-bump branch: re-force `dualwielding = false`.
    - `bgunEquipWeapon2(HAND_LEFT, …)`: early-return refusing left-hand equips.
    - Companion patch in `bgunCycleForward` / `bgunCycleBack`: pretend `weapon2 == weapon1` for the cycle's lookup when the player has a DUAL inventory item but the left hand is force-disabled, so the cycle walks past the DUAL slot.
17. **§11 I-frames + damage flash + fire lockout** — the largest single piece, multi-file:
    - **New fields** in `src/include/types.h` under `#ifndef PLATFORM_N64`: `chrdata.lastdamagetick60` and `player.damageflashstart60`. Both at the tail of their respective structs.
    - **Reset sites**: `chrInit` (`chr.c`), `botReset` respawn block (`bot.c`), `playerStartNewLife` (`player.c`) all zero `lastdamagetick60`. `playermgrAllocatePlayer` (`playermgr.c`) initialises `damageflashstart60 = -1000000` sentinel.
    - **I-frame gate** at top of `chrDamage` (`chraction.c`): u32-wrap-safe `(u32)(lvframe60 - lastdamagetick60) < (u32)TICKS(12)` check (note the gate uses 12, the player flash window uses 8, the bot fire lockout uses 18 — different values intentionally).
    - **Stamp at damage-application sites** in `chrDamage` (not at function entry — that would fire on zero-damage probe calls). Player branch + sim/chr branch both stamp `chr->lastdamagetick60 = lvframe60`. Player branch also stamps `currentplayer->damageflashstart60 = lvframe60` with anti-stack guards (only when the previous flash has ended AND the chr isn't still in its prior i-frame window).
    - **Flash render** appended to `playerRenderHealthBarGE` (`player.c`): full-screen white `gDPHudRectangle` with triangular alpha curve over 8 frames `8 → 22 → 36 → 50 → 50 → 36 → 22 → 8`.
    - **Fire lockout helper** `bgunCurrentPlayerInIframe()` in `bondgun.c` + three gate sites: `bgunSetState` (refuse new ATTACK/ATTACKEMPTY), `bgunTickInc` (snap an in-flight attack back to IDLE), and `chrTickShoot` in `chraction.c` (bot fire lockout, `TICKS(18)` window — wider than player gate because bots otherwise blast a freshly-damaged sim before it can react).
18. **§12 No blur / dizzy** — `chraction.c` `makedizzy` clamp around line 4448 ("makedizzy = false" after the existing assignment); `chr.c` poison-blur accumulation skip around 2321; `bot.c` residual wipe (`blurdrugamount`, `blurnumtimesdied` force-zero per tick when GE active).
19. **§12 Hide weapon function indicator** — `bondgun.c` two sites: the red/yellow square (`textSetPrimColour` / `gDPFillRectangleScaled` / `text0f153838` triple around line 13179), and the function-name text overlay (around 13245-13316). Both wrapped in a GE-mode check.

### Optional / related

20. **Per-player crosshair options** — independent of GE but referenced by the §5/§6 gates. See the dedicated section earlier in this doc. Files: `extplayerconfig` fields in `types.h`, `PLAYER_EXT_CFG_DEFAULT` in `mplayer.c`, `configRegisterInt` calls in `port/src/main.c`, menu entries in `port/src/optionsmenu.c`, the gate ORs in `sight.c`.

21. **(Netplay branches only)** No `NET_PROTOCOL_VER` bump needed for GE Style alone — `MPOPTION_GOLDENEYE` rides in the existing `g_MpSetup.options` (now `u64`) that `SVC_STAGE_START` already serializes. The "GoldenEye Style" entry in `port/src/net/netmenu.c`'s `s_opts[]` lobby summary table is nice-to-have, not load-bearing.

**Skip-conditions**: If your branch has no cheats menu, skip step 2 + 4 and gate everything on the MP option only (the `goldeneyeStyleActive()` helper just stops checking `cheatIsActive`). If your branch has no netplay, skip step 21 entirely. If you only want a subset of the 12 behavioural rules, the foundation steps (1-6) are still required — individual rules can be cherry-picked from steps 7-19 freely; they're independent.

---

## Porting gotchas

- **`goldeneyeStyleActive()` IS the contract — never bypass it.** Inlining the `(MPOPTION_GOLDENEYE | CHEAT_GOLDENEYE)` check at a new gate site means future additions to the helper (a per-weapon loadout option, a settings UI override, anything) won't see your gate. Route every site through the helper. The 30 seconds of indirection is worth it.
- **`bgunSecondaryFunctionDisabled` takes a `weaponnum` and uses it.** The Custom Weapon Presets system consumes the parameter via `mpSlotFlagsForWeapon(weaponnum)`. If you port GE Style alone (no presets), the helper's body simplifies but **keep the parameter in the signature** — when presets land later, every existing call site already passes the right argument and nothing has to change.
- **`bgunDualWieldDisabled` is parameterless on purpose.** Currently GE-only; future per-weapon dual-wield bans would either widen this signature OR use a separate per-weapon helper that ORs with this one. Don't pre-emptively widen — let the future use case dictate the shape.
- **The HUD assumes 2D HUD render state is already set up by the caller** (`menu.c:5533` runs `func0f0d49c8` before `playerRenderHealthBar`). If you rewire callers or move the GE HUD into a fresh render site, set up 2D state first or the arcs render with garbage transforms.
- **`bondprevpos` is start-of-tick, not end-of-tick.** `bwalkUpdatePrevPos` runs at the top of every `bwalkTick`, before the horizontal move. The ledge wall uses this to snap back to "before this tick's move." If you reorder `bwalkUpdatePrevPos`, the ledge wall silently breaks (no compile error — just wrong gameplay).
- **`vv_height = 159` for Jo, constant regardless of crouch state** (per `types.h:2709` comment). The ledge-wall code computes `vv_manground = bondprevpos.y - vv_height` (feet position). Using `bondprevpos.y` directly puts the engine's feet 159 units above the floor and the player visibly floats. Don't try to make this crouch-aware — `vv_height` doesn't change during crouch.
- **I-frame stamp arithmetic MUST use u32.** The check `(u32)(lvframe60 - lastdamagetick60) < (u32)TICKS(12)` wraps safely when `lastdamagetick60` is stale from a previous chr life. Using signed arithmetic produces a huge negative number that incorrectly satisfies `< TICKS(12)` and grants permanent invulnerability. The same u32 cast appears in `chrTickShoot`'s bot fire lockout (`TICKS(18)` there).
- **Reset `lastdamagetick60` in every chr-life-start site.** Currently `chrInit`, `botReset` respawn block, `playerStartNewLife`. If you add a new chr-spawn path (a new bot type, a respawn-with-special-state code path), reset there too — a recycled chrslot with a stale stamp grants the new chr permanent i-frame.
- **The "zero is sentinel" convention requires bumping to 1 if `lvframe60 == 0`.** The damage-application sites set `chr->lastdamagetick60 = lvframe60`, but bump to 1 if `lvframe60` happened to be 0 (so the "never damaged" sentinel can't be re-armed by chance at level start).
- **The flash stamp is conditional, the chr stamp is unconditional.** `chr->lastdamagetick60 = lvframe60` always fires at every damage-application site. `currentplayer->damageflashstart60 = lvframe60` only fires when the *previous* flash has fully ended (≥ 8 frames ago) AND the chr is not still in its prior i-frame window. Chained damage calls otherwise re-stack the flash and produce a strobe effect.
- **`currentplayer` has to be the damaged player when the flash stamp fires.** The earlier `setCurrentPlayerNum(damagedplayernum)` call in `chrDamage`'s player branch must complete before the flash stamp runs. If you move the stamp earlier, the flash lands on the wrong viewport in split-screen.
- **GE classic-crosshair forcing covers `SIGHT_ZOOM` weapons too.** Zoom-corner brackets and the sniper fullscreen scope are gone in GE mode. The FOV-change on zoom still works. If your branch needs zoom UI retained, exclude `SIGHT_ZOOM` from the `sightDraw` top force — but then the per-player "Force Classic" option becomes inconsistent (it forces classic on those weapons; GE mode wouldn't).
- **Cycle-forward/back DUAL fix is a companion patch, not a GE gate.** When `lefthand.inuse` is force-disabled by the dual-wield gate, `bgunGetSwitchToWeapon(HAND_LEFT)` returns `WEAPON_NONE`. Without the cycle-lookup fix, `invChooseCycleForwardWeapon` matches the player's own DUAL inventory item and the cycle gets stuck. The fix lives in `bgunCycleForward` / `bgunCycleBack`: if the player has a DUAL of the current weapon, pretend `weapon2 == weapon1` for the lookup so the cycle walks past the DUAL slot. If you cherry-pick only the no-dual-wield rule, you need this companion patch too.
- **`actiontype` is intentionally NOT touched.** The bot AI's per-action tick (`chrTickStand`, `chrTickAttack`, etc.) is decompiled code; GE Style doesn't gate on actiontype. If you find yourself wanting to gate AI behaviour on GE mode, do it in a port-only post-tick block (the pattern in `bot.c`'s cloak gate is the model).
- **`MPOPTION_GOLDENEYE` rides in the existing `g_MpSetup.options` (a `u64`) — DO NOT add a new wire byte.** No `NET_PROTOCOL_VER` bump is needed solely for GE Style. If you also add a wire byte, you bump compat for no benefit and break wire-level interop with this branch.
- **The low-word upper byte is fully allocated**, but `g_MpSetup.options` is now a `u64` — new port-only MP options take a high-word bit (32-63), e.g. `MPOPTION_NODOORS 0x0000000100000000ULL`. See [`PORTING_HOWTO.md`](PORTING_HOWTO.md) §3. If your target branch still has a 32-bit `options`, widen it the same way or pick a free low-word bit and document it.
- **N64 build must remain byte-identical.** Every edit lives under `#ifndef PLATFORM_N64`. New struct fields (`lastdamagetick60`, `damageflashstart60`, the per-player crosshair fields) go at the **tail** of their structs under the guard. Reordering or inserting fields mid-struct breaks the decompilation contract.
