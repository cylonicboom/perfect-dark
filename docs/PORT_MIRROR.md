# Port-only "Mirror" cheat (horizontal world flip)

Flips the **entire rendered 3D world left-to-right** (a horizontal reflection). Unlike a Combat Sim
option, this is a **cheat** (Cheats → Gameplay, always unlocked) so it works everywhere — including
the **Carrington Institute hub** — because the CI hub (`STAGE_CITRAINING = 0x12`) renders through the
exact same `lvRender` → `bgRender` path as every gameplay level. No stage special-casing is needed.

The world flip itself is **cosmetic**: gameplay logic, hit detection, and LOS all run on un-mirrored
game-side coordinates. To make the mirror *playable* the cheat also (a) **inverts only the local
player's camera yaw** (look/turn) so turning matches the flipped view — strafe, manual aim and the
crosshair stay natural — and (b) **mirrors the ammo HUD** to the opposite side so the bullet counter
sits under the now-left-hand weapon. (a) only changes the local player's heading, which is real and
syncs normally, so it's still netplay-safe and save-safe. General 2D HUD/text are not mirrored and
stay readable.

Live toggle: `/mirror [on|off]` console command (alongside the Cheats-menu checkbox). Default off.
The N64 build is byte-identical (all game-side edits are under `#ifndef PLATFORM_N64`, or — for the
shared `bgunDrawHud` path — reduce to the original expressions via identity macros; the rest is
fast3d-only).

## Controls — yaw + strafe inversion only (`bondwalk.c`)

Two movement axes are inverted, each at the single point where the input becomes motion, gated on
`cheatIsActive(CHEAT_MIRROR) && !currentplayer->isremote` (local player only):

- **Look / turn** — `bwalkUpdateTheta` negates `rotateamount` (the per-tick yaw delta from
  `speedtheta`) before it's integrated into `vv_theta`.
- **Strafe** — `bwalkUpdateSpeedSideways` negates the incoming `targetspeed` (covers digital step
  keys `-1`/`+1` and analog `analogstrafe*scale` alike); harmless when it's 0.

Everything else stays on the original input so it's natural: **manual aim** (crosshair moved by
`c1stickxraw` / mouse `freelookdx`), the **crosshair swivel** (`bmoveApplyCrosshairSwivel`, which also
reads `speedtheta` — left un-negated), and forward/back. **Aim-mode edge-turn** rides
`speedtheta`→`rotateamount`, so it inverts with the look (the view pans the mirror-correct way when the
reticle reaches a screen edge).

An earlier approach negated the raw stick/mouse inputs at the source in `bmoveProcessInput`; that
over-inverted strafe + aim + crosshair and was reverted in favour of these two motion-level negates.

**Other control paths now inverted too** (same `cheatIsActive(CHEAT_MIRROR) && !isremote` gate, at the
input→motion point):

- **Eyespy / camspy** (`bondeyespy.c` `eyespyProcessInput`) renders through the same flipped `lvRender`
  but doesn't run the walk-mode negates — so its horizontal **look** (mouse `mdx` + stick `c1stickx`
  → `eyespy->theta`) and **strafe** (`sidespeed`) are negated locally.
- **Held / grabbed object** (`bondgrab.c`) rotation is driven by raw `speedtheta` (the walk fix only
  negates `rotateamount`/`speedsideways`, not `speedtheta`), so the held box/bed tracked backwards;
  the `speedtheta`-derived rotation is negated at both apply sites (`f0` in the "doextra" path and
  `angle` in `bgrab0f0cdef0`). The rotation-induced lateral and `speedsideways` already line up.
- **Auto-aim X** (`prop.c` `autoaimTick`): the horizontal target-offset fed to
  `bmoveUpdateAutoAimXProp` is negated, because the shot direction is taken from the *reflected*
  crosshair — so auto-aim must pull the crosshair to the target's **mirrored** screen side (otherwise
  it tracked the opposite side and the bullet missed).

**Limitation:** turret / vehicle (hovercraft, etc.) movement modes are still not inverted.

## Ammo HUD mirror (`bondgun.c` `bgunDrawHud`)

The **bullet counter** — the clip + reserve gauges, their numbers, and the combat-boost timer (both
hands) — is reflected to the opposite side via helpers: `bgunHudMirrorX` (reflect an x about the view
centre in the `view-pixel / g_ScaleX` HUD space), `bgunHudMirrorHalign` (swap text LEFT/RIGHT
anchoring), and `bgunHudMirrorAlign` (swap the `g_HudAlignModeR/L` widescreen edge anchor).
`bgunDrawHudGauge` gained a port-only `x1>x2` normalisation because the reflection swaps a bar's
left/right edges. The whole upper cluster is mirrored to the left too: the primary/secondary
**fire-mode square** and the **weapon-name / function-name labels** (via `bgunHudMirrorXL/XR` for the
fill boxes, `x = bgunHudMirrorX(x) - textwidth` for the rightward-rendered `textRenderProjected` text,
and the block's `bgunHudMirrorAlign`). Those labels are 2D texrects (`textRenderProjected` →
`gSPTextureRectangleEXT`), so they're moved game-side. On N64 the helpers are identity macros, so
`bgunDrawHud` is byte-identical.

