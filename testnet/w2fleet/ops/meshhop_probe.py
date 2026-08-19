#!/usr/bin/env python3
"""Mesh-hop instrumentation — measure the PROPAGATION term of roll latency directly
instead of inferring it (FANOUT_BUDGET_ANALYSIS.md §11).

WHAT IT MEASURES
  commitment broadcast (caller) -> per-member mempool acceptance, for the 11 members
  of the quorum that actually signs the roll.  Headline = TIME TO SIXTH MEMBER: six
  partials is the signing threshold, so the sixth acceptance is the moment the roll
  *could* complete.  roll_total - time_to_sixth = the dial-and-sign component.

WHY POLLING, NOT THE MEMPOOL ENTRY TIME (checked before building)
  This fork has no getmempoolentry; getrawmempool(verbose)'s "time" is documented
  "in seconds since 1 Jan 1970" -- 1s resolution against a ~1.2s signal, i.e. up to
  ~80% error at the clean rung.  debug.log is also 1s (no -logtimemicros).  So the
  no-deploy read exists but not at usable resolution; the no-deploy read that IS
  usable is a high-frequency poll of getrawmempool (non-verbose, cheap) from ONE
  host process = ONE clock, no daemon change, no GM deploy.

WHY THIS IS CHEAP ENOUGH NOT TO PERTURB WHAT IT MEASURES
  Consecutive rolls from a caller reuse the same quorum (verified), so a discovery
  roll pins the 11 members and we poll 12 nodes, not 153: ~120 req/s total at 100ms,
  ~8ms per call.  Polling all 153 (~1000 req/s) would have risked changing the
  propagation timing it is trying to measure.

KNOWN LIMITS -- state these with any number this produces
  * Quantisation: first-sight is resolved to the poll interval (default 100ms).
  * Under netem the poll path is delayed too (host->veth carries the same delay),
    which inflates t0 and every t_i by ~the same RTT, so it CANCELS in t_i - t0.
    Residual is jitter, not offset.  Per-node poll RTT is recorded so this is visible.
  * "Acceptance" is observed as "txid present in getrawmempool", which is
    acceptance + up to one poll interval, not the daemon's internal accept instant.
"""
import json, sys, time, base64, threading, statistics, http.client, os

W2 = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet"
ENVF = "/mnt/pve/Node14TB/hemis-ptx/docker-w2r/.env"
OUT = f"{W2}/meshhop_results.jsonl"
POLL_MS = int(os.environ.get("MESHHOP_POLL_MS", "100"))
# Upper bound on the post-roll propagation window. Must exceed worst-case gossip
# at the slowest rung (baseline d200 to_last reached 8.4s), NOT the roll duration.
GRACE_MAX_S = 12.0
PTXROLLCOMMIT = 12

_env = dict(l.strip().split("=", 1) for l in open(ENVF) if "=" in l and not l.startswith("#"))
_AUTH = "Basic " + base64.b64encode(f"{_env['RPCUSER']}:{_env['RPCPASSWORD']}".encode()).decode()

def port_of(node):
    if node == "caller1": return 32000
    if node.startswith("caller"): return 32900 + int(node[6:]) - 1
    return 32000 + int(node[2:])

class Conn:
    """Keep-alive JSON-RPC connection; reconnects on error."""
    def __init__(self, port):
        self.port = port; self.c = None
    def _connect(self):
        self.c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
    def call(self, method, params=None):
        body = json.dumps({"jsonrpc": "1.0", "id": "mh", "method": method, "params": params or []})
        for attempt in (0, 1):
            try:
                if self.c is None: self._connect()
                self.c.request("POST", "/", body,
                               {"Content-Type": "application/json", "Authorization": _AUTH})
                r = self.c.getresponse(); data = r.read()
                return json.loads(data)["result"]
            except Exception:
                try: self.c.close()
                except Exception: pass
                self.c = None
                if attempt: raise
        return None

def poller(node, first_seen, stop, rtts, lock):
    c = Conn(port_of(node))
    while not stop.is_set():
        t = time.monotonic()
        try:
            mp = c.call("getrawmempool")
        except Exception:
            time.sleep(POLL_MS / 1000.0); continue
        now = time.monotonic()
        with lock:
            rtts.setdefault(node, []).append((now - t) * 1000)
            fs = first_seen.setdefault(node, {})
            for txid in mp:
                if txid not in fs: fs[txid] = now
        d = (POLL_MS / 1000.0) - (time.monotonic() - t)
        if d > 0: time.sleep(d)

