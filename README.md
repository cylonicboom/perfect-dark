# Perfect Dark port (`port-net`)

> ### Branch review aid — `port-net-predict` (Claude-touched files)
>
> This fork branch builds on `port-net` and adds entity interpolation, client-side
> prediction (CSP), lag compensation, sim chr sync, positional weapon sounds,
> server-authoritative kill/score feed, an outgoing-latency simulator, a
> diagnostic CSV log, `/lag` / `/loss` / `/diag` / `/igtick` console commands,
> client-reported hits (`CLC_HIT`), lobby state display, server favourites list,
> King of the Hill sync (with port-only Static Hill dropdown), explosion sync,
> Capture the Case per-team base pins, Hold the Briefcase / Hack that Mac
> static spawn pins, client respawn fix,
> no-room-culling and no-draw-slot-limit combat options,
> GoldenEye Style Combat Sim option (`MPOPTION_GOLDENEYE`) and matching always-
> unlocked cheat, Custom Weapon Presets (mpsetups.bin v2 with per-slot
> FNFLAG_* gates), Force Classic Crosshair / Hide Crosshair Unless Aiming
> per-player options, host spectator mode (`MPOPTION_HOSTSPECTATOR`, WIP),
> and various fixes. Full design notes, rationale, and known limitations
> are in [CLAUDE.md](CLAUDE.md) and the per-feature docs under [docs/](docs).
>
> **Added on this branch:**
> - `CLAUDE.md` — netplay design notes, branch state, environment + code-writing guidelines
> - `docs/PORT_GOLDENEYE.md` — full per-file rationale for the GoldenEye Style option + cheat (snap lean, no crouch accuracy bonus, lower-and-raise reloads, ledge wall, classic crosshair, hide-unless-aiming, GE arc health/shield HUD, no secondary functions, no mid-crouch, no dual-wield, no blur/dizzy, i-frames + damage flash + fire lockout, hidden function indicator)
> - `docs/PORT_KOH_STATIC_HILL.md` — per-file rationale for the KotH Static Hill picker + Mobile/Static mode dropdowns; mpsetup wad v3 encoding; `SVC_STAGE_START` payload extension
> - `docs/PORT_WEAPON_PRESETS.md` — per-file rationale for the Custom Weapon Presets manager; `mpsetups.bin` v2 tail section; `FNFLAG_*` per-slot bits; `bgunPrimary/SecondaryFunctionDisabled` hook surface
> - `docs/PORT_HOST_SPECTATOR.md` — per-file rationale for the host-spectator panel system (WIP/paused); architecture decisions + known breakage with remote combatants
> - `docs/PORTING_HOWTO.md` — methodology guide for porting any port-only feature into another fork / branch (guard patterns, MPOPTION budget, protocol version discipline, wad versioning, deterministic invariants, helper choke points)
> - `docs/PORT_CTC_STATIC_BASE.md` — per-file rationale + porting checklist for the CTC per-team base pin system (four "Team N Base" dropdowns, dynamic enable based on player count, `g_MpSetup.ctcteambase[4]`)
> - `docs/PORT_HTB_HTM_STATIC_SPAWN.md` — per-file rationale + porting checklist for HTB/HTM "Static Spawn" dropdowns (`g_MpSetup.htbstaticpad` / `htmstaticpad`, the 28-pad constant, the `htbCreateUplink`-is-HTM naming gotcha)
> - `port/include/spectator.h`, `port/src/spectator.c` — host-spectator panel state, six per-panel modes (PLAYER_FP/TP, SIM_FP/TP, FREECAM, TOPDOWN), freecam input incl. KBM mouse-look, per-mode pose, minimal world render
>
> **Modified — network core (`port/`):**
> - `port/include/net/net.h` — new structs (`csp_snapshot`, `lagcomp_snapshot`, `netkillfeedentry`, `netlobbystate`, `netlobbyclient`, `netlobbybot`), `inmove[]` ring buffer in `netclient`, CSP / lag-comp / console-cmd / kill-feed / lobby API, host-spectator sentinel `NET_PLAYERNUM_SPECTATOR` (0xFE) + per-client `is_spectator` byte, `NET_PROTOCOL_VER 29`
> - `port/include/net/netmsg.h` — `SVC_CHR_FIRE` / `SVC_KILL` / `SVC_SCORE` / `SVC_KOH_STATE` / `SVC_EXPLOSION` / `SVC_LOBBY_STATE` IDs; `CLC_HIT` ID; all read/write prototypes
> - `port/include/mpsetups.h` — `struct mpweaponpreset` (Custom Combat Sim weapon presets); `g_MpWeaponPresets[]` table; per-slot `g_MpSlotFnFlags[]` runtime gates; `mpWeaponPreset{Find,Add,Replace,Rename,Delete}` API
> - `port/src/net/net.c` — CSP reconcile + tick, lag-comp save / begin / end, sim-chr broadcast in `netEndFrame`, outgoing-latency queue, `/lag` / `/loss` / `/diag` / `/netinfo` / `/igtick` / tuning-knob console commands, diagnostic CSV log infra, expanded F9 debug overlay; `CLC_HIT` deferred-hit queue drained in `netEndFrame`; `SVC_KOH_STATE` keep-alive every 60 ticks; `SVC_LOBBY_STATE` broadcast every 60 ticks in lobby phase; syncid bump after initial allocation; spectate local-body hide/restore; host-spectator panel plumbing
> - `port/src/net/netmsg.c` — `inmove` ring-buffer push, chr-state block on `SVC_PROP_MOVE` (yrot + animation + held weapons + aim), projectile rotation auto-derive, bot configs synced in `SVC_STAGE_START`, `SVC_CHR_FIRE` / `SVC_KILL` / `SVC_SCORE` / `SVC_KOH_STATE` / `SVC_EXPLOSION` / `SVC_LOBBY_STATE` / `CLC_HIT` read/write; `SVC_PLAYER_MOVE` echoes client's own `CLC_MOVE` position back (prevents CSP snap at high latency); `SVC_PLAYER_STATS` no longer overwrites local player's ammo; per-client `is_spectator` flag appended to `SVC_STAGE_START` / `SVC_LOBBY_STATE`; `g_MpSetup.kohstatichill` appended to `SVC_STAGE_START` (server + client must agree before `kohInitProps` for RNG-seed parity); `g_MpSetup.ctcteambase[4]` + `htbstaticpad` + `htmstaticpad` appended to `SVC_STAGE_START` after `kohstatichill` for the CTC / HTB / HTM static-pin features (same RNG-parity requirement before `ctcInitProps` / `htbCreateToken` / `htbCreateUplink`); `MPOPTION_GOLDENEYE` rides the existing options field
> - `port/src/net/netmenu.c` — favourites list (8 saved server addresses with names, persisted in `pd.ini` as `Net.Favourites.N.Name/Addr`); `Net.Client.HideAddress` streamer-mode toggle hides IP from UI; lobby state display in the join screen (shows host's scenario, arena, score/time limits, player list, bots, weapon set while waiting); host-spectator lobby toggle + panel-count slider; "GoldenEye Style" / "Host Spectator" entries in the active-options summary line
> - `port/src/console.c` — route `/`-prefixed chat lines to `netConsoleCommand`
> - `port/src/crash.c` — `addr2line` fallback when DbgHelp lacks symbols on MinGW DWARF builds; added `crashFindAddr2Line` to search MSYS2 install paths when tool not in PATH; also looks next to the exe before falling back
> - `port/src/pdmain.c` — `netInit` placement, per-subsystem diag-log trail around stage init
> - `port/src/main.c` — config keys `Game.PlayerN.CrosshairForceClassic` / `Game.PlayerN.CrosshairHideUnlessAiming` registered for the new per-player crosshair options
> - `port/src/mpsetups.c` — `mpsetups.bin` bumped to v4: v2 appended Custom Weapon Presets section; v3 added the KoH `kohstatichill` 4-bit field per setup; v4 appended the CTC `ctcteambase[4]` (3 bits each), HTB `htbstaticpad` (6 bits) and HTM `htmstaticpad` (6 bits) per setup; `mpWeaponPreset{Find,Add,Replace,Rename,Delete}` impl + load/save
> - `port/src/optionsmenu.c` — Force Classic Crosshair / Hide Crosshair Unless Aiming checkboxes in Options > Extended Game
> - `port/src/input.c` — one-frame mask cooldown after `inputStopTextInput` so a held Enter (used to submit an OSK dialog) doesn't read as a fresh 0->1 edge and fire START_BUTTON into the underlying dialog (was triggering Combat Sim match-start immediately on preset save)
>
> **Modified — game logic (`src/game/`):**
> - `src/game/bondwalk.c` — `bwalkUpdateRemote` rewritten for 8-snapshot ring-buffer position interpolation; early return on server when `UCMD_FL_FORCEMASK` is set (prevents overwriting the authoritative spawn position with the client's stale death-point inmove on frames T+1+ after respawn); GE-mode snap lean (`bwalk0f0c69b8` per-frame cap raised so lean reaches near-target in ~3 frames); GE-mode invisible-wall ledges (`bwalkUpdateVertical` restores start-of-tick pose and nudges player back when drop > 60 units); GE-mode no mid-crouch (`bwalkAdjustCrouchPos` collapses DUCK landings to STAND/SQUAT)
> - `src/game/bondmove.c` — `bmoveProcessRemoteInput` uses ring buffer; speeds lerp, angles snap, animation snaps when server diverges; GE-mode no mid-crouch on all keyboard / toggle / hold crouch input paths; remote players' `UCMD_DUCK` collapsed to SQUAT for visual parity
> - `src/game/bondgun.c` — `bgunPlayGunSound` routes remote players' weapon sounds through `psCreate` (positional) instead of `sndStart`; guard `bgunStartAnimation` for remote players (crash on partially-synced weapon gset `fire_animation` pointer); GE-mode lower-and-raise reloads (skip first-person reload anim entirely); GE-mode skip crouch accuracy bonus in `bgunCalculatePlayerShotSpread`; GE-mode fire lockout during i-frames (helper `bgunCurrentPlayerInIframe`, gates at `bgunSetState` and `bgunTickInc`); GE-mode hide weapon-function indicator (red/yellow square + function-name text); reusable `bgunPrimaryFunctionDisabled` / `bgunSecondaryFunctionDisabled` / `bgunDualWieldDisabled` helpers gating equip / cycle / toggle paths; left-hand equip refusal + per-tick force-clear for no-dual-wield; cycle-forward/back fix that walks past DUAL inventory slots when left hand is force-disabled
> - `src/game/chraction.c` — `SVC_CHR_FIRE` broadcast at sim shot on/off transitions, sim action-tick skipped on client; `func0f0341dc` sends `CLC_HIT` to server on client instead of silently dropping; `chrDie` skips `botinvDropAll` on client to preserve weapon syncids for in-flight `SVC_PROP_MOVE`; GE-mode i-frame gate at top of `chrDamage` (`TICKS(18)` window using new `chr->lastdamagetick60` stamp, u32-wrap-safe); damage-application sites stamp `lastdamagetick60` (player + sim branches); local-player damage flash stamped via `currentplayer->damageflashstart60` with anti-stack guards; GE-mode `makedizzy` clamp disables weapon-induced screen blur; bot fire lockout in `chrTickShoot` during the bot's own i-frame window
> - `src/game/prop.c` — sims tick via `chrTick` (not `botTick`) on client, lag-compensation hooks in `shotCalculateHits`; server skips `chrHit` for remote-player shots (CLC_HIT path handles those to avoid double-damage)
> - `src/game/propobj.c` — tick remote-client projectiles on the server; `propExplode` broadcasts `SVC_EXPLOSION` for networked props
> - `src/game/player.c` — `inmove[0]` → `inmove[inmove_head]` fixes for respawn logic; `playerStartNewLife` skips `scenarioChooseSpawnLocation` on `NETMODE_CLIENT` — keeps current position so the server's force-correction `SVC_PLAYER_MOVE` is the sole authority on spawn placement (without this, a client-chosen local spawn diverged from the server's and the echo-back CSP path never corrected it, causing clients to respawn at or near the death point); GE-mode HUD `playerRenderHealthBarGE` (two 8-segment half-circle arcs — yellow→red health left, light-cyan→dark-blue shield right — vertically centred, vanilla shield bar suppressed); full-screen white damage flash with triangular 8-frame alpha curve appended; `playerStartNewLife` also resets `damageflashstart60` and `lastdamagetick60` on respawn
> - `src/game/playermgr.c` — `damageflashstart60` initialised to `-1000000` in `playermgrAllocatePlayer` (sentinel so the flash never fires before the first real damage event)
> - `src/game/sight.c` — GE-mode and per-player Force Classic Crosshair forces `SIGHT_CLASSIC` on every weapon (incl. zoom weapons, trade-off: no zoom-corner brackets / sniper fullscreen scope); GE-mode and per-player Hide Crosshair Unless Aiming early-returns from `sightDraw` when `!sighton`
> - `src/game/menuitem.c` — keyboard-tick OSK accept path stops text input + clears the pending key + early-returns so a single Enter press doesn't run the accept flow twice and accidentally fire START_BUTTON into the underlying menu
> - `src/game/menutick.c` — preserve bot slot bits (`0xff00`) when net server returns from match to lobby; was resetting `chrslots = 1` and clearing all simulants
> - `src/game/mplayer/mplayer.c` — dedicated RNG seed for `mpChooseTrack` to keep music in sync; `g_BotBodies[]` table for random body selection in `mpCreateBotFromProfile`; `PLAYER_EXT_CFG_DEFAULT` adds `crosshairforceclassic` / `crosshairhideunlessaiming` defaults; `g_MpSlotFnFlags[]` definition + per-`mpApplyWeaponSet` reset; Random Preset rotation extended with saved Custom presets (`g_MpWeaponPresets[]`); v3 mpsetup wad serializes `g_MpSetup.kohstatichill`; v4 mpsetup wad additionally serializes `g_MpSetup.ctcteambase[4]`, `htbstaticpad`, `htmstaticpad` (with default-to-zero fallback for older files)
> - `src/game/mplayer/setup.c` — Saved Custom Weapon Presets manager (`g_MpCustomPresetsMenuDialog` + edit / save-name / rename / overwrite / delete / saved / maxed dialogs); 6 weapon-slot dropdowns + 6 cycling fn-mode selectables; Set=Custom dropdown auto-opens the manager
> - `src/game/mplayer/scenarios.c` — includes `net.h`/`netmsg.h` for KoH sync
> - `src/game/mplayer/scenarios/combat.inc` — "No Room Culling", "No Draw Slot Limit", and "GoldenEye Style" checkboxes (port-only) added to Combat Options menu
> - `src/game/mplayer/scenarios/kingofthehill.inc` — clients skip RNG hill selection and wait for `SVC_KOH_STATE` (prevents divergence); server broadcasts new hill state immediately on change; port-only "Hill Mode" (Mobile/Static) dropdown + "Static Hill" dropdown (per-stage hill count via `kohGetStageHillCount`); "No Room Culling" / "No Draw Slot Limit" / "GoldenEye Style" mirrored into KoH menu
> - `src/game/mplayer/scenarios/capturethecase.inc` — port-only `menuhandlerMpCtcTeamBase` + four "Team N Base" dropdowns (dynamically greyed when the team has no players in the lobby); `ctcInitProps` team-assignment loop honours `g_MpSetup.ctcteambase[i]` before falling through to `rngRandom() % 4`; `!teamsdone[…]` check resolves "two teams pinned to the same base" ties (first wins)
> - `src/game/mplayer/scenarios/holdthebriefcase.inc` — port-only `menuhandlerMpHtbStaticPad` + "Static Spawn" dropdown (Random + Pad 1..28, where 28 is the empirically-stable max of `INTROCMD_CASE` + `INTROCMD_CASERESPAWN` entries across all CTC-supported MP stages); `htbCreateToken` bypasses the ammocrate-replacement scan AND the `rngRandom() % nextindex` fallback when `g_MpSetup.htbstaticpad` is set and in range
> - `src/game/mplayer/scenarios/hackthatmac.inc` — port-only `menuhandlerMpHtmStaticPad` + "Static Spawn" dropdown (same 28-pad range as HTB); `htbCreateUplink` (decompile naming quirk — the function actually creates the HTM uplink) bypasses the ammocrate scan + fallback when `htmstaticpad` is set. Terminal placement in `htmInitProps` is intentionally NOT pinned — only the data uplink is
> - `src/include/constants.h` — `MPOPTION_NOCULL`, `MPOPTION_NOOMLIMIT`, `MPOPTION_HOSTSPECTATOR`, `MPOPTION_GOLDENEYE`, `CHEAT_NOCULL`, `CHEAT_NODRAWLIMIT`, `CHEAT_GOLDENEYE`, `CHEATFLAG_ALWAYSUNLOCKED`, `MPWEAPONPRESET_MAXNAME`, `MPWEAPONPRESET_MAXENTRIES`, `FNFLAG_PRIMARY_DISABLED`, `FNFLAG_SECONDARY_DISABLED` added (upper-byte MPOPTION space is now fully allocated)
> - `src/include/types.h` — port-only `chrdata.lastdamagetick60` and `player.damageflashstart60` fields for the GE i-frame / damage-flash window; `extplayerconfig.crosshairforceclassic` / `crosshairhideunlessaiming` fields; weapon preset structs referenced by `mpsetups.h`; `mpsetup.kohstatichill` / `ctcteambase[4]` / `htbstaticpad` / `htmstaticpad` fields for the per-scenario static-spawn pin features
> - `src/include/game/bondgun.h` — port-only `bgunPrimaryFunctionDisabled` / `bgunSecondaryFunctionDisabled` prototypes
> - `src/include/game/cheats.h` — port-only `goldeneyeStyleActive()` prototype (the single helper both GE activation routes route through)
> - `src/game/bg.c` — `g_BgDrawSlots` expanded to 256 on port (sentinel at [255]); draw-slot cap raised to 254 when `g_BgNoDrawSlotLimit`; portal bypass via `g_BgNoCull`; both flags computed each frame from cheats + MP options
> - `src/game/cheats.c` — `CHEAT_NOCULL` (unlocks all content on activation + disables portal culling), `CHEAT_NODRAWLIMIT` (removes 60-room draw cap), and `CHEAT_GOLDENEYE` (port-only GE Style rule set, also enables `goldeneyeStyleActive()` outside of Combat Sim) added as always-unlocked port-only cheats in the Gameplay cheats menu; `goldeneyeStyleActive()` helper is the single choke point for the cheat + `MPOPTION_GOLDENEYE` gates
> - `src/game/chr.c` — GE-mode poison-blur accumulation skipped; `chrInit` resets `lastdamagetick60` to 0 so a recycled chrslot can't grant the freshly-spawned chr permanent invulnerability
> - `src/game/bot.c` — GE-mode post-tick clamp force-disables `cloakdeviceenabled` / `rcp120cloakenabled` (bots can't go invisible in GE mode); also force-zeroes `blurdrugamount` / `blurnumtimesdied` so flipping the cheat mid-match instantly wipes lingering dizziness; `botReset` respawn block clears `lastdamagetick60`
> - `src/game/botinv.c` — GE-mode + Custom-preset `funcnum` clamp in `botinvSwitchToWeapon` (single choke point for every bot weapon-equip path; AI's per-weapon scoring still considers secondaries but the final commit is forced to primary)
> - `src/game/mpstats.c` — gate `mpchrconfig` stat writes to server only, broadcast `SVC_KILL` / `SVC_SCORE`
>
> Inline comments throughout these files explain the WHY — constraints, tradeoffs,
> past failures — not the WHAT. Where an approach was tried and reverted (e.g. CSP
> history-shift, full-array bone-matrix lag-comp), the comment records why.


## Experimental netplay branch

This branch of the port contains an **extremely** early and experimental implementation of network play.  
**DISCLAIMER:** This is **NOT READY** for prime time. Use at your own risk if you are not a developer.  
**Do not create issues about problems in this branch until this disclaimer is gone.**

See [this file](https://github.com/fgsfdsfgs/perfect_dark/blob/port-net/docs/netplay.md) for more information on how this works.

## Original description

This repository contains a work-in-progress port of the [Perfect Dark decompilation](https://github.com/n64decomp/perfect_dark) to modern platforms.

To run the port, you must already have a Perfect Dark ROM, specifically one of the following:
* `ntsc-final`/`US V1.1`/`US Rev 1` (md5 `e03b088b6ac9e0080440efed07c1e40f`).  
  **This is the recommended version to use**.  
  Called `NTSC version 8.7 final` on the boot screen.
* `ntsc-1.0`/`US V1.0` (md5 `7f4171b0c8d17815be37913f535e4e93`).  
  Technically supported, but not recommended.  
  Called `NTSC version 8.7 final` on the boot screen as well.
* `jpn-final` (md5 `538d2b75945eae069b29c46193e74790`).  
  Technically supported, but requires a separate custom-built executable.  
  Called `JPN version 8.9 final` on the boot screen.
* `pal-final` (md5 `d9b5cd305d228424891ce38e71bc9213`).  
  Technically supported, but requires a separate custom-built executable.  
  Called `PAL 8.7 final` on the boot screen.

## Status

The game is in a mostly functional state, with both singleplayer and split-screen multiplayer modes fully working.  
There are minor graphics- and gameplay-related issues, and possibly occasional crashes.

**The following extra features are implemented:**
* mouselook;
* dual analog controller support;
* widescreen resolution support;
* configurable field of view;
* 60 FPS support, including fixes for some framerate-related issues;
* fixes for a couple original bugs and crashes;
* basic mod support, currently enough to load a few custom levels;
* slightly expanded memory heap size;
* experimental high framerate support (up to 240 FPS):
  * enable `Uncap Tickrate` in `Extended Video Options` to activate;
  * in practice the game will have issues running faster than ~165 FPS, so use VSync or `Video.FramerateLimit` to cap it.
* emulate the Transfer Pak functionality the game has on the Nintendo 64 to unlock some cheats automatically.

**The following platforms are officially supported and tested:**
* Windows 7+: i686, x86_64
* Linux: i686, x86_64
* MacOS: x86_64 (OS 10.9+), arm64 (OS 11.0+)
* Nintendo Switch: arm64

## Download

Latest [automatic builds](https://github.com/fgsfdsfgs/perfect_dark/actions) of the netplay branch for supported platforms:
* [x86_64-windows (`port-net`)](https://nightly.link/fgsfdsfgs/perfect_dark/workflows/c-cpp/port-net/pd-x86_64-windows.zip)
* [i686-windows (`port-net`)](https://nightly.link/fgsfdsfgs/perfect_dark/workflows/c-cpp/port-net/pd-i686-windows.zip)
* [x86_64-linux (`port-net`)](https://nightly.link/fgsfdsfgs/perfect_dark/workflows/c-cpp/port-net/pd-x86_64-linux.zip)
* [i686-linux (`port-net`)](https://nightly.link/fgsfdsfgs/perfect_dark/workflows/c-cpp/port-net/pd-i686-linux.zip)

If you are looking for regular builds (the `port` branch), see [this link](https://github.com/fgsfdsfgs/perfect_dark/blob/port/README.md#download).

## Running

You must already have a Perfect Dark ROM to run the game, as specified above.  

This assumes that you're using an x86_64 build. If you aren't, replace `x86_64` below with your arch (e.g. `i686`).

1. Create a directory named `data` next to `pd.x86_64` if it's not there.
2. Put your Perfect Dark NTSC ROM named `pd.ntsc-final.z64` into it.
3. Run the `pd.x86_64` executable.

If you want to use a PAL or JPN ROM instead, put them into the `data` directory and run the appropriate executable:
* PAL: ROM name `pd.pal-final.z64`, executable name `pd.pal.x86_64`.
* JPN: ROM name `pd.jpn-final.z64`, executable name `pd.jpn.x86_64`.

Optionally, you can also put your Perfect Dark for GameBoy Color ROM named `pd.gbc` in the `data` directory if you want to emulate having the Nintendo 64's Transfer Pak and unlock some cheats automatically.

Optionally, you can move the data folder to `~/.local/share/perfectdark` on Linux or `~/Library/Application Support/perfectdark` on MacOS.

Note that users with different ROMs can't play netgames with each other.

Additional information can be found in the [wiki](https://github.com/fgsfdsfgs/perfect_dark/wiki).

A GPU supporting OpenGL 3.0/ES3.0 or above is required to run the port.

### Installing the Nintendo Switch version

The Nintendo Switch build ZIP comes with all 3 regions in different folders: `perfectdark`, `perfectdark_pal` and `perfectdark_jpn`.

Take the folder for the region you want and put it into the `/switch` folder on your SD card, then put your ROM into the `data` folder inside of the folder you extracted as described above.

`port-net` currently does not work on the Switch.

## Controls

1964GEPD-style and Xbox-style bindings are implemented.

N64 pad buttons X and Y (or `X_BUTTON`, `Y_BUTTON` in the code) refer to the reserved buttons `0x40` and `0x80`, which are also leveraged by 1964GEPD.

Support for one controller, two-stick configurations are enabled for 1.2.

Note that the mouse only controls player 1.

Controls can be rebound in `pd.ini`. Default control scheme is as follows:

| Action           | Keyboard and mouse     | Xbox pad                 | N64 pad                   |
| -                | -                      | -                        | -                         |
| Fire / Accept    | LMB/Space              | RT                       | Z Trigger                 |
| Aim mode         | RMB/Z                  | LT                       | R Trigger                 |
| Use / Cancel     | E                      | N/A                      | B                         |
| Use / Accept     | N/A                    | A                        | A                         |
| Crouch cycle     | N/A                    | L3                       | `0x80000000` (Extra)      |
| Half-Crouch      | Shift                  | N/A                      | `0x40000000` (Extra)      |
| Full-Crouch      | Control                | N/A                      | `0x20000000` (Extra)      |
| Reload           | R                      | X                        | X `(0x40)`                |
| Previous weapon  | Mousewheel forward     | B                        | D-Left                    |
| Next weapon      | Mousewheel back        | Y                        | Y `(0x80)`                |
| Radial menu      | Q                      | LB                       | D-Down                    |
| Alt fire mode    | F                      | RB                       | L Trigger                 |
| Alt-fire oneshot | `F + LMB` or `E + LMB` | `A + RT` or  `RB + RT`   | `A + Z`     or `L + Z`    |
| Quick-detonate   | `E + Q`   or `E + R`   | `A + B`  or  `A + X`     | `A + D-Left`or `A + X`    |

## Building

### Windows

1. Install [MSYS2](https://www.msys2.org).
2. Open the `MINGW64` prompt if building for x86_64, or the `MINGW32` prompt if building for i686. (**NOTE:** _do not_ use the `MSYS` prompt)
3. Install dependencies:  
   `pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-SDL2 mingw-w64-x86_64-zlib mingw-w64-x86_64-cmake mingw-w64-x86_64-python3 mingw-w64-i686-toolchain mingw-w64-i686-SDL2 mingw-w64-i686-zlib mingw-w64-i686-cmake mingw-w64-i686-python3 make git`
4. Get the source code:  
   `git clone -b port-net --recursive https://github.com/fgsfdsfgs/perfect_dark.git && cd perfect_dark`
5. Run `cmake -G"Unix Makefiles" -Bbuild .`.
   * Add ` -DROMID=pal-final` or ` -DROMID=jpn-final` at the end of the command if you want to build a PAL or JPN executable respectively.\
6. Run `cmake --build build -j4 -- -O`.
7. The resulting executable will be at `build/pd.x86_64.exe` (or at `build/pd.i686.exe` if building for i686).
8. If you don't know where you downloaded the source to, you can run `explorer .` to open the current directory.

### Linux

1. Ensure you have gcc, g++ (version 10.0+), make, cmake, git, python3 and SDL2 (version 2.0.12+), libGL and ZLib installed on your system.
   * If you wish to crosscompile, you will also need to have libraries and compilers for the target platform installed, e.g. `gcc-multilib` and `g++-multilib` for x86_64 -> i686 crosscompilation.
2. Get the source code:  
   `git clone -b port-net --recursive https://github.com/fgsfdsfgs/perfect_dark.git && cd perfect_dark`
3. Run the following command:
   * ```cmake -G"Unix Makefiles" -Bbuild .```
   * Add ` -DROMID=pal-final` or ` -DROMID=jpn-final` at the end of the command if you want to build a PAL or JPN executable respectively.
   * Add ` -DCMAKE_C_FLAGS=-m32 -DCMAKE_CXX_FLAGS=-m32` at the end of the command if you want to crosscompile from x86_64 to x86.
4. Run `cmake --build build -j4`.
5. The resulting executable will be at `build/pd.<arch>` (for example `build/pd.x86_64`).

### MacOS

1. Set up Homebrew.
2. Install dependencies:
   * Execute command: `brew install cmake gcc python3 zlib git`
3. Install SDL2:
   * Execute commands:
     ```
     wget http://libsdl.org/release/SDL2-2.30.9.dmg -O SDL2.dmg
     hdiutil mount SDL2.dmg
     sudo cp -vr /Volumes/SDL2/SDL2.framework /Library/Frameworks
     hdiutil detach /Volumes/SDL2
     ```
   * This installs SDL2 system-wide and this is how the automatic builds are done. The game will also look for it in the executable path, so you could
     download it locally instead.
4. Get the source code:  
   `git clone --recursive https://github.com/fgsfdsfgs/perfect_dark.git && cd perfect_dark`
5. Configure:
   * Execute command: `cmake -G"Unix Makefiles" -Bbuild -DCMAKE_OSX_ARCHITECTURES=x86_64 .`
   * Replace `x86_64` with `arm64` if building for an ARM64 Mac.
   * Add ` -DROMID=pal-final` or ` -DROMID=jpn-final` at the end of the command if you want to build a PAL or JPN executable respectively.
6. Build:
   * Execute command: `cmake --build build --target pd -j4 --clean-first`
7. The resulting executable will be at `build/pd.<arch>` (for example `build/pd.x86_64`).
   * You might need to execute `chmod +x build/pd.x86-64` before you can run it.

### Nintendo Switch

1. Set up the [devkitA64 environment](https://devkitpro.org/wiki/Getting_Started).
   * On Windows you can do it under MSYS2 or WSL, usually MSYS2 is recommended.
   * If using MSYS2, make sure to use the **MSYS2** shell, **not** MINGW32 or MINGW64.
2. Install host dependencies:
   * On MSYS2: execute command `pacman -Syuu && pacman -S git make cmake python3`
   * On Linux: use your package manager as normal to install the above dependencies.
3. Install Switch toolchain and dependencies:
   * Execute commands:
     ```
     dkp-pacman -Syuu
     dkp-pacman -S devkitA64 libnx switch-zlib switch-sdl2 switch-cmake dkp-toolchain-vars
     ```
   * If in MSYS2 or `dkp-pacman` doesn't work, replace it with just `pacman`.
4. Get the source code:  
   `git clone --recursive https://github.com/fgsfdsfgs/perfect_dark.git && cd perfect_dark`
5. Ensure devkitA64 environment variables are set:
   * Execute command: `source /opt/devkitpro/switchvars.sh`
   * If your `$DEVKITPRO` path is different, substitute that instead or set the variables manually.
6. Configure:
   * Execute command: `aarch64-none-elf-cmake -G"Unix Makefiles" -Bbuild .`
   * Add ` -DROMID=pal-final` or ` -DROMID=jpn-final` at the end of the command if you want to build a PAL or JPN executable respectively.
7. Build:
   * Execute command: `make -C build -j4`
8. The resulting executable will be at `build/pd.arm64.nro`.

### Notes

Alternate compilers or toolchains can be specified by passing `-DCMAKE_TOOLCHAIN_FILE=whatever` as normal. The port does not build with Visual Studio.

You will need to provide a `jpn-final` or `pal-final` ROM to run executables built for those regions, named `pd.jpn-final.z64` or `pd.pal-final.z64`.

It might be possible to build and run the game on platforms that are not specified in the supported platforms list (e.g. Linux on armv7), but this has not been tested.

## Credits

* the original [decompilation project](https://github.com/n64decomp/perfect_dark) authors;
* Ryan Dwyer for the above, additional help, and `pd-extract`;
* doomhack for the only other publicly available [PD porting effort](https://github.com/doomhack/perfect_dark) I could find;
* [sm64-port](https://github.com/sm64-port/sm64-port) authors for the audio mixer and some other changes;
* [Ship of Harkinian team](https://github.com/Kenix3/libultraship/tree/main/src/graphic/Fast3D), Emill and MaikelChan for the libultraship version of fast3d that this port uses;
* lieff for [minimp3](https://github.com/lieff/minimp3);
* Mouse Injector and 1964GEPD authors for some of the 60FPS- and mouselook-related fixes;
* Raf for the 64-bit port;
* NicNamSam for the icon;
* everyone who has submitted pull requests and issues to this repository and tested the port;
* probably more I'm forgetting.
