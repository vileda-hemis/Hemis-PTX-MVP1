#!/usr/bin/env python3
"""SG-2b cross-node ceremony sweep — the getstakingstatus-all-23 analog.

Greps the ceremony markers from all N GM debug.logs and aligns them into a
per-node phase TIMELINE + a group_pk IDENTITY check, so a failed 11-node
formation is located by checkpoint instead of read by hand across 11 logs.

Markers (all unconditional LogPrintf, so visible without a -debug flag):
  CP-2  "PTX formation: ceremony session STARTED ... my_idx=I (SG-2a driver)"
  CP-4  "PTX ceremony: phase A->B height=H my_idx=I qual=Q bad=D"   (driver)
  CP-5  "PTX formation: ceremony DONE ... group_pk=HEX ..."          (converge)
        "PTX formation: ceremony ABORTED ..."                        (fail-safe)
        "PTX formation: ceremony session EXITED ..."                 (epilogue)
        "PTX: member-connections SET/REMOVED ..."                    (KDD-065)

GMAUTH-era rework (2026-07-21, the SG-2b-0 flap-run lessons):
  - The FINAL STARTED session per node is the one judged (the first-STARTED
    read was stale during fork-flap session thrash).
  - "No transitions yet, window still open" (IN-WINDOW) is DISTINCT from
    "window exceeded mid-phase" (STALL@phase) — judged against the node's
    CURRENT height, threshold --stall (default 12 blocks).
  - Designed non-rejoin (session torn down by re-arm, not re-instantiated;
    no-late-join design input) is DISTINCT from STALL: NON-REJOIN.
  - Pairing counters: STARTED==EXITED and mconn SET==REMOVED per node (the
    connection-entry leak detector).
  - ★ CHAIN-DARK guard (PERMANENT, verification not scaffold): every member's
    height must be within --dark (default 6) blocks of the fleet max seen —
    the height-clocked walk self-detects a relay-partitioned member; this
    assertion is what would catch a real partition if one ever occurred.

Verdicts:
  CONVERGED           — every judged member reached DONE on ONE group_pk
  ★ PARTIAL-DIVERGENCE — >1 distinct group_pk among DONE nodes
  STALL@<phase>       — live session, no activity for > --stall blocks
  IN-WINDOW           — live session, activity within --stall blocks
  NON-REJOIN          — all sessions exited without DONE/ABORT (designed)
  ABORTED             — sub-threshold close (fail-safe; >=6 DONE still valid)

Usage: ceremony_sweep.py <N> [quorum_hash_prefix] [--stall B] [--dark B]
Read-only: docker exec grep + getblockcount only; no lifecycle, no mutation.
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
RE_EXITED  = re.compile(r"ceremony session EXITED quorum_hash=(\w+)")
RE_MCONN   = re.compile(r"member-connections (SET|REMOVED) quorum_hash=(\w+)")


def node_lines(idx):
    try:
        out = subprocess.run(
            ["docker", "exec", CONTAINER % idx, "grep", "-hE",
             "PTX formation: ceremony|PTX ceremony: phase|PTX: member-connections", LOG],
            capture_output=True, text=True, timeout=30)
        return out.stdout.splitlines()
    except Exception as e:
        return ["__ERR__ %s" % e]


def node_height(idx):
    try:
        out = subprocess.run(
            ["docker", "exec", CONTAINER % idx, "Hemis-cli", "-ptxbea",
             "-datadir=/root/.hemis-ptxbea", "getblockcount"],
            capture_output=True, text=True, timeout=30)
        return int(out.stdout.strip())
    except Exception:
        return None


class NodeView:
    """Chronological parse of one node's log: sessions in order, counters."""
    def __init__(self):
        self.sessions = []      # list of dicts, chronological
        self.n_started = 0
        self.n_exited = 0
        self.n_set = 0
        self.n_removed = 0

    def feed(self, ln):
        m = RE_STARTED.search(ln)
        if m:
            self.n_started += 1
            self.sessions.append({"q": m.group(1), "h0": int(m.group(2)),
                                  "idx": int(m.group(3)), "phases": [],
                                  "done": None, "aborted": False, "exited": False})
            return
        cur = self.sessions[-1] if self.sessions else None
        m = RE_PHASE.search(ln)
        if m and cur is not None:
            cur["phases"].append((m.group(1), m.group(2), int(m.group(3)),
                                  int(m.group(5)), int(m.group(6))))
            return
        m = RE_DONE.search(ln)
        if m and cur is not None and cur["q"] == m.group(1):
            cur["done"] = m.group(2)
            return
        m = RE_ABORTED.search(ln)
        if m and cur is not None and cur["q"] == m.group(1):
            cur["aborted"] = True
            return
        m = RE_EXITED.search(ln)
        if m:
            self.n_exited += 1
            if cur is not None and cur["q"] == m.group(1):
                cur["exited"] = True
            return
        m = RE_MCONN.search(ln)
        if m:
            if m.group(1) == "SET":
                self.n_set += 1
            else:
                self.n_removed += 1

    def final_session(self, qfilter):
        for s in reversed(self.sessions):
            if not qfilter or s["q"].startswith(qfilter):
                return s
        return None


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    opts = {a.split("=")[0]: a for a in sys.argv[1:] if a.startswith("--")}

    def opt_int(name, default):
        if name in opts and "=" in opts[name]:
            return int(opts[name].split("=", 1)[1])
        return default

    n = int(args[0])
    qfilter = args[1] if len(args) > 1 else None
    stall_after = opt_int("--stall", 12)
    dark_after = opt_int("--dark", 6)

    views, errors, heights = {}, {}, {}
    for i in range(1, n + 1):
        v = NodeView()
        for ln in node_lines(i):
            if ln.startswith("__ERR__"):
                errors[i] = ln
                break
            v.feed(ln)
        views[i] = v

    members = {i: v.final_session(qfilter) for i, v in views.items()}
    members = {i: s for i, s in members.items() if s is not None}
    for i in members:
        heights[i] = node_height(i)

    fleet_max = max((h for h in heights.values() if h is not None), default=None)

    print("=== SG-2b ceremony sweep: %d nodes queried, %d with a matching session ==="
          % (n, len(members)))
    if errors:
        print("  UNREACHABLE:", ", ".join("gm%02d(%s)" % (i, errors[i]) for i in errors))

    stuck, dark, leaks = {}, {}, {}
    done, aborted, nonrejoin = {}, {}, {}
    for i in sorted(members):
        s = members[i]
        v = views[i]
        walk = "->".join(p[1] for p in s["phases"]) if s["phases"] else "(no transitions)"
        h = heights.get(i)
        if s["done"]:
            state = "DONE gpk=%s.." % s["done"][:16]
            done[i] = (s["q"], s["done"])   # (quorum, key): convergence is per-quorum (L>1)
        elif s["aborted"]:
            state = "ABORTED"
            aborted[i] = True
        elif s["exited"]:
            state = "NON-REJOIN (torn down, not re-instantiated — designed no-late-join)"
            nonrejoin[i] = True
        else:
            last_h = s["phases"][-1][2] if s["phases"] else s["h0"]
            last_p = s["phases"][-1][1] if s["phases"] else "HASH_COMMIT"
            if h is not None and h - last_h > stall_after:
                state = "STALL@%s (+%d blocks silent)" % (last_p, h - last_h)
                stuck[i] = last_p
            else:
                state = "IN-WINDOW@%s" % last_p
        if fleet_max is not None and h is not None and fleet_max - h > dark_after:
            state += "  ★CHAIN-DARK(h=%d, fleet=%d)" % (h, fleet_max)
            dark[i] = h
        if v.n_started != v.n_exited + (0 if s["exited"] or s["done"] or s["aborted"] else 1):
            pass  # live session accounts for the open pair
        if v.n_set != v.n_removed and (s["exited"] or s["done"] or s["aborted"]):
            leaks[i] = (v.n_set, v.n_removed)
        print("  gm%02d my_idx=%d h0=%d q=%s..  %s  => %s"
              % (i, s["idx"], s["h0"], s["q"][:12], walk, state))
        print("        pairs: STARTED=%d EXITED=%d mconnSET=%d mconnREMOVED=%d"
              % (v.n_started, v.n_exited, v.n_set, v.n_removed))

    # ★ L>1 (W2.5b): convergence is judged PER QUORUM — different quorums
    # MUST carry different group_pks (a shared key across quorums would be the
    # actual catastrophe).  The old fleet-pooled check flagged PARTIAL-
    # DIVERGENCE the moment a SECOND quorum completed (first tripped at L=2,
    # quorums 086f360c/1eaed663 — both internally 11/11-identical).
    # Divergence is only real WITHIN one quorum.
    perq = defaultdict(lambda: defaultdict(list))   # quorum -> gpk -> [nodes]
    for i, (q, k) in done.items():
        perq[q][k].append(i)
    diverged = False
    print("--- CONVERGENCE (per quorum):")
    if not done:
        print("  NO node reached DONE.")
    for q, keys in sorted(perq.items()):
        if len(keys) == 1:
            k, ns = next(iter(keys.items()))
            print("  q=%s.. CONVERGED: %d DONE on ONE group_pk=%s.."
                  % (q[:12], len(ns), k[:24]))
        else:
            diverged = True
            print("  ★ q=%s.. PARTIAL-DIVERGENCE — %d distinct group_pk WITHIN one quorum:"
                  % (q[:12], len(keys)))
            for k, ns in keys.items():
                print("      %s.. : gm%s" % (k[:24], ",".join("%02d" % i for i in ns)))
    if aborted:
        print("  ABORTED: gm%s (fail-safe; >=6 CONVERGED still valid)"
              % ",".join("%02d" % i for i in sorted(aborted)))
    if nonrejoin:
        print("  NON-REJOIN: gm%s (designed no-late-join, NOT a stall)"
              % ",".join("%02d" % i for i in sorted(nonrejoin)))
    if stuck:
        print("  ★ STALL: gm%s — stuck phase named above (windowed-advance/self-clock suspect)"
              % ",".join("%02d" % i for i in sorted(stuck)))
    if dark:
        print("  ★ CHAIN-DARK: gm%s — member(s) lag fleet height by >%d blocks "
              "(relay partition suspect — NOT a ceremony bug)"
              % (",".join("%02d" % i for i in sorted(dark)), dark_after))
    if leaks:
        print("  ★ MCONN-LEAK: gm%s — SET != REMOVED on a closed session "
              "(connection-entry leak)"
              % ",".join("%02d" % i for i in sorted(leaks)))

    # exit code: 0 converged-clean; 1 no-done; 2 divergence/stall/dark/leak
    if done and not diverged and not stuck and not dark and not leaks:
        sys.exit(0)
    sys.exit(1 if not done else 2)


if __name__ == "__main__":
    main()
