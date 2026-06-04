# Port Co-op — Scaling to 8 Players (Phase 5)

> Groundwork + plan for taking net campaign co-op from the engine's hardwired
> **2 players** up to **8** (`MAX_PLAYERS`). Read [`PORT_COOP_PLAN.md`](PORT_COOP_PLAN.md)
> first (this is its Phase 5). Status: **groundwork in progress** — see "Done so far".

## The one insight that makes this tractable

The engine's co-op is built around **splitscreen**: two (up to four) *local*
players sharing one screen, with `g_Vars.coop` a single second-local-player
pointer. There are **~185 `g_Vars.coop` references** — but the overwhelming
majority are **splitscreen-local**: a second viewport, its HUD, its camera, its
input, its screen-split quadrant math. **Net co-op has exactly ONE local player
per machine** (the partner is a remote `chr`, driven by `SVC_PLAYER_MOVE`), so
those sites never fire and we **do not touch them**. That collapses an apparent
185-site rewrite into a small set of *shared-logic* sites that genuinely mean
"any / all co-op players".

## "Is a co-op player" — the generalisation

In net co-op there is **no counter-op** (`antiplayernum < 0`), so **every player
is a co-op player**. The engine already has the exact predicate:

```c
PLAYER_IS_NOT_ANTI(plr)   // (antiplayernum < 0 || plr == bond)  -> true for all players in any co-op game
```

So the pervasive 2-player pattern

```c
for (i = 0; i < PLAYERCOUNT(); i++)
    if (g_Vars.players[i] == g_Vars.bond || g_Vars.players[i] == g_Vars.coop) { ... }
```

becomes a **one-token swap**, identical for SP / 2-player / anti, correct for N:

```c
for (i = 0; i < PLAYERCOUNT(); i++)
    if (PLAYER_IS_NOT_ANTI(g_Vars.players[i])) { ... }
```

For "all co-op dead" / "any co-op alive" checks, replace the explicit
`bond->isdead && coop->isdead` with an iteration:

```c
// any co-op player alive?
bool anyalive = false;
for (i = 0; i < PLAYERCOUNT(); i++)
    if (PLAYER_IS_NOT_ANTI(g_Vars.players[i]) && !g_Vars.players[i]->isdead) { anyalive = true; break; }
```

`g_Vars.coop` (the single pointer) stays valid (it points at slot 1, NULL
otherwise) so splitscreen and any unconverted reader keep working — we only
*add* N-player breadth to the shared-logic sites.

## Bucket B — shared-logic sites to widen (the actual work)

Each is small and **behaviour-identical for 2 players**. Convert incrementally;
nothing here flips the player count.

| Site | What it does | Widening |
|---|---|---|
| `objectives.c` COLLECTOBJ / THROWOBJ loops | "any co-op player holding the item" | **DONE** — `bond\|\|coop` → `PLAYER_IS_NOT_ANTI` |
| `objectives.c` `objectivesShowHudmsg` (~411) | show objective toast to each co-op player | **DONE** — `bond\|\|coop` → `PLAYER_IS_NOT_ANTI` |
| `player.c` co-op revive (~5128) | dying player steals half a buddy's health | **DONE** — pick the **first alive** co-op buddy among N (identical to "the other player" at 2P, since current is already dead). N>2 economics (first-alive vs most-health) can be refined when flipping C |
| `player.c` all-dead mission end (~4614) | `bond && coop both dead` → end | **DONE** — iterate: end only when **all** co-op players are fully dead |
| `chr.c` cloaked-NPC hide / no-autoaim (~5203) | skip autoaim if prop is bond/coop | **DONE** — co-op has no anti, so `prop->type == PROPTYPE_PLAYER` ≡ bond\|\|coop for 2P, correct for N |
| `bondgun.c` mission-critical-weapon guard (~6730) | attacker is bond/coop | **DONE** — `attackerprop->type == PROPTYPE_PLAYER` (same reasoning) |
| `endscreen.c` mission-fail (~1712) | both dead / either aborted | → all co-op dead / any aborted (not yet done) |
| `mplayer.c` respawn-restart (~1801) | can't restart if both dead | → can't restart if all co-op dead (not yet done) |
| `chraicommands.c` `chr_toggle_p1p2` (~9088) | swap NPC target bond↔coop | → cycle target across all alive co-op players (host-side idle `p1p2` cycling in `chraTick` already perceives all players; this is the scripted-toggle variant) (not yet done) |
| `chr.c` `chrGetTargetProp` / `p1p2` | NPC's single target slot | already handled host-side by the idle `p1p2` cycling; verify scripted set-pieces |

