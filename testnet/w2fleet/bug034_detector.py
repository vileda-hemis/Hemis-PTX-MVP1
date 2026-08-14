#!/usr/bin/env python3
"""BUG-034 settle detector — OPERATIONAL signal, not a permanent-loss counter.

Under the approved no-bound design the permanently-orphaned-valid-settle class
is EMPTY BY CONSTRUCTION (a valid settle is unminable only if its commitment is
reorged out, which undoes the fee too).  A permanent-loss counter would
therefore always read zero — a vacuous green.  What CAN go wrong operationally:

  * a valid settle sits PENDING in the mempool and doesn't mine (relay or
    assembler trouble — pre-relax this is exactly the BUG-034 halt shape;
    post-relax it should never exceed ~1 block),
  * the settle-after-commit delta drifts (the timing baseline Phase 2 must
    drive to "same or next block"),
  * the commit-mined-without-settle rate (includes LEGITIMATE abandons — the
    forfeiture path — so it is labelled as a mixed rate, not a loss count).

Emits:
  state JSON  (for the dashboard panel)   --state
  event JSONL (append-only, for analysis) --events
  ALERT log lines when a pending settle's age crosses --alert-blocks.

Read-only RPC observer.  No daemon change, no consensus interaction.
"""
import argparse, json, os, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from harness.cluster import W2Cluster

