# Porting Port-only Features Into Another Branch

> **POLICY NOTE (2026-07-31):** this repo adopted a **port-first policy** — the N64
> byte-matching contract is retired (see the root CLAUDE.md "PORT-FIRST POLICY").
> Symbol names stay; struct layouts may change except serialized formats; netplay
> determinism is the binding contract. The guard-pattern guidance below remains
> accurate for lifting features into forks that still target N64 or matching.

This is the methodology to follow when lifting any of the port-only features in this repo (GoldenEye Style, KotH Static Hill, Custom Weapon Presets, No Room Culling, Host Spectator, etc.) into a different fork or branch. For per-feature specifics, see `docs/PORT_*.md`.

Read this once, then use it as a checklist alongside the per-feature doc.

---

## 1. The decompilation contract — never rename `src/` symbols

`src/game/`, `src/lib/`, `src/include/` are decompiled N64 C. **Every identifier maps to an address in the original binary.** Renaming a symbol — even a static helper — silently breaks the decompilation contract for anyone diffing against the upstream `n64decomp/perfect_dark` repo.

Rules:
- **Don't rename anything in `src/`.** Even if a name is unhelpful (`func0f0d49c8`, `unk5d5_06`). Add a comment if needed, leave the name alone.
- **Adding new symbols is fine** — port-only helpers, new structs, new fields. Use `#ifndef PLATFORM_N64` so the N64 build doesn't see them.
- **`port/` is yours.** Rename freely.

The same rule applies to struct fields. If a field is `unk5d5_06`, the offset matters; don't reorder or rename. Port-only fields go at the **tail** of a struct, under `#ifndef PLATFORM_N64`.

---

## 2. The two guard patterns

Almost every port-only change in `src/` falls into one of two patterns. Pick the right one and use it consistently — mixing them confuses future readers.

### `#ifndef PLATFORM_N64` — for code that must be absent on N64

Use when:
- The change touches a struct (adding a new field).
- The change introduces a new function or static helper.
- The change references a symbol that doesn't exist on N64 (`g_NetMode`, `g_MpSetup.kohstatichill`, `goldeneyeStyleActive`, etc.).

```c
#ifndef PLATFORM_N64
    if (g_MpSetup.options & MPOPTION_GOLDENEYE) {
        // GE-mode behaviour
    }
#endif
```

### `if (g_NetMode != NETMODE_CLIENT)` — for server-only logic in code that *is* compiled on N64

Use when:
- The change is purely netplay-related but the surrounding code path is otherwise unchanged.
- You're gating a write to authoritative state (score, hit registration, sim AI tick).

```c
if (g_NetMode != NETMODE_CLIENT) {
    // server-only — clients get this state from SVC_*
}
```

**`NETMODE_SERVER` includes the host running locally.** Use `!= NETMODE_CLIENT` to mean "server or singleplayer," not `== NETMODE_SERVER`.

When both apply (port-only AND server-only), nest:

```c
#ifndef PLATFORM_N64
    if (g_NetMode != NETMODE_CLIENT) { ... }
#endif
```

---

## 3. MPOPTION budget — the field is now `u64`

`g_MpSetup.options` is a **`u64`** (it was a `u32` until the upper byte filled up, then was widened unconditionally — N64 included). The original game uses the lower 24 bits; the upper byte of the low word (`0xff000000`) **and all of bits 32-63** are port-only territory (N64 ignores them).

Low-word port bits in use on `port-net-predict`:

| Bit | Symbol | Used by |
|---|---|---|
| `0x10000000` | *(reserved — retired `MPOPTION_NOCULL`)* | `docs/PORT_NO_CULLING.md` (retired; do not reuse) |
| `0x20000000` | *(reserved — retired `MPOPTION_NOOMLIMIT`)* | `docs/PORT_NO_CULLING.md` (retired; do not reuse) |
| `0x40000000` | `MPOPTION_HOSTSPECTATOR` | `docs/PORT_HOST_SPECTATOR.md` |
| `0x80000000` | `MPOPTION_GOLDENEYE` | `docs/PORT_GOLDENEYE.md` |

High-word bits (32-63) in use:

| Bit | Symbol | Used by |
|---|---|---|
| `0x0000000100000000` | `MPOPTION_NODOORS` | `docs/PORT_NODOORS.md` |
| `0x0000000200000000` | `MPOPTION_OWNEDROOMSPAWN` | `docs/PORT_GRAFFITI.md` |
| bits 34-45 | `MPOPTION_CLASSIC_*` (12 bits) | `docs/PORT_GOLDENEYE.md` (Classic Options) |

