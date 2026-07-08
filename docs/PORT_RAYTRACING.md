# Port Raytracing Suite (screen-space AO / shadows / GI / SSR / path tracing)

**Status: GL backend runtime-CONFIRMED (2026-07-08, full checklist passed).
SDL_GPU backend (Vulkan / D3D12) implemented, compile-verified only — run the
checklist on it before trusting it.** SDL_GPU additionally requires MSAA off
(see the SDL_GPU section).

## What this is

A screen-space raytracing post pipeline for the OpenGL fast3d backend,
toggled live with `/rt` and persisted via `Video.RT.*` in `pd.ini`:

- **Ambient occlusion** — cosine-hemisphere occlusion rays marched against the
  depth buffer (raymarched AO, not kernel SSAO), bilateral-blurred.
- **Sun shadows** — a screen-space directional shadow march toward the sun.
  On stages with a **lens-flare sun** (Hostage One, Infiltration, Rescue,
  Escape, Air Base, Crash Site, Skedar Ruins…), the direction tracks the
  actual sun automatically (`/rt autosun`, default on): `env suns[0].pos` is
  an absolute world point, so the direction is recomputed per camera per
  frame (`rtComputeSunDir`, artifact.c). The sun's own RGB also replaces the
  warm-white daylight hue in the skylight mapping. Sunless stages fall back
  to the manual `/rt sun X Y Z`. Still **off by default** (stylized indoors).
- **SSR reflections** — reflected-ray march with binary refinement, fresnel
  weighting and edge/distance confidence fades. Everything is slightly
  reflective at grazing angles (there is no material data to say what's shiny).
- **Global illumination** — `ssgi` mode: single-bounce screen-space GI;
  `pt` mode: stochastic multi-bounce **path tracing** (2-3 bounces, 1-3 paths
  per pixel per frame) with temporally accumulated, depth-validated
  reprojection so the noise converges over frames.
- **Dark / relight mode** — `/rt dark` crushes the whole scene to a
  configurable ambient floor (`/rt ambient F`, default 0.08) and re-illuminates
  it with **dynamic point lights harvested from the map's own light fixtures**
  (the same room-light data the glare/lens-flare artifacts draw from), each
  with its own screen-space shadow ray march. Lights honour game state: a
  shot-out light stops illuminating exactly like it stops glaring. Plus a
  camera-mounted **test torch** (`/rt torch`) — a view-axis spotlight that
  needs no shadow rays by construction (along the eye ray the depth buffer IS
  the first hit). The lights also work *without* dark mode as additive
  highlights. Per-light output is clamped by a **hue-preserving brightness
  cap** (`/rt lightmax`, default 1.0) so a big `lightrad` doesn't blow small
  rooms out to pure white — close-range surfaces saturate toward the light's
  COLOUR instead. And the **skylight** (`/rt skylight`, default on) derives a
  global tint from the stage's live sky colour: warm skies (sunset/dawn) wash
  their own rich hue, bright blue skies read as a sunny day (warm-white),
  dark blue skies as night (dim moon-blue) — applied to the dark-mode ambient
  floor and the GI sky term (`/rt skygain`). The base colour (`g_Env.sky_*`)
  is ALSO PD's fog colour (envTick sets the RDP fog colour from the same
  field), so foggy stages derive from their fog automatically; on cloudy
  stages the tinted cloud layer is blended in 50/50. Black sky (indoor
  stages) = neutral, no change.
- **Debug views** — `/rt debug depth|normals|ao|shadow|gi|ssr|light` replaces
  the scene with the named buffer; this is the diagnosis tool for everything
  below.

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
5b. **Dynamic lights + torch** (when harvested lights exist, the torch is on,
   or dark mode needs them): map lights are collected game-side by
   `rtCollectLights` (artifact.c) — nearest lit room lights around the
   player's camera, world pos = light bbox average + room pos, colour = the
   4/4/4/4 nibbles, intensity = brightnessmult/32, skipping "off" and
   shot-out (`!healthy`) lights — carried in the per-player `rtcamera`
   (`RT_MAX_LIGHTS` = 64; the harvest reach is `radius + Video.RT.LightCull`
   from the CAMERA, decoupled from the falloff radius because a fixture can
   light a visible surface from far beyond its own radius — a light down a
   long corridor), transformed to view space CPU-side, and evaluated
   per pixel with distance/N·L attenuation plus a per-light screen-space
   shadow march (quality-scaled steps). The torch is a view-axis spotlight
   evaluated in the same pass.
6. **Composite** over the game framebuffer as two blended fullscreen
   triangles: a multiplicative quad (`dst *= AO²·intensity × shadow ×
   dark-ambient`) and an additive quad (`dst += GI·albedo·intensity +
   SSR·confidence·intensity + light·albedo`). In dark mode the GI/SSR terms
   are scaled down (they sample the pre-darkened capture) while the
   dynamic-light term deliberately uses the bright capture as albedo — that's
   what the lights re-reveal. **Blending, not replacement, so per-sample MSAA
   edge colour survives** — only the debug views overwrite the scene.

