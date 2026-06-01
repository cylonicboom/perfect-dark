# Lua scripts

Drop-in Lua for the port's action-block scripting layer. The engine looks for
`scripts/init.lua` **relative to the working directory** (i.e. next to the
executable / in your game data folder) on startup, on each stage load, and on
`/lua reload`.

- `init.lua` — entry point; loads `showcase.lua`.
- `showcase.lua` — demo: weapon-fire / enemy-alert / kill events, a live AI
  "X-ray" overlay, and an (optional) fully Lua-authored enemy.
- `examples/lua_authored_enemy.lua` — a fully-commented enemy whose combat
  behaviour is written from scratch in Lua via `ctx:run`; the "if this works,
  almost anything will" proof.

## Authoring AI in Lua

Two references make this writable by a human or an agent:

- [`../docs/aicommands.md`](../docs/aicommands.md) — **every** engine AI command
  (~440), each with its opcode, operand **byte layout**, engine handler, and a
  description. The lookup table for `ctx:run(opcode, bytes...)`.
- [`../docs/luascripting.md`](../docs/luascripting.md) — the `ctx` / `pd` API,
  events, the override mechanism, and the return-value contract.

`aicommands.md` is generated from the engine source by
[`../tools/gen_aicommands.py`](../tools/gen_aicommands.py); re-run it after any
change to the AI command set so the reference can't drift.

## Console (open with `~`)

- `/lua reload` — reset and re-run `scripts/init.lua` live.
- `/lua <expr>` — run a Lua expression now, e.g.
  `/lua pd.draw_text(80,60,"hello",0xffffffff,3)`.
- `pd.log(...)` output appears in the console.

See [`../docs/luascripting.md`](../docs/luascripting.md) for the full API.

Scripting is **on by default** and always falls back to the original engine
behaviour if a script errors — a broken script logs to the console and the game
keeps running.
