# Perfect Dark headless dedicated server (Debian/systemd)

Files for deploying the headless dedicated server on a GUI-less Linux box.
Design/rationale: [`docs/PORT_DEDICATED_SERVER_DEBIAN.md`](../../../docs/PORT_DEDICATED_SERVER_DEBIAN.md).

| File | Purpose |
|---|---|
| `pd-server@.service` | Templated systemd unit (one instance per name) |
| `pd-server.conf.example` | Per-instance config (copied to `/etc/pd-server/<name>.conf`) |
| `server_playlist.example.ini` | Sample map/mode rotation |
| `install-pd-server.sh` | Installs binary + unit + example configs |

## Build

The server needs **no SDL/OpenGL/audio** — only a C/C++ toolchain and zlib:

```sh
sudo apt-get install -y build-essential cmake zlib1g-dev
cmake -B build-server -DDEDICATED_SERVER=ON .
cmake --build build-server -j
# -> build-server/pd-server.x86_64  (links only libc/libstdc++/libm/libz)
```

## Install & run

```sh
sudo dist/linux/server/install-pd-server.sh build-server/pd-server.x86_64

# put your ROM in place
sudo cp pd.ntsc-final.z64 /opt/pd-server/data/

# edit /etc/pd-server/dm.conf and /opt/pd-server/playlists/default.ini, then:
sudo systemctl enable --now pd-server@dm
journalctl -u pd-server@dm -f
```

## Multiple instances

Each instance is one config file with a unique `PORT`; it gets its own UDP port,
its own persistent state dir (`/var/lib/pd-server/<name>`), and its own transient
user. The install dir is shared read-only.

```sh
# /etc/pd-server/dm.conf        -> PORT=27100
# /etc/pd-server/objective.conf -> PORT=27101
sudo systemctl enable --now pd-server@dm pd-server@objective
```

Open the UDP ports you assign (e.g. `27100-27101/udp`) in the firewall.
Budget ~45–50 MB RAM and up to ~1 CPU core per busy instance.
