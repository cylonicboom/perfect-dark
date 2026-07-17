# Crash Symbolication & the Symbol Archive

Port-only workflow for turning raw-offset `pd.crash.log` files — including ones
from **other users' machines** and **stale/rebuilt exes** — into full
function + file:line backtraces with one command.

## The problem it solves

The Windows crash handler (`port/src/crash.c`) resolves symbols in-process via
addr2line only when `addr2line.exe` is present next to the exe or in an MSYS2
install. Users we ship builds to have neither, so their `pd.crash.log` contains
raw module-relative offsets (`#00: 00007ff7...: [base]+00000000000ac20a`).
Those offsets are only meaningful against the *exact binary* that crashed — and
the local build dir has usually moved on by the time the report arrives.

## The three pieces

1. **Self-identifying crash logs** — the dump header now contains
   `BUILD: <branch> <git-hash> (<target>) link=<PE-timestamp>` (both the
   Windows SEH handler and the Linux signal handler; Linux omits `link=`).
   - The git hash comes from `versioninfo.h`, which is **configure-time**: it
     only refreshes when cmake reconfigures, so builds from a dirty tree or an
     old configure carry a stale hash.
   - `link=` is the PE COFF `TimeDateStamp` — unique **per link**, so it
     disambiguates multiple builds sharing one configure-time hash. It is the
     authoritative match key.

2. **The symbol archive** — `symbols/` at the repo root (gitignored). Each
   shipped exe gets its DWARF debug info split out via
   `objcopy --only-keep-debug` into
   `symbols/pd-<branch>-<hash>-<linkstamp>.debug` (~25 MB each; the exe keeps
   its own debug info too — the archive is only for after it's overwritten).

   **Archive at deploy time, from the deployed exe:**
   ```
   python tools/symbolicate.py --archive "F:/Games/Perfect Dark AIO/pd.x86_64.exe"
   ```
   Re-running is idempotent (skips if that link stamp is already archived).

3. **The one-command symbolicator** — parse any pasted/received crash log:
   ```
   python tools/symbolicate.py pd.crash.log            # auto-picks the archive
   python tools/symbolicate.py - < paste.txt           # stdin
   python tools/symbolicate.py crash.log some/pd.debug # explicit symbol file
   ```
   Symbol-file resolution order: `link=` stamp match → BUILD hash match →
   newest archive (warns) → `build_debug_sdl3/pd.x86_64.exe` (warns). It
   resolves the `PC:` line plus every main-module backtrace frame, including
   inline chains (`addr2line -f -C -p -i`), and reads the preferred image base
   from the symbol file's PE header (ASLR makes the runtime base in the log
   meaningless; MinGW x64 default `0x140000000` is the fallback).

## Gotchas

- **addr2line treats its address arguments as hex regardless of prefix** — a
  decimal `$((...))` shell expansion silently resolves the wrong address and
  returns `??`. The tool always formats `0x%x`.
- Pre-existing crash logs (before the BUILD line shipped) can't auto-match;
  pass the symbol file explicitly, or let the newest-archive fallback guess.
- Logs written by *old* exes keep coming after a fix ships — check the BUILD
  line before assuming a "fixed" crash regressed.
- The `.debug` files are per-*link*: rebuild without deploying → nothing to
  archive; deploy without archiving → that build is only debuggable while
  `build_debug_sdl3/pd.x86_64.exe` still matches.
- `--archive` extracts the identity from the exe's embedded
  `version: <branch> <hash> (<target>)` string (the one `system.c` logs at
  boot), so the archive name always agrees with what that exe prints in its
  own crash logs — even when the hash is stale relative to git.

## Read when

Symbolicating any user crash report, touching `crash.c`'s dump format (the
parser keys on `MAIN MODULE:`, `BUILD:`, and the `#NN: addr: [base]+ofs` frame
shape), or changing the deploy routine (the archive step rides it).