All GL state the passes touch is saved with `glGet*` on entry and restored on
exit (FBO bindings, viewport/scissor, depth/blend/cull, program, VAO/VBO,
texture units 0-7), so the immediate-mode renderer's cached state never goes
stale; the dispatcher additionally re-flags `viewport_or_scissor_changed`.

Requirements gate (gfx_opengl.cpp wrapper): desktop GL, GLSL ≥ 130,
`gfx_framebuffers_enabled`. RGBA16F render targets must be supported (checked
at target build; failure logs and self-disables the suite). Any shader build
failure also logs + self-disables (`s_broken`), so a broken driver costs one
log line, not a hang.

### The shared shader layer (`gfx_rt_common.h`)

The pass ALGORITHMS — every fragment `main()` plus the reconstruction/hash
helpers, the quality table and the CPU matrix helpers — live in
`port/fast3d/gfx_rt_common.h`, included by BOTH backend implementations. The
bodies deliberately contain **no declarations**: each backend prepends its own
prelude declaring `vUV`/`oCol`, the samplers and the uniform names the bodies
reference (the header's top comment lists them). GL declares them as loose
GLSL-130 uniforms (unused ones optimize out; same-type samplers sharing a
texture unit is legal); SDL_GPU declares per-pass `set=2` samplers and one
std140 UBO whose MEMBERS carry the same bare names. Fix a pass's algorithm in
the header and both backends get it; add a uniform and you must touch both
preludes (GL: kFSCommon + the rtLink sampler table; SDL_GPU: the UBO string +
`RtGpuUni` mirror + `rt_pass_samplers`).

### The SDL_GPU implementation (Vulkan / D3D12; gfx_sdlgpu.cpp RT section)

Same passes, recompiled as GLSL450 through the same glslang(/SPIRV-Cross)
pipeline as the combiner + HDR present shaders — so it runs wherever the
backend does (Vulkan and D3D12; Metal untested like everything there).
Differences from GL, all deliberate:

- **Depth is sampled directly** from the framebuffer depth texture — fb depth
  now carries `SAMPLER` usage (added in `update_framebuffer_parameters` when
  `gpu.depth_samplable`, a new init-time capability check on the chosen depth
  format). No depth copy exists at all.
- **Scene colour** is captured with one same-format
  `SDL_CopyGPUTextureToTexture` copy pass on the render CB.
- **MSAA framebuffers are unsupported**: SDL_GPU can neither sample nor
  resolve multisample depth (`SDL_GPUDepthStencilTargetInfo` has no resolve).
  The resolve warns once ("set Video.MSAA=1") and no-ops. GL keeps its MSAA
  support (the capture blit resolves there).
- **Depth linearization is unchanged**: gfx_pc's 0..1-clip remap is
  `z01 = (z+w)/2`, so the shared `lin()` (`d*2-1` → GL NDC) still holds.
- **`uYSign` is -1** (the GL default's inverse): fb0 is stored top-down
  (top-left texture origin). `--gpu-invert-y` flips it back — that's the
  runtime lever if `/rt debug normals` shows vertically-mirrored lighting.
  The viewport rect converts GL bottom-left → top-left with the fb height,
  same as `apply_viewport`.
- The fullscreen triangle is `gl_VertexIndex`-generated (the present-VS
  pattern, no vertex buffer); the uv rect rides a tiny `set=1` VS UBO.
- Pipelines are created eagerly at first resolve (formats are fixed by then:
  FP16 / RGBA8 / `gpu.fb_format` — the composite pipelines key on fb_format,
  so **HDR's FP16 fb0 is handled by construction**). The 9 shader pairs
  compile once at first use (not in the disk shader cache; ~one-time hitch).
- After the passes: `st.pass`/`st.bound_pipeline` reset and
  `st.vs_dirty`/`st.fs_dirty` forced true — **the RT uniform pushes clobber
  the command buffer's push-uniform slots**, and the immediate path must
  re-push its own blocks. Forgetting this = psychedelic geometry.
- RT textures are cleared once at creation so blur edge-taps / history reads
  outside the viewport rect never see garbage (GL leaves them undefined and
  got away with it; SDL_GPU is explicit).

## Files

| File | Change |
|---|---|
| `src/include/gbiex.h` | `G_RTRESOLVE_EXT 0x4b` + `gDPRtResolveEXT` (next free EXT opcode: 0x4c) |
| `port/include/rt_ext.h` | NEW — `rtcamera` struct + all `gfx_rt_*` control globals (shared C header) |
| `port/fast3d/gfx_rt_common.h` | NEW — SHARED pass bodies + GLSL helpers + quality table + matrix helpers (both backends) |
| `port/fast3d/gfx_rt.h/.cpp` | NEW — the GL pipeline (preludes, targets, temporal history, state save/restore) |
| `port/fast3d/gfx_rendering_api.h` | `rt_resolve` rapi entry (nullable) |
| `port/fast3d/gfx_pc.cpp` | control-global definitions + dispatch case |
| `port/fast3d/gfx_opengl.cpp` | capability gate + fb-info wrapper, rapi entry |
| `port/fast3d/gfx_sdlgpu.cpp` | full SDL_GPU implementation (RT section: GLSL450 preludes, UBO, pipelines, passes) + samplable fb depth |
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
/rt debug off|depth|normals|ao|shadow|gi|ssr|light
/rt sun X Y Z                     world-space direction TOWARD the light
/rt sky R G B                     GI miss/sky radiance
/rt aoint|aorad|shint|shlen|ssrint|giint|giscale <f>
/rt dark [on|off]                 blacken the world (relight from lights)
/rt ambient <f>                   dark mode's remaining base brightness
/rt lights|lightshadows [on|off]  map-light harvest / per-light shadow rays
/rt lightint|lightrad <f>         map-light gain / falloff radius (world units)
/rt lightcull <f>                 harvest reach beyond the radius (default 3000)
/rt lightmax <f>                  per-light brightness cap, hue-preserving (1.0)
/rt skylight [on|off]             sky-colour ambient tint + GI sky (default on)
/rt skygain <f>                   skylight -> GI miss-radiance scale (0.3)
/rt bounces <n>                   GI/PT bounce override 1..8, 0 = quality preset
/rt autosun [on|off]              shadow dir tracks the stage's lens-flare sun
/rt torch [on|off]                camera-mounted test spotlight
/rt torchint|torchrange <f>       torch tuning
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

- **GL backend runtime-confirmed (2026-07-08, full checklist). SDL_GPU
  implementation compile-verified only** — the checklist below has never run
  on Vulkan/D3D12; the depth convention and uYSign derivations are reasoned,
  not observed (`--gpu-invert-y` and `/rt debug` are the levers if wrong).
- SDL_GPU requires MSAA off (no way to sample or resolve multisample depth);
  Metal is untested like the rest of that backend.
- Screen-space by construction: light/reflections from offscreen or occluded
  geometry don't exist; SSR shows backfaces of nothing (confidence fades hide
  most of it); PT bounces terminate at the screen edge and fall back to the
  `sky` term. Disocclusion during fast motion = one-frame GI noise.
- The viewmodel is excluded (captures pre-gun depth) — the gun neither casts
  nor receives any of the effects.
- GL ES and GLSL < 130 are gated off. HDR (SDL_GPU) composites in the FP16
  buffer by construction but is runtime-untested with the additive pass.
- Split-screen: per-viewport passes + per-player history are implemented, but
  the viewport-rect Y-orientation handling is runtime-unverified.
- Scene colour is used as both albedo and radiance in GI/PT (standard
  screen-space hack) — emissive-looking surfaces over-contribute.
- Dark mode's "albedo" is the baked scene (albedo × baked lighting), so a
  torch reveals the original shading, not flat unlit texture colour — usually
  looks natural, but pre-baked dark corners stay dim even under a light.
- Light harvesting is INDEPENDENT of room streaming: the per-room light
  index (`g_Rooms[].numlights/lightindex`, the dlights.c idiom) and
  `g_BgLightsFileData` are both stage-resident, so lights exist — with live
  shot-out state — even for rooms never loaded. The harvest is per-camera
  nearest-64 within `radius + lightcull`; a scene with more candidates drops
  the farthest (raise `/rt lightcull` and/or `RT_MAX_LIGHTS` if that ever
  shows). Lights are point sources at the fixture bbox centre — long
  fluorescent tubes light from their midpoint.
- The per-light shadow rays are screen-space like everything else: an
  occluder outside the frame won't cast, and light through a wall that's
  offscreen can leak.

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
8. MSAA on + RT on (GL: edges should stay antialiased; SDL_GPU: expect the
   one-time "set Video.MSAA=1" warning and no effects), supersampling on
   (Video internal scale ≠ window), and a 2P split-screen sanity pass.
9. Perf: `/fps` with quality 0/1/2 at native res.
10. SDL_GPU specifics: run 1-7 on `--renderer sdlgpu` (Vulkan), then
    `--gpu-driver direct3d12`; verify `/gpu` still prints sane state after
    `/rt on`; if normals/shadows are vertically mirrored, retry with
    `--gpu-invert-y` and report — that pins the uYSign derivation. Also HDR
    on + `/rt gi ssgi` (FP16 composite path).
11. Dark mode: `/rt dark` in a lit corridor — world goes near-black except
    pools of light under the fixtures (`/rt debug light` isolates the light
    buffer). Shoot a light out — its pool must die with the glare. `/rt
    torch` and sweep the walls; `/rt ambient 0.02` for full horror. Then
    `/rt lightrad 1200` / `/rt lightint 2` to taste.
