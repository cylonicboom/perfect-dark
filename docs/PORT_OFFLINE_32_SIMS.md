# Offline 32 Simulants + Simulants Carousel

Port-only: **offline/local Combat Sim supports up to 32 simulants**; the
Simulants menu becomes a **4-page carousel** (slots 1-8 / 9-16 / 17-24 /
25-32). **Explicitly NOT netplay-compatible** — net games stay capped at 8
bots with zero wire/protocol change, and the extra pages are hidden while in
a net lobby. Extras are **session-only** (saved setups keep only slots 1-8;
no MPSETUP_VERSION bump).

## Architecture

- `MAX_BOTS` is **32** on the port (8 on N64); new `NET_MAX_BOTS 8` caps every
  wire/lobby/query/browser surface, and `MAX_BOTS_PRESET 8` pins every FILE
  image (ROM `struct mpconfig.simulants[]`/`aibotnames[]`, the mpsetups.bin
  wad bot block). **Rule: a serialization loop must never use `MAX_BOTS`.**
- `g_MpSetup.chrslots` is **u64** (players bits 0-15, bots bits 16-47 — the
  `options`-u64 precedent). All bit ops go through **`MPCHRSLOT(n)`**
  (`1ULL << n`); a plain `1 << (slot + MAX_PLAYERS)` is UB past bit 31 —
  never reintroduce one (audit:
  `rg -n "chrslots.*1[uU]? <<" src port` must stay clean).
- The wire still carries a **u32 chrslots field**: writes mask to
  `MPCHRSLOTS_PLAYERS_MASK | NET_MPCHRSLOTS_BOTS_MASK` (low 24 bits) — the
  natural netplay clamp. `MAX_MPCHRS` is now 48; per-chr arrays grew
  automatically.

## Netplay containment (why no protocol bump is sound)

1. **RNG-parity clamp (LOAD-BEARING)**: `setupCreateProps`' bot-spawn loop
   consumes one `rngRandom()` per iteration with net-synced seeds on server
   AND client. In any net mode it clamps `maxsimulants` to `NET_MAX_BOTS`,
   keeping RNG consumption identical to every existing proto-75 build —
   remove this and `g_MpBotChrPtrs[]` silently desyncs across peers.
2. Strip choke points (`chrslots &= PLAYERS | NET_BOTS`): `mpStartMatch`
   (any net role), `netStartClient`/host lobby entry (already reset chrslots
   to 1), menutick's server-lobby restore, `CLC_ADMIN_SETUP` read.
3. `mpGetMaxBotSlots()` (mplayer.c) = `NET_MAX_BOTS` online / `MAX_BOTS`
   offline — used by `mpGetSlotForNewBot`, `mpHasUnusedBotSlots`,
   `mpIsSimSlotEnabled`, and the Quick Team "Number Of Simulants" count.

## The carousel (mplayer/setup.c + menu.c)

Pages 2-4 are sibling dialogs (`g_MpSimulants2/3/4MenuDialog`) chained via
`nextsibling` from `g_MpSimulantsMenuDialog` — the Classic Options pattern.
Rows carry the **absolute slot index in `param`** (8-31): the existing
`menuhandlerMpSimulantSlot` / `mpMenuTextSimulantName` handlers work
unchanged. Row labels come from `mpMenuTextSimulantSlotLabel` (a text
function — language IDs only exist for "1:".."8:"); note **`param2`
function pointers dispatch via `menuResolveText` only WITHOUT
`MENUITEMFLAG_LITERAL_TEXT`** (that flag means "param2 is a literal char*").

Hiding online: pages carry `MENUDIALOGFLAG_NETPLAY_HIDDEN`;
`menuPushDialog`'s sibling walk (menu.c) skips flagged siblings when
`g_NetMode != NETMODE_NONE` (push-time, when net state is known). The walk's
documented `@bug` soft-lock (dialogs[] full → infinite loop) is fixed with
an else-break while there. Caps: 5 siblings/layer (we use 4), 10 dialogs[].

**Menu layout-array overflow (fixed 2026-06-16).** Opening the carousel pushes
all 4 pages as *simultaneously-open* sibling dialogs, and `func0f0f1d6c` (menu.c)
appends **every open dialog's** rows/columns/item-blocks into the fixed
`struct menu` arrays `rows[88]` / `cols[12]` / `blocks[80]` with **no bound** —
they only reset on a full `menuClose` (and roll back correctly per dialog in
`menuCloseDialog`). The N64 Simulants menu was a *single* page (~13 rows); the
4-page carousel (~52 rows) plus the parent Combat-Sim stack plus a deep open
Add/Edit-Simulant → character-config sub-stack overflowed `rows[]` into the
adjacent `rowend`/`cols[]`/`blocks[]` fields, corrupting the menu — symptom: every
Combat-Sim item draws at row 0, navigation dead (only Begin Match / Alt-F4),
self-heals on a stage load (`menuClose` resets the counters). Fix (port-only,
N64 byte-identical): the three arrays are enlarged (`256` / `32` / `160`) in
`struct menu` (types.h — `g_Menus` is pure runtime BSS, no save/wire/sizeof
dependency), **and** `func0f0f1d6c` got a port-only bounds guard that stops
appending before it can overrun them (graceful: extra items don't lay out).

## Preset / save / challenge containment

`mpApplyConfig` and the wad load loop read exactly `MAX_BOTS_PRESET` records
and then **disable rows 8-31** (name, difficulty → DISABLED, chrslots bit)
so presets, challenges and loaded setups replace the WHOLE roster — stale
session extras can never leak into a challenge. The wad save writes only the
first 8 (the 4-bit numsims field stays valid). `challengePerformSanityChecks`
walks `MAX_BOTS_PRESET`.

## Latent bugs fixed en route (from the 16-player change)

- `mpCopySimulant`: `1 << (dest + 4)` — an N64 4-player-era literal that set
  a PLAYER bit once MAX_PLAYERS grew → `MPCHRSLOT(dest + MAX_PLAYERS)`.
- `mpHasSimulants`: `chrslots & 0xff00` tested player bits 8-15 →
  `MPCHRSLOTS_BOTS_MASK`.

## Limits / notes

- **Memory**: 32 bot bodies/chrs live in MEMPOOL_STAGE; the 16MB default
  heap may not fit a 32-sim big-map match (`bodyAllocateModel` degrades to
  skipped bots with a loud log). Set `Game.MemorySize=256` if sims come up
  short.
- Saved setups keep slots 1-8 only (session-only extras by design).
- The active menu gets one bot-command screen per sim — 32 screens cycle but
  are unwieldy; "Command All Simulants" is the practical path.
- Scoreboard/rankings iterate `g_MpNumChrs` and scale; 30+ rows may clip on
  the lo-res layout (cosmetic).
- Status: compile-verified + net-parity soak PASS; the offline 32-sim match
  itself needs a windowed runtime test (this repo's checklist: 4 pages
  cycle, adds land in lowest free slot, 30-sim match spawns/scores, Clear
  All clears 32, saved setup keeps 8, online shows one page).
