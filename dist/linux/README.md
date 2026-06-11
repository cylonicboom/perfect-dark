# Linux packaging

## Flatpak (Perfect Dark Netplay)

`io.github.murkantor.perfect_dark_netplay.yaml` builds the netplay client
**from this source tree** (NTSC-final, OpenGL renderer) on the freedesktop
24.08 runtime, with SDL3 built as a module.

### Build & install

```sh
# from the repo root; needs flatpak-builder and the 24.08 SDK:
flatpak install flathub org.freedesktop.Platform//24.08 org.freedesktop.Sdk//24.08
flatpak-builder --user --install --force-clean build_flatpak_dir \
    dist/linux/io.github.murkantor.perfect_dark_netplay.yaml
```

### ROM and saves

The app is sandboxed; its private data dir on the host is
`~/.var/app/io.github.murkantor.perfect_dark_netplay/data/`. The launcher
creates `roms/`, `saves/` and `mods/` in there on first run. Put your ROM at:

```
~/.var/app/io.github.murkantor.perfect_dark_netplay/data/roms/pd.ntsc-final.z64
```

Saves and `pd.ini` land in `data/saves/`. Mod files (e.g. the optional
external model files some MP bodies use — see `docs/PORT_NET_CRASH_LEDGER.md`
#22) go in `data/roms/data/`, mirroring a normal install's layout relative to
`--basedir`.

### Netplay notes

- The manifest grants `--share=network`: joining, hosting (default UDP
  27100) and the master-server browser all work. To accept *incoming*
  connections when hosting, the usual host-side router/firewall port
  forwarding still applies — the flatpak sandbox itself does not block
  listening sockets.
- Run with extra flags via the wrapper, e.g.:
  `flatpak run io.github.murkantor.perfect_dark_netplay --connect host:27100`

### Variants

- The manifest builds NTSC-final only. To add PAL/JPN, duplicate the build
  block with `-DROMID=pal-final` / `jpn-final` (binaries land as
  `pd.pal.x86_64` / `pd.jpn.x86_64`) — the launcher script already dispatches
  per-ROM when those binaries exist.
- The SDL_GPU (Vulkan) renderer is disabled in the flatpak
  (`-DUSE_SDLGPU=OFF`) because the freedesktop SDK doesn't export a glslang
  CMake config; OpenGL is the project default regardless. To enable it, add a
  glslang module and drop the flag.
- `io.github.fgsfdsfgs.perfect_dark.*` are the upstream port's flatpak files
  (prebuilt-binary packaging used by its release CI); they are kept untouched
  for merge friendliness.

### Status

**Built and smoke-tested 2026-06-11** (WSL2 Debian trixie, flatpak-builder
1.4.x, user installation): SDL3 module + game build clean from the source
tree, app installs and launches — launcher creates the sandbox data dirs,
binary reports the correct version stamp, SDL3 creates a GL context, and it
proceeds to the ROM check (full in-game run needs a ROM in `data/roms/`).
One source fix was needed and is committed (`strcasecmp` include in
`port/src/input.c` — the 24.08 SDK's gcc treats implicit declarations as
errors). Not yet built in CI.

## Dedicated server

See `server/README.md` (systemd unit, install script, playlist example) and
`docs/PORT_DEDICATED_SERVER_DEBIAN.md`.
