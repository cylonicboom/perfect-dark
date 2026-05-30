# Port-only Feature: Client Spectator (render-redirect)

A client (or a playing host) can spectate another **player** and see their *exact*
viewport — full first-person, HUD, crosshair, aim — by **rendering the target's
player slot**, not by moving a camera. Triggers on death and via a manual toggle.

Branch: `port-net-predict`. All code is `#ifndef PLATFORM_N64` / net-gated.

## How it was found (the key insight)

A client that **reconnects to a host already mid-match** joins as a JIP spectator
(`is_spectator = 1`, `playernum = NET_PLAYERNUM_SPECTATOR (254)`, no player of its
own). Its single viewport then renders slot 0 — which is a **real remote
combatant (the host)** — so you see the host's complete frame, live. It's clean
because `currentplayerindex == 0` is a genuine player slot; the engine's
slot-0-is-a-real-combatant invariant holds.

That is the opposite of the host-spectator **panels** (`PORT_HOST_SPECTATOR.md`,
`SPEC_MODE_PLAYER`), which put a *fake* player in slot 0 and break that invariant.
So the clean way to spectate a player is to **render their real slot**, which is
what this feature does deliberately.

## Mechanism: the render redirect

`lvRender`'s per-player loop sets `currentplayer = playermgrGetPlayerAtOrder(i)`
then renders that player's full first-person frame (`src/game/lv.c`, ~line 1170).
The redirect, when the local pawn's own viewport iteration is reached and we're
spectating a player target, **substitutes the target's slot**:

- Find the target by **pointer-comparing** `g_NetSpectateChr` against each live,
  **connected** player slot's chr (`g_Vars.players[pn]->client && ->prop &&
  ->prop->chr == g_NetSpectateChr`). Never dereference `g_NetSpectateChr` (it can
  dangle — see Crash safety). The `->client` test drops orphan slots (a
  disconnected client whose chr lingers until the next stage).
- Copy our full-screen `view*` (left/top/width/height, fovy, aspect) onto the
  target so it fills the screen (a remote player's own `view*` can be stale on the
  client), then `setCurrentPlayerNum(target)`.

Gated to: any net session with a local pawn (playing CLIENT **or** playing HOST —
a host `/spec`'ing a client needs this path too), and our own viewport iteration.
The spectator-host (panels) is excluded because it has `player == NULL`.

## Triggers and controls (`port/src/net/net.c`)

| | |
|---|---|
| **On death** | `netSpectateAutoUpdate` (per-frame, client) — death transition → `netSpectateCycle(+1)`; respawn transition → `netSpectateStop()`. Hooked next to `netSpectateApply` in `lv.c`. |
| **Manual toggle** | `netSpectateToggle()` + `/spec toggle` — enter (first live target) / leave. |
| **Cycle** | `/spec next` / `/spec prev` (existing) cycles live players + sims. |
| **First-person pitch** | `netSpectateApply` rides the target player's `vv_verta` (was yaw-only). If pitch is ever inverted, negate `pitchDeg`. |

## Sims (no player slot — camera path)

Sims (`PROPTYPE_CHR`) have **no `g_Vars.players` slot and no first-person
HUD/weapon state**, so the render redirect can't apply. They use the camera path
(`netSpectateApply`): camera at the sim's eye, looking where it faces. A sim's
`prop->pos` is its **centre**, so we nudge `+50` to head height (matching the
host-spectator's `spectatorTargetEyeAndForward`) — otherwise the camera sits
inside the model (the "torso" view). Sim spectate is therefore a clean
first-person **world** view (no player HUD), yaw-only (sims don't expose a synced
aim pitch like players' `vv_verta`).

## Crash safety: `g_NetSpectateChr` can dangle

`g_NetSpectateChr` is a raw `chrdata*` that can be **freed under us** (target
leaves; chr torn down at round-end). Crashes (`0xc0000005` in `lvRender`) came
from dereferencing it. Four independent guards:

1. **Redirect** finds the target by pointer comparison only (above) — never
   derefs `g_NetSpectateChr`, and skips orphan slots.
2. **Disconnect clear** (`netServerEvDisconnect`): `netSpectateStop()` when the
   spectated client's peer disconnects, before its backlink is nulled.
3. **Stage-end clear** (`mainEndStage`, `port/src/pdmain.c`): `netSpectateStop()`
   at entry, so round-over / "end match" / disconnect drops the target before the
   teardown frees it. Reached on host and client.
4. **Camera path** (`netSpectateApply`): validates `g_NetSpectateChr` against the
   live `g_MpAllChrPtrs` list (pointer-only) before any deref; stops if gone.

### Render-body-runs-gameplay caveat

`lvRender`'s loop body runs per-player **gameplay**, not just drawing (eyespy
fire, interact, reload, pickup). Rendering the target's slot runs *its* gameplay
with `currentplayer = target`. The JIP-spectator does the same and is mostly fine
because that gameplay is gated on the target's *synced* input state, rarely set on
the spectator. One gap surfaced: the eyespy `Z`-press block dereferenced
`eyespy->prop` unguarded → crash on a remote target whose eyespy is partial at
round-end. Patched with an `eyespy->prop` NULL-check in `lv.c` (always true on
N64). If other round-end spectate crashes appear, the heavier fix is to skip the
input-driven gameplay block entirely when `currentplayer` is a spectate target.

## Files touched

```
src/game/lv.c            render redirect + eyespy->prop guard + netSpectateApply hook
port/src/net/net.c       netSpectateToggle, netSpectateAutoUpdate, FP pitch, sim eye
                         nudge, mpchr-list validation, disconnect clear, /spec toggle
port/include/net/net.h   netSpectateToggle / netSpectateAutoUpdate decls
port/src/pdmain.c        mainEndStage spectate clear
```

## Known limits / future work

- **Client → another client is jumpy** — the spectated remote player's *position*
  is interpolated (Fix #1) but their **view angles are snapped** (deliberate for
  hit-reg). Smoothing the spectate camera's `vv_theta`/`vv_verta` (without
  touching the hit-test aim) would make it as smooth as client → host. Candidate
  for the Fix #2 family.
- **Manual toggle is console-only** (`/spec toggle`); a key bind is a follow-up.
- **Sims** are camera-based (no HUD, yaw-only) — inherent to them not being
  players.
