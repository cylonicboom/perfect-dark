# Lua scripting (action blocks)

This port can execute the game's action blocks (AI lists / ailists) through an
embedded Lua runtime instead of the original bytecode interpreter. This is the
foundation for scripted single-player missions, custom multiplayer maps, and
networking helpers, and it makes modded behaviour possible without rebuilding
the engine.

> Status: experimental. Lua execution is **on by default** but always falls back
> to the original bytecode interpreter if anything goes wrong, so the game keeps
> running. See *Disabling* below.

## How it works

The original action block system (see [`ailists.md`](ailists.md)) is a bytecode
VM: each ailist is a list of two-byte commands, executed by `chraiExecute()` in
`src/game/chrai.c`. Each frame, every character/object runs its list until it
hits a `yield`.

With Lua enabled, the flow becomes:

1. `chraiExecute()` calls `luaaiExecute()` (`src/game/luaai.c`).
2. `chraiPrepare()` resolves the entity and applies the normal list-switch logic
   (shot list, dodge list, dark-room list).
3. The active ailist's bytecode is **transpiled on load** into a Lua chunk
   (`src/game/luaai_transpile.c`) and cached. The chunk is a dispatch loop:

   ```lua
   return function(ctx)
     while true do
       local off = ctx:cur()
       if false then
       elseif off == 0 then goto L_0
       elseif off == 3 then goto L_3
       else return 0 end
       ::L_0:: do local r = ctx:exec(0) if r ~= 0 then return r end end goto NEXT
       ::L_3:: do local r = ctx:exec(3) if r ~= 0 then return r end end goto NEXT
       ::NEXT::
     end
   end
   ```

4. `ctx:exec(off)` calls back into the original C command handler for the command
   at `off`. The handler advances/branches the program counter exactly as it
   always did, so **behaviour is identical to the bytecode interpreter** — Lua is
   just driving the loop. `ctx:exec` returns:
   - `0` continue to the next command
   - `1` yield (stop running this list this frame)
   - `2` the active list changed (e.g. `set_ailist`) or terminated

Because every jump target is resolved by the original `chraiGoToLabel()` logic,
all 480+ commands are supported on day one without re-implementing any of them.

## Modding

When the game starts (and whenever a new stage loads) the runtime looks for
`scripts/init.lua` in the working directory. If present it is executed. From
there you can register overrides and require other files.

### Overriding an ailist

```lua
-- scripts/init.lua

-- Replace global ailist 0x0001 (GAILIST_...) with custom Lua logic.
-- ctx:run(opcode, b0, b1, ...) invokes an engine command directly with the
-- given operand bytes. ctx:cur()/ctx:exec() are also available if you want to
-- drive the original list.
pd.register_ailist(0x0001, function(ctx)
  pd.log("running my custom ailist 0x0001")
  -- ... your logic here, using Lua control flow + ctx:run(...) ...
  return 1 -- 1 = yield this frame, 0 = list finished, 2 = list switched
end)
```

`pd.register_ailist(id, fn)` takes precedence over the auto-transpiled chunk for
that id. The id is the ailist id (see the ranges documented in
[`ailists.md`](ailists.md)).

> **Netplay:** AI is server-authoritative. Overrides only run on the host
> (`NETMODE_SERVER`) and in single-player (`NETMODE_NONE`); a connected client
> ignores its own overrides and runs the deterministic transpiled chunk, so the
> host's `scripts/init.lua` is the single source of truth for custom AI (e.g.
> custom bots). Clients do not need a matching script, and a mismatched one
> cannot desync AI. The auto-transpiled (non-override) path is identical to the
> bytecode interpreter and is always safe on both sides.

### The `ctx` object

| Call | Meaning |
| --- | --- |
| `ctx:cur()` | current command offset (program counter) |
| `ctx:exec(off)` | run the original command at `off`; returns 0/1/2 as above |
| `ctx:run(opcode, b0, b1, ...)` | build a synthetic command and run its handler; returns its break flag |

`ctx:run` is for hand-written lists that want to call engine commands directly.
Note that control-flow commands (labels, gotos) are not meaningful in synthetic
mode — use Lua's own `if`/`while`/`goto` instead.

### The `pd` table

| Call | Meaning |
| --- | --- |
| `pd.register_ailist(id, fn)` | override the ailist with the given id |
| `pd.log(msg)` | print a message to stderr |

This API is intentionally small for now and will grow (helper wrappers for the
most common commands, entity/world queries, networking hooks).

## Disabling

Set the global `g_LuaAiEnabled` to `0` to fall back to the pure bytecode
interpreter (it is `1` by default). The engine also disables Lua automatically
and reverts to bytecode if a Lua error occurs at runtime.

## Validating without the ROM

The transpiler and the dispatch contract can be tested without a Perfect Dark
ROM:

```sh
sh tools/luaai_test/build.sh
```

This compiles the vendored Lua, the transpiler, and a small harness that runs a
synthetic action block through the generated Lua and checks the control flow.

## Files

| Path | Purpose |
| --- | --- |
| `port/lua/` | vendored Lua 5.4 sources |
| `src/game/luaai_transpile.c` | bytecode → Lua transpiler (engine-independent) |
| `src/game/luaai.c` | Lua runtime, bridge, chunk cache, modding API |
| `src/include/game/luaai.h` | public API + bridge declarations |
| `src/game/chrai.c` | `chraiExecute` dispatch + bridge implementations |
| `tools/luaai_test/` | standalone transpiler/dispatch test |
