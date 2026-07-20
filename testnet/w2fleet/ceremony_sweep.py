#!/usr/bin/env python3
"""SG-2b-0 cross-node ceremony sweep — the getstakingstatus-all-23 analog.

Greps the ceremony markers from all N GM debug.logs and aligns them into a
per-node phase TIMELINE + a group_pk IDENTITY check, so a failed 11-node
formation is located by checkpoint instead of read by hand across 11 logs.

Markers (all unconditional LogPrintf, so visible without a -debug flag):
  CP-2  "PTX formation: ceremony session STARTED ... my_idx=I (SG-2a driver)"
  CP-4  "PTX ceremony: phase A->B height=H my_idx=I qual=Q bad=D"   (driver)
  CP-5  "PTX formation: ceremony DONE ... group_pk=HEX ..."          (converge)
        "PTX formation: ceremony ABORTED ..."                        (fail-safe)

Verdicts:
  CONVERGED           — every started member reached DONE on ONE identical group_pk
  ★ PARTIAL-DIVERGENCE — >1 distinct group_pk among DONE nodes (the silent
                         failure the group_pk-in-DONE instrument exists to catch)
  STALL@<phase>       — a member never reached DONE/ABORTED; last phase named
  ABORTED             — members closed sub-threshold (report count; >=6 others
                         may still have CONVERGED — that is the fail-safe)

Usage: ceremony_sweep.py <N> [quorum_hash_prefix]
Read-only: docker exec grep only; no RPC, no lifecycle, no fleet mutation.
"""
import re
import subprocess
import sys
from collections import defaultdict

LOG = "/root/.hemis-ptxbea/ptxbea/debug.log"
CONTAINER = "ptx-w2-gm%02d"

RE_STARTED = re.compile(r"ceremony session STARTED quorum_hash=(\w+) formation_height=(\d+) my_idx=(-?\d+)")
RE_PHASE   = re.compile(r"PTX ceremony: phase (\w+)->(\w+) height=(\d+) my_idx=(-?\d+) qual=(\d+) bad=(\d+)")
RE_DONE    = re.compile(r"ceremony DONE quorum_hash=(\w+) group_pk=(\w+)")
RE_ABORTED = re.compile(r"ceremony ABORTED quorum_hash=(\w+)")


def node_lines(idx):
    try:
        out = subprocess.run(
            ["docker", "exec", CONTAINER % idx, "grep", "-hE",
             "PTX formation: ceremony (session STARTED|DONE|ABORTED)|PTX ceremony: phase", LOG],
            capture_output=True, text=True, timeout=30)
        return out.stdout.splitlines()
    except Exception as e:
        return ["__ERR__ %s" % e]


def main():
    n = int(sys.argv[1])
    qfilter = sys.argv[2] if len(sys.argv) > 2 else None

    started, phases, done, aborted, errors = {}, defaultdict(list), {}, {}, {}
    for i in range(1, n + 1):
        for ln in node_lines(i):
            if ln.startswith("__ERR__"):
                errors[i] = ln; continue
            m = RE_STARTED.search(ln)
            if m and (not qfilter or m.group(1).startswith(qfilter)):
                started[i] = (m.group(1), int(m.group(2)), int(m.group(3))); continue
            m = RE_PHASE.search(ln)
            if m:
                phases[i].append((m.group(1), m.group(2), int(m.group(3)),
                                  int(m.group(5)), int(m.group(6)))); continue
            m = RE_DONE.search(ln)
            if m and (not qfilter or m.group(1).startswith(qfilter)):
                done[i] = m.group(2); continue
            m = RE_ABORTED.search(ln)
            if m and (not qfilter or m.group(1).startswith(qfilter)):
                aborted[i] = True

    members = sorted(started)
    print("=== SG-2b-0 ceremony sweep: %d nodes queried, %d STARTED as members ===" %
          (n, len(members)))
    if errors:
        print("  UNREACHABLE:", ", ".join("gm%02d(%s)" % (i, errors[i]) for i in errors))

    # per-node timeline + stall locator
    stuck = {}
    for i in members:
        walk = "->".join(p[1] for p in phases[i]) if phases[i] else "(no transitions)"
        if i in done:
            state = "DONE gpk=%s.." % done[i][:16]
        elif i in aborted:
            state = "ABORTED"
        else:
            last = phases[i][-1][1] if phases[i] else started[i] and "HASH_COMMIT"
            state = "STALL@%s" % last
            stuck[i] = last
        print("  gm%02d my_idx=%d h0=%d  %s  => %s" %
              (i, started[i][2], started[i][1], walk, state))

    # convergence / partial-divergence verdict
    keys = defaultdict(list)
    for i, k in done.items():
        keys[k].append(i)
    print("--- CONVERGENCE:")
    if not done:
        print("  NO node reached DONE.")
    elif len(keys) == 1:
        k = next(iter(keys))
        print("  CONVERGED: %d/%d DONE on ONE group_pk=%s" % (len(done), len(members), k[:24] + ".."))
    else:
        print("  ★ PARTIAL-DIVERGENCE DETECTED — %d distinct group_pk among DONE nodes:" % len(keys))
        for k, ns in keys.items():
            print("      %s.. : gm%s" % (k[:24], ",".join("%02d" % i for i in ns)))
    if aborted:
        print("  ABORTED: gm%s (fail-safe; >=6 CONVERGED still valid)" %
              ",".join("%02d" % i for i in sorted(aborted)))
    if stuck:
        print("  ★ STALL: gm%s — locate the stuck phase above (windowed-advance/self-clock suspect)" %
              ",".join("%02d" % i for i in sorted(stuck)))

    # exit code: 0 converged-clean, 2 divergence/stall, 1 no-done
    if done and len(keys) == 1 and not stuck:
        sys.exit(0)
    sys.exit(1 if not done else 2)


if __name__ == "__main__":
    main()
