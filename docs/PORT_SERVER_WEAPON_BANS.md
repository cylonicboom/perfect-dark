# Port-only Feature: Server Weapon / Weapon-Function Bans

Lets a server disallow specific weapons (unbalanced weapons, gadgets, the
Combat Boost) or just one *function* of a weapon (e.g. the Laptop Gun's
deploy-as-sentry secondary) for every match it hosts. Configured in the
dedicated-server playlist's `[server]` section; enforced at match start by
filtering the final loadout, so it composes with weapon sets, custom presets,
random rotation and admin setup pushes alike.

Everything is `#ifndef PLATFORM_N64`-gated; the N64 build is byte-identical.

---

## Config

In the playlist file (see `dist/linux/server/server_playlist.example.ini`):

```ini
[server]
banned = COMBATBOOST, CLOAK, FARSIGHT      ; whole-weapon bans
banned = LAPTOPGUN:sec, DRAGON:sec, MINES  ; function bans + a group token
```

- Comma-separated tokens, case-insensitive. Multiple `banned=` lines OR together.
- `NAME` — whole-weapon ban: any slot holding it becomes **Nothing**
  (`MPWEAPON_NONE`), so the weapon pad never spawns and nobody (player or bot)
  can ever hold it.
- `NAME:pri` / `NAME:sec` (also `:primary`/`:secondary`) — function ban: the
  weapon stays, but that function is disabled via the weapon-preset fn-flag
  system (`FNFLAG_*` → `bgunPrimaryFunctionDisabled` /
  `bgunSecondaryFunctionDisabled` / `botinvSwitchToWeapon`). Banning *both*
  functions of one weapon is promoted to a whole-weapon ban (the engine's
  fn-flag hooks assume never-both, see `PORT_WEAPON_PRESETS.md`).
- Group tokens: `GADGETS` = XRAYSCANNER, NIGHTVISION, IRSCANNER,
  CLOAKINGDEVICE; `MINES` = TIMEDMINE, PROXIMITYMINE, REMOTEMINE. A suffix on
  a group applies to every member.
- Bans are server-wide (not per-entry) and apply to every match in the
  rotation. No playlist / empty key = no bans.

### Weapon tokens

Canonical names (aliases in parentheses): FALCON2, FALCON2SILENCED,
FALCON2SCOPE, MAGSEC4, MAULER, PHOENIX, DY357MAGNUM (MAGNUM), DY357LX,
CMP150, CYCLONE, CALLISTO, RCP120, LAPTOPGUN (LAPTOP), DRAGON, K7AVENGER (K7),
AR34, SUPERDRAGON, SHOTGUN, REAPER, SNIPERRIFLE (SNIPER), FARSIGHT,
DEVASTATOR, ROCKETLAUNCHER (ROCKET), SLAYER, COMBATKNIFE (KNIFE), CROSSBOW,
TRANQUILIZER (TRANQ), GRENADE, NBOMB, TIMEDMINE, PROXIMITYMINE (PROXYMINE),
REMOTEMINE, LASER, XRAYSCANNER (XRAY), NIGHTVISION, IRSCANNER,
CLOAKINGDEVICE (CLOAK), COMBATBOOST (BOOST, SPEEDPILL), PP9I, CC13, KL01313,
KF7SPECIAL (KF7), ZZT, DMC, AR53, RCP45, SHIELD.

---

## Surface

| File | Site | What |
|---|---|---|
| `port/include/net/playlist.h` | top | `PLAYLIST_BAN_PRI/SEC/WEAPON` bits (low two == `FNFLAG_*` by design), `PLAYLIST_WEAPONBAN_SLOTS`, `weapon_bans[]` in `struct playlist`, `playlistApplyWeaponBans` decl |
| `port/src/net/playlist.c` | tables | `s_mpweapons[]` name→`MPWEAPON_*` table + `GADGETS`/`MINES` groups |
| `port/src/net/playlist.c` | parser | `banned=` key in `[server]`; `parseBannedList` / `banApplyBits` / `playlistFormatBans` (load log + `/playlist list` line) |
| `port/src/net/playlist.c` | apply | `playlistApplyWeaponBans()` — slots → `MPWEAPON_NONE` for whole bans, `g_MpSlotFnFlags[i] \|=` for function bans; self-gated `g_NetMode == NETMODE_SERVER`; idempotent |
| `src/game/mplayer/mplayer.c` | `mpStartMatch`, after the MPFEATURE option strips | calls `playlistApplyWeaponBans()` (`#ifndef PLATFORM_N64`) — runs after the `AUTORANDOMWEAPON_START` re-roll so random/preset picks are filtered too |
| `port/src/net/netmsg.c` | `netmsgSvcStageStartWrite/Read` | `g_MpSlotFnFlags[6]` synced right after the weapons block (**proto 58**) |
| `port/include/net/net.h` | `NET_PROTOCOL_VER` | 57 → 58 |

## Why `mpStartMatch` is the choke point

`g_MpSetup.weapons[]` can be rewritten right up to match start: lobby menu
changes, `CLC_ADMIN_SETUP` pushes, `playlistApply`'s preset copy, and the
`MPOPTION_AUTORANDOMWEAPON_START` re-roll *inside* `mpStartMatch` itself
(which also resets `g_MpSlotFnFlags` via `mpApplyWeaponSet`). Filtering after
that re-roll — in the same server-gated block as the vanilla
`challengeIsFeatureUnlocked` option strips (the `MPWEAPON_SHIELD →
MPWEAPON_NONE` strip in `mpApplyConfig` is the vanilla precedent for slot
substitution) — means the *final* loadout is filtered exactly once, before:

- weapon pads spawn from the slots during stage load, and
- `netServerStageStart` broadcasts the slots + fn-flags in `SVC_STAGE_START`
  (including the JIP copy sent to late joiners).

## Client enforcement (the fn-flag wire gap, closed)

`PORT_WEAPON_PRESETS.md` documented that `g_MpSlotFnFlags[]` was host-local:
clients evaluated all-zero flags, so fn restrictions only bound host-side
firing logic. Proto 58 ships the 6 bytes after the weapons block in
`SVC_STAGE_START`; the client reads them straight into `g_MpSlotFnFlags`.
This makes function bans bind on clients **and** fixes pure fn-flag custom
presets in net games. (Client lifecycle is safe because the client's
`mpStartMatch` never enters the server-gated block that calls
`mpApplyWeaponSet`, the only fn-flag reset path.)

## Notes / limitations

- The lobby's weapon-set display (`SVC_LOBBY_STATE`) shows the *unfiltered*
  set until match start — bans bind when the match locks in. The `/playlist
  list` console command prints the active ban list.
- Whole-banned weapons vanish from the loadout, so nothing needs to gate
  pickups: the props never exist. Scenario items (briefcase, uplink) are not
  weapons and are unaffected.
- `SHIELD` can be banned by name (slot → Nothing) — same effect as the
  vanilla locked-feature strip in `mpApplyConfig`.
- Bans live in `g_NetPlaylist`, so they apply to any *hosting* session with a
  playlist loaded (dedicated is the intended consumer). Clients never apply
  bans locally — their slots/flags are wire-authoritative.
