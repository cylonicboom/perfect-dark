# Port Raytracing Suite (screen-space AO / shadows / GI / SSR / path tracing)

**Status: compile-verified only — not yet runtime-tested. Run the checklist at
the bottom before trusting any of it.** GL backend only; SDL_GPU ignores it.

## What this is

A screen-space raytracing post pipeline for the OpenGL fast3d backend,
toggled live with `/rt` and persisted via `Video.RT.*` in `pd.ini`:

- **Ambient occlusion** — cosine-hemisphere occlusion rays marched against the
  depth buffer (raymarched AO, not kernel SSAO), bilateral-blurred.
- **Sun shadows** — a screen-space directional shadow march toward a
  configurable world-space light (`/rt sun X Y Z`). Stylized (PD has no global
  sun), so **off by default**.
- **SSR reflections** — reflected-ray march with binary refinement, fresnel
  weighting and edge/distance confidence fades. Everything is slightly
  reflective at grazing angles (there is no material data to say what's shiny).
- **Global illumination** — `ssgi` mode: single-bounce screen-space GI;
  `pt` mode: stochastic multi-bounce **path tracing** (2-3 bounces, 1-3 paths
  per pixel per frame) with temporally accumulated, depth-validated
  reprojection so the noise converges over frames.
- **Debug views** — `/rt debug depth|normals|ao|shadow|gi|ssr` replaces the
  scene with the named buffer; this is the diagnosis tool for everything below.

Why screen-space: SDL_GPU exposes no hardware ray-tracing pipelines, and the
world geometry only exists as transient display-list streams (or dlcache GPU
buffers keyed by leaf pointers) — there is no scene BVH to trace. Everything
here is reconstructed from the depth buffer + captured colour, which is the
same family of technique the "RTX shader mod" packs use. The inherent limits
(section "Honest limits") come from that choice.

## Architecture

### The resolve point (`G_RTRESOLVE_EXT 0x4b`, gbiex.h)

`playerRenderHud` (player.c) emits `gDPRtResolveEXT(gdl++, cam)` **at its very
top**: the world + props are fully rendered and the depth buffer is still
intact. This placement is load-bearing — `bgunRender` **clears the depth
buffer** right after (see docs/PORT_GLARE_OCCLUSION.md), so capturing any
later would read gun-only depth. Consequence: the viewmodel draws after the
composite and is deliberately untouched by AO/GI/SSR.

`cam` points at a per-player-slot `static rtcamera` (port/include/rt_ext.h):
the player's `worldtoscreenmtx` (a straight 16-float copy — PD's `Mtxf`
`m[i][j]` flattens to GL column-major exactly, see `mtx4TransformVec`),
`viGetFovY()`, `viGetAspect()`, `viGetZRange()`, and the player number (the
temporal-history slot). The static array makes the pointer stable until
`gfx_run` consumes the display list at frame end.

### Dispatch (gfx_pc.cpp)

