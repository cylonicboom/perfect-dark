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
| **Current-chr handle** `ctx:self()` (chrnum, pos, health, shield, alertness, target) | `luaai.c`, `chrai.c` |
| **World / entity query API** `pd.chr_info/chr_pos/chr_health/player_pos/player_count/distance` | `luaai_api.c`, `chrai.c` |
| **World mutation** `pd.spawn_at_chr(chrnum, weaponnum)` + `pd.spawn(weaponnum, x,y,z, [ref])` | `luaai_api.c`, `chraction.c` |
| **Events** `weaponfire`, `alert`, `damage`, `kill`, `spawn`, `roomenter`, `draw` | `luaai_api.c` + hook sites |

The pipeline is proven end to end: every enemy's AI runs through Lua, and a
human or agent can author new behaviour from the reference + helper library
without reading engine source. With `ctx:self()` + the query API an override can
read the world and make real decisions, and the spawn calls drop world objects
at a chr or at arbitrary coords.

## Next (high-value, feasibility checked)

These are ordered by value-to-effort. Each builds on the shipped base.

### 4b. `objective` event (remaining from #4)
`roomenter` is now shipped (synthesised per-frame in `luaTick` by diffing player
0's room — there is no single engine call site for it). `objective` (a criterion
or objective completing) is still open: completion is spread across many criteria
types in `objectives.c` (`criteria_roomentered`, `criteria_throwinroom`,
`criteria_holograph`, ...), so a clean single firing point needs a small design
pass rather than a one-line emit. Deferred until that's worth doing.

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
