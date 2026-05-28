# Port-only Feature: Host Spectator Mode

> **Status: WIP / paused.** Works for "host alone with sims" but is broken with remote combatants present (sim AI freezes on host, client positions don't apply). Root cause: decompiled code's hard dependency on `g_Vars.currentplayerindex == 0` being a real-player viewport, which fights the panel-slot allocation either way it's arranged. Two refactor paths to try are in the `project_host_spectator` memory entry. Don't port this verbatim into a stable branch — port the architecture if you want it, but expect to revisit the slot-allocation choice for your scenario.

Lets the host opt out of being a combatant in the lobby and instead run 1–4 panel observer viewports. Per-panel modes: first/third-person on a remote player, first/third-person on a sim, free flying camera, or top-down. All 8 wire player slots stay available to combatants and bots.

Everything is gated under `#ifndef PLATFORM_N64`. N64 build is byte-identical.

---

## Architecture (the load-bearing decisions)

1. **The host's `netclient` carries a sentinel `playernum = NET_PLAYERNUM_SPECTATOR (0xFE)`.** Its `config` and `player` stay `NULL`. `netPlayersAllocate` skips it when assigning sequential combatant playernums, freeing all 8 slots for remote clients + bots.
2. **Local panels reuse `g_Vars.players[0..N-1]`.** The same split-screen quadrant math that drives 2/3/4-player split is reused for the panels by overriding `LOCALPLAYERCOUNT()` to return the panel count.
3. **`is_spectator` on the player struct** tells `lvRender` to dispatch to `spectatorRenderPanel` instead of the chr/HUD body. Panel slots have no chr, no prop, no inventory — touching any of those crashes, so the early-continue is mandatory.
4. **Wire field: one extra byte per client in `SVC_STAGE_START` and `SVC_LOBBY_STATE`** carries `is_spectator`. That's the entire wire surface.
5. **`MPOPTION_HOSTSPECTATOR (0x40000000)`** rides in `g_MpSetup.options` so existing wire/save paths cover it.

The hard part isn't any single hook — it's keeping the engine's `currentplayer`/`currentplayernum` invariants intact when slots `[0..N-1]` are panels rather than combatants. See "Known breakage" below.

---

## Surface

### New files

| File | What |
|---|---|
| `port/include/spectator.h` | Per-panel struct, mode constants, externs, public API |
| `port/src/spectator.c` | Panel state, freecam input, per-mode pose updates, minimal render path |

### New constants

| Symbol | Where | Value |
|---|---|---|
| `MPOPTION_HOSTSPECTATOR` | `src/include/constants.h` | `0x40000000` |
| `NET_PLAYERNUM_SPECTATOR` | `port/include/net/net.h` | `0xFE` |
| `SPEC_MAX_PANELS` | `port/include/spectator.h` | `4` |
| `SPEC_MODE_PLAYER_FP` .. `SPEC_MODE_TOPDOWN` | `port/include/spectator.h` | `0..5` |
| `SPEC_TARGET_NONE` | `port/include/spectator.h` | `0xFF` |

### Modified files

| File | Site | What |
|---|---|---|
| `src/include/types.h` | `struct player` | `u8 is_spectator;` + spectator panel-index byte |
| `port/include/net/net.h` | `struct netclient`, `struct netlobbyclient` | `u8 is_spectator;` each; `NET_PLAYERNUM_SPECTATOR` define |
| `port/src/net/net.c` | `netPlayersAllocate` | Skip clients with `is_spectator` when assigning combatant `playernum`s; set `cl->player = NULL` |
| `port/src/net/netmsg.c` | `SVC_STAGE_START` write/read | Append `is_spectator` byte to per-client manifest |
| `port/src/net/netmsg.c` | `SVC_LOBBY_STATE` write/read | Same byte in the lobby per-client block |
| `port/src/net/netmsg.c` | `SVC_PROP_PICKUP`/`USE`/`DOOR` | Refuse `setCurrentPlayerNum(actcl->playernum)` when `actcl->is_spectator` (would crash on NULL `config`) |
| `port/src/net/netmenu.c` | host dialog | "Spectator Mode" checkbox + "Spectator Panels" 1–4 slider |
| `port/src/net/netmenu.c` | `menuhandlerHostStart` | Set `MPOPTION_HOSTSPECTATOR`, `g_NetLocalClient->is_spectator = 1`, `g_SpectatorPanelCount = slider_value` |
| `port/src/net/netmenu.c` | lobby player line | Tag spectator clients with `(spec)` |
| `src/game/player.c` | `playerGetLocalCount` | Return `g_SpectatorPanelCount` when host is spectator |
| `src/game/playermgr.c` | after `playermgrAllocatePlayer` loop | Call `spectatorAllocatePanels()` before `netPlayersAllocate` — `is_spectator` must be set on panel slots before remote-client slot binding runs |
| `src/lib/main.c` | viewport setup | Inflate `numplayers` to `g_SpectatorPanelCount` when host is spectator |
| `src/lib/main.c` | per-frame viewport loop | Call `spectatorReadInput()` once per frame, `spectatorTickPanel(N)` per panel (replaces `lvTickPlayer` for panels — it would deref `prop->pos` on a slot with no mpchr) |
| `src/game/lv.c` | `lvRender` per-player loop | `if (g_Vars.currentplayer->is_spectator) gdl = spectatorRenderPanel(gdl); continue;` — skips the chr / HUD body |
| `src/game/lv.c` | other per-player loops | Defensive `is_spectator || !prop` continues anywhere that touches `g_Vars.players[N]->prop` (SLOWMOTION_SMART, etc.) |
| `src/game/chr.c` | `chrRender` (or wherever chr-prop iteration runs per-camera) | Honour `g_SpectatorHideChrProp` so first-person spectate hides the target's own body |
| `port/src/pdmain.c` | scheduler hook | `spectatorReadInput()` wired into the per-frame call sequence |

---

## Per-panel state (`struct spectatorpanel`)

Lives in `g_SpectatorPanels[SPEC_MAX_PANELS]`, indexed by `g_Vars.currentplayernum` during a panel's tick/render.

```c
struct spectatorpanel {
    u8 mode;          // SPEC_MODE_*
    u8 target;        // netclient id for PLAYER, sim idx for SIM, unused otherwise
    struct coord pos, look, up;   // cached pose; PLAYER/SIM refresh from target each tick,
                                  // FREECAM/TOPDOWN integrate input and persist
    f32 yaw, pitch;               // freecam accumulators (avoid normalising look/up every frame)
    s32 room;                     // last room rendered from — BSP culls from cam_room, so
                                  // freecam straying off-portal-cluster needs this updated
    u8 bgun_inited;               // Phase B (bgun emulation for FP weapon overlay)
    s8 bgun_weaponnum;            // -1 = none currently equipped
};
```

`bgun_inited` / `bgun_weaponnum` are Phase B scaffolding — full bgun emulation for the FP overlay is partially wired but not validated. Leave them in if you port; don't rely on them.

---

## Public API (`spectator.h`)

```c
s32  spectatorIsActive(void);                  // true when host is local + MPOPTION_HOSTSPECTATOR
void spectatorAllocatePanels(void);            // call at lobby→game transition
void spectatorFreePanels(void);                // call at stage end
void spectatorTickPanel(s32 panelnum);         // replaces lvTickPlayer for panels
Gfx *spectatorRenderPanel(Gfx *gdl);           // replaces lvRender's per-player body
void spectatorReadInput(void);                 // once per frame, before viewport loop
void spectatorCycleTarget(s32 panelnum, s32 dir);
void spectatorCycleMode(s32 panelnum, s32 dir);
```

`g_SpectatorHideChrProp` is a single `struct prop *` global: `spectatorRenderPanel` sets it to the target chr on FP entry and clears it on exit, so the chr-render path can skip that one chr for one panel without changing render-loop signatures.

---

## Wire surface

```c
// SVC_STAGE_START — per-client block, after existing fields
netbufWriteU8(dst, ncl->is_spectator);    // <-- new

// SVC_LOBBY_STATE — per-client block, same idea
netbufWriteU8(dst, lobby->clients[i].is_spectator);
```

Bump `NET_PROTOCOL_VER`. The byte is appended at the **tail** of each per-client block so older versions of the per-client decoder skip it cleanly if they're protocol-flexible — but in practice this branch enforces an exact match on `NET_PROTOCOL_VER`, so the version bump is the actual compat gate.

`MPOPTION_HOSTSPECTATOR` rides in the existing `g_MpSetup.options` u32 — no separate field needed.

---

## Allocation order (don't reorder)

1. `playermgrAllocatePlayer` loop runs for `[0..numplayers-1]`. Panel slots are allocated as ordinary players first.
2. `spectatorAllocatePanels()` runs and **sets `is_spectator = 1` on `g_Vars.players[0..panelcount-1]`** (and parks the cam pose so subsequent ticks have valid coords).
3. `netPlayersAllocate()` runs. It now sees `is_spectator` flagged and refuses to bind any remote client to those slots.

If you swap 2 and 3, a remote combatant can land on slot 0 first and the panel allocator clobbers them. If you skip 2 entirely, every panel deref crashes once `lvRender` tries to read the panel slot's chr.

---

## Controls (gamepad, host only)

| Input | Effect |
|---|---|
| Left stick | Pan active panel's freecam in local horizontal plane |
| Right stick | Yaw / pitch active panel's freecam |
| R-trigger | Freecam ×4 boost |
| D-pad ↑/↓ | Freecam altitude |
| C-Left / C-Right | Cycle target (PLAYER or SIM mode only) |
| C-Up / C-Down | Cycle active panel's mode (PLAYER_FP → PLAYER_TP → SIM_FP → SIM_TP → FREECAM → TOPDOWN) |
| Z-trigger | Cycle which panel is active |

No menu UI for per-panel selection currently — gamepad-only. See "Known limitations" in `PORT_NET_KNOWN_ISSUES.md`.

---

## Known breakage (read before porting)

From `PORT_NET_KNOWN_ISSUES.md` and the project memory:

- **Broken with remote combatants present.** Host alone + sims works. As soon as a remote client joins: sim AI freezes on the host, and client positions don't apply on the host's view. Root cause is the engine's hard dependency on `g_Vars.currentplayerindex == 0` being a real-player viewport for code paths the panel allocator doesn't touch.
- **No audio listener routing.** 3D pan/volume tracks the active panel only via `setCurrentPlayerNum` side effects; cycling panels rapidly feels inconsistent.
- **Target-disappears-mid-frame** renders last-known pose for one frame before failing over to freecam. Cosmetic.
- **No pause-menu UI** for per-panel mode/target — gamepad-only.
- **Not validated end-to-end** in the original commit.

Two paths to fix the combatant-with-remote breakage, both architectural:

- **Path A**: panels live at `g_Vars.players[combatant_count..combatant_count+panel_count-1]`, combatants always start at 0. Pro: keeps `currentplayerindex == 0` an always-valid combatant; con: requires reworking the `MAX_PLAYERS` viewport-layout math which assumes slot 0 is the top-left quadrant.
- **Path B**: keep panels at 0..N-1 but stub the chr/prop/inventory accessors with sentinel objects that return safe defaults rather than NULL, so all the implicit-slot-0 code paths just no-op rather than crash. Pro: minimal layout disruption; con: large surface area of "safe defaults" to chase.

Pick before porting; don't try to merge both.

---

## Porting checklist

1. Add `MPOPTION_HOSTSPECTATOR 0x40000000` to `constants.h`. Verify your upper-byte MPOPTION space isn't already at this bit.
2. Add `NET_PLAYERNUM_SPECTATOR 0xFE` and the `is_spectator` field to `netclient` + `netlobbyclient` in `net.h`. Bump `NET_PROTOCOL_VER`.
3. Add `u8 is_spectator` to `struct player` in `types.h`.
4. Drop in `port/include/spectator.h` + `port/src/spectator.c` verbatim from `port-net-predict`.
5. Wire `SVC_STAGE_START` + `SVC_LOBBY_STATE` to carry the byte, and add the defensive guards in `SVC_PROP_*` handlers.
6. Add the host-dialog UI (`netmenu.c`) and the `menuhandlerHostStart` writes.
7. Patch `playerGetLocalCount` to return `g_SpectatorPanelCount` when active, so `LOCALPLAYERCOUNT()` drives the panel layout.
8. Call `spectatorAllocatePanels()` between the `playermgrAllocatePlayer` loop and `netPlayersAllocate`. Call `spectatorFreePanels()` at stage end.
9. Add the per-frame `spectatorReadInput()` + `spectatorTickPanel()` calls in the viewport loop. Replace `lvTickPlayer` for panel slots.
10. Add the `is_spectator` early-continue in `lvRender` (dispatches to `spectatorRenderPanel`). Audit other per-player loops for `prop`/`chr`/`hud` derefs and gate with `is_spectator` or `!prop` checks.
11. Honour `g_SpectatorHideChrProp` in `chrRender`.

If you intend to ship this, validate path A or B first and rework slot allocation accordingly. Don't ship as-is.