**Adding a new port-only MP option:** claim the next free high-word bit, written with a `ULL` suffix (e.g. `0x0000000200000000ULL`). It then flows automatically over the wire (the 64-bit `options` is serialized in `SVC_STAGE_START` / `CLC_ADMIN_SETUP` / `SVC_LOBBY_STATE`) and to the mpsetups.bin wad (the inline 64-bit `options` write). Plumb it through the same choke points as `MPOPTION_NODOORS` — search the codebase for it as the worked example. Three gotchas:

- **Menus can't carry a >32-bit mask in `menuitem.param3`** (it's 32-bit). High-word checkboxes use `menuhandlerMpCheckboxPortOption`, which shifts a 32-bit `param3` up by 32; the menu item passes `MYOPTION >> 32`.
- **The per-setup wad block (`MPSETUP_BLOCKSIZE` = 80 bytes) is ~99% full** (~10 spare bits). A new *bit* on the existing 64-bit `options` costs nothing extra to save, but a new *field* needs the block enlarged (a wad-format migration).
- **The dedicated-server playlist** (`port/src/net/playlist.c`) has its own 64-bit option vocabulary (`s_options` / `struct namedoption`). Add your option's name there so `options=` can set it.

When porting to another branch, **use the same bit values** — save/wire compatibility depends on bit positions. If your target branch still has a 32-bit `options`, either widen it the same way or move state out of `options` entirely — like `kohstatichill` (a `u8` field, not a bit).

---

## 4. NET_PROTOCOL_VER — bump on every wire change

Constant lives in `port/include/net/net.h`. Every change to the wire format requires a bump. "Wire format" means:

- Adding / removing a field in any `SVC_*` or `CLC_*` message read/write.
- Changing the encoding (u8 → u16, big-endian → little-endian, etc.).
- Adding a new `SVC_*` or `CLC_*` message ID.

The server's `SVC_AUTH` enforces an exact match. There is no client/server forward-compat path — a mismatched protocol version refuses the connection. So no need to gate new fields behind "if version >= N" reads, just bump the constant.

Server query packets (`PDQM\x01`) include the protocol version in their payload so server browsers can show mismatched servers as such.

**Don't bundle multiple feature commits into one version bump.** Each feature that touches the wire gets its own bump in the commit that adds the wire fields. Easier to bisect.

---

## 5. Savefile / wad version bumps

`mpsetups.bin` has its own version (`MPSETUP_VERSION` in `port/src/mpsetups.c`). Each setup wad inside the file is decoded by `mpsetupfileLoadWad(buffer, version)` where `version` is the *file* version.

