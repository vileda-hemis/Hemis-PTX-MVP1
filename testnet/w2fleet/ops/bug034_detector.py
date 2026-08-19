#!/usr/bin/env python3
"""BUG-034 P1 divergence detector — pending-settle watch (reimplemented
2026-08-16; the original lived in a dead session's scratch and was lost with
the crash — banked as a FILE this time).

Semantics (BUG-034 P1 register): a PTXROLLCOMMIT (type 12) must be resolved by
either a PTXSETTLE (type 6) spending it (delivery) or a PTXCOALESCE (type 9)
spending it (recorded forfeit).  A commit that stays UNRESOLVED is the
divergence signal the 2-layer fix converted from LOUD halt to SILENT drop —
this detector is the mandated loud channel.  ALERT at pending age >= 3 blocks.

Writes the state JSON the dashboard's detector panel reads (one writer, one
schema): tip, t, pending_count, pending_max_age_blocks, alert, delta_p50/p95/n
(commit->settle block deltas over the window), baseline_commits/settled,
baseline_unsettled_rate_mixed, baseline_window.
"""
import json, subprocess, sys, time, statistics

NODE = "ptx-w2r-caller1"
CLI = ["Hemis-cli", "-ptxbea", "-rpcuser=ptxw2rpc", "-rpcpassword=ptxw2pass2026"]
STATE = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/bug034_state_w2r153.json"
WINDOW = 240
ALERT_AGE = 3
TICK = 30

def rpc(method, *params):
    out = subprocess.run(["docker", "exec", NODE] + CLI + [method] + [str(p) for p in params],
                         capture_output=True, text=True, timeout=30)
    if out.returncode != 0:
        raise RuntimeError(out.stderr.strip()[:200])
    try:
        return json.loads(out.stdout)
    except json.JSONDecodeError:
        return out.stdout.strip()   # bare-string results (e.g. getblockhash)

block_cache = {}   # height -> (hash, [(txid, type, [vin prevout hashes])])

def block_at(h):
    bh = rpc("getblockhash", h)
    cached = block_cache.get(h)
    if cached and cached[0] == bh:
        return cached[1]
    b = rpc("getblock", bh, 2)
    txs = [(t["txid"], t.get("type", 0),
            [v.get("txid") for v in t.get("vin", []) if v.get("txid")])
           for t in b["tx"]]
    block_cache[h] = (bh, txs)
    for k in [k for k in block_cache if k < h - WINDOW - 10]:
        del block_cache[k]
    return txs

def scan(tip):
    lo = max(1, tip - WINDOW + 1)
    commits = {}          # txid -> height
    resolved = {}         # commit txid -> (kind, height)
    for h in range(lo, tip + 1):
        for txid, ty, vins in block_at(h):
            if ty == 12:
                commits[txid] = h
            elif ty in (6, 9):
                kind = "settle" if ty == 6 else "coalesce"
                for parent in vins:
                    # Settle-priority: a coalesce legitimately sweeps the FEE
                    # output of a commit whose POT output a settle delivers
                    # (observed at h1028: both spend the same commit txid,
                    # different vouts) — txid-level matching must never let
                    # the sweep overwrite the delivery.
                    if kind == "settle" or parent not in resolved:
                        resolved[parent] = (kind, h)
    deltas = [rh - commits[c] for c, (k, rh) in resolved.items()
              if k == "settle" and c in commits]
    pending = {c: h for c, h in commits.items() if c not in resolved}
    settled = sum(1 for c in commits if resolved.get(c, ("", 0))[0] == "settle")
    max_age = max((tip - h for h in pending.values()), default=0)
    return {
        "tip": tip, "t": time.time(),
        "pending_count": len(pending),
        "pending_max_age_blocks": max_age,
        "alert": max_age >= ALERT_AGE,
        "delta_p50": int(statistics.median(deltas)) if deltas else 0,
        "delta_p95": int(sorted(deltas)[max(0, int(len(deltas) * 0.95) - 1)]) if deltas else 0,
        "delta_n": len(deltas),
        "baseline_commits": len(commits),
        "baseline_settled": settled,
        "baseline_unsettled_rate_mixed":
            round(1 - settled / len(commits), 4) if commits else None,
        "baseline_window": WINDOW,
    }

def main():
    print("[bug034] detector up (alert at %d blocks pending)" % ALERT_AGE, flush=True)
    last_alert = False
    while True:
        try:
            tip = rpc("getblockcount")
            st = scan(tip)
            with open(STATE + ".tmp", "w") as f:
                json.dump(st, f)
            import os
            os.replace(STATE + ".tmp", STATE)
            if st["alert"] and not last_alert:
                print("[bug034] ALERT: %d pending settle(s), max age %d blocks @ tip %d"
                      % (st["pending_count"], st["pending_max_age_blocks"], tip), flush=True)
            last_alert = st["alert"]
        except Exception as e:
            print("[bug034] tick error: %s" % e, flush=True)
        time.sleep(TICK)

if __name__ == "__main__":
    main()
