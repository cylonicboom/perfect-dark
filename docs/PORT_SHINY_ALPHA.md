# Shiny-Surface Darkness Fade Fix (`/shinyalpha`)

Port-only fix for the long-open "shiny surfaces turn see-through" family
(first logged 2026-06 as the dataDyne Extraction night-vision-section
regression): env-mapped ("shiny") room surfaces — black shiny metal, marble,
etc. — became progressively transparent as room lights dimmed, and fully
invisible in blacked-out rooms, letting the always-on sky room (Extraction /
Defection / MBR draw room 1 behind everything) show through walls.

**Status: runtime-CONFIRMED 2026-07-18** (Extraction dark section: metal
solid, glass correct, lit levels unchanged).

## Mechanism (why it happened)

- The per-frame room reshade (`dlights.c`, the colour-palette rebuild) treats
  vertices with **`vtx.flags & 0x01` (the env/shiny class)** specially: RGB
  is kept as authored, but **alpha = authored alpha × settled regional room
  brightness / 255**. Blacked-out room → alpha 0.
- fast3d feeds that palette alpha to the surface's SHADE_ALPHA blend
  (`gfx_sp_vertex`: `d->color.a = vcn->a`) → alpha 0 renders fully
  transparent.
- Explains the old ledger mysteries: muzzle flashes never fixed it
  (`br_flash` only brightens the *non*-shiny class RGB; shiny alpha tracks
  settled brightness only), and goggles changed it (NVG forces brightness to
  `var8009caec` = 0xbc, set by `bviewDrawNvLens`).
- The likely true N64 divergence (unproven, not implemented): these metal
  surfaces sit in the **opaque render layer**, and the N64 blender draws
  opaque rendermodes solid regardless of shade alpha — fast3d applies the
  alpha as transparency everywhere. The fix below achieves the same visible
  result at the data level instead of the blender level.

## The fix

Two pieces, both port-guarded:

1. **`bgBuildShinyXluMask(roomnum)`** (`bg.c`, run at room load after
   `bgFindRoomVtxBatches`, freed in `bgUnloadRoom`, stored in the port-only
   `g_Rooms[].shinyxlumask` field): walks the room's **XLU-layer** leaf gdls
   (`bgGetNextGdlInLayer` / `bgFindVerticesForGdl`, tracking the `G_COL`
   colour-palette base per leaf) and bitmasks every colour index referenced
   by translucent geometry. This is the glass-vs-metal discriminator —
   authored alphas can't split them (both author 255; verified with
   `/shinyalpha info` histograms).
2. **The floor** (`dlights.c` reshade, flag-0x01 branch): colour entries
   **not** in the xlu mask get their faded alpha floored at
   `authored × g_RoomShinyAlphaFloor / 255`. Default **255** = opa-layer
   shiny surfaces never fade at all (solid at any light level). XLU (glass)
   entries always keep the vanilla fade, so glass reflections still dim to
   nothing with darkness — the designed look.

## Controls

- `/shinyalpha [0-255]` — live floor value. `0` = vanilla full fade-out
  (reproduces the original bug), `255` = default. Persists via
  **`Video.ShinyAlphaFloor`** (pd.ini).
- `/shinyalpha info` — for the current room: histogram of authored shiny
  alphas (`roomShinyAlphaDebug`, dlights.c) + per-layer shiny vertex counts
  (`bgShinyLayerStats`, bg.c). The diagnostic that drove the fix; keep for
  future rooms that misbehave.

## Honest limits / gotchas

- Glass reflections vanish entirely in fully dark rooms — that is vanilla
  behaviour (alpha scales with brightness), deliberately preserved. If a
  future report wants faint dark-room glass sheen, apply a small floor to
  masked (xlu) entries too.
- The reshade's flag test indexes `vertices[i].flags` by **colour** index —
  a decomp-documented original-game quirk (`@bug` comment). The mask is
  computed from real gdl references and only ever *narrows* the floor, so
  the quirk is preserved, not amplified.
- A colour entry shared by both opa and xlu geometry counts as xlu (fades).
- `memset` is not declared in `bg.c` (decomp unit) — the mask is zeroed with
  a loop; keep it that way.
- N64 build byte-identical (all changes `#ifndef PLATFORM_N64`).

## Read when

Touching the dlights.c room reshade, `bgGetNextGdlInLayer`/room gdl parsing,
the `G_COL`/`SPSEGMENT_BG_COL` palette path, or investigating any surface
that renders transparent when rooms darken.
