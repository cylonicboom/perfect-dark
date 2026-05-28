# Port-only Rendering Feature: No Room Culling / No Draw Slot Limit

Two port-only options that disable the N64's portal-based room culling and the 60-room draw-slot cap. Useful for debugging visibility bugs and capturing wide-vista screenshots. Exposed both as a Combat Sim MP option (per-match) and as a cheat (per-session). Implemented entirely under `#ifndef PLATFORM_N64` — the N64 build is unchanged.

> Separate from the netplay-prediction work documented in [`PORT_NET_PREDICT_CHANGES.md`](PORT_NET_PREDICT_CHANGES.md). The two features ship on `port-net-predict` but are independent.

---

## `src/include/constants.h`

Port-only additions; do not renumber.

- `MPOPTION_NOCULL 0x10000000` — disables portal culling in the active match
- `MPOPTION_NOOMLIMIT 0x20000000` — disables the 60-room draw-slot cap (only meaningful with `MPOPTION_NOCULL`, since the portal graph is what normally keeps room counts low)
- `CHEAT_NOCULL 43` — cheat-menu equivalent of `MPOPTION_NOCULL`
- `CHEAT_NODRAWLIMIT 44` — cheat-menu equivalent of `MPOPTION_NOOMLIMIT`
- `CHEATFLAG_ALWAYSUNLOCKED 16` — new flag bit so dev cheats are available without mission-completion gating

Bits `0x10000000` / `0x20000000` live in the upper word of the MP-options bitmask; the original game only uses the lower 24 bits, so the upper byte is free for port extensions (see [`../src/include/CLAUDE.md`](../src/include/CLAUDE.md)).

---

## `src/game/bg.c`

The draw-slot table is the heart of the change. `g_BgDrawSlots` is grown from 61 to 256 entries on port builds, with the sentinel relocated:

```c
#ifdef PLATFORM_N64
struct drawslot g_BgDrawSlots[61];
#else
// 256 slots: indices 0-254 are usable draw slots; index 255 is the special
// full-screen bbox sentinel (g_BgSpecialDrawSlot). The extra capacity is only
// used when g_BgNoDrawSlotLimit is set by CHEAT_NODRAWLIMIT or MPOPTION_NOOMLIMIT.
struct drawslot g_BgDrawSlots[256];
#endif
```

```c
#ifdef PLATFORM_N64
struct drawslot *g_BgSpecialDrawSlot = &g_BgDrawSlots[60];
#else
struct drawslot *g_BgSpecialDrawSlot = &g_BgDrawSlots[255];
#endif
```

Other touches in the same file:

- All hardcoded `g_BgDrawSlots[60]` references are replaced with `g_BgSpecialDrawSlot->` so the sentinel relocation is transparent.
- `bool g_BgNoCull` and `bool g_BgNoDrawSlotLimit` are port-only globals. Both are recomputed each frame inside `bgTickPortals` from **MP option OR cheat-active state** (`cheatIsActive(CHEAT_NOCULL)` ORed with `g_Vars.lvmpoptions & MPOPTION_NOCULL`, same pattern for the draw-limit pair).
- `bgSetRoomOnscreen` raises its room-index cap from 59 to 254 when `g_BgNoDrawSlotLimit` is set, so the expanded slot table can actually be filled.
- The local `roomorder[]` / `roomnums[]` arrays inside `bgRenderScene` are widened to `[255]` on port builds to match the bigger slot table.
- `#include "game/cheats.h"` is added (port-only) so `bgTickPortals` can call `cheatIsActive`.

---

## `src/game/cheats.c`

The cheat infrastructure needed several pieces to support dev cheats that have no lang-string entry:

- `CHEAT_NOCULL` and `CHEAT_NODRAWLIMIT` appended to `g_Cheats[]` inside `#ifndef PLATFORM_N64`, both flagged `CHEATFLAG_ALWAYSUNLOCKED`.
- `cheatIsUnlocked` short-circuits on `CHEATFLAG_ALWAYSUNLOCKED` so the unlock-by-mission-completion gauntlet is bypassed.
- `s_cheat_literal_names[]` — port-only string table indexed by cheat ID, used for cheats that don't have an `L_*` lang entry. Currently `[CHEAT_NOCULL] = "No Room Culling"` and `[CHEAT_NODRAWLIMIT] = "No Draw Slot Limit"`.
- `cheatGetName(cheat_id)` — new helper that returns the literal string if present, otherwise falls back to `langGet(...)`. Used by both marquee and name display.
- `cheatGetNameIfUnlocked` is rewritten to delegate to `cheatGetName`.
- `cheatGetMarquee` early-returns for `CHEATFLAG_ALWAYSUNLOCKED` cheats so they don't appear in the unlock-announcement marquee.
- `cheatActivate(CHEAT_NOCULL)` calls `gamefileUnlockEverything()` — activating the cull-disabling cheat also unlocks every mission, cheat, and weapon. This is intentional: it's a dev-mode trigger for testing campaigns.
- `cheatActivate(CHEAT_NODRAWLIMIT)` sets `g_BgNoDrawSlotLimit = true`; `cheatDeactivate` clears it.
- Both cheats appended to `g_CheatsGameplayMenuItems` so they appear in the in-game cheat menu.

---

## `src/game/mplayer/scenarios/combat.inc`

Two new checkbox menu items added after "Controllers Only", wrapped in `#ifndef PLATFORM_N64`:

- `"No Room Culling"` → `MPOPTION_NOCULL`
- `"No Draw Slot Limit"` → `MPOPTION_NOOMLIMIT`

These are Combat-scenario-only because that's the only scenario the port-net netplay supports anyway; if other scenarios become playable they'd want the same option added to their `.inc`.

---

## `port/src/net/netmenu.c`

The lobby "active options" summary string includes the human-readable labels `No Room Culling` and `No Draw Limit` when their respective bits are set, so clients can see at a glance whether the host has enabled them.

---

## Gotchas

- **MP option and cheat both feed the same globals.** `g_BgNoCull` / `g_BgNoDrawSlotLimit` are recomputed each frame as the OR of the per-match MP option and the per-session cheat state. Either route turns the feature on; both must be off to turn it off. There is no separate "MP-only" path.
- **`MPOPTION_NOOMLIMIT` is meaningless without `MPOPTION_NOCULL`.** With portal culling on, the room count almost never exceeds 60, so raising the cap changes nothing. The combat.inc menu does not enforce this — the user can tick the boxes independently — but the rendered behavior follows the same dependency.
- **Cheat indices are array indices.** `CHEAT_NOCULL = 43` and `CHEAT_NODRAWLIMIT = 44` are appended after the last original cheat. Inserting anything mid-array would shift every subsequent index and break saves, menu wiring, and the literal-name table lookup. See [`../src/include/CLAUDE.md`](../src/include/CLAUDE.md).
- **`gamefileUnlockEverything()` is a side-effect of `CHEAT_NOCULL` activation, not `CHEAT_NODRAWLIMIT`.** If you want the unlock-all behavior without disabling culling, activate `CHEAT_NOCULL` once then deactivate it — the unlock persists in the gamefile.
- **N64 build untouched.** All changes live under `#ifndef PLATFORM_N64`, so the decompilation contract is preserved.

---

## Porting checklist

Recommended order for adding these features to another fork / branch. Each step depends on the previous one's symbols existing. See [`PORTING_HOWTO.md`](PORTING_HOWTO.md) for the cross-cutting methodology this checklist sits on top of.