> `music.c` death cue: the only player-count music gate found is `player.c:5079`
> (`musicStartSoloDeath` vs `musicStartMpDeath`, gated on `mplayerisrunning`) — that
> is already correct for co-op (MP death music plays). No bond/coop death-cue site
> exists in `music.c`; the earlier estimate was wrong. Nothing to widen.

## Bucket C — allocation (the go-live flip)

The allocation chain **already scales** — Combat Sim seats up to 8 via it. Only
the co-op entry hardcodes 2:

- `port/src/net/net.c` `netCoopEnterStage` — `g_Vars.coopplayernum = 1; setNumPlayers(2);`
  → derive **N** (host: connected co-op clients + 1; client: from the
  `SVC_STAGE_START` co-op manifest, which already carries the player list) and
  `setNumPlayers(N)`. Keep `coopplayernum >= 0` as the co-op *gate*.
- `port/src/pdmain.c` (and `src/lib/main.c`) co-op path — `g_MpSetup.chrslots = 0x03`
  → `chrslots = (1 << N) - 1`.
- `setNumPlayers(N)` → `playermgrAllocatePlayers(N)` → `netPlayersAllocate()` sequential
  playernum assignment — **already N-ready, no change**.
- `playermgr.c` `g_Vars.coop = g_Vars.players[coopplayernum]` — keep (compat); the
  N players live in `g_Vars.players[0..N-1]`.

**Order matters:** do Bucket B *first* (safe, no-op for 2), then flip Bucket C.
Flipping C before B would route players 2..N-1 through the 2-player checks and
break objectives / revive / end-of-mission for the extra players.

## Bucket D — hard caps (mostly already port-widened)

- `MAX_COOPCHRS` = `MAX_PLAYERS` (8) on the port already. Don't change.
- `g_Vars.coop` stays a single pointer (splitscreen compat) — add breadth, don't remove.
- `wallhitreset.c` / `gfxmemory.c` / `vi.c` have `PLAYERCOUNT()/LOCALPLAYERCOUNT() == 2`
  checks — these are **splitscreen/perf** gates; in net co-op `LOCALPLAYERCOUNT()==1`,
  so they don't fire. Review only if local splitscreen + net co-op ever combine.

## Splitscreen feature-removals to RE-ENABLE for net co-op

The original game disables expensive viewmodel/world visuals when drawing
multiple viewports (lens flare / light glares, the Falcon 2 laser sight, gun
smoke, shell casings), gated on `mplayerisrunning` or `PLAYERCOUNT()`. **Net
co-op draws ONE local viewport per machine**, so the correct gate is the
single-local-viewport condition, not the game mode.

### The N64-byte-identical single-viewport predicate

Two `g_Vars` flags matter: `mplayerisrunning` is **true** in any MP mode
including co-op; `normmplayerisrunning` is **false** in co-op / anti (set in
`mplayer.c:547-551`, only true for *Combat Sim*). So:

- A site gated `!g_Vars.mplayerisrunning` (off in co-op) re-enables for SP + net
  co-op — and **only** those — with
  `LOCALPLAYERCOUNT() == 1 && !g_Vars.normmplayerisrunning`. On N64 there is no
  single-viewport co-op (co-op is always splitscreen, `LOCALPLAYERCOUNT() >= 2`),
  so this expression is **byte-identical to `!mplayerisrunning`** there.
