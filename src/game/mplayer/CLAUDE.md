# Multiplayer Subsystem (`src/game/mplayer/`)

> Do not rename symbols — identifiers here map to the original N64 binary. For net guard patterns and the broader netplay context, see [`../CLAUDE.md`](../CLAUDE.md) and [`../../../port/src/net/CLAUDE.md`](../../../port/src/net/CLAUDE.md).

Owns multiplayer session setup, bot allocation, scoring, and per-scenario logic. The allocation path in `setup.c` must stay deterministic — it runs identically on server and client using synced RNG seeds.

## Key Abstractions

| Symbol | File | Notes |
|---|---|---|
| `botmgrAllocateBot` | `setup.c` | Allocates sim chr props using `g_NetRngSeeds`; server and client must produce identical `g_MpBotChrPtrs[]` |
| `g_BotConfigsArray` | `setup.c` | Bot head/body/team/name config; server-authoritative, synced via `SVC_STAGE_START` before `mpStartMatch` runs |
| `g_BotCount`, `g_MpBotChrPtrs[]` | `setup.c` | Bot count and chr pointer array; derived from deterministic allocation, never synced directly |
| `mpStartMatch` | `mplayer.c` | Match start entry point; clients call this after `g_BotConfigsArray` is populated from the wire |
| `scenarios/*.inc` | `scenarios.c` | Per-scenario option menus and tick logic; `#include`d into `scenarios.c`, not standalone translation units |

## Conventions

- `#ifndef PLATFORM_N64` wraps port-only net options added to scenario menus (e.g., the Static Spawn dropdowns in `holdthebriefcase.inc`/`hackthatmac.inc`); the shared port options live on the "More Options" / "Classic Options" sibling pages in `setup.c`.
- `g_NetMode != NETMODE_CLIENT` guards server-only score and state writes in `mplayer.c` death handling.
- `.inc` files define static arrays and functions scoped to `scenarios.c`'s translation unit — treat them as part of that file.

## Gotchas

- **Do not insert allocations between `netClientSyncRng()` and `botmgrAllocateBot`.** Any allocation that runs on one side but not the other desynchronises `g_MpBotChrPtrs[]`, breaking sim identity on clients.
- **`g_BotConfigsArray` is stale on clients until `SVC_STAGE_START` arrives.** Adding a new bot config field requires wiring it through `netmsgSvcStageStartWrite/Read`; otherwise clients use whatever the local Combat Sim menu last had.
- **`chr->actiontype` is intentionally not synced.** Applying the server's actiontype on the client crashes on uninitialised union data. Clients always dispatch as `ACT_STAND`. See [`../CLAUDE.md`](../CLAUDE.md).

## Offline 32 Simulants (docs/PORT_OFFLINE_32_SIMS.md)

`MAX_BOTS` is 32 on the port; **net games cap at `NET_MAX_BOTS` (8)** and file
images at `MAX_BOTS_PRESET` (8). chrslots is u64 — bot bits 16-47, always via
`MPCHRSLOT(n)`. The `setupCreateProps` bot-spawn RNG-parity clamp (net →
exactly 8 iterations) is load-bearing for cross-peer determinism. Preset/wad
loads disable rows 8-31 so session extras never leak into challenges.
