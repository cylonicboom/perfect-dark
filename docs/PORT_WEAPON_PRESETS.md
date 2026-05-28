# Port-only Feature: Custom Weapon Presets

Adds saved Combat Sim weapon loadouts on top of the original "Weapon Set" dropdown. Selecting `Custom` in the Weapons menu opens a manager (New / Load / Rename / Delete); presets are persisted in `mpsetups.bin` v2+ and join the "Random Preset" rotation.

Each preset also stores a per-slot bitmask (`FNFLAG_PRIMARY_DISABLED` / `FNFLAG_SECONDARY_DISABLED`) that lets a saved preset ban primary or secondary function on individual weapons. The menu UI prevents both bits being set on the same slot, so the engine hooks don't have to handle that case.

Everything is gated under `#ifndef PLATFORM_N64`. N64 build is byte-identical.

---

## Surface

| File | Site | What |
|---|---|---|
| `src/include/constants.h` | bottom of MP section | `MPWEAPONPRESET_MAXNAME 17`, `MPWEAPONPRESET_MAXENTRIES 32`, `FNFLAG_PRIMARY_DISABLED 0x01`, `FNFLAG_SECONDARY_DISABLED 0x02` |
| `port/include/mpsetups.h` | bottom | `struct mpweaponpreset`, `g_MpWeaponPresets[]`, `g_MpWeaponPresetCount`, `g_MpSlotFnFlags[]`, `mpWeaponPreset{Find,Add,Replace,Rename,Delete}` decls |
| `port/src/mpsetups.c` | top | `g_MpWeaponPresets[]` / `g_MpWeaponPresetCount` definitions |
| `port/src/mpsetups.c` | file format header comment + version constant | `MPSETUP_VERSION` bumped to 2 (or higher); preset tail section documented |
| `port/src/mpsetups.c` | load / save paths | Read presets after `numsetups` setup blobs when `version >= 2`; write `numweaponpresets` + each preset blob |
| `port/src/mpsetups.c` | bottom | `mpWeaponPreset{Find,Add,Replace,Rename,Delete}` implementations |
| `src/game/mplayer/mplayer.c` | top of `mpApplyWeaponSet` | `g_MpSlotFnFlags[]` definition (port-only); reset all slots to 0 on entry |
| `src/game/mplayer/mplayer.c` | Random Preset branch | Extend the rotation pool with saved presets (`total = unlocked + g_MpWeaponPresetCount`); on hit, copy `weapons[]` + `slotfnflags[]` directly and return |
| `src/game/mplayer/setup.c` | new dialog block | Custom-preset manager: list / edit / save-name / rename / overwrite / delete / saved / maxed dialogs |
| `src/game/mplayer/setup.c` | `menuhandlerMpWeaponSetDropdown` `MENUOP_SET` | `if (value == WEAPONSET_CUSTOM) menuPushDialog(&g_MpCustomPresetsMenuDialog);` |
| `src/include/game/bondgun.h` | end of header | `bool bgunPrimaryFunctionDisabled(s32)`, `bool bgunSecondaryFunctionDisabled(s32)` prototypes |
| `src/game/bondgun.c` | new helpers | `mpSlotFlagsForWeapon(weaponnum)` (static), `bgunPrimary/SecondaryFunctionDisabled` |
| `src/game/bondgun.c` | `bgunSetState` `HANDSTATE_CHANGEFUNC` block | Refuse primary→secondary if `bgunSecondaryFunctionDisabled`; refuse secondary→primary if `bgunPrimaryFunctionDisabled` |
| `src/game/bondgun.c` | `bgunTickSwitch2` equip-init | Auto-flip a fresh equip to `FUNC_SECONDARY` when primary is gated and secondary isn't |
| `src/game/botinv.c` | `botinvSwitchToWeapon` | Clamp `funcnum` to FUNC_PRIMARY / FUNC_SECONDARY based on the slot's `FNFLAG_*` bits (also the GE-mode forced-primary clamp from `PORT_GOLDENEYE.md`) |

---

## Data model

```c
// port/include/mpsetups.h
struct mpweaponpreset {
    char name[MPWEAPONPRESET_MAXNAME + 1]; // null-terminated; +1 for the terminator
    u8   weapons[NUM_MPWEAPONSLOTS];       // same encoding as g_MpSetup.weapons (mpweapon idx)
    u8   slotfnflags[NUM_MPWEAPONSLOTS];   // FNFLAG_* per slot
};

extern u8                       g_MpWeaponPresetCount;
extern struct mpweaponpreset    g_MpWeaponPresets[MPWEAPONPRESET_MAXENTRIES];

// Per-slot fn-flags applied to the *currently-active* loadout. Live state,
// not persisted. Reset to all-zero at the top of every mpApplyWeaponSet
// call; populated when a saved preset's Load action runs.
extern u8 g_MpSlotFnFlags[NUM_MPWEAPONSLOTS];
```

