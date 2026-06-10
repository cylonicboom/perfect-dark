#!/usr/bin/env bash
# parity_latest.sh — run the combined server<->client manifest-parity verdict
# (tools/netsoak.py) on the NEWEST server_*.csv + client_*.csv pair in
# tools/soak/out. Companion to soak_oneclick.bat; see docs/PORT_NET_SOAK.md.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PY="$(command -v python3 || command -v python || command -v py || true)"

S="$(ls -t "$HERE"/tools/soak/out/server_*.csv 2>/dev/null | head -1 || true)"
C="$(ls -t "$HERE"/tools/soak/out/client_*.csv 2>/dev/null | head -1 || true)"

echo "server CSV: ${S:-<none>}"
echo "client CSV: ${C:-<none>}"

if [ -z "$PY" ]; then
  echo "error: python not found (need python3/python/py on PATH)" >&2
  exit 2
fi
if [ -z "$S" ] || [ -z "$C" ]; then
  echo "error: need BOTH a server_*.csv and a client_*.csv in tools/soak/out" >&2
  exit 2
fi

exec "$PY" "$HERE/tools/netsoak.py" "$S" "$C"
