# Perfect Dark headless dedicated server (Windows)

The same `DEDICATED_SERVER` build as Debian, for Windows. It's a **console**
application that links **no SDL2 or OpenGL** — just the C runtime, zlib and
Winsock — so it runs with no GUI and a tiny footprint.

| File | Purpose |
|---|---|
| `run-pd-server.bat` | Simple launcher (edit the flags) |
| `server_playlist.example.ini` | Sample rotation (copy from `dist/linux/server/`) |

## Build (MSYS2 / MinGW)

In an **MSYS2 MINGW64** shell (no SDL2 needed for the server):

```sh
pacman -S --needed mingw-w64-x86_64-toolchain mingw-w64-x86_64-zlib mingw-w64-x86_64-cmake make
cmake -G"Unix Makefiles" -Bbuild-server -DDEDICATED_SERVER=ON .
cmake --build build-server -j
# -> build-server/pd-server.x86_64.exe  (console app, no SDL/GL)
```

Ship these runtime DLLs next to the exe (from `/mingw64/bin`):
`zlib1.dll`, `libgcc_s_seh-1.dll`, `libwinpthread-1.dll`.

## Run

1. Put your ROM at `data\pd.ntsc-final.z64` (next to the exe), or pass
   `--rom-file C:\path\to\pd.ntsc-final.z64`.
2. Put a `server_playlist.ini` next to the exe (copy the example).
3. Double-click `run-pd-server.bat` (or run the exe from a console).
4. Open the UDP port (default 27100) in Windows Firewall.

Closing the console window or pressing Ctrl-C shuts the server down cleanly
(handled by the console-control handler in `headless.c`).

## Run as a Windows service

`pd-server.x86_64.exe` is a console program, so wrap it with a service manager.
**NSSM** (the Non-Sucking Service Manager) is the simplest:

```bat
nssm install PDServer "C:\pd-server\pd-server.x86_64.exe"
nssm set PDServer AppDirectory "C:\pd-server"
nssm set PDServer AppParameters "--dedicated --port 27100 --maxclients 8 --server-name \"Perfect Dark Dedicated\" --playlist server_playlist.ini"
nssm set PDServer AppStdout "C:\pd-server\server.log"
nssm set PDServer AppStderr "C:\pd-server\server.log"
nssm start PDServer
```

Manage it with `nssm stop/restart/remove PDServer` or the standard
`sc.exe` / Services console. For multiple instances, install several services
with distinct names, ports and playlists (mirrors the Linux `pd-server@.service`
template idea).

> Built-in `sc.exe create` alone won't work directly because the exe isn't a
> native service binary — use NSSM (or `srvany`) as the wrapper.
