# Port-only Feature: KotH Static Hill

Lets the host pick a specific hill on King of the Hill instead of the engine randomising between the per-stage hillpads. The original game cycled all hillpads when `MPOPTION_KOH_MOBILEHILL` was set or randomised once at match start when it was clear; the port adds a per-setup `kohstatichill` value (0 = Random, 1..N = pick `hillpads[N-1]`) and a UI to set it.

Everything is gated under `#ifndef PLATFORM_N64`. The N64 build is byte-identical.

---

## Surface

| File | Site | What |
|---|---|---|
| `src/include/types.h` | `struct mpsetup` (~line 4141, after `fileguid`) | Append `u8 kohstatichill` field |
| `src/game/mplayer/scenarios/kingofthehill.inc` | top of file | `kohGetStageHillCount(stagenum)`, `menuhandlerMpKohHillMode`, `menuhandlerMpKohStaticHill` |
| `src/game/mplayer/scenarios/kingofthehill.inc` | `g_KohOptionsMenuItems[]` | Replace the existing `MPOPTION_KOH_MOBILEHILL` checkbox with port-only "Hill Mode" + "Static Hill" dropdowns, with the original checkbox kept inside the `#else` branch so the N64 layout doesn't change |
| `src/game/mplayer/scenarios/kingofthehill.inc` | `kohInitProps` | Honour `g_MpSetup.kohstatichill` before the `rngRandom()` hill pick |
| `src/game/mplayer/mplayer.c` | `mpsetupfileLoadWad` / `SaveWad` | Serialize `kohstatichill` (4 bits) at the tail under a `version >= 3` gate (read) and unconditionally (write) |
| `port/src/mpsetups.c` | `MPSETUP_VERSION` | Bump file version (currently 3 — covers presets + kohstatichill) |
| `port/src/net/netmsg.c` | `netmsgSvcStageStartWrite` / `Read` | Write/read `g_MpSetup.kohstatichill` as a `u8` immediately after the existing `weapons[]` block |
| `port/include/net/net.h` | `NET_PROTOCOL_VER` | Bump by 1 (any wire change requires this) |

No new MPOPTION bit. `kohstatichill` is a separate `u8` field in `mpsetup`, not a bit in `options`.

---

## Behavioural change

### `kohInitProps` (`kingofthehill.inc`)

```c
if (g_ScenarioData.koh.hillcount > 1) {
#ifndef PLATFORM_N64
    if (g_MpSetup.kohstatichill > 0 && g_MpSetup.kohstatichill <= g_ScenarioData.koh.hillcount) {
        g_ScenarioData.koh.hillindex = g_MpSetup.kohstatichill - 1;
    } else
#endif
    {
        g_ScenarioData.koh.hillindex = rngRandom() % g_ScenarioData.koh.hillcount;
    }
    pad_id = g_ScenarioData.koh.hillpads[g_ScenarioData.koh.hillindex];
}
```

The `else` keeps the original `rngRandom()` consumer in place, so RNG state advances identically on N64 / port-Random. The static path **skips** `rngRandom()`, which is why both server and client must agree on `kohstatichill` *before* `kohInitProps` runs — see "Determinism" below.

### Hill counts per stage (`kohGetStageHillCount`)

Hand-pulled from the stage setup files. Don't try to derive at runtime — `g_ScenarioData.koh.hillcount` is only populated after the stage loads, which is too late for the lobby UI's dropdown range.

```
RAVINE 5, COMPLEX 5, G5BUILDING 5, TEMPLE 4, PIPES 5, SKEDAR 5,
BASE 4, AREA52 5, WAREHOUSE 4, CARPARK 5, RUINS 7, SEWERS 4,
FELICITY 4, FORTRESS 5, VILLA 4, GRID 4.
```

### Menu wiring (`g_KohOptionsMenuItems[]`)

The original N64 layout has one `MPOPTION_KOH_MOBILEHILL` checkbox in this slot. Replace it with `#ifndef PLATFORM_N64` two dropdowns + `#else` original checkbox, like:

```c
#ifndef PLATFORM_N64
    { MENUITEMTYPE_DROPDOWN, ..., "Hill Mode",   0, menuhandlerMpKohHillMode },
    { MENUITEMTYPE_DROPDOWN, ..., "Static Hill", 0, menuhandlerMpKohStaticHill },
#else
    { MENUITEMTYPE_CHECKBOX, ..., L_MPMENU_..., MPOPTION_KOH_MOBILEHILL, menuhandlerMpCheckboxOption },
#endif
```

