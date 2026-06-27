"""Canonical Perfect Dark item/location names + ids.

Dependency-free on purpose (no Archipelago imports), so the mock server and the
datapackage generator can share the exact same id tables the apworld registers.
Names here are the CONTRACT with scripts/ap/client.lua (which maps names ->
engine gates) -- keep all three in sync. Tables are append-only: never reorder
or the ids shift and old seeds break.
"""

GAME = "Perfect Dark"
BASE_ID = 0x50_0000  # arbitrary unique base for this (unofficial) world

STAGE_NAMES = [
    "Defection", "Investigation", "Extraction", "Villa", "Chicago",
    "G5 Building", "Infiltration", "Rescue", "Escape", "Air Base",
]
# Agent is the always-available starting difficulty (not an item).
DIFFICULTY_ITEMS = ["Special Agent", "Perfect Agent"]
ALL_DIFFICULTIES = ["Agent"] + DIFFICULTY_ITEMS
DEVICE_NAMES = ["Night Vision", "IR Scanner", "X-Ray Scanner", "Cloaking Device"]
FILLER_NAMES = ["Body Armor", "Combat Boost", "Extra Ammo"]

# ---- Weapons / gadgets (AP weapon-fire gate, keyed by engine `weaponnum`) ----
# Under ap_mode a weapon can't fire until its item arrives (bgunSetState ATTACK
# checks AP_CAT_WEAPON_PRI/SEC[weaponnum]). The five gadgets (Eye Spy / Door
# Decoder / Data Uplink / AutoSurgeon / Suitcase) deploy through the SAME fire
# gate (user-confirmed 2026-06-27), so they are weapon items too -- NOT devices
# (devices are the four passive scanners above, on the separate device gate).
#
# MVP set: just the objective-gating items the soft-lock catalog references plus
# small @WPN/@EXPL representative pools, sized to fit the 30 mission-complete
# locations. (display name, engine weaponnum). Decimal weaponnums from
# `enum weaponnum` in src/include/constants.h. The display names are the
# CONTRACT with scripts/ap/client.lua's WEAPON_NAME_TO_NUM -- keep in sync.
WEAPON_ITEMS = [
    # @WPN group (any combat weapon)
    ("Falcon 2", 2),
    ("MagSec 4", 5),
    ("AR34", 17),
    ("Shotgun", 19),
    # @EXPL group (any explosive)
    ("Rocket Launcher", 24),
    ("Grenade", 30),
    # Objective-gating weapons / gadgets
    ("Sniper Rifle", 21),
    ("ECM Mine", 53),
    ("Data Uplink", 54),
    ("Eye Spy", 46),
    ("Door Decoder", 57),
    ("Remote Mine", 34),
    ("Tracer Bug", 62),
    ("Comms Rider", 61),
    ("AutoSurgeon", 58),
    ("Suitcase", 77),
]
WEAPON_NAME_TO_NUM = {name: num for name, num in WEAPON_ITEMS}

# Item-group members (display names) for the "@WPN" / "@EXPL" any-of logic rules.
WPN_GROUP_NAMES = ["Falcon 2", "MagSec 4", "AR34", "Shotgun"]
EXPL_GROUP_NAMES = ["Rocket Launcher", "Grenade"]

# Weapons that physically appear in the Carrington firing range -> each gets a
# "Firing Range: <name>" location (reported via the weaponfound event), reachable
# once you hold the weapon. Mission gadgets (Data Uplink / Suitcase / Eye Spy /
# ...) are NOT range weapons, so they get no range location. These extra
# locations also give generation the filler slack a mission-only pool lacks
# (a fully packed all-progression pool can't satisfy `accessibility: full`).
RANGE_WEAPONS = ["Falcon 2", "MagSec 4", "AR34", "Shotgun",
                 "Rocket Launcher", "Grenade", "Sniper Rifle", "Remote Mine"]

# Combat Simulator challenges. Available from game start with no item gate, so
# they are requirement-free ("sphere 0") locations -- both honest checks and the
# lightly-gated early homes the fill needs to seed the weapon-gated missions
# (otherwise an almost-entirely weapon-gated pool can't satisfy full accessibility).
# Reported via the challengecomplete event (engine challenge index -> 1-based).
CHALLENGE_COUNT = 10
CHALLENGE_NAMES = [f"Challenge {i}" for i in range(1, CHALLENGE_COUNT + 1)]

# Item granted at the start so sphere 0 isn't unarmed (satisfies @WPN). Mirrors
# the Defection stage precollect; both are pushed in PerfectDarkWorld.create_items.
STARTING_WEAPON = "Falcon 2"

