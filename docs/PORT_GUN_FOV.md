# Port-only: Gun FOV (separate viewmodel FOV) + relative weapon zoom

Port-only feature: the first-person gun/hand viewmodel renders with its **own
FOV** ("Gun FOV" slider) instead of the world FOV, so a high Vert FOV no longer
stretches/warps the weapon. On top of that, weapon zoom (sniper, Falcon scope,
FarSight, horizon scanner) is now **relative to the world FOV** in true
magnification terms, instead of tweening to linearly scaled absolute FOV
targets.

All of it is `#ifndef PLATFORM_N64`; the N64 build is byte-identical. Purely
render/cosmetic on the viewmodel side — no wire format change, no
`NET_PROTOCOL_VER` bump (but see the netplay note at the bottom).

## User-facing surface

- **Slider**: Extended options → Game → "Gun FOV", directly under "Vert FOV".
  Range 15–170, default 60. 60 = stock N64 viewmodel look at any world FOV;
  setting it equal to Vert FOV reproduces the old (warped) behaviour exactly.
- **Config**: `Game.PlayerN.GunFovY` (float, 5–175) in `pd.ini`, per local
  player (`g_PlayerExtCfg[].gunfovy`). Values below 5 disable the feature
  (viewmodel follows world FOV as before).
- Zoom relativity rides the existing **"FOV Affects Zoom"** checkbox
  (`Game.PlayerN.FovAffectsZoom` / `fovzoommult`): ON (default) = zoom targets
  mapped relative to the base FOV; OFF = absolute vanilla targets (60-based).

## The six pieces (all interlocking)

### 1. Render-pass projection override — `bgunRender` (`src/game/bondgun.c`)

The gun pass already builds its own projection (`vi0000aca4(gdl, 1.5, 1000)`,
which reads the world FOV from `g_ViBackData`). The override swaps in a
projection built from the **effective gun FOV** via `viPerspectiveFov()` — a
new port-only `vi.c` helper that is `vi0000aca4` with an explicit `fovy`
parameter (`vi0000b0e8` could not be reused: it takes the stage znear/zfar and
the gun needs znear 1.5 or it clips).

Scoping is critical — the gun pass also draws **world-space** geometry that
must keep the world projection or it visibly misaligns:

