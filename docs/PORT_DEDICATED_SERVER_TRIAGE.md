# port-net-predict — Dedicated Server Triage (live)

Active bug investigation on top of commit `8b3d2359d` ("dedicated server framework"). Live notebook — append findings as they happen. Promote stable conclusions to `PORT_NET_PREDICT_CHANGES.md` (what changed and why) or `PORT_NET_REVERTED_EXPERIMENTS.md` (failed approaches) when the dust settles.

---

## Current brick wall

**Symptom**: clients connected to a dedicated server **do not sync with the host**. Specifically:

- Cannot see sims (bots fall through the world from the client's perspective, ending up at Y ≈ -56 000)
- Cannot pick up weapons or ammo
- Cannot respawn after death
- Remote players appear frozen or grossly out-of-sync

### Repro

Dedicated server (`--dedicated` or `--dedicated-windowed`) + at least one connected client. Manifests immediately after the first stage starts.

### Working hypothesis — **identified, fix applied uncommitted (2026-05-30)**

`port/src/net/net.c::netEndFrame`, the per-tick server-broadcast block at the old line 1355 was gated on `g_NetLocalClient->state == CLSTATE_GAME && g_NetLocalClient->player && g_NetLocalClient->player->prop`.

- `netStartServer` sets `g_NetLocalClient->is_spectator = 1` in dedicated mode.
- `netPlayersAllocate` explicitly sets `cl->player = NULL` for any spectator client (line 1711).
- So `g_NetLocalClient->player` is always NULL in dedicated → gate evaluates FALSE every tick → entire broadcast block skipped.

The block covers: `SVC_PLAYER_MOVE` per remote client, `SVC_PROP_MOVE` per sim, KoH / score / stats heartbeats, and lag-comp save. None of those went out in dedicated mode. That's a single-point root cause for every reported symptom — sims fall through floor because clients run local `chrTick` (which doesn't include floor collision for AI-disabled chrs) with zero server overrides to clamp them; remote players freeze for the same reason via `bwalkUpdateRemote`'s starved `inmove[]` ring; pickups never reach other clients because `SVC_PROP_PICKUP` is reliable-queued but no per-tick state ever validates it; respawn force-corrections are queued into `SVC_PLAYER_MOVE` which is gated out.

The earlier "cl=2 inmove_tick frozen for 17 s" Finding 1 — moved to "Hypotheses ruled out" below. With nothing being broadcast either way, server-side per-peer freeze was downstream noise.

### Fix applied

`port/src/net/net.c` line 1355 area — replaced the single gate with split conditions:

```c
if ((g_NetMode == NETMODE_CLIENT
        && g_NetLocalClient->state == CLSTATE_GAME
        && g_NetLocalClient->player && g_NetLocalClient->player->prop)
        || (g_NetMode == NETMODE_SERVER
        && g_NetLocalClient->state == CLSTATE_GAME)) {
```

Client branch unchanged: still requires its own `player+prop` to record/send a move. Server branch newly allowed through with `state == CLSTATE_GAME` only — its inner loop at line 1370 already does per-client `cl->state >= CLSTATE_GAME && cl->player` checks before dereferencing, so NULL safety is preserved.

