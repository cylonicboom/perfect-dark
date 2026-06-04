# Port Platform Layer (`port/src/`)

> Auto-loads when working under `port/src/`. For the net subsystem specifically, see [`net/CLAUDE.md`](net/CLAUDE.md).

## Key Platform Files

| File | Purpose |
|---|---|
| `main.c` | Entry point; initialises all subsystems in order |
| `pdmain.c` | Game-side init (memory, stage selection) |
| `pdsched.c` | Per-frame scheduler; calls `netStartFrame`/`netEndFrame` |
| `video.c` | SDL2 + OpenGL window/render |
| `input.c` | SDL2 input, key binds |
| `audio.c` / `mixer.c` | Audio mixer |
| `config.c` | INI config (`pd.ini`) |
| `fs.c` | File system / mod support |
| `system.c` | Logging, args |
| `console.c` | In-game dev console (`~`) |
| `mpsetups.c` | Multiplayer setup file save/load |

## Console (`console.c`)

- **Scrollback**: PageUp / PageDown step the visible window by half a page; Home jumps to the oldest line in the ring, End back to the live tail. Closing the console or submitting a line snaps back to the tail. While scrolled, the prompt prefix shows `[-N]` so it's obvious you're not at live output. Ring buffer is `CON_ROWS = 80` lines.
- **Closed-console message overlay** (`conRenderMsgs`): log/chat lines flagged `LOGFLAG_SHOWMSG` normally flash on screen for a few seconds even with the console closed. Gated by config `Console.ShowMessages` (`conShowMsgs`, **default 0 = off** for a clean boot / streaming); set to `1` to restore the popups. The console (`~`) still opens and records full scrollback regardless — only the transient on-screen flash is suppressed.
