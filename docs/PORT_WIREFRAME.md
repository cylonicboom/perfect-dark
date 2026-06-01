# Port-only Rendering Feature: Wireframe Mode

A port-only developer/visual cheat that draws the 3D scene as polygon outlines instead
of filled triangles. Useful for inspecting geometry, culling, and LODs. Exposed as a
cheat only (Cheats → Gameplay → **Wireframe**), always unlocked. Implemented entirely
under `#ifndef PLATFORM_N64` and inside the in-tree fast3d renderer — the N64 build is
unchanged.

It is purely local and visual: **no wire-format change, no save-format change, no
`NET_PROTOCOL_VER` bump.** Scope is **depth-tested 3D geometry only** (level, props,
characters, first-person weapon); the HUD, menus, crosshair, and 2D sprites stay solid
and readable.

> Sibling of the No Room Culling / No Draw Slot Limit feature
> ([`PORT_NO_CULLING.md`](PORT_NO_CULLING.md)) — same cheat plumbing, same per-frame
> sync mechanism as `CHEAT_NOCULL`.

---

## Mechanism

`CHEAT_WIREFRAME` (always-unlocked cheat id) → recomputed once per in-stage frame in
`bgTickPortals` via `cheatIsActive(CHEAT_WIREFRAME)` → writes renderer global
`gfx_wireframe_mode` → in the OpenGL backend's triangle draw, when `gfx_wireframe_mode`
is set **and** the current draw is depth-tested **and** the context is not GL ES, the
draw is wrapped in `glPolygonMode(GL_LINE)` / restored to `GL_FILL`.

Tracking the active state per-frame (the `CHEAT_NOCULL` pattern) rather than flipping a
flag in `cheatActivate`/`cheatDeactivate` (the `CHEAT_NODRAWLIMIT` pattern) means the
renderer global always reflects the live cheat state and never gets stuck after the
cheat is turned off.

---

## `src/include/constants.h`

- `CHEAT_WIREFRAME 46` — appended after `CHEAT_GOLDENEYE 45`. Append-only; never insert
  mid-array (cheat ids are indices into `g_Cheats[]`). Lives in bank 1 (cheats 32–63),
  bit 14 — well within range; no save-format impact (it's one more bit in an existing
  `u32`, zero on old saves).

## `src/game/cheats.c`

- `g_Cheats[]` table entry `{ 0, 0, 0, 0, CHEATFLAG_ALWAYSUNLOCKED }` (in the
  `#ifndef PLATFORM_N64` block, after the GoldenEye entry).
- `s_cheat_literal_names[CHEAT_WIREFRAME] = "Wireframe"` (the menu label; port-only
  cheats use literal names instead of lang string ids).
- `g_CheatsGameplayMenuItems[]` checkbox entry (after the GoldenEye checkbox), using the
  shared `cheatGetNameIfUnlocked` label fn and `cheatCheckboxMenuHandler` toggle handler.

No `cheatActivate` / `cheatDeactivate` case — unlike `CHEAT_NODRAWLIMIT`, state is synced
per-frame in `bg.c`.

## `src/game/bg.c`

- `extern unsigned char gfx_wireframe_mode;` forward-decl alongside the existing
  port-only `g_BgNoCull` / `g_BgNoDrawSlotLimit` globals. **Not `bool`** — see the
  gotcha below.
- In `bgTickPortals`, beside the `g_BgNoCull` recompute:
  `gfx_wireframe_mode = cheatIsActive(CHEAT_WIREFRAME) ? 1 : 0;`

### Gotcha: `bool` is `s32` in game code, 1 byte in the renderer

`src/include/types.h` (and `data.h`) do `#define bool s32`, so in `bg.c` (and all
decompiled game code) `bool` is **4 bytes**. The renderer's `gfx_wireframe_mode` is a
C++ `bool` — **1 byte**. The original implementation declared the game-side extern as
`bool` and assigned `cheatIsActive(CHEAT_WIREFRAME)` directly, which broke in two
compounding ways:

1. A 4-byte store through a 1-byte symbol (type/size mismatch, corrupts neighbours).
2. `cheatIsActive` returns the **raw masked bit**, not 0/1. `CHEAT_WIREFRAME` is bit 14
   of bank 1, so it returns `0x4000`. Little-endian, the low byte the renderer reads is
   `0x00` → wireframe never enabled (the build looked completely unchanged).

`CHEAT_NOCULL` never hit this because its consumer `g_BgNoCull` is also game-side `s32`,
so the full 4-byte truthy value is preserved. The bug only surfaces when a 1-byte
renderer global reads a value written by 4-byte game `bool`. Fix: declare the game-side
extern as a 1-byte type (`unsigned char`) and normalize the value with `? 1 : 0`. The
`gfx_framebuffers_enabled` / `gfx_detail_textures_enabled` globals avoid this only
because their sole writer, `video.c`, deliberately does **not** include `types.h`, so
its `bool` is the 1-byte `<stdbool.h>` `_Bool`.

## `port/src/net/net.c` — `/wireframe` console command

`/wireframe [on|off]` (alias `/wf`) toggles the cheat live with no stage reload — handy
since, like every cheat, the menu checkbox only takes effect at the next `cheatsReset`
(stage load). It flips the cheat's **active + enabled** bits in `g_CheatsActiveBank1` /
`g_CheatsEnabledBank1`; `bgTickPortals` then pushes the state into `gfx_wireframe_mode`
each in-game frame. No arg = toggle. Routed through `netConsoleCommand`, so it works
outside a net session (open the console with `~`). It deliberately does **not** write
`gfx_wireframe_mode` directly — net.c also has `bool == s32`, so going through the cheat
bit + `bgTickPortals` keeps the 1-byte store in one place.

## `port/fast3d/gfx_api.h` / `port/fast3d/gfx_pc.cpp`

- `extern bool gfx_wireframe_mode;` (header) / `bool gfx_wireframe_mode = false;`
  (definition). Mirrors the existing `gfx_framebuffers_enabled` / `gfx_detail_textures_enabled`
  globals — a plain (unmangled) global, so C game code can write it directly.

## `port/fast3d/gfx_opengl.cpp`

- `static bool s_wireframe_depth_test` caches the most recent depth-test state, recorded
  at the top of `gfx_opengl_set_depth_mode`. 2D HUD/menus draw with depth test off, so
  this is `false` for them.
- `gfx_opengl_draw_triangles` wraps `glDrawArrays` in
  `glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)` / `…GL_FILL` when
  `gfx_wireframe_mode && s_wireframe_depth_test && !gl_es`.

---

## Limitations

- **GL ES only**: `glPolygonMode` is desktop-GL only; on GL ES targets (e.g. Switch)
  wireframe is a silent no-op. Desktop builds (Windows/Linux/macOS, compatibility or core
  profile) work.
- Lines are drawn with each surface's existing textured/shaded fragment color (a
  "natural" wireframe), not a forced flat line color. Forcing a flat color would require
  a shader uniform/variant and is intentionally out of scope.
- Like all cheats, a mid-stage menu toggle takes effect at the next stage load
  (`cheatsReset`); `bgTickPortals` then tracks it live each frame.