In non-dedicated host-and-play this is **identical** behaviour (the host's local player exists, gate was TRUE either way). Only dedicated/spectator-host mode behaviour changes.

### Test result after gate fix (2026-05-30, uncommitted)

**Progress confirmed:**
- ✅ Sims now visible to clients — `SVC_PROP_MOVE` broadcast flowing for bots.

**Still broken:**
- ❌ Pickups not working.
- ❌ Respawn not working — client died, never came back; force-disconnected + reconnected and was re-admitted as a JIP spectator.
- ❌ **New crash**: with a stale prop-less player slot left over (dead-without-respawn client), the next combatant who punched a sim crashed the server with `0xc0000005` at `src/game/chraction.c:5527` — `g_Vars.players[i]->prop` deref where `prop == NULL`. addr2line frame #0 conclusive; deeper frames unreliable (FPO + optimization).

#### Fix applied for the crash

`src/game/chraction.c::chrIsRoomOffScreen` — port-only `#ifndef PLATFORM_N64` NULL-skip on `g_Vars.players[i]` and `->prop` before the dereferences at lines 5525/5527. Sim AI calls this on damage events and any other path that touches sim AI room-visibility checks; the NULL-skip prevents the crash but does not address the underlying invariant violation (a player slot should ordinarily always have a prop). Logging both conditions wherever the prop is being cleared without re-creation would be the next investigation.

#### Open questions for next session

- Why does respawn not work? Hypothesis: `playerStartNewLife` has more headless-tier dependencies than the user's `mainTick` scaffolding currently covers. With broadcasts now flowing, a fresh log run with respawn attempted should give us probes to chase. Add a `respawn_start` / `respawn_done` probe pair around `playerStartNewLife` to verify whether it even runs.
- Why don't pickups fire? Hypothesis: pickup detection in `propsTickPlayer` works for the local player slot, but in headless `setCurrentPlayerNum(j)` is set to each remote combatant — `objPickupCollide` may have invariants assuming `currentplayer` is local (e.g., reading local input or HUD state) that silently fail for remote slots. Add a `pickup_attempt` probe at the pickup collision site.
- Why does a player slot end up with NULL prop in the first place? On clean disconnect, `g_Vars.players[i]` should be freed/reused. On death without respawn, prop may be cleared but slot kept. Need to instrument the prop-clear path for that player.

### Round 3 (2026-05-30, uncommitted) — diagnostic probes + stronger crash guard

After Phase-1 exploration confirmed that `netClientReset` doesn't clear `g_Vars.players[]` slots and `playerStartNewLife → playerResetDefaults` reassigns `player->prop` via `propAllocate()` without freeing the old, the same crash recurred at `chraction.c:5534` (PC offset `0xa81d2`, decoded via `addr2line --section=.text`). The NULL guard passed; the prop pointer was stale.

**Stronger guard** (`src/game/chraction.c::chrIsRoomOffScreen`):
- Replaced the single NULL check with: NULL check, then verify `prop` is inside `g_Vars.props[0..maxprops)`, then verify `prop->type == PROPTYPE_PLAYER`. Catches both "freed-and-reused-as-different-type" and "pointer to heap garbage outside the prop array".

**Probes added** (all use existing `netDiagLogf`):
| Probe | File:line | Fires when |
|---|---|---|
| `pickup_attempt` | `src/game/propobj.c::propPickupByPlayer` entry | Pickup function called — logs prop syncid, objtype, current pnum, `client` ptr, isdead, netmode |
| `pickup_svc_write` | `src/game/propobj.c` ~17579 | `SVC_PROP_PICKUP` actually written — logs cl id, prop syncid, result |
| `respawn_ucmd_seen` | `port/src/pdmain.c` headless respawn-detect | UCMD_RESPAWN observed in remote player's inmove — logs pnum, cl id, isdead, dostartnewlife |
| `respawn_invoke` | `port/src/pdmain.c` headless respawn-detect | `playerStartNewLife()` about to be called |
| `respawn_start` | `src/game/player.c::playerStartNewLife` entry | Function entered — logs pnum, netmode, isremote, isdead |
| `force_move_write` | `port/src/net/netmsg.c::netmsgSvcPlayerMoveWrite` has_force branch | SVC_PLAYER_MOVE with force bits being written — logs cl id, ucmd, pos |
| `force_move_apply` | `port/src/net/netmsg.c::netmsgSvcPlayerMoveRead` FORCEMASK apply | Client receives FORCEMASK move — logs cl id, ucmd, target pos, before pos |

Diagnosis matrix for the next log run is in the plan file (`C:\Users\tidbu\.claude\plans\memoized-greeting-shell.md`). Probes will be removed once root causes are identified.

### Round 4 (2026-05-30, uncommitted) — pickup root cause + diagnostics deepening

Round 3 test results: none of the new probes fired in either log. Crash recurred at the same `chraction.c:5539` (PC `0xa81d2`).

**Pickup root cause identified:**
- `propsTestForPickup` (the entry point to pickup detection) is **only called from `src/game/lv.c:1416`** — inside `lvRender`'s per-player loop, which is skipped in headless. The user's existing `mainTick` headless block calls `propsTickPlayer / scenarioTickChr / propsSort`, but `propsTickPlayer` does **not** call `propsTestForPickup`. So `propPickupByPlayer` never runs in dedicated mode and the `pickup_attempt` probe never fires.
- **Fix**: added `propsTestForPickup()` and `currentPlayerInteract(false)` (gated on `bondactivateorreload & JO_ACTION_ACTIVATE`) to the `mainTick` headless block after `propsSort()`. Mirrors lv.c:1391–1416 verbatim minus the rendering calls.

**Respawn — diagnostics deepening:**
- Existing `respawn_ucmd_seen` / `respawn_invoke` probes are INSIDE the conditional gate `(p->isremote && p->isdead && p->client && !mpIsPaused() && g_NumReasonsToEndMpMatch == 0 && ucmd & UCMD_RESPAWN)`. If any single condition fails, none of the probes fire — which is what we saw.
- **Added** `respawn_dead_state` probe OUTSIDE the gate, throttled to 1 Hz per remote player slot. Logs `isdead`, `client` ptr, paused state, end-match flag, full `ucmd`, and `dostartnewlife`. Next run will show which condition is missing.

**Crash — instrument the loop:**
- `chrIsRoomOffScreen` is called from at least six AI paths in `chraction.c` (lines 6368, 6544, 13046, 13083, 13318, 13326) — all sim AI / waypoint logic. Sim AI runs during `propsTickPlayer`. The crash recurs because *something* fails inside the loop after the guards pass.
- **Added** two probes inside `chrIsRoomOffScreen`:
  - `chroffsc_skip` — fires when any of the three guards (NULL, bounds, type) filters a slot. Tells us whether the guards are actually catching stale slots in practice.
  - `chroffsc_iter` — fires immediately before the unprotected `portal00018148 / arrayIntersects` calls, throttled to one per `g_NetTick`. The LAST entry before the crash identifies the offending slot's `pp`, `type`, and `pos`.

#### Next-run diagnosis matrix (round 4)

| Symptom | What to grep for | Conclusion |
|---|---|---|
| pdhost.log has `pickup_attempt` entries during a pickup attempt | grep `pickup_attempt` | propsTestForPickup is being reached. Then check `pickup_svc_write` — if missing, the gate `g_NetMode == NETMODE_SERVER && currentplayer->client` is failing (check the `client=` field). |
| pdhost.log has no `pickup_attempt` after walking on a pickup | grep `pickup_attempt` | propsTestForPickup not being called for the correct player slot — verify `setCurrentPlayerNum` matches the user's combatant slot. |
| `respawn_dead_state` logs `isdead=0` while player is visibly dead | grep `respawn_dead_state` | Server-side death detection didn't fire — `chrDamage → playerDie` path isn't being hit on the server. Need to check chrDamage call path in headless. |
| `respawn_dead_state` logs `isdead=1` but `client=0` | grep `respawn_dead_state` | netClientReset cleared the backlink after a disconnect; reconnect didn't re-bind. JIP-spectator path issue. |
| `respawn_dead_state` logs `isdead=1 client=valid ucmd=0x00000000` | grep `respawn_dead_state` | Server is receiving CLC_MOVE but UCMD_RESPAWN bit isn't set — client side isn't sending respawn. Check `bondmove.c:556` gate (requires `isdead` on client side too). |
| Server crashes; last `chroffsc_iter` line shows `pp=…type=6 pos=(x,y,z)` | grep `chroffsc_iter` then look at next-tick crash | The pp guards pass but `portal00018148` or `arrayIntersects` still crashes — `prop->rooms` is the only remaining unprotected read. Add NULL check there. |
| Server crashes; no `chroffsc_iter` line in last second | grep `chroffsc_skip` | Crash isn't at line 5546+ at all — addr2line was misled by optimization. Look at chr access at line 5504 (`chr->prop->pos / rooms`) instead. |

### Round 5 (2026-05-30, uncommitted) — pickups confirmed, respawn root cause = death-state machine

**Pickups confirmed working** after the round-4 fix (adding `propsTestForPickup()` + `currentPlayerInteract()` to mainTick). User reports pickups + shooting sims both work now.

**Respawn root cause:** at server tick 11280 the `respawn_dead_state` probe shows the full chain working:
- `isdead=1` (death detection works)
- `ucmd=0x08000080` (user is pressing UCMD_RESPAWN)
- `client=valid`, `paused=0`
- BUT `endmatch=1` (g_NumReasonsToEndMpMatch != 0)

The user's mainTick gate requires `g_NumReasonsToEndMpMatch == 0`, matching the original gate at `player.c:5151`. With `endmatch=1` the gate blocks respawn — which is correct behaviour: when the score limit is hit, the round is ending and respawn is intentionally blocked until the next round.

The deeper problem is that the **round never ends**. `lvTickPlayer` at `lv.c:2402` only calls `mainEndStage()` when `numdying == 0`, where `numdying` is the count of dead players whose `redbloodfinished/deathanimfinished/colourfadetimemax60` flags haven't advanced. Those flags are normally advanced by the death state machine inside `playerRenderHud` (`player.c:4966+`), which doesn't run in headless. So:

```
score limit reached → endmatch>0 → player dies → deathanimfinished stays false
→ numdying stays > 0 → mainEndStage never fires → match never ends
→ next round never starts → respawn gate blocked forever
```

**Fix applied (uncommitted):** `port/src/pdmain.c::mainTick` headless block — when a remote player has `isdead != 0`, directly advance the death-terminal state:
- `isdead: 1 → 2`
- `deathanimfinished = true`
- `redbloodfinished = true`
- `colourfadetimemax60 = -1` (if positive)

No animation to wait for in headless; setting the flags immediately lets `numdying` reach 0 in `lvTickPlayer`, fires `mainEndStage()`, transitions to the scoreboard/vote, starts the next round via the playlist machine. `playerResetDefaults` (called from `playerStartNewLife`) zeroes these fields again on respawn, so the advancement won't re-fire.

Note: with this fix, respawn during a single round is still blocked once score limit is reached (same as original behaviour). What's restored is round-to-round transition. If a user dies before score limit is hit, the existing UCMD_RESPAWN → dostartnewlife → playerStartNewLife chain should fire immediately — the round-4 mainTick mirror already covers that path.

### Round 6 (2026-05-30, uncommitted) — orphaned player slot after mid-match disconnect

**Diagnosis from `pickup_attempt` probes:**
- Up to tick 30511: `pickup_attempt … client=00007ff6e1d1e710 isdead=0 …` — pickups working, valid backlink.
- From tick 31368: `pickup_attempt … client=0000000000000000 isdead=1 …` — `players[0]->client` is now NULL but the pickup loop still fires.
- `disconnect` event in the diag log not logged until tick 31440 (later) — that's `netDisconnect()`'s log line, which only fires on the local-client teardown path. `netServerEvDisconnect` only does a `sysLogPrintf` so the mid-match server-side disconnect that nuked the backlink is silent in the diag log.

**Root cause:**
1. Client disconnects mid-match → `netServerEvDisconnect` → `netClientReset` clears `players[N]->client = NULL` and zeroes the netclient struct.
2. `g_Vars.players[N]` itself is NOT cleared (only `playermgrAllocatePlayers` at the next stage transition does that), so the orphaned slot persists with a stale-ish prop and chr.
3. Between disconnect and the next stage's `playermgrAllocatePlayers → netPlayersAllocate`, no rebind happens. The orphan slot has `client == NULL` but is still iterated by `propsTickPlayer` (pickup detection), `chrIsRoomOffScreen` (sim AI), and any other code that walks `g_Vars.players[]`.

**Visible symptoms:**
- Silent pickups: `propPickupByPlayer` runs for the orphan slot, consumes the prop's pickup state (sets `OBJHFLAG_DELETING` / drops it from the prop list), but the SVC_PROP_PICKUP wire-write gate at `propobj.c:17579` (`g_NetMode == NETMODE_SERVER && g_Vars.currentplayer->client`) fails on NULL client → no broadcast. Other clients see the weapon vanish with no kill / no pickup message.
- Reconnect-and-punch-sim crash: `chrIsRoomOffScreen` iterates the orphan slot. The previous three-layer guard (NULL, bounds, type) wasn't sufficient — the prop pointer can still be valid-looking enough to pass type==PROPTYPE_PLAYER while the chr's prop linkage is in a half-cleaned state, and the next deref into `prop->pos` / `prop->rooms` crashes.

**Fix applied (three places):**
| File | Change |
|---|---|
| `port/src/pdmain.c::mainTick` headless loop | Skip combatant slot when `currentplayer->client == NULL` — don't tick props for an orphan. |
| `src/game/chraction.c::chrIsRoomOffScreen` | Add fourth guard: skip slots with NULL client. Logs `chroffsc_skip reason=orphan`. |
| `src/game/propobj.c::propPickupByPlayer` | Early-return `TICKOP_NONE` when `g_NetMode == NETMODE_SERVER && !currentplayer->client` — prevents the silent consume on a server-side orphan. Safe outside dedicated (solo / host-and-play have non-NULL client in the legitimate cases). |

**Open problem (deferred):** the orphan window itself is the bug. The proper fix is to rebind player↔netclient on JIP reconnect so the slot is never orphaned. Two reasonable approaches:
1. Have `netServerEvConnect`'s JIP branch search for an orphan slot (`players[i] != NULL && players[i]->client == NULL` and `players[i]->isremote`) and bind the new netclient to it immediately — avoids waiting until next round.
2. Call a narrowed `netPlayersAllocate` from `netServerEvConnect` after the JIP setup completes.
The defensive guards above keep tests progressing without crashes; do the rebind in a follow-up.

### Round 7 (2026-05-30, uncommitted) — orphan slot crash recurred via lvTickPlayer

Round-6 fix wasn't sufficient. New crash log shows server hangs ~8.7 s after the user disconnects (tick 1920 wall 33.8 s → last log entry tick 3486 wall 61.9 s → `disconnect` event at wall 70.5 s = `netDisconnect` cleanup from a fault).

**Why round-6 guards weren't enough:** the headless gameplay-tick block in `mainTick` got an orphan-skip in round 6, but **`pdmain.c::mainTick` has a SECOND per-player loop at lines 703–724** that runs `lvTickPlayer()` (the level/player tick — physics, input handling, lookingatprop, the whole `playerTick` chain). That loop runs in both headless and non-headless paths and was missed by the round-6 patch.

After disconnect, the orphan combatant slot still has `players[0]` non-NULL with chr / aibot / bgun state that hasn't been cleanly torn down. `lvTickPlayer` walks into that state, hits the sim-AI path via `chrIsRoomOffScreen`, and faults. The probe `chroffsc_skip` didn't fire because crash happens *before* the loop's first guard line reads — the stale chain corrupts something earlier than the explicit deref.

**Fix:** mirror the orphan-skip in the lvTickPlayer loop. Same precondition (`!currentplayer->client && g_NetMode == NETMODE_SERVER`); skip the tick entirely (don't even fall back to `spectatorTickPanel` since the slot isn't a spectator). Logs `mt_lvtp_skip_orphan` when `mt_log` is active so we can see it firing during the next test run.

**Files modified this round:**
| File | Change |
|---|---|
| `port/src/pdmain.c` (lines 703–724) | Orphan-skip branch added to the `lvTickPlayer` per-player loop, between the spectator branch and the default branch. |

Defensive coverage now spans all four per-player loops touched in the headless dedicated path:
1. `mainTick` lvTickPlayer loop (this round)
2. `mainTick` headless propsTickPlayer loop (round 6)
3. `chrIsRoomOffScreen` players iteration (round 6, layered guard)
4. `propPickupByPlayer` entry (round 6, early-return)

The proper fix (rebind on JIP reconnect) remains the right long-term answer; these guards just keep tests moving.

### What may still not work after this fix

- **cl=2's 17-second CLC_MOVE pause** (originally Finding 1): independent of the gate bug. Likely a defocused-window or process-suspend artifact on one of the two client instances running on this machine. After the gate fix this should manifest as "remote player visibly frozen for 17 s while server is starved of their input" instead of "frozen forever because nothing ever broadcasts anyway". Not a fix-required bug — testing with focus-aware clients (or one client per machine) will tell us if it's a real protocol issue or a Windows focus-loss-pause.
- **Respawn correctness**: depends on `playerStartNewLife` working in headless. It's been called by the user's `mainTick` headless gameplay-tick scaffolding (Section A) but never verified end-to-end because no force-correction `SVC_PLAYER_MOVE` was ever reaching clients to test it. May surface more bugs once broadcasts flow.
- **Pickup detection on remote clients**: depends on the server's `propsTickPlayer` (run per non-spectator slot in the user's `mainTick` addition) correctly detecting pickup overlaps for remote players. Probably works since `bwalkUpdateRemote` updates the remote player's `prop->pos` and the standard prop tick reads that. Worth a probe.
- **Multi-client log interleaving** (Finding 2): purely a diagnostic confound, not a bug in dedicated. Sidebar fix: include PID in netDiag log filename so concurrent client instances don't clobber each other's logs. Independent of this fix.

