# Port-only: light glares occluded by the first-person weapon

Port-only fix so light **glares** (the corona/bloom sprites around lights, drawn by
`artifact.c` → `func0f0b2150`) no longer paint **over** the first-person gun and hands.
N64 build is byte-identical.

## Symptom

Aiming the viewmodel across a light source, the glare sprite drew on top of the
gun/hands instead of being hidden behind them — the weapon looked semi-transparent
wherever a light sat behind it.

## Root cause

A glare is a **depth-less 2D screen sprite**:

- `artifactsConfigureForGlares` (`artifact.c`) uses `G_RM_CLD_SURF` (no z-write/compare)
  and the glare is emitted as a `gSPTextureRectangle`. At draw time nothing in the depth
  buffer can occlude it — its *only* occlusion is a per-glare **visibility pre-check**.
- On the port that pre-check is `artifacts[k].visiblelos` (`artifact.c`, the `#else`
  branch) = `artifactTestLos` → `shotTestLos(camera → light)`, which tests **world
  collision only**. The viewmodel is not collision geometry, so a light directly behind
  the gun still counts as visible and emits a glare. (N64 reads the world z-buffer here,
  which is also gun-less — see the history comment at `artifact.c:83-88`: it once used a
  full z-buffer and regressed when the z-buffer started being cleared before the gun.)
- The glare was drawn **after** the gun in `playerRenderHud`, so the depth-less sprite
  simply painted over the freshly-drawn weapon.

## Fix

`src/game/player.c`, `playerRenderHud`, first-person branch
(`cameramode != CAMERAMODE_EYESPY`, ~line 4904): draw the glares **before**
`bgunRender` instead of after — **port-only**.

The gun is opaque and is drawn into a **freshly-cleared depth buffer**
(`bgunRender` → `viPrepareZbuf` → `zbufClear` → `gDPClearDepthEXT`). So having the
opaque gun **overdraw** glares that were drawn just before it is equivalent to
depth-testing them: a clean cut at the gun silhouette, with the bloom intact
everywhere the gun isn't.

```c
gdl = boltbeamsRender(gdl);
#ifndef PLATFORM_N64
// Port: glares before the viewmodel so the opaque gun/hands overdraw them.
if (g_Vars.currentplayer->visionmode != VISIONMODE_XRAY) {
    gdl = bgRenderArtifacts(gdl);
}
#endif
bgunRender(&gdl);
gdl = lasersightRenderDot(gdl);
#ifdef PLATFORM_N64
// N64: original after-gun order, byte-identical.
if (g_Vars.currentplayer->visionmode != VISIONMODE_XRAY) {
    gdl = bgRenderArtifacts(gdl);
}
#endif
```

Only the first-person path changed. The third-person (`player.c:4890`) and eyespy
(`player.c:5232`) `bgRenderArtifacts` calls are untouched — neither draws a viewmodel —
and the `visionmode != VISIONMODE_XRAY` guard is preserved on both sides.

## Why reorder, not a renderer depth-test

A "make the glare depth-test against the gun" fix would need the fast3d backend to honor
prim-depth for texrects. It currently draws rectangles at a fixed `z = -1.0`
(`gfx_pc.cpp` `gfx_draw_rectangle`) and `depth_source_prim` only flips the depth func to
`GL_LEQUAL` — it ignores the prim-depth *value*. So a true depth test would be fragile
(and entangled with the existing z-clamp hacks). The reorder converges to the same
visual result with no renderer change, because the gun is opaque over a cleared depth
buffer. See `PORT_DLCACHE.md` for the broader fast3d depth/texrect notes.

## Verification

Build in MSYS2. In a level with visible lights (e.g. Carrington Villa), sweep the gun
in front of a light:

- **Before:** glare sprite drew over the gun/hands.
- **After:** glare is occluded by the gun silhouette and still shows beside it (bloom
  intact up to the weapon edge).

Confirm third-person and eyespy glares look unchanged, and X-ray still hides glares.
The N64 build is byte-identical (the new draw is `#ifndef PLATFORM_N64`; the original
call remains under `#ifdef PLATFORM_N64`).