When adding a new per-setup field:
- Bump `MPSETUP_VERSION`.
- Write the new field **at the tail** of the wad encoding (always — old files don't have it).
- Read the new field **gated on `version >= N`**, with a default initialization above the gate:

```c
g_MpSetup.kohstatichill = 0;          // default for old files
if (version >= 3) {
    g_MpSetup.kohstatichill = savebufferReadBits(buffer, 4);
}
```

When adding a new section to the **file** (e.g. weapon presets at the tail of the file, after all setup blobs):
- Same version bump.
- Section is read after the existing setups, gated on `setupfile->version >= N`.

Old files load fine; new fields default to zero. Don't ever reorder existing fields or insert mid-stream — that breaks every existing save.

---

## 6. Deterministic invariants

Several systems on `port-net-predict` rely on **server and client running identical code paths with identical inputs.** Breaking any of these silently desyncs the match:

### RNG seed parity

`g_RngSeed` and `g_Rng2Seed` are snapshotted in `SVC_STAGE_START`. Both sides advance them by calling `rngRandom()` the same number of times. Any port-only change that conditionally calls `rngRandom()` on one side but not the other is a bug.

Concrete example: KotH Static Hill (`kohInitProps`). The static-pick path **skips** `rngRandom()`. The wire field is read in `SVC_STAGE_START` *before* `mpStartMatch`, so client and host agree on whether to skip before they touch RNG. If you add similar conditional RNG consumers, do the same: ensure the gating value is on the wire and applied early.

### Bot allocation

`botmgrAllocateBot` uses `g_NetRngSeeds[]`. The function must run the same way on server and client to produce identical `g_MpBotChrPtrs[]` (sim chr identity depends on this — props are referenced by `syncid`, which is a sequential index into the per-prop allocation).

**Don't insert allocations between `netClientSyncRng()` and `botmgrAllocateBot`.** Any heap allocation that happens on one side but not the other shifts subsequent syncids and breaks every sim reference.

### `g_BotConfigsArray` arrival before `mpStartMatch`

The wire path is `SVC_STAGE_START` writes all `MAX_BOTS` slots (head/body/team/type/difficulty/name) at the end of the packet; the client reads them into `g_BotConfigsArray` before `mpStartMatch` runs. If you add a new bot config field, **also** add it to the wire — otherwise clients use stale local lobby defaults and sims spawn wrong.

### `chr->actiontype` is NOT synced — intentionally

Client always dispatches sim `chrTick` as `ACT_STAND`. Don't try to sync actiontype — the per-action union data is uninitialised on the client and causes crashes. Visible animation comes from synced `animnum`, not from action ticks.

---

## 7. Single helpers as choke points

When a feature gates multiple engine sites on the same condition, **put the condition behind one helper** and route every site through it. Future related options OR additional terms into the helper without re-threading every call site.

Canonical examples on `port-net-predict`:

```c
bool goldeneyeStyleActive(void);           // checks MPOPTION_GOLDENEYE + CHEAT_GOLDENEYE
bool bgunSecondaryFunctionDisabled(s32);   // ORs GE + per-slot FNFLAG
bool bgunPrimaryFunctionDisabled(s32);     // per-slot FNFLAG only (so far)
bool bgunDualWieldDisabled(void);          // GE only (so far)
bool bgunCurrentPlayerInIframe(void);      // GE i-frame state
```

The pattern: one declaration in the appropriate header (`src/include/game/<sub>.h`), one definition in the matching `.c`, every gate site is `if (helper())`. When a new related option arrives, add its term inside the helper. No call-site changes needed.

This is more important than it sounds. The first version of GE Style had per-site gates; consolidating them into helpers shrank the diff by ~40% and made the Custom Weapon Presets work plug in cleanly.

---

## 8. Pick a per-feature doc shape

Every port-only feature in this repo has its own `docs/PORT_*.md`. The shape is:

1. **One-paragraph summary** — what it does, what gates it on/off, which build sees it.
2. **Surface table** — file × site × what changed. This is the porting checklist content.
3. **Behavioural changes** — each engine hook explained with code excerpts where the hook is non-obvious.
4. **Wire / save / sync surface** — if any. Otherwise note "no wire" / "no save."
5. **Determinism notes** — anything subtle about RNG / order / state.
6. **Porting checklist** — numbered, ordered, copy-pasteable.

Match this shape when adding a new feature. The reviewer (future you, or someone forking) wants to scan the surface table and jump to whichever section is non-obvious. Don't bury the wire section in prose — make it findable.

---

## 9. Where to find canonical examples

- **Smallest** — `PORT_KOH_STATIC_HILL.md`. Single field, single wire byte, single engine hook. Good to read first.
- **Medium** — `PORT_NO_CULLING.md`. Two MP options + two cheats sharing a runtime flag.
- **Largest, gated cleanly** — `PORT_GOLDENEYE.md`. 12 behavioural rules, all routing through one helper. The pattern to copy when a feature has many small hooks.
- **Architectural / WIP** — `PORT_HOST_SPECTATOR.md`. Read for the "what NOT to do" notes on slot allocation.

The two `port-net-predict` reference docs (separate from per-feature docs):

- `PORT_NET_PREDICT_CHANGES.md` — per-file rationale for every change on this branch. Use to look up *why* a file was modified.
- `PORT_NET_KNOWN_ISSUES.md` — current limitations and partially-broken features. Read before promising any feature works on clients.

---

## 10. The handoff

When you're done porting a feature:

1. Update the target branch's equivalent of the README's "Claude-touched files" or porting changelog with the new file list.
2. Add the per-feature doc to the target branch's `docs/`.
3. If you modified `CLAUDE.md`'s docs index in this repo, add the equivalent entry in the target branch's `CLAUDE.md`.

Test plan (regardless of whether you can compile here): run a local match alone, then a local match with sims, then a LAN match with one remote client. The three buckets cover roughly the failure modes — single-player gate logic, sim-AI interaction, and wire / netcode integration.
