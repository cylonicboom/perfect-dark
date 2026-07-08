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
