# Lua Scripting — Roadmap

Status of the action-block Lua scripting layer and the planned path forward.
See [`luascripting.md`](luascripting.md) for the current API and
[`aicommands.md`](aicommands.md) for the full command reference.

## Shipped

| Capability | Where |
|---|---|
| Action blocks transpiled to Lua and executed each frame (all lists) | `src/game/luaai.c`, `luaai_transpile.c` |
| Server-authoritative `pd.register_ailist` overrides | `luaai.c`, `chrai.c` |
| In-game `~` console: `/lua reload`, `/lua <expr>`, `pd.log` → console | `port/src/net/net.c`, `console.c` |
| 2D overlay draw API: `pd.draw_box`, `pd.draw_text` | `src/game/luaai_api.c` |
| Events: `pd.on("weaponfire" / "alert" / "kill" / "draw")` | `luaai_api.c` + hook sites |
| AI X-ray: `pd.each_chr` (live ailist id + offset per enemy) | `luaai_api.c` |
| **Generated command reference** (all ~440 commands) | `docs/aicommands.md` |
| **Generated Lua helper library** (`ai.<command>(ctx, ...)`) | `scripts/ai.lua` |
| Generator folded into the build (can't drift) | `tools/gen_aicommands.py`, CMake `pd_aiscripts` |
| Worked from-scratch Lua enemy example | `scripts/examples/lua_authored_enemy.lua` |

The pipeline is proven end to end: every enemy's AI runs through Lua, and a
human or agent can author new behaviour from the reference + helper library
without reading engine source.

## Next (high-value, feasibility checked)

These are ordered by value-to-effort. Each builds on the shipped base.

### 1. Current-chr handle inside ailist callbacks
**Why:** today an override function knows it is running but not *which* chr it
is (no `self`). That blocks per-enemy state and most interesting AI.
**Plan:** expose the active chr to Lua — extend `ctx` with `ctx:self()` returning
a light handle (chrnum + accessors). `chrai.c` already tracks `g_Vars.chrdata`
during execution, so this is a bridge accessor, not new state.
**Feasibility:** straightforward; same bridge pattern as `chraiLuaGetChrNum`.

### 2. Entity / world query API (`pd.*` read accessors)
**Why:** scripts need to *read* the world to make decisions (positions, rooms,
distances, health, alertness, the player).
**Plan:** add read-only `pd` accessors backed by existing engine getters
(`chrGetPos`, room lookups, `g_Vars.players`, `chr->damage`, etc.), guarded
`#ifndef PLATFORM_N64`. Start with: `pd.chr_pos(chrnum)`, `pd.player_pos(n)`,
`pd.chr_health(chrnum)`, `pd.distance(a, b)`.
**Feasibility:** moderate; wraps existing accessors, no new engine logic.

### 3. 3D marker / spawn at a kill (the "cube on death")
**Why:** the requested visceral proof — spawn a world object where an enemy died.
**Plan:** `kill` event already fires (`chraction.c` `chrDie`). Add a `pd.spawn`
that allocates a prop at a position. `propAllocate` and
`weaponCreateProjectileFromWeaponNum` exist; the work is object
initialisation/registration (model, room, type) done safely.
**Feasibility:** feasible-but-nontrivial — needs object-init investigation; do a
small spike first (spawn one known model at a fixed pad, then generalise).

### 4. More events
`pd.on("spawn" / "damage" / "objective" / "roomenter")`. Each is one guarded
emit call at the relevant engine site, mirroring the existing three.
**Feasibility:** easy, incremental; add as needed.

## Later (ambitious, needs a research spike)

- **Switch player models at runtime** — model/config swap path not yet located;
  spike needed before committing.
- **Controllable custom entity** ("run around as a cube") — entity create +
  input/camera routing; the largest item, depends on #2 + #3 landing first.
- **Mission director toolkit** — higher-level helpers over events + spawn +
  query (reinforcement waves, scripted encounters, hive-mind alerts).

## Principles (carry forward)

- **Don't break the decompilation contract** — no symbol renames; engine hooks
  go behind `#ifndef PLATFORM_N64`, cosmetic/local unless deliberately gated.
- **Server-authoritative for anything affecting AI/gameplay** — overrides and
  any future mutating API must respect `g_NetMode != NETMODE_CLIENT`.
- **Generate, don't hand-maintain** — anything derivable from the engine source
  (command tables, helper bindings) should come from `gen_aicommands.py` so it
  can't drift.
- **Prove each increment** — ship a small verifiable step the user can build and
  see, then expand.