---

## Log evidence (run captured 2026-05-29)

Files: `F:\Games\Perfect Dark Netplay\__SERVER\pdhost.log` (4752 lines) and `F:\Games\Perfect Dark Netplay\pd.log` (4445 lines).

### Finding 1 — Server's CLC_MOVE stream from cl=2 stalls for 17 seconds (real bug)

Server saw two clients (both `name=Murk`, slots cl=1 and cl=2). cl=1's inputs and broadcast position progress normally. **cl=2's `inmove_tick` freezes at 851 from server tick 900 (wall ≈ 15.15s) through tick 1860 (wall ≈ 31.4s), then jumps to 1936 at tick 1920 (wall ≈ 32.4s).** That's a 17-second gap during which:

- `bwalkrem_enter` for cl=2 logs identical `inmove_tick=851 inmove_pos=(2067.0, 167.0, 623.0)` every probe.
- `bwalkrem_exit_force` fires every tick → `cl->forcetick` stays non-zero (it would have cleared via `netmsgClcMoveRead` if any CLC_MOVE landed).
- Server-broadcast `pos_cl id=2` is identical every tick: `x=2067.0 y=157.0 z=623.0 ping=23 theta=224.98 verta=0.00`. ping does not change during the gap (RTT field not updated → no packets arriving from cl=2 to derive RTT from).

