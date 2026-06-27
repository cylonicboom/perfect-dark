# Perfect Dark — Archipelago Mission Gate Table

**Status:** DRAFT for review (2026-06-27). These are the proposed per-mission item
requirements; the `[domain]` rows are gameplay knowledge, not script-enforced, and need
group review before they're trusted. This table is the human-readable companion to the
machine source of truth, `data.LOGIC` in `tools/ap/apworld/perfect_dark/data.py`.

**What this is:** for every mission × difficulty, the items a player *must have* to complete
it under AP `ap_mode` (where content is locked until its item arrives). These become the
apworld's logic rules — get them right and a seed can never place progression behind a
mission you can't finish; get them wrong in the "too loose" direction and you get a
soft-lock. See also `docs/archipelago_softlock_catalog.md` (the objective tables this is
distilled from).

## Legend

| Symbol | Meaning |
|---|---|
| **—** | Completable with **no items** (pure stealth/traversal). Always reachable. |
| **@WPN** | *Any* combat weapon (item group — having any one satisfies it). |
| **@EXPL** | *Any* explosive (Rocket Launcher / Grenade / Dragon / Devastator…). |
| **A ∨ B** | Either one satisfies it (OR). |
| *italic* | A non-AP **level item** (keycard, disguise, briefcase, necklace) — picked up in-mission, *not* gated, listed only for context. |

**Confidence:** `[decomp]` = the mission script hard-enforces it (`if_chr_weapon_equipped`
etc.) · `[domain]` = needed to play but not script-checked (gameplay knowledge) · `[user]`
= confirmed in-game this project.

**Baseline always available (never gated):** melee/fists, and the starting **Falcon 2** (all
variants — normal/silenced/scoped share one unlock). So "—" missions are never a dead end
and you can always at least punch.

## Master table

| # | Stage | Diff | Required AP items | Driving objective(s) | Conf. |
|---|---|---|---|---|---|
| 0 | **Defection** | A | — | Gain entrance to laboratory (stealth) | [user] |
| | | SA | ECM Mine | Disable internal + external comms hubs (ECM thrown) · *keycode necklace* | [decomp] |
| | | PA | ECM Mine, Data Uplink | + Download project files (uplink at terminal) | [decomp] |
| 1 | **Investigation** | A | Eye Spy | Holograph radioactive isotope (only holograph device) | [decomp] |
| | | SA | Eye Spy, Data Uplink | + Shut down experiments ×3 (uplink) | [decomp] |
| | | PA | Eye Spy, Data Uplink, @WPN | + Obtain experimental tech (drop the K7 carrier) | [decomp]/[domain] |
| 2 | **Extraction** | A | @WPN | Defeat Cassandra's bodyguards | [domain] |
| | | SA | @WPN, @EXPL | + Destroy dataDyne hovercopter (gunfire won't kill it) | [domain] |
| | | PA | @WPN, @EXPL | (+ reactivate office elevator — no item) | [domain] |
| 3 | **Villa** | A | Sniper Rifle | Save the negotiator (snipe rooftop assassins) | [user]/[domain] |
| | | SA | Sniper Rifle | + Eliminate rooftop snipers | [domain] |
| | | PA | Sniper Rifle, @WPN | + Eliminate hackers · capture guard (KO) | [domain] |
| 4 | **Chicago** | A | (Data Uplink ∨ @WPN) | Vehicular diversion: reprogram taxi **or** kill 4 sealer guards | [decomp] |
| | | SA | Remote Mine, (Data Uplink ∨ @WPN) | + Prepare escape route (mine the fire doors) | [decomp] |
| | | PA | Tracer Bug, Remote Mine, (Data Uplink ∨ @WPN) | + Attach tracer to limousine | [decomp] |
| 5 | **G5 Building** | A | Eye Spy, Door Decoder | Holograph conspirators (camspy) · open Caroll's safe (decoder) | [decomp] |
| | | SA | Eye Spy, Door Decoder | (+ deactivate laser grid — switch) | [decomp] |
| | | PA | Eye Spy, Door Decoder | (+ disable damping field — interact) | [decomp] |
| 6 | **Infiltration** | A | @WPN | Shut down air-intercept radar (destroy terminal) · *Keycard 4B* | [domain] |
| | | SA | @WPN, Comms Rider | + Plant comms device on antenna (thrown) | [decomp] |
| | | PA | @WPN, Comms Rider | + Disable robot interceptors (destroy) | [decomp]/[domain] |
| 7 | **Rescue** | A | — *(verify Data Uplink for door control?)* | *Disguise* + *keycard* reach the lab | [decomp] level items |
| | | SA | X-Ray Scanner | + Locate conspiracy evidence (use device) | [decomp] |
| | | PA | X-Ray Scanner, @WPN | + Destroy computer records | [decomp]/[domain] |
| 8 | **Escape** | A | — | Reach the hoverbike (don't destroy terminals) | [domain] |
| | | SA | AutoSurgeon | Revive Maian bodyguard on hoverbed | [decomp] |
| | | PA | AutoSurgeon | + Locate alien-tech medpack | [decomp] |
| 9 | **Air Base** | A | — | *Disguise* to enter base | [decomp] level item |
| | | SA | Suitcase | Check in equipment (equip + deposit) | [user] |
| | | PA | Suitcase | (+ flight plans from safe — switches, no item) | [user] |

## Open questions to settle in review

1. **Rescue (Agent)** — are the door/light/autogun control terminals (which use **Data
   Uplink**) on the critical path to the autopsy lab, or can you reach it with just the
   *disguise* + *keycard*? If uplink is needed, Rescue (A) gains `Data Uplink`.
2. **@WPN vs specific guns** — "needs a weapon" is modelled as *any* combat weapon. If a
   mission genuinely needs a *specific* class (silenced gun for stealth Villa, long-range
   gun beyond the Sniper, …), name it and it becomes its own required item.
3. **KO requirement** (Villa PA "capture guard") — currently folded into @WPN. If it should
   require a true non-lethal tool (Tranquilizer / unarmed), add a `KO-method` token.
4. **Explosive reach** — does the Extraction hovercopter need a *direct* launcher
   (Rocket/Grenade), or do thrown mines count? Decides whether `@EXPL` includes mines.
5. **Scope** — this table is the soft-lock-critical subset. Making *every* weapon a findable
   item (the full ~50-gun pool) doesn't change this logic; it only adds items/locations.

## How corrections flow back into the build

Hand over the corrected cells (or redline this doc), then:
1. Edit `data.LOGIC` to match (token vocabulary: `"Weapon: X"`, `"Device: X"`, `"@WPN"`,
   `"@EXPL"`, `["a","b"]` for OR, `[]` for none).
2. Add any new required item to `WEAPON_ITEMS` (data.py) + `WEAPON_NAME_TO_NUM` (client.lua).
3. Rebuild the `.apworld` zip, redeploy, and re-run `ArchipelagoGenerate.exe` to confirm it
   still generates with `accessibility: full`.
