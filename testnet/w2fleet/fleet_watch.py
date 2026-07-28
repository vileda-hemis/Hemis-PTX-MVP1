#!/usr/bin/env python3
"""W2.5b fleet watch — the live aggregate view (a MONITOR, not an asserter;
validate_fleet.py owns gate-assertions — this is the rolling human watch).

DESIGN — the log-vs-RPC split (ODC-043): quorum STATE comes from RPC
(ptx_quorum_list at the fleet's best height); lifecycle EVENTS ride the LOGS,
because ptx_quorum_info does not surface the record-v2 state heights
(reformed/superseded/disbanded — the ODC-043 observability gap).  Every event
marker below is an UNCONDITIONAL LogPrintf (visible at the fleet's default log
level, no -debug flag), and the datadirs are host-side bind mounts, so the
whole fleet is greppable without SSH.  ★ If ODC-043 is ever fixed (record-v2
heights surfaced over RPC), events can move to RPC and the offset-tailing here
retires.

Surfaced live, per tick:
  HEALTH   height min/max/spread across all nodes, laggards (> --lag behind),
           unreachable nodes, ACTIVE quorum count vs --expect-quorums.
  STATE    per-state quorum counts (RPC ptx_quorum_list at best height).
  EVENTS   new-this-tick + cumulative, from the fleet's debug.logs:
             GUARD2   "fairness floor overrides"       ★ THE B-vs-A signal
             REFORM   "quorum REFORMED at height"        (KDD-074/076 producer)
             YIELD    "rotation YIELDED (KDD-075)"
             ROTATE   "PTX formation: ROTATION of"
             FORM     "ceremony session STARTED" / "ceremony DONE" / "ABORTED"
  ROLLS    per-quorum roll counts from the CALLER logs
           ("PTX roll: DKG signing material — quorum_hash=…") — the live
           §7.4 routing-distribution check, independent of the demand
           driver's own from-returns count.

Usage:
  python3 fleet_watch.py --n 98 --callers 127.0.0.1:31000,127.0.0.1:31099 \
      [--datadir-root /mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs] \
      [--interval 30] [--expect-quorums 8]
"""

import argparse
import glob
import os
import re
import time

from harness.node import Node

DEF_RPC_USER = "ptxw2rpc"
DEF_RPC_PASS = "ptxw2pass2026"
DEF_DATA_ROOT = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs"

EVENT_PATTERNS = [
    ("GUARD2", re.compile(r"fairness floor overrides")),
    ("REFORM", re.compile(r"quorum REFORMED at height (\d+)")),
    ("YIELD",  re.compile(r"rotation YIELDED \(KDD-075\)")),
    ("ROTATE", re.compile(r"PTX formation: ROTATION of ([0-9a-f]{16})")),
    ("FORM+",  re.compile(r"ceremony session STARTED")),
    ("DONE",   re.compile(r"ceremony DONE")),
    ("ABORT",  re.compile(r"ceremony ABORTED")),
]
ROLL_RE = re.compile(r"PTX roll: DKG signing material — quorum_hash=([0-9a-f]+)")


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, required=True, help="GM count")
    ap.add_argument("--callers", required=True,
                    help="comma-separated caller host:port endpoints")
    ap.add_argument("--rpc-user", default=DEF_RPC_USER)
    ap.add_argument("--rpc-pass", default=DEF_RPC_PASS)
    ap.add_argument("--port-base", type=int, default=31000,
                    help="gm01 = port-base+1 … (gen_fleet layout)")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--datadir-root", default=DEF_DATA_ROOT)
    ap.add_argument("--interval", type=float, default=30.0)
    ap.add_argument("--expect-quorums", type=int, default=8)
    ap.add_argument("--lag", type=int, default=6,
                    help="blocks behind fleet-max that counts as a laggard")
    return ap.parse_args()


class LogTail:
    """Byte-offset tailer over the host-mounted debug.logs (no SSH, no docker)."""

    def __init__(self, root):
        self.root = root
        self.offsets = {}   # path -> byte offset (start at current EOF: watch is live-forward)
        for path in self._paths():
            try:
                self.offsets[path] = os.path.getsize(path)
            except OSError:
                pass

    def _paths(self):
        return sorted(glob.glob(os.path.join(self.root, "*", "ptxbea", "debug.log")))

    def new_lines(self):
        for path in self._paths():
            try:
                size = os.path.getsize(path)
                off = self.offsets.get(path, 0)
                if size < off:          # rotated/truncated
                    off = 0
                if size == off:
                    continue
                with open(path, "r", errors="replace") as f:
                    f.seek(off)
                    for line in f:
                        yield os.path.basename(os.path.dirname(os.path.dirname(path))), line
                    self.offsets[path] = f.tell()
            except OSError:
                continue