After the gap, cl=2's `inmove_tick` jumps by 1085 (≈18 s @ 60 Hz), matching the wall-clock gap → **cl=2's client kept counting ticks the whole time but the server received nothing.** Single fresh packet at tick 1936, not a flood; rules out "ENet was buffering and finally drained." Once the stream resumes, `bwalkrem_exit_interp` replaces the force path (forcetick auto-clears) and cl=2's Y falls from 167 → 30 over a few ticks as gravity catches up server-side.

Three explanations rank-ordered by likelihood:

1. **cl=2 client process paused for 17 s** (window defocus, OS suspend, GPU stall, breakpoint, etc.). Stops sending CLC_MOVE while its NetTick keeps counting (timer-driven, not paint-driven). Fits the single-fresh-packet resume perfectly.
2. **ENet reliable-channel stall on that one peer.** Would normally produce a disconnect after ~30 s, but localhost wouldn't trigger that. Possible if the OS pinned the receive socket buffer for cl=2's UDP source-port.
3. **Server-side per-peer starvation.** Receive path enumerating peers in a way that starves one. Unlikely given cl=1 was handled correctly throughout.

Cheap discriminators to add:
- Log every `enet_host_service` event with `peer->incomingDataTotal` / `outgoingDataTotal` deltas; if peer's incoming counter is flat during the gap → (1) or (2). If non-flat but server still doesn't advance inmove_tick → server-side bug.
- Add `clcmove_recv` probe (cl, inmove_tick, server NetTick) in `netmsgClcMoveRead`. Bins (1)/(2) cleanly: if no log lines for cl=2 during the gap, the packet never reached our parser.