## How it works

A horizontal mirror = **negate clip-space X** + **compensate triangle winding** (mirroring reverses
winding, so back-face culling would otherwise render the world inside-out) + **reflect the per-room
scissor** (rooms seen through doorways use a portal-clipped screen-space draw-slot scissor; without
reflecting it, that geometry would be flipped to the opposite side of the screen but clipped on the
original side).

State plumbing follows the `CHEAT_WIREFRAME` pattern exactly:

- `CHEAT_MIRROR` = **47** (`src/include/constants.h`, append-only — it's a `g_Cheats[]` subscript).
- Table row + `s_cheat_literal_names[CHEAT_MIRROR] = "Mirror"` + a `MENUITEMTYPE_CHECKBOX` entry in
  `src/game/cheats.c` (all under `#ifndef PLATFORM_N64`).
- `bgTickPortals` (`src/game/bg.c`) syncs it each frame to a **1-byte** renderer global:
  `gfx_mirror_mode = cheatIsActive(CHEAT_MIRROR) ? 1 : 0;`. Declared `extern unsigned char` game-side
  (game `bool` is `s32`; the renderer's is 1 byte — see the `gfx_wireframe_mode` note for the
  size-mismatch gotcha), and `bool` in `port/fast3d/gfx_api.h` / `gfx_pc.cpp`.
- `/mirror [on|off]` in `netConsoleCommand` (`port/src/net/net.c`) flips the cheat's active+enabled
  bits in `g_CheatsActiveBank1` / `g_CheatsEnabledBank1`; `bgTickPortals` picks it up next frame.

## Renderer injection points (fast3d, `port/fast3d/gfx_pc.cpp`)

The renderer has **two** geometry paths and they cull differently, so both are handled:

| Path | Geometry flip | Winding compensation | Scissor |
|---|---|---|---|
| **Immediate** (`/dlcache off`, the viewmodel + menu 3D, dlcache miss frames) | `gfx_sp_vertex`: `if (gfx_mirror_mode) x = -x;` after `gfx_adjust_x_for_aspect_ratio` | `gfx_sp_tri1`: `if (gfx_mirror_mode) cross = -cross;` at the `G_EX_INVERT_CULLING` comment. CPU cull — `GL_CULL_FACE` is **disabled** for immediate (`gfx_opengl.cpp`) | flush site reflects `rdp.scissor.x` via `gfx_mirror_scissor_x`, **gated on `!is_rect`** (don't reflect 2D-rect scissors) |
| **dlcache replay** (cached static rooms) | `dlcacheReplay`: negate the X scale folded into `uMVP` (`mx = gfx_mirror_mode ? -1 : 1`) | `dlcacheReplay`: `cache_set_cull(cm, g_DlCacheFrontCcw ^ (gfx_mirror_mode != 0))` — GPU `glFrontFace` | `dlcacheReplay` reflects `rdp.scissor.x` via `gfx_mirror_scissor_x` |

`gfx_mirror_scissor_x(x, w)` reflects a scissor box about the viewport's horizontal centre **in window
pixels**: `2*(rdp.viewport.x + rdp.viewport.width/2) - (x + w)`. This is correct regardless of the
aspect-mode viewport/scissor asymmetry, because the geometry reflects about NDC x=0, which `glViewport`
maps to the viewport centre, and both `rdp.viewport` and `rdp.scissor` are ultimately window-pixel
coordinates (that's how GL combines them). Full-viewport scissors (HUD, the player's own room) are
symmetric about that axis, so the reflection is a no-op for them.

**No cache invalidation on toggle.** The dlcache stores object-space geometry; only the replay-time
`uMVP` and cull winding change with the flag, so `/mirror` toggles live with `/dlcache on`.

## Why the HUD stays un-mirrored

2D rects/texrects (the HUD, ammo, crosshair, text) are drawn by `gfx_draw_rectangle`, which builds its
own vertices and **does not** go through `gfx_sp_vertex` — so the X-negate never touches them. They
also disable culling (`rsp.geometry_mode = 0`), so the `gfx_sp_tri1` cross-negate is skipped (it's
inside the `G_CULL_BOTH` block). And the scissor reflection is gated on `!is_rect`, so a rect's scissor
is never reflected even when it's a sub-region — important because `gfx_draw_rectangle` also forces a
full-screen viewport for its draw, which would otherwise reflect a split-screen HUD into the wrong
quadrant.

## Known limitations / things to verify in-game

- **Viewmodel — mirror-imaged in the left hand (by choice).** The gun + hands are full 3D, so the
  renderer flips them with the world: the gun lands in the **left** hand, model reversed, and its
  firing/muzzle side flips to match. The user evaluated a `G_NOMIRROR_EXT`-tagged "normal model in the
  right hand" and chose to keep the **mirror-imaged left-hand** gun instead (it's internally consistent
  — gun, swing and firing all flip together). So the viewmodel is left as-is (no special handling). A
  *normal* model in the *left* hand would still need a reflection inside the gun's model-matrix
  hierarchy (`hand->gunmodel.matrices`, built by the pose system); not pursued.
  - **Gun aim/sway + bullet** are corrected via two *independent* paths (a screen-space reflection of
    `crosspos2`→`aimpos` was tried and **reverted** — that's a secondary auto-aim/muzzle path and it
    *doubled* the error):
    - **Visual lean:** the gun's horizontal aim is the yaw `sp1a4.y` in the gun-pose builder
      (`bondgun.c` ~8182, → `cammtx`→`posmtx`→`muzzlemat`). The renderer flips the gun, so the lean
      tracked backwards; negating `sp1a4.y` (when `CHEAT_MIRROR`, after the `lastrotangy` save) makes
      the gun lean toward the crosshair. It's the direct lean control, so no doubling.
    - **Shot direction:** `bgunCalculatePlayerShotSpread` builds `gundir` from `player->crosspos` (game
      space). The local crosshair copy is reflected about the view centre before `cam0f0b4c3c` so the
      bullet hits what the player sees under the crosshair on the flipped screen (the displayed reticle
      is untouched). Together the gun and bullet both track the crosshair.
- **UI borders (pickup-box / menu) — fixed via `G_NOMIRROR_EXT`.** `menugfxDrawFilledRect` →
  `menugfxDrawProjectedLine` / shimmer build 3D triangles (`menugfxDrawTri2`), which the renderer was
  flipping — mirroring the pickup-message box **border** (`hudmsgRenderBox`; also the always-on
  `/graslu` banner, a handy test case) off its un-mirrored 2D text/fill. New geometry-mode bit
  `G_NOMIRROR_EXT` (`gbiex.h`, shared with the renderer via `PR/gbi.h`); `gfx_sp_vertex`/`gfx_sp_tri1`
  skip the mirror X-negate + winding-negate when it's set; `menugfxDrawTri2` tags its geometry with it
  (gated on `cheatIsActive(CHEAT_MIRROR)`), so all menu/HUD line geometry stays put while the world (and
  the left-hand viewmodel) keep their flip. (menugfx panels that allocate vertices directly — gradients,
  carousel chevrons — aren't tagged yet; extend the same way if any show up mirrored.)
- **HUD message boxes (pickup notifications + `/graslu`) are mirrored to the opposite side** (by user
  choice). They normally sit on the left; `hudmsgsRender` (hudmsg.c) and `netGrasluRender` (net.c)
  reflect the box's left anchor `x` about the view centre (`viGetWidth()/screenw - x - width`,
  accounting for box width so the whole box moves as a unit) and flip the alignh L↔R align mode. The 2D
  text/fill move via this game-side reflection; the 3D border keeps its `G_NOMIRROR_EXT` tag *precisely
  so the renderer doesn't double-mirror* the already-reflected border. Centred messages (subtitles)
  reflect to themselves (no-op).
- **Aiming crosshair vs weapon.** Controls are now inverted, so turning/strafing feel consistent with
  the flipped view. The crosshair itself is a centred 2D aim reticle; fully reconciling its world
  aim-point with the (still mirror-imaged) weapon is part of the remaining viewmodel "Phase 2" work.
- **2D HUD scissors** are not reflected (the `!is_rect` gate), so HUD/menus are safe. The one residual
  edge: if a 3D draw and an immediately-following 2D rect happen to share the *exact same* sub-region
  scissor with no scissor change between them, the rect inherits the 3D draw's reflected scissor (the
  change-detection memcmp skips the re-flush). HUD/rects normally reset the scissor to the full view
  first, so this is unlikely — but watch for any clipped HUD panel and report it.
- **Light glares are mirrored game-side** (they're 2D texrects that bypass the renderer flip): the
  room light-fixture glares (`artifactsRenderGlaresForRoom`, `artifact.c`) and the sun lens flare
  (`skyRenderArtifacts`→`skyRenderFlare`, `sky.c`) reflect their screen X about the view centre when
  `CHEAT_MIRROR` is active, so they track the mirrored world. The glares' *visibility* is a world-space
  LOS test, which is unaffected by the screen-space flip.
- **GL backend only** (the renderer this port uses). Other rapi backends are untouched.

## Verification

1. Carrington Institute hub → `/mirror on`: the whole hub flips left-right; HUD text stays readable.
2. Walk to a doorway / room edge: geometry is **not** inside-out and rooms-through-doorways clip on the
   correct side. Test with `/dlcache off` (immediate path) **and** `/dlcache on` (cached path) — they
   flip geometry, winding and scissor through different code.
3. Combat Sim level: world + characters + first-person weapon all mirror; HUD readable.
4. Toggle `/mirror` repeatedly with `/dlcache on`: live, no cache artifacts.
5. `/wireframe` + `/mirror` together: independent flags, both apply.
