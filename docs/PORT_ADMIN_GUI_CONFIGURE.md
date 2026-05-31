# Session-Safe Admin GUI Configure — Design / Scope

Status: **Resolved via a different approach — see "Resolution" below.** The
configure-session redesign in this doc (Option 2) was *not* built; instead a
lightweight, purpose-built **Admin: Match Setup** menu replaced the Phase-0
stopgap and sidesteps the crash class entirely. This doc is retained for the
root-cause analysis and the options that were considered.

Companion to [`PORT_ADMIN_CONTROL.md`](PORT_ADMIN_CONTROL.md) (the admin remote-control
protocol) and [`PORT_DEDICATED_SERVER_TRIAGE.md`](PORT_DEDICATED_SERVER_TRIAGE.md).

The goal: let a connected admin configure a match through a menu ("as if hosting
locally") and push it to a dedicated server, without crashing or dropping the
connection.

---

## Resolution (implemented)

Rather than reuse the **actual** Combat Simulator menu — whose entry runs the
destructive title-screen setup-load (`mpsetupCopyAllFromPak` → `mpInit`) that is
the entire source of the crash chain below — `/admin configure` now opens a small
**Admin: Match Setup** dialog (`g_NetAdminSetupMenuDialog`, `port/src/net/netmenu.c`).
Its handlers edit the already-synced `g_MpSetup` / `g_BotConfigsArray` **in
place** (no `mpInit`, no pak reload, no world teardown), then the in-menu
**"Push & Start Match"** button (or `/admin pushstart`) ships the result with the
existing, unchanged `CLC_ADMIN_SETUP`. Because nothing resets the live world,
there is no dangling-state class to chase — the menu is safe to open while the
client's lobby world is ticking.

It's a compact list of openers (modelled on the real "Game Setup" menu so it
doesn't overflow): **Arena**, **Weapons** and **Limits** open the *actual*
Combat Sim sub-dialogs (`g_MpArenaMenuDialog`, `g_MpWeaponsMenuDialog`,
`g_MpLimitsMenuDialog`), so Weapons is the full per-slot picker; **Scenario** /
**Simulants** / **Sim Difficulty** are inline dropdowns (`mpCreateBotFromProfile`);
**Options** is a sub-dialog of curated toggles (`menuhandlerMpCheckboxOption`).
Only the option list is curated, not every toggle — use the text `set` commands
for the rest. No `NET_PROTOCOL_VER` bump (reuses `CLC_ADMIN_SETUP`).

Known limitation: `/admin configure` pushes the dialog onto the active (lobby)
menu root; in the normal connected-lobby state that works, but if invoked from an
in-world state with no menu open the dialog may not appear — close/return to the
lobby first. A gated lobby-menu entry + admin-status sync is a possible follow-up.

The original analysis and the three design options follow, for the record.

---

The goal was: let a connected admin configure a match through the **built-in Combat
Simulator menu** ("as if hosting locally") and push it to a dedicated server,
without crashing or dropping the connection.

---

## Problem

`/admin configure` (`netAdminConfigure`, `port/src/net/netmenu.c`) opens the
title-screen Combat Sim menu and runs its setup-load:

```c
mpsetupCopyAllFromPak();   // -> mpInit(false): resets g_MpSetup scenario/stage/options (mplayer.c:595)
mpsetupLoadCurrentFile();
menuhandlerMainMenuCombatSimulator(MENUOP_SET, ...);  // flips bondplayernum/coopplayernum/antiplayernum
menuhandlerMpAdvancedSetup(MENUOP_SET, ...);
```

This is **title-screen setup logic** — it assumes a clean, world-less context.
But an admin client connected to a dedicated server has a **live, ticking world**
loaded locally (the server's CITRAINING lobby). The setup-load mutates global
MP/game state out from under that world, and the next frame's
`propsTick -> shieldhitsTick` (chr.c:6683) dereferences now-dangling state.

Confirmed crash chain (client, `0xc0000005`):
```
mainTick (pdmain.c:693) -> lvTick (lv.c:2559) -> propsTick (proptick.c:63)
  -> shieldhitsTick (chr.c:6683)   // derefs g_ShieldHits[i].prop
```

This is the third failure from the same source (em-dash font crash -> server
spawn crash -> shieldhits). They are all one fact: **the menu-configure flow runs
title-screen code while the client is mid-session in a live world.** Patching
individual danglers (`g_ShieldHits`, then `g_Wallhits`, then the prop list, ...)
is whack-a-mole — the fix must *isolate configure from live game state*.

## Constraints

- **Decompilation contract:** the Combat Sim menu and `mpsetup*` code under
  `src/game/` map to the N64 binary — minimize edits; prefer `#ifndef PLATFORM_N64`
  and port-side wrappers.
- **ENet must keep pumping** (`netStartFrame`/`netEndFrame` in `pdsched`) for the
  whole configure session, or the client times out (the observed "timeout").
- **A proven-safe model already exists:** the text `/admin set` path edits a
  scratch `g_NetAdminSetup` and `/admin apply` serializes + sends it — no menu, no
  world reset. The GUI flow must preserve that isolation.

## Design options

**Option 1 — Edit in place, skip the reset.** Open the menu bound to the existing
`g_MpSetup` without `mpsetupCopyAllFromPak`/`mpInit`. *Risk:* per-item menu
handlers (add sim, change scenario) may still mutate live state during use —
partial isolation, likely still crashes.

**Option 2 — "Configure session": park the client world-less, then re-sync. ✅ RECOMMENDED.**
On `/admin configure`, tear the client's local world down to a menu-only state
(the context the menu was designed for) while keeping `g_NetMode` + ENet alive and
pumping. Now the setup-load is safe — no live world to dangle. On pushstart the
existing `SVC_STAGE_START` path re-syncs the client into the match. Eliminates the
entire dangling-state class instead of chasing pointers.

**Option 3 — Freeze the world tick.** Keep the world loaded but suppress
`lvTick`/`propsTick` while the menu is open. *Risk:* the reset still corrupts
state; freezing only delays the crash to un-freeze.

## Recommended approach — Option 2, phased

- **Phase 0 — stopgap (DONE):** guard `/admin configure` to refuse with a message
  instead of crashing. Points users at the session-safe text commands.
- **Phase 1 — enter "configure session":** port-side state where the client unloads
  its local stage world to a menu-only state but stays `NETMODE_CLIENT` with the net
  loop still pumping. **Make-or-break piece** — verify `pdsched` keeps calling
  `netStartFrame`/`netEndFrame` with no stage loaded (keep it alive explicitly if not).
- **Phase 2 — open the menu in the clean state:** `netAdminConfigure`'s setup-load +
  menu-open now run exactly as at the title screen — no live props, no danglers.
- **Phase 3 — pushstart re-entry:** serialize `g_MpSetup` + bot configs (existing
  `CLC_ADMIN_SETUP`), server runs `mpStartMatch`, client re-syncs via `SVC_STAGE_START`
  (this path already works for normal match joins).
- **Phase 4 — cancel/back re-entry:** on leaving the menu without starting, re-sync
  the client back to the lobby (`SVC_LOBBY_STATE`, or reload the lobby stage).

## Files

- `port/src/net/netmenu.c` — `netAdminConfigure` (enter session + the Phase 0 guard),
  `netAdminPushStart` (exit -> match).
- `port/src/net/net.c` — configure-session state flag; keep the net pump running
  during it; cancel -> lobby re-sync.
- `port/src/pdmain.c` — gate the client world tick/render while in a configure session
  (similar shape to the existing `g_NetDedicatedMode` headless guards).
- Re-entry leans on existing `SVC_STAGE_START` / `SVC_LOBBY_STATE` — little/no new wire
  format, so likely **no `NET_PROTOCOL_VER` bump**.

## Risks / open questions (resolve in Phase 1)

1. **Can the client unload its stage world without dropping ENet?** Need a "stage
   teardown, keep connection" primitive — the crux. If `pdsched` stops pumping
   `netStartFrame` with no stage loaded, Phase 1 must keep it alive explicitly.
2. **Does the menu touch live state even world-less?** Verify in Phase 2 (should be
   safe — it's the title-screen context — but confirm).
3. **Cancel re-sync fidelity** — does `SVC_LOBBY_STATE` fully restore the client, or
   must it reload CITRAINING?

## Test plan

Fresh connect -> `/admin take` -> `/admin configure` (no crash, camera locked,
connection survives — watch the `tick` heartbeat in `pdhost.log`) -> set up in the
menu -> `/admin pushstart` (match starts, client transitions). Repeat with **cancel**
instead of pushstart (returns cleanly to the lobby). Then multi-client and
mid-lobby-join cases.

## Until then — the working path

The text commands are session-safe and fully cover "admin configures and starts a
match":
- `/admin start <index>` — launch a playlist entry (same code path as the working
  dedicated auto-start; safest).
- `/admin set stage <X>` / `set scenario <Y>` / `set bots <n> <diff>` -> `/admin apply`
  — custom match, entirely from the console.
