#!/usr/bin/env python3
"""
netsoak.py — offline verdict for a netplay prop-sync soak session.

Phase 2 of the prop-sync consistency plan (docs/PORT_NET_PROP_LIFECYCLE.md,
docs/PORT_NET_SOAK.md). The in-engine auditor (netprop.c, /audit) writes one
`audit:` line per second to the diagnostic CSV (Net.Debug.LogPath / --netdiag).
This tool ingests one or two such logs and prints a PASS/FAIL verdict plus the
detail behind it, so a long unattended run reduces to one exit code.

Usage:
    netsoak.py SERVER.csv [CLIENT.csv ...]
    netsoak.py --help

With one log: checks that machine's own invariants (no FAIL cycles, no standing
corruption, no heal/reap/orphan fires, slot occupancy bounded).

With a server log + one or more client logs: ALSO checks manifest parity — the
client's networked-prop set digest must match a recent server digest (within a
tolerance window, since the reliable channel + interp delay the client by a
bounded number of ticks). A sustained mismatch is the "client and host disagree
on which props exist" failure the whole sync layer exists to prevent.

Exit code 0 = PASS, 1 = FAIL, 2 = usage / no audit lines found.

The diag line format (netDiagLogf): `tick,realtime_s,event,k=v k=v ...`
The audit event fields:
    role=S|C result=PASS|FAIL netprops=N manifest=0xXXXXXXXX dupes=D corpses=C
    overcap=O slots_occ=.. synced=.. local=.. proj=.. projdead=.. orphan=..
    heal=H reap=R orphreap=OR
"""

import sys
import os


def parse_log(path):
    """Return list of audit dicts (with int 'tick') from a diag CSV."""
    audits = []
    other_counts = {}  # event -> count, for context (orphan_reap, propsheal lines)
    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split(",", 3)
            if len(parts) < 3:
                continue
            tick_s, _rt, event = parts[0], parts[1], parts[2]
            fields = parts[3] if len(parts) > 3 else ""
            if event != "audit":
                other_counts[event] = other_counts.get(event, 0) + 1
                continue
            d = {"tick": _safe_int(tick_s)}
            for tok in fields.split():
                if "=" not in tok:
                    continue
                k, v = tok.split("=", 1)
                d[k] = v
            audits.append(d)
    return audits, other_counts


def _safe_int(s, base=10):
    try:
        return int(s, base)
    except (ValueError, TypeError):
        return 0


def _ai(d, k):
    """audit-field as int (handles 0x.. for manifest)."""
    v = d.get(k, "0")
    if isinstance(v, str) and v.startswith("0x"):
        return _safe_int(v, 16)
    return _safe_int(v)


def summarize(path, audits, other):
    """Single-log invariant check. Returns (ok, lines)."""
    out = []
    if not audits:
        out.append("  no `audit:` lines found — was /audit on and a diag log open?")
        return False, out

    role = audits[0].get("role", "?")
    fail_cycles = sum(1 for a in audits if a.get("result") != "PASS")
    dupes = sum(_ai(a, "dupes") for a in audits)
    corpses = sum(_ai(a, "corpses") for a in audits)
    orphan = sum(_ai(a, "orphan") for a in audits)
    overcap = sum(_ai(a, "overcap") for a in audits)
    heal = sum(_ai(a, "heal") for a in audits)
    reap = sum(_ai(a, "reap") for a in audits)
    orphreap = sum(_ai(a, "orphreap") for a in audits)
    occ_max = max((_ai(a, "slots_occ") for a in audits), default=0)
    slots_max = max((_ai(a, "synced") for a in audits), default=0)  # 'synced' field carries g_MaxWeaponSlots
    netprops_max = max((_ai(a, "netprops") for a in audits), default=0)
    span = (audits[-1]["tick"] - audits[0]["tick"]) if len(audits) > 1 else 0

    ok = (fail_cycles == 0 and dupes == 0 and corpses == 0
          and orphan == 0 and overcap == 0
          and heal == 0 and reap == 0 and orphreap == 0)

    out.append("  role=%s  cycles=%d  span=%d ticks (~%.1f min)"
               % (role, len(audits), span, span / 3600.0))
    out.append("  FAIL cycles:      %d" % fail_cycles)
    out.append("  standing corrupt: dupes=%d corpses=%d orphan=%d overcap=%d"
               % (dupes, corpses, orphan, overcap))
    out.append("  heal-layer fires: heal=%d reap=%d orphreap=%d" % (heal, reap, orphreap))
    out.append("  weapon slots:     peak occ=%d / %d   peak netprops=%d"
               % (occ_max, slots_max, netprops_max))
    # Context from non-audit tripwire lines if present.
    for ev in ("orphan_reap", "weaponslots"):
        if ev in other:
            out.append("  (diag had %d %s line(s))" % (other[ev], ev))
    return ok, out


