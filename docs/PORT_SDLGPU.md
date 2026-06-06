# Port-only: SDL_GPU Rendering Backend (Vulkan / D3D12 / Metal)

A second fast3d rendering backend built on SDL3's GPU API, alongside the
existing OpenGL backend. OpenGL stays the default and is untouched; SDL_GPU is
opt-in at runtime.

**Status: Phase 2 (display-list cache + shade palette).** Phase 1 (full
immediate-path rendering: shaders, textures/samplers, pipelines, fog,
framebuffers, blur/front-buffer emulation, scissor semantics) is verified.
Phase 2 implements the port-only dlcache rapi surface for real: persistent
per-leaf vertex buffers (stride `(num_floats+1)*4`, trailing `aShadeIdx`),
per-segment replay with cull/front-face pipeline state, and the shade palette
as a count×1 RGBA8 texture sampled in the **vertex** stage (set=0) by a
lazily-compiled cached VS variant with the uShadeRoute routing. `/dlcache`
works as on GL. Not yet done: real MSAA (requests degrade to no-AA).

Debug/verify levers: `--debug-gpu` (Vulkan validation), `--gpu-invert-y`
(flips the reported clip-space y inversion at runtime in case the orientation
analysis is wrong on some driver — saves a rebuild during verification).

## Selection

- Config: `Video.Renderer=sdlgpu` in `pd.ini` (default `opengl`).
- CLI override: `--renderer sdlgpu` (or `--renderer opengl`).
- Driver: `--gpu-driver <vulkan|direct3d12|metal>`. On Windows the default
  stays **Vulkan** (the battle-tested path) even though DXBC is offered —
  D3D12 is opt-in via `--gpu-driver direct3d12`. The `SDL_GPU_DRIVER` env var
  also works. Other platforms use SDL's default (Metal on macOS, untested).
- `--debug-gpu` enables the SDL_GPU debug/validation layer.
- If the SDL_GPU device can't be created, the probe fails *before* the window
  is created and the game falls back to OpenGL with a warning in the log.
- In-game: `/gpu` (alias `/renderer`) prints the active backend, and for
  SDL_GPU the driver, shader format, applied/requested MSAA, vsync interval,
  HDR state and shader-cache entry count.
- **Extended > Video menu**: "Renderer (restart)" dropdown (OpenGL /
  SDL GPU Vulkan / SDL GPU Direct3D 12 — Metal on macOS) writes
  `Video.Renderer` + `Video.GpuDriver`; "HDR (restart)" checkbox + live
  "HDR Brightness" (paper white) slider write `Video.HDR` /
  `Video.HDRPaperWhite`.

## VRR (G-Sync / FreeSync)

`Video.VSync = -2` (menu: Vsync → "VRR (G-Sync/FreeSync)") — works on **both
backends** (GL and SDL_GPU): the backend presents with vsync off
(tearing-allowed, which VRR displays sync to; IMMEDIATE present mode on
SDL_GPU) and the frame limiter is automatically bounded just below the
display refresh per the Blur Busters rule (`refresh − refresh²/3600`: 60→59,
120→116, 144→138) so frame delivery stays inside the VRR window and never
bounces off the vsync ceiling. The user's `Video.FramerateLimit` still
applies when lower; the stored setting is never overwritten by the VRR cap
(the get-paths skip their window-manager read-backs in VRR mode). The cap
re-derives every ~2s to track display-mode/monitor changes, and composes
with the netplay cap. Requires VRR enabled for windowed games in the
GPU driver/display settings, as usual.

## HDR output (SDL_GPU only)

`Video.HDR=1` (or the menu checkbox; applied at startup): when the display
and driver support it, the swapchain switches to **scRGB extended-linear**
(FP16) composition and every internal render target (fb0, the MSAA game fb,
aux fbs, the front-buffer snapshot) switches from RGBA8 to
**R16G16B16A16_FLOAT** — fog and blend gradients gain real precision instead
of 8-bit quantisation. The final present is no longer a blit but a fullscreen
triangle (`gfx_sdlgpu_shader_compile_fixed`, same glslang/SPIRV-Cross pipeline
as everything else, so it works on Vulkan and D3D12 alike) that scales the
SDR-authored content by `Video.HDRPaperWhite / 80` (scRGB 1.0 = 80 nits;
default 200, live-adjustable via the menu slider or
`gfx_sdlgpu_set_hdr_paperwhite`). If the display rejects the composition
(OS HDR off) or anything in the setup fails, the backend logs and backs out
to SDR cleanly before any framebuffer exists. OpenGL ignores the setting.

