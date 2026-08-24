#!/usr/bin/env python3
"""Sustained roll pressure — hold a FLOOR of >= N rolls per block, split across
every caller, until stopped.

WHY A NEW DRIVER AND NOT demand_driver.py: that one paces by a resampled
block GAP (fire, then wait gap blocks), so its output is a mean, not a floor —
at gap 1 it still emits exactly one roll per *observed* height increment and
silently under-delivers whenever two blocks pass between polls.  The ask here
is a guaranteed minimum per block, which is a different control law: watch the
tip, and for every block height that goes by, owe it a batch.

WHY ROUND-ROBIN AND NOT random.choice (demand_driver's split): at 2 rolls per
block a uniform random pick over 8 callers leaves individual callers idle for
long stretches by luck alone.  Round-robin makes "split across callers" an
invariant of the schedule rather than a property of the average.

FLOOR PRESERVATION: a roll that fails is retried ONCE on the next caller in the
rotation, because an unretried failure silently drops that block below the
floor.  Both attempts are recorded; the retry is flagged so the failure is
never hidden by its own repair.

CATCH-UP IS CAPPED AND SAID SO: if the tip jumps (driver paused, node resync)
the arrears are paid at most --max-catchup blocks' worth, and the truncation is
logged.  A silent cap would read as "the floor held" when it did not.

Usage (from testnet/w2fleet):
  python3 ops/roll_pressure.py [--per-block 2] [--poll 2] \
      [--jsonl .../roll_pressure.jsonl] [--stopfile .../roll_pressure.stop]
Stop: touch the stopfile, or SIGTERM/SIGINT.  Either way it prints final stats.
"""

import argparse
import base64
import collections
import json
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

RPC_USER = os.environ.get("RPCUSER", "ptxw2rpc")
RPC_PASS = os.environ.get("RPCPASSWORD", "ptxw2pass2026")

# Roll shapes cycled for realistic demand diversity.  These do NOT steer
# routing (§7.4 keys on the tip hash) — they exercise the sampler's branches.
SHAPES = [
    ("d100",       1, 1, 100,     False, []),
    ("d6",         1, 1, 6,       False, []),
    ("coin",       1, 0, 1,       False, []),
    ("multi5",     5, 1, 100,     False, []),
    ("unique5",    5, 1, 100,     True,  []),
    ("bigrange",   1, 1, 10**9,   False, []),
    ("int-excl",   1, 1, 20,      False, [3, 7, 11]),
    ("lotto6of49", 6, 1, 49,      True,  []),
]


def rpc(port, method, params=None, timeout=180):
    req = urllib.request.Request(
        f"http://127.0.0.1:{port}/",
        data=json.dumps({"jsonrpc": "1.0", "id": "rp", "method": method,
                         "params": params or []}).encode(),
        headers={"Content-Type": "text/plain",
                 "Authorization": "Basic " + base64.b64encode(
                     f"{RPC_USER}:{RPC_PASS}".encode()).decode()})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        out = json.load(r)
    if out.get("error"):
        raise RuntimeError(json.dumps(out["error"])[:200])
    return out["result"]


def discover_callers(prefix, n, exclude=()):
    """Host ports from docker, not a literal — the map is not contiguous
    (caller1 is 32000, callers 2-8 are 32901-32907) and a fleet re-gen moves it.

    ★ `exclude` is an explicit deny-list rather than a smaller `--callers` count.
    "callers 1-6" and "all callers except 7 and 8" happen to coincide today, but
    only one of them still means the right thing if the fleet is re-generated or
    the count is raised later. The operator said NEVER on 7 or 8; that is a
    standing constraint, so it is stated as one."""
    found = []
    for i in range(1, n + 1):
        if i in exclude:
            print(f"[rp] caller{i} EXCLUDED by operator instruction — not dialled", flush=True)
            continue
        name = f"{prefix}{i}"
        try:
            p = subprocess.run(["docker", "port", name, "29995/tcp"],
                               capture_output=True, text=True, timeout=20)
            line = p.stdout.strip().splitlines()[0]
            found.append((name, int(line.rsplit(":", 1)[1])))
        except Exception as e:
            print(f"[rp] caller {name} not resolvable ({e}) — excluded", flush=True)
    return found