TYPE_SESS, TYPE_COALESCE, TYPE_COMMIT = 6, 9, 12


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--n", type=int, default=311)
    ap.add_argument("--compose", default="/mnt/pve/Node14TB/hemis-ptx/docker-w2r/docker-compose.generated.yml")
    ap.add_argument("--project", default="ptx-w2r")
    ap.add_argument("--port-base", type=int, default=32000)
    ap.add_argument("--subnet-base", default="172.32.0")
    ap.add_argument("--callers", type=int, default=8)
    ap.add_argument("--state", default="/mnt/pve/Node14TB/hemis-ptx/w2-fleet/bug034_state.json")
    ap.add_argument("--events", default="/mnt/pve/Node14TB/hemis-ptx/w2-fleet/bug034_events.jsonl")
    ap.add_argument("--interval", type=float, default=15.0)
    ap.add_argument("--alert-blocks", type=int, default=3,
                    help="pending-settle age (blocks) that raises the ALERT flag")
    ap.add_argument("--baseline-window", type=int, default=240,
                    help="trailing blocks for the commit-without-settle rate")
    a = ap.parse_args()

    c = W2Cluster(a.n, compose_file=a.compose, project=a.project,
                  port_base=a.port_base, subnet_base=a.subnet_base, callers=a.callers)
    node = c.gms[0]  # any majority node; read-only

    pending = {}            # settle txid -> {"first_tip": h, "first_wall": t, "parent": txid}
    deltas = []             # mined settle-after-commit deltas, in blocks (rolling)
    last_scanned = node.call("getblockcount")

    def emit(ev):
        with open(a.events, "a") as f:
            f.write(json.dumps(ev) + "\n")

    print("[bug034] detector up @ tip %d (alert at %d blocks pending)" % (last_scanned, a.alert_blocks), flush=True)
    while True:
        try:
            tip = node.call("getblockcount")

            # --- 1. pending settles in the mempool (parent already confirmed) ---
            mp = node.call("getrawmempool")
            mpset = set(mp)
            seen_now = set()
            for txid in mp:
                try:
                    j = node.call("getrawtransaction", txid, True)
                except Exception:
                    continue
                if j.get("type") != TYPE_SESS:
                    continue
                parent = j["vin"][0]["txid"]
                if parent in mpset:
                    continue  # same-mempool pair: normal in-flight roll
                try:
                    pj = node.call("getrawtransaction", parent, True)
                except Exception:
                    continue
                if pj.get("confirmations", 0) <= 0:
                    continue
                seen_now.add(txid)
                if txid not in pending:
                    pending[txid] = {"first_tip": tip, "first_wall": time.time(), "parent": parent}
                    emit({"ev": "pending_settle", "settle": txid, "parent": parent, "tip": tip, "t": time.time()})
            # resolve entries that left the mempool: mined (delta) or evicted
            for txid in list(pending.keys()):
                if txid in seen_now:
                    continue
                info = pending.pop(txid)
                try:
                    j = node.call("getrawtransaction", txid, True)
                    conf = j.get("confirmations", 0)
                except Exception:
                    conf = 0
                if conf > 0:
                    sh = tip - conf + 1
                    try:
                        pj = node.call("getrawtransaction", info["parent"], True)
                        ch = tip - pj.get("confirmations", 1) + 1
                    except Exception:
                        ch = sh
                    deltas.append(sh - ch)
                    deltas[:] = deltas[-500:]
                    emit({"ev": "pending_resolved_mined", "settle": txid, "delta_blocks": sh - ch,
                          "pending_blocks": tip - info["first_tip"], "t": time.time()})
                else:
                    # left the mempool UNMINED — the loud line the tolerance layers must never silence
                    print("[bug034] ALERT: pending settle %s left mempool UNMINED (evicted?) after %d blocks"
                          % (txid[:12], tip - info["first_tip"]), flush=True)
                    emit({"ev": "pending_evicted_unmined", "settle": txid, "parent": info["parent"],
                          "pending_blocks": tip - info["first_tip"], "t": time.time()})

            # --- 2. new blocks: mined commit/settle pairing + deltas ---
            commits_h, settles_parent = {}, {}
            lo = max(last_scanned + 1, tip - a.baseline_window)
            for h in range(lo, tip + 1):
                try:
                    blk = node.call("getblock", node.call("getblockhash", h), 2)
                except Exception:
                    continue
                for tx in blk["tx"]:
                    ty = tx.get("type", 0)
                    if ty == TYPE_COMMIT:
                        commits_h[tx["txid"]] = h
                    elif ty == TYPE_SESS:
                        settles_parent[tx["vin"][0]["txid"]] = h
            for ctx, ch in commits_h.items():
                if ctx in settles_parent:
                    deltas.append(settles_parent[ctx] - ch)
            deltas[:] = deltas[-500:]
            last_scanned = tip

            # --- 3. trailing baseline: commit-without-settle rate (MIXED: includes legit abandons).
            # Exact pairing from the block scan itself: a commit counts as settled iff a MINED
            # settle spends it (settle.vin[0] == commit txid).  NOT gettxout — that counts
            # wallet-reclaimed dust and mempool spends as "settled" (measured flaw, 2026-08-14).
            base_lo = max(1, tip - a.baseline_window)
            base_commits, base_settle_parents = set(), set()
            for h in range(base_lo, tip + 1):
                try:
                    blk = node.call("getblock", node.call("getblockhash", h), 2)
                except Exception:
                    continue
                for tx in blk["tx"]:
                    ty = tx.get("type", 0)
                    if ty == TYPE_COMMIT:
                        base_commits.add(tx["txid"])
                    elif ty == TYPE_SESS:
                        base_settle_parents.add(tx["vin"][0]["txid"])
            n_commits = len(base_commits)
            n_settled = len(base_commits & base_settle_parents)

            # --- 4. state out ---
            ages = sorted(tip - v["first_tip"] for v in pending.values())
            alert = bool(ages and ages[-1] >= a.alert_blocks)
            ds = sorted(deltas)
            state = {
                "tip": tip, "t": time.time(),
                "pending_count": len(pending),
                "pending_max_age_blocks": ages[-1] if ages else 0,
                "alert": alert,
                "delta_p50": ds[len(ds) // 2] if ds else None,
                "delta_p95": ds[min(len(ds) - 1, int(len(ds) * 0.95))] if ds else None,
                "delta_n": len(ds),
                "baseline_commits": n_commits,
                "baseline_settled": n_settled,
                "baseline_unsettled_rate_mixed": (round(1 - n_settled / n_commits, 3) if n_commits else None),
                "baseline_window": a.baseline_window,
            }
            tmp = a.state + ".tmp"
            with open(tmp, "w") as f:
                json.dump(state, f)
            os.replace(tmp, a.state)
            if alert:
                print("[bug034] ALERT: %d pending settle(s), oldest %d blocks (>= %d)"
                      % (len(pending), ages[-1], a.alert_blocks), flush=True)
        except Exception as e:
            print("[bug034] loop error: %s" % str(e)[:120], flush=True)
        time.sleep(a.interval)


if __name__ == "__main__":
    main()
