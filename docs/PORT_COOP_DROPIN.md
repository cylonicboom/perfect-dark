# Port-only Feature: Co-op Drop-in (mid-mission join / leave / rejoin)

A client connecting while a campaign co-op mission is running spawns as a
co-op partner **immediately** (checkpoint respawn) instead of spectating
forever — co-op has no round boundary, so the Combat Sim "spectate until
next round" model can never seat anyone there. A leaver's slot is held under
their name and reclaimed on rejoin. Protocol 61.

## How it works

1. **Pre-allocation (the linchpin).** Net co-op stages always allocate
   `NET_COOP_MAX_SLOTS` (4) player slots regardless of the connected count
   (`netCoopEnterStage` → `setNumPlayers`), so every machine — including a
   future joiner's fresh stage load — produces the identical player/prop/
   syncid layout (the deterministic-allocation invariant). The connected
   count still sizes the shared F3 lives pool. Splitscreen co-op unchanged.
2. **Dormant slots.** At stage load, `netCoopDormantInit` (called from
   `netSyncIdsAllocate`, after `netPlayersAllocate` bound the connected
   clients) parks every unbound slot: `isdormant=1`, dead-with-finished-anims
   ("dead awaiting respawn" — the engine state that gets enemy-ignore and
   buddy-skip for free), chr `CHRCFLAG_HIDDEN`.
3. **Claim.** A mid-mission joiner goes through the JIP path (spectator +
   `SVC_STAGE_START` + world load) and sends `CLC_STAGE_READY`; the server
   ships the JIP catch-up snapshot, then `netServerCoopClaim` picks a slot
   (name-reserved first, else first dormant), seats it (`netCoopSeatClient`),
   revives the pawn through the normal respawn path (`dostartnewlife` →
   `playerStartNewLife` + the established spawn force-snap), and broadcasts
   **`SVC_COOP_CLAIM`** `{clientid, playernum, name, bodybit}` — which is
   also how the other clients learn the joiner exists.
4. **Leave / rejoin.** On a mid-mission co-op disconnect the slot is reserved
   under the leaver's `settings.name`, the pawn parks dormant, and a release
   broadcast (`SVC_COOP_CLAIM` with `clientid = NET_NULL_CLIENT`) parks it on
   every client. A joiner whose name matches a reservation reclaims that slot
   (fresh checkpoint spawn); reservations last until mission end.
5. **NPC ghost healing.** A joiner fresh-loads ALL stage NPCs alive, but the
   host freed the ones killed before the join (their `SVC_PROP_FREE` predates
   the connection). `SVC_PROP_RECONCILE` now also lists **chr** syncids in
   co-op; the client `CHRHFLAG_DELETING`s any synced chr the host doesn't
   have (the `netmsgSvcPropFreeRead` chr path). Combat Sim excluded (sims are
   never freed mid-match).

## Surface

| File | What |
|---|---|
| `src/include/types.h` | `struct player.isdormant` (port-only block) |
| `port/include/net/net.h` | `NET_COOP_MAX_SLOTS`, `netCoopSeatClient` / `netServerCoopClaim` / `netCoopDormantSlot` decls, proto 61 |
| `port/include/net/netmsg.h` | `SVC_COOP_CLAIM 0x54` + read/write decls |
| `port/src/net/net.c` | slot inflation in `netCoopEnterStage`; `netCoopDormantInit/Slot`, `netCoopSeatClient`, `netServerCoopClaim`, `g_NetCoopReservedNames[]`; release-on-disconnect in `netServerEvDisconnect`; `netRestoreLocalProfile` (now incl. contpads); dormant-init call in `netSyncIdsAllocate` |
| `port/src/net/netmsg.c` | claim write/read; claim trigger in `netmsgClcStageReadyRead`; chr syncids in the reconcile write + chr-ghost reaping in the read |
| `src/game/player.c` | all-out mission-fail loop skips dormant slots (their unspent F3 lives made the mission unfailable); dormant slots never read local respawn input (their mpReset contpad maps to a real local device) |
| `src/game/lv.c` | net render rollback keys on the LOCAL player's render order instead of hard-coded order 0 — a claimant binds at wire slot N **without** the load-time slot-0 swap |

## Design notes / known limitations

- **No mid-mission slot-0 swap.** A normal co-op client is swapped to local
  slot 0 at stage load; a claimant stays at its wire slot N (swapping
  mid-mission would re-thread live syncids). Consequences handled: render
  rollback keys on the local order (lv.c), contpads restored from the local
  profile (`netRestoreLocalProfile` — slot N's mpReset default is pad N).
  Residual risk: machine-local `g_Vars.bond` (= players[0]) is the HOST's
  pawn on a claimant's machine; engine paths that key on `bond` may behave
  as if the host is the primary player there.
- **Body type:** the claim carries the joiner's F2 body bit, but the dormant
  pawn's chrbody was built at stage start — a mismatched choice may not
  apply until a model rebuild. Cosmetic.
- **Claimant pawn state** is the stage-start spawn state revived in place:
  position from the host's respawn force-snap, health from the
  `SVC_PLAYER_STATS` heartbeat.
- **5+ connectees**: only `NET_COOP_MAX_SLOTS` (4) seats exist (the locked
  co-op plan cap); extra joiners stay spectators ("no free co-op slot").
- Reservations are name-based (`settings.name` exact match) — a joiner using
  a different name takes a fresh slot instead of their old pawn.
