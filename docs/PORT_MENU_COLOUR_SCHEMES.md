# Port-only: Menu Colour Schemes (how to author **hardcoded** ones)

How the menu colour system works and how to add **hardcoded** (compile-time,
non-user-selectable) menu colour schemes. The selectable "Menu Colour Scheme"
dropdown (Extended > Experiments, commit `0406b9382`) is the template; this guide
shows how to bake a scheme in instead of exposing it.

All of this is **port-only** (`#ifndef PLATFORM_N64`); the N64 path stays
byte-identical.

> **A working reference scaffold is already in the tree** (added alongside the
> selectable feature, off by default):
> - `MENU_HARDCODE_SCHEME` — a compile-time `#define` in `src/game/menu.c`. Leave
>   it undefined (default) and the runtime dropdown is used unchanged; build with
>   `-DMENU_HARDCODE_SCHEME=N` (or uncomment the line) to bake scheme `N` in with
>   no user toggle. Both dispatch functions read the effective scheme through
>   `menuActiveColourScheme()`.
> - **Scheme 4 = "Amber"** — a worked **custom-hue palette row** (the Recipe-2
>   example): row index `6` / `MENUDIALOGTYPE_AMBER`, appended port-only to all
>   three palette tables, wired into `menuApplyColourScheme`'s `schemerows[]` and
>   `menuSchemeColour`'s `case 4`. It is intentionally **not** in the Experiments
>   dropdown — it's reachable only via `MENU_HARDCODE_SCHEME=4`, demonstrating a
>   hardcoded-only scheme. Tune the 15 amber field values to taste.
>
> **A worked SELECTABLE custom-hue scheme also ships: "Redvox57"** (scheme
> index 5, palette row `MENUDIALOGTYPE_REDVOX57` = 7). It is teal base
> `#02f5c4` + purple highlight `#7c02f5` (the highlight drives `dialog_border2`,
> `listgroup_headerfg` and the `menuSchemeColour` accent — i.e. menu edges and
> bars, mirroring how the blue scheme uses a brighter blue there). It's in the
> Experiments dropdown via a **decoupled index→value map** in
> `menuhandlerMenuColourScheme` (`{0,1,2,3,5}`) so the hardcode-only Amber
> (scheme 4) stays hidden while Redvox57 (5) is selectable. Use it as the
> template for adding another selectable custom-hue scheme.
>
> **Adding a selectable scheme is now a near-one-liner** thanks to the
> `MENUSCHEMEROW(rb,gb,bb, rh,gh,bh)` macro in `menu.c`: it derives a full
> 15-field palette row from a **base** colour + a **highlight** colour
> (mirroring the blue row's structure — base drives the chrome + a *lightened*
> readable item text; highlight drives `border2`/`listgroup_headerfg` and the
> accent). To add one: append a `MENUSCHEMEROW(...)` to the `MENUSCHEME_ROWS`
> list + a `MENUSCHEMEWAVE*` to the wave lists, a `MENUDIALOGTYPE_*` row index,
> a `schemerows[]` entry, the highlight in `menuSchemeColour`'s `accents[]`
> table, and the name+value in the dropdown (`optionsmenu.c`) — then bump the
> `Game.MenuColourScheme` config max. Ten stock schemes ship this way
> (Sunburst, Fuchsia, Umber, Midnight, Denim, Frost, Glacier, Matrix, Rose,
> Peach). `menuSchemeColour` is data-driven from `accents[]` (0xRRGGBB scaled
> by intensity), so the accent always matches the row's highlight.
>
> The sections below explain the architecture and how to add your own.

---

## 1. Where menu colours come from (two layers)

A menu dialog's colour is **not** a single value — it comes from two places:

### Layer A — the palette tables (the dialog chrome)

Three parallel tables in `src/game/menu.c`, each an array of
`struct menucolourpalette` indexed by **`dialog->type`**:

| Table | Used for |
|---|---|
| `g_MenuColours[]` | the dialog border / title / body / item-state / list-header colours |
| `g_MenuWave1Colours[]` | title-text wave gradient (colour 1) |
| `g_MenuWave2Colours[]` | title-text wave gradient (colour 2) |

`struct menucolourpalette` (`src/include/types.h`) is **15 `u32` colour fields in
`0xRRGGBBAA` order**:

```
dialog_border1, dialog_titlebg, dialog_border2, dialog_titlefg, dialog_bodybg,
unused14, item_unfocused, item_disabled, item_focused_inner,
checkbox_checked_unfocused, item_focused_outer, listgroup_headerbg,
listgroup_headerfg, unused34, unused38
```

The tables currently have **6 rows**, indexed by `dialog->type`:

