# Lua scripts

Drop-in Lua for the port's action-block scripting layer. The engine looks for
`scripts/init.lua` **relative to the working directory** (i.e. next to the
executable / in your game data folder) on startup, on each stage load, and on
`/lua reload`.

- `init.lua` — entry point; loads `showcase.lua`.
- `showcase.lua` — demo: weapon-fire / enemy-alert / kill events, a live AI
  "X-ray" overlay, and an (optional) fully Lua-authored enemy.

## Console (open with `~`)

- `/lua reload` — reset and re-run `scripts/init.lua` live.
- `/lua <expr>` — run a Lua expression now, e.g.
  `/lua pd.draw_text(80,60,"hello",0xffffffff,3)`.
- `pd.log(...)` output appears in the console.

See [`../docs/luascripting.md`](../docs/luascripting.md) for the full API.

Scripting is **on by default** and always falls back to the original engine
behaviour if a script errors — a broken script logs to the console and the game
keeps running.