Two implementation notes:

- **Driver auto-switch**: scRGB over Vulkan is vendor-dependent on Windows
  (NVIDIA exposes it; AMD/Intel typically only via DXGI). If HDR is requested
  and the current driver can't do it, the device is destroyed and recreated
  on `direct3d12` (logged), before any GPU resource exists; if that fails
  too, it falls back to the original driver in SDR.
- **sRGB linearization**: the game's output is sRGB-encoded, while scRGB
  swapchains expect linear (1.0 = 80 nits). The present shader applies the
  sRGB EOTF before the paper-white scale — skipping it washes out mid-tones.
- **Highlight expansion ("HDR Peak")**: `Video.HDRPeak` (live slider, default
  600 nits) keeps the scene's diffuse range at paper white and ramps only
  near-white content toward the peak via a luminance-weighted gain in the
  present pass (`gain = 1 + (peak/paper − 1)·Y⁴` — mids gain ~6% of the boost
  at Y=0.5). **Peak is a hard luminance ceiling**: the gain is capped at
  `boost/Y`, so nothing — emissive, EOTF or curve — can compound past it (the
  first cut overshot to thousands of nits). The sRGB EOTF extends linearly
  above 1.0 for the same reason. The curve is deliberately conservative
  (strength 0.35: plain white lands ~a third of the way to peak, not at it —
  the second cut sent every white pixel, HUD text included, to full peak) and
  the dazzle emissive is a subtle nudge on top (≤ +12.5% luminance), so
  marked glares read slightly hotter than scene white. Tuning constants live
  in gfx_sdlgpu.cpp: curve strength 0.175 (present FS sources) and the
  emissive map 0.075 / cap 1.125 (`update_emissive`). Raise `Video.HDRPeak`
  for a stronger effect overall. Peak ≤ paper white disables expansion.
- **Dazzle brackets (`G_SETDAZZLE_EXT`, gbiex.h 0x48) — present but
  DISABLED**: the game marks genuinely emissive draws — the light-glare
  sprites (`artifactsConfigureForGlares`/`Unconfigure`, weighted by the
  existing **Glare Brightness** slider) and the overexposure screen flash
  (`sky.c`, weighted by **Overexposure Scale**) — via a flush-aligned DL
  command that sets `gfx_hdr_dazzle` (0..1) at execution time, mapped by the
  SDL_GPU backend to a `uEmissive` FS-uniform multiplier into the FP16 range.
  Playtesting found any emissive boost on large screen-space glare billboards
  reads as overblown, so the strength is zeroed
  (`GFX_SDLGPU_DAZZLE_STRENGTH` in gfx_sdlgpu.cpp, with a `_MAX` cap) —
  marked content currently brightens exactly like any equally-bright pixel.
  The full chain is kept inert for future use. GL never reads the weight; in
  SDR the multiplier is pinned to 1 — genuine no-ops everywhere.
- **HDR10 fallback**: some driver/display stacks only expose PQ output
  (observed: `scrgb=0 hdr10=1` on D3D12). The composition picker prefers
  scRGB and falls back to `HDR10_ST2084` with a PQ present-shader variant:
  sRGB EOTF → BT.709→BT.2020 primaries → absolute luminance (paper white /
  10000 — PQ is absolute) → ST2084 encode. The boot log's
  `composition support: sdr_linear=… scrgb=… hdr10=…` line shows what the
  stack offers; `scrgb=0 hdr10=0` means OS HDR is off for that display/mode
  (note: exclusive-fullscreen mode switches can drop a display out of HDR).

## Build

- CMake option `USE_SDLGPU` (default ON for desktop clients; forced OFF for
  `DEDICATED_SERVER` and `NINTENDO_SWITCH`, which have no SDL_GPU driver).
- New build dependency when ON: **glslang** (`mingw-w64-x86_64-glslang` on
  MSYS2) for the runtime GLSL→SPIR-V shader pipeline (used from Phase 1).
  `-DUSE_SDLGPU=OFF` builds exactly the old GL-only client.
