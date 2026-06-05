# SDL3 Extras: Gamepad LED Colours, Trigger Rumble, Taskbar Progress

Three small port-only quality-of-life features enabled by the SDL3 migration.
All are local-machine cosmetic/haptic — **no netplay wire impact, no protocol
version bump, no save-format impact**. The dedicated server build is
unaffected (input.c and gfx_sdl.cpp are excluded there; the one shared
touchpoint, `videoSetTaskbarProgress`, no-ops when video never initialised).

## 1. Gamepad LED player colours (`port/src/input.c`)

Pads with an RGB LED (DualShock 4 / DualSense lightbar) are tinted with a
per-player colour when assigned, PlayStation convention:

| Player | Colour |
|---|---|
| 1 | blue |
| 2 | red |
| 3 | green |
| 4 | pink |

While that player is **alive on low health** (`bondhealth < 0.25`) the LED
flashes red at 2Hz. Back above the threshold (or dead/respawning) it returns
to the base colour.

- Capability-gated at assign time via `SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN`
  (`padsCfg[].hasLED`); pads without an RGB LED are never touched.
- Driven by `inputUpdatePadLEDs()` from `inputUpdate()`: polled at 10Hz
  (`LED_UPDATE_INTERVAL_US`), and `SDL_SetGamepadLED` is only called when the
  colour actually changes (`padLEDState[]` cache) so we don't spam HID output
  reports every frame.
- Reads `g_Vars.players[cidx]` directly from input.c — same precedent as the
  `MPOPTION_CONTROLLERS_ONLY` gate in the same file. Local player *i* is
  assumed to drive pad *i*.
- Config: `Input.GamepadLED` (default `1`). Set `0` to never touch the LED
  (SDL/driver default behaviour is preserved).

## 2. Trigger rumble (`port/src/input.c`)

`inputRumble()` (the single choke point all game rumble goes through — weapon
fire, damage, N64 rumble-pak emulation) now mirrors the body rumble onto the
impulse triggers on pads that have them (Xbox One / Series controllers), at
the same strength and duration via `SDL_RumbleGamepadTriggers`.

- Capability-gated via `SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN`
  (`padsCfg[].hasTrigRumble`).
- Strength inherits the existing `RumbleScale` scaling.
- Config: `Input.PlayerN.TriggerRumble` (default `1`), per player.

## 3. Taskbar loading progress (`gfx_sdl.cpp` / `video.c` / `romdata.c`)

The boot-time asset preprocessing (animations, textures list, audio banks —
the multi-second part of startup) reports per-segment progress to the Windows
taskbar button (green fill) via SDL3's `SDL_SetWindowProgressState` /
`SDL_SetWindowProgressValue`.

- New `GfxWindowManagerAPI` entry `set_taskbar_progress(int state, float
  value)` (appended last in the struct; states 0 = none, 1 = indeterminate,
  2 = normal+value). Game-facing wrapper: `videoSetTaskbarProgress(s32, f32)`
  with the `VIDEO_TASKBAR_*` enum in `port/include/video.h`; guarded on
  `initDone`/`wmAPI` so headless dedicated is a no-op.
- Hooked in `romdataInit()` (`port/src/romdata.c`): fraction = segments
  initialised / total, cleared (`VIDEO_TASKBAR_NONE`) once the file table is
  loaded.
- **SDL version gate**: the progress API landed in SDL 3.4.0; the backend
  implementation is wrapped in `#if SDL_VERSION_ATLEAST(3, 4, 0)` and
  compiles to a no-op on older SDL3. Don't drop the guard.
- PC stage loads are sub-second, so per-stage progress is intentionally not
  reported — `videoSetTaskbarProgress` is public if a future long operation
  (e.g. shader cache warmup) wants it.

## Testing / troubleshooting — `/padtest` console command

`/padtest` (hidden debug command, routed through `netConsoleCommand` like
`/wireframe`; body is `inputPadTest` in input.c, no-op stub in
dedicated_stubs.c):

| Subcommand | Effect |
|---|---|
| `/padtest caps` | Logs each connected pad's name + `rumble/trig/rgbled` capability properties + wireless flag. A pad showing as "Steam Virtual Gamepad" means Steam Input has intercepted it (caps will be Xbox-shaped: no LED). |
| `/padtest led R G B` | Sets pad 1's LED directly, bypassing the health logic; the LED tick is held off for 5s so the test colour sticks. Isolates SDL/driver vs game-state issues. |
| `/padtest rumble S MS` | Raw body rumble (S = 0..65535), bypassing `RumbleScale`. `/padtest rumble 65535 2000` = max. If max raw rumble is weak, it's the SDL3 driver / connection, not our scaling. |
| `/padtest trig S MS` | Raw trigger rumble. Expected to fail ("That operation is not supported") on non-Xbox pads. |
| `/padtest hp` | Logs the `bondhealth` value the LED flash logic reads, plus `isdead` and the flash threshold. |

`inputInitController` also logs the caps line (`input: pad N caps: ...`) and
any `SDL_SetGamepadLED` failure at assign time.

Known behaviour notes:

- **Low-health flash needs `bondhealth < 0.25` while alive** — shields absorb
  first, and an instant death skips the band entirely, so in fast Combat Sim
  matches you may never see it. Verify with `/padtest hp` while damaged.
- **DualSense rumble is weaker under SDL3 than SDL2** on some firmware: SDL3
  drives the DualSense's haptic actuators' rumble emulation. USB vs Bluetooth
  changes intensity; `SDL_HINT_JOYSTICK_ENHANCED_REPORTS` is set to "1" by
  `inputInit` (successor of SDL2's `_PS4_RUMBLE`/`_PS5_RUMBLE` hints).
- **LED**: connect a DualShock 4 / DualSense → lightbar goes blue on assign
  (player 1). `Input.GamepadLED=0` in pd.ini disables (and restarting then
  shows the driver-default colour — useful to confirm our set is what's
  painting it).
- **Trigger rumble**: Xbox One/Series pad → fire a weapon; triggers buzz
  along with the body rumble. `Input.Player1.TriggerRumble=0` disables.
- **Taskbar**: watch the taskbar button fill green during game boot
  (most visible on a cold start with a big ROM / first-run preprocessing).
