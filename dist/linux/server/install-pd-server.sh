#!/bin/sh
# Install the Perfect Dark headless dedicated server as a systemd templated
# service. Run as root. Idempotent: re-running updates the binary + unit without
# clobbering your ROM, configs or playlists.
#
# Usage:
#   sudo ./install-pd-server.sh [path-to-pd-server-binary]
#
# If no path is given it looks for build-server/pd-server.x86_64 relative to the
# current directory (the default output of -DDEDICATED_SERVER=ON).

set -eu

PREFIX=/opt/pd-server
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
BIN_SRC=${1:-build-server/pd-server.x86_64}

if [ "$(id -u)" -ne 0 ]; then
  echo "error: run as root (sudo)." >&2
  exit 1
fi

if [ ! -f "$BIN_SRC" ]; then
  echo "error: server binary not found: $BIN_SRC" >&2
  echo "build it first:" >&2
  echo "  cmake -B build-server -DDEDICATED_SERVER=ON . && cmake --build build-server -j" >&2
  echo "or pass the path explicitly: $0 /path/to/pd-server.x86_64" >&2
  exit 1
fi

echo "Installing Perfect Dark dedicated server to ${PREFIX} ..."
install -d "${PREFIX}" "${PREFIX}/data" "${PREFIX}/playlists" /etc/pd-server
install -m 0755 "$BIN_SRC" "${PREFIX}/pd-server"

install -m 0644 "${HERE}/pd-server@.service" /etc/systemd/system/pd-server@.service

# First-run examples only — never overwrite operator-edited files.
if [ ! -f /etc/pd-server/dm.conf ]; then
  install -m 0644 "${HERE}/pd-server.conf.example" /etc/pd-server/dm.conf
fi
if [ ! -f "${PREFIX}/playlists/default.ini" ]; then
  install -m 0644 "${HERE}/server_playlist.example.ini" "${PREFIX}/playlists/default.ini"
fi

systemctl daemon-reload

cat <<EOF

Done. Next steps:

  1. Place your ROM:        ${PREFIX}/data/pd.ntsc-final.z64
  2. Edit instance config:  /etc/pd-server/dm.conf      (PORT, NAME, PLAYLIST, EXTRA)
  3. Edit playlist:         ${PREFIX}/playlists/default.ini
  4. Open the UDP port in your firewall (default 27100/udp).
  5. Start the instance:
       sudo systemctl enable --now pd-server@dm
       systemctl status pd-server@dm
       journalctl -u pd-server@dm -f

  More instances: create /etc/pd-server/<name>.conf with a unique PORT, then
       sudo systemctl enable --now pd-server@<name>
EOF