Critical invariants:

- `name` is fixed-size and null-terminated; helpers `strncpy` and force `name[MAX] = '\0'`.
- A bit set in `slotfnflags[i]` only takes effect when slot `i` is occupied by the right weapon (`mpSlotFlagsForWeapon` walks `g_MpSetup.weapons[]` to find the slot at gate-eval time).
- The menu UI enforces "at least one function enabled per slot." If you bypass the UI (importing a preset file from elsewhere), you can produce both-disabled and the engine will silently allow primary because `bgunPrimaryFunctionDisabled` is checked first in the auto-flip path.

---

## File format (`mpsetups.bin`)

The original layout is in the header comment of `port/src/mpsetups.c`:

```
[version{1}] [defaultsetup{1}] [numsetups{1}]
[setup_1{80}] ... [setup_n{80}]
# v2+ tail
[numweaponpresets{1}]
[preset_1{sizeof(struct mpweaponpreset)}]
...
[preset_m{sizeof(struct mpweaponpreset)}]
```

Two version concerns:

1. **`MPSETUP_VERSION` constant in `mpsetups.c`** — the file version stamped into the byte-1 header. Bump this when adding the preset section (port-net-predict is at 3 because of the kohstatichill bit added by `PORT_KOH_STATIC_HILL.md`). Backwards-compat for older files lives in the read path:
   ```c
   if (setupfile->version >= 2) {
       fread(&npresets, sizeof(npresets), 1, f);
       ...
   }
   ```
2. **`mpsetupfileLoadWad`'s `version` parameter** — used by the per-setup wad encoding (see `PORT_KOH_STATIC_HILL.md`). Unrelated to preset section, but both share the same `setupfile->version` value at load time, so a single version constant covers both.