`menuhandlerMpKohHillMode` is a 2-option dropdown ("Mobile" / "Static") that flips `MPOPTION_KOH_MOBILEHILL` in `g_MpSetup.options` — semantically equivalent to the original checkbox.

`menuhandlerMpKohStaticHill`:
- `MENUOP_CHECKDISABLED` greys it out when `MPOPTION_KOH_MOBILEHILL` is set (Mobile + Static doesn't make sense)
- `MENUOP_GETOPTIONCOUNT` returns `1 + kohGetStageHillCount(g_MpSetup.stagenum)` so the dropdown range follows the stage
- `MENUOP_GETOPTIONTEXT` returns `"Random"` for index 0, `"Hill 1".."Hill N"` for the rest (via a `static char label[16]`)
- `MENUOP_SET` writes `data->dropdown.value` to `g_MpSetup.kohstatichill`
- `MENUOP_GETSELECTEDINDEX` clamps stale values to 0 when the stage changed underneath the user

---

## Save / load (per-setup, in `mpsetupfileLoadWad` / `SaveWad`)

`mpsetup` blocks are versioned per the containing file's version. Append at the tail:

```c
// Load
#ifndef PLATFORM_N64
    g_MpSetup.kohstatichill = 0;
    if (version >= 3) {
        g_MpSetup.kohstatichill = savebufferReadBits(buffer, 4);
    }
#endif

// Save
#ifndef PLATFORM_N64
    savebufferOr(buffer, g_MpSetup.kohstatichill, 4);
#endif
```

4 bits is enough (max hill count is 7 on RUINS). Old v1/v2 files load as `kohstatichill = 0` (Random) — backwards-compatible without explicit migration.

Bump `MPSETUP_VERSION` in `port/src/mpsetups.c` if your branch's current value is < 3. If you only need this one field and don't have the weapon-presets section, you could go straight from your current version to (current+1) — just keep the load-side `version >= N` gate matching.

---

## Net wire (`netmsgSvcStageStartWrite` / `Read`)

```c
// Write — server side, in netmsgSvcStageStartWrite
netbufWriteData(dst, g_MpSetup.weapons, sizeof(g_MpSetup.weapons));
netbufWriteU8(dst, g_MpSetup.kohstatichill);    // <-- new

// Read — client side, in netmsgSvcStageStartRead
netbufReadData(src, g_MpSetup.weapons, sizeof(g_MpSetup.weapons));
g_MpSetup.kohstatichill = netbufReadU8(src);    // <-- new
```

Bump `NET_PROTOCOL_VER` in `port/include/net/net.h` by 1.

---

## Determinism (don't skip this)

The static-pick path **skips `rngRandom()`**. If server and client disagree on `kohstatichill` at the moment `kohInitProps` runs, their `g_RngSeed` advances by different counts, and every subsequent RNG consumer (weapon spawns, sim AI decisions, etc.) desyncs.

Two safeguards together prevent that:

1. The wire field is read in `netmsgSvcStageStartRead` **before** `mpStartMatch` runs, so the client has the server's value before any scenario-init code touches RNG.
2. The lobby UI only writes `kohstatichill` on the host. Clients never edit `g_MpSetup` while connected — they wait for `SVC_STAGE_START`.

If you change the order of init (e.g. move `kohInitProps` earlier), re-verify these two invariants still hold.

---

## Porting checklist

1. Add `u8 kohstatichill` to `struct mpsetup` in `types.h` (under `#ifndef PLATFORM_N64`).
2. Add `kohGetStageHillCount` + the two handlers + the menu entries to `kingofthehill.inc`.
3. Patch `kohInitProps` with the static-pick branch.
4. Append the 4-bit serializer to `mpsetupfileLoadWad` / `SaveWad`, bump `MPSETUP_VERSION`.
5. Append `kohstatichill` to `SVC_STAGE_START` read/write, bump `NET_PROTOCOL_VER`.

If your branch doesn't have netplay, skip step 5. If it doesn't have versioned mpsetup files, skip step 4 (the field becomes session-only).
