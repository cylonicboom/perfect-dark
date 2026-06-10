#!/usr/bin/env bash
# run_client.sh — launch a HEADLESS CLIENT for a prop-sync soak.
#
# Phase 2 of the prop-sync consistency plan (docs/PORT_NET_SOAK.md). Connects a
# headless build to a server as a combatant, runs the client-side prop-sync
# apply + the invariant auditor (both live in the game-TICK path, which runs
# headless — lvRender is skipped), and writes a diag CSV. Pair its CSV with the
# server's and feed both to tools/netsoak.py for the manifest-parity verdict.
#
# This is the second machine the soak needs: the dedicated server alone only
# proves the HOST's invariants; a connected client is what exercises the
# client-side apply paths (netmsgSvcPropMoveRead, netChrInterpolate, the client
# objTickPlayer physics, the reconcile/heal/reaper) and lets the parity check
# compare both sides' prop sets.
#
# WHAT IT DOES / DOESN'T DO (read docs/PORT_NET_SOAK.md "Headless client"):
#   * spawns at each ROUND START (server-authoritative mpStartMatch), takes
#     neutral input (stands still — easy kills = good drop/death churn from the
#     bots), receives + applies all prop sync, audits once a second.
#   * does NOT mid-round respawn (the local-pawn respawn trigger is render-path
#     gated for clients); it waits for the next round. Fine for a soak.
#
# Usage:
#   tools/soak/run_client.sh ADDR[:PORT] [BINARY] [MINUTES]
# Defaults: BINARY=build_ded/pd-server.x86_64 (the headless build can host OR
#           join), MINUTES=0 (until Ctrl-C).
# Examples:
#   tools/soak/run_client.sh 127.0.0.1:27100            # local soak server
#   tools/soak/run_client.sh pd.example.net:27100 '' 30 # 30-min VPS repro
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ADDR="${1:-}"
BIN="${2:-$HERE/build_ded/pd-server.x86_64}"
MINUTES="${3:-0}"

if [ -z "$ADDR" ]; then
  echo "usage: $0 ADDR[:PORT] [BINARY] [MINUTES]" >&2
  exit 2
fi
if [ ! -x "$BIN" ]; then
  echo "error: binary not found/executable: $BIN" >&2
  echo "build the headless target:  cmake -DDEDICATED_SERVER=ON .. && make -j" >&2
  exit 2
fi

OUTDIR="$HERE/tools/soak/out"
mkdir -p "$OUTDIR"
STAMP="$(date -u +%Y%m%d_%H%M%S)"
DIAG="$OUTDIR/client_${STAMP}.csv"
LOG="$OUTDIR/client_${STAMP}.log"

echo "soak headless client:"
echo "  binary   : $BIN"
echo "  connect  : $ADDR"
echo "  diag CSV : $DIAG   (feed this + the server CSV to tools/netsoak.py)"
echo "  console  : $LOG"
echo "  duration : $([ "$MINUTES" = 0 ] && echo 'until Ctrl-C' || echo "${MINUTES} min")"
echo
echo "NOTE: the server it joins must run THIS branch's build (same"
echo "      NET_PROTOCOL_VER) or auth is rejected with DISCONNECT_VERSION."
echo

# --headless-client <addr> : headless runtime (no window/audio/input) + JOIN.
# --netdiag opens the diag CSV; the auditor is on by default and logs `audit:`
# lines once a second (role=C).
CMD=( "$BIN" --headless-client "$ADDR" --netdiag "$DIAG" )

echo "+ ${CMD[*]}" | tee "$LOG"
if [ "$MINUTES" = 0 ]; then
  "${CMD[@]}" 2>&1 | tee -a "$LOG"
else
  timeout --signal=INT "${MINUTES}m" "${CMD[@]}" 2>&1 | tee -a "$LOG"
  echo
  echo "soak window elapsed. client-side verdict:"
  python3 "$HERE/tools/netsoak.py" "$DIAG"
  echo
  echo "for the full parity verdict, run with the matching server CSV:"
  echo "  tools/netsoak.py tools/soak/out/server_<stamp>.csv $DIAG"
fi
