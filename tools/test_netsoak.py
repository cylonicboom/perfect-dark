#!/usr/bin/env python3
"""
Self-test for tools/netsoak.py on synthetic diag logs. No game or ROM needed.

Run:  python3 tools/test_netsoak.py     (exit 0 = all cases pass)

Covers: a clean PASS run; heal-fire and standing-corruption FAILs; the
proto-86 ghost-twin detector (transient floorlocal PASSes, persistent
floorlocal FAILs); manifest parity match and mismatch; and old logs without
the floorlocal field (backwards compat).
"""
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import netsoak


def audit_line(tick, role, result="PASS", manifest=0, netprops=0, dupes=0,
               corpses=0, overcap=0, occ=0, slots=50, proj=0, projdead=0,
               orphan=0, heal=0, reap=0, orphreap=0, floorlocal=None):
    fields = ("role=%s result=%s netprops=%d manifest=0x%08x dupes=%d "
              "corpses=%d overcap=%d slots_occ=%d synced=%d local=%d proj=%d "
              "projdead=%d orphan=%d heal=%d reap=%d orphreap=%d"
              % (role, result, netprops, manifest, dupes, corpses, overcap,
                 occ, slots, occ, proj, projdead, orphan, heal, reap, orphreap))
    if floorlocal is not None:
        fields += " floorlocal=%d" % floorlocal
    return "%d,%.2f,audit,%s" % (tick, tick / 60.0, fields)


def write_log(lines):
    f = tempfile.NamedTemporaryFile("w", suffix=".csv", delete=False)
    f.write("\n".join(lines) + "\n")
    f.close()
    return f.name


def check(name, cond):
    status = "ok" if cond else "FAIL"
    print("  %-52s %s" % (name, status))
    return cond


def main():
    ok = True
    print("netsoak.py self-test:")

    # 1. Clean run passes.
    log = write_log([audit_line(t, "C", floorlocal=0) for t in range(60, 660, 60)])
    audits, other = netsoak.parse_log(log)
    r, _ = netsoak.summarize(log, audits, other)
    ok &= check("clean client run -> PASS", r is True)

    # 2. A heal fire fails the run (masked corruption is still a finding).
    log = write_log([audit_line(60, "S"), audit_line(120, "S", result="FAIL", heal=1)])
    audits, other = netsoak.parse_log(log)
    r, _ = netsoak.summarize(log, audits, other)
    ok &= check("heal fire -> FAIL", r is False)

    # 3. Standing corruption fails.
    log = write_log([audit_line(60, "S", result="FAIL", corpses=2)])
    audits, other = netsoak.parse_log(log)
    r, _ = netsoak.summarize(log, audits, other)
    ok &= check("standing corpse -> FAIL", r is False)

    # 4. Transient floorlocal (one twin for a cycle mid-drop) still passes.
    vals = [0, 0, 1, 0, 0, 0, 0, 0, 0, 0]
    log = write_log([audit_line(60 * (i + 1), "C", floorlocal=v)
                     for i, v in enumerate(vals)])
    audits, other = netsoak.parse_log(log)
    r, _ = netsoak.summarize(log, audits, other)
    ok &= check("transient floorlocal -> PASS", r is True)

    # 5. Persistent floorlocal (final 5 cycles all nonzero) fails: a
    #    corpse-drop path escaped the proto-86 twin suppression.
    vals = [0, 0, 1, 1, 2, 2, 3, 3, 3, 3]
    log = write_log([audit_line(60 * (i + 1), "C", floorlocal=v)
                     for i, v in enumerate(vals)])
    audits, other = netsoak.parse_log(log)
    r, out = netsoak.summarize(log, audits, other)
    ok &= check("persistent floorlocal -> FAIL", r is False)
    ok &= check("persistent floorlocal reported", any("PERSISTENT" in l for l in out))

    # 6. Old logs without the floorlocal field still parse and pass.
    log = write_log([audit_line(t, "C") for t in range(60, 660, 60)])
    audits, other = netsoak.parse_log(log)
    r, _ = netsoak.summarize(log, audits, other)
    ok &= check("pre-floorlocal log (backwards compat) -> PASS", r is True)

    # 7. Manifest parity: matching digests pass, diverged digests fail.
    srv = [audit_line(t, "S", manifest=0xAB, netprops=3) for t in range(60, 660, 60)]
    cli = [audit_line(t + 30, "C", manifest=0xAB, netprops=3) for t in range(60, 660, 60)]
    sa, _ = netsoak.parse_log(write_log(srv))
    ca, _ = netsoak.parse_log(write_log(cli))
    r, _ = netsoak.manifest_parity(sa, [("cli.csv", ca)])
    ok &= check("manifest parity (matching) -> PASS", r is True)

    cli_bad = [audit_line(t + 30, "C", manifest=0xCD, netprops=3) for t in range(60, 660, 60)]
    ca, _ = netsoak.parse_log(write_log(cli_bad))
    r, _ = netsoak.manifest_parity(sa, [("cli.csv", ca)])
    ok &= check("manifest parity (diverged) -> FAIL", r is False)

    print("OVERALL: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
