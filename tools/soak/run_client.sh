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
# Defaults: BINARY=build_ded/pd-server.x86_64[.exe] (the headless build can
#           host OR join), MINUTES=0 (until Ctrl-C).
# Examples:
#   tools/soak/run_client.sh 127.0.0.1:27100            # local soak server
#   tools/soak/run_client.sh pd.example.net:27100 '' 30 # 30-min VPS repro
#
# WINDOWS (MSYS2): run from the MSYS2 MinGW x64 shell at the repo root (second
# shell window alongside run_server.sh). The .exe suffix autodetects.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

resolve_bin() {
  for c in "$1" "$1.exe"; do
    if [ -x "$c" ]; then echo "$c"; return 0; fi
  done
  return 1
}

ADDR="${1:-}"
BIN_ARG="${2:-$HERE/build_ded/pd-server.x86_64}"
MINUTES="${3:-0}"

BIN="$(resolve_bin "$BIN_ARG" || true)"
PY="$(command -v python3 || command -v python || command -v py || true)"

if [ -z "$ADDR" ]; then
  echo "usage: $0 ADDR[:PORT] [BINARY] [MINUTES]" >&2
  exit 2
fi
if [ -z "$BIN" ]; then
  echo "error: binary not found/executable: $BIN_ARG[.exe]" >&2
  echo "build the headless target:  cmake -G 'Unix Makefiles' -DDEDICATED_SERVER=ON .. && make -j" >&2
  exit 2
fi

# Preflight: catch the Windows silent-death case. A MinGW exe with unresolved
# DLLs exits instantly with NO output at all — the usual cause is launching
# from the plain MSYS shell, which leaves /mingw64/bin (libwinpthread/libgcc/
# zlib1) off PATH. MSYS2's ldd understands PE binaries, so check up front.
if command -v ldd >/dev/null 2>&1; then
  MISSING_DLLS="$(ldd "$BIN" 2>/dev/null | grep -i 'not found' || true)"
  if [ -n "$MISSING_DLLS" ]; then
    echo "error: $BIN cannot load in THIS shell — unresolved libraries:" >&2
    echo "$MISSING_DLLS" >&2
    if [ -n "${MSYSTEM:-}" ] && [ "${MSYSTEM:-}" != "MINGW64" ]; then
      echo "you are in the '$MSYSTEM' shell — use the 'MSYS2 MinGW x64' shell" >&2
      echo "(prompt says MINGW64), which puts /mingw64/bin on PATH." >&2
    fi
    exit 2
  fi
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
if [ "$MINUTES" = 0 ] || ! command -v timeout >/dev/null 2>&1; then
  if [ "$MINUTES" != 0 ]; then
    echo "(no \`timeout\` on PATH — running uncapped; Ctrl-C to stop)"
  fi
  "${CMD[@]}" 2>&1 | tee -a "$LOG"
  RC=${PIPESTATUS[0]}
  CAPPED=0
else
  timeout --signal=INT "${MINUTES}m" "${CMD[@]}" 2>&1 | tee -a "$LOG"
  RC=${PIPESTATUS[0]}   # 124 = the full window elapsed (normal for a capped run)
  CAPPED=1
fi

echo
if [ ! -s "$DIAG" ]; then
  echo "FAIL: the client exited (status $RC) without ever writing the diag CSV." >&2
  echo "  expected: $DIAG" >&2
  echo "It died before connecting — check the console log: $LOG" >&2
  echo "If the log is EMPTY the exe never started (Windows: missing DLLs — run from" >&2
  echo "the MSYS2 MinGW x64 shell; check: ldd $BIN | grep -i 'not found')." >&2
  echo "If the log HAS output: missing ROM/data, server unreachable, or a" >&2
  echo "NET_PROTOCOL_VER mismatch (DISCONNECT_VERSION in the log)." >&2
  exit 1
fi
if [ "$CAPPED" = 1 ]; then
  echo "soak window elapsed (exit status $RC). client-side verdict:"
  if [ -n "$PY" ]; then
    "$PY" "$HERE/tools/netsoak.py" "$DIAG"
  else
    echo "(python not found — run: py tools/netsoak.py $DIAG)"
  fi
  echo
  echo "for the full parity verdict, run with the matching server CSV:"
  echo "  tools/netsoak.py tools/soak/out/server_<stamp>.csv $DIAG"
fi