### Finding 2 — `pd.log` is being written by two processes concurrently (setup confound)

`pd.log` is unreliable as a client-side ground truth. Evidence:
- Line 911: `744,7.748,12.549,dmg_sim,chrnum=1 sid=47 dmg=1.20 chrdmg=2.40 stamp=345` — literal byte interleaving mid-line. The `744,7.748,` prefix belongs to one writer; `12.549,dmg_sim,...` is a partial line from another.
- Line 908: pos_sim line truncated mid-token (`...yrot=2.338 ani744,7.241,tick,stage=...`).
- Wall-clock regressions: line 832 has `720,12.082,...` then line 909 has `744,7.257,...`. NetTick advanced 720→744 while wall went 12.082s → 7.257s. Two processes with independent `realtime_s` origins flushing to the same file.
- `bwalkrem cl=` identity flips from `cl=1` to `cl=0` at line 983 with no intervening reconnect/disconnect event — different process tables.

Either two client instances were launched against the same diag-log path, OR a previous run was appended without truncation and the file got crossed up. Until the writer count is exactly one, no client-side claim from this file is trustworthy.

Mitigation (cheap): make the diag-log filename include PID or instance name (e.g. `pd_<pid>.log`), or open with `O_EXCL` and skip if it fails. Code path is in `netDiagOpen` (whatever opens the log on `--netdiag <path>`).