- A site gated `PLAYERCOUNT() == 1` (e.g. the laser sight — also true for a
  1-human Combat Sim with bots, which we must NOT disable) re-enables with
  `(PLAYERCOUNT() == 1 || (LOCALPLAYERCOUNT() == 1 && !g_Vars.normmplayerisrunning))`.
  Also N64 byte-identical.
- Sites already gated `!g_Vars.normmplayerisrunning` (e.g. `sight.c:121` blue
  sight, `bondgun.c:2937`) **already fire in co-op** — leave them.

### Done (no gameplay-RNG, no desync risk)

| Site | Feature | Change |
|---|---|---|
| `bg.c` `bgCalculateGlaresForVisibleRooms` (~7379) | light glares / lens flare (calc) | `!mplayerisrunning` → `LOCALPLAYERCOUNT()==1 && !normmplayerisrunning`; room test `ROOMFLAG_ONSCREEN` → `bgRoomIsOnscreen(i)` (MP-mode visibility) |
| `bg.c` `bgRenderArtifacts` (~1364) | light glares (render) | same single-viewport gate |
| `bondgun.c` `bgunSwivel` (~5415) | laser-dot crosshair tracking | `!mplayerisrunning` → single-viewport gate |
| `bondgun.c` `bgunRender` (~11468) | Falcon 2 laser **beam** render | `PLAYERCOUNT()==1` → `PLAYERCOUNT()==1 \|\| single-viewport` |
| `bondgun.c` `bgun0f0a5550` (~8573) | Falcon 2 laser **sight** update | same; **plus `isremote` skip** — `g_LaserSights[]` is keyed by hand, not player, so a remote tick must not update/free the local slot |
| `sky.c` `skyRenderSuns` (~2595) | **sun disc(s)** | `mplayerisrunning` early-return → `LOCALPLAYERCOUNT()!=1 \|\| normmplayerisrunning` |
| `sky.c` `skyRenderArtifacts` (~3077) | **sun lens flare** (streak chain from the sun) | same single-viewport early-return gate |
| `gunfx.c` `casingCreateForHand` (~696) | gun **shell casings** | `PLAYERCOUNT()>=2` skip → `LOCALPLAYERCOUNT()>=2` skip; spawn RNG → cosmetic stream |
| `bondgun.c` `bgunTickGameplay2` (~8565) | gun **muzzle smoke** | `PLAYERCOUNT()==1` → `LOCALPLAYERCOUNT()==1`; smoke spawns at world muzzle (works for remote partners); smoke RNG → cosmetic stream |
| `gunfx.c` `beamRender` (~453) | Laser **secondary-fire beam** detail | `PLAYERCOUNT()==1` → `LOCALPLAYERCOUNT()==1`; beam jitter RNG → cosmetic stream |

The glare and laser-sight paths were verified free of `rngRandom()`, so enabling
them on a client/host that renders different things cannot drift the gameplay RNG.

### Casings / smoke / laser-beam: cosmetic-RNG conversion (the desync-safe enable)

These three were initially deferred because their particle randomness drew from
the **synced gameplay** stream (`RANDOMFRAC()` → `rngRandom()`), so re-enabling
them would let an on-screen-dependent effect drift `g_RngSeed` between host and
client → *gameplay* desync, not just visual. The branch already has a cosmetic
RNG stream (`rngCosmeticRandom()`, `src/game/rngcosmetic_c.c`, unsynced + excluded
from the determinism hash), and these files (`gunfx.c`, `casingtick.c`, `smoke.c`)
had been *partially* converted (the integer `% N` calls). The remaining
`RANDOMFRAC()` float calls in those three files were converted to a new
`RANDOMFRACCOSMETIC()` macro (cosmetic-stream sibling of `RANDOMFRAC`, in
`constants.h`). Now the casing/smoke/beam visuals may freely diverge per machine
(short-lived, cosmetic) **without** touching the gameplay seed — which is exactly
the "client-side, OK to desync" property wanted. This also incidentally removes
the *existing* gameplay-seed contamination from explosion smoke (which already
ran in co-op via `smoke.c`).