The `G_RTRESOLVE_EXT` case flushes pending triangles, then calls the new
`gfx_rapi->rt_resolve(cam, viewport)` hook (gfx_rendering_api.h) with
`rdp.viewport` — the emitting player's viewport in window coords, which is
what scopes the passes per split-screen viewport. SDL_GPU sets the hook to
`nullptr`; the dispatcher checks. Control globals (`gfx_rt_*`) are defined in
**gfx_pc.cpp** — not gfx_rt.cpp — so the dedicated server (which drops
gfx_opengl.cpp/gfx_rt.cpp but still links net.c's `/rt`) links.

### The pipeline (gfx_rt.cpp, called via gfx_opengl.cpp)

1. **Capture.** Colour + depth are copied into RT-owned textures. Two paths,
   because **depth blits require exactly matching formats**:
   - game-FBO source: one `glBlitFramebuffer` (colour+depth) — the RT scene
     textures are `GL_RGB8` / `GL_DEPTH24_STENCIL8` precisely to match the
     game framebuffer, which also makes the blit a legal MSAA resolve;
   - default-framebuffer source (`fbo == 0` — the no-MSAA / no-scaling case):
     the backbuffer's depth format is driver-chosen, so it captures via
     `glCopyTexSubImage2D` into a separate `GL_DEPTH_COMPONENT24` texture.
2. **Prepass** — view-space normals + linear depth into an RGBA16F target,
   reconstructed from depth with closer-side derivative selection (reduces
   edge streaks). Sky = depth ≥ 0.99999 → `w = 0`, every later pass keys off
   that.
3. **AO + shadow** trace → RG of an RGBA8 target → separable depth-aware blur.
4. **GI/PT** trace at `Video.RT.GIScale` resolution (default 0.5) → temporal
   accumulation into a **per-player history ping-pong** (split-screen players
   must not cross-feed) → depth-aware blur into a final GI texture. The
   history itself stays unblurred so reprojection doesn't smear.
   Reprojection = `P_prev * V_prev * inv(V_cur)` built CPU-side from the
   per-player previous camera; history is validated per-pixel against the
   stored linear depth (8% tolerance) and rejected on disocclusion.
5. **SSR** trace (full res).
6. **Composite** over the game framebuffer as two blended fullscreen
   triangles: a multiplicative quad (`dst *= AO²·intensity × shadow`) and an
   additive quad (`dst += GI·albedo·intensity + SSR·confidence·intensity`).
   **Blending, not replacement, so per-sample MSAA edge colour survives** —
   only the debug views overwrite the scene.

All GL state the passes touch is saved with `glGet*` on entry and restored on
exit (FBO bindings, viewport/scissor, depth/blend/cull, program, VAO/VBO,
texture units 0-7), so the immediate-mode renderer's cached state never goes
stale; the dispatcher additionally re-flags `viewport_or_scissor_changed`.

Requirements gate (gfx_opengl.cpp wrapper): desktop GL, GLSL ≥ 130,
`gfx_framebuffers_enabled`. RGBA16F render targets must be supported (checked
at target build; failure logs and self-disables the suite). Any shader build
failure also logs + self-disables (`s_broken`), so a broken driver costs one
log line, not a hang.

## Files

| File | Change |
|---|---|
| `src/include/gbiex.h` | `G_RTRESOLVE_EXT 0x4b` + `gDPRtResolveEXT` (next free EXT opcode: 0x4c) |
| `port/include/rt_ext.h` | NEW — `rtcamera` struct + all `gfx_rt_*` control globals (shared C header) |
| `port/fast3d/gfx_rt.h/.cpp` | NEW — the whole GL pipeline (shaders, targets, temporal history, state save/restore) |
| `port/fast3d/gfx_rendering_api.h` | `rt_resolve` rapi entry (nullable) |
| `port/fast3d/gfx_pc.cpp` | control-global definitions + dispatch case |
| `port/fast3d/gfx_opengl.cpp` | capability gate + fb-info wrapper, rapi entry |
| `port/fast3d/gfx_sdlgpu.cpp` | explicit `nullptr` rapi entry |
| `src/game/player.c` | camera snapshot + marker emit at top of `playerRenderHud` |
| `port/src/video.c` | `Video.RT.*` config keys |
| `port/src/net/net.c` | `/rt` console command (+ `/help` lines) |
| `CMakeLists.txt` | gfx_rt.cpp added to the DEDICATED_SERVER removal list (it needs glad) |

## Controls

```
/rt [on|off]                      master (also /raytrace)
/rt ao|shadows|ssr [on|off]       per effect (auto-enables master)
/rt gi [off|ssgi|pt]              GI mode; /rt pt = shortcut to path tracing
/rt quality 0..2                  sample/step budget preset (default 1)
/rt debug off|depth|normals|ao|shadow|gi|ssr
/rt sun X Y Z                     world-space direction TOWARD the light
/rt sky R G B                     GI miss/sky radiance
/rt aoint|aorad|shint|shlen|ssrint|giint|giscale <f>
/rt status
```

Config: `Video.RT.Enabled/AO/Shadows/SSR/GI/Quality` (ints),
`Video.RT.AOIntensity/AORadius/ShadowIntensity/ShadowLength/SSRIntensity/`
`GIIntensity/GIScale/SunX..Z/SkyR..B` (floats). Defaults: master **off**;
when enabled → AO + SSR + SSGI on, shadows off, quality 1, GI at half res.

## Gotchas (read before touching)

- **Never move the marker emit below `bgunRender`** — depth is cleared there.
- **Depth-blit format matching** is why there are two capture paths and why
  the scene colour texture is `GL_RGB8` (not RGBA8): an MSAA resolve blit
  requires identical internal formats on both ends. Change either format and
  MSAA capture silently breaks with `GL_INVALID_OPERATION`.
- **`uYSign` / `invert_y`**: the game framebuffer is rendered vertically
  flipped in the supersampling case (`gfx_pc` invert_y) and the shaders fold
  that into the NDC↔UV mapping. If `/rt debug normals` ever shows ceilings
  lit like floors (or the sun direction acts vertically mirrored) on one
  render path but not the other, this mapping is the suspect.
- **Depth linearization assumes the standard GL depth mapping** built from
  `guPerspectiveF(fovy, aspect, znear, zfar)`. If `/rt debug depth` shows a
  plausible gradient, the assumption holds; if it's all-white/all-black the
  port's z-remap differs and the `lin()` in the prepass shader is where to fix
  it.
- **Weapon zoom**: reconstruction uses `viGetFovY()`; if zoom leaves the
  vi fovy at the base value while rendering with a zoom projection, RT
  geometry maths will be wrong while zoomed (visible as AO/SSR "swimming"
  only during zoom). Untested — check during runtime verification.
- The camera struct is a **snapshot pointer** consumed at `gfx_run` time; it
  lives in a `static` per-player array in player.c. Don't move it to the
  stack.
- `rt_resolve` is the **last** rapi entry; SDL_GPU's table ends with an
  explicit `nullptr`. Keep new entries after it in both tables in sync.
- Temporal history is per player-slot; the GI-scale resize (or any fb resize)
  drops all history + previous matrices (one noisy frame, by design).

## Honest limits

- **Compile-verified only.** Zero frames rendered yet.
- Screen-space by construction: light/reflections from offscreen or occluded
  geometry don't exist; SSR shows backfaces of nothing (confidence fades hide
  most of it); PT bounces terminate at the screen edge and fall back to the
  `sky` term. Disocclusion during fast motion = one-frame GI noise.
