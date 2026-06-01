# Perfect Dark Port — Code Map

One-line orientation for every non-obvious `.c` / `.h` under `src/` and `port/`.
Files are grouped by folder; files whose path alone explains their purpose are omitted.

---

## src/ (root)

| File | Purpose |
|---|---|
| `src/firingrange.c` | Firing-range tutorial text/commands used in the pre-NTSC-1.0 training stage |
| `src/textureconfig.c` | Prebuilt GBI state blocks for the standard texture rendering modes |

---

## src/assets/

Each ROM variant (`ntsc-final`, `pal-final`, `jpn-final`, `ntsc-1.0`, betas) has:
- `files/list.c` — compiled table mapping file IDs to ROM segment offsets for that build
- `mpstrings/mpstrings*.c` — multiplayer UI string tables for each language code (`E`=English, `F`=French, etc.)

---

## src/include/

| File | Purpose |
|---|---|
| `asm_helper.h` | Macros that align decompiled assembly stubs with the MIPS ABI calling convention |
| `bss.h` | `extern` declarations for all BSS (zero-initialized global) variables |
| `commands.h` | Opcode enum for level-setup command bytecode in stage binary files |
| `constants.h` | Umbrella include: pulls in `versions.h`, `math.h`, `files.h`, `sfx.h`, `animations.h` |
| `data.h` | `extern` declarations for all initialized global variables |
| `files.h` | File-ID enum (`FILE_*`) for every asset segment in the ROM |
| `gbiex.h` | Extra GBI display-list macros not present in the original N64 SDK headers |
| `gunscript.h` | Opcode enum for weapon animation script (`GUNCMD_*`) bytecode |
| `intro.h` | Hard-coded intro cutscene data (timing, text, camera) |
| `lang.h` | String-ID enum (`L_*`) for every localised text entry |
| `macros.inc` | MIPS assembler macros shared by `.s` files |
| `pads.h` | Pad-ID enum: every named waypoint/object-tag in every stage |
| `platform.h` | Compiler/target abstraction macros (`PLATFORM_N64`, `PLATFORM_64BIT`, endian helpers) |
| `props.h` | Macro DSL for encoding prop objects in C-source stage-setup files (door, stdobject, key, etc.) |
| `sfx.h` | Sound-effect ID enum (`SFX_*`) |
| `sgidefs.h` | SGI/MIPS fundamental type definitions required by the N64 SDK |
| `stagesetup.h` | Binary-format constants and structs for stage-setup segment parsing |
| `textureconfig.h` | Declarations for the GBI texture-mode state blocks in `src/textureconfig.c` |
| `tiles.h` | Tile-ID enum for 2-D menu/HUD graphics |
| `tvcmds.h` | Opcode enum for TV (cinematic/cutscene) command script |
| `types.h` | Core game type definitions: `struct prop`, `struct chr`, `struct coord`, `struct player`, etc. |
| `util.h` | Small inline/macro utilities (ARRAYCOUNT, alignment, bit manipulation) |
| `versions.h` | Version constants (`VERSION_NTSC_FINAL`, `VERSION_PAL_FINAL`, etc.) and comparison macros |

### src/include/game/ (non-obvious headers)

