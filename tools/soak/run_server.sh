#!/usr/bin/env bash
# run_server.sh — launch a dedicated server configured for a prop-sync soak.
#
# Phase 2 of the prop-sync consistency plan (docs/PORT_NET_SOAK.md). Brings up
# the headless dedicated server with the soak playlist, the invariant auditor
# writing to a diag CSV, and a sane bandwidth/rate config. It then auto-starts
# an 8-bot Combat Sim match as soon as ONE client connects (see the playlist's
# min_humans_to_start) — the bots generate the projectile/drop/death churn; the
# auditor logs one `audit:` line per second.
#
# This does NOT run unattended on its own: it needs (a) a ROM-provisioned build
# and (b) at least one client to connect (there is no headless client). Launch
# this, connect your client (tools/soak/CLIENT_NOTES below / PORT_NET_SOAK.md),
# let it run, then feed both diag CSVs to tools/netsoak.py for the verdict.
#
# Usage:
#   tools/soak/run_server.sh [SERVER_BINARY] [PORT] [MINUTES]
# Defaults: SERVER_BINARY=build_ded/pd-server.x86_64[.exe]  PORT=27100  MINUTES=0(=forever)
#
# WINDOWS (MSYS2): run this from the MSYS2 MinGW x64 shell at the repo root —
# bash, coreutils (timeout/tee/date) and python are all present there. The
# binary autodetects the .exe suffix. Ctrl-C / closing the window is a CLEAN
# shutdown (headlessInstallSignalHandlers); even on a hard kill the `audit:`
# lines survive (netDiagLogf flushes every non-position line immediately).
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"   # repo root

# Resolve a binary path, trying the bare name and the Windows .exe form.
resolve_bin() {
  for c in "$1" "$1.exe"; do
    if [ -x "$c" ]; then echo "$c"; return 0; fi
  done
  return 1
}

BIN_ARG="${1:-$HERE/build_ded/pd-server.x86_64}"
PORT="${2:-27100}"
MINUTES="${3:-0}"

BIN="$(resolve_bin "$BIN_ARG" || true)"
PY="$(command -v python3 || command -v python || command -v py || true)"

PLAYLIST="$HERE/tools/soak/playlist_soak.txt"
OUTDIR="$HERE/tools/soak/out"
mkdir -p "$OUTDIR"
STAMP="$(date -u +%Y%m%d_%H%M%S)"
DIAG="$OUTDIR/server_${STAMP}.csv"
LOG="$OUTDIR/server_${STAMP}.log"

if [ -z "$BIN" ]; then
  echo "error: server binary not found/executable: $BIN_ARG[.exe]" >&2
  echo "build it first (MSYS2 MinGW x64 shell or Linux):" >&2
  echo "  cmake -G 'Unix Makefiles' -DDEDICATED_SERVER=ON -DCMAKE_BUILD_TYPE=Release .. && make -j" >&2
  exit 2
fi
if [ ! -f "$PLAYLIST" ]; then
  echo "error: playlist missing: $PLAYLIST" >&2
  exit 2
fi

echo "soak server:"
echo "  binary   : $BIN"
echo "  port     : $PORT"
echo "  playlist : $PLAYLIST"
echo "  diag CSV : $DIAG   (feed this to tools/netsoak.py)"
echo "  console  : $LOG"
echo "  duration : $([ "$MINUTES" = 0 ] && echo 'until Ctrl-C' || echo "${MINUTES} min")"
echo
echo "Now connect a client to this host:port and run '/audit on' + '/diag <path>'"
echo "on it too (or set Net.Debug.LogPath in its pd.ini). See PORT_NET_SOAK.md."
echo

# --dedicated implies --host (auto-start server on boot).
# --svcrate 2 mirrors how pdmaster spawns instances (30Hz state, ~half band).
# --netdiag opens the diag CSV at host time; the auditor is on by default.
CMD=( "$BIN" --dedicated --port "$PORT" --playlist "$PLAYLIST"
      --netdiag "$DIAG" --svcrate 2 --maxclients 8 )

echo "+ ${CMD[*]}" | tee "$LOG"
if [ "$MINUTES" = 0 ] || ! command -v timeout >/dev/null 2>&1; then
  if [ "$MINUTES" != 0 ]; then
    echo "(no \`timeout\` on PATH — running uncapped; Ctrl-C to stop)"
  fi
  "${CMD[@]}" 2>&1 | tee -a "$LOG"
else
  # Run with a wall-clock cap, then SIGINT for a clean shutdown (flushes the
  # diag file via netDisconnect's netDiagClose). On MSYS2/Windows the INT may
  # arrive as a console ctrl event or a hard kill depending on the runtime —
  # either way the audit: lines are already flushed line-by-line.
  timeout --signal=INT "${MINUTES}m" "${CMD[@]}" 2>&1 | tee -a "$LOG"
  echo
  echo "soak window elapsed. verdict:"
  if [ -n "$PY" ]; then
    "$PY" "$HERE/tools/netsoak.py" "$DIAG"
  else
    echo "(python not found — run: py tools/netsoak.py $DIAG)"
  fi
fi
