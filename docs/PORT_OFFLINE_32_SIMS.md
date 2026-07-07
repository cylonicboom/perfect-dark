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
N64 byte-identical): the three arrays are enlarged (`256` / `32` / `320`) in
`struct menu` (types.h — `g_Menus` is pure runtime BSS, no save/wire/sizeof
dependency), **and** `func0f0f1d6c` got a port-only bounds guard that stops
appending before it can overrun them (graceful: extra items don't lay out).
(`blocks` is `320` because the Team Control carousel below is the heaviest open
stack: 36 `MENUITEMTYPE_DROPDOWN` rows × 4 item-blocks each = 144 blocks.)

## Team Control carousel (mplayer/setup.c)

The Team Control menu got the same 4-page carousel so all 36 offline combatants
(up to 4 humans + 32 sims) can be assigned teams. Team rows are keyed by the
**compacted combatant ordinal** — `menuhandlerMpTeamSlot` / `mpMenuTextChrNameForTeamSetup`
resolve `item->param` through `mpGetChrConfigBySlotNum`, which walks the set
`chrslots` bits **players-first then sims**, so the humans always occupy the
lowest ordinals. Page 1 is the original `g_MpTeamsMenuItems` (ordinals 0-11 = every
human + the first sims, the "max 12" first page); pages 2-4 (`g_MpTeams2/3/4MenuDialog`,
the `MP_TEAMPAGE_*` macros) carry ordinals 12-19 / 20-27 / 28-35, chained via
`nextsibling` and flagged `MENUDIALOGFLAG_NETPLAY_HIDDEN` (offline only — net games
cap bots at `NET_MAX_BOTS`, so those ordinals never exist online; the online team
menu is unchanged). Overflowing rows on the last page (e.g. only 1 human + 32 sims =
33 combatants) are auto-disabled by the existing `mpGetChrConfigBySlotNum`-returns-NULL
→ `CHECKDISABLED` gate.

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

## Explosion/projectile owner nibble truncation (fixed 2026-06-22)

**Symptom:** with 4 local humans + 32 sims, AFK human players accrued kills
they never made. Direct gunfire was attributed correctly (it resolves the
attacker via `mpPlayerGetIndex(aprop->chr)`), but **explosion / projectile /
thrown-weapon / destructible** kills leaked onto human slots 0-3.

**Root:** the per-object owner is packed into the **top nibble of
`obj->hidden`** (`playernum << 28`, read back `(hidden & 0xf0000000) >> 28`) — a
4-bit field that only holds **0..15**. That was safe on N64 (max 4+8=12
combatants) but the 32-sim feature pushes the *packed* combatant index
(`mpPlayerGetIndex` / `g_MpAllChrPtrs` order — humans first, then sims) to
0..35. A sim at packed index ≥16 truncates: `16&0xf=0 … 19&0xf=3`, `32&0xf=0 …
35&0xf=3`, so eight specific sims credited their blast kills to the four human
slots. The flags already fill bits 0-27 and the owner the top nibble, so the
field can't be widened in place; `defaultobj` is also offset-bound by the setup
preprocessor's `n64_*` structs.

**Fix:** mirror the **full** owner index onto a new port-only
`struct prop.ownerplayernum` (sentinel `-1` = unset, init in `propAllocate`,
next to `syncid`) via two helpers in `propobj.c`:
`objSetOwnerPlayerNum` (writes the legacy nibble **and** the prop mirror) and
`objGetOwnerPlayerNum` (prefers the untruncated mirror, falls back to the
nibble). Every nibble write site (bondgun throw/fire, `chrEquipWeapon`, weapon
create, `objFall`/`objDamage`/`func0f085050`, HTM activate, co-op mine setup)
and every read site (`propExplode`, mine/nbomb detonation, rocket-embed,
laptop-sentry target/fire, the bounce-damage tick, HTM read) now route through
the helpers. The explosion struct's `owner` is `s8`, so the full index survives
end-to-end into `mpGetChrFromPlayerIndex` → correct attribution. **N64
byte-identical** (the mirror is `#ifndef PLATFORM_N64`; helpers reduce to the
original nibble math). **No wire/proto change** — net stays ≤ `NET_MAX_BOTS`
(≤12 combatants ≤15), so `netmsg.c`/`netprop.c` keep reading the nibble.

Gotcha closed: the remote-mine detonation test `g_PlayersDetonatingMines &
(1 << ownerplayernum)` was implicitly bounded by the 0..15 nibble; with the full
index it can now be ≥32, so both branches gained a `(u32)ownerplayernum < 32U`
guard to avoid the UB shift (a sim-owned mine never matches the player-only
detonator mask anyway).

### Round 2 hardening (2026-07-07) — residual misattribution holes

Kill-misattribution reports persisted after the mirror fix. A full re-audit of
every attribution path confirmed the mirror covers the high-frequency creation
paths, and closed the remaining holes:

- **No-owner sentinel restored** (`objGetOwnerPlayerNum`): vanilla writes owner
  `-1` (packs to nibble `0xf`) for "no owner" — e.g. the crusher-object
  `objDamage(..., -1)` sites — and relied on `mpGetChrFromPlayerIndex(15)`
  returning NULL with ≤12 combatants. The port allows 16+ combatants, so packed
  index 15 became a REAL entry and every ownerless blast was credited to
  whoever sat there. The getter now returns `-1` for nibble `0xf` (port-only)
  and the setter records an explicit no-owner as mirror `-2` (distinct from
  `-1` = "mirror unset"). A genuine combatant-15 owner still resolves via the
  mirror. `netbufReadHidden` passes the `0xf` nibble through instead of
  remapping it via the (empty) `g_NetClients[15]`.
- **Recycled-slot stale nibble** (`objInit`): a pooled weapon/hat obj slot kept
  its previous life's owner nibble while the fresh prop's mirror reset to `-1`,
  so the getter fell back to a stale TRUNCATED nibble. `objInit` now resets the
  owner to no-owner — **Combat Sim only** (`normmplayerisrunning`): the solo
  campaign relies on the owner-0 default (G5 Building's pre-placed remote
  mines detonate for Bond because their owner reads 0).
- **`func0f18d0e8` stale 4-player boundary** (`mplayer.c`): the slot→packed
  inverse of `func0f18d074` hardcoded the N64 `4` where the port's boundary is
  `MAX_PLAYERS` (16) — every bot mis-mapped (slot 16 read
  `g_BotConfigsArray[12]`), corrupting Pop-a-Cap scoring and Judge-bot target
  ranking. Now uses `MAX_PLAYERS` (byte-identical on N64 where it is 4).
- **Last Attacker Attribution recency window** (`mpstats.c` +
  `chr->lastattackerstamp60`, port-only chrdata field, init in `chrInit`):
  `chr->lastattacker` never expires, so with `MPOPTION_LASTATTACKERKILL`
  enabled a player who damaged a chr once inherited every later env/fall/
  suicide death of that chr — indefinitely. The recovery now requires the last
  hit to be within ~10s (`TICKS(600)`); the `killattrib` diag line logs the
  age.

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
