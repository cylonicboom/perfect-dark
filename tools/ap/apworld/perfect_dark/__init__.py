"""Perfect Dark — minimal Archipelago world definition (P2 vertical slice).

This is the SERVER-side half of the integration: it defines the items the
multiworld can hand out and the locations (checks) the player can fill. The
GAME-side half is the engine's transport bridge + scripts/ap/client.lua, which
map these item/location *names* to engine gates via the DataPackage (so ids are
never hard-coded on the game side).

Scope (intentionally small, matches scripts/ap/client.lua):
  Items     : Stage unlocks (10), Difficulty unlocks (Special/Perfect Agent),
              Device unlocks (4), plus filler.
  Locations : "<Stage> (<Difficulty>)" mission-complete checks, 10 x 3 = 30.
  Logic     : a mission-complete check needs its Stage item, and its Difficulty
              item for the harder difficulties (Agent is always available).
  Goal      : complete every stage on Agent (i.e. collect all 10 Stage items).

Packaging: zip this `perfect_dark/` folder into `perfect_dark.apworld` and drop
it in your Archipelago `custom_worlds/` (or `lib/worlds/`). See ../README.md.

Tested against the Archipelago 0.5.x World API. Names here MUST match the
name->gate table in scripts/ap/client.lua.
"""
from dataclasses import dataclass

from BaseClasses import Item, ItemClassification, Location, Region
from Options import PerGameCommonOptions
from worlds.AutoWorld import WebWorld, World
from worlds.generic.Rules import add_rule, set_rule

from .data import (
    ALL_DIFFICULTIES, FILLER_NAMES, GAME, PROGRESSION_PREFIXES, STAGE_NAMES,
    item_name_to_id, location_name_to_id,
)


class PerfectDarkItem(Item):
    game = GAME


class PerfectDarkLocation(Location):
    game = GAME


@dataclass
class PerfectDarkOptions(PerGameCommonOptions):
    pass


class PerfectDarkWeb(WebWorld):
    theme = "dirt"


class PerfectDarkWorld(World):
    """A minimal Perfect Dark randomizer: stage/difficulty/device access shuffle."""

    game = GAME
    web = PerfectDarkWeb()
    options_dataclass = PerfectDarkOptions
    item_name_to_id = item_name_to_id
    location_name_to_id = location_name_to_id

    def create_item(self, name: str) -> PerfectDarkItem:
        classification = (
            ItemClassification.progression
            if name.startswith(PROGRESSION_PREFIXES)
            else ItemClassification.filler
        )
        return PerfectDarkItem(name, classification, self.item_name_to_id[name], self.player)

    def create_regions(self) -> None:
        menu = Region("Menu", self.player, self.multiworld)
        for loc_name, loc_id in self.location_name_to_id.items():
            menu.locations.append(
                PerfectDarkLocation(self.player, loc_name, loc_id, menu)
            )
        self.multiworld.regions.append(menu)

    def create_items(self) -> None:
        pool = [
            self.create_item(name)
            for name in self.item_name_to_id
            if name not in FILLER_NAMES
        ]
        # Pad to one item per location with filler.
        while len(pool) < len(self.location_name_to_id):
            pool.append(self.create_item(self.random.choice(FILLER_NAMES)))
        self.multiworld.itempool += pool

    def set_rules(self) -> None:
        player = self.player
        for stage in STAGE_NAMES:
            for diff in ALL_DIFFICULTIES:
                loc = self.multiworld.get_location(f"{stage} ({diff})", player)
                set_rule(loc, lambda state, s=stage: state.has(f"Stage: {s}", player))
                if diff != "Agent":
                    add_rule(loc, lambda state, d=diff: state.has(f"Difficulty: {d}", player))

        self.multiworld.completion_condition[player] = lambda state: all(
            state.has(f"Stage: {s}", player) for s in STAGE_NAMES
        )
