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
| `ctx:self()` | read-only snapshot of the chr currently running this ailist, or `nil` |

`ctx:self()` returns a table describing *which* chr the callback is running for,
so an override can make per-enemy decisions and keep per-`chrnum` state:

| Field | Meaning |
| --- | --- |
| `chrnum` | the chr's id (stable for its lifetime; key your state table by this) |
| `x`, `y`, `z` | world position |
| `room` | first room number (`-1` if unknown) |
| `health`, `maxhealth` | `maxdamage - damage`, and `maxdamage` |
| `shield` | current shield |
| `alertness` | `0..255` |
| `target_chrnum` | the chr it is targeting, if that target is another chr (else absent) |
| `target_playernum` | the player it is targeting, if the target is a player (else absent) |

It is a snapshot for that call — re-call each frame for fresh values. Returns
`nil` for object-driven lists (trucks/helis/hovercars) which have no chr.

`ctx:run` is for hand-written lists that want to call engine commands directly.
Note that control-flow commands (labels, gotos) are not meaningful in synthetic
mode — use Lua's own `if`/`while`/`goto` instead.

**You usually don't need raw `ctx:run`.** The generated helper library
[`scripts/ai.lua`](../scripts/ai.lua) wraps *every* command (all ~440) as a
named function that packs the operands for you:

```lua
local ai = dofile("scripts/ai.lua")
ai.set_target_chr(ctx, 0xf6)          -- CHR_TARGET
ai.try_attack_stand(ctx, 0x220, 0, 0) -- vs ctx:run(0x15, 0x02,0x20, 0,0, 0)
```

Every command's opcode, exact operand byte layout, engine handler, and
description are in [`aicommands.md`](aicommands.md) — the reference for both the
`ai.*` wrappers and raw `ctx:run`. Both `ai.lua` and `aicommands.md` are
generated from the engine source by `tools/gen_aicommands.py` (wired into the
build), so they can't drift. For a complete, commented example that drives an
enemy entirely from Lua, see
[`scripts/examples/lua_authored_enemy.lua`](../scripts/examples/lua_authored_enemy.lua).

### The `pd` table

| Call | Meaning |
| --- | --- |
| `pd.register_ailist(id, fn)` | override the ailist with the given id |
| `pd.on(event, fn)` | register an event handler (see Events) |
| `pd.draw_box(x, y, w, h, color, [secs])` | 2D overlay box; `secs` omitted/0 = one frame |
| `pd.draw_text(x, y, text, color, [secs])` | 2D overlay text |
| `pd.each_chr(fn)` | iterate live characters this frame (see X-ray) |
| `pd.log(msg)` | print to stderr **and** the in-game console |
| `pd.chr_info(chrnum)` | table for any chr by id (same shape as `ctx:self()`), or `nil` |
| `pd.chr_pos(chrnum)` | `x, y, z` of a chr, or `nil` |
| `pd.chr_health(chrnum)` | `health, maxhealth` of a chr, or `nil` |
| `pd.player_pos([n])` | `x, y, z` of player `n` (default 0), or `nil` |
| `pd.player_count()` | number of active local players |
| `pd.distance(x1,y1,z1, x2,y2,z2)` | Euclidean distance (helper) |

### World / entity queries

The `pd.chr_*` / `pd.player_*` accessors are **read-only** snapshots of current
engine state — use them to make decisions (range, health, line-of-fire). They
return `nil` (or no values) for an unknown chrnum or absent player. `chr_info`
returns the same table shape as [`ctx:self()`](#the-ctx-object). Example —
shoot only when the player is close:

```lua
local me = ctx:self()
local px, py, pz = pd.player_pos(0)
if px and pd.distance(me.x, me.y, me.z, px, py, pz) < 1500 then
  ai.try_attack_stand(ctx, 0x220, 0, 0)
end
```

Overlay coordinates are the lo-res virtual screen (~320x240, the same space the
console uses); `color` is `0xRRGGBBAA`. Overlays with a `secs` lifetime persist
and fade out on their own; one-frame overlays are meant to be re-issued every
frame from a `"draw"` handler.

### Events (`pd.on`)

| Event | Handler args | Fires when |
| --- | --- | --- |
| `"weaponfire"` | `(weaponnum, playernum)` | a player fires a shot |
| `"alert"` | `(chrnum, playernum)` | an enemy reacts to / targets the player (its action block switches to its shot / shooting-at-me list) |
| `"kill"` | `(chrnum, killerplayernum)` | a character dies |
| `"draw"` | `()` | once per frame, for immediate-mode drawing |

Handlers run through `pcall`, so an error in one is logged and skipped — it never
crashes the game. Events are **local and cosmetic**: they fire wherever that code
runs (campaign = locally; netplay = where AI/guns run, i.e. the host) and have no
effect on game state or the network protocol.

### AI X-ray (`pd.each_chr`)

Inside a `"draw"` handler, `pd.each_chr(fn)` calls
`fn(chrnum, ailistid, aioffset, alertness, islua)` for every character whose AI
ran this frame — `ailistid`/`aioffset` are sampled straight from the Lua exec
loop, so drawing them proves each action block is transpiled to Lua and executed
live. See `scripts/showcase.lua`.

### Console

Open the console with `~`:

- `/lua reload` — reset and re-run `scripts/init.lua` immediately.
- `/lua <expr>` — evaluate a Lua string now (result/errors print to the console).

This API will keep growing (engine-command helpers, entity/world queries,
networking hooks).

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