(`gunfx.c:131` `PLAYERCOUNT()>=2` is the inverse — it *adds* chr-fireslot beam
sharing for 2+ players, already correct for co-op; leave it.)

### Weapon / character models — already full quality in net co-op (verified)

`playerTickChrBody` (`player.c:1441`) has an SP branch
(`!mplayerisrunning || (IS4MB() && PLAYERCOUNT()==1)`) that loads the local
player's body + held-weapon model defs, and an MP `else` branch that just updates
the existing chr (the body/weapon were loaded by the **MP spawn system**). Net
co-op runs `mplayerisrunning == true`, so it takes the MP path — the **same**
full-quality character-body + weapon loading Combat Sim uses. The
`weaponmodeldef`/`PLAYERCOUNT()==1` logic at `player.c:1640-1690` lives *inside*
the SP branch and never runs in co-op. A sweep of `model*.c` / `chr.c` found **no**
viewport-count-gated model LOD / poly reduction anywhere. No change needed.

## Phased order

1. **Groundwork (this doc):** the `PLAYER_IS_NOT_ANTI` strategy, Bucket B
   conversions (behaviour-identical for 2P), splitscreen-feature re-enablement.
   *Player count stays 2 — nothing visible changes yet.*
2. **Lobby/derivation:** host derives N from connected co-op clients; the
   `SVC_STAGE_START` co-op manifest already lists them, so the client mirrors N.
3. **Flip Bucket C:** `netCoopEnterStage(stage, diff, N)`, `chrslots` from N.
   First test at **N=3** (host + 2 clients) before 8.
4. **Targeting/AI for N:** verify NPCs distribute attention across N players (the
   idle `p1p2` cycling already perceives all players; scripted toggles widened).
5. **Polish at 8:** HUD/radar showing N teammates, scoreboard, spawn spread,
   bandwidth (N×N player-move traffic — the byte-budgeted broadcast already
   round-robins, but re-check at 8).

## Done so far (groundwork)

- `objectives.c` COLLECTOBJ / THROWOBJ loops widened to `PLAYER_IS_NOT_ANTI`
  (identical for ≤2 players, correct for N).
- Bucket B shared-logic widenings (all behaviour-identical at 2 players):
  `objectives.c` `objectivesShowHudmsg`, `player.c` co-op revive buddy-pick +
  all-dead mission end, `chr.c` no-autoaim teammate test, `bondgun.c`
  mission-critical-weapon guard. See the Bucket B table for the per-site rule.
  Remaining: `endscreen.c`, `mplayer.c` restart, `chraicommands.c` scripted
  `chr_toggle_p1p2`.
- Splitscreen visual re-enablement for net co-op: light glares (`bg.c`), sun disc
  + sun lens flare (`sky.c`), Falcon 2 laser sight beam + sight update + dot
  crosshair tracking (`bondgun.c`), plus shell casings / muzzle smoke / laser
  secondary beam (`gunfx.c`, `bondgun.c`, with their randomness routed to the
  cosmetic RNG stream so the visual desync can't drift gameplay) — see the table.
- Verified weapon/character models are already full quality in net co-op (MP
  spawn path == Combat Sim); no change needed.
- This design doc.

## Risks

- **Engine is 2-player at heart** — the splitscreen-local surface is large; the
  discipline is to *only* widen shared-logic and never the viewport surface.
- **Revive economics at N** — "steal half a buddy's health" needs a defined
  buddy-selection rule for N players (and what happens with one alive teammate vs
  six). Decide before flipping C.
- **Bandwidth at 8** — every player's move + every NPC's chr-state to N-1 clients.
  The round-robin NPC broadcast helps; player moves are small but N² total.
- **Untestable increments** — convert Bucket B in small, behaviour-identical
  steps verified against the working 2-player slice before flipping the count.