class Stats:
    def __init__(self):
        self.lock = threading.Lock()
        self.total = self.ok = self.fail = 0
        self.retries = self.retry_saves = 0
        self.per_caller_ok = collections.Counter()
        self.per_caller_fail = collections.Counter()
        self.per_quorum = collections.Counter()
        self.per_shape_ok = collections.Counter()
        self.latencies = []
        self.errors = collections.Counter()
        self.blocks_served = 0
        self.blocks_short = 0          # blocks that ended below the floor
        self.first_h = self.last_h = None

    def pct(self, p):
        if not self.latencies:
            return None
        s = sorted(self.latencies)
        return s[min(len(s) - 1, int(p * len(s)))]

    def block(self, h, per_block, started):
        with self.lock:
            mins = (time.time() - started) / 60.0
            lines = [
                f"── roll pressure @ h{h}  (+{mins:.0f} min) ──",
                f"rolls: {self.total} total, {self.ok} ok, {self.fail} fail"
                + (f"   ★ FAIL RATE {100.0*self.fail/self.total:.1f}%" if self.total and self.fail else ""),
                f"blocks served: {self.blocks_served} at floor {per_block}/block"
                + (f"   ★ {self.blocks_short} BLOCK(S) FINISHED BELOW THE FLOOR" if self.blocks_short else "  (floor held)"),
                f"retries: {self.retries} fired, {self.retry_saves} recovered the floor",
            ]
            if self.latencies:
                lines.append("latency ms (req→signed answer): "
                             f"min {self.pct(0.0):.0f} · p50 {self.pct(0.50):.0f} · "
                             f"p95 {self.pct(0.95):.0f} · max {max(self.latencies):.0f}")
            if self.first_h is not None and self.ok:
                span = (self.last_h - self.first_h) or 1
                lines.append(f"achieved rate: {self.ok/span:.2f} ok-rolls per block "
                             f"over h{self.first_h}→h{self.last_h}")
            if self.per_quorum:
                tq = sum(self.per_quorum.values())
                lines.append("per-quorum distribution (§7.4 tip-hash routing):")
                for qh, c in self.per_quorum.most_common(12):
                    lines.append(f"  {qh[:16]}…  {c:6d}  ({100.0*c/tq:5.1f}%)")
            names = sorted(set(self.per_caller_ok) | set(self.per_caller_fail))
            lines.append("per-caller: " + ", ".join(
                f"{n.split('-')[-1]}={self.per_caller_ok[n]}ok/{self.per_caller_fail[n]}f" for n in names))
            if self.per_shape_ok:
                lines.append("per-shape ok: " + ", ".join(
                    f"{s}={c}" for s, c in sorted(self.per_shape_ok.items())))
            for e, n in self.errors.most_common(6):
                lines.append(f"  fail x{n}: {e}")
            return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-block", type=int, default=2,
                    help="FLOOR of rolls owed per block (default 2 — the ask is a "
                         "minimum of 1, this carries margin so one failure does not breach it)")
    ap.add_argument("--callers", type=int, default=8)
    ap.add_argument("--prefix", default="ptx-w2r-caller")
    ap.add_argument("--exclude", default="",
                    help="comma-separated caller indices to NEVER dial (e.g. 7,8)")
    ap.add_argument("--poll", type=float, default=2.0, help="tip poll seconds")
    ap.add_argument("--max-catchup", type=int, default=3,
                    help="cap on arrears paid when the tip jumps (blocks' worth)")
    ap.add_argument("--roll-timeout", type=int, default=180)
    ap.add_argument("--stats-every", type=int, default=25, help="stats block every N rolls")
    ap.add_argument("--jsonl", default="/mnt/pve/Node14TB/hemis-ptx/docker-w2r/roll_pressure.jsonl")
    ap.add_argument("--stopfile", default="/mnt/pve/Node14TB/hemis-ptx/docker-w2r/roll_pressure.stop")
    ap.add_argument("--tag", default="rp")
    args = ap.parse_args()

    excluded = {int(x) for x in args.exclude.split(",") if x.strip()}
    callers = discover_callers(args.prefix, args.callers, excluded)
    if len(callers) < 2:
        sys.exit("REFUSED: fewer than 2 callers reachable — 'split across callers' "
                 "cannot be honoured (SG-3 coordinator independence)")

    stats = Stats()
    stop = {"flag": False, "why": None}

    def _sig(signum, _f):
        stop["flag"] = True
        stop["why"] = f"signal {signum}"
    signal.signal(signal.SIGINT, _sig)
    signal.signal(signal.SIGTERM, _sig)

    if os.path.exists(args.stopfile):
        os.remove(args.stopfile)

    jsonl = open(args.jsonl, "a")
    jl_lock = threading.Lock()
    started = time.time()
    pool = ThreadPoolExecutor(max_workers=max(8, args.per_block * 4))
    rr = {"i": 0}
    rr_lock = threading.Lock()

    def next_caller():
        with rr_lock:
            c = callers[rr["i"] % len(callers)]
            rr["i"] += 1
            return c

    def do_roll(caller, shape, seq, height, attempt):
        name, count, low, high, unique, exclude = shape
        salt = "%08x" % ((seq * 2654435761 + attempt * 40503 + height) & 0xFFFFFFFF)
        cname, port = caller
        t0 = time.perf_counter()
        rec = {"t": int(time.time()), "seq": seq, "h": height, "attempt": attempt,
               "caller": cname, "shape": name, "game_id": f"{args.tag}-{seq:07d}"}
        try:
            r = rpc(port, "ptx_roll",
                    [count, low, high, unique, exclude, rec["game_id"], salt],
                    timeout=args.roll_timeout)
            lat = (time.perf_counter() - t0) * 1000.0
            with stats.lock:
                stats.total += 1
                stats.ok += 1
                stats.latencies.append(lat)
                stats.per_caller_ok[cname] += 1
                stats.per_shape_ok[name] += 1
                qh = r.get("quorum_hash", "?")
                stats.per_quorum[qh] += 1
                if stats.first_h is None:
                    stats.first_h = height
                stats.last_h = height
            rec.update(ok=True, latency_ms=round(lat, 1), quorum_hash=r.get("quorum_hash"),
                       results=r.get("results"), tx_id=r.get("tx_id"),
                       round_seed=r.get("round_seed"), block_height=r.get("block_height"))
            ok = True
        except Exception as e:                      # resilience by design
            lat = (time.perf_counter() - t0) * 1000.0
            msg = str(e)[:200]
            with stats.lock:
                stats.total += 1
                stats.fail += 1
                stats.per_caller_fail[cname] += 1
                stats.errors[msg.splitlines()[-1][:90]] += 1
            rec.update(ok=False, latency_ms=round(lat, 1), error=msg)
            print(f"[{args.tag}] roll {seq} FAILED on {cname} after {lat:.0f}ms: {msg[:120]}",
                  flush=True)
            ok = False
        with jl_lock:
            jsonl.write(json.dumps(rec) + "\n")
            jsonl.flush()
        return ok

    def roll_with_retry(shape, seq, height):
        """One owed roll. A failure is retried ONCE on the NEXT caller so a
        single caller hiccup does not put the block under the floor."""
        if do_roll(next_caller(), shape, seq, height, 1):
            return True
        with stats.lock:
            stats.retries += 1
        if do_roll(next_caller(), shape, seq, height, 2):
            with stats.lock:
                stats.retry_saves += 1
            return True
        return False

    def serve_block(height, seq0):
        futs = [pool.submit(roll_with_retry, SHAPES[(seq0 + j) % len(SHAPES)],
                            seq0 + j, height) for j in range(args.per_block)]
        good = sum(1 for f in futs if f.result())
        with stats.lock:
            stats.blocks_served += 1
            if good < args.per_block:
                stats.blocks_short += 1
        if good < args.per_block:
            print(f"[{args.tag}] ★ h{height}: only {good}/{args.per_block} rolls landed "
                  f"— block finished BELOW the floor", flush=True)

    def tip():
        for _, port in callers:
            try:
                return rpc(port, "getblockcount", timeout=20)
            except Exception:
                continue
        return None

    h = tip()
    if h is None:
        sys.exit("no caller reachable at start")
    print(f"[{args.tag}] start @ h{h} · floor {args.per_block} rolls/block · "
          f"{len(callers)} callers round-robin ({', '.join(c for c, _ in callers)}) · "
          f"poll {args.poll}s · jsonl {args.jsonl}", flush=True)
    print(f"[{args.tag}] stop with: touch {args.stopfile}", flush=True)

    served_through = h
    seq = 0
    last_new_block = time.time()
    next_stats_at = args.stats_every
    inflight = []

    while not stop["flag"]:
        if os.path.exists(args.stopfile):
            stop["flag"], stop["why"] = True, "stopfile"
            break
        time.sleep(args.poll)
        nh = tip()
        if nh is None:
            print(f"[{args.tag}] WARNING: no caller reachable — retrying", flush=True)
            continue
        if nh <= served_through:
            if time.time() - last_new_block > 300:
                print(f"[{args.tag}] WARNING: chain stalled at h{nh} for "
                      f"{int(time.time()-last_new_block)}s — rolls paused, still polling",
                      flush=True)
                last_new_block = time.time()
            continue
        last_new_block = time.time()
        owed = nh - served_through
        if owed > args.max_catchup:
            print(f"[{args.tag}] ★ tip jumped {owed} blocks (h{served_through}→h{nh}); "
                  f"paying {args.max_catchup} blocks' arrears, DROPPING "
                  f"{owed - args.max_catchup} — the floor did not hold across the jump",
                  flush=True)
            with stats.lock:
                stats.blocks_short += owed - args.max_catchup
            owed = args.max_catchup
        for _ in range(owed):
            # Batches launch on the tick and are NOT awaited here: an in-flight
            # roll must never delay the next block's owed batch, or slow rolls
            # would themselves breach the floor they are meant to hold.
            inflight.append(pool.submit(serve_block, nh, seq))
            seq += args.per_block
        served_through = nh
        inflight = [f for f in inflight if not f.done()]
        if stats.total >= next_stats_at:
            next_stats_at = stats.total + args.stats_every
            print(stats.block(nh, args.per_block, started), flush=True)

    print(f"\n[{args.tag}] stopping ({stop['why']}) — draining "
          f"{sum(1 for f in inflight if not f.done())} in-flight batch(es)…", flush=True)
    pool.shutdown(wait=True)
    print(stats.block(served_through, args.per_block, started), flush=True)
    jsonl.close()


if __name__ == "__main__":
    main()
