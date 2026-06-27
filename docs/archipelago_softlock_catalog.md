# Archipelago — Soft-lock Catalog (solo campaign)

**Purpose.** Under AP `ap_mode`, content is **locked by default** and only usable once its item
arrives (`apGateIsUnlocked` is a set-membership test — see `port/.../luaai_api.c`). Weapons are a
*legitimate progression gate* (decided 2026-06-27): the sniper being inoperable until you receive it
is correct AP behaviour. The consequence is that **any gated item that gates an objective must be an
AP item**, and the apworld's **logic rules** must require it on the matching location — otherwise a
seed can place a progression item behind a mission you cannot finish.

This catalog is the source of truth for those logic rules: for every stage × difficulty it lists the
objectives that are active and the items their completion needs.

## How to read it

- **Difficulties**: A = Agent, SA = Special Agent, PA = Perfect Agent. (The engine's 4th "Perfect
  Dark" difficulty `DIFFBIT_PD` is **not** an AP difficulty and is ignored.) Harder difficulties add
  objectives, so requirements are cumulative-ish but not strictly nested.
- **Confidence**: `[decomp]` = the mission script *enforces* the item (an `if_chr_weapon_equipped` /
  `if_weapon_thrown_on_object` / `require_object_*` / `if_player_using_device` check — a hard gate).
  `[domain]` = required to play it but not script-enforced (e.g. "kill the bodyguards" needs *a*
  weapon). `[user]` = confirmed in-game this session.
- **AP item vs level item**: gadgets/weapons keyed by `weaponnum` are AP-gated (fire/device gate).
  **Keycards, disguises, briefcases, the necklace, flight plans** are in-level `OBJ_*` quest items
  picked up during play — they are **not** weapon/device-gated, so they are **not** AP items and need
  no logic rule (you just have to reach them, which the Stage item already covers).

## Item → AP category (the gating mechanism that applies)

| Item | `WEAPON_*` | Gate that blocks it | AP category | Verified? |
|---|---|---|---|---|
| Night Vision | NIGHTVISION (45) | device gate (`currentPlayerSetDeviceActive`) | DEVICE | ✓ (shipped item) |
| IR Scanner | (48) | device gate | DEVICE | ✓ |
| X-Ray Scanner | XRAYSCANNER (47) | device gate | DEVICE | ✓ |
| Cloaking Device | (49) | device gate | DEVICE | ✓ |
| Eye Spy / CamSpy | EYESPY | **deploy = fire?** | WEAPON_? or DEVICE? | **NEEDS VERIFY** |
| Door Decoder | DOORDECODER | **use = equip+proximity, may NOT fire** | WEAPON_? or none | **NEEDS VERIFY** |
| Data Uplink | DATAUPLINK | **use at terminal = fire?** | WEAPON_? | **NEEDS VERIFY** |
| ECM Mine | ECMMINE | throw = fire (ATTACK) | WEAPON_PRI | likely |
| Remote Mine | REMOTEMINE | throw = fire | WEAPON_PRI | likely |
| Tracer Bug | TRACERBUG | throw = fire | WEAPON_PRI | likely |
| Comms Rider | COMMSRIDER | throw = fire | WEAPON_PRI | likely |
| AutoSurgeon | AUTOSURGEON | use on hoverbed = fire? | WEAPON_? | **NEEDS VERIFY** |
| Sniper Rifle | SNIPERRIFLE | fire | WEAPON_PRI | ✓ [user] |
| Any combat gun | (many) | fire | WEAPON_PRI | ✓ |

> **The #1 open question** (blocks correct logic): do the *gadget-use* actions (Eye Spy deploy, Door
> Decoder crack, Data Uplink terminal, AutoSurgeon revive) actually route through `bgunSetState`
> ATTACK (and so the AP weapon-fire gate), or through a different code path that AP doesn't gate yet?
> If they are **not** currently gated, those missions are *not* soft-lock risks today — but they also
> won't behave as progression gates. Each must be checked against `bgunSetState` / the device gate.
> Until then, treat them as **required** in logic (conservative — never under-gates).

---

## Per-stage objective tables

### 0 — Defection (`setupame.c`)
| Obj | Text | Diffs | Item (confidence) |
|---|---|---|---|
| 4 | Gain entrance to laboratory | A | — traversal only |
| 0 | Disable internal security hub | SA PA | **ECM Mine** `if_weapon_thrown_on_object` [decomp] |
| 1 | Obtain keycode necklace | SA PA | necklace = level item |
| 3 | Disable external comms hub | SA PA | **ECM Mine** [decomp] |
| 5 | Gain entrance to laboratory | SA PA | — traversal |
| 2 | Download project files | PA | **Data Uplink** `if_chr_weapon_equipped` [decomp] |