| Index | `MENUDIALOGTYPE_*` (constants.h) | Look |
|---|---|---|
| 0 | (type 0 — neutral / title / transition) | dark grey |
| 1 | `MENUDIALOGTYPE_DEFAULT` | **blue** (the normal menu) |
| 2 | `MENUDIALOGTYPE_DANGER` | **red** (confirm/abort dialogs) |
| 3 | `MENUDIALOGTYPE_SUCCESS` | **green** (success dialogs) |
| 4 | `MENUDIALOGTYPE_4` | white |
| 5 | `MENUDIALOGTYPE_WHITE` | grey |

The renderer reads these via the `MIXCOLOUR(dialog, property)` macro
(`constants.h`) and direct `g_MenuColours[dialog->type].field` accesses in
`menugfx.c` / `menu.c`. During an open/close transition it blends
`g_MenuColours[dialog->type2]` → `g_MenuColours[dialog->type]` by
`dialog->colourweight`.

### Layer B — hardcoded accent colours (`menuSchemeColour`)

A few menu accents were **hardcoded blue** in `menugfx.c` and are *not* in the
palette tables — the slider marker + gradient line, the dropdown-row background
tint, and the list-group header bars. The port routes those through
`menuSchemeColour(intensity)` (`src/game/menu.c`) so they follow the scheme too.

```c
// Returns 0xRRGGBB00 with the given intensity placed on the scheme's hue.
// The caller ORs in the alpha byte. Default (blue) == intensity << 8.
u32 menuSchemeColour(u32 intensity);
```

**A scheme is therefore two things:** (A) which palette *row* the normal (blue)
dialogs use, and (B) the accent hue from `menuSchemeColour`. Both must agree or
the chrome and the accents will clash.

---

## 2. The dispatch points (what reads the scheme)

Two functions in `src/game/menu.c` are the whole mechanism (declared in
`src/include/data.h` under `#ifndef PLATFORM_N64`):

```c
s32 menuApplyColourScheme(s32 type);   // remaps DEFAULT -> the scheme's palette row
u32 menuSchemeColour(u32 intensity);   // the accent hue
```

`menuApplyColourScheme` is called at exactly two sites so a scheme applies at
open time **and** keeps re-applying through per-frame transitions:

- `menuOpenDialog` (`menu.c`, ~line 1559): `dialog->type = menuApplyColourScheme(dialog->type);`
- `dialogTick` (`menu.c`, ~line 4235): `transitiontotype = menuApplyColourScheme(transitiontotype);`

Both **only remap `MENUDIALOGTYPE_DEFAULT`** — danger/success/white dialogs keep
their semantic colours. Don't change that: recolouring DANGER would make confirm
prompts indistinguishable.

The current selectable implementation switches both functions on the runtime
global `g_MenuColourScheme` (0..3), which the dropdown sets and `pd.ini` persists.
**For hardcoded schemes you replace that global with a compile-time constant and
drop the dropdown + config registration.**

---

## 3. Recipe 1 — a hardcoded scheme that reuses an existing row (quick)

If your scheme is one of the hues that already exists as a palette row
(red/green/white), you don't need a new palette — just bake the selection.

1. **Replace the runtime global with a compile-time constant.** In `menu.c`,
   change

   ```c
   s32 g_MenuColourScheme = 0;
   ```

   to a fixed value (or a build `#define`):

   ```c
   // 0 = Perfect (blue), 1 = Shinku (red), 2 = Complete (green), 3 = Missing (white)
   #ifndef MENU_COLOUR_SCHEME
   #define MENU_COLOUR_SCHEME 1   // <-- bake the scheme here (e.g. red)
   #endif
   static const s32 g_MenuColourScheme = MENU_COLOUR_SCHEME;
   ```

   `menuApplyColourScheme` / `menuSchemeColour` already switch on
   `g_MenuColourScheme`, so they now resolve at compile time. You can pick the
   build's scheme with `-DMENU_COLOUR_SCHEME=2` without editing source.

2. **Remove the user-facing surface** so it isn't selectable:
   - delete the `Game.MenuColourScheme` line in `port/src/main.c`
     (`configRegisterInt`), and
   - delete the dropdown item in `port/src/optionsmenu.c`
     (`g_ExtendedExperimentsMenuItems` entry + `menuhandlerMenuColourScheme`).

   Keep `g_MenuColourScheme` referenced only by the two dispatch functions.

That's it — the menus are now permanently the chosen hue, with no menu entry and
no config key.

---

## 4. Recipe 2 — a hardcoded scheme with a brand-new custom hue (full control)

For a colour that isn't one of the four existing rows (e.g. amber, purple,
teal), author a **new palette row** and point the scheme at it. This is the only
way to control the full dialog chrome (borders, title bg/fg, body bg, item
focus/disabled, list headers), not just the accent.

