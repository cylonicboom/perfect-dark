# Port-only 7th Simulant Difficulty — "Demon" / DemonSim

**Status: compile-verified, runtime PENDING.** Added 2026-07-30. Protocol bumped
**88 → 89** (value-only change; see *Deploy coordination*).

A seventh Combat Sim difficulty past Dark, selectable anywhere the six ROM
difficulties are (Configure Simulants → per-sim Difficulty, and the Quick Team
sim-difficulty dropdown). It is a DarkSim with every remaining restraint
removed.

## What it changes

Everything below is *relative to Dark*, the previous hardest. Read
`PORT_*`-free background on how these parameters work first — the table in
`src/game/bot.c` (`g_BotDifficulties[]`) plus the aim model in `bot0f192a74`.

| Parameter | Dark | Demon | Effect |
|---|---|---|---|
| Aim error floor vs **cloaked** target (`unk14`) | 8° | **0°** | Cloak was the ONLY thing that ever made a DarkSim miss. A DemonSim has literally zero aim error under every condition. |
| Tranquilizer resistance (`dizzyamount`) | 4000 | **8000** | Needs 2× the blur before it degrades into blind firing. |
| Movement speed multiplier | 11.2 | **16.8** | 1.5× Dark, and faster than SpeedSim's 14.0. |
| Melee cooldown | 0.5 s | **0** | `punchtimer60 = 0`; the timer counts DOWN and swings when negative, so it re-swings on the next tick. |
| Weapon-choice randomness (`equipextrascores`) | ±15 | **±1** | Weapon selection is its scoring pass and essentially nothing else. |
| Team follow chance | 0% | **100%** | Inverts Dark's lone-wolf behaviour — DemonSims always squad up. |
| Unlimited ammo | no | **yes** | See below. |
| All map weapons at spawn | no | **yes** | See below. |

Inherited from Dark unchanged: zero trigger delay, instant aim settle, turning
never spoils aim, and the full spawn shield when the scenario has shields.

Everything Dark *doesn't* change stays unchanged here too — **HP is still
stock**. A DemonSim is not a tank; it is a perfectly accurate, very fast,
fully-armed one. (Difficulty has never scaled bot health in this engine.)

### Unlimited ammo

`BOTFLAG_UNLIMITEDAMMO` was **already fully implemented upstream** — `botact.c`
returns the ammotype capacity for every query and skips every deduction (4
sites). Dark's `botReset` branch carries a decomp-annotated `@bug`: it *clears*
the flag instead of setting it, so nothing in the shipped game ever turned it on.
Demon is the first thing that does. **Dark's inverted behaviour is deliberately
left alone** — this is a new difficulty, not a fix to an existing one.

The flag is **derived, not merely set**: `botReset` clears it and then re-sets it
for Demon only. An admin can change a sim's difficulty *in place* mid-match
(`CLC_ADMIN_SETUP` edits `g_BotConfigsArray` directly), so a slot recycled from a
DemonSim would otherwise keep unlimited ammo as, say, a MeatSim.

### All map weapons at spawn

`botSpawn` gains a Demon block modelled on the existing `MPOPTION_SPAWNWITHWEAPON`
slot-0 block, but looping all `NUM_MPWEAPONSLOTS` (6) and ignoring that option —
a full loadout is the point, and the AI's own per-situation scoring then picks
from it (sniper duel → sniper, corridor → shotgun) instead of hunting pickups.

- `MPWEAPON_NONE` / `_DISABLED` / `_SHIELD` slots are skipped.
- **No ammo is handed out** — `UNLIMITEDAMMO` is already set by `botReset` (which
  runs first), making `botactGiveAmmoByType` an early-return no-op and every
  query report full capacity, so the scoring pass never discounts a dry gun.
- Bot inventories hold **10** items (`botmgr.c` `botinvInit`) against 6 slots, so
  the item list cannot overflow.
- Duplicate slots collapse to one copy (`botinvGiveSingleWeapon` ignores a weapon
  already held) — a map listing the same gun twice does **not** yield a dual
  wield. Granting duals is a possible follow-up (`botinvGiveDualWeapon`).

## Why the enum value is 7, appended after `BOTDIFF_DISABLED`

Not cosmetic — it is the only legal choice, for two independent reasons:

1. **Difficulty is persisted in a 3-bit save field.** `mpsetupSaveToBuffer`
   (`mplayer.c`) writes `savebufferOr(buffer, difficulty, 3)`. With
   `BOTDIFF_DISABLED == 6`, **7 is the only value left.** An 8th difficulty would
   require widening that field and bumping the wad version.
2. **Inserting at 6 would corrupt every existing save.** `BOTDIFF_DISABLED` means
   "no bot in this slot" and is written for every empty slot; re-pointing 6 at a
   difficulty would fill saved setups (and the wire) with sims.

