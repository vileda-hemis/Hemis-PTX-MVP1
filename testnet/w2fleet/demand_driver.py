#!/usr/bin/env python3
"""W2.5b demand driver — sustained multi-caller roll generation (the run-validity
precondition).

WHY THE PACING (load-bearing, ODC-052): the idle arm reads absence-of-rolls as
idleness, and at L=8 with nRetireWindow=200 the sparsity floor is one roll per
25 blocks fleet-wide.  This driver paces WELL ABOVE that floor — default one
roll per 2-5 blocks (uniform gap resample per roll) — because the validation
needs to MEASURE distribution (does §7.4 spread across 8 quorums?) and
contention (does Guard 2 fire?), which needs roll VOLUME, not just
enough-to-avoid-churn.

COORDINATOR INDEPENDENCE (SG-3 pattern): demand is spread across >= 2 callers
(refuses to start on one unless --allow-single).  Routing keys on the TIP HASH
(§7.4 decision A'), never caller_salt — varied game_ids/salts here are for
realistic roll diversity, NOT routing steering (they cannot steer; that is the
anti-grind design working).

RESILIENT BY DESIGN: an individual roll failure (e.g. a roll landing mid-reform)
is logged-and-counted, never fatal; a caller going dark shifts demand to the
rest with a warning.  A failing-roll SPIKE is itself a signal — success/fail
rates are part of the output, so the driver doubles as a demand-health monitor.

Output: periodic stats block (total/ok/fail, per-quorum distribution from the
returned quorum_hash, per-caller counts, achieved mean interval in blocks) +
a JSONL record per roll for post-run analysis.

Usage:
  python3 demand_driver.py --callers 127.0.0.1:31000,127.0.0.1:31099 \
      [--gap-min 2 --gap-max 5] [--stats-every 20] [--jsonl demand.jsonl]
"""

import argparse
import json
import random
import signal
import sys
import time

from harness.node import Node, RPCError

DEF_RPC_USER = "ptxw2rpc"
# ★ single source of truth: the .env gen_fleet writes (9e41d79 auth fix) —
# this file used to carry its own copied literal, which kept polling with a
# rotated-away password and presented 106 healthy nodes as unreachable.
from harness.cluster import DEF_RPC_PASS


def parse_args():
    ap = argparse.ArgumentParser()
    ap.add_argument("--callers", required=True,
                    help="comma-separated host:port RPC endpoints (>= 2 enforced)")
    ap.add_argument("--rpc-user", default=DEF_RPC_USER)
    ap.add_argument("--rpc-pass", default=DEF_RPC_PASS)
    ap.add_argument("--gap-min", type=int, default=2,
                    help="min blocks between rolls (default 2)")
    ap.add_argument("--gap-max", type=int, default=5,
                    help="max blocks between rolls (default 5; mean ~3.5 — well above the 25-block ODC-052 floor)")
    ap.add_argument("--count", type=int, default=1, help="values per roll")
    ap.add_argument("--low", type=int, default=1)
    ap.add_argument("--high", type=int, default=100)
    ap.add_argument("--stats-every", type=int, default=20,
                    help="print the stats block every N rolls")
    ap.add_argument("--jsonl", default="demand_driver.jsonl",
                    help="per-roll JSONL log (append)")
    ap.add_argument("--poll", type=float, default=5.0,
                    help="height poll interval, seconds")
    ap.add_argument("--stall-warn", type=int, default=300,
                    help="warn if no new block for this many seconds")
    ap.add_argument("--allow-single", action="store_true",
                    help="override the >=2-caller requirement (NOT for validation runs)")
    return ap.parse_args()


class Stats:
    def __init__(self):
        self.total = 0
        self.ok = 0
        self.fail = 0
        self.per_quorum = {}      # quorum_hash -> count
        self.per_caller_ok = {}   # caller name -> count
        self.per_caller_fail = {}
        self.first_roll_height = None
        self.last_roll_height = None
        self.latencies = []       # ms, caller-measured request→answer (ok rolls)

    def latency_pctiles(self):
        if not self.latencies:
            return None
        s = sorted(self.latencies)
        pick = lambda p: s[min(len(s) - 1, int(p * len(s)))]
        return pick(0.0), pick(0.50), pick(0.95), pick(0.999)

    def mean_interval_blocks(self):
        if self.ok < 2 or self.first_roll_height is None:
            return None
        span = self.last_roll_height - self.first_roll_height
        return span / (self.ok - 1) if self.ok > 1 else None

    def block(self, height):
        lines = [
            f"── demand stats @ h{height} ──",
            f"rolls: {self.total} total, {self.ok} ok, {self.fail} fail"
            + (f"  ★ FAIL RATE {100.0 * self.fail / self.total:.1f}% — a spike here is a signal"
               if self.total and self.fail else ""),
            f"achieved mean interval: "
            + (f"{self.mean_interval_blocks():.2f} blocks/roll" if self.mean_interval_blocks() else "n/a")
            + "  (ODC-052 floor: 25)",
        ]
        if self.latency_pctiles():
            lo, p50, p95, p999 = self.latency_pctiles()
            lines.append(f"roll latency (caller req→signed answer): "
                         f"min {lo:.0f} · p50 {p50:.0f} · p95 {p95:.0f} · max {p999:.0f} ms")
        if self.per_quorum:
            total_q = sum(self.per_quorum.values())
            lines.append("per-quorum distribution (§7.4 routing):")
            for qh, c in sorted(self.per_quorum.items(), key=lambda kv: -kv[1]):
                lines.append(f"  {qh[:16]}…  {c:5d}  ({100.0 * c / total_q:5.1f}%)")
        lines.append("per-caller: " + ", ".join(
            f"{n}={self.per_caller_ok.get(n, 0)}ok/{self.per_caller_fail.get(n, 0)}f"
            for n in sorted(set(self.per_caller_ok) | set(self.per_caller_fail))))
        return "\n".join(lines)


