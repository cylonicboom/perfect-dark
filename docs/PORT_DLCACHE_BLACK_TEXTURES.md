# DLCache "Black / Missing Textures" Investigation Ledger

Session notes (2026-06-17) for the display-list-cache rendering-corruption family.
Two **distinct** bugs surfaced for **one user**; the developer could **not reproduce
either** (same build) → both are environment/driver-specific. One is **fixed**, one is
**open**. Read alongside `docs/PORT_DLCACHE.md`.

> TL;DR: a user on a **2023 GPU driver** sees, with `/dlcache on` only:
> - **Vulkan/SDL_GPU:** cached walls missing (see-through to skybox) — **FIXED** (cull winding).
> - **OpenGL:** base-game wall textures render **black** — **OPEN** (cull-immune, not winding/texture-pack/eviction).

---

## BUG A — Vulkan/SDL_GPU cached geometry culled — FIXED

**Symptom:** whole walls missing on the cached path — black void / see-through to
skybox. Floor + ceiling fine. Cached leaf, not painted black — *geometry not drawn*.

**Root cause:** the cached path uses **GPU face-culling** with a front-face winding
(`g_DlCacheFrontCcw`, default CCW); the immediate path CPU-culls and is immune. That
user's 2023 driver culls the cached front faces CCW keeps → walls vanish. The
developer's driver is fine with CCW → **driver-dependent**, a fixed flip can't satisfy
both.

**Confirmation chain (all by the user, at a fixed repro spot):**
- `/dlcache off` → correct (bounds it to the cache).
- `tex 104/4096` in the overlay → **not** texture-cache eviction.
- `BAD 0` → not an aborted leaf.
- `/dlcache cull off` (draw both faces) → **fixed** ⇒ it was a *missing face* (cull), not a black texture.
- `/dlcache ff` (flip winding, cull still **on**) → **also fixed** ⇒ **pure winding mismatch**, not cull-vs-no-cull. Culling + its perf can stay on.

**Fixes shipped (per-machine overrides; defaults unchanged so other machines untouched):**
- `Video.DlCacheFrontFaceGL|GPU = ccw|cw` (per-renderer — Vulkan reverses winding vs GL) (pd.ini) — preferred (keeps culling + perf).
- **Extended > Video → "DL Cache Flip Winding"** checkbox — same backing store, applies live.
- `Video.DlCacheCull = auto|off|back|front` (pd.ini) — the cull-off escape, also persisted.
- Console: `/dlcache ff`, `/dlcache cull off` (live, non-persistent).

**Backend scope:** the winding global is shared, read by every backend's cull setup
(`gfx_opengl_cache_set_cull` glFrontFace; `gfx_sdlgpu` pipeline `front_face`, which is
driver-agnostic — Vulkan **and D3D12 and Metal**). So the toggle is a universal
"missing/culled cached geometry" fix. It is **one global, not per-renderer** — if a
machine needs different windings per backend, re-toggle on `Video.Renderer` change.

---

## BUG B — OpenGL base-game texture renders black — OPEN

**Symptom:** specific **base-game** wall textures render **pure black** via the cached
path on OpenGL. Geometry is **present and solid** (not see-through).

**What is RULED OUT (and how):**
| Ruled out | Evidence |
|---|---|
| Culling / winding | **cull-immune** — `/dlcache cull off` (both faces) does **not** fix it; geometry is solid, not missing |
| Shader-side palette / live shade | `/dlcache palette off` (verified it printed `GPU palette = OFF`) → still black |
| HD / external texture pack | `Video.ExternalTextures=0` doesn't fix; affected textures are **base game** |
| Texture-cache eviction | overlay `tex 104/4096` — nowhere near the 4096 cap |
| Aborted leaf (lighting/texgen/cullboth) | `BAD 0` |
| Generic vertex/stride bug | geometry is correct ⇒ positions right ⇒ texcoords (same buffer/stride) right ⇒ it's the **texture bind / combiner**, not vertex data |
| Renderer-specific code | red herring — same user breaks on **both** GL (black) and Vulkan (see-through); dev fine on both. It's their **environment** |

**Still true:** `/dlcache off` = correct; `/dlcache on` = black; `/dlcache clear`
(re-record, recording stays on) reproduces; only **stopping** recording clears it →
the corruption is in the cached **data/draw** for that leaf, reproduced every record.
**Dev cannot reproduce on either renderer** (same build) → user's **2023 GL driver**.

**Leading hypothesis:** like Bug A, a quirk of the old GL driver in a dlcache-specific
GPU usage — but a **texture/combiner** feature, not culling. Candidates, by suspicion:
1. **Vertex-shader texture sampler** (`uPalette`) is declared in **every** desktop GL
   shader. `palette_supported` (gfx_opengl.cpp) gates only on `!gl_es && gl_glsl_version
   >= 130` — it does **not** check `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS`. A GPU with 0
   vertex texture units would have the VS sampler declared anyway. *Counter-evidence:*
   palette-OFF is still black, so the sampling itself isn't the trigger — but merely
   *declaring* a VS sampler can misbehave on some drivers (the code already binds a
   dummy 1×1 to dodge draw rejection; this driver may need more).
2. The extra `aShadeIdx` vertex attrib that `cache_draw` enables.
3. Persistent-VBO / custom-stride texture-coordinate handling on that driver.

**NEXT STEPS (need from the affected user):**
- Exact **GPU + driver version**, and the **GL version / renderer string** (boot log).
- **`GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS`** value — add a one-line boot log for it.
- **Screenshot** of the GL black spot.
- Code probes to try:
  - Make `palette_supported` also require `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS > 0`
    (graceful degrade — also matters for OG Xbox below).
  - Temporarily **stop declaring** the VS `uPalette` sampler / `aShadeIdx` when the
    palette path is unsupported, and see if the GL black clears → implicates the VS
    sampler declaration.
  - Log the cached segment's bound texture id vs the immediate path's for that surface.

