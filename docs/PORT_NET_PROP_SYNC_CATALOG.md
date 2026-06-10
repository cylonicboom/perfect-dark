# Netplay Prop Sync & Garbage-Collection Catalog

> **Status: investigation log, 2026-06-08.** A working reference for which prop
> classes need active network sync + lifecycle (GC) management, what the current
> coverage is, and which known symptoms map to which gap. Confidence is marked
> per item: **[confirmed]** (read in code), **[hypothesis]** (mechanism inferred,
> needs runtime confirmation). Pairs with `port/src/net/CLAUDE.md` (the SVC_*/CLC_*
> protocol) and `docs/PORT_NET_KNOWN_ISSUES.md`.

---

## 1. The sync model (recap)

Every active prop gets a `syncid` (u16, 1-based) at stage start via
`netSyncIdsAllocate()`. The pool splits into:

- **Initial syncids** `1 .. g_NetFirstDynamicSyncId-1` — the stage's setup props,
  allocated **deterministically** on host and client (same `setup.c` walk, same
  order ⇒ same syncid on both machines). Never spawned over the wire.
- **Dynamic syncids** `g_NetFirstDynamicSyncId ..` — everything spawned **during
  play**: dropped guns, projectiles, thrown gadgets, deployed sentries. The host
  owns these and must replicate them: `SVC_PROP_SPAWN` (create) → `SVC_PROP_MOVE`
  (per-tick state) → `SVC_PROP_FREE` (destroy), with `SVC_PROP_RECONCILE` as a 2 Hz
  full-list backstop that reaps ghosts the FREE event missed.

`SVC_PROP_SPAWN` already serializes the hard cases **[confirmed]**
(`netmsgSvcPropSpawnWrite`, netmsg.c:2944): PROPTYPE_WEAPON (modelnum, weaponnum,
gunfunc, timer), PROPTYPE_OBJ incl. OBJTYPE_AUTOGUN (ammo, firecount, targetteam,
owner client id), and the full projectile block (`nextsteppos`, owner/target prop,
projectile flags, `mtx`, powered-rocket vectors). So spawn **coverage** is broad;
the failures below are in **pickup authority, client-side physics divergence, and
GC**, not missing spawn fields.

---

## 2. Category A — Deterministic stage props (NO runtime sync)

Created identically on every machine from stage data with matching syncids; the
host references them by syncid for state changes only. **GC = stage pool wipe at
`mainEndStage`** — no per-prop free needed. Do not spend sync effort here.

| OBJTYPE | Examples |
|---|---|
| DOOR / PADLOCKEDDOOR / LINKLIFTDOOR / LIFT | doors, lifts (state via `SVC_PROP_DOOR`/`SVC_PROP_LIFT`) |
| GLASS / TINTEDGLASS | windows (break state via damage) |
| SINGLE/MULTI/HANGINGMONITOR, CCTV, ALARM | scenery + cameras |
| AMMOCRATE / MULTIAMMOCRATE / SHIELD | spawnable pickups (consume → `SVC_PROP_*`) |
| BASIC / CONDITIONALSCENERY / BLOCKEDPATH / GASBOTTLE / SAFE | scenery / destructibles |
| TANK / TRUCK / HELI / HOVER* / FAN / CHOPPER / ESCASTEP | vehicles & set-pieces |
| AUTOGUN (map-placed) | fixed sentries (distinct from the **thrown** laptop, below) |

---

## 3. Category B — Runtime-dynamic props (NEED sync + GC)

These are spawned mid-match and are where every current bug lives. Table key:
**Sync** = is host→client replication present/correct; **GC** = does it free
cleanly on both sides.

