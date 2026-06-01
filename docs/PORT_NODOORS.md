# Port-only Feature: No Doors

A Combat Sim option that removes ordinary doors from the arena — only locked
doors and lift doors survive, so maps play more open. Toggled from the **More
Options** menu ("No Doors") and applied at stage setup.

Everything is gated under `#ifndef PLATFORM_N64`. The N64 build is byte-identical.

---

> **⚠️ UPDATED (u64 `options` merge):** The separate `g_MpSetup.portoptions` overflow
> word described below **no longer exists.** `g_MpSetup.options` was widened to `u64`
> and `MPOPTION_NODOORS` now lives in **bit 32** of `options`:
> `#define MPOPTION_NODOORS 0x0000000100000000ULL`. Test it with
> `g_MpSetup.options & MPOPTION_NODOORS` (the old cross-field footgun is gone). It rides
> the 64-bit `options` on the wire (no separate `portoptions` bytes) and is saved in the
> high 32 bits of the inline options word (`MPSETUP_VERSION >= 6`; v5 saves are migrated
> on load). The menu still uses `menuhandlerMpCheckboxPortOption`, now retargeted to the
> high 32 bits of `options` (`param3 = MPOPTION_NODOORS >> 32`, shifted back up by 32).
> The sections below describe the original `portoptions` design for historical context.

## Why this one is different: the `portoptions` overflow word

Every other MP option is a bit in `g_MpSetup.options` (a `u32`). By the time this
feature landed, **that word was full**: the original game uses the lower 24 bits
(including the unnamed-but-live `MPOPTION_00010000` at bit 16) and the port had
already claimed the entire upper byte (`MPOPTION_CONTROLLERS_ONLY` … `_GOLDENEYE`,
`0x08000000`–`0x80000000`). There was no free, non-colliding bit.

So No Doors is the **first MP option to live in a separate `u32 g_MpSetup.portoptions`
field** — an overflow word for port-only options. Its bit is:

```c
#define MPOPTION_NODOORS 0x00000001 // portoptions, NOT options
```

> ⚠️ **Footgun:** `0x00000001` in `options` is `MPOPTION_ONEHITKILLS`. `MPOPTION_NODOORS`
> is only distinct because it lives in a different field. **Never** `&` it against
> `g_MpSetup.options` — always `g_MpSetup.portoptions`. Future port options that
> don't fit `options` go here too (bit 1, 2, …).

---

## Surface

| File | Site | What |
|---|---|---|
| `src/include/types.h` | `struct mpsetup`, port-only block (after `htmstaticpad`) | Add `u32 portoptions` field |
| `src/include/constants.h` | after the port `MPOPTION_*` block | `#define MPOPTION_NODOORS 0x00000001` in a labeled portoptions block |
| `src/game/setup.c` | `setupMarkLiftDoors()` + its call in `setupCreateProps` | Mark lift doors exempt, then skip non-lift unlocked doors. Guarded; `nodoors` is hard-`false` on N64 |
| `src/game/mplayer/setup.c` | `menuhandlerMpCheckboxPortOption` + `g_MpExtGameOptionsMenuItems[]` | New portoptions checkbox handler; "No Doors" menu item uses it (guarded) |
| `src/include/game/mplayer/setup.h` | after `menuhandlerMpCheckboxOption` decl | Declare `menuhandlerMpCheckboxPortOption` (guarded) |
| `src/game/mplayer/mplayer.c` | `mpsetupfileLoadWad` / `SaveWad` | Serialize `portoptions` (32 bits) at the tail under a `version >= 5` gate (read) and unconditionally (write) |
| `port/src/mpsetups.c` | `MPSETUP_VERSION` | Bump 4 → 5 |
| `port/src/net/netmsg.c` | `netmsgSvcStageStartWrite`/`Read` **and** `netmsgClcAdminSetupWrite`/`Read` | Write/read `portoptions` as a `u32` right after the `htmstaticpad` field |
| `port/include/net/net.h` | `NET_PROTOCOL_VER` | Bump 33 → 34 |

---

## Behavioural change (`src/game/setup.c`)

Two cooperating pieces inside `setupCreateProps`, both keyed off one `nodoors` flag:

```c
// just before the prop-creation loop
#ifndef PLATFORM_N64
    bool nodoors = (g_MpSetup.portoptions & MPOPTION_NODOORS) != 0;
    if (nodoors) {
        setupMarkLiftDoors();   // tag lift doors with door->extra1 = 1
    }
#else
    bool nodoors = false;       // keeps the OBJTYPE_DOOR case below compiling on N64
#endif

    // ... while (obj->type != OBJTYPE_END) ... case OBJTYPE_DOOR:
    // dont skip doors that are locked or are lift doors
    bool skipdoor = nodoors && g_Vars.normmplayerisrunning
                 && door->keyflags == 0 && door->extra1 == 0;
    if (!skipdoor && withobjs && (obj->flags2 & diffflag) == 0) {
        setupCreateDoor(door, index);
    }
```

- `setupMarkLiftDoors()` walks the setup, setting `door->extra1 = 1` on every
  lift door (and its sibling). `extra1` is otherwise unused on doors, so it's a
  free marker. **It only exempts lift doors — it does not skip anything.**
- The actual skip happens per-door in the `OBJTYPE_DOOR` case: an unlocked
  (`keyflags == 0`), non-lift (`extra1 == 0`) door in a running MP match isn't
  created.

