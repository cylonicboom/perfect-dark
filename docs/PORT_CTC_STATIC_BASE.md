# Port-only Feature: CTC Per-Team Base Pins

Lets the host pin one or more Capture the Case teams to specific base configurations instead of taking the engine's `rngRandom() % 4` shuffle. Each team gets its own dropdown ("Team 1 Base" / "Team 2 Base" / "Team 3 Base" / "Team 4 Base") that lists Random + Base 1..4; the menu dynamically greys out dropdowns for teams that have no players assigned in the current lobby.

Mirrors the shape of `PORT_KOH_STATIC_HILL` but at four-dimensional team granularity instead of one-dimensional hill granularity. Everything is gated under `#ifndef PLATFORM_N64`; the N64 build is byte-identical.

See [`PORTING_HOWTO.md`](PORTING_HOWTO.md) for the cross-cutting porting methodology this sits on top of.

---

## Surface

| File | Site | What |
|---|---|---|
| `src/include/types.h` | `struct mpsetup` tail, under `#ifndef PLATFORM_N64` | Append `u8 ctcteambase[4]` (after `kohstatichill`) |
| `src/game/mplayer/scenarios/capturethecase.inc` | top of file, under `#ifndef PLATFORM_N64` | `menuhandlerMpCtcTeamBase` (one handler, four menu entries share it via `item->param` carrying the team index) |
| `src/game/mplayer/scenarios/capturethecase.inc` | inside `g_CtcOptionsMenuItems[]`'s existing port-only block | Four `MENUITEMTYPE_DROPDOWN` entries — "Team 1 Base" .. "Team 4 Base" — each with `item->param = team_index` |
| `src/game/mplayer/scenarios/capturethecase.inc` | `ctcInitProps` team-assignment loop | Honour `ctcteambase[i]` before falling through to the `rngRandom() % 4` do-while |
| `src/game/mplayer/mplayer.c` | `mpsetupfileLoadWad` (gated `version >= 4`) and `SaveWad` (unconditional) | Serialize the 4 team-base bytes as 3 bits each |
| `port/src/mpsetups.c` | `MPSETUP_VERSION` | Bumped to `4` (covers the CTC pins + the HTB/HTM pins added in the same commit) |
| `port/src/net/netmsg.c` | `netmsgSvcStageStart{Write,Read}` | Write/read 4 bytes after the existing `kohstatichill` byte |
| `port/include/net/net.h` | `NET_PROTOCOL_VER` | Bumped to `29` |

The dropdown count is hardcoded at 5 (Random + Base 1..4). All 10 CTC-supported MP stages define exactly 4 bases — confirmed across `mp_setupmp1.c` (Base), `mp_setupmp3.c` (Area 52), `mp_setupmp4.c` (Warehouse), `mp_setupmp5.c` (Car Park), `mp_setupmp9.c` (Ruins), `mp_setupmp10.c` (Sewers), `mp_setupmp11.c` (Felicity), `mp_setupmp12.c` (Fortress), `mp_setupmp13.c` (Villa), `mp_setupmp15.c` (Grid). No per-stage helper needed unlike KoH's `kohGetStageHillCount`.

---

## Menu handler shape

```c
MenuItemHandlerResult menuhandlerMpCtcTeamBase(s32 operation, struct menuitem *item, union handlerdata *data)
{
    static char label[16];
    s32 team_idx = item->param;
    s32 k;
    bool team_in_use;

    switch (operation) {
    case MENUOP_CHECKDISABLED:
        team_in_use = false;
        for (k = 0; k < MAX_MPCHRS; k++) {
            if ((g_MpSetup.chrslots & (1 << k)) && MPCHR(k)->team == team_idx) {
                team_in_use = true;
                break;
            }
        }
        return team_in_use ? 0 : 1;
    case MENUOP_GETOPTIONCOUNT:
        data->dropdown.value = 5;            // Random + Base 1..4
        break;
    case MENUOP_GETOPTIONTEXT:
        if (data->dropdown.value == 0) return (uintptr_t)"Random";
        sprintf(label, "Base %u", (unsigned)data->dropdown.value);
        return (uintptr_t)label;
    case MENUOP_SET:
        g_MpSetup.ctcteambase[team_idx] = (u8)data->dropdown.value;
        break;
    case MENUOP_GETSELECTEDINDEX:
        if (g_MpSetup.ctcteambase[team_idx] > 4) data->dropdown.value = 0;
        else data->dropdown.value = g_MpSetup.ctcteambase[team_idx];
        break;
    }
    return 0;
}
```

