# Port-only Feature: HTB / HTM Static Spawn Pins

Lets the host pin where the token spawns in Hold the Briefcase (briefcase) and Hack that Mac / Hacker Central (data uplink), instead of taking the engine's random pick from the stage's intro-defined pad list or its ammocrate-replacement scan.

One "Static Spawn" dropdown per scenario, Random + Pad 1..N. Same UI shape as `PORT_KOH_STATIC_HILL` (single dimension) — not the four-per-team shape used by [`PORT_CTC_STATIC_BASE`](PORT_CTC_STATIC_BASE.md), because HTB and HTM each spawn a single token with no per-team semantics.

Everything is gated under `#ifndef PLATFORM_N64`; the N64 build is byte-identical.

See [`PORTING_HOWTO.md`](PORTING_HOWTO.md) for the cross-cutting porting methodology this sits on top of.

---

## The "28 pads" constant

All CTC-supported MP stages (HTB and HTM use the same stage set) feed exactly **4 `INTROCMD_CASE` + 24 `INTROCMD_CASERESPAWN` = 28 entries** through their `*AddPad` paths via `scenarios.c:860`. Verified across all 10 stages (`mp_setupmp{1,3,4,5,9,10,11,12,13,15}.c`). So `padnums[0..27]` is the stable, intro-defined range.

HTM additionally appends runtime-discovered `OBJTYPE_MULTIAMMOCRATE` pads (in `htmInitProps`, lines ~309-323) at `padnums[28+]`. Those are stage-state-dependent and not exposed in the dropdown — the menu only covers the 28 intro-defined pads.

Both scenarios use `HTB_MAX_STATIC_PAD = 28` / `HTM_MAX_STATIC_PAD = 28` as the dropdown range. Out-of-range pin values fall through to Random via the runtime gate.

---

## Surface

| File | Site | What |
|---|---|---|
| `src/include/types.h` | `struct mpsetup` tail, under `#ifndef PLATFORM_N64` | Append `u8 htbstaticpad` and `u8 htmstaticpad` (after `ctcteambase[4]`) |
| `src/game/mplayer/scenarios/holdthebriefcase.inc` | top of file, under `#ifndef PLATFORM_N64` | `HTB_MAX_STATIC_PAD` define + `menuhandlerMpHtbStaticPad` |
| `src/game/mplayer/scenarios/holdthebriefcase.inc` | inside `g_HtbOptionsMenuItems[]`'s existing port-only block | "Static Spawn" `MENUITEMTYPE_DROPDOWN` entry |
| `src/game/mplayer/scenarios/holdthebriefcase.inc` | `htbCreateToken` | Bypass ammocrate scan + rngRandom fallback when `htbstaticpad > 0` and within `nextindex` |
| `src/game/mplayer/scenarios/hackthatmac.inc` | top of file, under `#ifndef PLATFORM_N64` | `HTM_MAX_STATIC_PAD` define + `menuhandlerMpHtmStaticPad` |
| `src/game/mplayer/scenarios/hackthatmac.inc` | inside `g_HtmOptionsMenuItems[]`'s existing port-only block | "Static Spawn" dropdown |
| `src/game/mplayer/scenarios/hackthatmac.inc` | `htbCreateUplink` (misnamed — actually creates the HTM uplink) | Same bypass pattern as HTB |
| `src/game/mplayer/mplayer.c` | `mpsetupfileLoadWad` (gated `version >= 4`) and `SaveWad` | Serialize each field as 6 bits |
| `port/src/mpsetups.c` | `MPSETUP_VERSION` | Bumped to `4` (shares the bump with the CTC pins) |
| `port/src/net/netmsg.c` | `netmsgSvcStageStart{Write,Read}` | Write/read 2 bytes after the CTC team-base bytes |
| `port/include/net/net.h` | `NET_PROTOCOL_VER` | Bumped to `29` (shares the bump with the CTC pins) |

---

## Menu handler shape

Both handlers are structurally identical — just different field names:

```c
MenuItemHandlerResult menuhandlerMpHtbStaticPad(s32 operation, struct menuitem *item, union handlerdata *data)
{
    static char label[16];
    switch (operation) {
    case MENUOP_GETOPTIONCOUNT:
        data->dropdown.value = 1 + HTB_MAX_STATIC_PAD;     // Random + Pad 1..28
        break;
    case MENUOP_GETOPTIONTEXT:
        if (data->dropdown.value == 0) return (uintptr_t)"Random";
        sprintf(label, "Pad %u", (unsigned)data->dropdown.value);
        return (uintptr_t)label;
    case MENUOP_SET:
        g_MpSetup.htbstaticpad = (u8)data->dropdown.value;
        break;
    case MENUOP_GETSELECTEDINDEX:
        if (g_MpSetup.htbstaticpad > HTB_MAX_STATIC_PAD) data->dropdown.value = 0;
        else data->dropdown.value = g_MpSetup.htbstaticpad;
        break;
    }
    return 0;
}
```