| Prop class | Type | Spawned by | Sync | GC | Known issue |
|---|---|---|---|---|---|
| **Dropped weapon** (disarm / death drop) | WEAPON | `objDrop*`, disarm, `chrDropAllItems` | spawn ✅ / pos ✅ (§5.2 fix) | ⚠️ | ~~intangible (§5.1)~~ / ~~floats (§5.2)~~ — both FIXED |
| **Thrown grenade / N-Bomb** | WEAPON (projectile) | grenade throw | spawn ✅ / flight ✅ (§5.2 pass 1) | ⚠️ | ~~bounce/land divergence~~ FIXED |
| **Rocket / SK-rocket / Devastator / SuperDragon grenade** | WEAPON (projectile, some POWERED) | fire | spawn ✅ (powered vec) / flight ✅ (§5.2 pass 1) | ⚠️ | impact free |
| **Proximity / remote / timed mine** (incl. **Dragon** secondary) | WEAPON (projectile→stuck) | lay mine | spawn ✅ / stick ⚠️ | ❌ **screen-gated free** | ghost mines (§6) |
| **Laptop Gun sentry** (thrown) | OBJ / **AUTOGUN** + `OBJFLAG_THROWNLAPTOP` | `laptopDeploy` (bondgun.c:4690) | spawn ✅ / **AI/fire ❌** | ⚠️ | **doesn't work on client** (§5.3) |
| **Embedded mine/knife on a chr** | WEAPON, `OBJHFLAG_EMBEDDED` child of chr | stick to body | ⚠️ | ❌ **screen-gated** | the original ghost class (reconcile backstop) |
| **Ammo/shield from a consumed crate** | OBJ pickups | crate break | spawn ✅ | ✅ | ok |
| **Client-local sim hand-weapon** | WEAPON, **syncid 0** | client `chrGiveWeapon` (chr-state sync) | n/a (local) | ❌ **the cycle culprit** | free-without-delist → list cycle (§6) |

---

## 4. The physics-weapon lifecycle (where it breaks)

Mine / Dragon(2nd) / SuperDragon / Devastator / Grenade / N-Bomb / Laptop-sentry
all share the **projectile** path and a multi-stage lifecycle. Each transition is
a sync + GC checkpoint:

```
throw/fire ──► AIRBORNE (flight) ──► bounce/SLIDING ──► land/STICK ──► armed ──► detonate/pickup ──► FREE
   spawn          MOVE (mtx,pos)        MOVE             MOVE+flags     (none)      FREE / pickup
```

- **Flight** rides `OBJHFLAG_PROJECTILE` + `projectile->{flags,mtx,nextsteppos}`,
  serialized in SPAWN **[confirmed]**. But `objTickPlayer` runs the projectile
  **physics locally on the client too** (it is *not* server-gated like `botTick`)
  **[confirmed]**, while `SVC_PROP_MOVE` *also* drives the position — so the client
  integrates its own gravity/bounce AND receives wire positions. For Combat Sim
  WEAPON props there is **no wire-pos reconcile** (the `g_NetCoopObjWireDriven`
  snap is `PROPTYPE_OBJ` + co-op only) **[confirmed]**, so the two diverge →
  **float / wrong landing** **[hypothesis]**.
- **Stick/embed** (mines): the free that should happen when the host reaps the
  mine is **screen-gated** (`chr0f022214` on-screen vs `func0f0706f8` off-screen)
  → a headless host takes the off-screen branch and the FREE can be missed →
  **ghost mines** on clients (the reconcile backstop was added for exactly this).
- **Laptop sentry**: spawned as an AUTOGUN, but its **targeting/fire AI is
  server-only** (the autogun tick is in the bot/AI family); the client renders the
  sentry but never runs its fire logic, and `SVC_CHR_FIRE` only covers chr muzzle
  flashes, not autogun beams. Also `SVC_PROP_SPAWN` derefs
  `g_Vars.players[ownerplayernum]->client->id` (netmsg.c:2980) — fragile if the
  owner slot/`client` is wrong on a dedicated host **[hypothesis]**.

---

## 5. Symptom → cause map (the reported bugs)

### 5.1 Dropped weapons on the floor are **intangible** to clients — **FIXED**
> Resolved by the 2026-06-10 netprop consolidation (see
> `PORT_NET_PROP_LIFECYCLE.md` "Combat Sim client pickups"): `objTestForPickup`'s
> Combat-Sim client hard-bail was replaced — clients run the same read-only
> pickup checks, send `CLC_PICKUP_REQUEST`, and take ONLY via the host's
> `SVC_PROP_PICKUP` echo (no local take, so no double-give; the host's own
> proximity scan for remote pawns remains the second detection path). The
> original analysis below is kept for the record.

`objTestForPickup` (propobj.c:17808) early-returned `TICKOP_NONE` when
`g_NetMode == NETMODE_CLIENT && g_Vars.coopplayernum < 0`. The client pickup path
(`CLC_PICKUP_REQUEST` → host validates → `SVC_PROP_PICKUP`) was wired **for co-op
only**; Combat Sim clients could never collect a dropped/floor weapon. This was a
design gap, not corruption.