- `lasersightRenderBeam` (before the hand loop) — world FOV.
- `beamRender(&hand->beam, …)` (first thing in each hand's visible block) —
  world FOV; the override is emitted **after** it.
- `casingsRender` (after the loop) — world FOV; the world projection is
  restored at the **end of each hand block** (`vi0000aca4` again).

Skipped entirely during teleport (`teleportstate != TELEPORTSTATE_INACTIVE`)
— that effect forces its own 60° projection (`vi0000b0e8(gdl, 60, f2)`) — and
skipped when the effective gun FOV equals the world FOV (no redundant matrix
loads; default users at Vert FOV 60 get a bit-identical frame).

### 2. Viewmodel position offsets follow the *render* FOV

The port already shipped a NeonNyan-style "viewmodel position fix"
(`bgunGetFovOffsetY/Z`) that pushes the gun back/down proportionally to
`PLAYER_DEFAULT_FOV`. Left as-was, it double-compensated: gun drawn at 60° but
still shoved back as if drawn at 140°. The offsets now derive from
`bgunGetRenderFovY()` — the Gun FOV when valid, else the world FOV — so at Gun
FOV 60 they collapse to **zero** and the viewmodel sits exactly where it does
on N64. Offsets stay static during zoom (vanilla: the gun magnifies in place).

### 3. Aim-pose sync — `bgun0f0a5550`

The gun's crosshair-tracking yaw/pitch is computed against a view-space aim
point derived from the crosshair's screen position **under the world
projection** (`bgun0f0a24f0` → `cam0f0b4c3c`). A view-space ray projects to
different screen points under different FOVs, so the barrel over-rotated past
the crosshair whenever the FOVs differed (~4.8× overshoot at 140/60). Fix: the
aim point's lateral components are rescaled by

```
tan(gunfov/2) / tan(basefov/2)
```

before the angle math, putting the barrel's *apparent* aim back on the
crosshair. Purely visual — `lastrotangx/y` are write-only in the codebase and
the actual shot direction/spread never see this. Centered crosshair = no-op.

### 4. Laser-sight sync — `bgunUpdateLasersight`

The laser beam renders in world space (world projection) but the gun is drawn
with the gun projection, so the beam origin (muzzle node, view space) appeared
detached from the drawn barrel tip. The origin's view-space laterals are
rescaled by the **inverse** factor (`tan(base/2)/tan(gunfov/2)`) before the
view→world transform, which makes it land on-screen exactly where the gun-FOV
render puts the barrel. Also applied to the two barrel-tracking far-point
variants (reload/busy animation point, xray/posrot direction). Deliberately
NOT applied to the crosshair-aimed far end and the wall dot — those must stay
on the **true** aim point.

### 5. Gun magnifies with weapon zoom (tan space) — `bgunRender`

The effective gun render FOV scales with the world's current zoom ratio:

```
tan(gunRenderFov/2) = tan(gunFov/2) × tan(currentWorldFov/2) / tan(baseVertFov/2)
```

Not zoomed → identity. Zoomed → the gun magnifies on screen by *exactly* the
world's magnification factor (vanilla zoom feel). Side benefit: the
compensation ratios in (3) and (4) become **constant** under this scaling —
they reduce to the unzoomed `gunfov` vs `PLAYER_DEFAULT_FOV` ratio, which is
what the code uses (no per-frame zoom terms), keeping everything in sync
mid-tween.

### 6. Relative weapon zoom — `ADJUST_ZOOM_FOV` → `playerAdjustZoomFovY`

`ADJUST_ZOOM_FOV(x)` (`src/include/data.h`) was a linear scale
(`x × basefov/60` when "FOV Affects Zoom" is on). Linear FOV ratios are not
magnification ratios, so zoom strength drifted with base FOV (sniper at base
140: 3.92× linear / 10.2× with the option off, vs 4.39× true vanilla). The
macro now calls `playerAdjustZoomFovY()` (`src/game/player.c`):

```
tan(out/2) = tan(in/2) × tan(base/2) / tan(30°)        [base = 60 × fovzoommult]
```

Each vanilla zoom target (2–60) now produces the **same on-screen
magnification** relative to the player's world FOV as it does relative to 60
on N64. `ADJUST_ZOOM_FOV(60)` still maps to exactly the base FOV (no zoom),
and `mult == 1` ("FOV Affects Zoom" off) is identity — absolute targets like
before. Applies everywhere through the macro: initial `gunzoomfovs` (15/60/30),
the 2–60 zoom-step clamps, FarSight/Eraser dynamic zoom.

`playerUnadjustZoomFovY()` is the inverse; `hudmsgRenderZoomRange`
(`src/game/hudmsg.c`) uses it to map stored values back to vanilla 60-space so
the zoom "X" readout shows the N64 numbers (1.00X–4.00X sniper) at any FOV
(with a float-fuzz snap so the `== 60` no-zoom early-out keeps working).

## Gotchas

- **No `tanf` in this codebase.** Use the camera.c idiom `sinf(x)/cosf(x)`
  (helpers: `bgunTanHalfFovY` in bondgun.c, `playerTanHalfFovY` in player.c).
  PD's own `atan2f(x, z)` (game/atan2f.h) equals `atan(x)` for `z = 1, x > 0`
  — used to convert tangents back to degrees.
- The gun pass interleaves world-space and gun-space draws per hand
  (beam → gun model → next hand's beam → …). Any new draw added to
  `bgunRender` must pick the right projection side of the per-hand
  override/restore pair.
- `PLAYER_EXTCFG()` wraps `mpindex & 3` — same convention (and same netplay
  edge cases) as every other ext setting.
- **Known cosmetic gap**: muzzle smoke and fired-shot tracers still spawn at
  the true (world-projected) muzzle — visible for a frame or two when firing
  with mismatched FOVs. Fixable with the same reprojection as (4) if it ever
  bothers anyone.
- **Netplay**: `fovzoommult` was already wire-synced, so both sides compute
  the same `ADJUST_ZOOM_FOV`. No wire format change, but mixed old/new builds
  disagree on zoom clamp values — run matched builds. Gun FOV itself is
  viewer-local cosmetics and is deliberately NOT synced.

## Files touched

| File | Change |
|---|---|
| `src/include/types.h` | `f32 gunfovy` in `struct extplayerconfig` |
| `src/game/mplayer/mplayer.c` | default `.gunfovy = 60.f` |
| `port/src/main.c` | `Game.PlayerN.GunFovY` config registration |
| `port/src/optionsmenu.c` | "Gun FOV" slider + `menuhandlerGunFieldOfView` |
| `src/lib/vi.c`, `src/include/lib/vi.h` | `viPerspectiveFov()` helper |
| `src/game/bondgun.c` | render override, offsets, aim sync, laser sync, zoom scaling, `bgunTanHalfFovY` |
| `src/game/player.c`, `src/include/game/player.h` | `playerAdjustZoomFovY` / `playerUnadjustZoomFovY` |
| `src/include/data.h` | `ADJUST_ZOOM_FOV` → tan-space mapping |
| `src/game/hudmsg.c` | zoom "X" readout remap |