| File | Purpose |
|---|---|
| `bg.h` | Portal/room visibility culling and level BSP rendering API |
| `bondcutscene.h` | In-engine scripted cutscene playback |
| `bondeyespy.h` | Camera drone / FarSight X-Ray optic control |
| `bondgrab.h` | Player door-grab and interaction system |
| `bondgun.h` | Player weapon state, fire logic, animation triggers |
| `bondhead.h` | Head look-at and neck-tracking system |
| `bondmove.h` | Player physics input processing; remote-player animation interpolation |
| `bondview.h` | First-person camera, view bob, zoom |
| `bondwalk.h` | Player walk movement; remote-player position ring-buffer interpolation |
| `bossfile.h` | Dossier (career save file) load/save API |
| `botact.h` | AI action-state logic (patrol, attack, retreat, etc.) |
| `botcmd.h` | AI command interpreter (script-bytecode opcode dispatch) |
| `botinv.h` | Bot weapon-inventory management |
| `botroom.h` | Bot room-awareness and navigation state |
| `casing.h` | Bullet casing ejection particle |
| `chr.h` | Character struct lifecycle: allocation, init, destroy |
| `chraction.h` | Character per-frame action-tick dispatch (`ACT_STAND`, `ACT_GOPOS`, etc.) |
| `chrai.h` | Character AI state evaluation and transition |
| `chraicommands.h` | AI script bytecode opcode table |
| `chrmgr.h` | Character roster manager (slot allocation, lookup) |
| `dlights.h` | Dynamic light source management |
| `dyntex.h` | Animated texture scrolling on room geometry vertices |
| `env.h` | Environment parameters: fog colour, ambient light level |
| `file.h` | ROM asset file loading by file ID (low-level) |
| `filelist.h` | Compiled file-ID-to-segment table for the active ROM variant |
| `filemgr.h` | Asset file manager: caching, DMA dispatch, decompression |
| `floor.h` | Floor and ground surface geometry queries |
| `fmb.h` | In-match Combat Simulator options menu (drop out, abort, accept challenge) |
| `gfxmemory.h` | Per-frame graphics heap (`gfxAllocate`); resets each frame via `gfxSwapBuffers` |
| `gfxreplace.h` | In-place GBI display-list texture patching for material swaps |
| `gunfx.h` | Muzzle flash and weapon fire visual effects |
| `inv.h` | Player weapon-slot inventory |
| `lang.h` | Language/text rendering system |
| `lv.h` | Level tick and render orchestrator (the game's main per-frame driver) |
| `modeldef.h` | Model-slot definitions (maps model ID to ROM file ID) |
| `modelmgr.h` | Model asset manager (load and cache by slot) |
| `mpstats.h` | Multiplayer kill/death/score stat recording |
| `mtxf2lbulk.h` | Bulk float-matrix to fixed-point conversion for the RDP |
| `nbomb.h` | N-bomb / nerve-gas grenade weapon (gas cloud, damage, overlay) |
| `pad.h` | N64 controller pad reading and button-state management |
| `padhalllv.h` | AI navigation: waypoint/waygroup pathfinding (find-route, A*, segment enable/disable) |
| `pak.h` | Controller Pak (memory card) read/write via the PFS API |
| `pdmode.h` | "Perfect Dark" difficulty scaling (enemy health and reaction speed) |
| `portal.h` | Portal-graph BSP room visibility API |
| `portalconv.h` | Portal binary data loading and pointer fixup |
| `prop.h` | Entity (prop) system: spawn, despawn, type dispatch |
| `propobj.h` | Object-type props: doors, crates, weapons, pickups, projectiles |
| `propobjstop.h` | Prop object cleanup |
| `propsnd.h` | Positional sound emitters attached to props |
| `race.h` | Animation-based velocity integration (sums translated angle per anim frame) |
| `room.h` | Room BSP node allocation and portal-graph adjacency |
| `savebuffer.h` | EEPROM save buffer serialise/deserialise |
| `setup.h` | Stage binary parsing: chr/prop/waypoint/cover data extraction |
| `setuputils.h` | Stage setup parsing helpers |
| `shards.h` | Glass shard particle system |
| `sight.h` | Line-of-sight tests between characters and props |
| `sky.h` | Skybox and distant background rendering |
| `stagemusic.h` | Per-stage background music track assignment |
| `stagetable.h` | Master table of all game stages (name, file, flags) |
| `tex.h` | Texture loading, palette management, cache |
| `texdecompress.h` | N64 RZIP texture decompression |
| `timing.h` | Frame timing, diffframe counter, tick rate |
| `utils.h` | Geometry/math utilities (coord transforms, trig helpers) |
| `vtxstore.h` | Persistent deformable vertex buffer (soft-body / destructible geometry) |
| `wallhit.h` | Bullet wall-impact decal and particle |
| `zbuf.h` | Z-buffer (depth buffer) and colour framebuffer allocation |
| `game_*.h` | Unnamed decompiled code units, identified by ROM offset only |
| `stubs/game_*.h` | Stub declarations for very small unnamed decompiled functions |

### src/include/lib/ (non-obvious headers)

| File | Purpose |
|---|---|
| `ailist.h` | AI bytecode list interpretation engine |
| `args.h` | Command-line argument parsing (N64 boot args) |
| `audiodma.h` | N64 audio DMA transfer management |
| `audiomgr.h` | N64 audio manager thread |
| `boot.h` | N64 boot entry point and memory initialization |
| `collision.h` | Low-level collision detection (cylinder/sphere vs. BSP portals) |
| `crash.h` | N64 crash/fault handler |
| `debughud.h` | Debug text overlay (`dhudSetPos`, `dhudPrintf`, colour/bg) |
| `dma.h` | DMA block transfer helpers |
| `fault.h` | N64 hardware fault/exception handler |
| `joy.h` | N64 joystick/controller polling |
| `mema.h` | Ad-hoc allocator: 300 KB pool supporting individual frees, reset per stage |
| `memp.h` | Main pool allocator: two banks (onboard + Expansion Pak), reset per stage |
| `model.h` | 3-D skeletal model: load, tick (anim advance), render (RDP display list) |
| `mp3.h` | Top-level MP3 audio playback wrapper |
| `mtx.h` | N64 RSP-compatible 4×4 matrix operations |
| `path.h` | Navigation path following (advance along a waypoint route) |
| `pimgr.h` | PI (cartridge parallel interface) DMA manager thread |
| `profile.h` | N64 RSP/RDP timing profiler |
| `rdp.h` | RDP display list task submission |
| `reset.h` | N64 software reset handler |
| `rmon.h` | Remote monitor / debugger via UART |
| `rng.h` | Random number generator |
| `rzip.h` | RZIP (run-length + inflate) decompressor |
| `sched.h` | N64 multi-threaded scheduler (audio and graphics task threads) |
| `segments.h` | ROM segment boundary symbol declarations |
| `snd.h` | Non-positional (in-head) sound effect playback |
| `speaker.h` | Audio output speaker management |
| `str.h` | Custom string utilities and in-game font text rendering |
| `vars.h` | Global variable struct declarations |
| `vi.h` | N64 video interface (VI register control, mode set) |
| `videbug.h` | Video-interface debug output |
| `vm.h` | N64 virtual memory (TLB / 4 MB → 8 MB expansion) |

---

## src/game/

### Character and AI

| File | Purpose |
|---|---|
| `chr.c` | Character struct lifecycle: allocate, initialise, destroy |
| `chraction.c` | Per-frame action tick dispatch (`ACT_STAND`, `ACT_GOPOS`, `ACT_ATTACK`, etc.) |
| `chrai.c` | Character AI state evaluation and transition |
| `chraicommands.c` | AI script bytecode opcode dispatch |
| `chrmgr.c` | Character roster manager (slot allocation and lookup) |
| `chrmgrstop.c` | Character manager cleanup on stage unload |
| `bot.c` | Simulant (AI bot) per-frame tick |
| `botact.c` | Bot action-state logic (patrol, attack, retreat, throw grenade, etc.) |
| `botcmd.c` | Bot AI command interpreter (script-level opcode runner) |
| `botinv.c` | Bot weapon-inventory runtime management |
| `botinvinit.c` | Bot weapon-inventory initialisation from config |
| `botmgr.c` | Bot spawning and roster management |
| `botroom.c` | Bot room-awareness bookkeeping |
| `gailists.c` | Global AI bytecode sequences shared across all stages (guard idle, combat, etc.) |
| `sight.c` | Line-of-sight tests between characters and props |

### Player (Bond)

| File | Purpose |
|---|---|
| `bondwalk.c` | Player walk movement; remote-player position ring-buffer interpolation (CSP/interp target) |
| `bondmove.c` | Player physics input processing; applies remote-player animation state |
| `bondview.c` | First-person camera positioning, view bob, zoom |
| `bondhead.c` | Head look-at and neck-tracking system |
| `bondheadreset.c` | Head tracker reset |
| `bondgun.c` | Player weapon state machine, fire logic, animation triggers, positional sound routing |
| `bondgunreset.c` | Weapon state reset |
| `bondgunstop.c` | Weapon system cleanup |
| `bondcutscene.c` | In-engine scripted cutscene playback |
| `bondeyespy.c` | Camera drone and FarSight X-Ray optic control |
| `bondgrab.c` | Player door-grab and object-interaction system |
| `bondbike.c` | Vehicle (hoverbike) control |
| `body.c` | Dead body / ragdoll physics and rendering |
| `bodyinit.c` | Dead-body list initialisation |
| `bodyreset.c` | Dead-body state reset |

### Props (Entities)

| File | Purpose |
|---|---|
| `prop.c` | Entity (prop) system: spawn, despawn, per-type tick and render dispatch |
| `propobj.c` | Object-type props: doors, crates, weapons, pickups, projectiles |
| `propobjbss.c` | BSS section for prop-object globals |
| `propobjstop.c` | Prop object cleanup |
| `proptick.c` | Master prop-tick loop: iterates all live props each frame |
| `propsstop.c` | All-props cleanup on stage unload |
| `propsnd.c` | Positional sound emitters attached to props |
| `propsndreset.c` | Prop sound reset |
| `propsndstop.c` | Prop sound cleanup |
| `inv.c` | Player weapon-slot inventory |
| `invitems.c` | Weapon/item definition data |
| `invreset.c` | Inventory reset |
| `gunfx.c` | Muzzle flash and weapon-fire visual effects |
| `gunfxreset.c` | Gun-fx reset |
| `casingreset.c` | Bullet casing ejection particle reset |
| `casingtick.c` | Bullet casing per-frame physics tick |
| `splat.c` | Blood and liquid splat decals (puddle + drop types) |
| `wallhit.c` | Bullet wall-impact decal and debris particles |
| `wallhitreset.c` | Wall-hit particle reset |
| `nbomb.c` | N-bomb / nerve-gas grenade: gas cloud simulation, damage, HUD overlay |

### Level / World

| File | Purpose |
|---|---|
| `lv.c` | Level tick and render orchestrator (the per-frame game driver) |
| `bg.c` | Room BSP portal-culling and level background geometry rendering; port-only outdoor-room octree sub-room culling (`docs/PORT_OCTREE.md`) |
| `bgbss.c` | BSS section for `bg.c` globals |
| `portal.c` | Portal-graph BSP visibility determination |
| `portalconv_c.c` | Portal binary data loading and internal pointer fixup |
| `room.c` | Room BSP node allocation and portal-graph adjacency management |
| `roomreset.c` | Room node reset on stage load |
| `roomtick.c` | Per-room ambient trigger ticks |
| `floor.c` | Floor and ground surface geometry queries |
| `env.c` | Environment parameters: fog colour, ambient light level |
| `dlights.c` | Dynamic light source management |
| `dyntex.c` | Animated texture scrolling on room-geometry vertices |
| `sky.c` | Skybox and distant-background rendering |
| `skyreset.c` | Skybox reset |
| `skytick.c` | Skybox animation tick |
| `stars.c` | Star-field background (space/outdoor levels) |
| `zbuf.c` | Z-buffer (depth buffer) and colour framebuffer allocation and config |

### Stage Setup

| File | Purpose |
|---|---|
| `setup.c` | Stage binary parsing: chr, prop, waypoint, cover-point data extraction |
| `setupcover.c` | Cover-point extraction from stage setup binary |
| `setuppads.c` | Pad/tag object initialisation from stage setup |
| `setuputils.c` | Stage setup binary parsing helpers |
| `setupwaypoints.c` | Waypoint graph initialisation from stage setup |
| `padhalllv.c` | AI navigation: waypoint/waygroup pathfinding (route find, A*, segment enable/disable) |
| `stagetable.c` | Master table of all game stages with names, file IDs, and flags |
| `stagemusic.c` | Per-stage background music track assignment |
| `varsreset.c` | Game-state variable reset on every stage load |

### Graphics and Rendering

| File | Purpose |
|---|---|
| `camera.c` | Game camera positioning, cinematic/free-look animation |
| `camdraw.c` | Camera render pass setup |
| `gfxmemory.c` | Per-frame graphics heap (`gfxAllocate`); reset by `gfxSwapBuffers` each frame |
| `gfxreplace.c` | In-place GBI display-list patching to swap textures at runtime |
| `tex.c` | Texture loading, format decoding, palette management |
| `texdecompress.c` | N64 RZIP texture decompression |
| `texinit.c` | Texture system initialisation |
| `texreset.c` | Texture cache reset |
| `texselect.c` | Per-material texture selection for model surface rendering |
| `tilesreset.c` | 2-D tile (menu/HUD graphic) cache reset |
| `vtxstore.c` | Persistent deformable vertex buffer (destructible or animated geometry) |
| `mtxf2lbulkasm_c.c` | Bulk float-to-fixed-point matrix conversion for RDP submission |
| `dyntex.c` | Animated UV texture scrolling on room vertices |

### Menu and UI

| File | Purpose |
|---|---|
| `mainmenu.c` | Main menu screen logic |
| `menu.c` | Menu system (dialog stack, input routing) |
| `menugfx.c` | Menu graphics rendering |
| `menuitem.c` | Menu item type handlers (checkboxes, sliders, lists, etc.) |
| `menustop.c` | Menu cleanup |
| `menutick.c` | Menu per-frame tick driver |
| `activemenu.c` | In-game pause/quick menu |
| `activemenutick.c` | In-game menu tick |
| `fmb.c` | In-match Combat Simulator options menu (drop out, abort, accept challenge) |
| `hudmsg.c` | HUD message queue (timed objective/event text) |
| `healthbar.c` | Player health and shield bar rendering |
| `radar.c` | In-game minimap/radar |

### Saves and Files

| File | Purpose |
|---|---|
| `bossfile.c` | Dossier (career save file) load/save/defaults via Controller Pak or EEPROM |
| `gamefile.c` | Mission save-slot management (agent/SA/PA completion flags) |
| `savebuffer.c` | EEPROM save buffer serialise/deserialise |
| `pak.c` | Controller Pak (memory card) read/write via PFS API |
| `filemgr.c` | Asset file manager: caching, DMA dispatch, decompression |
| `filelist.c` | Compiled file-ID-to-ROM-segment table for the active ROM variant |
| `file.c` | Low-level ROM asset file loading by file ID |

### Misc Game Systems

| File | Purpose |
|---|---|
| `lang.c` | Localised text rendering and string lookup |
| `langinit.c` | Language system initialisation |
| `langreset.c` | Language system reset |
| `langtick.c` | Language ticker (animated text effects) |
| `cheats.c` | Cheat code list, lock/unlock logic, activation callbacks |
| `pdmode.c` | "Perfect Dark" difficulty scaling: enemy HP and reaction-speed modifiers |
| `options.c` | Global game option variables and persistence |
| `objectives.c` | Mission objective tracking and completion checks |
| `objectivesreset.c` | Objectives reset |
| `objectivesstop.c` | Objectives cleanup |
| `challenge.c` | Challenge mode timer and scoring |
| `challengeinit.c` | Challenge mode initialisation |
| `training.c` | Training mode logic |
| `trainingmenus.c` | Training mode menus |
| `race.c` | Animation-based velocity integration (sums root-motion angle per frame) |
| `timing.c` | Frame timing, diffframe counter, tick-rate management |
| `buildtime.c` | Returns the hard-coded ROM build-timestamp string for the active version |
| `vmstats.c` | N64 virtual-memory paging statistics debug overlay (4 MB expansion mode) |
| `crc.c` | CRC-16 checksum used for save-file integrity |
| `utils.c` | Geometry and math utility functions (coord transforms, trig, AABB helpers) |
| `rng2_c.c` | Secondary RNG (C portion; separate stream from lib/rng) |
| `debug1.c` | Debug HUD screen 1 (profiling / frame stats) |
| `debug2.c` | Debug HUD screen 2 (memory pool usage) |
| `debug3.c` | Debug HUD screen 3 (object lists / chr states) |
| `mpconfigs.c` | Preset multiplayer arena configurations (stage, scenario, weapon set, score limit) |
| `getitle.c` | In-cutscene character-name title card renderer |
| `collisionutils.c` | Game-level collision helpers built on top of `lib/collision` |
| `game_*.c` | Unnamed decompiled code units, identified by ROM offset only |

---

## src/game/mplayer/

| File | Purpose |
|---|---|
| `mplayer.c` | Multiplayer session init, player slots, round/match management, bot-body table |
| `ingame.c` | In-match multiplayer HUD overlay, scoreboard, end-of-round tick |
| `scenarios.c` | Scenario dispatcher and shared scenario-tick logic (includes net.h for KoH sync) |
| `setup.c` | Multiplayer lobby/configuration screen and bot-slot assignment |
| `mpaicommands.c` | AI script commands specific to multiplayer (objective interaction, scenario rules) |
| `mpstats.c` | Kill/death/score recording; server-only on netplay, broadcasts SVC_KILL/SVC_SCORE |

### src/game/mplayer/scenarios/

Each `.inc` is `#include`-d by `scenarios.c`; split into separate files to keep `scenarios.c` manageable.

| File | Scenario |
|---|---|
| `combat.inc` | Combat (deathmatch) — also holds "No Room Culling" / "No Draw Slot Limit" port options |
| `capturethecase.inc` | Capture the Case |
| `holdthebriefcase.inc` | Hold the Briefcase |
| `kingofthehill.inc` | King of the Hill — clients skip local hill selection; wait for `SVC_KOH_STATE` |
| `hackthatmac.inc` | Hacker Central (hack the MAC terminal) |
| `popacap.inc` | Pop a Cap |

---

## src/lib/

Original N64 library code, decompiled from the ROM. **Do not rename symbols** — they map to the binary.

| File | Purpose |
|---|---|
| `ailist.c` | AI bytecode list interpretation engine (runs `gailists.c` + per-stage AI scripts) |
| `anim.c` | Skeletal animation system (keyframe interpolation, root-motion extraction) |
| `args.c` | Boot argument parsing |
| `audiodma.c` | N64 audio DMA transfer management |
| `audiomgr.c` | N64 audio manager thread |
| `base.c` | Game base initialisation (sets up main game thread) |
| `boot.c` | N64 boot entry point and physical memory initialisation |
| `collision.c` | Low-level collision detection: cylinder and sphere vs. BSP portal geometry |
| `crash.c` | N64 crash dump (registers, stack trace) |
| `debughud.c` | Debug text overlay: `dhudSetPos`, `dhudPrintf`, colour and background helpers |
| `dma.c` | DMA block transfer helpers |
| `fault.c` | N64 hardware fault/exception handler |
| `joy.c` | N64 joystick/controller polling (SI interface) |
| `main.c` | N64 main game thread (top-level tick loop) |
| `mema.c` | Ad-hoc allocator: 300 KB pool, supports individual frees, resets per stage load |
| `memp.c` | Main pool allocator: onboard + Expansion Pak banks, pool/stage/permanent tiers |
| `model.c` | 3-D skeletal model: load, animation tick, RDP display-list generation |
| `modelasm_c.c` | C portion of model matrix computation (paired with `modelasm.s`) |
| `mp3.c` | Top-level MP3 audio-playback wrapper |
| `mtx.c` | N64 RSP-compatible 4×4 matrix operations |
| `mtx_c.c` | C fallback portion of matrix operations |
| `music.c` | N64 music sequence playback (naudio) |
| `path.c` | Navigation path-following helper (advance chr along a waypoint route) |
| `pimgr.c` | PI (cartridge parallel-interface) DMA manager thread |
| `profile.c` | RSP/RDP frame-time profiler |
| `rdp.c` | RDP task submission and display-list flushing |
| `reset.c` | N64 software-reset handler |
| `rmon.c` | Remote monitor / debugger communication via UART |
| `rng_c.c` | C portion of the random-number generator (paired with `rng.s`) |
| `rzip_c.c` | C portion of the RZIP (run-length + inflate) decompressor |
| `sched.c` | N64 multi-threaded scheduler: coordinates audio and graphics task threads |
| `snd.c` | Non-positional (in-head) sound-effect playback |
| `speaker.c` | Audio output speaker management |
| `str.c` | Custom string utilities and in-game proportional-font text rendering |
| `varsinit.c` | Static initial values for global variables |
| `vi.c` | N64 video interface: VI register control, mode setting |
| `videbug.c` | VI debug output (prints register state) |
| `vminit.c` | N64 virtual-memory (TLB) initialisation for 8 MB Expansion Pak |
| `lib_*.c` | Unnamed decompiled library units, identified by ROM offset only |

### src/lib/mp3/

N64 MP3 decoder implementation split across multiple unnamed segments plus:
- `decoder.c` — frame decode entry point
- `main.c` — MP3 subsystem init
- `util_c.c` — C utility helpers
- `lib_45ed0.c`, `lib_47d20.c`, `lib_47ef0.c` — unnamed decoder segments

### src/lib/naudio/

Nintendo Audio (naudio) library: synthesizer, envelope, sequencer, CSP (compressed sequence player), reverb, and audio-graph bus management. All files are original N64 library code.

### src/lib/ultra/audio/

libultra audio subsystem: bank file parser (`bnkf.c`), compressed sequence reader (`cseq.c`), sound list (`sl.c`), heap, pitch utilities.

### src/lib/ultra/gu/

libultra graphics utility: matrix builders (`frustum`, `ortho`, `perspective`, `rotate`, `scale`, `translate`, `lookat`), normalise, `sins`/`coss` lookup tables.

### src/lib/ultra/io/

libultra hardware I/O: PI/SI/VI/AI/SP/DP register drivers; controller, Controller Pak (PFS), EEPROM, and Game Boy Pak interfaces. One `.c` file per OS call.

### src/lib/ultra/libc/

libultra C library: `bcopy`/`bcmp`/`bzero` (MIPS assembly), `sprintf`, string utilities, 64-bit integer support (`ll`, `llcvt`), `xprintf` formatted output.

### src/lib/ultra/os/

libultra OS: threads, message queues, TLB mapping, exception/interrupt handlers, timer management, cache operations. One `.c`/`.s` file per OS call.

---

## port/src/

Port-layer files written for the modern platform target. May be freely modified.

| File | Purpose |
|---|---|
| `main.c` | Port entry point; initialises all subsystems in dependency order |
| `pdmain.c` | Game-side init (memory pools, stage selection) called from `main.c` after subsystems are up |
| `pdsched.c` | Per-frame game scheduler; drives `netStartFrame`/`netEndFrame` around the game tick |
| `video.c` | SDL2 window creation, OpenGL context, fast3d renderer integration |
| `input.c` | SDL2 keyboard/mouse/gamepad input, virtual-key bindings, controller remapping |
| `audio.c` | SDL2 audio backend: feeds N64 audio-manager output to the hardware audio stream |
| `mixer.c` | Software audio mixer replacing the N64 RSP audio path |
| `config.c` | INI config file (`pd.ini`) read/write, key registration |
| `console.c` | In-game developer console (`~`): command routing, `/`-prefixed net commands, 80-line scrollback ring |
| `crash.c` | Crash handler: DbgHelp symbol lookup on Windows with `addr2line` DWARF fallback for MinGW |
| `fs.c` | Filesystem abstraction: ROM asset paths, external data files, mod directory overlays |
| `system.c` | Platform logging (`sysPrintf`), command-line argument parsing, `sysFatalError` |
| `romdata.c` | ROM asset loader: reads N64 ROM, byteswaps big-endian data, calls preprocessors, handles mod overrides |
| `libultra.c` | Stub implementations of N64 OS/libultra functions not needed on the port (timers, TLB, etc.) |
| `mod.c` | Mod support: loads custom textures, animations, and sequences from disk |
| `mpsetups.c` | Multiplayer controller-pak setup file (saved loadouts) read/write to disk |
| `optionsmenu.c` | Port-specific "Extended Options" menu items (framerate cap, uncap tickrate, etc.) |
| `utils.c` | Port utility helpers: file I/O wrappers, path manipulation, string helpers |

### port/src/net/

| File | Purpose |
|---|---|
| `net.c` | ENet event loop, connection lifecycle, CSP reconcile/tick, lag-comp save/begin/end, sim-chr broadcast, fake-lag queue, diagnostic CSV log, F9 debug overlay |
| `netmsg.c` | All `SVC_*` and `CLC_*` message serialisation, deserialisation, and application to game state |
| `netbuf.c` | Typed byte-level read/write buffer used for all net messages |
| `netmenu.c` | Host/Join game menus, server favourites list, lobby-state display while joining |

### port/src/preprocess/

These run at load time to convert N64 big-endian binary assets into host-usable structures.

| File | Purpose |
|---|---|
| `common.c` | Shared N64→host pointer-fixup table used by all preprocessors |
| `filebg.c` | Background/room geometry: decompress, fix pointers, convert GBI display lists |
| `filelang.c` | Language/text data conversion |
| `filemodel.c` | 3-D model data: pointer fixup, vertex format conversion |
| `filepads.c` | Pad/waypoint data fixup |
| `filesetup.c` | Stage setup binary: decompress and fix all internal pointers |
| `filetiles.c` | 2-D tile (menu graphic) data conversion |
| `gbi.c` | GBI display-list pointer/address fixup (big-endian → host) |
| `misc.c` | Miscellaneous asset preprocessors: animations, sequences, textures |
| `segaudio.c` | Audio segment (sound bank / sequence) data fixup |
| `segfonts.c` | Font data conversion |

---

## port/include/

| File | Purpose |
|---|---|
| `input.h` | Input API and virtual-key enum (`VK_*`) — SDL scancode values |
| `mixer.h` | Software mixer API |
| `mod.h` | Mod loading API |
| `mpsetups.h` | Multiplayer setup file API |
| `preprocess.h` | Preprocessor entry-point declarations (one per asset type) |
| `preprocess/common.h` | Shared preprocessor types, byteswap helpers, pointer-fixup macros |
| `preprocess/gbi.h` | GBI preprocessor API |
| `preprocess/setup.h` | Stage setup preprocessor API |
| `romdata.h` | ROM asset loader API |
| `net/net.h` | Netplay core: `netclient`, `netplayermove`, CSP/lag-comp structs, extern globals, function decls |
| `net/netbuf.h` | `netbuf` struct and typed read/write API |
| `net/netenet.h` | ENet include wrapper (undefines `bool`, `near`, `far` before including enet.h) |
| `net/netmsg.h` | `SVC_*`/`CLC_*` message ID constants and all read/write function prototypes |
| `external/enet.h` | Bundled ENet reliable-UDP library header |
| `external/minimp3.h` | Bundled minimp3 MP3 decoder header |

---

## port/fast3d/

libultraship's fast3d renderer: interprets N64 GBI display lists and renders them on OpenGL.

| File | Purpose |
|---|---|
| `gfx_pc.h` | PC-side GBI display-list interpreter (core fast3d state machine) |
| `gfx_api.h` | Abstract graphics API interface bridging the interpreter to a backend |
| `gfx_rendering_api.h` | Rendering backend interface (texture upload, draw call, etc.) |
| `gfx_opengl.h` | OpenGL 3.0 rendering backend |
| `gfx_cc.h` | N64 RDP colour combiner state decoder |
| `gfx_sdl.h` | SDL2 window manager for fast3d |
| `gfx_window_manager_api.h` | Window manager interface |
| `gfx_screen_config.h` | Screen and framebuffer configuration |
| `glad/glad.c` | OpenGL function loader (generated by glad) |
| `glad/glad.h` | OpenGL function declarations |
| `glad/khrplatform.h` | Khronos platform type definitions |

---

## port/external/

| File | Purpose |
|---|---|
| `enet.c` | Bundled ENet reliable-UDP library source (used by netplay) |
| `minimp3.c` | Bundled minimp3 MP3 decoder source (used by audio system) |