- The viewmodel is excluded (captures pre-gun depth) — the gun neither casts
  nor receives any of the effects.
- GL backend only (SDL_GPU has no implementation; GL ES and GLSL < 130 are
  gated off). HDR output path untested with the additive composite.
- Split-screen: per-viewport passes + per-player history are implemented, but
  the viewport-rect Y-orientation across the two fb orientations is
  runtime-unverified.
- Scene colour is used as both albedo and radiance in GI/PT (standard
  screen-space hack) — emissive-looking surfaces over-contribute.

## Runtime verification checklist

1. `/rt on`, then `/rt debug depth` — expect a smooth near-white-to-black
   gradient with distance (if not: depth linearization, see gotchas).
2. `/rt debug normals` — walls/floor/ceiling in distinct stable colours;
   floors should match each other. Mirrored vertical = uYSign.
3. `/rt debug ao` — dark creases in corners, white open floor. Then
   `/rt debug off` and compare corners with `/rt ao off/on`.
4. `/rt gi ssgi` in a bright-floor room — colour bleed onto adjacent walls.
5. `/rt pt`, stand still — noise should visibly converge within ~2s; moving
   the camera should not smear (reprojection) beyond a frame of noise.
6. `/rt ssr on` near shiny floors at grazing angle.
7. `/rt shadows on`, `/rt sun` pointed sensibly on an outdoor stage.
8. MSAA on + RT on (edges should stay antialiased), supersampling on
   (Video internal scale ≠ window), and a 2P split-screen sanity pass.
9. Perf: `/fps` with quality 0/1/2 at native res.