- `USE_SDLGPU_STATIC` links the shader toolchain statically
  (`libglslang.a` + `libglslang-default-resource-limits.a` +
  `libSPIRV-Tools{,-opt}.a`, and routes spirv-cross to its `.a` fallback
  instead of `libspirv-cross-c-shared.dll`), so the shipped DLL set shrinks
  back to the pre-SDL_GPU four (SDL3, zlib1, libgcc_s_seh-1, libwinpthread-1
  — `libstdc++-6.dll` also drops out since the exe links `-static-libstdc++`).
  MSYS2 packages ship the static archives alongside the DLLs; the glslang
  CMake config only exports SHARED targets, hence the direct `find_library`
  on explicit `lib*.a` names. **Default ON for MinGW/Windows builds**, OFF
  elsewhere (distro static glslang archives aren't guaranteed; the
  DLL-shipping problem is Windows-specific). `-DUSE_SDLGPU_STATIC=OFF`
  restores the shared-DLL link (slightly faster links for dev iteration).
  Adds ~17 MB to the exe (measured, Debug). Note: an option default never
  overrides an existing build dir's cache — reconfigure with an explicit
  `-DUSE_SDLGPU_STATIC=ON` to flip a pre-existing dir.
- Sources: `port/fast3d/gfx_sdlgpu.{h,cpp}` (+ `gfx_sdlgpu_shader.cpp` from
  Phase 1). Bodies are `#ifdef USE_SDLGPU`; CMake also drops the files from
  the glob when OFF.

## Architecture (agreed plan; see plan file for full detail)