### 5.2 Weapons **floating above their spawn point** — **FIXED (send-side reconcile)**
> Resolved 2026-06-11: the server now re-broadcasts Combat Sim dynamic prop
> positions itself (net.c `netEndFrame`, the same two-pass design as the co-op
> movable-OBJ block directly above it): **pass 1** sends every synced,
> unparented, non-embedded WEAPON/OBJ in projectile motion every tick (clients
> track the full fall/bounce arc — also covers thrown grenade/N-Bomb/rocket
> flight divergence); **pass 2** round-robins a few settled ones per tick
> (heals a diverged rest position, a dropped impulse packet, or a JIP client).
> The read side already applied pos + rooms + the projectile block, so there
> is **no wire or protocol change**. This also masks cause (a) below — a
> client copy that never fullticks still follows the wire to the floor; the
> objTickPlayer gate cleanup stays with the projectile-sync WIP.

Original analysis: a dropped gun spawns at the drop height and should fall
(`OBJFLAG_FALL` → projectile fall physics). Two candidate causes: (a) the
client runs its own fall in `objTickPlayer` but the prop is
**backgrounded/not ticked** so it never falls, or (b) it falls locally but
diverges from the host because there was **no wire-pos reconcile for Combat
Sim WEAPON props** (§4). Both shapes are addressed by the host-owned position
stream; clients still integrate locally between updates for smoothness.

### 5.3 Throwing the **Laptop Gun sentry does not work** — **[hypothesis]**
Spawn is covered, but the sentry's **fire AI is server-only** and its beam isn't
in the chr-fire sync, so on the client it's an inert model; plus the owner-id
deref in the spawn writer is fragile on a dedicated host (§4). Likely the same
class affects any **deployed gadget that ticks its own AI** (autogun-type).

---

## 6. GC / lifecycle invariants & the failure modes we've hit

The crash family this session all traces to **one prop-list invariant being
violated**: *a prop that is freed must be unlinked from `g_Vars.activeprops`
(`propDelist`) before/while `propFree` recycles it, and a freed prop must never be
re-`propActivate`d while still referenced.* Violations seen:

1. **Free-without-delist** → null-obj corpse in the active list →
   `objTickPlayer`/`g_PausableObjs[obj->type]` AV. *Fixed:* `propsTickPlayer`
   **reaps** null-obj props (`TICKOP_FREE`) instead of skipping (aff12448c).
2. **Freed prop relinked into the list** → `->next` **cycle** → every unbounded
   walk (`roomsTickLighting`, `propsRenderBeams`) hangs. *Fixed:*
   `propsHealActiveList()` per frame (1f09d7e0c) — breaks the cycle, **logs the
   culprit** `propsheal: back-edge prop N type T syncid S`. The recurring culprit
   is the **syncid-0 client-local sim hand-weapon** (the chr-state weapon-swap
   path) — the prime root-cause suspect.
3. **Screen-gated frees on a headless host** → mine/embedded-prop FREE missed →
   **ghosts**. *Mitigated:* `SVC_PROP_RECONCILE` 2 Hz full weapon/obj (+co-op chr)
   syncid list; client reaps anything the host no longer lists.
4. **syncid recycle race** → a stale unreliable chr-state move lands on a syncid
   the host reused for a weapon/obj → type-confusion write. *Mitigated:* read-side
   `prop->type` gate + room sanitization.
5. **Memory-pool exhaustion** (not a list bug): the stage pool fills (HD mod
   textures) so dynamic props can't allocate → `syncid N does not exist` flood →
   NULL deref. *Mitigated by config:* `Game.MemorySize` (16→256 MB).

**GC checklist for any NEW dynamic prop type:**
- [ ] Host frees it through `objFree`/`propExecuteTickOperation` (delist→disable→free), never a bare `propFree` on a listed prop.
- [ ] The free path is **not screen-gated** on a dedicated host (or is covered by reconcile).
- [ ] It carries a non-zero `syncid` if it must be addressed over the wire (client-local props use 0 and must never be wire-referenced).
- [ ] It is listed in `SVC_PROP_RECONCILE`'s scope so a missed FREE self-heals.
- [ ] It is never `propActivate`d twice / re-activated after free.

---

## 7. Open work (priority order)

1. **Combat Sim client pickups** (§5.1) — clear design gap, self-contained fix.
2. **Root-cause the syncid-0 hand-weapon relink** (§6.2) — the `propsheal` log now
   names it; one repro's `pd.log` should pinpoint the free/activate site.
3. **WEAPON-prop position authority** (§5.2) — decide host-owned vs client-fall +
   snap; covers floating guns and projectile-flight divergence together.
4. **Deployed-gadget AI sync** (§5.3) — autogun/sentry fire replication (likely a
   new SVC for autogun beam state, or generalize `SVC_CHR_FIRE`).
5. **Generalize the headless free paths** to run the on-screen branch (the
   "force on-screen prop logic on the dedicated server" idea) so the stream
   matches a P2P host and §6.3-class ghosts stop at the source.