1. **Constants** (`src/include/constants.h`, port-only block at the bottom):
   - `MPOPTION_NOCULL 0x10000000`
   - `MPOPTION_NOOMLIMIT 0x20000000`
   - `CHEAT_NOCULL 43`, `CHEAT_NODRAWLIMIT 44` (append after the last existing port-only cheat — don't insert mid-array)
   - `CHEATFLAG_ALWAYSUNLOCKED 16` (a flag bit, not an index — already free)

2. **Reusable cheat infrastructure** (`src/game/cheats.c`) — needed if your branch doesn't already have it from a prior port-only cheat:
   - `s_cheat_literal_names[]` — sparse table indexed by cheat ID; entries for cheats without an `L_*` lang string.
   - `char *cheatGetName(s32 cheat_id)` — returns the literal name if present, falls back to `langGet(...)`.
   - `CHEATFLAG_ALWAYSUNLOCKED` short-circuit in `cheatIsUnlocked` (return true unconditionally).
   - `CHEATFLAG_ALWAYSUNLOCKED` early-return in `cheatGetMarquee` (no unlock-announcement marquee for dev cheats).
   - Rewrite `cheatGetNameIfUnlocked` to delegate to `cheatGetName`.

3. **Cheat table entries** in `g_Cheats[]` at indices 43 and 44 — both with `flags = CHEATFLAG_ALWAYSUNLOCKED` and zeroed mission/category/nametextid fields. The literal name comes from `s_cheat_literal_names`.

4. **Activate / deactivate handlers** (`cheats.c` `cheatActivate` / `cheatDeactivate` switch blocks, port-only):
   - `CHEAT_NOCULL` activate → `gamefileUnlockEverything()` (side effect — see gotchas). No deactivate work; `g_BgNoCull` is recomputed every frame by `bgTickPortals`.
   - `CHEAT_NODRAWLIMIT` activate → `g_BgNoDrawSlotLimit = true`. Deactivate → `g_BgNoDrawSlotLimit = false`.

5. **Cheat menu entries** — append `CHEAT_NOCULL` and `CHEAT_NODRAWLIMIT` to `g_CheatsGameplayMenuItems[]` so they show up in the in-game Gameplay cheats menu. Use `MENUITEMTYPE_CHECKBOX` with `cheatGetNameIfUnlocked` as the label callback.

6. **Draw-slot table expansion** (`src/game/bg.c`):
   - Wrap the existing `struct drawslot g_BgDrawSlots[61]` in `#ifdef PLATFORM_N64` / `#else` and add the 256-element port variant.
   - Same dual-define for `g_BgSpecialDrawSlot`: `&g_BgDrawSlots[60]` on N64, `&g_BgDrawSlots[255]` on port.
   - Grep `bg.c` for every literal `g_BgDrawSlots[60]` reference and replace with `g_BgSpecialDrawSlot->` (the sentinel relocation is otherwise transparent).
   - Wrap `#include "game/cheats.h"` in `#ifndef PLATFORM_N64` — the N64 build of `bg.c` doesn't reference cheats.

7. **Runtime flag globals** (`bg.c`, top of file):
   ```c
   #ifndef PLATFORM_N64
   bool g_BgNoCull = false;
   bool g_BgNoDrawSlotLimit = false;
   #endif
   ```

8. **Per-frame recompute** in `bgTickPortals` (port-only block, near the top of the function):
   ```c
   g_BgNoCull         = (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_NOCULL))    || cheatIsActive(CHEAT_NOCULL);
   g_BgNoDrawSlotLimit = (g_Vars.normmplayerisrunning && (g_MpSetup.options & MPOPTION_NOOMLIMIT)) || cheatIsActive(CHEAT_NODRAWLIMIT);
   ```
   Note the operator precedence — wrap each MPOPTION test in its own parens. The first time I missed this, `cheatIsActive` was bitwise-OR'd into the options test instead of logically-OR'd with it, which compiled fine and silently never triggered.

9. **Room-index cap** in `bgSetRoomOnscreen` (`bg.c`):
   ```c
   #ifndef PLATFORM_N64
   if (g_BgNoDrawSlotLimit) {
       if (roomidx >= 254) return;
   } else
   #endif
   {
       if (roomidx >= 59) return;
   }
   ```
   (Adapt to whatever your branch's existing cap check looks like — the values to swap are 59 → 254 and 60 → 255.)

10. **Widen local arrays** in `bgRenderScene`: `roomorder[]` / `roomnums[]` to `[255]` on port. Use a `#ifdef PLATFORM_N64` switch around the array decl, keep the original `[60]` for N64.

11. **MP-option menu entries** (`src/game/mplayer/scenarios/combat.inc`, inside the existing `#ifndef PLATFORM_N64` block in `g_MpCombatOptionsMenuItems[]`):
    - `"No Room Culling"` → `MPOPTION_NOCULL`
    - `"No Draw Slot Limit"` → `MPOPTION_NOOMLIMIT`
    Both use `menuhandlerMpCheckboxOption`, `MENUITEMFLAG_LOCKABLEMINOR | MENUITEMFLAG_LITERAL_TEXT`.
    Mirror to other scenario `.inc` files (`kingofthehill.inc`, etc.) that should also expose the toggle.

12. **(Netplay branches only)** Lobby summary string — `port/src/net/netmenu.c` `s_opts[]` table — add `{ MPOPTION_NOCULL, "No Room Culling" }` and `{ MPOPTION_NOOMLIMIT, "No Draw Limit" }`. No `NET_PROTOCOL_VER` bump needed: the bits ride in the existing `g_MpSetup.options` u32 that `SVC_STAGE_START` already serializes.

**Skip-conditions**: If your branch has no cheats menu, skip steps 2–5 and gate everything on the MP option only. If it has no netplay, skip step 12. If you only want one of the two features, drop the other constant + table entry + handler — they're independent.

---

## Porting gotchas

- **MPOPTION bit positions are wire- and save-format identifiers.** If your target branch already uses `0x10000000` or `0x20000000` for unrelated port-only options, **pick fresh upper-byte bits** (and document them in the equivalent of `src/include/CLAUDE.md`). `mpsetupfileLoadWad` / `SaveWad` read/write `g_MpSetup.options` as 32 raw bits, so old setup files baked at the previous bit positions will silently mean the wrong thing on the new code path. Cross-branch save compatibility is rarely worth chasing — pick whatever bits are free in your target.
- **Cheat indices are append-only and saved.** `CHEAT_NOCULL = 43` and `CHEAT_NODRAWLIMIT = 44` are the next two indices after the last original cheat. If your target branch has already appended cheats in this range (e.g. from a different port-only feature), **use whatever the next two unused indices are** — but renumbering across branches is acceptable because the cheat-active bitmask is RAM-only, not saved. The literal-name table is indexed by cheat ID, so the entries must match whatever indices you picked.
- **`#include "game/cheats.h"` in `bg.c` must be `#ifndef PLATFORM_N64`-guarded.** The N64 build of `bg.c` doesn't include the cheats header, and `cheats.h` may transitively pull in port-only types. Skipping the guard breaks the N64 build.
- **The `g_BgDrawSlots` array growth costs memory unconditionally.** Even when neither cheat nor MP option is set, the port build carries a 256-entry table (vs. N64's 61). Cost is `256 * sizeof(struct drawslot)` ≈ ~10 KB depending on the struct. Acceptable on modern platforms; if you're tight on RAM (Switch homebrew, etc.), consider gating the size on a build-time `PORT_BIG_DRAWSLOTS` flag.
- **`g_BgNoCull` / `g_BgNoDrawSlotLimit` are recomputed every `bgTickPortals` call from BOTH routes.** Activating the cheat mid-MP-match doesn't fight the MP option — they OR together. Deactivating one when the other is on changes nothing. There is no priority / override; both routes feed the same global.
- **`gamefileUnlockEverything()` is a one-way trigger.** Once activated via `CHEAT_NOCULL` it persists in the gamefile (saved unlock state). Deactivating the cheat doesn't re-lock anything. If your branch wants `CHEAT_NOCULL` to be a pure rendering toggle, **drop the `gamefileUnlockEverything()` call** from `cheatActivate`'s NOCULL branch and document the change in your port's notes.
- **`bgSetRoomOnscreen`'s `roomidx >= 254` cap is one-less than the array size.** Index 255 is reserved for `g_BgSpecialDrawSlot`. If you grow the array further (>256), update both the cap and the sentinel location together — the sentinel is referenced by name (`g_BgSpecialDrawSlot->`) but the cap is a numeric literal.
- **The Combat Sim menu does not enforce `MPOPTION_NOOMLIMIT` implies `MPOPTION_NOCULL`.** Users can tick "No Draw Slot Limit" without "No Room Culling" and get a no-op (the portal graph caps room count well below 60 in normal play). Documenting this in the menu help text is optional but recommended; gating one checkbox on the other would require menu-handler logic the original Combat Options menu doesn't otherwise need.
- **`cheatIsActive` works across all game modes.** Unlike the MP option (`g_Vars.normmplayerisrunning` gated), the cheat takes effect in solo, training, and campaign too. That's the design intent — these are dev cheats — but be aware if you wire it into a code path that assumes MP-only state.
- **N64 build must remain byte-identical.** Every edit lives under `#ifndef PLATFORM_N64`. If a future change to this feature needs a struct field, add it at the **tail** of the struct under the guard. Reordering or inserting fields mid-struct breaks the decompilation contract.