def one_roll(caller, members, salt, game_id):
    """Poll caller+members while firing one roll; return the measurement dict."""
    nodes = [caller] + members
    first_seen, rtts, lock, stop = {}, {}, threading.Lock(), threading.Event()
    threads = [threading.Thread(target=poller, args=(n, first_seen, stop, rtts, lock), daemon=True)
               for n in nodes]
    for t in threads: t.start()
    time.sleep(1.0)                       # let every poller establish a baseline mempool
    with lock:
        baseline = {n: set(first_seen.get(n, {}).keys()) for n in nodes}

    cc = Conn(port_of(caller))
    t_start = time.monotonic()
    err, res = None, None
    try:
        res = cc.call("ptx_roll", [1, 1, 100, False, [], game_id, salt])
    except Exception as e:
        err = str(e)[:200]
    t_end = time.monotonic()

    # ★ GRACE MUST NOT BE DERIVED FROM THE ROLL (2026-08-19, KDD-088 post-deploy).
    # This was `time.sleep(2.0)`. Pre-attach a d200 roll took ~8s, so the window
    # from t0 was ~10s and gossip (to_last up to 8.4s) fit inside it. Direct-attach
    # cut the roll to ~1.25s, which silently cut the observation window to ~3.25s --
    # so members that had NOT yet received the commitment by gossip were recorded as
    # `never_accepted` and rungs read 1-4/11 while every roll SUCCEEDED. That is a
    # measurement failure, not non-delivery: the instrument's window was coupled to
    # the very quantity the change shrank. Same family as KDD-087 (two ceilings on
    # one clock are one ceiling) -- derive it independently, or you measure your own
    # speedup as a delivery fault.
    # Wait for propagation on ITS own clock: stop early once every member has seen
    # something new (costs nothing on the clean rung), else cap at GRACE_MAX_S.
    grace_t0 = time.monotonic()
    while time.monotonic() - grace_t0 < GRACE_MAX_S:
        with lock:
            pending = [n for n in members
                       if not (set(first_seen.get(n, {}).keys()) - baseline[n])]
        if not pending:
            break
        time.sleep(0.1)
    stop.set()
    for t in threads: t.join(timeout=5)

    with lock:
        fs = {n: dict(first_seen.get(n, {})) for n in nodes}
        rtt_med = {n: round(statistics.median(v), 1) for n, v in rtts.items() if v}

    # ★ ANTI-VACUITY (2026-08-19): the polled member list comes from a discovery
    # roll, but quorums ROTATE. If the roll actually signed with a different set,
    # every polled node reports "never accepted" and the sample looks like a total
    # blackout while the roll succeeded. All 11 reporting identically is the
    # check-the-probe-not-the-chain signature; a stale member list is a MEASUREMENT
    # failure and must be reported as invalid, never counted as non-delivery.
    actual = sorted(m.split(":")[0] for m in (res or {}).get("quorum_members", [])) \
             if isinstance(res, dict) else []
    quorum_matched = (not actual) or (actual == sorted(members))

    # The commitment = the earliest NEW tx at the caller that is type 12.
    new_at_caller = sorted(((ts, tx) for tx, ts in fs[caller].items()
                            if tx not in baseline[caller]), key=lambda x: x[0])
    commit = None
    for ts, tx in new_at_caller:
        try:
            if int(cc.call("getrawtransaction", [tx, 1]).get("type", -1)) == PTXROLLCOMMIT:
                commit = (tx, ts); break
        except Exception:
            continue
    if commit is None:
        return {"ok": False, "why": "no PTXROLLCOMMIT observed at caller",
                "quorum_matched": quorum_matched, "quorum_actual": actual,
                "new_at_caller": [t for _, t in new_at_caller], "err": err,
                "roll_total_s": round(t_end - t_start, 3)}

    txid, t0 = commit
    deltas = {}
    for m in members:
        ts = fs[m].get(txid)
        deltas[m] = None if ts is None else round(ts - t0, 3)
    seen = sorted(d for d in deltas.values() if d is not None)
    # ★ ENFORCE THE ANTI-VACUITY GUARD (2026-08-19). Until now it only SET a flag
    # and left ok=True, so a stale-member-list sample was indistinguishable from a
    # real one unless the reader remembered to filter -- and no reader did. Every
    # anomalous d200 roll (to_first 2.5-3.2s, accepted 1-4/11) was quorum_matched
    # False; every matched roll was to_first ~0.4-0.6s at full acceptance. The
    # propagation fields are MEANINGLESS when the polled set is not the signing set:
    # those nodes were never dialled, so they measure gossip to NON-MEMBERS.
    # roll_total IS still valid (the roll really happened and really took that long)
    # so it is kept; only the per-member fields are nulled. Detect-and-flag was not
    # enough -- the invalid number has to be ABSENT, not merely labelled.
    if not quorum_matched:
        return {
            "ok": err is None and res is not None,
            "quorum_matched": False,
            "invalid_reason": "stale member list - polled set != signing set "
                              "(quorum rotated mid-run); propagation fields dropped",
            "commit_txid": txid,
            "roll_total_s": round(t_end - t_start, 3),
            "members_total": len(members),
            "quorum_actual": actual,
            "members_accepted": None, "to_first_s": None, "to_sixth_s": None,
            "to_last_s": None, "per_member_s": None, "never_accepted": None,
            "poll_rtt_median_ms": rtt_med, "err": err,
        }

    return {
        "ok": err is None and res is not None,
        "quorum_matched": quorum_matched,
        "quorum_actual": actual,
        "commit_txid": txid,
        "roll_total_s": round(t_end - t_start, 3),
        "t0_after_rpc_start_s": round(t0 - t_start, 3),
        "members_total": len(members),
        "members_accepted": len(seen),
        "to_first_s":  seen[0]  if len(seen) >= 1 else None,
        "to_sixth_s":  seen[5]  if len(seen) >= 6 else None,
        "to_last_s":   seen[-1] if seen else None,
        "per_member_s": deltas,
        "never_accepted": [m for m, d in deltas.items() if d is None],
        "poll_rtt_median_ms": rtt_med,
        "err": err,
    }

