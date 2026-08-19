#!/usr/bin/env python3
"""One-line fleet capacity summary for the turnover watch (BUG-039 aftermath).

Prints: dead=<quorums capable<6> active=<total> worst=<min capable> newest=<mined_h>
Aggregated from ptx_quorum_health across all GMs (member+share_current), the
same figures the dashboard capacity panel renders.
"""
import json, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
from collections import defaultdict

CLI = ["Hemis-cli", "-ptxbea", "-rpcuser=ptxw2rpc", "-rpcpassword=ptxw2pass2026"]

def one(i):
    c = f"ptx-w2r-gm{i:02d}"
    try:
        p = subprocess.run(["docker", "exec", c] + CLI + ["ptx_quorum_health"],
                           capture_output=True, text=True, timeout=30)
        return json.loads(p.stdout) if p.returncode == 0 else None
    except Exception:
        return None

members = defaultdict(int); capable = defaultdict(int); mined = {}
with ThreadPoolExecutor(max_workers=24) as ex:
    for o in ex.map(one, range(1, 154)):
        if not o:
            continue
        for q in o["quorums"]:
            if not q["member"]:
                continue
            h = q["quorum_hash"]
            mined[h] = q["mined_height"]
            members[h] += 1
            if q["share_current"]:
                capable[h] += 1

if not mined:
    print("dead=? active=0 worst=? newest=? (no reports)")
    sys.exit(0)
dead = sum(1 for h in mined if capable[h] < 6)
worst = min(capable[h] for h in mined)
print(f"dead={dead} active={len(mined)} worst={worst} newest={max(mined.values())}")