Truncate `g_MpWeaponPresetCount` to `MPWEAPONPRESET_MAXENTRIES` on read (defensive — corrupt files shouldn't OOB the array).

---

## Random Preset rotation (`mpApplyWeaponSet`)

The built-in sets table is `g_MpWeaponSets[]`. The Random Preset path counts unlocked built-ins, then picks one with `rngRandom() % unlocked`. To include saved presets:

```c
#ifndef PLATFORM_N64
    s32 total = unlocked + (s32)g_MpWeaponPresetCount;
#else
    s32 total = unlocked;
#endif
    if (total > 0) {
        s32 target = (s32)(rngRandom() % (u32)total);
#ifndef PLATFORM_N64
        if (target >= unlocked) {
            // Custom preset picked. Apply directly without recursing back
            // through mpApplyWeaponSet — would clobber g_MpSlotFnFlags via
            // the top-of-function reset. Then return so the random label
            // ("Random Preset") stays the displayed set name.
            const struct mpweaponpreset *p = &g_MpWeaponPresets[target - unlocked];
            for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
                g_MpSetup.weapons[i] = p->weapons[i];
                g_MpSlotFnFlags[i]   = p->slotfnflags[i];
            }
            return;
        }
#endif
        // ...original built-in selection loop...
    }
```

`mpApplyWeaponSet`'s top-of-function reset is what makes this safe:

```c
#ifndef PLATFORM_N64
    for (i = 0; i < NUM_MPWEAPONSLOTS; i++) {
        g_MpSlotFnFlags[i] = 0;
    }
#endif
```

Anyone applying a built-in set ends up with clean fn-flags; saved-preset loads write directly to `g_MpSlotFnFlags` after this reset, so the flags only apply when a preset is actively selected.

---

## Runtime hook helpers

In `bondgun.c`, the single choke points that every other gate site routes through:

```c
static u8 mpSlotFlagsForWeapon(s32 weaponnum)
{
    if (!g_Vars.normmplayerisrunning) return 0;
    for (s32 i = 0; i < NUM_MPWEAPONSLOTS; i++) {
        if (g_MpSlotFnFlags[i] == 0) continue;
        u8 mpweaponnum = g_MpSetup.weapons[i];
        if (g_MpWeapons[mpweaponnum].weaponnum == weaponnum) {
            return g_MpSlotFnFlags[i];
        }
    }
    return 0;
}

bool bgunSecondaryFunctionDisabled(s32 weaponnum)
{
    if (goldeneyeStyleActive()) return true;  // see PORT_GOLDENEYE.md
    if (mpSlotFlagsForWeapon(weaponnum) & FNFLAG_SECONDARY_DISABLED) return true;
    return false;
}

bool bgunPrimaryFunctionDisabled(s32 weaponnum)
{
    if (mpSlotFlagsForWeapon(weaponnum) & FNFLAG_PRIMARY_DISABLED) return true;
    return false;
}
```

Note that `goldeneyeStyleActive()` is OR'd into the secondary check but **not** the primary check. GE mode disables every weapon's secondary; it doesn't disable any weapon's primary.

The hooks plug into three engine sites: `bgunSetState` (player function-cycle button), `bgunTickSwitch2` (equip-time auto-flip), and `botinvSwitchToWeapon` (bot AI's single weapon-equip path). See `PORT_GOLDENEYE.md` section 8 for the bot-side detail — same hook, same engine behaviour, different driver.

---

## Menu structure (`setup.c`)

The Set=Custom dropdown push:

```c
case MENUOP_SET:
    mpSetWeaponSet(data->dropdown.value);
#ifndef PLATFORM_N64
    if (data->dropdown.value == WEAPONSET_CUSTOM) {
        menuPushDialog(&g_MpCustomPresetsMenuDialog);
    }
#endif
    break;
```

`g_MpCustomPresetsMenuDialog` is a single LIST (not selectables). Index 0 is "New"; indices 1..N are saved preset names. Putting "New" inside the LIST avoids trapping focus on selectables that the LIST can't yield to.

- **New** → `g_MpEditCustomPresetMenuDialog` (6 weapon-slot dropdowns + 6 fn-mode cyclers + Save + Back). Save opens `g_MpWeaponPresetSaveNameMenuDialog` (keyboard input).
- **Existing entry** → per-entry Manage dialog (Load / Rename / Delete). Load copies `weapons[]` + `slotfnflags[]` to `g_MpSetup` / `g_MpSlotFnFlags` and pops back to the Weapons menu.

Two module-scope statics tie the multi-step keyboard / confirm flow together:

```c
static s32  g_MpWeaponPresetSlotIndex = -1;        // -1 = saving a new entry
static char g_MpWeaponPresetNameBuf[MPWEAPONPRESET_MAXNAME + 1];
```

`g_MpWeaponPresetSlotIndex = -1` is the "saving a new entry" sentinel used by the save-name flow. Rename / Delete set it to the index they're operating on.

---

## Wire (none)

Custom presets are entirely host-side. Once a Custom loadout is applied, the result lives in `g_MpSetup.weapons` and the host's local `g_MpSlotFnFlags`. The wire fields that need to be synced to clients are:

- `g_MpSetup.weapons[]` — already synced in `SVC_STAGE_START`.
- `g_MpSlotFnFlags[]` — **not currently synced**. Clients evaluate their own `mpSlotFlagsForWeapon` against their local `g_MpSlotFnFlags` which is all-zero on the client. This is a known gap: the host's fn-flag restrictions only apply to host-side firing logic; clients see no restrictions. If you need full sync, append 6 bytes to `SVC_STAGE_START` after the existing weapons block and bump `NET_PROTOCOL_VER`.

The current state is acceptable because the dominant use case (GoldenEye Style preset) drives its restrictions through `goldeneyeStyleActive()` which **is** wire-synced via `MPOPTION_GOLDENEYE` in `g_MpSetup.options`. Pure fn-flag-only presets (no MPOPTION_GOLDENEYE) won't restrict client firing.

---

## Porting checklist

1. Add the four `#define`s to `constants.h`.
2. Add `struct mpweaponpreset`, the externs, and the helper prototypes to `mpsetups.h`.
3. Define `g_MpWeaponPresets` / `g_MpWeaponPresetCount` and implement the five helpers in `mpsetups.c`.
4. Bump `MPSETUP_VERSION`, extend the file format read/write paths with the `v2+` tail.
5. Add `g_MpSlotFnFlags[]` definition + per-`mpApplyWeaponSet` reset to `mplayer.c`. Extend Random Preset to include saved presets.
6. Add the menu dialogs + Set=Custom auto-push to `setup.c`.
7. Add the three `bgun*FunctionDisabled` helpers + their three gate sites in `bondgun.c`. Add prototypes to `bondgun.h`.
8. Add the `funcnum`-clamp in `botinv.c`'s `botinvSwitchToWeapon`.
9. (Optional) If you need fn-flag restrictions on remote clients too, sync `g_MpSlotFnFlags[]` in `SVC_STAGE_START` and bump `NET_PROTOCOL_VER`.

If your branch doesn't have netplay, skip step 9 entirely. If your branch doesn't have the GoldenEye Style feature, drop the `if (goldeneyeStyleActive())` line from `bgunSecondaryFunctionDisabled` and the helpers still work — they just respond only to the FNFLAG bits.
