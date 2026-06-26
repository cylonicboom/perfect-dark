# Perfect Dark — Archipelago tooling

The game-side transport bridge lives in the engine (`src/game/luaai_ap.c` +
`scripts/ap/client.lua` + `json.lua`). This folder is the **server-side** half
and the test harness.

## Contents

| Path | What |
|---|---|
| `apworld/perfect_dark/` | The Archipelago world definition (items, locations, logic, goal). |
| `apworld/perfect_dark/data.py` | Canonical item/location **names + ids** — dependency-free, shared by the apworld, the generator, and (by name) `scripts/ap/client.lua`. |
| `gen_datapackage.py` | Dumps `data.py`'s id tables to `datapackage.json`. |
| `mock_ws.py` | Throwaway mock AP server for validating the transport without a real server. Serves the DataPackage and grants a scripted starting set. |
| `datapackage.json` | Generated (git-ignored); the mock loads it. |

## The name contract

Item/location **names** are the contract between three places — keep them in sync:

- `apworld/perfect_dark/data.py` (defines names → ids),
- the DataPackage the server sends (derived from the above),
- `scripts/ap/client.lua` (`STAGE_NAME_TO_INDEX` / `DIFF_NAME_TO_INDEX` /
  `DEVICE_NAME_TO_WEAPON` / `STAGE_DISP`, which map names → engine gates).

The game side resolves ids **by name via the DataPackage**, so ids are never
hard-coded in the engine — only names are.

## Test against the mock (no real AP install needed)

```sh
python3 tools/ap/gen_datapackage.py            # -> datapackage.json
python3 tools/ap/mock_ws.py                     # ws://127.0.0.1:38281
#   wss:  python3 tools/ap/mock_ws.py --tls --cert cert.pem --key key.pem
#   cert: openssl req -x509 -newkey rsa:2048 -nodes -keyout key.pem \
#           -out cert.pem -days 1 -subj //CN=localhost   (// for Git Bash)
```
In-game console: `/lua ap.connect("ws://127.0.0.1:38281")`. The mock grants
Defection + Villa + Special Agent + Night Vision; those stages should appear in
the solo mission list, and completing a listed mission reports a check.

## Run against a real Archipelago server

1. **Package the apworld:** zip the `perfect_dark/` folder into
   `perfect_dark.apworld` (the zip must contain the `perfect_dark/` directory)
   and drop it in your Archipelago `custom_worlds/` folder.
2. **Generate a seed** with a YAML that selects `game: Perfect Dark`, then run
   `MultiServer.py` on the output.
3. In-game: `/lua ap.connect("ws://<host>:<port>")` (or `wss://…`). Set the slot
   name first if needed: `/lua ap.slot_name = "YourSlot"` before connecting.

**Status:** P2 vertical slice — a small item/location set (10 stages × 3
difficulties as checks; stage/difficulty/device items). The apworld targets the
Archipelago 0.5.x `World` API and is **compile-checked only**; validate it in
your AP install (`python -m Tests` / a test generation) before trusting a seed.