`MENUOP_CHECKDISABLED` is recomputed on every menu tick. As players or bots are added / removed from the lobby (which updates `g_MpSetup.chrslots` and `MPCHR(k)->team`), the dropdown greys / un-greys automatically — no menu reload needed.

The four menu entries each carry their team index via `item->param` (the second field of `struct menuitem`, a `u8`). Values 0..3.

---

## Engine hook (`ctcInitProps`)

Replace the existing team-assignment loop with one that honours pins:

```c
for (i = 0; i != ARRAYCOUNT(g_ScenarioData.ctc.teamindexes); i++) {
#ifndef PLATFORM_N64
    if (g_MpSetup.ctcteambase[i] > 0 && g_MpSetup.ctcteambase[i] <= 4
            && !teamsdone[g_MpSetup.ctcteambase[i] - 1]) {
        g_ScenarioData.ctc.teamindexes[i] = g_MpSetup.ctcteambase[i] - 1;
    } else
#endif
    {
        do {
            g_ScenarioData.ctc.teamindexes[i] = rngRandom() % 4;
        } while (teamsdone[g_ScenarioData.ctc.teamindexes[i]]);
    }
    teamsdone[g_ScenarioData.ctc.teamindexes[i]] = true;
}
```

The `!teamsdone[…]` check resolves "two teams both pinned to Base 2" — first wins, second falls through to the random branch. Pinning a team **skips** `rngRandom()` for that iteration (and the do-while's potential retries), so the RNG seed advance differs between server and client unless both agree on the pin values. The wire field in `SVC_STAGE_START` is read before `mpStartMatch` triggers `ctcInitProps`, satisfying this.

---

## Save / wire surface

### `mpsetup` wad (4 teams × 3 bits = 12 bits)

```c
// Load — gated on version >= 4 (defaults to Random for older files)
#ifndef PLATFORM_N64
    for (i = 0; i < (s32)ARRAYCOUNT(g_MpSetup.ctcteambase); i++) {
        g_MpSetup.ctcteambase[i] = 0;
    }
    if (version >= 4) {
        for (i = 0; i < (s32)ARRAYCOUNT(g_MpSetup.ctcteambase); i++) {
            g_MpSetup.ctcteambase[i] = savebufferReadBits(buffer, 3);
        }
    }
#endif

// Save — always written
#ifndef PLATFORM_N64
    for (i = 0; i < (s32)ARRAYCOUNT(g_MpSetup.ctcteambase); i++) {
        savebufferOr(buffer, g_MpSetup.ctcteambase[i], 3);
    }
#endif
```

3 bits per team is enough (values 0..4 fit). The HTB/HTM static-spawn fields share this same wad-version gate (`version >= 4`) and follow the CTC bytes in the encoding — see [`PORT_HTB_HTM_STATIC_SPAWN.md`](PORT_HTB_HTM_STATIC_SPAWN.md).

### `SVC_STAGE_START` wire (4 bytes)

```c
// Write (in netmsgSvcStageStartWrite, after the existing kohstatichill byte)
for (s32 i = 0; i < 4; ++i) {
    netbufWriteU8(dst, g_MpSetup.ctcteambase[i]);
}

// Read (in netmsgSvcStageStartRead, same position)
for (s32 i = 0; i < 4; ++i) {
    g_MpSetup.ctcteambase[i] = netbufReadU8(src);
}
```

`NET_PROTOCOL_VER` was bumped from 28 to 29 in the same commit; mismatched versions are rejected at auth time. The HTB/HTM bytes follow immediately after.

---

## Determinism

The static path skips `rngRandom() % 4` for the pinned iteration AND the do-while's collision retries. RNG advance differs from the unpinned path. Server and client must agree on `ctcteambase[]` **before** `ctcInitProps` runs.

Two safeguards together:

1. The wire field is read in `netmsgSvcStageStartRead` before `mpStartMatch` triggers any scenario init.
2. Clients never edit `g_MpSetup` while connected. The lobby UI only writes on the host.

---

## Porting checklist

1. **Constants & field** — Append `u8 ctcteambase[4]` to `struct mpsetup` in `src/include/types.h` under `#ifndef PLATFORM_N64`.
2. **Handler & menu entries** — Add `menuhandlerMpCtcTeamBase` to `capturethecase.inc` and four `MENUITEMTYPE_DROPDOWN` entries inside the existing port-only block of `g_CtcOptionsMenuItems[]`. Each entry's `item->param` carries the team index 0..3.
3. **Engine hook** — Patch the team-assignment loop in `ctcInitProps`.
4. **Wad save / load** — Bump `MPSETUP_VERSION` in `port/src/mpsetups.c`. Extend `mpsetupfileLoadWad` (gated `version >= N`) and `SaveWad` (unconditional) in `mplayer.c`. The KoH bullet at lines 4309-4314 / 4384-4386 of `mplayer.c` is the template.
5. **Wire** — Append 4 bytes to `netmsgSvcStageStartWrite` / `Read` in `port/src/net/netmsg.c`. Bump `NET_PROTOCOL_VER` in `port/include/net/net.h`.

If your branch has no netplay, skip step 5. If your branch has no versioned mpsetup files, skip step 4 (the field becomes session-only — won't survive a restart).

---

## Porting gotchas

- **`item->param` is a `u8` and used to carry the team index.** Don't try to store a wider value here — `struct menuitem`'s `param` field is one byte. CTC teams are 0..3 so this is fine; if you ever need >256 distinct items sharing a handler, use `param3` instead.
- **Two teams can be pinned to the same base — the first wins.** The `!teamsdone[…]` check makes this safe but it means the menu UI doesn't prevent the conflict. Two teams both pinned to Base 2 will see team 0 actually at Base 2 and team 1 falling back to a random unused base. Documented behaviour, not a bug.
- **`teamsdone[]` is local to `ctcInitProps`.** The pin check reads from `g_MpSetup.ctcteambase` (persistent) and from `teamsdone[]` (per-call). Don't try to skip the `teamsdone[]` updates "because the pin makes them redundant" — the random branch still needs them.
- **`MENUOP_CHECKDISABLED` walks `chrslots` every menu tick.** That's `MAX_MPCHRS` iterations per dropdown per tick = 12 × 4 = 48 iterations per menu frame. Cheap. Don't try to cache the result without a robust invalidation hook — players / bots can change teams while the menu is open.
- **Pinning skips `rngRandom()` for that iteration AND the do-while collision retries.** The total RNG advance differs significantly between pinned and unpinned matches. Anything downstream of `ctcInitProps` that consumes RNG (weapon spawn rotation, sim spawn picks, etc.) will land on different states. This is why server / client agreement on `ctcteambase[]` is non-negotiable.
- **Don't try to do the team count helper at lobby render time using `g_ScenarioData.ctc.playercountsperteam[]`.** That array is populated by `ctcInitProps` at match start — in the lobby it's stale or zero. Walk `g_MpSetup.chrslots` + `MPCHR(k)->team` directly each time, as the handler does.
- **All 10 CTC stages define exactly 4 bases.** No per-stage helper needed. If your branch adds new CTC-supported stages with different counts, add a `ctcGetStageBaseCount(stagenum)` helper following the `kohGetStageHillCount` pattern. The handler's `MENUOP_GETOPTIONCOUNT` would then become `1 + ctcGetStageBaseCount(g_MpSetup.stagenum)`.
- **N64 build remains byte-identical.** The `ctcteambase[4]` struct field is at the tail of `mpsetup` under `#ifndef PLATFORM_N64`; the menu entries are inside the existing port-only block; the engine hook is wrapped. No decomp-contract risk.