### What the logs do **not** show

- No `ERROR` / `WARNING` / `CRASH` markers in either file.
- No `disconnect` event on the **server** during the cl=2 gap — server still thought the peer was connected throughout. (`pd.log` has two `disconnect,wasingame=1` entries at client wall 34.7s and 39.9s — these are from after the freeze ended, and given the multi-writer issue, may be from either instance.)
- No `stage_start` / re-match during cl=2's gap — server stays in stage=50 clstate=GAME the whole time. So the freeze isn't a stage-transition artifact.

---

## Trail of attempts (uncommitted on top of 8b3d2359d)

---

## Trail of attempts (uncommitted on top of 8b3d2359d)

Grouped by hypothesis. All edits live in the working tree right now — `git diff HEAD` to see exact code.

### A. Headless gameplay-tick scaffolding (fundamental — most other fixes depend on this)

`lvRender` is skipped in headless, and **it owns gameplay-tier calls**, not just rendering: `propsTickPlayer` / `scenarioTickChr` / `propsSort`. Without them, sim AI never advances and the server broadcasts stale positions.

- **`port/src/pdmain.c::mainTick`** — added a headless gameplay loop before the render bail-out: per non-spectator slot it sets `setCurrentPlayerNum(j)` then calls `propsTickPlayer / scenarioTickChr / propsSort`. Also sets `g_Vars.alwaystick = 1` so the foreground gate at `prop.c:2006` treats every active prop as foreground (otherwise `PROPFLAG_ONANYSCREENPREVTICK` — populated by the render path — gates every prop tick to no-op).
- **`port/src/pdmain.c::mainTick`** — added respawn detect+consume mirroring the normally render-tier flow (`playerRenderHud` sets `dostartnewlife` on `UCMD_RESPAWN`, `lvRender` calls `playerStartNewLife`).

