#!/usr/bin/env python3
"""W2.5b quorum roster watch — prints each forming quorum's selected 11 as it
forms (anchor height, the 11 GMs, share_index order).

Trigger: the unconditional "PTX formation: ceremony session STARTED
quorum_hash=…" log line (any node; the quorum_hash IS the anchor block hash).
Per NEW anchor seen, the roster is fetched via `ptx_debug_selectquorum
<anchor_hash>` on a query node — the same canonical selection formation and
V5 run (KDD-060 one-function contract), so what prints is what the chain will
validate.  NOTE: for a ROTATION ceremony the live members are the
predecessor's 11 (same-set re-DKG), not a fresh draw — rotations are marked
from the "PTX formation: ROTATION of" line when present.

On-demand equivalent for any boundary height H:
  CLI="docker exec ptx-w2-gm01 Hemis-cli -ptxbea -rpcuser=... -rpcpassword=..."
  $CLI ptx_debug_selectquorum $($CLI getblockhash H)

Usage:
  python3 quorum_roster.py [--query 127.0.0.1:31001] [--datadir-root …] [--poll 10]
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

STARTED_RE = re.compile(r"ceremony session STARTED.*quorum_hash=([0-9a-f]{64})")
ROTATION_RE = re.compile(r"PTX formation: ROTATION of ([0-9a-f]{64}) at anchor ([0-9a-f]{64})")


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--query", default="127.0.0.1:31001",
                    help="host:port of the query node (default gm01)")
    ap.add_argument("--rpc-user", default=DEF_RPC_USER)
    ap.add_argument("--rpc-pass", default=DEF_RPC_PASS)
    ap.add_argument("--datadir-root", default=DEF_DATA_ROOT)
    ap.add_argument("--poll", type=float, default=10.0)
    ap.add_argument("--from-start", action="store_true",
                    help="scan logs from the beginning (default: from EOF, live-forward)")
    return ap.parse_args()


def main():
    args = parse_args()
    host, port = args.query.rsplit(":", 1)
    q = Node("query", host, int(port), args.rpc_user, args.rpc_pass)

    paths = sorted(glob.glob(os.path.join(args.datadir_root, "*", "ptxbea", "debug.log")))
    offsets = {}
    for p in paths:
        try:
            offsets[p] = 0 if args.from_start else os.path.getsize(p)
        except OSError:
            pass

    seen = set()          # anchors already printed
    rotations = {}        # anchor_hash -> predecessor_hash
    print(f"[roster] watching {len(paths)} logs; query node {args.query}; "
          f"{'from start' if args.from_start else 'live-forward'}")

    while True:
        for p in sorted(glob.glob(os.path.join(args.datadir_root, "*", "ptxbea", "debug.log"))):
            try:
                size = os.path.getsize(p)
                off = offsets.get(p, 0)
                if size < off:
                    off = 0
                if size == off:
                    continue
                with open(p, "r", errors="replace") as f:
                    f.seek(off)
                    for line in f:
                        rm = ROTATION_RE.search(line)
                        if rm:
                            rotations[rm.group(2)] = rm.group(1)
                        m = STARTED_RE.search(line)
                        if not m:
                            continue
                        anchor = m.group(1)
                        if anchor in seen:
                            continue
                        seen.add(anchor)
                        try:
                            sel = q.call("ptx_debug_selectquorum", anchor)
                            height = sel.get("height", "?")
                            members = sel.get("selected", [])
                            kind = ("ROTATION of " + rotations[anchor][:16] + "…"
                                    if anchor in rotations else "FRESH")
                            print(f"\n★ QUORUM FORMING #{len(seen)} @ h{height} "
                                  f"({kind})  anchor={anchor[:16]}…")
                            for mem in sorted(members, key=lambda x: x.get("share_index", 0)):
                                print(f"   {mem.get('share_index', '?'):>2}. "
                                      f"{mem.get('node_id', '?')}")
                            if anchor in rotations:
                                print("   (rotation: live members = the predecessor's 11, "
                                      "same-set re-DKG — the fresh draw above is informational)")
                        except Exception as e:  # noqa: BLE001 — keep watching
                            print(f"\n★ QUORUM FORMING @ anchor={anchor[:16]}… "
                                  f"(roster query failed: {str(e)[:100]})")
                    offsets[p] = f.tell()
            except OSError:
                continue
        time.sleep(args.poll)


if __name__ == "__main__":
    main()