- **Backend seam:** `port/src/video.c` picks `gfx_sdlgpu_api` vs
  `gfx_opengl_api` and calls `gfx_sdl_set_backend(1)` so the shared SDL window
  manager (`gfx_sdl.cpp`) creates a plain window: no `SDL_WINDOW_OPENGL`, no
  GL context/attributes, no `SDL_GL_SwapWindow` (present happens in the
  renderer's `end_frame` command-buffer submit), and vsync forwards to
  `gfx_sdlgpu_set_vsync` (present modes: 0 = IMMEDIATE, >0 = VSYNC with the
  frame limiter pacing intervals > 1, <0 = MAILBOX where supported). The
  frame-pacing sleep and all fullscreen/display-mode/HiDPI/taskbar logic stay
  shared in `gfx_sdl.cpp`.
- **Shader strategy (Phase 1):** runtime-generate Vulkan-dialect GLSL 450 per
  combiner permutation (paralleling `gfx_opengl_create_and_load_new_shader`),
  compile with glslang → SPIR-V → `SDL_CreateGPUShader`. SDL_GPU SPIR-V
  binding convention: VS samplers set=0 (the dlcache palette!), VS UBO set=1,
  FS samplers set=2 (uTex0/1), FS UBO set=3. Later: SPIRV-Cross→HLSL→
  `D3DCompile` (system `d3dcompiler_47.dll`)→DXBC for D3D12, SPIRV-Cross→MSL
  source for Metal (untestable, best-effort).
- **Offscreen fb0 + final blit:** rapi framebuffer 0 is an internal offscreen
  texture, never the swapchain (swapchain textures are color-target-only).
  `end_frame` blits fb0 → swapchain and submits. Solves `copy_framebuffer
  use_back`, the GL↔Vulkan y-flip, and screenshots in one place.
- **Clip parameters:** `{ z_is_from_0_to_1 = true, invert_y = per-fb }` — z in
  [0,1] removes the GL `z *= 0.3` no-depth-clamp hack (rasterizer depth clamp
  instead). One `invert_y` value drives both the immediate path (gfx_pc clip-y
  negate) and the dlcache replay (folded into uMVP), so it only has to be
  right in one place.
- **Pipeline cache:** SDL_GPU bakes GL's dynamic state into immutable
  pipelines. Cache keyed by shader × blend mode × depth test/write/func ×
  depth-bias (ZMODE_DEC replaces `glPolygonOffset(-2,-2)`) × cull/front-face
  (mirror's `front_ccw`) × fill mode (wireframe) × target format × sample
  count. Viewport/scissor stay dynamic.
- **Uniforms:** push-style per command buffer; one VS block (uMVP, fog,
  palette enable/W/shade-route) and one FS block (frame_count, noise_scale,
  three-point flags, wireframe_color), dirty-tracked.
- **Command buffers:** one per frame; render passes end/re-begin at each
  `start_draw_to_framebuffer`, with lazy pass-begin so `clear_framebuffer`
  folds into the pass CLEAR load-op. Texture/buffer uploads accumulate and
  flush as a copy pass at pass boundaries (uploads always precede sampling).
- **dlcache (Phase 2):** real `cache_*` buffers (persistent vertex buffers,
  stride `(num_floats+1)*4` with trailing `aShadeIdx`) and the shade palette
  as a count×1 RGBA8 texture sampled in the **vertex** stage.

## Phases

| Phase | Content | Status |
|---|---|---|
| 0 | Seam, selection, CMake, device + magenta clear | done (verified) |
| 1 | Shaders, textures, pipelines, immediate path, framebuffers — full game render on Vulkan | done (verified; incl. front-buffer emulation + empty-scissor fixes) |
| 2 | dlcache + shade palette | done (verified; incl. the z_is_from_0_to_1 uMVP-fold fix in dlcacheReplay) |
| 3 | MSAA | done (verified) |
| 3b | disk SPIR-V shader cache | done (verified) |
| 4 | D3D12 (DXBC) + Metal (MSL) shader formats | implemented, awaiting build |

### D3D12 / Metal shader formats (Phase 4)

- glslang's SPIR-V stays the universal intermediate. For non-SPIR-V devices,
  SPIRV-Cross translates it: **DXBC** = SPIRV-Cross HLSL (SM 5.1) →
  `D3DCompile` (`d3dcompiler_47.dll`, loaded dynamically, ships with Windows
  10+) → `vs_5_1`/`ps_5_1` bytecode; **MSL** = SPIRV-Cross MSL source
  (entrypoint becomes `main0`). Requires building with spirv-cross
  (`GFX_SDLGPU_HAS_SPIRV_CROSS`, auto-detected by CMake) — without it the
  backend is Vulkan-only.
- Register mapping relies on our descriptor-set convention aligning 1:1 with
  SDL_GPU's D3D12 register spaces: VS t/s space0 + b space1, FS t/s space2 +
  b space3 — SPIRV-Cross SM 5.1 emits `register(xN, spaceM)` straight from
  the SPIR-V binding/set decorations. Vertex attributes map location →
  `TEXCOORD<location>`, SDL_GPU's DXBC convention.
- The disk cache stores the **final format blob** (DXBC bytes / MSL text) —
  the cache key carries a format tag, so switching drivers populates separate
  entries (cache file version 2).
- Metal is best-effort and untested (no Apple hardware here); its resource
  bindings use explicit SPIRV-Cross MSL remaps (buffers/textures/samplers by
  binding index).

### MSAA (Phase 3)

- `Video.MSAA` (1-16) clamps to the highest device-supported sample count
  (1/2/4/8, checked for both the colour and depth formats via
  `SDL_GPUTextureSupportsSampleCount`).
- An msaa fb's colour/depth textures carry the sample count (colour loses
  SAMPLER usage — multisample textures can't be sampled in SDL_GPU) and gain a
  single-sample `resolve` target. Pipelines are keyed on the target's sample
  count.
- `resolve_msaa_color_buffer` (same-size, the only case PD hits) is an empty
  render pass on the msaa colour with `RESOLVE_AND_STORE` straight into the
  destination's texture; STORE keeps the msaa contents valid for the
  `copy_framebuffer(use_back)` pause-blur read, which resolves through
  `fb_readable_color` into the fb's resolve target before blitting.

### dlcache mapping (Phase 2)

- `cache_create_buffer` → persistent VERTEX `SDL_GPUBuffer`, one-shot upload on
  the upload CB (`cycle=false`, fresh buffer); 0 on failure → leaf bad →
  legacy path. Deletions are graveyarded (`dead_buffers`) since the frame's
  recorded draws may still reference the buffer.
- `cache_draw` → binds the cached buffer at `base_float*4`, resolves a
  *cached* pipeline (stride `(num_floats+1)*4`, `aShadeIdx` attribute, the
  cached VS variant, cull/front-face from `cache_set_cull`), binds the palette
  as a **vertex-stage** sampler (set=0) when the variant uses it, then draws.
  Cached draws stay solid under `/wireframe` (GL parity — glPolygonMode only
  wraps draw_triangles there).
- Cached VS variant: compiled lazily on first `cache_draw` per shader; aliases
  the immediate VS when the permutation has no combiner inputs (only the
  pipeline stride differs). Palette lookup is `texelFetch` + the 3-bit
  per-input uShadeRoute routing, identical to GL's `palette_supported` path.
- Palettes: count×1 RGBA8 textures; re-uploads allocate a fresh texture (same
  mid-frame ordering rule as regular textures); `cache_bind_palette` also
  drives `uPaletteW` in the VS uniform block.

## Architecture notes as implemented (Phase 1)

- **Two command buffers per frame** (`upload_cb`, `render_cb`): texture and
  vertex uploads record copy passes on the upload CB as they occur mid-frame;
  draws/blits record on the render CB; end_frame submits upload-then-render,
  so uploads always execute before the draws that sample them. This sidesteps
  "no copy passes inside render passes" without forced pass breaks.
- **Re-uploads allocate a fresh GPU texture** (the old one is graveyarded):
  draws recorded earlier in the frame keep sampling the old contents, exactly
  matching GL's mid-frame upload ordering. gfx_pc never calls delete_texture —
  it pools ids and re-uploads over them — so this is the only ordering case.
- **Vertex streaming**: one 24 MB per-frame vertex buffer + transfer buffer;
  draw_triangles appends into the mapped transfer buffer and binds the vertex
  buffer at the byte offset; a single bulk copy at end_frame (cycle=true on
  both map and copy avoids stalls on in-flight frames).
- **clear_framebuffer = immediate empty render pass** whose load-ops do the
  clearing (full-target, like GL's scissor-disabled glClear); regular passes
  use LOAD/STORE. Freshly created attachments are "virgin" and clear on first
  use so nothing ever loads garbage.
- **Viewport/scissor** arrive from gfx_pc in GL bottom-left window coords and
  are flipped to top-left here using the current target's height; scissor is
  clamped into the target (Vulkan requires non-negative offsets).
- **Depth**: format is D32_FLOAT (with fallbacks); ZMODE_DEC uses rasterizer
  depth bias (-2, -2) replacing glPolygonOffset; `enable_depth_clip = false`
  replaces GL_DEPTH_CLAMP (so the GL `z *= 0.3` shader hack is gone); depth
  compare ops mirror the GL switch (INTER/OPA/XLU/DEC, depth_source_prim).
- **Pipeline key**: shader × blend(off/alpha/modulate) × depth test/write/func
  × depth-bias × fill mode (wireframe) × has-depth. Immediate path always
  culls NONE (gfx_pc culls on the CPU), so `/mirror` works unchanged.
- **Front-buffer emulation**: `copy_framebuffer(use_back=false)` on the main
  fb means `glReadBuffer(GL_FRONT)` in GL — the previously presented frame.
  fb0 is snapshotted into `front_tex` at the end of every frame (after all
  passes), and main-fb copies with `use_back=false` read the snapshot. Without
  this the pause-menu blur / menu backgrounds blur the freshly cleared current
  frame → black.
- **Vertex-buffer cycling rule** (learned the hard way): SDL_GPU `cycle`
  resolves at the *record point* of the call requesting it. Binds recorded
  earlier in the frame keep referencing the pre-cycle backing, so cycling the
  end-of-frame vertex upload made every draw read stale data (corrupt
  geometry, periodically correct frames). The streaming buffers are instead a
  manual 3-deep ring (`GFX_SDLGPU_VTX_RING`) with `cycle=false` on the upload;
  only the transfer-buffer *map* cycles (its consumer records after the map).
  Rule of thumb: only cycle a resource if ALL its references record after the
  cycling call.

### Disk SPIR-V shader cache (Phase 3b)

- `<home>/shadercache_sdlgpu.bin` (the SDL pref path, next to pd.ini): one
  packed append-only file — magic + version header, then records keyed by
  (shader_id0, shader_id1, filter_mode, variant). On a hit the glslang compile
  (the cold-start hitch) is skipped; the GLSL text generation always runs (it
  also derives the program metadata and costs microseconds).
- Variant 1 records are the dlcache cached-VS (filter-independent → keyed with
  filter 0xff, fs blob empty).
- Corrupt/old-version/truncated files are compacted-rewritten on load; delete
  the file or run with `--no-shader-cache` to bypass.
- **Bump `SHADER_CACHE_VERSION` (gfx_sdlgpu_shader.cpp) whenever shader
  codegen changes**, or stale SPIR-V will be loaded for the new generator.

## Known cosmetic differences (by design)

- Noise/dither (`frame_count`/`noise_scale` hash) won't be byte-identical to
  GL: `gl_FragCoord` origin differs (GL bottom-left vs Vulkan top-left). Looks
  like dithering either way.

## Gotchas

- `gfx_sdlgpu.cpp` defines its own opaque `struct ShaderProgram` like every
  fast3d backend; never share `ShaderProgram*` across backends.
- `gfx_sdlgpu_probe()` creates the device early (before the window) and the
  rapi `init()` reuses it — don't create a second device.
- `Video.VSync` intervals > 1 have no SDL_GPU equivalent; they present as
  VSYNC and rely on the existing frame limiter for pacing.
