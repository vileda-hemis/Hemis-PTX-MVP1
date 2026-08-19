#!/usr/bin/env python3
"""Roll battery for the fan-out-budget latency arc (pre-testnet, 2026-08-16).

Recreation of the lost session-3ddfa438 baseline_rolls.py (never banked; the
clean-substrate numbers survive in the standup: 48/48 OK, p50 1.2s / p95 1.5s,
FANOUT_MAX_ATTEMPTS=60, ~9s budget never engaged). Shape names match the
baseline battery; exact ranges re-chosen where the originals weren't recorded.

Usage: latency_battery.py <tag> [samples_per_shape=3]
Appends one JSON line per run to w2-fleet/latency_ladder_results.jsonl.
Run under netem via netem_mesh.sh apply <D_ms>; tag e.g. d50 for 50ms one-way.
"""
import json, subprocess, sys, time, statistics, datetime

CLI = ["Hemis-cli", "-ptxbea", "-rpcuser=ptxw2rpc", "-rpcpassword=ptxw2pass2026"]
CALLERS = [f"ptx-w2r-caller{i}" for i in range(1, 9)]
OUT = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/latency_ladder_results.jsonl"
PACING_S = 12  # matches baseline battery pacing

# (name, count, low, high, unique, exclude)
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

def roll(caller, name, count, low, high, unique, exclude, salt):
    args = [str(count), str(low), str(high), "true" if unique else "false",
            json.dumps(exclude), f"lat-{name}", salt]
    t0 = time.monotonic()
    try:
        p = subprocess.run(["docker", "exec", caller] + CLI + ["ptx_roll"] + args,
                           capture_output=True, text=True, timeout=300)
        ok = p.returncode == 0
        err = "" if ok else (p.stderr.strip() or p.stdout.strip())[:300]
    except subprocess.TimeoutExpired:
        ok, err = False, "client timeout 300s"
    dt = time.monotonic() - t0
    return ok, dt, err

def main():
    tag = sys.argv[1]
    samples = int(sys.argv[2]) if len(sys.argv) > 2 else 3
    runs, lat_ok = [], []
    ci = 0
    for s in range(samples):
        for (name, count, low, high, unique, exclude) in SHAPES:
            caller = CALLERS[ci % len(CALLERS)]; ci += 1
            salt = f"{s:02d}{ci:02d}".ljust(8, "a")
            ok, dt, err = roll(caller, name, count, low, high, unique, exclude, salt)
            runs.append({"shape": name, "caller": caller, "ok": ok,
                         "latency_s": round(dt, 2), "err": err})
            if ok:
                lat_ok.append(dt)
            print(f"[{tag}] {s+1}/{samples} {name:<10} {caller} "
                  f"{'OK ' if ok else 'FAIL'} {dt:5.1f}s {err}", flush=True)
            time.sleep(PACING_S)
    n_ok = sum(r["ok"] for r in runs)
    summary = {
        "tag": tag, "when": datetime.datetime.now().isoformat(timespec="seconds"),
        "ok": n_ok, "total": len(runs),
        "p50_s": round(statistics.median(lat_ok), 2) if lat_ok else None,
        "p95_s": round(sorted(lat_ok)[max(0, int(len(lat_ok)*.95) - 1)], 2) if lat_ok else None,
        "max_s": round(max(lat_ok), 2) if lat_ok else None,
        "runs": runs,
    }
    with open(OUT, "a") as f:
        f.write(json.dumps(summary) + "\n")
    print(f"[{tag}] DONE {n_ok}/{len(runs)} ok  p50={summary['p50_s']}s "
          f"p95={summary['p95_s']}s max={summary['max_s']}s  -> {OUT}")

if __name__ == "__main__":
    main()
