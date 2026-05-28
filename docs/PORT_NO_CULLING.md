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
