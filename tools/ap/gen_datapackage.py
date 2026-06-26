#!/usr/bin/env python3
"""Dump the Perfect Dark apworld's item/location id tables to datapackage.json
so the mock server (mock_ws.py) can serve a DataPackage identical to what a real
Archipelago server would. Imports apworld/perfect_dark/data.py directly (by file
path, so it doesn't pull in the Archipelago framework).

Usage:  python3 tools/ap/gen_datapackage.py   ->  tools/ap/datapackage.json
"""
import importlib.util
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
DATA_PY = os.path.join(HERE, "apworld", "perfect_dark", "data.py")


def load_data():
    spec = importlib.util.spec_from_file_location("pd_data", DATA_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    data = load_data()
    dp = {
        "games": {
            data.GAME: {
                "item_name_to_id": data.item_name_to_id,
                "location_name_to_id": data.location_name_to_id,
                "checksum": "mock",
            }
        }
    }
    out = os.path.join(HERE, "datapackage.json")
    with open(out, "w") as f:
        json.dump(dp, f, indent=1)
    print(f"wrote {out}: {len(data.item_name_to_id)} items, "
          f"{len(data.location_name_to_id)} locations")


if __name__ == "__main__":
    main()
