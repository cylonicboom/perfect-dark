# Shared Game Headers (`src/include/`)

> Do not rename any symbol in this directory — all identifiers map to decompiled N64 addresses.

Shared types, constants, BSS layout, and global data declarations for the original N64 game logic.

## Key Files

| File | Notes |
|---|---|
| `constants.h` | Game-wide constants and enums; port-only additions are at the bottom or under `#ifndef PLATFORM_N64` |
| `bss.h` | BSS section layout — global variable declarations in original link order |
| `data.h` | Global data table declarations (`g_Vars`, etc.) |

## Port-only Additions in `constants.h`

These are new — not original N64 constants. Do not remove or renumber them:

- `CHEAT_NOCULL 43`, `CHEAT_NODRAWLIMIT 44` — **retired** (feature replaced by `/octree`); defines + `g_Cheats[]` rows kept as dead placeholders so later indices don't shift
- `CHEAT_GOLDENEYE 45`, `CHEAT_WIREFRAME 46`, `CHEAT_MIRROR 47`, `CHEAT_TONALINVERSION 48` — appended after all original cheat indices; menu items live in Extended Options > Experiments (`port/src/optionsmenu.c`), not the Cheats menu
- `CHEAT_CLASSIC_* 49-60` — the GoldenEye Style rule set broken into per-behaviour cheats (Snap Lean … No Blur Effects); each pairs with an `MPOPTION_CLASSIC_*` high-word bit via `classicOptionActive()` (see `docs/PORT_GOLDENEYE.md`)
- `CHEATFLAG_ALWAYSUNLOCKED 16` — new flag bit, absent from the original
- `ROOMFLAG_EX_OCTREE 0x0002` — port-only bit in `struct room.extra_flags` (the overflow word), marks a room for octree frustum-culling (see `docs/PORT_OCTREE.md`)
- `AIENVCMD_ROOM_SETOCTREE 0x10` — port-only AI env-command, appended after `STOPUFOHUM 0x0f`; sets `ROOMFLAG_EX_OCTREE` via `configure_environment` / `aiSetRoomOctree` (handled `#ifndef PLATFORM_N64` in `aiConfigureEnvironment`). Append-only — no N64 setup uses `0x10`
- `MPOPTION_HOSTSPECTATOR 0x40000000`, `MPOPTION_GOLDENEYE 0x80000000` — new MP option bits in the upper byte of the original 32-bit word. Bits `0x10000000`/`0x20000000` (the retired `MPOPTION_NOCULL`/`MPOPTION_NOOMLIMIT`) are **reserved — do not reuse** (stale saved setups may carry them). **`struct mpsetup.options` was widened to `u64`** (unconditionally, on every build — the N64 offset comments on the fields below it are now stale), so bits 32-63 are free for new port-only options — write them with a `ULL` suffix (e.g. `MPOPTION_NODOORS 0x0000000100000000ULL`). High-word bits used so far: 32 NODOORS, 33 OWNEDROOMSPAWN, 34-45 `MPOPTION_CLASSIC_*` (the GoldenEye Style breakdown, `docs/PORT_GOLDENEYE.md`); 46-63 free. The old `portoptions` overflow word is gone — it was folded into these high bits. Full 64 bits sync over the wire and persist to `mpsetups.bin` (the old 32-bit `portoptions` tail word was repurposed as the high word, so the 80-byte block is unchanged in size — but it now has only ~10 spare bits, so a *new* saved field needs `MPSETUP_BLOCKSIZE` enlarged). Menu toggles for high-word bits use `menuhandlerMpCheckboxPortOption` (param3 = `BIT >> 32`, shifted back up by 32) because `menuitem.param3` is only 32-bit.

## Gotchas

- **Cheat indices are array indices into `g_Cheats[]`.** Appending is safe; inserting mid-array shifts every subsequent index and breaks saves, menu references, and hardcoded comparisons throughout the codebase.
- **`MPOPTION_*` bits in the upper word are port-only.** The original game only uses the lower 24 bits of the options bitmask — upper bits are free for port extensions. The field is now `u64`; bits 32-63 are available for new options and are synced over the wire *and* saved to disk (see above).
