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

Lives in `port/src/net/netmenu.c` alongside the existing `Host Network Game` /
`Join Game` / `Server Browser` menus, using the same `menuitem` / `menudialogdef`
framework. A new **"Cooperative"** entry on the top network menu
(`g_NetMenuItems`) opens `g_NetCoopMenuDialog`.

**Flow (host):** Network Game → Cooperative → pick Mission + Difficulty +
mutators → **Start Hosting** (`netStartServer`, enters lobby; clients can now
join) → wait for partners → **Launch Mission** (`netCoopEnterStage(stage, diff,
g_NetNumClients)`, the same entry point `/coop` used). **Clients** join via the
existing Join Game / Server Browser and are pulled into the stage by the host's
`SVC_STAGE_START` (`NETSTAGEMODE_COOP`) — no co-op menu needed client-side.

### Menu layout (increment 1)

```
Cooperative
  Mission            (dropdown — g_SoloStages[0..NUM_SOLOSTAGES-1], name3)
  Difficulty         (dropdown — Agent / Special Agent / Perfect Agent / Perfect Dark)
  ---
  Lives              (dropdown — Off (Steal Health) / Per Player / Shared Pool)   [F3 stores; gameplay wiring later]
  Body Type          (dropdown — Feminine / Masculine / Random)                   [F2 stores; render wiring later]
  Import Combat Sim Profile                                                       [F1 stub]
  ---
  Start Hosting      (netStartServer if not already hosting)
  Launch Mission     (netCoopEnterStage — host only)
  ---
  Back
```

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

## F2 — body type (later, art-blocked)

Per-player body-type selection (Feminine / Masculine / Random-per-level), chosen
in F0 and synced. Render side picks the body model accordingly; **Masculine falls
back to the feminine model** until a per-mission masculine body is authored.
"Random" rolls per stage start (must use a *synced* roll so all machines agree —
the gameplay RNG or an explicit host broadcast, **not** the cosmetic stream).

## F3 — lives mutator (later)

`Lives = Off` keeps the stock **steal-half-a-buddy's-health** revive
(`player.c` co-op revive, widened for N in Bucket B). `Per Player` / `Shared Pool`
**replace** it: each death decrements a life (own counter, or a shared pool), and
at zero the player stays down (spectate) instead of being revivable. Host-
authoritative: the host owns the counters and broadcasts them; the revive path is
gated `if (livesmode == OFF)`. Needs a small co-op options block on the wire +
a HUD readout of remaining lives.

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