def manifest_parity(server, clients, window=180):
    """
    For each client audit at tick T, the client's manifest digest should equal
    the server's manifest at some server tick in [T-window, T] (clients lag the
    host by the reliable-channel + interp delay; window default 180 ticks = 3s,
    generous). Report the fraction of client cycles that found a match.

    Returns (ok, lines). A run is parity-OK if >= 95% of client cycles match
    (the occasional miss is a normal in-flight spawn/free straddling the sample).
    """
    out = []
    # Build server (tick -> manifest) and a sorted tick list.
    srv = [(a["tick"], _ai(a, "manifest"), _ai(a, "netprops")) for a in server]
    srv.sort()
    srv_ticks = [t for (t, _m, _n) in srv]

    import bisect
    overall_ok = True
    for cpath, caudits in clients:
        if not caudits:
            out.append("  %s: no audit lines" % os.path.basename(cpath))
            overall_ok = False
            continue
        matched = 0
        checked = 0
        worst = None
        for a in caudits:
            t = a["tick"]
            cm = _ai(a, "manifest")
            checked += 1
            # candidate server samples in [t-window, t]
            lo = bisect.bisect_left(srv_ticks, t - window)
            hi = bisect.bisect_right(srv_ticks, t)
            found = any(srv[i][1] == cm for i in range(lo, hi))
            if found:
                matched += 1
            elif worst is None:
                # record first divergence for the report
                near = srv[hi - 1] if hi > 0 else (0, 0, 0)
                worst = (t, cm, near)
        frac = matched / checked if checked else 0.0
        cok = frac >= 0.95
        overall_ok = overall_ok and cok
        out.append("  %s: manifest parity %.1f%% (%d/%d cycles)%s"
                   % (os.path.basename(cpath), frac * 100, matched, checked,
                      "" if cok else "  <-- BELOW 95%"))
        if worst:
            t, cm, near = worst
            out.append("    first divergence @client tick %d: client=0x%08x "
                       "nearest server(tick %d)=0x%08x netprops c?/s=%d"
                       % (t, cm, near[0], near[1], near[2]))
    return overall_ok, out


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("-")]
    if not args or "--help" in argv or "-h" in argv:
        print(__doc__)
        return 2

    logs = []
    for p in args:
        if not os.path.exists(p):
            print("error: no such file: %s" % p)
            return 2
        audits, other = parse_log(p)
        logs.append((p, audits, other))

    print("=" * 70)
    print("netsoak verdict")
    print("=" * 70)

    all_ok = True
    parsed = []
    for path, audits, other in logs:
        role = audits[0].get("role", "?") if audits else "?"
        print("\n[%s]  role=%s" % (path, role))
        ok, lines = summarize(path, audits, other)
        print("\n".join(lines))
        print("  --> %s" % ("PASS" if ok else "FAIL"))
        all_ok = all_ok and ok
        parsed.append((path, audits, role))

    # Manifest parity if we have exactly one server + >=1 client.
    servers = [(p, a) for (p, a, r) in parsed if r == "S"]
    clients = [(p, a) for (p, a, r) in parsed if r == "C"]
    if servers and clients:
        print("\n" + "-" * 70)
        print("manifest parity (client set must match a recent server set)")
        print("-" * 70)
        sok, slines = manifest_parity(servers[0][1], clients)
        print("\n".join(slines))
        print("  --> %s" % ("PASS" if sok else "FAIL"))
        all_ok = all_ok and sok
    elif len(parsed) > 1:
        print("\n(parity check skipped: need exactly one server[role=S] + "
              ">=1 client[role=C] log)")

    print("\n" + "=" * 70)
    print("OVERALL: %s" % ("PASS" if all_ok else "FAIL"))
    print("=" * 70)
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