**Workaround for the user now:** run **Vulkan** (`Video.Renderer=sdlgpu` + DL Cache Flip
Winding **On**) — fully working. On OpenGL, `/dlcache off` (byte-identical to a
non-cached build).

---

## BUG C — Vulkan/SDL_GPU per-vertex cache glitches — OPEN

**Symptom (screenshots, 2026-06-17):** with `/dlcache on` and the **correct** winding
(toggle off / ccw — the default is right for this machine's Vulkan), the level is
mostly correct but a *few* vertices per area are wrong: **stretched/smeared textures**
(a vertex UV way off — "stretched one way, repeating the other"), **mis-coloured
vertices** (e.g. a vivid blue bleed on a stone wall), and the occasional **missing
vertex**. `BAD 0`, `TEX` well under cap. **Intermittent — moving away and back fixes
it** (re-record / colour-dirty re-record settles it).

**Important correction to Bug A's story:** on this machine the correct Vulkan cull
winding is the **default ccw** (enabling the flip = all back faces). The earlier
"`/dlcache ff` fixed Vulkan" reading didn't survive later builds — treat Bug A's fix
as "persist the winding the machine wants," not "Vulkan always wants cw."

**Audited and RULED OUT (SDL_GPU cache path is correct on paper):**
- Buffer upload — whole `memcpy` of `num_floats*4`, correct (`gfx_sdlgpu_cache_create_buffer`).
- Vertex **pitch** — `(num_floats+1)*4`, correct (the trailing `aShadeIdx`).
- Vertex **attribute offsets** — `aShadeIdx` at offset `num_floats`, matches the record side.
- Not winding/cull (correct winding set), not eviction (`TEX` low), not `BAD`.

**Leading read:** same family as Bug B — the **2023 driver mishandling the GPU-resident
cache**, here as transient per-vertex glitches the code path doesn't explain. Dev can't
repro. Intermittent + self-healing + driver-specific = a driver/timing quirk, not a
clean code bug.

**Next probe if resumed:** `/dlcache palette off` on the *colour* glitch — if the blue
bleed clears, the colour **index** (`aShadeIdx`) is being misread for some vertices
(implicates the trailing-float attribute on that driver); the UV stretch is separate.

---

## Verdict for this machine + the off-switch

The dlcache mis-renders on this user's 2023 driver on **both** backends (Bug B black
GL textures, Bug C Vulkan per-vertex glitches), with the data path correct on paper.
The cache is **on by default** (`g_DlCacheEnabled = true`, `bg.c`). Shipped a
persistent **off-switch** so affected hardware can opt out cleanly:
- `Video.DlCache = 0` in `pd.ini`, or **Extended > Video → "Display List Cache"** (off),
  or `/dlcache off`. Off is **byte-identical to a non-cached build** — zero glitches.

Recommend this user run with the cache **off** until/unless the driver-class issues are
solved. Same recommendation pre-emptively for the OG Xbox target below.

---

## Diagnostics / levers added today (all in `docs/PORT_DLCACHE.md`)
- `/dlcache palette [on|off]` — isolate the shader-side shade path (baked vs live).
- `/texcache N` + `Video.TextureCacheSize` + `tex used/max` in `/dlcache stats` and the
  Lua overlay — texture-cache fill readout (ruled eviction out here).
- Runtime texture-cache count cap (`g_TextureCacheMaxSize`).

## Files in play
| File | Role |
|---|---|
| `port/fast3d/gfx_pc.cpp` | shared cache record/replay — **audited, correct** (so a backend/driver bug, not here) |
| `port/fast3d/gfx_opengl.cpp` | GL `cache_draw` / `cache_set_cull` / palette VS — **Bug B lives here or in the driver** |
| `port/fast3d/gfx_sdlgpu.cpp` | SDL_GPU cache — Bug A winding (`pipeline_resolve` `front_face`, ~L687-690) |
| `port/src/video.c` | `Video.DlCacheCull` / `DlCacheFrontFace` / `TextureCacheSize` + `videoGet/SetDlCacheFlipWinding` |
| `port/src/optionsmenu.c` | "DL Cache Flip Winding" checkbox (`menuhandlerDlCacheWinding`) |

---

## OG Xbox / NXDK / SDL2 relevance (for the planned port)

- **NXDK is SDL2**; this port is **SDL3 + SDL_GPU**. The SDL_GPU (Vulkan/D3D12/Metal)
  backend won't exist on OG Xbox — you'd use the **OpenGL** backend (or a GL-ES-class
  path). So Bug B's territory (the GL cache path on a limited driver) is *exactly* what
  an Xbox port will live in.
- **OG Xbox GPU = NV2A (2001), fixed-function era.** Expect **no** modern GLSL, **no**
  vertex-shader texture units, **no** `texelFetch` / integer shader ops. The dlcache
  **palette path** (VS sampler + integer shade routing, needs GLSL ≥ 130) will be
  unsupported → `cache_create_palette` returns 0 → re-record fallback. The `uMVP` +
  persistent-VBO core *might* work at a high enough GL feature level, but is risky.
- **Treat Bug B as a preview** of the low-end-GL problem class. Plan for the dlcache to
  be **default-off / unsupported** on OG Xbox, with the immediate path as the safe
  baseline (`/dlcache off` is byte-identical to a non-cached build, so this is clean).
- **Do** add the capability guards from Bug B's "next steps" (the
  `GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS` check, graceful sampler/attrib degrade) — they
  benefit both the 2023-driver user *and* the Xbox target.
