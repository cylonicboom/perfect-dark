# Perfect Dark Port — Claude Reference

## Environment Rules (READ FIRST)

**Do not build.** This repo only compiles inside an MSYS2 MinGW x64 shell, which isn't reachable from this Claude environment. Never run `make`, `cmake --build`, `ninja`, or any other compile invocation — the user builds externally and reports back. Reading CMake files, headers, and verifying code by inspection is fine.

## Code Writing Guidelines

- **Do not rename decompiled symbols.** Identifiers under `src/` map to the original N64 binary; renaming silently breaks the decompilation contract.
- Keep changes minimal and focused on the task at hand — no surrounding cleanup, no speculative abstractions, no refactor-while-you're-there.
- Respect existing patterns. If you're adding netplay-aware code, use the guard patterns documented in `src/game/CLAUDE.md` (`#ifndef PLATFORM_N64`, `g_NetMode != NETMODE_CLIENT`).
- Take notes as you work and re-read changes before reporting done — this catches scope creep and broken assumptions early.

## Repository Overview

Work-in-progress port of the [Perfect Dark N64 decompilation](https://github.com/n64decomp/perfect_dark) to modern platforms (Windows, Linux, macOS, Nintendo Switch). The source ROM must be supplied separately.

- **Main branch**: `port` — stable port, no netplay.
- **Netplay branch**: `port-net` (`remotes/origin/port-net`) — experimental internet/LAN multiplayer.
- **Local netplay-predict branch**: `port-net-predict` — adds CSP, entity interpolation, and lag compensation on top of `port-net`. Forked at https://github.com/murkantor/perfect_dark_netplay.

## Repo Layout

```
CMakeLists.txt          — build system entry point
src/game/               — original N64 game logic (decompiled C)
src/include/            — game headers (types, constants, bss, data)
src/lib/                — original N64 lib code (sched, vi, snd, etc.)
port/src/               — port-specific platform layer
port/include/           — port headers
port/fast3d/            — libultraship fast3d renderer (OpenGL)
port/external/          — bundled third-party libs (minimp3, enet on port-net)
docs/                   — supplemental docs (netplay.md, etc.)
tools/                  — helper scripts
```

## Build System

CMake with `Unix Makefiles`. Does NOT support Visual Studio. Key flags:
- `-DROMID=pal-final` / `-DROMID=jpn-final` for non-NTSC builds
- Target executable: `build/pd.<arch>[.exe]`

**Toolchain (Windows): MSYS2.** Install dependencies once from an MSYS2 shell:

```
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-zlib mingw-w64-x86_64-cmake mingw-w64-x86_64-python3 mingw-w64-i686-toolchain mingw-w64-i686-SDL2 mingw-w64-i686-zlib mingw-w64-i686-cmake mingw-w64-i686-python3 make git
```

Build from the **MSYS2 MinGW x64** shell (not the plain MSYS2 shell). If the exe launches with `0xc000007b`, check DLL bitness — a 32-bit DLL next to a 64-bit exe is the usual cause.

## Netplay (high level)

Client-server over [ENet](http://enet.bespin.org) reliable UDP — the host is always the server, up to `MAX_PLAYERS` (8) clients including the host itself. Default port `27100`, configurable via `pd.ini` or `--port`. Protocol version constant lives in `port/include/net/net.h` (`NET_PROTOCOL_VER`) and must be bumped whenever the wire format changes. Status: highly experimental, only Combat Sim is partially functional. For protocol, CSP, interpolation, and lag-comp design, see `port/src/net/CLAUDE.md`.

## How to find things

**Grep first, read second.** This codebase is ~half a million lines of decompiled C — speculative reading is wasteful. Common symbol prefixes to grep for:

- `g_*` — globals (game state, config, BSS-resident tables). Examples: `g_Vars`, `g_NetMode`, `g_BotConfigsArray`, `g_NetLocalClient`.
- `net*` — netplay functions and structs in `port/src/net/` and `port/include/net/`.
- `SVC_*` — server→client message IDs (declared in `port/include/net/netmsg.h`).
- `CLC_*` — client→server message IDs (same file).
- `UCMD_*` — input bitflags on `netplayermove.ucmd` (`net.h`). `UCMD_FL_FORCE*` are server force-correction bits.
- `CLSTATE_*` — client connection-state enum (`net.h`).
- `NETMODE_*` — `NONE` / `SERVER` / `CLIENT`. The host has `NETMODE_SERVER`; gate server-only writes on `g_NetMode != NETMODE_CLIENT`.

For per-file purpose lookups (which `.c` does what), consult `docs/CODEMAP.md` before opening files at random.

## Nested CLAUDE.md and docs index

### Auto-loaded CLAUDE.md (loaded when Claude Code operates under the matching directory)

- `port/src/CLAUDE.md` — Port platform-layer file index (`main.c`, `pdsched.c`, `video.c`, `console.c`, …) and console scrollback behaviour. Loads when editing anything under `port/src/`.
- `port/src/net/CLAUDE.md` — Full netplay protocol (SVC_*/CLC_* tables, player-move struct, prop sync), CSP / entity-interpolation / lag-comp design notes, debug console commands, F9 overlay. Loads when editing anything under `port/src/net/`.
- `port/include/net/CLAUDE.md` — Net header inventory; field-ordering and protocol-version gotchas (`netClientNeedMove` memcmp exclusion, `NET_CSP_*` macro aliases). Loads when editing net headers.
- `src/game/CLAUDE.md` — Net-guard patterns specific to decompiled game logic (`#ifndef PLATFORM_N64`, `g_NetMode != NETMODE_CLIENT`); positional weapon sounds design. Loads when editing anything under `src/game/`.
- `src/game/mplayer/CLAUDE.md` — Multiplayer subsystem: deterministic bot allocation invariant, `g_BotConfigsArray` wire sync, `actiontype` non-sync gotcha. Loads when editing MP setup / scenarios / scoring.
- `src/include/CLAUDE.md` — Shared headers; flags the five port-only additions to `constants.h` (`MPOPTION_NOCULL`, `CHEAT_NOCULL`, …) and the cheat-index append-only rule. Loads when editing shared headers.

### Load-on-demand docs (read when the situation matches)

- `docs/CODEMAP.md` — One-line purpose per non-obvious `.c` / `.h` under `src/` and `port/`. Read when you need to locate which file does X before opening anything.
- `docs/PORT_NET_PREDICT_CHANGES.md` — Per-file rationale for every change on `port-net-predict`. Read when you need to understand *why* a file was modified, not just what changed.
- `docs/PORT_NET_KNOWN_ISSUES.md` — Current netplay limitations and partially-broken features. Read before promising any feature works on clients, or when diagnosing client-only bugs.
- `docs/PORT_NET_REVERTED_EXPERIMENTS.md` — Failed approaches with root-cause analysis. Read before re-attempting anything that smells like a previously-tried idea.
- `docs/PORT_NO_CULLING.md` — Port-only "No Room Culling" / "No Draw Slot Limit" MP option + cheat feature. Read when touching `bg.c` draw-slot logic, `cheats.c` infrastructure, or the upper MP-option bits.

## Submodules / do-not-modify paths

- `tools/recomp` — submodule pointing at [Emill/ido-static-recomp](https://github.com/Emill/ido-static-recomp.git). Do not commit edits inside this path; it tracks upstream independently.
- `port/external/` — bundled third-party sources (ENet, minimp3, GLAD). Treat as vendored; upstream patches go in commit messages, not local edits without documentation.
- `port/fast3d/` — libultraship fast3d renderer carried in-tree. No netplay modifications; treat changes here as renderer work, not port work.