### B. NULL-deref guards for render-tier game code called from gameplay

- **`src/game/camera.c::camGetWorldToScreenMtxf`** — return a static identity matrix when `worldtoscreenmtx` is NULL. `projectileFindCollidingProp` etc. still call this during `propsTickPlayer`; returning NULL crashes `mtx4TransformVec`. Identity makes collision math degrade to plain world-space (less efficient broad-phase, still correct).
- **`src/game/player.c::playerTickThirdPerson`** — early-return when `g_NetDedicatedMode == 1`. It's pure render-tier (third-person pose for someone else's view); skipping avoids NULL deref on `camGetProjectionMtxF()`.

### C. Ghost-host elimination (phantom local-player chr/prop visible to remote clients)

Commit 8b3d2359d's "Path A" used `g_SpectatorPanelCount = 1` to satisfy slot-0 invariants; this turned out to spawn a visible ghost player. Switched to **zero panels** everywhere:

- **`port/src/net/net.c::netStartServer`** — `g_SpectatorPanelCount = 0` (was 1).
- **`port/src/spectator.c::spectatorAllocatePanels`** — early-return at 0 in dedicated (don't let it clamp to 1).
- **`port/src/pdmain.c::mainLoop`** — `numplayers = combatants` only in dedicated.
- **`port/src/net/netmenu.c::menuhandlerHostStart`** — don't apply `g_NetMenuHostSpectator` in dedicated CLI flow (would re-spawn the ghost).
- **`port/src/net/net.c::netSyncIdsAllocate`** — skip the prop-existence check + swap when the local client is `is_spectator` (covers JIP-as-spectator: no player/prop until next round's `mpStartMatch`).

### D. Server view of remote players stale at spawn (in-flight, **active probes live**)

Hypothesis from comment in `bondwalk.c`: the FORCEMASK skip-guard added to fix client-respawn (commit `ad45ea3e2`) locks the server's view of remote players forever when `cl->forcetick == 0`. Init code seeds `pl->ucmd` with FORCEMASK bits but `forcetick` stays 0, so the auto-clear path (`netmsgClcMoveRead`, which only clears when `forcetick != 0`) never fires.

- **`src/game/bondwalk.c::bwalkUpdateRemote`** — gate the FORCEMASK return on `cl->forcetick != 0`; one-shot-clear stale FORCEMASK bits when `forcetick == 0`.
- **Probes** (throttled to ~1 Hz, no-op unless `netDiagLogf` log open):
  - `bwalkrem_enter` — every call: cl id, pnum, isremote, ucmd, inmove tick/pos, prop pos, interp ticks.
  - `bwalkrem_skip_force` — the legitimate skip path.
  - `bwalkrem_exit_force` / `bwalkrem_exit_nosnap` / `bwalkrem_exit_interp` — branch chosen.
  - `lvtp_enter` in `src/game/lv.c::lvTickPlayer` — confirms whether the player gets ticked at all on the server, with which `var80075d64`/`var80075d68` state.

### E. State-drift / JIP healing

`SVC_SCORE` and `SVC_PLAYER_STATS` are emitted only on change; a single dropped packet or a JIP client lands with permanently wrong local state.

- **`port/src/net/net.c::netEndFrame`** — added 1 Hz heartbeats for both messages, phase-offset within `NET_HEARTBEAT_INTERVAL` (KoH at `0`, score at `1/4`, lobby at `1/2`, stats at `3/4`) so bandwidth spreads instead of stacking on one frame.

### F. Operational polish

- **`port/src/headless.c::headlessInstallSignalHandlers` + `main.c`** — Windows `SetConsoleCtrlHandler` (CTRL_C/BREAK/CLOSE/LOGOFF/SHUTDOWN) and POSIX `SIGINT/SIGTERM/SIGHUP` → `_exit(0)`. Without this, clicking X on the cmd window orphans the headless process. Uses `_exit` (not `exit`) to bypass `atexit` cleanup → ENet teardown can block on unresponsive peers; OS reclaims UDP socket immediately.
- **`port/src/main.c::cleanup`** — skip `inputSaveBinds` + `configSave` when `g_NetDedicatedMode == 1` (binds were never loaded; saving would wipe shared pd.ini's keybinds with empty strings).
- **`port/src/net/playlist.c::playlistLoad`** — search `$S` / `$E` / `.` / `$B` for bare-name playlist files; honour explicit paths through `fsFullPath`. Default path changed from `server_playlist.ini` to `$S/server_playlist.ini`.
- **`port/include/net/playlist.h` + `playlist.c` + `net.c`** — `min_humans_to_start` (default 1). Dedicated auto-start arms a 1s grace when humans ≥ threshold, cancels if they drop, re-arms on CITRAINING re-entry. Post-vote: if humans dropped below threshold, return to CITRAINING lobby instead of `mpStartMatch`.
- **`port/src/input.c::inputMouseGetPosition`** — div-by-zero guard for `videoGetWidth() == 0` / `Height() == 0` (gfx_init skipped in headless; menu input handlers still poll mouse).
- **`port/src/video.c`** — `wmAPI` null-checks on `videoUpdateNativeResolution`, `videoCreateFramebuffer`, `videoSetFramebuffer`, `videoResetFramebuffer`, `videoResizeFramebuffer`, `videoCopyFramebuffer`, `videoResetTextureCache`, `videoFreeCachedTexture`.
- **`port/src/audio.c` + `port/src/video.c`** — forward-decl `g_NetDedicatedMode` instead of `#include "net/net.h"` (avoids `types.h` redefining `bool` over `<stdbool.h>` already included transitively).
- **`port/src/net/netmenu.c`** — `DEDLINE` macro drops `MENUITEMFLAG_LITERAL_TEXT` (cosmetic).

---

## Hypotheses ruled out

> _TODO — record dead ends with one-liners so we don't re-attempt them. Format: "What was tried → what proved it wasn't the cause → which probe / log line showed it."_

---

## Open hypotheses / next experiments

> _TODO — ordered by cost-to-test. Each entry: "Hypothesis → how we'd confirm → which probe to add if not already there."_

---

## Live probes (remove before commit)

| Probe key | File | Branch / meaning |
|---|---|---|
| `bwalkrem_enter` | `src/game/bondwalk.c` | every `bwalkUpdateRemote` entry (1 Hz) — cl, pnum, isremote, ucmd, inmove tick/pos, prop pos, interp ticks |
| `bwalkrem_skip_force` | `src/game/bondwalk.c` | legitimate FORCEMASK skip (forcetick != 0) |
| `bwalkrem_exit_force` | `src/game/bondwalk.c` | force-correction branch chosen |
| `bwalkrem_exit_nosnap` | `src/game/bondwalk.c` | no usable interp snapshots |
| `bwalkrem_exit_interp` | `src/game/bondwalk.c` | normal interp branch — target, delta, prop pos out |
| `lvtp_enter` | `src/game/lv.c` | `lvTickPlayer` per-player entry — pnum, isremote, has_client, is_spec, var80075d64/68 |

Enable with `--netdiag <path>` (or whatever the diag-log flag is — confirm in `net.c`).

---

## Files touched (uncommitted)

```
port/include/headless.h       6 +++
port/include/net/playlist.h   5 ++
port/src/audio.c              5 +-
port/src/headless.c          60 +++++++++
port/src/input.c             14 +++-
port/src/main.c              15 +++-
port/src/net/net.c          117 +++++++++++++++++
port/src/net/netmenu.c       26 +++++--
port/src/net/playlist.c      59 ++++++++++-
port/src/pdmain.c            74 ++++++++++++++--
port/src/spectator.c          9 ++++
port/src/video.c             13 +++-
src/game/bondwalk.c          49 +++++++++-
src/game/camera.c            17 +++++
src/game/lv.c                15 +++++
src/game/player.c            11 +++
```

Untracked: `run-dedicated.bat`, `server_playlist.ini` (operator-side, not source).

---

## Conventions for this file

- Append below the relevant section, don't rewrite history; we want to see dead ends.
- When something is confirmed dead, move the entry from "Open hypotheses" → "Hypotheses ruled out" with the evidence line.
- When a fix is confirmed working, **don't** delete the entry from "Trail of attempts" — note the confirmation date next to it and let it stay until promotion to `PORT_NET_PREDICT_CHANGES.md` at commit time.
- Probes that prove their hypothesis get removed before the commit they belong to; the "Live probes" table reflects what's currently in the tree.