> **Why `nodoors` needs the `#else false`:** the `OBJTYPE_DOOR` case references
> `nodoors` unconditionally, but `g_MpSetup.portoptions` doesn't exist on N64.
> Declaring `nodoors = false` in the `#else` keeps the door-skip expression
> compiling and reduces to original behaviour (nothing skipped). Don't delete the
> local thinking it's only used at the mark site — it's used twice.

`setupMarkLiftDoors()` is itself wrapped in `#ifndef PLATFORM_N64`; since its only
caller is inside the same guard, it isn't referenced on N64 (no unused-function).

---

## Menu wiring (`src/game/mplayer/setup.c`)

The generic `menuhandlerMpCheckboxOption` hardcodes `g_MpSetup.options`, so it
**can't** drive a `portoptions` bit. A port-only twin is added:

```c
#ifndef PLATFORM_N64
MenuItemHandlerResult menuhandlerMpCheckboxPortOption(s32 op, struct menuitem *item, union handlerdata *data)
{
    switch (op) {
    case MENUOP_GET:
        return (g_MpSetup.portoptions & item->param3) != 0;
    case MENUOP_SET:
        g_MpSetup.portoptions &= ~item->param3;
        if (data->checkbox.value) g_MpSetup.portoptions |= item->param3;
    }
    return 0;
}
#endif
```

It's identical to the options handler except for the field. The "No Doors"
checkbox in `g_MpExtGameOptionsMenuItems[]` carries `MPOPTION_NODOORS` in
`item->param3` and points at this handler, all inside `#ifndef PLATFORM_N64`.

> Using the options handler instead would silently toggle bit 0 of `options`
> (`MPOPTION_ONEHITKILLS`) when the user flips "No Doors".

---

## Save / load (`mpsetupfileLoadWad` / `SaveWad`)

```c
// Load (in the existing #ifndef PLATFORM_N64 block, after the v4 fields)
g_MpSetup.portoptions = 0;                                  // in the init group
if (version >= 5) {
    g_MpSetup.portoptions = savebufferReadBits(buffer, 32);
}

// Save (unconditional — the writer always emits the latest format)
savebufferOr(buffer, g_MpSetup.portoptions, 32);
```

32 bits so the whole overflow word round-trips (only bit 0 is used today, but
future port options share the field). Old v1–v4 setups load as `portoptions = 0`
(No Doors off) — backwards-compatible, no migration. Bump `MPSETUP_VERSION` to 5
in `port/src/mpsetups.c` so new saves advertise the field.

`portoptions` is reset to 0 only on the load path, matching the sibling port
fields (`kohstatichill` etc.) — `mpInit` doesn't touch them; they rely on the
zero-initialised global.

---

## Net wire (`netmsg.c`)

Synced in the same two messages that already carry the port-only static-spawn
fields, immediately after `htmstaticpad`:

```c
// SVC_STAGE_START (server -> client) and CLC_ADMIN_SETUP (admin -> server)
netbufWriteU32(dst, g_MpSetup.portoptions);   // write
g_MpSetup.portoptions = netbufReadU32(src);   // read
```

In `CLC_ADMIN_SETUP` the read goes into a local temporary first and is committed
only after the auth check passes (same read-then-commit pattern as the rest of
that handler). Wire reads aren't version-gated — mismatched protocol versions are
rejected at auth — so bump `NET_PROTOCOL_VER` to 34 instead.

`SVC_LOBBY_STATE` does **not** carry `portoptions` (it doesn't carry the other
port fields either); the lobby display doesn't need it.

---

## Determinism / sync (don't skip this)

No Doors changes **which props are created** at stage start. Prop `syncid`s are
assigned from prop order in `g_Vars.props` (`netSyncIdsAllocate`), so if the
server skips doors and a client doesn't (or vice-versa), every subsequent
`syncid` shifts and prop sync desyncs across the whole arena.

The invariant that prevents this: `SVC_STAGE_START` delivers `portoptions` to the
client **before** `mpStartMatch` runs, so both sides hit `setupCreateProps` with
the same value. The lobby UI only writes `portoptions` on the host; clients never
edit `g_MpSetup` while connected. If you reorder stage init, re-verify this.

---

## Porting checklist

1. Add `u32 portoptions` to `struct mpsetup` in `types.h` (under `#ifndef PLATFORM_N64`).
2. Add `#define MPOPTION_NODOORS 0x00000001` to `constants.h` (portoptions namespace).
3. Add `setupMarkLiftDoors` + the `nodoors` flag (with `#else false`) + the
   `OBJTYPE_DOOR` skip to `setup.c`.
4. Add `menuhandlerMpCheckboxPortOption` (+ header decl) and the guarded "No Doors"
   menu item in `mplayer/setup.c`.
5. Append the 32-bit serializer to `mpsetupfileLoadWad`/`SaveWad`, bump `MPSETUP_VERSION`.
6. Append `portoptions` to `SVC_STAGE_START` and `CLC_ADMIN_SETUP` read/write, bump `NET_PROTOCOL_VER`.

If your branch doesn't have netplay, skip step 6. If it doesn't have versioned
mpsetup files, skip step 5 (the field becomes session-only). The `portoptions`
field + handler are reusable: any future port option that won't fit `options`
just claims the next bit (0x2, 0x4, …) and reuses `menuhandlerMpCheckboxPortOption`.