No `MENUOP_CHECKDISABLED` — these dropdowns are always interactive. Unlike CTC's per-team dropdowns, there's no "team has no players" condition to grey out.

---

## Engine hooks

### HTB (`htbCreateToken`)

The existing code tries an ammocrate-replacement scan first, falling back to `padnums[rngRandom() % nextindex]`. The pin bypasses both:

```c
#ifndef PLATFORM_N64
if (g_MpSetup.htbstaticpad > 0
        && g_MpSetup.htbstaticpad <= g_ScenarioData.htb.nextindex) {
    g_ScenarioData.htb.tokenpad = g_ScenarioData.htb.padnums[g_MpSetup.htbstaticpad - 1];
} else
#endif
if (count > 0) {
    // ...existing ammocrate-replace path (sets var800869ec, hides the crate)...
} else if (g_ScenarioData.htb.nextindex > 0) {
    g_ScenarioData.htb.tokenpad = g_ScenarioData.htb.padnums[rngRandom() % g_ScenarioData.htb.nextindex];
} else {
    g_ScenarioData.htb.tokenpad = 0;
}
```

The `<= nextindex` bound is the engine gate: if a user-pinned value exceeds the live padnums count (shouldn't happen with the 28-pad constant since every stage has all 28, but defensive), it falls through to the existing random branches. Pinning **skips** both `count = rngRandom() % count` (only reached when `count > 0`) and the fallback `rngRandom() % nextindex`. The ammocrate-scan loop itself doesn't consume RNG.

### HTM (`htbCreateUplink`)

The function name is `htbCreateUplink` — that's a naming quirk in the decompile, the function actually creates the HTM uplink. Same pattern:

```c
#ifndef PLATFORM_N64
if (g_MpSetup.htmstaticpad > 0
        && g_MpSetup.htmstaticpad <= g_ScenarioData.htm.numpads) {
    padnum = g_ScenarioData.htm.padnums[g_MpSetup.htmstaticpad - 1];
} else
#endif
if (count > 0) {
    // ...existing ammocrate-replace path...
} else if (g_ScenarioData.htm.numpads > 0) {
    padnum = g_ScenarioData.htm.padnums[rngRandom() % g_ScenarioData.htm.numpads];
} else {
    padnum = 0;
}
```

`numpads` is HTM's equivalent of HTB's `nextindex` — the live count of pads in `padnums[]` at the time `htbCreateUplink` runs (intro-defined pads + runtime ammocrate pads).

**Only the uplink is pinned, not the terminal.** The terminal placement happens in `htmInitProps` (lines 333-344) via a separate shuffle that picks `HTM_NUM_TERMINALS = 1` pad. Leaving that random keeps the gameplay loop intact — players still have to traverse to the terminal.

---

## Save / wire surface

### `mpsetup` wad (6 bits each)

```c
// Load — gated on version >= 4
#ifndef PLATFORM_N64
    g_MpSetup.htbstaticpad = 0;
    g_MpSetup.htmstaticpad = 0;
    if (version >= 4) {
        // ...CTC bytes first (see PORT_CTC_STATIC_BASE.md)...
        g_MpSetup.htbstaticpad = savebufferReadBits(buffer, 6);
        g_MpSetup.htmstaticpad = savebufferReadBits(buffer, 6);
    }
#endif

// Save — always written
#ifndef PLATFORM_N64
    // ...CTC bytes first...
    savebufferOr(buffer, g_MpSetup.htbstaticpad, 6);
    savebufferOr(buffer, g_MpSetup.htmstaticpad, 6);
#endif
```

6 bits handles values 0..63, more than enough for the 0..28 range. The `padnums[60]` array size is the absolute upper bound the engine could ever populate, well within 6 bits.

### `SVC_STAGE_START` wire (2 bytes)

```c
// Write — after CTC's 4 team-base bytes
netbufWriteU8(dst, g_MpSetup.htbstaticpad);
netbufWriteU8(dst, g_MpSetup.htmstaticpad);

// Read — same position
g_MpSetup.htbstaticpad = netbufReadU8(src);
g_MpSetup.htmstaticpad = netbufReadU8(src);
```

`NET_PROTOCOL_VER` was bumped to 29 in the same commit as the CTC pins. Single bump covers both.

---

## Determinism

Pinning skips:

- The `count = rngRandom() % count` line in the ammocrate-replace path (only reached when ammocrate candidates exist).
- The `padnums[rngRandom() % …]` line in the fallback path.

Either way, RNG advance differs from the unpinned path. Server and client must agree on `htbstaticpad` / `htmstaticpad` **before** `htbCreateToken` / `htbCreateUplink` runs. The wire fields are read in `netmsgSvcStageStartRead` ahead of `mpStartMatch`, satisfying this.

The HTM terminal-placement shuffle in `htmInitProps` (separate from the uplink path) is NOT touched — it still consumes `rngRandom() % numpads` at its own rate. The terminal stays unpinned by design.

---

## Porting checklist

1. **Storage** — Add `u8 htbstaticpad` and `u8 htmstaticpad` to `struct mpsetup` in `src/include/types.h` under `#ifndef PLATFORM_N64`.
2. **HTB handler + menu + engine hook** — Add `HTB_MAX_STATIC_PAD` define, `menuhandlerMpHtbStaticPad`, and the "Static Spawn" dropdown entry to `holdthebriefcase.inc`. Patch `htbCreateToken`.
3. **HTM handler + menu + engine hook** — Add `HTM_MAX_STATIC_PAD` define, `menuhandlerMpHtmStaticPad`, and the "Static Spawn" dropdown entry to `hackthatmac.inc`. Patch `htbCreateUplink` (yes, the function with the `htb` prefix is the one for HTM — don't be misled by the name).
4. **Wad save / load** — Bump `MPSETUP_VERSION` and extend `mpsetupfileLoadWad` (gated `version >= N`) / `SaveWad` in `mplayer.c`. Append 6 bits each, after any CTC team-base bytes you're also adding.
5. **Wire** — Append 2 bytes to `netmsgSvcStageStartWrite` / `Read` after any CTC team-base bytes. Bump `NET_PROTOCOL_VER` (already covered if you bumped it for the CTC pins).

If your branch has no netplay, skip step 5. If your branch has no versioned mpsetup files, skip step 4. If you only want one of HTB/HTM, drop the other field + handler + entry + serialization — they're independent.

---

## Porting gotchas

- **`htbCreateUplink` is the HTM function.** Decomp naming quirk. Don't accidentally patch HTB's `htbCreateToken` thinking you're patching HTM, or vice versa — they're in different `.inc` files (`holdthebriefcase.inc` vs `hackthatmac.inc`) but the HTM one carries the `htb` prefix in its name.
- **The 28-pad max is empirical, not enforced by the engine.** All CTC-supported stages happen to define 4 case + 24 case_respawn entries. If a future stage author adds CTC support with a different layout (more case_respawn entries, fewer, whatever), the dropdown range stays at 28 unless you bump the constant. The runtime gate (`<= nextindex` / `<= numpads`) still does the right thing — pins beyond the live range fall through to Random.
- **HTM has runtime-added pads beyond index 28.** `htmInitProps` scans `OBJTYPE_MULTIAMMOCRATE` props and appends their pad numbers to `padnums[28+]`. Those positions are stage-state-dependent and the dropdown doesn't expose them. If you want to let users pin to ammocrate positions, you'd need a much higher max + the runtime gate becomes the only real bound. Probably not worth it — the ammocrate set is unstable across stage variants.
- **Only the HTM uplink is pinned, not the terminal.** The terminal placement is a separate shuffle in `htmInitProps`. Don't try to extend the pin to the terminal without thinking about gameplay impact — pinning both turns HTM into a fully deterministic stage which removes the traversal element.
- **HTB's `tokenpad = 0` final fallback is reachable when no candidates and no padnums exist.** That's the original behaviour (briefcase spawns at pad 0, wherever that is on the stage). The static pin doesn't change this — if your pin is in-range it always wins; if it's out-of-range the original chain takes over.
- **The dropdown doesn't tell the user which pad is which.** "Pad 14" is meaningful to the engine but not to a human. There's no in-game preview; the user picks blind and starts the match to see where the token lands. If your branch has a stage map renderer or pad debug overlay, hooking that up to the menu would be a nice UX upgrade — out of scope here.
- **The `padnums[]` array is `padnums[60]` for both HTB and HTM.** 6 bits in the wad encoding handles the full range. If you ever bump `padnums[]` to a larger size you'll need to widen the wad encoding (and bump the wad version).
- **Pinning bypasses the ammocrate-replacement entirely.** The original game uses this mechanism to hide an ammocrate that the briefcase / uplink will occupy. With a pin, no ammocrate is hidden — the chosen pad just gets the token placed on top of whatever was there. If a pad happens to coincide with an ammocrate position, both will render. This is a tolerable cosmetic quirk; documented here so future readers know it's intentional.
- **N64 build remains byte-identical.** Both fields are at the tail of `mpsetup` under `#ifndef PLATFORM_N64`; both handlers and dropdown entries are inside the existing port-only blocks; both engine hooks are wrapped.