def main():
    args = parse_args()
    endpoints = [e.strip() for e in args.callers.split(",") if e.strip()]
    if len(endpoints) < 2 and not args.allow_single:
        sys.exit("REFUSED: >= 2 callers required (SG-3 coordinator-independence; "
                 "--allow-single to override for smoke only)")

    callers = []
    for i, ep in enumerate(endpoints):
        host, port = ep.rsplit(":", 1)
        callers.append(Node(f"caller{i + 1}@{ep}", host, int(port),
                            args.rpc_user, args.rpc_pass))

    # Height source: any live caller (re-selected on failure).
    def best_height():
        for c in callers:
            try:
                return c.getblockcount()
            except Exception:
                continue
        return None

    stats = Stats()
    stop = {"flag": False}
    signal.signal(signal.SIGINT, lambda *_: stop.update(flag=True))
    signal.signal(signal.SIGTERM, lambda *_: stop.update(flag=True))

    jsonl = open(args.jsonl, "a")
    seq = 0
    h = best_height()
    if h is None:
        sys.exit("no caller reachable at start")
    next_roll_at = h + random.randint(args.gap_min, args.gap_max)
    last_new_block = time.time()
    last_h = h
    print(f"[driver] start @ h{h}, {len(callers)} callers, gap {args.gap_min}-{args.gap_max} blocks, "
          f"target mean ~{(args.gap_min + args.gap_max) / 2:.1f} blocks/roll")

    while not stop["flag"]:
        time.sleep(args.poll)
        h = best_height()
        if h is None:
            print("[driver] WARNING: no caller reachable — retrying")
            continue
        if h > last_h:
            last_h, last_new_block = h, time.time()
        elif time.time() - last_new_block > args.stall_warn:
            print(f"[driver] WARNING: chain stalled at h{h} for "
                  f"{int(time.time() - last_new_block)}s — still polling")
            last_new_block = time.time()  # re-arm, keep warning periodically
        if h < next_roll_at:
            continue

        # Fire one roll from a random caller; varied synthetic inputs.
        seq += 1
        caller = random.choice(callers)
        game_id = f"w25b-{seq:06d}"
        salt = "%08x" % random.getrandbits(32)
        stats.total += 1
        rec = {"seq": seq, "t": int(time.time()), "h": h,
               "caller": caller.name, "game_id": game_id}
        # ★ caller-measured latency: wall-clock request → threshold-signed answer
        # (covers the whole fund-then-sign path: commit broadcast + fan-out to the
        # quorum + partial-sig collection to threshold + reconstruction). Measured
        # on BOTH branches so a slow FAILURE is visible, not just successes.
        t0 = time.perf_counter()
        try:
            r = caller.ptx_roll(args.count, args.low, args.high, game_id, salt)
            lat_ms = round((time.perf_counter() - t0) * 1000, 1)
            stats.ok += 1
            stats.latencies.append(lat_ms)
            qh = r.get("quorum_hash", "?")
            stats.per_quorum[qh] = stats.per_quorum.get(qh, 0) + 1
            stats.per_caller_ok[caller.name] = stats.per_caller_ok.get(caller.name, 0) + 1
            if stats.first_roll_height is None:
                stats.first_roll_height = h
            stats.last_roll_height = h
            rec.update(ok=True, quorum_hash=qh,
                       results=r.get("results"), tx_id=r.get("tx_id"),
                       round_seed=r.get("round_seed"),  # join key for the dashboard
                       latency_ms=lat_ms)
        except (RPCError, Exception) as e:  # noqa: BLE001 — resilience by design
            lat_ms = round((time.perf_counter() - t0) * 1000, 1)
            stats.fail += 1
            stats.per_caller_fail[caller.name] = stats.per_caller_fail.get(caller.name, 0) + 1
            rec.update(ok=False, error=str(e)[:200], latency_ms=lat_ms)
            print(f"[driver] roll {seq} FAILED via {caller.name} after {lat_ms:.0f}ms: {str(e)[:120]} — continuing")
        jsonl.write(json.dumps(rec) + "\n")
        jsonl.flush()

        next_roll_at = h + random.randint(args.gap_min, args.gap_max)
        if stats.total % args.stats_every == 0:
            print(stats.block(h))

    print("\n[driver] stopping — final stats:")
    print(stats.block(last_h))


if __name__ == "__main__":
    main()