def main():
    caller = sys.argv[1] if len(sys.argv) > 1 else "caller1"
    tag    = sys.argv[2] if len(sys.argv) > 2 else "clean"
    n      = int(sys.argv[3]) if len(sys.argv) > 3 else 5

    disc = Conn(port_of(caller)).call("ptx_roll", [1, 1, 100, False, [], "meshhop-disc", "d15c0000"])
    members = sorted(m.split(":")[0] for m in disc["quorum_members"])
    print(f"[{tag}] quorum {disc['quorum_hash'][:16]} members={len(members)}: {','.join(members)}")

    rows = []
    for i in range(n):
        r = one_roll(caller, members, f"{i:04x}beef", f"meshhop-{tag}")
        r.update({"tag": tag, "caller": caller, "i": i, "poll_ms": POLL_MS,
                  "quorum_hash": disc["quorum_hash"]})
        rows.append(r)
        print(f"[{tag}] {i+1}/{n} total={r.get('roll_total_s')}s "
              f"first={r.get('to_first_s')} sixth={r.get('to_sixth_s')} last={r.get('to_last_s')} "
              f"accepted={r.get('members_accepted')}/{r.get('members_total')}"
              + ("" if r.get("ok") else f"  !! {r.get('why') or r.get('err')}"))
        with open(OUT, "a") as f:
            f.write(json.dumps(r) + "\n")
        time.sleep(8)

    good = [r for r in rows if r.get("to_sixth_s") is not None]
    if good:
        six = [r["to_sixth_s"] for r in good]; tot = [r["roll_total_s"] for r in good]
        print(f"\n[{tag}] n={len(good)}  to_sixth p50={statistics.median(six):.2f}s "
              f"min={min(six):.2f} max={max(six):.2f}   roll_total p50={statistics.median(tot):.2f}s")
        print(f"[{tag}] propagation share of roll latency: "
              f"{100*statistics.median(six)/statistics.median(tot):.0f}%")

if __name__ == "__main__":
    main()
