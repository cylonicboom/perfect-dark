# Game Logic (`src/game/`)

> Auto-loads when working under `src/game/`. For the full netplay protocol and CSP/interp/lag-comp design, see [`../../port/src/net/CLAUDE.md`](../../port/src/net/CLAUDE.md).

**Keep symbol names in this directory** (port-first policy, root CLAUDE.md 2026-07-31): the repo no longer targets N64 or byte-matching, but decompiled names are the stable coordinate system of the docs/memory/crash-symbol base — keep them. Struct repacking and deleting N64-only paths are now allowed (serialized formats stay frozen; netplay determinism is the binding contract).

---

## Net-related modifications

Many files in this folder have been modified for netplay. Two recurring patterns to preserve when editing:

- **`#ifndef PLATFORM_N64` blocks** — the historical guard for port-only logic. Since the 2026-07-31 port-first policy, NEW work doesn't need them (the N64 build is retired); existing guards may be collapsed when touching a function.
- **`if (g_NetMode != NETMODE_CLIENT)` guards** — gate server-only writes and AI logic so clients don't clobber authoritative state. Examples: `botTick` in `prop.c`, `mpstatsRecordDeath` writes in `mpstats.c`, `bwalkUpdateRemote` force-position early-return in `bondwalk.c`.

When adding logic that should only run on the server, use `g_NetMode != NETMODE_CLIENT` (not `g_NetMode == NETMODE_SERVER` — the host player also has `NETMODE_SERVER`). When adding logic that must be absent from N64 builds, wrap in `#ifndef PLATFORM_N64`.

---

## Positional Weapon Sounds (`bondgun.c`)

`bgunTick*` functions originally called `sndStart(var80095200, ...)` for shoot/reload/empty/cock sounds. `sndStart` is non-positional ("in your head") which is correct for the local player but wrong for remote players whose `bgunTick` runs locally too (driven by inputs received via `SVC_PLAYER_MOVE` after `setCurrentPlayerNum(remotenum)`). Result: every remote shot played at full volume as if the local player fired.

Fix: new static helper `bgunPlayGunSound(soundnum, handle_out, pstype)` in `bondgun.c`. When `currentplayer->isremote && currentplayer->prop`, routes through `psCreate(NULL, pl->prop, ...)` (3D positional, pans/attenuates by listener distance). Otherwise falls back to `sndStart(...)`. Wired into 6 sites: main shoot sound (both hands), reload, empty-fire (Maian water-hit, tranq, default), and the `GUNCMD_PLAYSOUND` animation-triggered path. Pitch-shift effects that depend on the returned `struct sndstate *` handle (e.g. mauler charge) are skipped for remote shots — minor cosmetic loss.

**Continuous-loop sounds skipped for remote** (SFX_805E / Reaper spin, SFX_LASER_STREAM, SFX_MAULER_CHARGE): these store `hand->audiohandle` for ongoing volume/pitch shaping. `psCreate` doesn't return a compatible handle, and calling it every tick where the condition holds (`audiohandle == NULL`) would spam-overlap. The cleanest workaround is to suppress these continuous sounds for remote players — the actual fire sound still plays positionally via `bgunPlayGunSound`.

PLATFORM_N64 build keeps the original `sndStart` path unchanged.

**Known still-broken sounds**: punching (and possibly some other animation-script-driven sounds outside the GUNCMD_PLAYSOUND path) still play first-person for everyone. Source not yet located.
