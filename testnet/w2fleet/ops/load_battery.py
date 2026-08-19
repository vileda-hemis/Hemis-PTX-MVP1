#!/usr/bin/env python3
"""Concurrent-load roll battery (fan-out-budget arc, 2026-08-17).

Load profile per operator spec: 20 rolls fired CONCURRENTLY at each new block
(round-robin across all 8 callers, cycling the 8 baseline shapes), sustained
for 10 blocks per rung => 200 rolls/rung. Batches launch on the block tick
regardless of whether earlier rolls are still in flight (real overlap load).

Usage: load_battery.py <tag> [rolls_per_block=20] [blocks=10]
Appends one JSON line per rung to w2-fleet/load_ladder_results.jsonl.
Run under netem via netem_mesh.sh apply <D_ms>; tag e.g. load-d50.
"""
import hashlib, json, subprocess, sys, time, statistics, datetime, collections
from concurrent.futures import ThreadPoolExecutor

CLI = ["Hemis-cli", "-ptxbea", "-rpcuser=ptxw2rpc", "-rpcpassword=ptxw2pass2026"]
CALLERS = [f"ptx-w2r-caller{i}" for i in range(1, 9)]
OUT = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/load_ladder_results.jsonl"

SHAPES = [
    ("d100",       1, 1, 100,        False, []),
    ("d6",         1, 1, 6,          False, []),
    ("coin",       1, 0, 1,          False, []),
    ("multi5",     5, 1, 100,        False, []),
    ("unique5",    5, 1, 100,        True,  []),
    ("bigrange",   1, 1, 10**9,      False, []),
    ("int-excl",   1, 1, 20,         False, [3, 7, 11]),
    ("lotto6of49", 6, 1, 49,         True,  []),
]

def rpc_height():
    p = subprocess.run(["docker", "exec", CALLERS[0]] + CLI + ["getblockcount"],
                       capture_output=True, text=True, timeout=15)
    return int(p.stdout.strip())

def roll(caller, shape, salt, block):
    name, count, low, high, unique, exclude = shape
    args = [str(count), str(low), str(high), "true" if unique else "false",
            json.dumps(exclude), f"load-{name}", salt]
    t0 = time.monotonic()
    try:
        p = subprocess.run(["docker", "exec", caller] + CLI + ["ptx_roll"] + args,
                           capture_output=True, text=True, timeout=180)
        ok = p.returncode == 0
        err = "" if ok else (p.stderr.strip() or p.stdout.strip())[:300]
    except subprocess.TimeoutExpired:
        ok, err = False, "client timeout 180s"
    dt = time.monotonic() - t0
    return {"shape": name, "caller": caller, "block": block, "ok": ok,
            "latency_s": round(dt, 2), "err": err}

def main():
    tag = sys.argv[1]
    per_block = int(sys.argv[2]) if len(sys.argv) > 2 else 20
    n_blocks = int(sys.argv[3]) if len(sys.argv) > 3 else 10
    pool = ThreadPoolExecutor(max_workers=per_block * 3)
    futures, ci = [], 0
    h = rpc_height()
    print(f"[{tag}] start tip h{h}: {per_block} rolls/block x {n_blocks} blocks", flush=True)
    for b in range(n_blocks):
        # wait for the next block tick (first batch fires on the first new block)
        while True:
            time.sleep(2)
            nh = rpc_height()
            if nh > h:
                h = nh
                break
        batch = []
        for j in range(per_block):
            caller = CALLERS[ci % len(CALLERS)]
            shape = SHAPES[ci % len(SHAPES)]
            # salt must be pure hex (ptx_roll validates): 4 hex chars from the
            # tag + block/slot indices (decimal digits are hex-valid)
            salt = f"{hashlib.md5(tag.encode()).hexdigest()[:4]}{b:02d}{j:02d}"
            batch.append(pool.submit(roll, caller, shape, salt, h))
            ci += 1
        futures.extend(batch)
        in_flight = sum(1 for f in futures if not f.done())
        print(f"[{tag}] block {b+1}/{n_blocks} h{h}: fired {per_block}, "
              f"in-flight {in_flight}", flush=True)
    runs = [f.result() for f in futures]
    pool.shutdown()
    lat_ok = [r["latency_s"] for r in runs if r["ok"]]
    n_ok = len(lat_ok)
    errs = collections.Counter()
    for r in runs:
        if not r["ok"]:
            key = r["err"].splitlines()[-1][:80] if r["err"] else "?"
            errs[key] += 1
    by_block = collections.Counter()
    by_block_ok = collections.Counter()
    for r in runs:
        by_block[r["block"]] += 1
        by_block_ok[r["block"]] += r["ok"]
    summary = {
        "tag": tag, "when": datetime.datetime.now().isoformat(timespec="seconds"),
        "per_block": per_block, "blocks": n_blocks,
        "ok": n_ok, "total": len(runs),
        "p50_s": round(statistics.median(lat_ok), 2) if lat_ok else None,
        "p95_s": round(sorted(lat_ok)[max(0, int(len(lat_ok)*.95) - 1)], 2) if lat_ok else None,
        "max_s": round(max(lat_ok), 2) if lat_ok else None,
        "fail_reasons": dict(errs),
        "ok_by_block": {str(h): f"{by_block_ok[h]}/{by_block[h]}" for h in sorted(by_block)},
        "runs": runs,
    }
    with open(OUT, "a") as f:
        f.write(json.dumps(summary) + "\n")
    print(f"[{tag}] DONE {n_ok}/{len(runs)} ok  p50={summary['p50_s']}s "
          f"p95={summary['p95_s']}s max={summary['max_s']}s", flush=True)
    for e, n in errs.most_common():
        print(f"[{tag}]   fail x{n}: {e}", flush=True)

if __name__ == "__main__":
    main()
