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

PROGRESSION_PREFIXES = ("Stage: ", "Difficulty: ", "Device: ")

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

_LOC_BASE = BASE_ID + 0x1000
location_name_to_id = {}
_n = 0
for _s in STAGE_NAMES:
    for _d in ALL_DIFFICULTIES:
        location_name_to_id[f"{_s} ({_d})"] = _LOC_BASE + _n; _n += 1