**Consequence — the one real gotcha:** `g_BotProfiles` rows 0..5 are MEAT..DARK,
i.e. *profile index == difficulty*, and several callers rely on that by handing a
difficulty straight to `mpCreateBotFromProfile`. That identity does **not** hold
for Demon. Its profile row is **appended at the END** of `g_BotProfiles` (after
the personality block) because `challenge.c` indexes that table by bot **type**
(`g_BotProfiles[simtype]`) — inserting mid-table would silently re-point every
challenge's sim-type feature gate.

So: **always convert with `mpBotProfileForDifficulty()`** (mplayer.c) when handing
a difficulty to `mpCreateBotFromProfile`. Passing `BOTDIFF_DEMON` raw yields
profile index 7 = `BOTTYPE_SHIELD` — a silently-wrong ShieldSim. The three
quick-add sites in `setup.c` and the admin-menu site in `netmenu.c` are already
routed through it (identity for everything those can currently produce).

## Naming

The difficulty label is **"Demon"**; sims are named **"DemonSim"**.

The label is a plain C literal in the dropdown handlers — the ROM's
`L_MISC_082+` difficulty strings can't be extended. The sim name comes from
`mpBotProfileName()` (mplayer.c) rather than `langGet` on the profile row, for the
same reason; the row's `name` field is a placeholder that nothing should read.

⚠ **Keep any profile name ≤ 8 characters.** `mpGenerateBotNames` sprintf()s
`"<name>:<count>\n"` into a `char[16]`; the longest ROM name ("VendettaSim", 11)
with a two-digit count already fills that buffer *exactly*, so a longer name
smashes the stack. "DemonSim" is 8. (An earlier draft used "NightmareSim" — 12 —
which would have overflowed at counts ≥ 10.)

## Wire safety

`bot->difficulty` is read off the wire as a **raw u8** in two places
(`netmsg.c`, `SVC_STAGE_START` and the admin-setup path) and is used to **index
`g_BotDifficulties[]`** on every aim/reaction/dizzy read. That was an unvalidated
out-of-bounds read waiting to happen; both sites now clamp through
`netmsgSanitizeBotDifficulty()` (anything > `BOTDIFF_DEMON` → `BOTDIFF_NORMAL`).

## Deploy coordination

**Protocol 88 → 89. Server and clients must be updated together.** There is no
layout change — difficulty was always a u8 on the wire — but the *value* 7 is
new, and an old peer receiving it indexes `g_BotDifficulties[7]`, one past its
7-row table, driving that sim's aim/reaction thresholds off garbage. New builds
sanitize on read; old ones cannot.

Saved `mpsetups.bin` files are **forward and backward compatible** (3-bit field
unchanged, value 6 still means disabled) — but a setup saved with a DemonSim
loaded into an older build will read difficulty 7 and hit the same OOB read.

## Known limits

- **Fill All doesn't offer Demon.** `mpFillAllSimulants` treats its difficulty
  range as profile indices and clamps to `BOTDIFF_DARK`; supporting Demon there
  needs the range mapped through `mpBotProfileForDifficulty` (the clamp currently
  protects it, so this is a missing feature, not a bug). Its `numspecial` count
  *was* adjusted so "random special" can't accidentally roll the appended Demon
  row.
- **No dual wields** at spawn (see above).
- The pre-existing option-index-vs-difficulty mismatch in the difficulty
  dropdowns when Hard/Perfect/Dark are challenge-locked is **left as-is**; Demon
  is mapped explicitly around it rather than relying on the identity.

## Runtime checklist

1. Configure Simulants → a sim → Difficulty: "Demon" appears as the 7th option
   and sticks after leaving/re-entering the menu.
2. The sim is named **DemonSim** (or `DemonSim:1`, `:2`… with several).
3. It spawns holding a weapon and visibly cycles between the map's weapons as
   the situation changes; it never runs out of ammo.
4. It moves noticeably faster than a DarkSim and melees continuously in contact.
5. Tranquilizing it takes far longer to make it spray wildly.
6. Cloak does **not** make it miss.
7. In a teams match it follows a teammate.
8. Save a setup with a DemonSim, reload it: still Demon (not disabled, not Meat).
9. Set the sim back to Meat mid-session and confirm it loses unlimited ammo.

## Read when touching

`g_BotDifficulties[]` / `bot0f192a74` (aim), `botReset` / `botSpawn`,
`botCalculateMaxSpeed`, the `punchtimer60` sites, `botmgr.c` `followchance`,
`botinv.c` `equipextrascores`, `g_BotProfiles` / `mpFindBotProfile` /
`mpCreateBotFromProfile`, the difficulty dropdowns in `setup.c`, or anything
serializing a bot difficulty.