1. **Add one row to ALL THREE tables**, keeping them the same length. Append a
   new index (here `6`) to `g_MenuColours[]`, `g_MenuWave1Colours[]`, and
   `g_MenuWave2Colours[]` in `menu.c`. Remember `g_MenuColours` has a
   `#if VERSION >= VERSION_JPN_FINAL` / `#else` pair — add the row to **both**.
   Start by copying the blue row (index 1) and shifting the hue; the 15 fields
   are `0xRRGGBBAA`. (Wave rows: copy the corresponding blue wave row.)

   ```c
   // g_MenuColours[] — append after the grey row (index 5):
   { 0xbf6000_7f, ... 15 fields ..., },   // index 6: Amber (example)
   ```

2. **Name the new type** in `src/include/constants.h` (append-only, after
   `MENUDIALOGTYPE_WHITE 5`):

   ```c
   #define MENUDIALOGTYPE_AMBER 6   // port-only custom menu scheme row
   ```

3. **Map DEFAULT onto it** in `menuApplyColourScheme` (or, for a single hardcoded
   scheme, just return it unconditionally for DEFAULT):

   ```c
   s32 menuApplyColourScheme(s32 type) {
       if (type == MENUDIALOGTYPE_DEFAULT) {
           return MENUDIALOGTYPE_AMBER;
       }
       return type;
   }
   ```

4. **Match the accents** in `menuSchemeColour` — return the same hue with
   `intensity` placed on the right channel(s). For amber (R+G):

   ```c
   u32 menuSchemeColour(u32 intensity) {
       intensity &= 0xff;
       return (intensity << 24) | ((intensity * 6 / 10) << 16); // R + 60% G
   }
   ```

   (If you keep multiple hardcoded schemes, switch on your compile-time constant
   inside both functions, exactly as Recipe 1 does.)

5. Remove the dropdown + config registration as in Recipe 1 step 2.

The new row participates in the transition blend automatically (the `type2`
blend indexes the same tables), so opening/closing animates correctly.

---

## 5. Files & functions checklist

| File | What |
|---|---|
| `src/game/menu.c` | the palette tables `g_MenuColours` / `g_MenuWave1Colours` / `g_MenuWave2Colours`; `menuApplyColourScheme`; `menuSchemeColour`; `g_MenuColourScheme` (→ make it a compile-time constant) |
| `src/include/types.h` | `struct menucolourpalette` (the 15 fields) — reference only |
| `src/include/constants.h` | `MENUDIALOGTYPE_*` — append a new type index for a custom row |
| `src/include/data.h` | `extern` decls for `g_MenuColourScheme` / `menuApplyColourScheme` / `menuSchemeColour` (under `#ifndef PLATFORM_N64`) |
| `src/game/menugfx.c` | the accent call sites (`menugfxRenderSlider`, `menugfxDrawDropdownBackground`, `menugfxDrawListGroupHeader`) — already routed through `menuSchemeColour`; only touch if adding a new accent |
| `port/src/main.c` | `configRegisterInt("Game.MenuColourScheme", ...)` — **delete** to drop the config key |
| `port/src/optionsmenu.c` | the Experiments dropdown item + `menuhandlerMenuColourScheme` — **delete** to drop the menu entry |

---

## 6. Gotchas

- **Keep the three tables the same length.** `dialog->type` indexes all of them;
  a row present in `g_MenuColours` but missing from `g_MenuWave2Colours` is an
  out-of-bounds read on the title wave.
- **Only remap `MENUDIALOGTYPE_DEFAULT`.** Danger (red) and success (green) carry
  meaning — leave them. The two dispatch sites already gate on DEFAULT.
- **Apply at BOTH dispatch sites.** `menuOpenDialog` handles the initial open;
  `dialogTick` handles per-frame transitions. Miss the second and the scheme
  flips back to blue mid-animation.
- **Accents are separate from the palette.** Changing only the palette row leaves
  the slider/dropdown/header blue; changing only `menuSchemeColour` leaves the
  dialog borders blue. Do both.
- **Colour format is `0xRRGGBBAA`** (alpha in the low byte). `menuSchemeColour`
  returns the RGB in the top 3 bytes with alpha 0 — the caller ORs the alpha in.
- **`#ifndef PLATFORM_N64` everything.** The palette *tables* exist on N64, but
  the scheme functions, the new accent routing, and any new `MENUDIALOGTYPE_*`
  custom row must be port-guarded so the N64 build stays byte-identical. (A new
  table *row* is fine on N64 as long as nothing selects it there — but the
  cleanest path is to guard the whole feature.)
- **Title-screen / type-0 dialogs** use row 0 and are not remapped; the main
  front-end menu is `MENUDIALOGTYPE_DEFAULT` and *is* recoloured.