- **A** → (nothing AP-gated; pure stealth/traversal — this is why it's winnable now)
- **SA** → ECM Mine
- **PA** → ECM Mine, Data Uplink

### 1 — Investigation (`setupear.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 0 | Holograph radioactive isotope | A SA PA | **Eye Spy** (only holograph device) [decomp] |
| 4 | Locate Dr. Caroll | A SA PA | — |
| 1 | Start security maintenance cycle | SA PA | — terminal |
| 2 | Shut down experiments | SA PA | **Data Uplink** ×3 `if_chr_weapon_equipped` [decomp] |
| 3 | Obtain experimental technologies | PA | collect K7 Avenger + Night Vision + Shield Tech (level items; getting the K7 needs *a weapon* to drop the guard [domain]) |

- **A** → Eye Spy
- **SA** → Eye Spy, Data Uplink
- **PA** → Eye Spy, Data Uplink (+ a combat weapon for the K7 carrier)

### 2 — Extraction (`setupark.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 0 | Access foyer elevator | A SA PA | — positional |
| 3 | Defeat Cassandra's bodyguards | A SA PA | **any combat weapon** [domain] |
| 4 | Rendezvous at helipad | A SA PA | — positional |
| 2 | Destroy dataDyne hovercopter | SA PA | **explosive** (Rocket/Grenade/Dragon — gunfire won't kill it) [domain] |
| 1 | Reactivate office elevator | PA | — activate |

- **A** → any combat weapon
- **SA/PA** → combat weapon + an explosive

### 3 — Villa (`setupeld.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 0 | Save the negotiator | A SA | **Sniper Rifle** (snipe the rooftop assassins threatening him) [user][domain] |
| 2 | Activate wind generator | A SA PA | — (needs cooling+power first) |
| 5 | Rescue Carrington | A SA PA | — |
| 1 | Eliminate rooftop snipers | SA PA | **Sniper Rifle** / ranged [domain] |
| 3 | Locate & eliminate hackers | PA | a combat weapon [domain] |
| 4 | Capture dataDyne guard | PA | a **KO** method (must not kill) [domain] |

- **A** → Sniper Rifle
- **SA** → Sniper Rifle
- **PA** → Sniper Rifle, combat weapon, KO method

### 4 — Chicago (`setuppete.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 0 | Retrieve drop point equipment | A SA PA | collect briefcases (level items) |
| 3 | Create vehicular diversion | A SA PA | **Data Uplink** (reprogram taxi) **OR** kill 4 sealer guards [decomp — explicit OR] |
| 4 | Gain entry to G5 building | A SA PA | — positional |
| 2 | Prepare escape route | SA PA | **Remote Mine** `if_weapon_thrown` on fire doors [decomp] |
| 1 | Attach tracer to limousine | PA | **Tracer Bug** `if_weapon_thrown_on_object` [decomp] |

- **A** → (Data Uplink **OR** combat weapon)
- **SA** → Remote Mine + (Data Uplink OR combat weapon)
- **PA** → Tracer Bug + Remote Mine + (Data Uplink OR combat weapon)

### 5 — G5 Building (`setupdepo.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 2 | Holograph meeting conspirators | A SA PA | **Eye Spy / CamSpy** `if_chr_in_room` camspy [decomp] |
| 3 | Retrieve Dr. Caroll backup from safe | A SA PA | **Door Decoder** `if_chr_weapon_equipped` [decomp] |
| 4 | Exit building | A SA PA | — positional |
| 1 | Deactivate laser grid systems | SA PA | — switch (Agent pre-disabled) |
| 0 | Disable damping field generator | PA | — interact |

- **A** → Eye Spy, Door Decoder
- **SA** → Eye Spy, Door Decoder
- **PA** → Eye Spy, Door Decoder

### 6 — Infiltration (`setuplue.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 0 | Shut down air intercept radar | A SA PA | **a weapon** (destroy terminal) [domain] |
| 3 | Gain access to hangar lift | A SA PA | Keycard4B = level item (`if_chr_has_object`) [decomp, not AP] |
| 4 | Make contact with CI spy | A SA PA | — room entry |
| 1 | Plant comms device on antenna | SA PA | **Comms Rider** `if_weapon_thrown_on_object` [decomp] |
| 2 | Disable all robot interceptors | PA | **a weapon** (destroy) [domain] |

- **A** → combat weapon
- **SA** → combat weapon + Comms Rider
- **PA** → combat weapon + Comms Rider

### 7 — Rescue (`setuplip.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 2 | Obtain & use lab technician disguise | A SA PA | disguise = level item [decomp, not AP] |
| 3 | Gain access to autopsy lab | A SA PA | Keycard = level item (`require_object_collected`) [decomp, not AP] |
| 4 | Rescue the crash survivor | A SA PA | — escort |
| 1 | Locate conspiracy evidence | SA PA | **X-Ray Scanner** `if_player_using_device` [decomp] |
| 0 | Destroy computer records | PA | **a weapon** [domain] |

- Door/light/autogun control terminals gate on **Data Uplink** (`if_chr_weapon_equipped`) — *may* be
  needed to open the path to the lab; **VERIFY** whether they're on the obj-3 critical path.
- **A** → (none AP-gated if disguise+keycard suffice; **verify Data Uplink for door control**)
- **SA** → X-Ray Scanner (+ Data Uplink?)
- **PA** → X-Ray Scanner + combat weapon (+ Data Uplink?)

### 8 — Escape (`setuptra.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 1 | Rendezvous with CI spy | A SA PA | — auto |
| 2 | Locate secret hangar | A SA PA | — room entry |
| 4 | Escape from Area 51 | A SA PA | — reach hoverbike; don't destroy terminals |
| 3 | Revive Maian bodyguard | SA PA | **AutoSurgeon** `if_chr_weapon_equipped` on hoverbed [decomp] |
| 0 | Locate alien tech medpack | PA | **AutoSurgeon** (pick up) [decomp] |

- **A** → (nothing AP-gated — winnable bare)
- **SA** → AutoSurgeon
- **PA** → AutoSurgeon

### 9 — Air Base (`setupcave.c`)
| Obj | Text | Diffs | Item |
|---|---|---|---|
| 0 | Obtain disguise and enter base | A SA PA | disguise41 = level item [decomp, not AP] |
| 2 | Subvert security monitoring system | A SA PA | — console |
| 4 | Board Air Force One | A SA PA | — positional |
| 1 | Check in equipment | SA PA | Suitcase = level item, equip+deposit (`if_chr_weapon_equipped`) [decomp — equip, prob not fire-gated; VERIFY] |
| 3 | Obtain flight plans from safe | PA | — safe switches |

- **A** → (nothing AP-gated)
- **SA** → (Suitcase equip — verify if gated)
- **PA** → (Suitcase equip — verify)

---

## Consolidated logic matrix (→ apworld `set_rules`)

Each cell is the item set a location `"<Stage> (<Diff>)"` must require. `WPN` = "any combat weapon"
(an AP item-group rule), `EXPL` = "any explosive". Bracketed = verify-gating before trusting.

| Stage | Agent | Special Agent | Perfect Agent |
|---|---|---|---|
| Defection | — | ECM Mine | ECM Mine, Data Uplink |
| Investigation | [Eye Spy] | [Eye Spy], [Data Uplink] | [Eye Spy], [Data Uplink], WPN |
| Extraction | WPN | WPN, EXPL | WPN, EXPL |
| Villa | Sniper | Sniper | Sniper, WPN, KO-method |
| Chicago | ([Data Uplink] ∨ WPN) | Remote Mine, ([Data Uplink]∨WPN) | Tracer Bug, Remote Mine, ([Data Uplink]∨WPN) |
| G5 Building | [Eye Spy], [Door Decoder] | [Eye Spy], [Door Decoder] | [Eye Spy], [Door Decoder] |
| Infiltration | WPN | WPN, Comms Rider | WPN, Comms Rider |
| Rescue | [Data Uplink?] | X-Ray, [Data Uplink?] | X-Ray, WPN, [Data Uplink?] |
| Escape | — | [AutoSurgeon] | [AutoSurgeon] |
| Air Base | — | [Suitcase?] | [Suitcase?] |

**Always-winnable bare (no AP item):** Defection (A), Escape (A), Air Base (A). These are the safe
"sphere expanders" alongside the precollected `Stage: Defection`.

---

## Implementation plan (apworld + client)

1. **Verify the gadget gates** (the `[...]` rows). For Eye Spy / Door Decoder / Data Uplink /
   AutoSurgeon / Suitcase, confirm whether their use routes through the AP weapon-fire gate
   (`bgunSetState` ATTACK) or the device gate, or is currently **ungated**. Update categories above.
2. **Add all weapons as AP items** in `data.py` (`Weapon: <name>` → `weaponnum`), plus the gadget
   names that turn out to be weapon-gated. Mirror the name→`weaponnum` table into
   `scripts/ap/client.lua` (`WEAPON_NAME_TO_NUM`, both PRI and SEC categories as needed).
3. **Starting loadout grant** — fixes the "everything locked, can't fight at all" state. Precollect a
   baseline (Falcon 2) so `WPN` is satisfiable from the start, OR add it to the item pool with a
   guaranteed-early rule. Without this, no combat mission is reachable in sphere 0.
4. **Item groups** — define `WPN` (all combat guns) and `EXPL` (Rocket/Grenade/Dragon/mines) as AP
   item groups so the OR/any-of rules above are expressible (`state.has_group` / `has_any`).
5. **Write `set_rules`** from the matrix: per `"<Stage> (<Diff>)"` location, `add_rule(... state.has(...))`.
6. **Re-verify generation** (`ArchipelagoGenerate.exe`) and play a real seed.

## Provenance

Objective tables extracted from `src/setups/setup*.c` (`beginobjective` + script triggers), 10
parallel readers, 2026-06-27. `[domain]`/`[user]` rows are gameplay knowledge — verify in-game
before shipping a seed. Stage→file map and gate mechanics: see memory `ap-softlock-catalog`.
