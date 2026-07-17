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

## Gyro aim (`port/src/input.c`) — MVP

Motion aiming from pad 1's gyroscope (DualSense / DualShock 4 / Switch Pro),
for player 1. **Default off** — enable with `/gyro on` or `Input.GyroAim=1`.

How it works:

- `SDL_SetGamepadSensorEnabled(.., SDL_SENSOR_GYRO, ..)` at assign (or on
  `/gyro on`); the **event watcher** integrates each
  `SDL_EVENT_GAMEPAD_SENSOR_UPDATE`'s angular velocity (rad/s) against the
  sensor's own `sensor_timestamp` deltas — frame-rate-independent, no
  sample-and-hold error (DualSense delivers ~250Hz). Gaps > 0.5s are
  discarded (`GYRO_MAX_EVENT_DT`).
- `inputUpdateGyro()` converts the integrated radians to a per-frame delta in
  mouse-delta units, applying `Input.GyroSpeedX/Y` at conversion time so live
  sens changes apply immediately. The conversion (`GYRO_UNIT_SCALE`) is
  **empirically calibrated so sens 1.0 = 1:1** between physical pad rotation
  and camera rotation (the raw rad→deg value overshoots ~3.3× through the
  mouse-delta consumer scaling — measured on a DualSense). Default 1.0;
  negative inverts an axis.
- The delta is merged in `inputMouseGetScaledDelta` — one choke point feeding
  bondmove freelook, aim-mode crosshair, eyespy, possess and spectator — so
  gyro works everywhere mouse-look works. It rides the same `mouseLocked`
  gameplay gate (no drift in menus/console) but is **deliberately not
  suppressed by `MPOPTION_CONTROLLERS_ONLY`** (gyro is controller input).
  The `Abs` variant (active-menu selection) deliberately gets no gyro.
- `/gyro` console command: toggle / `on` / `off` / `sens X [Y]` / `status`
  (status prints capability + sensor data rate).
- **Menu**: Extended → Controller → Player 1, below Vibration behind a
  separator rule — "Gyro Aim" checkbox + "Gyro Speed X/Y" sliders (0..4.00,
  ×100 scale like the mouse-speed sliders). The whole section (rule included)
  hides via `MENUOP_CHECKHIDDEN` when `inputGyroSupported(player)` is false —
  i.e. on players 2-4 and on pads without a gyro — mirroring how Vibration
  hides without rumble.
- Config: `Input.GyroAim` (0/1, default 0), `Input.GyroSpeedX/Y`
  (-30..30, default 1).

Known MVP limits (build-out candidates): pad 1/player 1 only; no ratchet
mode (e.g. gyro-only-while-aiming or touchpad-held-to-ratchet); no drift
calibration (SDL's DualSense bias handling is decent; `/gyro status` +
feel-testing will tell); gyro pauses whenever the mouse-lock gate is off,
which also means it needs `Input.MouseEnabled=1` for the lock to engage;
axis inversion is config/console-only (negative speed), the menu sliders
clamp at 0.

## HiDPI pass (`gfx_sdl.cpp` / `video.c` / `input.c` / `optionsmenu.c`)

**Verified on Windows at 4K; the macOS Retina / Wayland items (native-pixel
rendering, mouse points→pixels) are implemented per SDL3 semantics but need
community verification on those platforms.**

- `Video.AllowHiDpi` now **defaults to 1**: the window gets
  `SDL_WINDOW_HIGH_PIXEL_DENSITY`, rendering at native pixel density on
  macOS Retina / Wayland. No effect on Windows (windows there are always
  pixel-sized; SDL3 is DPI-aware out of the box, which already killed the
  SDL2-era blurry-upscale problem). Menu: Extended → Video → "HiDPI
  (restart)" checkbox (`videoGet/SetAllowHiDpi`; window-creation flag, needs
  restart).
- **Windows scaled-desktop window sizing** (`gfx_sdl_init`,
  `#ifdef PLATFORM_WIN32`): windowed boots multiply the configured size by
  `SDL_GetDisplayContentScale`, so the default window isn't physically tiny
  at 125%/150%/4K scaling. Fullscreen boots are exempt — mode selection keeps
  using the configured pixel size. No config feedback loop: the scale is
  applied at window creation only and never written back to
  `Video.DefaultWidth/Height`.
- **Mouse points→pixels mapping** (`inputUpdateMouse`): absolute cursor
  coords are multiplied by `SDL_GetWindowPixelDensity` so the menu-cursor
  mapping against `videoGetWidth()` (pixels) stays 1:1 on macOS/Wayland
  HiDPI windows. Density is 1.0 on Windows — a no-op there. Relative aim
  deltas are deliberately untouched (sensitivity preference, not geometry).
- `SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED` joins the resize cases in
  `gfx_sdl_handle_events` (drag between monitors with different scaling).

## Refresh-rate picker (`gfx_sdl.cpp` / `video.c` / `optionsmenu.c`)

Extended → Video gains a **"Refresh Rate"** dropdown under Resolution: "Auto"
plus the distinct rates the current display offers for the selected
resolution (SDL3 display modes carry float Hz; near-duplicates within
0.05Hz, e.g. 59.94 vs 59.95, are collapsed; shown as `%g Hz`).

- Only meaningful for **exclusive** fullscreen — the dropdown greys out
  (`MENUOP_CHECKDISABLED`) while Full Screen Mode is Borderless, same pattern
  as the Resolution dropdown.
- Two new `GfxWindowManagerAPI` entries (appended): `get_refresh_rates(w, h,
  out, max)` and `set_refresh_rate(hz)`. The desired rate feeds the
  `SDL_GetClosestFullscreenDisplayMode` calls in `apply_fullscreen_mode` and
  `set_closest_resolution` (0 = auto); setting it while already in exclusive
  fullscreen re-picks the mode immediately.
- Config: `Video.RefreshRate` (float Hz, 0 = auto), applied at `videoInit`.
- If the saved rate isn't available at a newly selected resolution, SDL's
  closest-mode matching degrades gracefully (nearest rate wins).

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

## Mouse grab / window-active mouse lock (`port/src/input.c`, 2026-07-18)

`Input.MouseGrab` (default `1`): confines the OS cursor to the game window
while it has focus, via `SDL_SetWindowMouseGrab`. This is the free-cursor
half of "mouse lock when the window is active" — relative mouse mode
(`Input.MouseLockMode`) already captures the cursor during gameplay, but in
menus the cursor is free and could wander onto a second monitor where a
click deactivates the game.

- Applied at `inputInit` (videoInit runs just before, so the window exists)
  and re-asserted on every `SDL_EVENT_WINDOW_FOCUS_GAINED` in the input
  event watcher, which also re-asserts relative capture if the game held
  the mouse when focus was lost. SDL releases the grab itself on focus
  loss, so alt-tab always frees the cursor.
- Follows `Input.MouseEnabled` (re-evaluated in `inputMouseEnable`):
  controller-only players keep a free cursor.
- Set `Input.MouseGrab=0` in pd.ini to restore the old unconfined cursor.
- Dedicated build unaffected (input.c excluded there).
