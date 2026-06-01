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

- `CHEAT_NOCULL 43`, `CHEAT_NODRAWLIMIT 44`, `CHEAT_GOLDENEYE 45`, `CHEAT_WIREFRAME 46` — appended after all original cheat indices
- `CHEATFLAG_ALWAYSUNLOCKED 16` — new flag bit, absent from the original
- `MPOPTION_NOCULL 0x10000000`, `MPOPTION_NOOMLIMIT 0x20000000`, `MPOPTION_HOSTSPECTATOR 0x40000000`, `MPOPTION_GOLDENEYE 0x80000000` — new MP option bits in the upper word (the byte is now fully allocated)

## Gotchas

- **Cheat indices are array indices into `g_Cheats[]`.** Appending is safe; inserting mid-array shifts every subsequent index and breaks saves, menu references, and hardcoded comparisons throughout the codebase.
- **`MPOPTION_*` bits in the upper word are port-only.** The original game only uses the lower 24 bits of the options bitmask — upper bits are free for port extensions.
