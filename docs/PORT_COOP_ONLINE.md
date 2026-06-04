# Port Co-op — "Cooperative > Online" Front-End & Mutators (epic)

> A front-end + feature epic layered on top of the N-player net co-op
> ([`PORT_COOP_8P.md`](PORT_COOP_8P.md)). Turns campaign co-op from a debug
> console command (`/coop`) into a real lobby with host mutators, plus two
> standalone in-game systems (name tags, world pings). Status: **F0 in progress.**

## Why this exists

Net campaign co-op currently launches **only** via the `/coop` console command
(`netConsoleCommand` → `netCoopEnterStage`). There is no menu. This epic adds the
front-end the user wants under **Network Game → Cooperative**, and hangs a set of
co-op features off it.

## Feature breakdown

| # | Feature | Nature | Depends on |
|---|---|---|---|
| **F0** | **Co-op lobby menu** (Network → Cooperative) | New front-end UI; container | — |
| **F1** | Import Combat Sim profile → character settings | Reads existing profile; pairs with the deferred per-client-settings manifest | F0 |
| **F2** | Body type: Feminine / Masculine / Random-per-level | Code toggle + wire sync; **art-blocked** (masculine model is per-mission art, doesn't exist) | F0 + art |
| **F3** | Host mutators: **Lives** (per-player or shared); enabling lives **disables take-half-health** | Gameplay + host-authoritative sync; touches the Bucket-B revive code | F0 |
| **F4** | Name tags over the player under your crosshair | Self-contained: aim/LOS test + billboard text | — |
| **F5** | **Ping/marker** — point at wall/prop, marker visible through walls to all; **universal (co-op + Combat Sim)** | Self-contained: networked marker + depthless billboard | — |

### Decisions locked (this session)
- **Start with F0** (the menu is the foundation for F1/F2/F3).
- **F2 body type: code the toggle now, feminine fallback.** Build the full
  Feminine/Masculine/Random toggle + wire sync; "Masculine" renders the existing
  feminine model until a per-mission masculine model is hand-crafted, so the
  plumbing is ready and art drops in later.

## F0 — the Cooperative lobby menu

**Placement: its own top-level path, separate from Combat Simulator.** The port
main menu (`g_MainMenuMenuItems`, mainmenu.c) already has four top-level entries —
Solo Missions, Combat Simulator, **Co-Operative**, Counter-Operative — and each
mode entry opens a small **Local / Online** submenu. Combat Simulator → Online
goes to the Combat Sim network menu (`g_NetMenuDialog`); the **Co-Operative →
Online** entry was a disabled placeholder. F0 wires that entry to the new co-op
lobby `g_NetCoopMenuDialog`, so co-op online is fully separate from the Combat Sim
network menu:

```
Main Menu
├─ Combat Simulator → Local / Online → g_NetMenuDialog      (Combat Sim network)
└─ Co-Operative     → Local / Online → g_NetCoopMenuDialog  (co-op lobby — this)
```

The dialog itself lives in `port/src/net/netmenu.c` (non-static so mainmenu.c can
reference it) using the same `menuitem` / `menudialogdef` framework as the other
net menus. The wiring edit is in `src/game/mainmenu.c` `g_CoopModeMenuItems`
(remove `MENUITEMFLAG_ALWAYSDISABLED`, add `OPENSDIALOG` → `&g_NetCoopMenuDialog`).

**Flow (host):** Co-Operative → Online → pick Mission + Difficulty + mutators →
**Start Hosting** (`netStartServer`, enters lobby; clients can now join) → wait
for partners → **Launch Mission** (`netCoopEnterStage(stage, diff,
g_NetNumClients)`, the same entry point `/coop` used). **Clients** join via the
existing Combat Simulator → Online → Join Game / Server Browser (the join path is
mode-agnostic) and are pulled into the stage by the host's `SVC_STAGE_START`
(`NETSTAGEMODE_COOP`) — no co-op menu needed client-side.

### Menu layout (increment 1)

The Online entry opens a **hub** (Host / Join / Browser) mirroring the Combat Sim
network menu but separate from it; Join / Browser reuse the mode-agnostic
handlers (the host's `SVC_STAGE_START NETSTAGEMODE_COOP` is what makes it co-op).

```
Co-Operative > Online        (g_NetCoopMenuDialog — hub)
  My Body Type               (dropdown — Feminine / Masculine / Random)  [F2 per-player; synced via CLC_SETTINGS]
  ---
  Host Co-op Game            → g_NetCoopHostMenuDialog (below)
  Join Game                  → menuhandlerJoinGame      (reused)
  Server Browser             → menuhandlerServerBrowser (reused)
  Back

  Host Co-op Game            (g_NetCoopHostMenuDialog — host-only setup)
    Mission                  (dropdown — g_SoloStages[0..NUM_SOLOSTAGES-1], name3)
    Difficulty               (dropdown — Agent / Special Agent / Perfect Agent / Perfect Dark)
    ---
    Lives                    (dropdown — Off (Steal Health) / Per Player / Shared Pool)   [F3a]
    Lives Count              (dropdown — 1..9)                                            [F3a]
    Import Combat Sim Profile                                                             [F1 stub]
    ---
    Start Hosting            (netStartServer if not already hosting)
    Launch Mission           (netCoopEnterStage — host only)
    ---
    Back
```

"My Body Type" is in the hub (not the host-only setup) because it's a **per-player**
customisation each client sets for themselves.

**Increment 1 (this pass)** wires Mission / Difficulty / Start Hosting / Launch
fully (a menu replacement for `/coop`, which is kept). The Lives and Body Type
dropdowns and the Import action are present and **store their selection** in
menu-local state, but their gameplay/render effects land in F1/F2/F3. Menu state
globals (`g_NetCoopMenu*`) are file-static for now; they get promoted to synced
net globals when F2/F3 wire them over the wire (`SVC_STAGE_START` / a co-op
options block) and bump `NET_PROTOCOL_VER`.

### Handler pattern

Each dropdown is a `menuhandlerNetCoop*` mirroring `menuhandlerNetAdminScenario`
(`MENUOP_GETOPTIONCOUNT` / `GETOPTIONTEXT` / `SET` / `GETSELECTEDINDEX`,
`data->dropdown.value` to read, `data->checkbox.value` on SET). Start Hosting /
Launch / Import are `MENUITEMTYPE_SELECTABLE` items with an action handler firing
on `MENUOP_SET`. The "Cooperative" top entry uses a `menuhandlerCooperative` that
`menuPushDialog(&g_NetCoopMenuDialog)` (mirrors `menuhandlerHostGame`).

## F1 — import Combat Sim profile (later)

The active Combat Sim player config (`g_PlayerConfigsArray` / saved player pak)
carries head/body/name. F1 copies that into the local player's co-op identity and
into the **co-op manifest** (the `SVC_STAGE_START` co-op branch currently sends
only `{id, playernum}` — see the `PORT_COOP_8P.md` "remote partner appearance"
follow-up). This is where the deferred manifest-settings work lands.

## F2 — body type — **plumbing done (feminine fallback)**

Session-wide body-type selection (Feminine / Masculine / Random), chosen in the
F0 host lobby, with the **masculine model falling back to the feminine model
until per-mission art exists** — so this is currently a no-op visually, by design.

**Key architecture fact (from the user):** Jo's body *and* head are **outfit-driven
per level** — `playerChooseBodyAndHead` resolves an `outfit` (combat suit, leather,
wetsuit, lab coat, …) and the switch on it sets `*bodynum`/`*headnum`. So the
masculine model is a **per-outfit counterpart**, not a single global body.

**Body type is a PER-PLAYER choice** (each player's own customisation), and **the
head is ALWAYS the player's Combat Sim profile head** — the fixed campaign
Joanna/Velvet heads are never used in co-op (feminine *and* masculine bodies take
the CS head).

Implemented:
- **Per-player choice → CLC_SETTINGS.** The "My Body Type" dropdown (in the
  co-op Online *hub*, reachable by host and clients alike) writes
  `g_NetCoopBodyMode` (`COOPBODY_FEMININE`/`MASCULINE`/`RANDOM`) and calls
  `netClientSettingsChanged()`. Each player's choice rides `CLC_SETTINGS`
  (`settings.coopbodytype`) to the host.
- **Host assembles + syncs.** In the `SVC_STAGE_START` co-op write the host stamps
  its own choice into `g_NetLocalClient->settings.coopbodytype`, then walks the
  client manifest and resolves every player's `coopbodytype` into the bitmask
  `g_NetCoopBodyBits` (bit *i* = player *i* masculine). `RANDOM` is rolled here
  from the **cosmetic** RNG (`rngCosmeticRandom`) — the *result* ships, so all
  machines agree and the network-synced gameplay seed stays clean. The client
  applies the wire bitmask in `netmsgSvcStageStartRead` before its
  `netCoopEnterStage`. (proto 49 covers both the CLC field and the SVC bitmask.)
- **Render hook.** `playerChooseBodyAndHead`, after the outfit switch, for co-op:
  - **HEAD** is always set from `g_PlayerConfigsArray[mpindex].base.mpheadnum`
    (same source as the Combat Sim path) — each player looks like their CS
    character, never the campaign default.
  - **BODY**: if this player's `g_NetCoopBodyBits` bit is set,
    `coopGetMasculineBody(outfit, stagenum)` supplies the masculine body —
    currently a `switch (outfit) { default: return -1; }` stub, so every outfit
    falls back to feminine. **Per-outfit masculine bodies drop in as `case`s
    here.** `#ifndef PLATFORM_N64`; N64 byte-identical.

  *Known caveat (invisible until art):* the host assembles the bitmask in the
  `SVC_STAGE_START` write, which fires during stage load; if the host's own
  `playerChooseBodyAndHead` runs before that write, the host renders *itself*
  feminine for that load. Harmless while masculine == feminine; when art lands,
  hoist the host resolve to `netPlayersAllocate` (pre-body-choice, playernums
  valid).

## F3 — lives mutator — **F3a done (gameplay); F3b = HUD + counter sync**

`Lives = Off` keeps the stock **steal-half-a-buddy's-health** revive (`player.c`
co-op revive, widened for N in Bucket B). `Per Player` / `Shared Pool` **replace**
it: each death spends a life (own counter, or a shared pool of `count * N`), and at
zero the player stays down. Host-authoritative.

**F3a (implemented):**
- **Settings** (`net.h`): `g_NetCoopLivesMode` (`COOP_LIVES_OFF/PERPLAYER/SHARED`)
  + `g_NetCoopLivesCount`, host settings, synced in `SVC_STAGE_START` (proto 50).
  Lobby "Lives" + "Lives Count" dropdowns (host setup).
- **Counters** (`net.c`): `g_NetCoopLives[MAX_PLAYERS]` + `g_NetCoopSharedLives`,
  host-authoritative, seeded in `netCoopEnterStage` (`count` each / `count*N`
  pool).
- **Revive gate** (`player.c`): when a lives mode is active, the steal-health
  revive `if` is gated off (`&& COOP_LIVES_OFF`) and a host-only lives branch runs
  instead — on respawn input, if a life remains, decrement and respawn at **full
  health** (no steal); else stay down. The client takes no action (host drives
  respawn via force-position).
- **All-out mission end** (`player.c`): a player with a life left isn't "out", so
  the stage ends only when every co-op player is fully dead *and* out of lives;
  host-gated (client follows `SVC_STAGE_END`).

**F3b (implemented) — respawn notification (no persistent HUD counter):** on each
respawn the player is told their **remaining** lives as a bottom-left notification
("`N lives remaining`" / "`1 life remaining`"), mirroring Combat Sim's "Killed by
X" (`hudmsgCreate(text, HUDMSGTYPE_DEFAULT)`). Players track it mentally — there's
no on-screen counter. Targeting matches the pool semantics:
- **Shared Pool** → notify **all** players (everyone shares the pool, so everyone
  sees it tick down): host shows locally + `SVC_COOP_LIVES` (0x52) broadcast.
- **Per Player** → notify **only that player**: host shows locally if it's the
  host, else unicasts `SVC_COOP_LIVES` to that one client.

Flow: `player.c` (host) decrements, then `netServerNotifyLives(victimplayernum,
remaining, shared)` (`net.c`) does the targeting; the recipient's
`netCoopShowLivesMsg` → `netCoopShowLivesText` renders it for its local player
(slot `g_NetLocalClient->playernum`). Non-net splitscreen co-op shows directly to
the victim's viewport. `SVC_COOP_LIVES` (id `0x52`) carries just the `u8` count
(proto 51).

### Reusing the lives system in Combat Simulator (note)

This lives system is intentionally mode-agnostic in its plumbing and is a good
base for a **Combat Sim lives/stock mode** ("last team standing" / limited
respawns). To port it:
- **Counters/mode/count** (`g_NetCoopLives*` in `net.c`): the same per-player /
  shared-pool model works; in Combat Sim, seed per-player (FFA) or per-team
  (pool-per-team) instead of per co-op player. Reset on round start
  (`mpStartMatch`) rather than `netCoopEnterStage`.
- **Respawn gate**: Combat Sim respawn is the `else` branch in the same
  `player.c` death handler (the anti/normal-MP path, ~the `UCMD_RESPAWN` /
  `deadtimer` block) — gate it on "has a life left", denying respawn at zero
  (the player becomes a spectator via the existing client-spectator / dead-cam).
- **Round/match end**: end the round when a side is fully eliminated (all out of
  lives), analogous to the co-op all-out check, via the existing
  `g_NumReasonsToEndMpMatch` / round-end path.
- **Notification**: `netServerNotifyLives` + `SVC_COOP_LIVES` are already generic
  — reuse verbatim (the message has no co-op-specific fields). Optionally widen
  "shared" to "per-team" targeting (notify the victim's team).
- **Menu**: the "Lives"/"Lives Count" dropdowns mirror cleanly into the Combat Sim
  setup (`g_NetAdminSetupMenuItems` / the Combat Sim options), syncing through the
  existing `SVC_STAGE_START` / `CLC_ADMIN_SETUP` option plumbing.

## F4 — name tags (later, standalone)

When the local crosshair is over another player's model (an aim/LOS test like the
auto-aim target search), draw that player's name as a small billboarded label
near their model. Self-contained; no menu dependency. Combat-Sim-safe (gate on
"is a player prop", not co-op).

## F5 — ping / world marker (later, standalone, **universal**)

Point at a wall or prop and drop a marker at that world spot / on that prop,
**visible through walls** to **all** players. Networked (new `SVC`/`CLC` ping
message carrying the world point + optional prop syncid + owner). Rendered as a
depthless billboard (the light-glare / artifact path already draws depth-less 2D
texrects — reuse that approach so it shows through geometry). **Built mode-
agnostic from day one** (not gated on `coopplayernum`) so Combat Sim can reuse the
same `netPing*` primitive — the user's explicit requirement.

## Risks / discipline

- **Untestable here** — all menu/feature code is verified by inspection + CI
  compile; correctness needs an in-game session.
- **Protocol version** — any new wire field (F1 manifest settings, F2 body type,
  F3 lives, F5 ping) bumps `NET_PROTOCOL_VER` (`port/include/net/net.h`).
- **Decompilation contract** — co-op/menu code is port-only; don't rename
  decompiled symbols. New port symbols (`g_NetCoopMenu*`, `netPing*`) are free.
- **F2 art** is the only hard external dependency; the code path is built now with
  a feminine fallback so it can't block.