def main():
    args = parse_args()
    callers = []
    for i, ep in enumerate(e.strip() for e in args.callers.split(",") if e.strip()):
        host, port = ep.rsplit(":", 1)
        callers.append(Node(f"caller{i + 1}", host, int(port), args.rpc_user, args.rpc_pass))
    gms = [Node(f"gm{i:02d}", args.host, args.port_base + i, args.rpc_user, args.rpc_pass)
           for i in range(1, args.n + 1)]
    nodes = callers + gms

    tail = LogTail(args.datadir_root)
    cum = {k: 0 for k, _ in EVENT_PATTERNS}
    rolls_per_quorum = {}
    tick = 0
    print(f"[watch] {len(nodes)} nodes, logs under {args.datadir_root}, "
          f"interval {args.interval}s — MONITOR only (assertions live in validate_fleet.py)")

    while True:
        tick += 1
        # ── RPC: heights + quorum state ──────────────────────────────────
        heights, dead = {}, []
        for n in nodes:
            try:
                heights[n.name] = n.getblockcount()
            except Exception:
                dead.append(n.name)
        hmax = max(heights.values()) if heights else 0
        hmin = min(heights.values()) if heights else 0
        laggards = [k for k, v in heights.items() if hmax - v > args.lag]

        states = {}
        for n in nodes:            # first reachable node answers
            try:
                ql = n.call("ptx_quorum_list", hmax)
                items = ql if isinstance(ql, list) else ql.get("quorums", [])
                for q in items:
                    # rpc shape: state == "active" | "state(N)" (ptx_quorum_list)
                    st = (q.get("state", "?") if isinstance(q, dict) else "?").upper()
                    states[st] = states.get(st, 0) + 1
                break
            except Exception:
                continue

        # ── LOGS: events + roll routing (new since last tick) ────────────
        new = {k: [] for k, _ in EVENT_PATTERNS}
        for node_name, line in tail.new_lines():
            for key, pat in EVENT_PATTERNS:
                if pat.search(line):
                    cum[key] += 1
                    if len(new[key]) < 6:      # cap the per-tick detail lines
                        new[key].append(f"{node_name}: {line.strip()[:150]}")
            m = ROLL_RE.search(line)
            if m:
                qh = m.group(1)[:16]
                rolls_per_quorum[qh] = rolls_per_quorum.get(qh, 0) + 1

        # ── render ───────────────────────────────────────────────────────
        active = states.get("ACTIVE", states.get("1", 0))
        qflag = "" if active == args.expect_quorums else f"  ★ EXPECTED {args.expect_quorums}"
        print(f"\n══ tick {tick} @ {time.strftime('%H:%M:%S')} "
              f"═ h {hmin}..{hmax} (spread {hmax - hmin}) "
              f"═ ACTIVE quorums {active}{qflag} ═ states {states or '{}'}")
        if dead:
            print(f"  ★ UNREACHABLE ({len(dead)}): {', '.join(dead[:8])}"
                  + (" …" if len(dead) > 8 else ""))
        if laggards:
            print(f"  ★ LAGGARDS >{args.lag} behind: {', '.join(laggards[:8])}"
                  + (" …" if len(laggards) > 8 else ""))
        ev_line = "  events: " + "  ".join(
            f"{k}={cum[k]}" + (f"(+{len([1 for _ in new[k]]) if new[k] else 0})" if new[k] else "")
            for k, _ in EVENT_PATTERNS)
        print(ev_line)
        for key in ("GUARD2", "REFORM", "ABORT"):   # the ones worth line-detail
            for detail in new[key]:
                print(f"    [{key}] {detail}")
        if rolls_per_quorum:
            total = sum(rolls_per_quorum.values())
            dist = "  ".join(f"{q}…:{c}" for q, c in
                             sorted(rolls_per_quorum.items(), key=lambda kv: -kv[1])[:10])
            print(f"  rolls/quorum (§7.4, from caller logs, {total} total): {dist}")

        time.sleep(args.interval)


if __name__ == "__main__":
    main()
