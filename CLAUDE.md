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
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-sdl3 mingw-w64-x86_64-zlib mingw-w64-x86_64-cmake mingw-w64-x86_64-python3 mingw-w64-i686-toolchain mingw-w64-i686-sdl3 mingw-w64-i686-zlib mingw-w64-i686-cmake mingw-w64-i686-python3 make git
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
- `docs/PORT_GOLDENEYE.md` — Port-only "GoldenEye Style" Combat Sim option (`MPOPTION_GOLDENEYE`): snap lean, lower-and-raise reloads, ledge wall, classic crosshair, hide-unless-aiming, GE-style vertical health/shield HUD, no secondary functions, no mid-crouch, no dual-wield. Documents the reusable `bgunSecondaryFunctionDisabled` / `bgunDualWieldDisabled` helpers that future weapon-loadout work plugs into.
- `docs/PORT_KOH_STATIC_HILL.md` — Port-only KotH "Static Hill" dropdown (per-stage hill picker) + Mobile/Static mode dropdown. Read when touching `kingofthehill.inc` hill selection, `mpsetup` versioning, or `SVC_STAGE_START` payload ordering.
- `docs/PORT_CTC_STATIC_BASE.md` — Port-only CTC per-team base pins (four "Team N Base" dropdowns, dynamic enable based on player count). Read when touching `capturethecase.inc` team-assignment, `mpsetup.ctcteambase[4]`, or the menu handler patterns that share `item->param` across multiple entries.
- `docs/PORT_HTB_HTM_STATIC_SPAWN.md` — Port-only HTB / HTM "Static Spawn" dropdowns. Read when touching `holdthebriefcase.inc` `htbCreateToken`, `hackthatmac.inc` `htbCreateUplink` (which is the HTM function — naming quirk), or the 28-pad constant derived from intro-command counting.
- `docs/PORT_SDL3_EXTRAS.md` — Port-only SDL3-era extras: per-player gamepad RGB-LED colours with low-health flash (`Input.GamepadLED`, `inputUpdatePadLEDs`), impulse-trigger rumble mirror (`Input.PlayerN.TriggerRumble`, gated on gamepad capability properties), boot-time taskbar progress (`videoSetTaskbarProgress` / new `set_taskbar_progress` wmAPI entry, SDL 3.4+ only, guarded `SDL_VERSION_ATLEAST(3,4,0)`), and **gyro aim** (pad 1 gamepad gyro integrated by sensor timestamps in the event watcher, merged into `inputMouseGetScaledDelta`; `/gyro`, `Input.GyroAim`/`GyroSpeedX/Y`, default off). Read when touching `inputRumble`/`inputInitController`/`inputMouseGetScaledDelta`, the `GfxWindowManagerAPI` struct, or `romdataInit` boot preprocessing.
- `docs/PORT_WEAPON_PRESETS.md` — Port-only Custom Weapon Presets system (`mpsetups.bin` v2 tail; per-slot `FNFLAG_*` bits; `g_MpWeaponPresets[]`; `g_MpSlotFnFlags[]`). Read when touching the Combat Sim Weapons menu, the Random Preset rotation, or the `bgunPrimary/SecondaryFunctionDisabled` helpers.
- `docs/PORT_NODOORS.md` — Port-only "No Doors" Combat Sim option. Historically introduced the `g_MpSetup.portoptions` overflow word; that word was **later removed** when `g_MpSetup.options` was widened to `u64` (`MPOPTION_NODOORS` now = bit 32, `0x0000000100000000ULL`, tested against `options`). Read when touching `setupMarkLiftDoors`/the `OBJTYPE_DOOR` skip in `setup.c`, the high-word `menuhandlerMpCheckboxPortOption`, or how high-bit (32-63) MP options ride `options` in `SVC_STAGE_START` / `CLC_ADMIN_SETUP` / `SVC_LOBBY_STATE` and the mpsetups.bin wad. The doc's top callout summarises the merge; the body below it is the original portoptions design (historical).
- `docs/PORT_HOST_SPECTATOR.md` — Port-only Host Spectator Mode (`MPOPTION_HOSTSPECTATOR`, WIP/paused). Read when touching `port/src/spectator.c`, the `is_spectator` wire bytes in `SVC_STAGE_START` / `SVC_LOBBY_STATE`, or `playerGetLocalCount` / `LOCALPLAYERCOUNT()`. **Has known breakage with remote combatants — read the "Known breakage" section before extending.**
- `docs/PORT_CLIENT_SPECTATOR.md` — Port-only Client Spectator (render-redirect): a client/host renders a spectated **player's own viewport** (full first-person/HUD) by substituting the target's slot in `lvRender`'s per-player loop, plus on-death auto-spectate, `/spec toggle`, sim camera, and the `g_NetSpectateChr` dangling-pointer guards. Read when touching the `lvRender` per-player loop, `netSpectate*` in `net.c`, or `mainEndStage`. Contrast with the (broken) panel-based `SPEC_MODE_PLAYER` in `PORT_HOST_SPECTATOR.md`.
- `docs/PORT_WIREFRAME.md` — Port-only "Wireframe" cheat (Cheats → Gameplay). Draws depth-tested 3D geometry as polygon outlines via `glPolygonMode` in the fast3d GL backend; HUD/2D stay solid. Cheat state (`CHEAT_WIREFRAME`) is synced per-frame to the renderer global `gfx_wireframe_mode` in `bgTickPortals` (the `CHEAT_NOCULL` pattern). Read when touching `gfx_opengl_draw_triangles`/`gfx_opengl_set_depth_mode`, the `gfx_*` render-toggle globals, or the depth-test wireframe gate. GL-ES is a no-op (desktop-GL only). Live toggle: `/wireframe [on|off]` console command. **Documents the `bool`=`s32` (game) vs 1-byte (renderer) bridging gotcha** — read before sharing any flag between decompiled game code and the fast3d renderer.
- `docs/PORT_OCTREE.md` — Port-only outdoor-room octree frustum-culling. Rooms flagged `ROOMFLAG_EX_OCTREE` (port-only `extra_flags` bit, separate from `ROOMFLAG_OUTDOORS`) build a per-room octree of the existing `vtxbatches[]` at load (`bgBuildRoomOctree`); each render pass traverses it, rejects offscreen subtrees, and emits a filtered copy of the leaf display list into `gfxAllocate` scratch (`bgEmitLeafCulled`) — copying state commands unconditionally, dropping only culled batches' `G_VTX`/`G_TRI*`. Read when touching `bgRenderRoomPass`'s LEAF case, `bgRenderRoomOpaque`/`Xlu`, `bgFindRoomVtxBatches`/`bgPopulateVtxBatchType`, or the per-frame gfx pools. Live toggle: `/octree [on|off|forcecull|stats]`. Non-octree path and N64 build are byte-identical.
- `docs/PORT_DLCACHE.md` — Port-only Fast3D "display-list cache": cache static room geometry in persistent GPU buffers and replay it with a GPU-side `uMVP` (added to every shader, identity for the immediate path), instead of CPU-transforming every vertex each frame. The enabling insight is that all CPU post-transform fixups (aspect-X, invert-Y, z-remap) are linear and fold into one matrix. New EXT opcodes `G_DLCACHE_BEGIN_EXT`/`END_EXT` (`gbiex.h`) bracket each room leaf (`bg.c` `bgRenderRoomPass`); the renderer keys the cache by the leaf `gdl` pointer, records object-space geometry on a miss (tee in `gfx_sp_tri1`/`gfx_sp_vertex`/`gfx_flush`), and replays per-segment in `dlcacheReplay`. Read when touching the fast3d vertex/flush hot path, the `uMVP` uniform, the `cache_*` rapi entries, or texture-cache invalidation (the cache is dropped on any texture-cache change so stored GL texture ids can't dangle). Live toggle: `/dlcache [on|off|stats|clear|ff]`. **Phase 1+2 done** (octree-aware: cached replay culls per vtxbatch via `g_BgCullVisible`, passed to the renderer in the `BEGIN` opcode), **plus GPU distance fog and dynamic vertex lighting** (shader-side palette) — 2-cycle/multitexture/grayscale/fog/dynamic-lighting all cached. Remaining roadmap coverage is the view-dependent cases only: `G_TEXTURE_GEN` (reflections), `G_LIGHTING` (normal lighting), and whole-room dyntex exclusion. Default off; N64 + `/dlcache off` byte-identical.
- `docs/PORT_GLARE_OCCLUSION.md` — Port-only fix so light glares (corona/bloom sprites, `artifact.c` → `func0f0b2150`) are occluded by the first-person weapon/hands instead of painting over them. Glares are depth-less 2D texrects whose visibility is a world-only LOS test (`artifactTestLos`), so a light behind the gun still emits one; the fix reorders `playerRenderHud` to draw `bgRenderArtifacts` **before** `bgunRender` (port-only `#ifndef PLATFORM_N64`), letting the opaque gun overdraw glares behind it (gun renders into a freshly-cleared depth buffer, so overdraw == depth-test). Read when touching the `playerRenderHud` first-person draw order, glare/artifact rendering, or before attempting a renderer-side texrect depth-test (fast3d draws rects at fixed `z=-1` and ignores prim-depth). N64 byte-identical.
- `docs/PORT_MIRROR.md` — Port-only "Mirror" cheat (Cheats → Gameplay): flips the entire rendered 3D world left-right (a horizontal reflection) in the fast3d GL backend; works everywhere including the Carrington Institute hub (shared `lvRender`→`bgRender` path, no special-casing). 2D HUD/text stay un-mirrored. Purely cosmetic (gameplay/hit-detection run on un-mirrored coords) so it's net/save-safe. `CHEAT_MIRROR` syncs per-frame to renderer global `gfx_mirror_mode` in `bgTickPortals` (the `gfx_wireframe_mode` pattern). The flip is **negate clip-space X + compensate winding + reflect the per-room scissor** across BOTH render paths: immediate (`gfx_sp_vertex` X-negate, `gfx_sp_tri1` cross-negate — CPU cull) and dlcache replay (`uMVP` X-scale negate, `cache_set_cull` front-face flip — GPU cull); the portal draw-slot scissor is reflected about the viewport centre via `gfx_mirror_scissor_x`. Read when touching the fast3d vertex/tri/scissor path or the viewmodel. Live toggle: `/mirror [on|off]`. **Phase 2 (gun as a normal model in the left hand, not a mirror-image) is not yet implemented** — Phase 1 mirror-images the viewmodel along with the world. Default off; N64 byte-identical.
- `docs/PORTING_HOWTO.md` — Methodology guide for lifting any port-only feature in this repo into another fork / branch. Covers the decompilation contract, the two guard patterns, MPOPTION budget, protocol version discipline, wad versioning, deterministic invariants, helper choke points. Read once, then use as a checklist alongside the per-feature doc.
- `docs/PORT_COOP_PLAN.md` — The **actionable** campaign co-op build plan (sequenced phases + concrete hook points). Locked decisions: one-level vertical slice, up to 4 players (built 2→4), checkpoint respawn. Identifies the linchpin NPC-AI client gate (`chrTick`→`chraTick`→`chraiExecute`, chr.c:2538 / chraction.c:13947), the host chr-state broadcast generalization (`g_MpBotChrPtrs`→`g_ChrSlots`), and the 2-vs-4 player-model risk (engine co-op is 2-player). Read with PORT_COOP_PREP.md (design rationale).
- `docs/PORT_COOP_PREP.md` — Pre-plan design notes for **campaign co-op** over the existing netplay. Inventories the chr-replication stack that Combat-Sim sim sync already provides (position interp, anim cross-fade, room time-align, vertical grounding, HP/shield) and enumerates what co-op adds: non-`aibot` NPC AI gating, the `actiontype`/union problem at campaign scope, objective/script/stage-flow sync, the `chr->aibot`-gated net logic that must widen to all client-driven chrs, and a recommended first vertical slice. Read before scoping or planning co-op work.

## Submodules / do-not-modify paths

- `tools/recomp` — submodule pointing at [Emill/ido-static-recomp](https://github.com/Emill/ido-static-recomp.git). Do not commit edits inside this path; it tracks upstream independently.
- `port/external/` — bundled third-party sources (ENet, minimp3, GLAD). Treat as vendored; upstream patches go in commit messages, not local edits without documentation.
- `port/fast3d/` — libultraship fast3d renderer carried in-tree. No netplay modifications; treat changes here as renderer work, not port work.