# Items the player already has at gate start (not placed in the pool).
PRECOLLECTED = ["Stage: Defection", f"Weapon: {STARTING_WEAPON}"]

PROGRESSION_PREFIXES = ("Stage: ", "Difficulty: ", "Device: ", "Weapon: ")

# ---- Per-(stage, difficulty) completion logic --------------------------------
# Feeds apworld set_rules. A requirement token is one of:
#   "Weapon: X" / "Device: X"  -> state.has(that item)
#   "@WPN" / "@EXPL"           -> state.has_group(that item group)
#   ["a", "b", ...]            -> OR: any one of the alternatives
# A cell is the AND of its tokens. The Stage item (always) and the Difficulty
# item (SA/PA) are added automatically by __init__ and are NOT listed here.
# Source: docs/archipelago_softlock_catalog.md consolidated matrix.
LOGIC = {
    "Defection": {
        "Agent": [],
        "Special Agent": ["Weapon: ECM Mine"],
        "Perfect Agent": ["Weapon: ECM Mine", "Weapon: Data Uplink"],
    },
    "Investigation": {
        "Agent": ["Weapon: Eye Spy"],
        "Special Agent": ["Weapon: Eye Spy", "Weapon: Data Uplink"],
        "Perfect Agent": ["Weapon: Eye Spy", "Weapon: Data Uplink", "@WPN"],
    },
    "Extraction": {
        "Agent": ["@WPN"],
        "Special Agent": ["@WPN", "@EXPL"],
        "Perfect Agent": ["@WPN", "@EXPL"],
    },
    "Villa": {
        "Agent": ["Weapon: Sniper Rifle"],
        "Special Agent": ["Weapon: Sniper Rifle"],
        "Perfect Agent": ["Weapon: Sniper Rifle", "@WPN"],
    },
    "Chicago": {
        "Agent": [["Weapon: Data Uplink", "@WPN"]],
        "Special Agent": ["Weapon: Remote Mine", ["Weapon: Data Uplink", "@WPN"]],
        "Perfect Agent": ["Weapon: Tracer Bug", "Weapon: Remote Mine",
                          ["Weapon: Data Uplink", "@WPN"]],
    },
    "G5 Building": {
        "Agent": ["Weapon: Eye Spy", "Weapon: Door Decoder"],
        "Special Agent": ["Weapon: Eye Spy", "Weapon: Door Decoder"],
        "Perfect Agent": ["Weapon: Eye Spy", "Weapon: Door Decoder"],
    },
    "Infiltration": {
        "Agent": ["@WPN"],
        "Special Agent": ["@WPN", "Weapon: Comms Rider"],
        "Perfect Agent": ["@WPN", "Weapon: Comms Rider"],
    },
    "Rescue": {
        "Agent": [],
        "Special Agent": ["Device: X-Ray Scanner"],
        "Perfect Agent": ["Device: X-Ray Scanner", "@WPN"],
    },
    "Escape": {
        "Agent": [],
        "Special Agent": ["Weapon: AutoSurgeon"],
        "Perfect Agent": ["Weapon: AutoSurgeon"],
    },
    "Air Base": {
        "Agent": [],
        "Special Agent": ["Weapon: Suitcase"],
        "Perfect Agent": ["Weapon: Suitcase"],
    },
}

item_name_to_id = {}
_n = 0
for _s in STAGE_NAMES:
    item_name_to_id[f"Stage: {_s}"] = BASE_ID + _n; _n += 1
for _d in DIFFICULTY_ITEMS:
    item_name_to_id[f"Difficulty: {_d}"] = BASE_ID + _n; _n += 1
for _dev in DEVICE_NAMES:
    item_name_to_id[f"Device: {_dev}"] = BASE_ID + _n; _n += 1
for _f in FILLER_NAMES:
    item_name_to_id[_f] = BASE_ID + _n; _n += 1
# Weapons appended last so the ids above stay stable (append-only).
for _w, _num in WEAPON_ITEMS:
    item_name_to_id[f"Weapon: {_w}"] = BASE_ID + _n; _n += 1

_LOC_BASE = BASE_ID + 0x1000
location_name_to_id = {}
_n = 0
for _s in STAGE_NAMES:
    for _d in ALL_DIFFICULTIES:
        location_name_to_id[f"{_s} ({_d})"] = _LOC_BASE + _n; _n += 1
# Firing-range checks appended after the mission checks (append-only ids).
for _w in RANGE_WEAPONS:
    location_name_to_id[f"Firing Range: {_w}"] = _LOC_BASE + _n; _n += 1
# Combat Simulator challenge checks (requirement-free).
for _c in CHALLENGE_NAMES:
    location_name_to_id[_c] = _LOC_BASE + _n; _n += 1
