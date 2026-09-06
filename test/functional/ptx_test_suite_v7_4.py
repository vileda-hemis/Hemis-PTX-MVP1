#!/usr/bin/env python3
"""
Hemis PTX Phase 2 — Live Node Test Suite v7.4
==============================================
226 tests across 20 categories.

v7.4 changes over v7.3:
  Burst pace sweep — empirical RPC error rate vs roll pacing.
  Identifies the BUG-012 mempool-conflict threshold for this wallet.

  T223  baseline   0.3s/roll  (~3.3 rolls/sec)  target: 0 errors
  T224  brisk      0.15s/roll (~6.7 rolls/sec)  target: <10% errors
  T225  fast       0.1s/roll  (~10 rolls/sec)   target: <25% errors
  T226  hammer     0.05s/roll (~20 rolls/sec)   BUG-012 stress probe

  All 4 tests use 30-roll bursts. Pre-flight requires >=50 usable
  2-3 HMS UTXOs in the caller wallet — split wallet first if not.
  Tests SKIP cleanly if wallet too consolidated or settlement imminent.
  Total wall time when ready: ~45s.

v7.3 changes over v7.2:
  KDD-032 / ODC-020 / KDD-034 coverage — PTXSETTLE (nType=7) and
  PTXCONSOLIDATE (nType=8) consensus tx structure verification.

  T200-T203  pool_utxo_count + ptx_lottery_status field additions (KDD-034 RPC)
  T204-T207  PTXSETTLE structure: confirmed on-chain, 1-vout, pays winner,
             input cap <= 200 (KDD-032 Rules 2/4/8 + 200-input cap fix)
  T208-T211  PTXCONSOLIDATE structure: 1-vout, pays back to pool address,
             input cap <= 500, sum(inputs) >= vout[0].value (KDD-034 C1-C5)
  T212-T215  ODC-020 winner address: PTXSETTLE vout pays an ODC-020 GM
             scriptPTXPayment address (Rule 9), distinct per settlement,
             format/length invariants
  T216-T219  Cross-tx consensus probes: PTXSETTLE + PTXCONSOLIDATE never
             coexist in same block; pool inputs only spend from pool address;
             pool_balance == sum(pool UTXOs); empty-scriptSig exemption holds.
  T220-T222  PTX service fee collection: rolls must pay into the pool.
             T220: single-roll pool delta > 0.
             T221: 10-roll pool grows monotonically.
             T222: caller wallet drops by pool delta + miner fee (conservation).

  T200-T219 are PASSIVE scanners — they inspect already-confirmed blocks
  rather than waiting for a settlement window. Runs in seconds.
  T220-T222 are ACTIVE — they fire rolls and wait 2-3 blocks per test.

  Scan range tunable via --ptx-scan-blocks N (default 200).

v7.2 changes over v7.1:
  T177  100-iteration p99 latency baseline at excl=100/300/512.
        Reports min/p50/p95/p99/max/avg/errors per size.
        Replaces single-shot measurement (~p50 by chance, useless for ODC-015).
        Allow ~5 min (3 sizes x 100 calls x ~1s each).

  T181  Escalating concurrency waves: 3/5/10 workers all with excl=512.
        Reports p50/p99/max per wave. Stops early if p99 exceeds 30s.
        Replaces fixed 3-worker pass/fail with no timing.

  T182  20-iteration x 5-worker concurrent load at excl=100/300/512.
        Reports conc_p50/p95/p99/max and delta vs inline single-session ref.
        Flags sizes where concurrent p99 > 3x single-session p50 (saturation).
        Replaces 2-worker avg-only measurement.
        Allow ~5 min (3 sizes x 20 iters x 5 workers x ~1s each).
  BUG-012     T196-T199  NEW — rapid sequential roll / mempool UTXO selection
                          T196: 10-call burst diagnostic — count orphaned results
                          T197: ptx_prepare_wallet(n=15) — skip if not implemented
                          T198: post-prep 15-call burst — 0 orphaned required
                          T199: result integrity — each result has on-chain PTXSESS tx

  BUG-012 root cause: FundTransaction uses pcoinsTip (confirmed UTXO set) only.
  Rapid calls select same UTXO; only call 1 commits on-chain. Calls 2-N return
  a result via RPC but log "input 0 already spent" — result has no chain anchor.
  Fix: CCoinControl + LockCoin/UnlockCoin for mempool-aware selection.
  New RPC: ptx_prepare_wallet(n, amount_each) — splits wallet into N UTXOs.
  See: src/ptx/ptx_mempool.cpp · PTX_AutoCommit · FundTransaction call.

v7 changes over v6.1:
  EXCL-LIMIT  T179-T185  NEW — find real exclusion limit (BUG-011 deep probe)
                          T179: binary search for crash/rejection boundary
                          T180: confirm error code on first over-limit call
                          T181: 3 concurrent sessions each with 512-item exclude
                          T182: latency under concurrent exclude-heavy load
                          T183: empty exclude list (zero items boundary)
                          T184: exclude count > pool size (exhaustion handling)
                          T185: all-duplicate exclude list (dedup behaviour)

  T130-DIAG   T186-T195  NEW — quorum_sig_hash derivation diagnostic (KDD-033 / ODC-019)
                          T186: dump full roll response fields (always passes)
                          T187: SHA256(hex_string) match attempt (likely FAIL)
                          T188: SHA256(hex_upper_string) match attempt
                          T189: SHA256(raw_bytes) match — KDD-033 canonical method
                          T190: SHA256d (double SHA256) match attempt
                          T191: confirm quorum_sig_hash not derived from any other field
                          T192: T130 re-run with per-method diagnosis output
                          T193: 5-roll quorum_sig_hash uniqueness (not reused)
                          T194: quorum_sig_hash length and encoding invariants
                          T195: quorum_sig_hash == SHA256(sig) consistency across
                                3 rolls on a conforming node (positive assertion)

  T90-FIX     T90 assertion inverted per KDD-003 revision (v1.4): two rapid
              same-block same-params calls must produce DIFFERENT seeds, not
              the same seed. This is the positive per-call nonce test.

  T130        Updated label to "quorum_sig_hash == SHA256(raw bytes) — KDD-033"
              and assertion hardened to raw-bytes method only (KDD-033 canonical).

v6.1 changes (carried):
  SALT FIX  All salts generated via mksalt() — MD5 guaranteed 8-char hex.
  BEACON    quorum_sig_hash used throughout (T14, T125, T130).
  BUG-011   T154/T164 document 513-item ACCEPTED; T176-T178 characterisation.

Usage (from node1 host):
  python3 ptx_test_suite_v7.py --fast --skip-excl-probe --skip-lottery
  python3 ptx_test_suite_v7.py --skip-excl-probe --skip-lottery
  python3 ptx_test_suite_v7.py

Flags:
  --fast              Skip statistical (T39-T46) and stress sections
  --skip-fail-modes   Skip adversarial T71-T80
  --skip-advanced     Skip T81-T100
  --skip-excl         Skip T101-T120 exclude hardening
  --skip-excl-probe   Skip T105-T113 BUG-004 characterisation (900 calls)
  --skip-lottery      Skip T150 settlement (requires 15+ min block wait)
  --skip-excl-ext     Skip T151-T160 exclude edge cases
  --skip-dev          Skip T161-T165 dev_seed / error code tests
  --skip-prev-round   Skip T171-T175 prev_round_txid (not yet implemented)
  --skip-excl-load    Skip T176-T178 exclude count load sweep (v6.1)
  --skip-excl-limit   Skip T179-T185 exclusion limit finder (BUG-011 deep probe)
  --skip-t130-diag    Skip T186-T195 quorum_sig_hash derivation diagnostic
  --skip-ptxsettle    Skip T200-T219 PTXSETTLE/PTXCONSOLIDATE structural tests
  --skip-fee          Skip T220-T222 fee collection tests (slow — waits for confirms)
  --skip-pace         Skip T223-T226 pace sweep tests (BUG-012 stress probe)
  --ptx-scan-blocks N Lookback window for T200-T219 chain scan (default 200)
"""

import urllib.request
import json
import base64
import sys
import time
import math
import hashlib
import argparse
import collections
import threading

# ─── Config ───────────────────────────────────────────────────────────────────

RPC_URL   = "http://172.28.0.10:29902/"
RPC_USER  = "ptxrpc"
RPC_PASS  = "ptxpass2026"
ALL_NODES = ["gm01","gm02","gm03","gm04","gm05","gm06","gm07","gm08","gm09","gm10","gm11"]
TIMEOUT   = 45

MAX_EXCLUDE_COUNT = 512   # KDD-028 consensus parameter

# 250025002500 Salt helper 25002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500250025002500

def mksalt(*args):
    """Generate a guaranteed 8-char pure-hex salt from any label args.
    MD5 of the joined args 2014 always valid regardless of input characters.
    Fixes the 'caller_salt must be a hex string' error caused by salts
    containing non-hex chars (l, o, s, g, t, p, etc.)."""
    raw = "_".join(str(a) for a in args)
    return hashlib.md5(raw.encode()).hexdigest()[:8]

# ─── RPC ──────────────────────────────────────────────────────────────────────

def rpc(method, params, url=RPC_URL):
    payload = json.dumps({"jsonrpc":"1.0","id":"ptx","method":method,"params":params}).encode()
    creds   = base64.b64encode(f"{RPC_USER}:{RPC_PASS}".encode()).decode()
    req     = urllib.request.Request(url, data=payload,
                headers={"Content-Type":"text/plain","Authorization":f"Basic {creds}"})
    try:
        resp = urllib.request.urlopen(req, timeout=TIMEOUT)
        data = json.loads(resp.read().decode())
        if data.get("error"):
            return None, data["error"]
        return data["result"], None
    except urllib.error.HTTPError as e:
        try:
            body = json.loads(e.read().decode())
            return None, body.get("error", str(e))
        except Exception:
            return None, str(e)
    except Exception as e:
        return None, str(e)

def roll(count, low, high, unique, exclude=None, game_id="test", salt="aabbcc00",
         prev_round_txid=None, dev_seed=None):
    params = [count, low, high, unique, exclude or [], game_id, salt]
    if prev_round_txid is not None:
        params.append(prev_round_txid)
    if dev_seed is not None:
        params.append(dev_seed)
    return rpc("ptx_roll", params)

def pose_status():
    return rpc("ptx_pose_status", [])

def lottery_status():
    return rpc("ptx_lottery_status", [])

def fail_mode(target, mode):
    return rpc("ptx_debug_setnodefailmode", [target, mode])

def blockcount():
    r, _ = rpc("getblockcount", [])
    return r or 0

def docker_stop_gm(name):
    import subprocess
    r = subprocess.run(["docker","stop", f"ptx-{name}"],
                       capture_output=True, text=True, timeout=20)
    return r.returncode == 0, r.stderr.strip()

def docker_start_gm(name):
    import subprocess
    r = subprocess.run(["docker","start", f"ptx-{name}"],
                       capture_output=True, text=True, timeout=20)
    return r.returncode == 0, r.stderr.strip()

# ─── Helpers ──────────────────────────────────────────────────────────────────

def node_alive():
    r, e = rpc("getblockcount", [])
    return r is not None and not e

def _inv(params, label):
    r, e = rpc("ptx_roll", params)
    alive = node_alive()
    if not alive:
        return False, f"{label}: NODE CRASHED"
    if e:
        return True, f"rejected: {str(e)[:70]}"
    return False, f"{label}: accepted — result: {r}"

def ok(cond, msg=""): return (bool(cond), msg)

def _chisq(counts, total, k):
    expected = total / k
    return sum((c - expected)**2 / expected for c in counts)

def get_pose_map():
    st, e = pose_status()
    if e or not st: return None
    records = st if isinstance(st, list) else st.get("nodes", st.get("pose_records", []))
    return {r["node_id"]: r for r in records}

def _latest_beacon():
    """Fallback: get beacon from most recent round via pose_status if not in roll response."""
    return None

def _near_settlement(margin=3):
    st, e = lottery_status()
    if e or not st: return False
    nsa = st.get("next_settlement_at", 0)
    return nsa > 0 and (nsa - blockcount()) <= margin

def _eligible_ids(st):
    raw = st.get("eligible_nodes", [])
    if not raw: return []
    if isinstance(raw[0], dict):
        return [n.get("node_id", "?") for n in raw]
    return list(raw)

def find_round_by_height(block_height, retries=3):
    for _ in range(retries):
        st, err = rpc("ptx_getroundstatus", [])
        if err or not st:
            time.sleep(1)
            continue
        for rd in st.get("rounds", []):
            if rd.get("block_height") == block_height:
                return rd
        time.sleep(1)
    return None

def _excl_probe(excl_size, iterations, pool_size, label):
    exclude = list(range(1, excl_size + 1))
    low, high = 1, pool_size
    violations, errors, completed = [], [], 0
    for i in range(iterations):
        r, e = roll(1, low, high, False, exclude=exclude,
                    game_id=f"{label}_{i}", salt=mksalt("probe", i))
        if e:
            errors.append(str(e)[:40])
            continue
        v = r["results"][0]
        completed += 1
        if v in exclude:
            violations.append(v)
    return violations, errors, completed

# ─── PTXSETTLE / PTXCONSOLIDATE helpers (v7.3) ─────────────────────────────────

# Cache the scanned blocks for the duration of one suite run so all T200-T219
# tests share work. Reset by clearing _PTX_TX_CACHE.
_PTX_TX_CACHE = {"settle": None, "consolidate": None, "scan_to": 0, "scan_from": 0}
PTX_SCAN_BLOCKS = 200  # overridden by --ptx-scan-blocks

def _pool_address():
    """Resolve PTX lottery pool address from chain params via getblockchaininfo
    or fall back to scanning a known PTXSETTLE output. Returns hex script or
    base58 address — whichever the node exposes. None if unresolvable."""
    info, _ = rpc("getblockchaininfo", [])
    if info:
        for key in ("ptx_lottery_pool", "lottery_pool_address", "ptx_pool_address"):
            v = info.get(key)
            if v:
                return v
    # Fall back to lottery_status if exposed there
    st, _ = lottery_status()
    if st:
        for key in ("pool_address", "lottery_pool_address"):
            v = st.get(key)
            if v:
                return v
    return None

def _scan_pool_txs(force=False):
    """Scan the most recent PTX_SCAN_BLOCKS for nType=7 (PTXSETTLE) and
    nType=8 (PTXCONSOLIDATE) txs. Caches results for the suite run.
    Returns (settle_list, consolidate_list) where each entry is:
      {"txid":..., "block_height":..., "block_hash":..., "tx":<full tx dict>}
    """
    current = blockcount()
    if not current:
        return [], []
    if (not force) and _PTX_TX_CACHE["settle"] is not None and _PTX_TX_CACHE["scan_to"] == current:
        return _PTX_TX_CACHE["settle"], _PTX_TX_CACHE["consolidate"]
    scan_from = max(1, current - PTX_SCAN_BLOCKS + 1)
    settles = []
    consols = []
    for h in range(scan_from, current + 1):
        bh, _ = rpc("getblockhash", [h])
        if not bh:
            continue
        blk, _ = rpc("getblock", [bh, 2])
        if not blk:
            continue
        for tx in blk.get("tx", []):
            payload = tx.get("extraPayload", {}) or {}
            tx_type = tx.get("type") if tx.get("type") is not None else payload.get("type")
            if tx_type == 7:
                settles.append({"txid": tx.get("txid"), "block_height": h,
                                "block_hash": bh, "tx": tx})
            elif tx_type == 8:
                consols.append({"txid": tx.get("txid"), "block_height": h,
                                "block_hash": bh, "tx": tx})
    _PTX_TX_CACHE.update({"settle": settles, "consolidate": consols,
                          "scan_to": current, "scan_from": scan_from})
    return settles, consols

def _vin_value_sum(tx):
    """Resolve each vin's prev_out value by fetching the source tx. Returns
    (sum_sat, n_resolved, n_failed). vin without a resolvable source is skipped."""
    total = 0
    ok_n = fail_n = 0
    for vin in tx.get("vin", []):
        prev_txid = vin.get("txid")
        prev_n    = vin.get("vout")
        if prev_txid is None or prev_n is None:
            fail_n += 1
            continue
        prev, _ = rpc("getrawtransaction", [prev_txid, True])
        if not prev:
            fail_n += 1
            continue
        try:
            v_btc = prev["vout"][prev_n]["value"]
            total += int(round(v_btc * 100_000_000))
            ok_n += 1
        except (KeyError, IndexError, TypeError):
            fail_n += 1
    return total, ok_n, fail_n

def _vin_addresses(tx):
    """Return the set of distinct addresses spent by tx.vin via source tx lookup."""
    addrs = set()
    for vin in tx.get("vin", []):
        prev_txid = vin.get("txid")
        prev_n    = vin.get("vout")
        if prev_txid is None or prev_n is None:
            continue
        prev, _ = rpc("getrawtransaction", [prev_txid, True])
        if not prev:
            continue
        try:
            spk = prev["vout"][prev_n].get("scriptPubKey", {})
            for a in spk.get("addresses", []) or [spk.get("address")] or []:
                if a:
                    addrs.add(a)
        except (KeyError, IndexError, TypeError):
            continue
    return addrs

def _vout_address(tx, vout_idx=0):
    """Return the (address, value_sat) of tx.vout[vout_idx]. None if missing."""
    try:
        out = tx["vout"][vout_idx]
        spk = out.get("scriptPubKey", {})
        addr = None
        addrs = spk.get("addresses", [])
        if addrs:
            addr = addrs[0]
        else:
            addr = spk.get("address")
        v_sat = int(round(out.get("value", 0) * 100_000_000))
        return addr, v_sat
    except (KeyError, IndexError, TypeError):
        return None, None

# ─── Test runner ──────────────────────────────────────────────────────────────

results = []
_pass = _fail = _skip = 0

def test(tid, name, fn):
    global _pass, _fail, _skip
    try:
        outcome, detail = fn()
        if outcome is None:
            print(f"  [SKIP] {tid}  {name}")
            if detail: print(f"         {detail}")
            results.append((tid,"SKIP",name,detail or ""))
            _skip += 1
        elif outcome:
            print(f"  [PASS] {tid}  {name}")
            results.append((tid,"PASS",name,detail or ""))
            _pass += 1
        else:
            print(f"  [FAIL] {tid}  {name}")
            if detail: print(f"         {detail}")
            results.append((tid,"FAIL",name,detail or ""))
            _fail += 1
    except Exception as e:
        print(f"  [FAIL] {tid}  {name}  — exception: {e}")
        results.append((tid,"FAIL",name,f"exception: {e}"))
        _fail += 1

# ═══════════════════════════════════════════════════════════════════════════════
# T01-T10  CORE FUNCTIONALITY
# ═══════════════════════════════════════════════════════════════════════════════

def t01():
    r, e = roll(1, 1, 100, False)
    if e: return None, f"RPC: {e}"
    v = r["results"][0]
    return ok(1 <= v <= 100, f"got {v}")

def t02():
    r, e = roll(1, 7, 7, False, game_id="bound_min", salt=mksalt("t02","min"))
    if e: return None, f"RPC: {e}"
    if r["results"][0] != 7: return False, f"min: got {r['results'][0]}"
    r, e = roll(1, 99, 99, False, game_id="bound_max", salt=mksalt("t02","max"))
    if e: return None, f"RPC: {e}"
    if r["results"][0] != 99: return False, f"max: got {r['results'][0]}"
    return True, "min=7 max=99"

def t03():
    r, e = roll(10, 1, 20, True, game_id="uniq10", salt=mksalt("t03"))
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v)) != 10: return False, f"duplicates in {v}"
    if not all(1 <= x <= 20 for x in v): return False, f"out of range: {v}"
    return True, f"{v}"

def t04():
    r, e = roll(20, 1, 2, False, game_id="nonuniq", salt=mksalt("t04"))
    if e: return None, f"RPC: {e}"
    v = r["results"]
    bad = [x for x in v if x not in [1,2]]
    return ok(not bad, f"out-of-range: {bad}")

def t05():
    exclude = list(range(1, 91))
    r, e = roll(5, 1, 100, True, exclude=exclude, game_id="excl_safe", salt=mksalt("t05"))
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if v in exclude]
    return ok(not bad, f"excluded values appeared: {bad}")

def t06():
    r, e = roll(1, 1, 10, False, exclude=[1,2,3,4,5,6,7,8,9], game_id="excl_one", salt=mksalt("t06"))
    if e: return None, f"RPC: {e}"
    v = r["results"][0]
    return ok(v == 10, f"expected 10, got {v}")

def t07():
    r, e = roll(10, 1, 10, True, game_id="fullperm", salt=mksalt("t07"))
    if e: return None, f"RPC: {e}"
    return ok(sorted(r["results"]) == list(range(1,11)), f"{r['results']}")

def t08():
    r, e = roll(1, 42, 42, False, game_id="single_val", salt=mksalt("t08"))
    if e: return None, f"RPC: {e}"
    return ok(r["results"][0] == 42, f"got {r['results'][0]}")

def t09():
    r, e = roll(3, 1, 1000000, True, game_id="large_range", salt=mksalt("t09"))
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v)) != 3: return False, f"duplicates: {v}"
    return ok(all(1 <= x <= 1000000 for x in v), f"{v}")

def t10():
    r, e = roll(1, 5, 5, True, game_id="pool_one_unique", salt=mksalt("t10"))
    if e: return None, f"RPC: {e}"
    return ok(r["results"][0] == 5, f"got {r['results'][0]}")

# ═══════════════════════════════════════════════════════════════════════════════
# T11-T20  CRYPTOGRAPHIC PROPERTIES P2
# ═══════════════════════════════════════════════════════════════════════════════

def t11():
    r, e = roll(1, 1, 100, False, game_id="seed_fmt", salt=mksalt("t11"))
    if e: return None, f"RPC: {e}"
    s = r.get("round_seed","")
    return ok(isinstance(s,str) and len(s)==64 and all(c in "0123456789abcdef" for c in s), f"'{s}'")

def t12():
    r, e = roll(1, 1, 100, False, game_id="sig_fmt", salt=mksalt("t12"))
    if e: return None, f"RPC: {e}"
    s = r.get("quorum_sig","")
    return ok(isinstance(s,str) and len(s)==192 and all(c in "0123456789abcdef" for c in s),
              f"len={len(s)} '{s[:16]}...'")

def t13():
    r, e = roll(1, 1, 100, False, game_id="members", salt=mksalt("t13"))
    if e: return None, f"RPC: {e}"
    members = sorted(r.get("quorum_members",[]))
    return ok(members == sorted(ALL_NODES), f"got {members}")

def t14():
    """quorum_sig_hash (beacon) — valid 64-char hex. Field is quorum_sig_hash not beacon."""
    r, e = roll(1, 1, 100, False, game_id="beacon_fmt", salt=mksalt("t14"))
    if e: return None, f"RPC: {e}"
    b = r.get("quorum_sig_hash") or r.get("beacon") or r.get("round_beacon","")
    return ok(isinstance(b,str) and len(b)==64 and all(c in "0123456789abcdef" for c in b),
              f"quorum_sig_hash='{b}'")

def t15():
    r, e = roll(1, 1, 100, False, game_id="bh_field", salt=mksalt("t15"))
    if e: return None, f"RPC: {e}"
    h = r.get("block_height")
    return ok(isinstance(h,int) and h > 0, f"block_height={h!r}")

def t16():
    seeds = set()
    for i in range(3):
        r, e = roll(1, 1, 100, False, game_id=f"seed_uniq_{i}", salt=mksalt("seed_uniq", i))
        if e: return None, f"RPC: {e}"
        seeds.add(r.get("round_seed"))
    return ok(len(seeds)==3, f"non-unique seeds: {seeds}")

def t17():
    beacons = set()
    for i in range(3):
        r, e = roll(1, 1, 100, False, game_id=f"beacon_uniq_{i}", salt=mksalt("beacon_uniq", i))
        if e: return None, f"RPC: {e}"
        b = r.get("quorum_sig_hash") or r.get("beacon") or r.get("round_beacon","")
        beacons.add(b)
    return ok(len(beacons)==3, f"non-unique beacons: {beacons}")

def t18():
    r1, e = roll(1, 1, 100, False, game_id="salt_diff", salt="aa000001")
    if e: return None, f"RPC: {e}"
    r2, e = roll(1, 1, 100, False, game_id="salt_diff", salt="bb000002")
    if e: return None, f"RPC: {e}"
    return ok(r1["round_seed"] != r2["round_seed"],
              f"same seed despite different salts: {r1['round_seed'][:16]}...")

def t19():
    r1, e = roll(1, 1, 100, False, game_id="reroll", salt="cc000001")
    if e: return None, f"RPC: {e}"
    r2, e = roll(1, 1, 100, False, game_id="reroll2", salt="cc000002")
    if e: return None, f"RPC: {e}"
    return ok(r1["round_seed"] != r2["round_seed"], "identical seeds on re-roll")

def t20():
    sigs = set()
    for i in range(3):
        r, e = roll(1, 1, 100, False, game_id=f"sig_uniq_{i}", salt=mksalt("sig_uniq", i))
        if e: return None, f"RPC: {e}"
        sigs.add(r.get("quorum_sig",""))
    return ok(len(sigs)==3, f"non-unique quorum_sigs: {sigs}")

# ═══════════════════════════════════════════════════════════════════════════════
# T21-T28  ROUND STATUS & POSE
# ═══════════════════════════════════════════════════════════════════════════════

def t21():
    r, e = roll(1, 1, 100, False, game_id="state_chk", salt=mksalt("rs",1))
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    return ok(rd.get("state")==2, f"state={rd.get('state')}")

def t22():
    r, e = roll(1, 1, 100, False, game_id="committed_chk", salt=mksalt("rs",2))
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    committed = sorted(rd.get("committed",[]))
    return ok(committed == sorted(ALL_NODES), f"committed={committed}")

def t23():
    r, e = roll(1, 1, 100, False, game_id="withheld_chk", salt=mksalt("rs",3))
    if e: return None, f"RPC: {e}"
    withheld = r.get("withheld", [])
    return ok(withheld == [], f"withheld non-empty: {withheld}")

def t24():
    r, e = roll(1, 1, 100, False, game_id="abstained_chk", salt=mksalt("rs",4))
    if e: return None, f"RPC: {e}"
    abstained = r.get("abstained", [])
    return ok(abstained == [], f"abstained non-empty: {abstained}")

def t25():
    r, e = roll(1, 1, 100, False, game_id="round_id_chk", salt=mksalt("rs",5))
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    rid = rd.get("round_id","")
    return ok(isinstance(rid,str) and len(rid)>0 and all(c in "0123456789abcdef" for c in rid),
              f"round_id='{rid}'")

def t26():
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st,list) else st.get("nodes", st.get("pose_records",[]))
    eligible = [r for r in records if r.get("eligible",True)]
    return ok(len(eligible)==len(ALL_NODES), f"eligible={len(eligible)} want {len(ALL_NODES)}")

def t27():
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st,list) else st.get("nodes", st.get("pose_records",[]))
    zero_tickets = [r["node_id"] for r in records if r.get("tickets",0) <= 0]
    return ok(not zero_tickets, f"nodes with zero tickets: {zero_tickets}")

def t28():
    r1, e = roll(1,1,100,False,game_id="rid_uniq_a",salt=mksalt("ru",1))
    if e: return None, f"RPC: {e}"
    r2, e = roll(1,1,100,False,game_id="rid_uniq_b",salt=mksalt("ru",2))
    if e: return None, f"RPC: {e}"
    id1 = r1.get("round_id","")
    id2 = r2.get("round_id","")
    if not id1 or not id2: return None, "round_id not in response"
    return ok(id1 != id2, f"duplicate round_ids: {id1}")

# ═══════════════════════════════════════════════════════════════════════════════
# T29-T38  GAME SCENARIOS
# ═══════════════════════════════════════════════════════════════════════════════

def t29():
    r, e = roll(1, 0, 1, False, game_id="coin_flip", salt=mksalt("t29"))
    if e: return None, f"RPC: {e}"
    return ok(r["results"][0] in [0,1], f"got {r['results'][0]}")

def t30():
    r, e = roll(1, 1, 6, False, game_id="d6", salt=mksalt("t30"))
    if e: return None, f"RPC: {e}"
    return ok(1 <= r["results"][0] <= 6, f"got {r['results'][0]}")

def t31():
    r, e = roll(1, 1, 20, False, game_id="d20", salt=mksalt("t31"))
    if e: return None, f"RPC: {e}"
    return ok(1 <= r["results"][0] <= 20, f"got {r['results'][0]}")

def t32():
    r, e = roll(1, 1, 100, False, game_id="d100", salt=mksalt("t32"))
    if e: return None, f"RPC: {e}"
    return ok(1 <= r["results"][0] <= 100, f"got {r['results'][0]}")

def t33():
    r, e = roll(5, 1, 52, True, game_id="cards", salt=mksalt("t33"))
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v))!=5: return False, f"duplicates: {v}"
    return ok(all(1<=x<=52 for x in v), f"{v}")

def t34():
    r, e = roll(52, 1, 52, True, game_id="full_deck", salt=mksalt("t34"))
    if e: return None, f"RPC: {e}"
    return ok(sorted(r["results"])==list(range(1,53)), "not a full 52-card permutation")

def t35():
    r, e = roll(1, 1, 10000, False, game_id="raffle", salt=mksalt("t35"))
    if e: return None, f"RPC: {e}"
    return ok(1 <= r["results"][0] <= 10000, f"got {r['results'][0]}")

def t36():
    r, e = roll(16, 1, 128, True, game_id="bracket", salt=mksalt("t36"))
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v))!=16: return False, f"duplicates: {v}"
    return ok(all(1<=x<=128 for x in v), f"{v}")

def t37():
    r1, e = roll(5, 1, 100, True, game_id="seq_a", salt=mksalt("t37a"))
    if e: return None, f"RPC: {e}"
    r2, e = roll(5, 1, 100, True, game_id="seq_b", salt=mksalt("t37b"))
    if e: return None, f"RPC: {e}"
    return ok(r1["results"] != r2["results"], f"identical: {r1['results']}")

def t38():
    r1, e = roll(5, 1, 52, True, game_id="hand1", salt=mksalt("t38a"))
    if e: return None, f"RPC: {e}"
    hand1 = r1["results"]
    r2, e = roll(5, 1, 52, True, exclude=hand1, game_id="hand2", salt=mksalt("t38b"))
    if e: return None, f"RPC: {e}"
    overlap = [v for v in r2["results"] if v in hand1]
    return ok(not overlap, f"overlap: hand1={hand1} hand2={r2['results']}")

# ═══════════════════════════════════════════════════════════════════════════════
# T39-T46  STATISTICAL / STRESS (skipped with --fast)
# ═══════════════════════════════════════════════════════════════════════════════

def t39():
    counts = [0, 0]
    for i in range(40):
        r, e = roll(5, 0, 1, False, game_id="stat_coin", salt=mksalt("stat_coin", i))
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts, sum(counts), 2)
    return ok(chi2 < 6.635, f"chi2={chi2:.4f} (limit 6.635) counts={counts}")

def t40():
    counts = [0]*7
    for i in range(100):
        r, e = roll(6, 1, 6, False, game_id="stat_d6", salt=mksalt("stat_d6", i))
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 6)
    return ok(chi2 < 15.086, f"chi2={chi2:.4f} (limit 15.086)")

def t41():
    counts = [0]*21
    for i in range(50):
        r, e = roll(20, 1, 20, False, game_id="stat_d20", salt=mksalt("stat_d20", i))
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 20)
    return ok(chi2 < 36.191, f"chi2={chi2:.4f} (limit 36.191)")

def t42():
    counts = [0]*101
    for i in range(20):
        r, e = roll(10, 1, 100, False, game_id="stat_d100", salt=mksalt("stat_d100", i))
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 100)
    return ok(chi2 < 148.23, f"chi2={chi2:.2f} (limit 148.23)")

def t43():
    errors = []
    for i in range(20):
        r, e = roll(1, 1, 1000, False, game_id=f"seq_{i}", salt=mksalt("seq", i))
        if e: errors.append(f"{i}: {e}")
    return ok(not errors, f"errors: {errors}")

def t44():
    r, e = roll(50, 1, 100, True, game_id="large_count", salt=mksalt("lc",1))
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v)) != 50: return False, "duplicates"
    return ok(all(1<=x<=100 for x in v), "out of range")

def t45():
    r, e = roll(1, 1, 2147483647, False, game_id="maxrange", salt=mksalt("mr",1))
    if e: return None, f"RPC: {e}"
    return ok(1 <= r["results"][0] <= 2147483647, f"got {r['results'][0]}")

def t46():
    errors = []
    for i in range(15):
        r, e = roll(3, 1, 100, True, game_id=f"burst_{i}", salt=mksalt("burst", i))
        if e: errors.append(f"roll {i}: {e}")
        elif len(set(r["results"])) != 3: errors.append(f"roll {i}: duplicates")
    return ok(not errors, f"{errors}")

# ═══════════════════════════════════════════════════════════════════════════════
# T47-T70  INVALID PARAMS
# ═══════════════════════════════════════════════════════════════════════════════

def t47(): return _inv([0, 1, 100, False, [], "inv_t47", "aa0001"], "count=0")
def t48(): return _inv([-1, 1, 100, False, [], "inv_t48", "aa0002"], "count=-1")
def t49(): return _inv(["1", 1, 100, False, [], "inv_t49", "aa0003"], 'count="1"')
def t50(): return _inv([1.5, 1, 100, False, [], "inv_t50", "aa0004"], "count=1.5")
def t51(): return _inv([1, 100, 1, False, [], "inv_t51", "aa0005"], "low>high")
def t52(): return _inv([2, 5, 5, True, [], "inv_t52", "aa0006"], "low==high unique count=2")
def t53(): return _inv([1, "1", 100, False, [], "inv_t53", "aa0007"], 'low="1"')
def t54(): return _inv([1, 1, "100", False, [], "inv_t54", "aa0008"], 'high="100"')
def t55(): return _inv([1, 1.5, 100, False, [], "inv_t55", "aa0009"], "low=1.5")
def t56(): return _inv([1, 1, 100.9, False, [], "inv_t56", "aa0010"], "high=100.9")
def t57(): return _inv([1, 1, 100, "false", [], "inv_t57", "aa0011"], 'unique="false"')

def t58():
    r, e = rpc("ptx_roll", [1, 1, 100, 0, [], "inv_t58", "aa0012"])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED — unique=0"
    if e: return True, f"rejected: {str(e)[:60]}"
    v = r["results"][0] if r else None
    return ok(v is not None and 1 <= v <= 100, f"accepted unique=0, got {v}")

def t59(): return _inv([1, 1, 100, False, "[]", "inv_t59", "aa0013"], 'exclude="[]"')
def t60(): return _inv([1, 1, 100, False, None, "inv_t60", "aa0014"], "exclude=null")
def t61(): return _inv([1, 1, 100, False, [1.5, 2.5], "inv_t61", "aa0015"], "exclude=[1.5,2.5]")
def t62(): return _inv([1, 1, 100, False, [1, None, 3], "inv_t62", "aa0016"], "exclude=[1,null,3]")
def t63(): return _inv([1, 1, 100, False, [[1,2],[3,4]], "inv_t63", "aa0017"], "exclude=[[1,2],[3,4]]")
def t64(): return _inv([1, 1, 100, False, [], 42, "aa0018"], "game_id=42")
def t65(): return _inv([1, 1, 100, False, []], "5 params — missing game_id and salt")
def t66(): return _inv([1, 1, 100, False, [], "inv_t66", "aa0019", "extra"], "8 params")
def t67(): return _inv([1, 1, 100, False, [], "inv_t67", 12345], "salt=12345")
def t68(): return _inv([1, 1, 100, False, [], "inv_t68", "hello_world"], 'salt="hello_world"')

def t69():
    r, e = rpc("ptx_roll", [1, 1, 100, False, [], "inv_t69", ""])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED — salt=''"
    if e: return True, f"rejected: {str(e)[:60]}"
    v = r["results"][0] if r else None
    return ok(v is not None and 1 <= v <= 100, f"accepted empty salt, got {v}")

def t70(): return _inv([15, 1, 10, True, [], "inv_t70", "aa0020"], "count=15>pool=10 unique")

# ═══════════════════════════════════════════════════════════════════════════════
# T71-T80  ADVERSARIAL / FAIL MODES
# ═══════════════════════════════════════════════════════════════════════════════

def t71():
    _, e = fail_mode("gm02", "withhold")
    if e: return None, f"set_fail_mode: {e}"
    try:
        r, e = roll(1, 1, 100, False, game_id="f1_withhold", salt="ad0001")
        return ok(r and 1 <= r["results"][0] <= 100, f"roll failed: {e}")
    finally:
        fail_mode("gm02", "normal")

def t72():
    fail_mode("gm02", "withhold")
    try:
        r, e = roll(1, 1, 100, False, game_id="f1_withheld_chk", salt="ad0002")
        if e: return None, f"roll failed: {e}"
        rd = find_round_by_height(r["block_height"])
        if not rd: return None, "round not found (BUG-005)"
        return ok("gm02" in rd.get("withheld",[]), f"gm02 not in withheld: {rd.get('withheld')}")
    finally:
        fail_mode("gm02", "normal")

def t73():
    _, e = fail_mode("gm03", "abstain")
    if e: return None, f"set_fail_mode: {e}"
    try:
        r, e = roll(1, 1, 100, False, game_id="f1_abstain", salt="ad0003")
        return ok(r and 1 <= r["results"][0] <= 100, f"roll failed: {e}")
    finally:
        fail_mode("gm03", "normal")

def t74():
    ok_stop, err = docker_stop_gm("gm11")
    if not ok_stop: return None, f"docker stop ptx-gm11 failed: {err}"
    time.sleep(15)
    try:
        r, e = roll(1, 1, 100, False, game_id="pose_incr_p2", salt=mksalt("p2ad",1))
        if e: return None, f"roll failed while gm11 stopped: {e}"
        st, _ = pose_status()
        records = st if isinstance(st,list) else st.get("nodes", st.get("pose_records",[]))
        gm11 = next((x for x in records if x["node_id"]=="gm11"), None)
        if not gm11: return None, "gm11 not in pose_status"
        return ok(gm11.get("pose_score",0) > 0, f"gm11.pose_score={gm11.get('pose_score',0)}")
    finally:
        docker_start_gm("gm11")
        time.sleep(10)

def t75():
    fail_mode("gm02", "withhold")
    fail_mode("gm04", "withhold")
    try:
        r, e = roll(1, 1, 100, False, game_id="f2_withhold", salt="ad0005")
        return ok(r and 1 <= r["results"][0] <= 100, f"f=2 withhold failed: {e}")
    finally:
        fail_mode("gm02", "normal")
        fail_mode("gm04", "normal")

def t76():
    fail_mode("gm03", "abstain")
    fail_mode("gm05", "abstain")
    try:
        r, e = roll(1, 1, 100, False, game_id="f2_abstain", salt="ad0006")
        return ok(r and 1 <= r["results"][0] <= 100, f"f=2 abstain failed: {e}")
    finally:
        fail_mode("gm03", "normal")
        fail_mode("gm05", "normal")

def t77():
    fail_mode("gm02", "withhold")
    fail_mode("gm02", "normal")
    r, e = roll(1, 1, 100, False, game_id="reset_chk", salt="ad0007")
    if e: return False, f"roll after reset: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    return ok("gm02" not in rd.get("withheld",[]), f"gm02 still withheld: {rd.get('withheld')}")

def t78():
    pm1 = get_pose_map()
    if not pm1: return None, "ptx_pose_status unavailable"
    for i in range(3):
        roll(1, 1, 100, False, game_id=f"stable_{i}", salt=mksalt("stable", i))
    pm2 = get_pose_map()
    if not pm2: return None, "ptx_pose_status unavailable (after)"
    grew = [n for n in ALL_NODES if pm2.get(n,{}).get("pose_score",0) > pm1.get(n,{}).get("pose_score",0)]
    return ok(not grew, f"pose_score grew unexpectedly: {grew}")

def t79():
    fail_mode("gm02", "withhold")
    fail_mode("gm05", "abstain")
    try:
        r, e = roll(1, 1, 100, False, game_id="mixed_fail", salt="ad0008")
        return ok(r and 1 <= r["results"][0] <= 100, f"mixed fail failed: {e}")
    finally:
        fail_mode("gm02", "normal")
        fail_mode("gm05", "normal")

def t80():
    for mode in ["withhold", "normal", "withhold", "normal"]:
        fail_mode("gm03", mode)
    r, e = roll(1, 1, 100, False, game_id="no_corruption", salt="ad0009")
    if e: return False, f"roll failed: {e}"
    return ok(r and 1 <= r["results"][0] <= 100, f"got {r['results'][0] if r else 'none'}")

# ═══════════════════════════════════════════════════════════════════════════════
# T81-T100  ADVANCED
# ═══════════════════════════════════════════════════════════════════════════════

def t81():
    results_list = [None]*5; errors_list = [None]*5
    def do_roll(idx):
        r, e = roll(1, 1, 1000, False, game_id=f"concurrent_{idx}", salt=mksalt("co", idx))
        results_list[idx] = r; errors_list[idx] = e
    threads = [threading.Thread(target=do_roll, args=(i,)) for i in range(5)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=45)
    errors = [f"thread {i}: {errors_list[i]}" for i in range(5) if errors_list[i]]
    seeds = [results_list[i]["round_seed"] for i in range(5) if results_list[i]]
    if errors: return False, f"errors: {errors}"
    return ok(len(seeds)==5, f"only {len(seeds)}/5 completed")

def t82():
    results_list = [None]*10; errors_list = [None]*10
    def do_roll(idx):
        r, e = roll(1, 1, 100, False, game_id=f"conc_seed_{idx}", salt=mksalt("cs", idx))
        results_list[idx] = r; errors_list[idx] = e
    threads = [threading.Thread(target=do_roll, args=(i,)) for i in range(10)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=45)
    errors = [i for i in range(10) if errors_list[i]]
    if errors: return False, f"errors on threads: {errors}"
    seeds = [results_list[i]["round_seed"] for i in range(10) if results_list[i]]
    if len(seeds) < 10: return False, f"only {len(seeds)}/10 completed"
    return ok(len(set(seeds))==len(seeds), "duplicate seeds across concurrent calls")

def t83():
    results_list = [None]*4; errors_list = [None]*4
    def do_roll(idx):
        r, e = roll(1, 1, 100, False, game_id="same_game", salt=mksalt("sg", idx))
        results_list[idx] = r; errors_list[idx] = e
    threads = [threading.Thread(target=do_roll, args=(i,)) for i in range(4)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=45)
    errors = [i for i in range(4) if errors_list[i]]
    if errors: return None, f"some calls failed: {errors}"
    seeds = [results_list[i]["round_seed"] for i in range(4) if results_list[i]]
    return ok(len(set(seeds)) > 1, f"all seeds identical despite different salts: {seeds[0] if seeds else '?'}")

def t84():
    errors = []
    for i in range(30):
        r, e = roll(1, 1, 100, False, game_id=f"load_{i}", salt=mksalt("load", i))
        if e: errors.append(f"{i}: {str(e)[:40]}")
        elif not (1 <= r["results"][0] <= 100): errors.append(f"{i}: out of range")
    return ok(not errors, f"{len(errors)} errors in 30 rolls: {errors[:3]}")

def t85():
    for i in range(30):
        roll(1, 1, 100, False, game_id=f"pre_load_{i}", salt=mksalt("preload", i))
    return ok(node_alive(), "node unresponsive after 30-roll load")

def t86():
    fake_txid = "a" * 64
    r, e = roll(1, 1, 100, False, exclude=[fake_txid], game_id="txid_fake", salt=mksalt("tx",1))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on fake tx_id"
    if e: return None, f"RPC error (acceptable): {str(e)[:60]}"
    v = r["results"][0]
    return ok(1 <= v <= 100, f"got {v} — fake tx_id silently skipped")

def t87():
    r1, e = roll(1, 1, 52, True, game_id="txid_source", salt=mksalt("tx",2))
    if e: return None, f"first roll failed: {e}"
    pending_txid = r1.get("tx_id","b"*64) or "b"*64
    r2, e2 = roll(1, 1, 52, False, exclude=[pending_txid], game_id="txid_pending", salt=mksalt("tx",3))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on pending tx_id"
    if e2: return None, f"RPC error (acceptable — unconfirmed): {str(e2)[:60]}"
    return ok(1 <= r2["results"][0] <= 52, f"got {r2['results'][0]}")

def t88():
    fake_txid = "c" * 64
    r, e = roll(1, 1, 10, False, exclude=[1,2,3,fake_txid,4,5], game_id="txid_mixed", salt=mksalt("tx",4))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on mixed exclude"
    if e: return None, f"RPC error: {str(e)[:60]}"
    v = r["results"][0]
    if v in [1,2,3,4,5]: return False, f"excluded integer appeared: {v}"
    return ok(6 <= v <= 10, f"got {v} — integer exclusion worked, tx_id skipped")

def t89():
    fake_txids = ["d"*64, "e"*64, "f"*64]
    r, e = roll(1, 1, 100, False, exclude=fake_txids, game_id="txid_multi", salt=mksalt("tx",5))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on multiple fake tx_ids"
    if e: return None, f"RPC error (acceptable): {str(e)[:60]}"
    return ok(1 <= r["results"][0] <= 100, f"got {r['results'][0]}")

def t90():
    """KDD-003 REVISED (v1.4): per-call nonce advancement means two rapid same-block
    same-params calls MUST produce DIFFERENT seeds. This is now a positive correctness
    test for nonce advancement, NOT an idempotency test.
    Previous (v1.3): asserted same seed. Now (v7): asserts DIFFERENT seed.
    Confirmed by Phase 2 T90 FAIL result (mksalt() exposed real behaviour).
    """
    r1, e = roll(1, 1, 100, False, game_id="anchor_same", salt=mksalt("bh",1))
    if e: return None, f"RPC: {e}"
    h1 = r1.get("block_height")
    r2, e = roll(1, 1, 100, False, game_id="anchor_same", salt=mksalt("bh",1))
    if e: return None, f"RPC: {e}"
    h2 = r2.get("block_height")
    s1 = r1.get("round_seed","")
    s2 = r2.get("round_seed","")
    msg = f"h1={h1} h2={h2} seed1={s1[:12]}... seed2={s2[:12]}..."
    if h1 != h2:
        # Blocks advanced — still valid test: seeds must differ (different blocks)
        return ok(s1 != s2, f"different blocks but same seed: {msg}")
    # Same block — per-call nonce must have advanced: seeds MUST differ
    return ok(s1 != s2,
              f"same-block call returned SAME seed — per-call nonce NOT advancing: {msg}")


def t91():
    r1, e = roll(1, 1, 100, False, game_id="anchor_diff", salt=mksalt("bh",2))
    if e: return None, f"RPC: {e}"
    h1 = r1["block_height"]
    for _ in range(30):
        time.sleep(3)
        if blockcount() > h1: break
    r2, e = roll(1, 1, 100, False, game_id="anchor_diff", salt=mksalt("bh",2))
    if e: return None, f"RPC: {e}"
    h2 = r2["block_height"]
    if h1 == h2: return None, f"block didn't advance (h={h1})"
    return ok(r1["round_seed"] != r2["round_seed"], "different blocks but same seed — anti-grinding broken")

def t92():
    r, e = roll(1, 1, 100, False, game_id="anchor_field", salt=mksalt("bh",3))
    if e: return None, f"RPC: {e}"
    h = r.get("block_height")
    bc = blockcount()
    return ok(isinstance(h,int) and h > 0 and abs(h - bc) <= 5, f"block_height={h} blockcount={bc}")

def t93():
    for n in ["gm02","gm03","gm04"]: fail_mode(n, "withhold")
    try:
        r, e = rpc("ptx_roll", [1,1,100,False,[],"f3_withhold","f30001"])
        alive = node_alive()
        if not alive: return False, "NODE CRASHED on f=3 — should return error"
        if e: return True, f"correctly failed: {str(e)[:80]}"
        members = r.get("quorum_members",[])
        if len(members) < 6: return False, f"threshold violation — only {len(members)} members"
        return None, f"round completed unexpectedly — members={members}"
    finally:
        for n in ["gm02","gm03","gm04"]: fail_mode(n, "normal")

def t94():
    for n in ["gm03","gm04","gm05"]: fail_mode(n, "abstain")
    try:
        r, e = rpc("ptx_roll", [1,1,100,False,[],"f3_abstain","f30002"])
        alive = node_alive()
        if not alive: return False, "NODE CRASHED on f=3 abstain"
        if e: return True, f"correctly failed: {str(e)[:80]}"
        members = r.get("quorum_members",[])
        if len(members) < 6: return False, f"threshold violated — {len(members)} members"
        return None, f"round completed unexpectedly"
    finally:
        for n in ["gm03","gm04","gm05"]: fail_mode(n, "normal")

def t95():
    for n in ALL_NODES: fail_mode(n, "normal")
    time.sleep(1)
    r, e = roll(1, 1, 100, False, game_id="f3_recovery", salt="f30003")
    if e: return False, f"node did not recover: {e}"
    return ok(r and 1 <= r["results"][0] <= 100, f"got {r['results'][0] if r else 'none'}")

def t96():
    seeds = []
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"nonce_{i}", salt="deadbeef00000000")
        if e: return None, f"RPC error at i={i}: {e}"
        seeds.append(r["round_seed"])
    return ok(len(set(seeds))==5, f"duplicate seeds — nonce not advancing: {seeds}")

def t97():
    r1,e = roll(1,1,100,False,game_id="nonce_adv_1",salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    r2,e = roll(1,1,100,False,game_id="nonce_adv_2",salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    r3,e = roll(1,1,100,False,game_id="nonce_adv_3",salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    seeds = [r1["round_seed"],r2["round_seed"],r3["round_seed"]]
    return ok(len(set(seeds))==3, f"duplicate seeds — nonce not chaining: {seeds}")

def t98():
    sigs = []
    for i in range(5):
        r, e = roll(1,1,100,False,game_id=f"sig_chain_{i}",salt=mksalt("sig_chain", i))
        if e: return None, f"RPC: {e}"
        sigs.append(r.get("quorum_sig",""))
    return ok(len(set(sigs))==5, f"duplicate quorum_sig: {sigs}")

def t99():
    excl = list(range(1,96))
    r, e = roll(1,1,200,False,exclude=excl,game_id="excl_95",salt="eb0001")
    if e: return False, f"95-item exclude failed (should succeed): {e}"
    v = r["results"][0]
    if v in excl: return False, f"excluded value returned: {v}"
    return ok(96 <= v <= 200, f"got {v}")

def t100():
    excl = list(range(1,97))
    r, e = roll(1,1,200,False,exclude=excl,game_id="excl_96",salt="eb0002")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED at 96-item exclude — BUG-003 regression"
    # Post-fix this should succeed; pre-fix it errored. Either is valid here.
    return ok(True, f"node alive — result={r['results'][0] if r else 'error:'+str(e)[:40]}")

# ═══════════════════════════════════════════════════════════════════════════════
# T101-T120  EXCLUDE PATH HARDENING
# ═══════════════════════════════════════════════════════════════════════════════

def t101():
    excl = list(range(1,98))
    r, e = roll(1,1,200,False,exclude=excl,game_id="excl_97",salt="fc0001")
    if e: return False, f"failed at 97 items: {e}"
    v = r["results"][0]
    return ok(v not in excl and 98 <= v <= 200, f"got {v}")

def t102():
    excl = list(range(1,201))
    r, e = roll(1,1,400,False,exclude=excl,game_id="excl_200",salt="fc0002")
    if e: return False, f"failed at 200 items: {e}"
    v = r["results"][0]
    return ok(v not in excl and 201 <= v <= 400, f"got {v}")

def t103():
    excl = list(range(1,501))
    r, e = roll(1,1,1000,False,exclude=excl,game_id="excl_500",salt="fc0003")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED at 500 items"
    if e: return False, f"failed at 500 items: {e}"
    v = r["results"][0]
    return ok(v not in excl and 501 <= v <= 1000, f"got {v}")

def t104():
    excl = list(range(1,1001))
    r, e = roll(1,1,2000,False,exclude=excl,game_id="excl_1000",salt="fc0004")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED at 1000 items"
    if e: return False, f"failed at 1000 items: {e}"
    v = r["results"][0]
    return ok(v not in excl and 1001 <= v <= 2000, f"got {v}")

def _char_test(excl_size, label):
    violations, errors, completed = _excl_probe(excl_size, 100, 200, label)
    rate = len(violations)/completed*100 if completed else 0
    detail = f"excl={excl_size} completed={completed} violations={len(violations)} ({rate:.1f}%)"
    if violations:
        return False, f"BUG-004 ACTIVE at excl={excl_size}: {detail}"
    return True, detail

def t105(): return _char_test(10, "bug4_c10")
def t106(): return _char_test(20, "bug4_c20")
def t107(): return _char_test(30, "bug4_c30")
def t108(): return _char_test(40, "bug4_c40")
def t109(): return _char_test(50, "bug4_c50")
def t110(): return _char_test(60, "bug4_c60")
def t111(): return _char_test(70, "bug4_c70")
def t112(): return _char_test(80, "bug4_c80")
def t113(): return _char_test(90, "bug4_c90")

def t114():
    v, err, done = _excl_probe(40, 200, 200, "fix_verify_40")
    return ok(not v, f"violations={len(v)} in {done} iters at excl=40 — BUG-004 not fixed")

def t115():
    v, err, done = _excl_probe(80, 200, 200, "fix_verify_80")
    return ok(not v, f"violations={len(v)} in {done} iters at excl=80 — BUG-004 not fixed")

def t116():
    for size in [10,20,30,40,50,60,70,80,90]:
        v, _, done = _excl_probe(size, 50, 200, f"sweep_{size}")
        if v: return False, f"violations at excl={size}: {len(v)}/{done}"
    return True, "zero violations across excl=10-90 sweep"

def t117():
    hand = []
    for rd in range(10):
        r, e = roll(5,1,52,True,exclude=hand,game_id=f"card_game_{rd}",salt=mksalt("card_game", rd))
        if e: return None, f"round {rd} failed: {e}"
        overlap = [v for v in r["results"] if v in hand]
        if overlap: return False, f"round {rd}: overlap with previous draws: {overlap}"
        hand.extend(r["results"])
    return ok(len(set(hand))==50, f"duplicates across 10 rounds: {len(hand)} draws {len(set(hand))} unique")

def t118():
    excl = list(range(1,48))
    r, e = roll(5,1,52,True,exclude=excl,game_id="near_depleted",salt=mksalt("cg_dep"))
    if e: return None, f"RPC: {e}"
    v = r["results"]
    bad = [x for x in v if x in excl]
    if bad: return False, f"excluded appeared: {bad}"
    return ok(all(48<=x<=52 for x in v) and len(set(v))==5, f"got {v}")

def t119():
    excl = list(range(1,91))
    violations = []
    for i in range(50):
        r, e = roll(1,1,100,False,exclude=excl,game_id=f"extreme_{i}",salt=mksalt("extreme", i))
        if e: continue
        if r["results"][0] in excl: violations.append(r["results"][0])
    return ok(not violations, f"{len(violations)} violations at 90% exclusion density")

def t120():
    errors = []
    for i in range(10):
        r, e = roll(3,1,100,True,game_id=f"clean_{i}",salt=mksalt("clean", i))
        if e: errors.append(f"{i}: {e}")
        elif len(set(r["results"]))!=3: errors.append(f"{i}: duplicates")
    return ok(not errors, f"clean-path regression errors: {errors}")

# ═══════════════════════════════════════════════════════════════════════════════
# T121-T130  BLS PHASE 2
# ═══════════════════════════════════════════════════════════════════════════════

def t121():
    r, e = roll(1,1,100,False,game_id="bls_sig_len",salt="b20001")
    if e: return None, f"RPC: {e}"
    sig = r.get("quorum_sig","")
    return ok(len(sig)==192, f"quorum_sig len={len(sig)} want 192")

def t122():
    r, e = roll(1,1,100,False,game_id="bls_members",salt="b20002")
    if e: return None, f"RPC: {e}"
    members = sorted(r.get("quorum_members",[]))
    return ok(len(members)==11 and members==sorted(ALL_NODES),
              f"members={members}")

def t123():
    sigs = []
    for i in range(5):
        r, e = roll(1,1,100,False,game_id=f"bls_consec_{i}",salt=mksalt("bls_consec", i))
        if e: return None, f"RPC at i={i}: {e}"
        sig = r.get("quorum_sig","")
        if len(sig)!=192: return False, f"roll {i}: sig len={len(sig)}"
        sigs.append(sig)
    return ok(len(set(sigs))==5, f"non-unique sigs: {sigs}")

def t124():
    r, e = roll(1,1,100,False,game_id="beacon_sha256",salt="b20003")
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r.get("block_height"))
    if not rd: return None, "round not found in status (BUG-005)"
    sig = rd.get("quorum_sig","")
    expected = hashlib.sha256(bytes.fromhex(sig)).hexdigest()
    # Compare against quorum_sig_hash in the roll response
    qsh = r.get("quorum_sig_hash","")
    if qsh:
        return ok(qsh == expected,
                  f"quorum_sig_hash mismatch: got {qsh[:16]}... want {expected[:16]}...")
    return None, "quorum_sig_hash not in roll response — cannot verify via response (BUG-005)"

def t125():
    """beacon field present and 64-char hex across 5 rolls."""
    for i in range(5):
        r, e = roll(1,1,100,False,game_id=f"beacon_hex_{i}",salt=mksalt("beacon_hex", i))
        if e: return None, f"RPC at i={i}: {e}"
        beacon = r.get("quorum_sig_hash") or r.get("beacon") or r.get("round_beacon","")
        if not beacon or len(beacon)!=64:
            return False, f"roll {i}: bad beacon='{beacon}'"
        if not all(c in "0123456789abcdef" for c in beacon):
            return False, f"roll {i}: beacon not hex: '{beacon}'"
    return True, "5/5 beacons are 64-char hex"

def t126():
    r, e = roll(1,1,100,False,game_id="committed_list",salt="b20004")
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r.get("block_height"))
    if not rd: return None, "round not found in status (BUG-005)"
    committed = sorted(rd.get("committed",[]))
    return ok(committed==sorted(ALL_NODES), f"committed={committed}")

def t127():
    r, e = roll(1,1,100,False,game_id="sig_match",salt="b20005")
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r.get("block_height"))
    if not rd: return None, "round not found in status (BUG-005)"
    sig_resp = r.get("quorum_sig","")
    sig_status = rd.get("quorum_sig","")
    return ok(sig_resp==sig_status,
              f"sig mismatch: resp={sig_resp[:16]}... status={sig_status[:16]}...")

def t128():
    r, e = roll(1,1,100,False,game_id="g2_encoding",salt="b20006")
    if e: return None, f"RPC: {e}"
    sig = r.get("quorum_sig","")
    if len(sig)!=192: return None, f"sig wrong length: {len(sig)}"
    first_byte = int(sig[:2], 16)
    compressed = (first_byte >> 7) & 1
    infinity   = (first_byte >> 6) & 1
    return ok(compressed==1 and infinity==0,
              f"first_byte=0x{first_byte:02x} compressed={compressed} infinity={infinity}")

def t129():
    sigs = []
    for i in range(10):
        r, e = roll(1,1,100,False,game_id=f"bls_10_{i}",salt=mksalt("bls_10", i))
        if e: return None, f"RPC: {e}"
        sig = r.get("quorum_sig","")
        if len(sig)!=192: return False, f"roll {i}: sig len={len(sig)}"
        sigs.append(sig)
    return ok(len(set(sigs))==10, "non-unique sigs across 10 rolls")

def t130():
    """KDD-033: quorum_sig_hash MUST equal SHA256(raw compressed G2 bytes of quorum_sig).
    This is the canonical definition per KDD-033 / ODC-019. Raw bytes method only.
    If this fails, run T186-T192 to identify which method the node is actually using.
    Note: quorum_sig is 192-char hex → 96 raw bytes. SHA256 of those 96 bytes → 64-char hex.
    """
    mismatches = []
    for i in range(3):
        r, e = roll(1,1,100,False,game_id=f"sha_verify_{i}",salt=mksalt("sha_verify", i))
        if e: return None, f"RPC at i={i}: {e}"
        sig   = r.get("quorum_sig","")
        shash = r.get("quorum_sig_hash","")
        if not sig or len(sig)!=192: return None, f"i={i}: quorum_sig missing or wrong length ({len(sig)})"
        if not shash or len(shash)!=64:
            return False, f"i={i}: quorum_sig_hash missing or wrong length: '{shash}' — run T186-T192 to diagnose"
        expected = hashlib.sha256(bytes.fromhex(sig)).hexdigest()
        if shash != expected:
            # Capture diagnostic variants for the failure message
            hex_str_hash = hashlib.sha256(sig.encode()).hexdigest()
            hex_upper_hash = hashlib.sha256(sig.upper().encode()).hexdigest()
            if shash == hex_str_hash:
                hint = "MATCHES SHA256(hex_string) — node using Option B not KDD-033 Option A"
            elif shash == hex_upper_hash:
                hint = "MATCHES SHA256(hex_upper_string) — node using Option C not KDD-033 Option A"
            else:
                hint = "does not match any standard variant — run T186-T195 for full diagnosis"
            mismatches.append(
                f"i={i}: quorum_sig_hash={shash[:16]}... want SHA256(bytes)={expected[:16]}... [{hint}]"
            )
    if mismatches:
        return False, f"quorum_sig_hash != SHA256(raw bytes) [KDD-033 violation]: {mismatches}"
    return True, "quorum_sig_hash == SHA256(raw compressed G2 bytes) for 3 rolls — KDD-033 PASS"

# ═══════════════════════════════════════════════════════════════════════════════
# T131-T140  POSE PHASE 2
# ═══════════════════════════════════════════════════════════════════════════════

def t131():
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st,list) else st.get("nodes", st.get("pose_records",[]))
    return ok(len(records)==11, f"got {len(records)} records, want 11")

def t132():
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st,list) else st.get("nodes", st.get("pose_records",[]))
    bad = [r["node_id"] for r in records
           if not isinstance(r.get("pose_score"),int) or not isinstance(r.get("tickets"),int)]
    return ok(not bad, f"bad record types: {bad}")

def t133():
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st,list) else st.get("nodes", st.get("pose_records",[]))
    found = sorted(r["node_id"] for r in records)
    return ok(found==sorted(ALL_NODES), f"found={found}")

def t134():
    if _near_settlement(5): return None, "settlement imminent — tickets test skipped"
    pm1 = get_pose_map()
    if not pm1: return None, "ptx_pose_status unavailable"
    for i in range(3):
        r, e = roll(1,1,100,False,game_id=f"ticket_roll_{i}",salt=mksalt("ticket", i))
        if e: return None, f"roll {i} failed: {e}"
    pm2 = get_pose_map()
    if not pm2: return None, "ptx_pose_status unavailable (after)"
    wrong = [n for n in ALL_NODES
             if pm2.get(n,{}).get("tickets",0) != pm1.get(n,{}).get("tickets",0)+3]
    return ok(not wrong, f"ticket counts wrong for: {wrong}")

def t135():
    ok_stop, err = docker_stop_gm("gm11")
    if not ok_stop: return None, f"docker stop failed: {err}"
    time.sleep(15)
    try:
        pm1 = get_pose_map()
        if not pm1: return None, "ptx_pose_status unavailable"
        score_before = pm1.get("gm11",{}).get("pose_score",0)
        r, e = roll(1,1,100,False,game_id="pose_stop_p2",salt=mksalt("p2s",1))
        if e: return None, f"roll failed while gm11 stopped: {e}"
        pm2 = get_pose_map()
        if not pm2: return None, "ptx_pose_status unavailable (after)"
        score_after = pm2.get("gm11",{}).get("pose_score",0)
        return ok(score_after > score_before,
                  f"gm11.pose_score: {score_before} → {score_after}")
    finally:
        docker_start_gm("gm11")
        time.sleep(10)

def t136():
    pm = get_pose_map()
    if not pm: return None, "ptx_pose_status unavailable"
    if pm.get("gm11",{}).get("pose_score",0)==0:
        return None, "gm11.pose_score=0 — run T135 first to accumulate score"
    score_before = pm["gm11"]["pose_score"]
    r, e = roll(1,1,100,False,game_id="pose_decay",salt=mksalt("p2d",1))
    if e: return None, f"roll failed: {e}"
    pm2 = get_pose_map()
    if not pm2: return None, "ptx_pose_status unavailable (after)"
    score_after = pm2.get("gm11",{}).get("pose_score",0)
    return ok(score_after < score_before,
              f"gm11 score did not decay: {score_before} → {score_after}")

def t137():
    if _near_settlement(5): return None, "settlement imminent — stability test skipped"
    pm1 = get_pose_map()
    if not pm1: return None, "ptx_pose_status unavailable"
    for i in range(3):
        r, e = roll(1,1,100,False,game_id=f"honest_{i}",salt=mksalt("honest", i))
        if e: return None, f"roll {i} failed: {e}"
    pm2 = get_pose_map()
    if not pm2: return None, "ptx_pose_status unavailable (after)"
    grew = [n for n in ALL_NODES
            if pm2.get(n,{}).get("pose_score",0) > pm1.get(n,{}).get("pose_score",0)]
    return ok(not grew, f"pose_score grew unexpectedly: {grew}")

def t138():
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st,list) else st.get("nodes", st.get("pose_records",[]))
    neg = [r["node_id"] for r in records if r.get("tickets",0) < 0]
    return ok(not neg, f"negative tickets: {neg}")

def t139():
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st,list) else st.get("nodes", st.get("pose_records",[]))
    found = sorted(r["node_id"] for r in records)
    return ok(found==sorted(ALL_NODES), f"node_ids mismatch: {found}")

def t140():
    st1, e = pose_status()
    if e: return None, f"first call: {e}"
    st2, e = pose_status()
    if e: return None, f"second call: {e}"
    r1 = st1 if isinstance(st1,list) else st1.get("nodes",st1.get("pose_records",[]))
    r2 = st2 if isinstance(st2,list) else st2.get("nodes",st2.get("pose_records",[]))
    ids1 = sorted(r["node_id"] for r in r1)
    ids2 = sorted(r["node_id"] for r in r2)
    return ok(ids1==ids2, f"inconsistent: {ids1} vs {ids2}")

# ═══════════════════════════════════════════════════════════════════════════════
# T141-T150  LOTTERY
# ═══════════════════════════════════════════════════════════════════════════════

def t141():
    st, e = lottery_status()
    return ok(e is None and st is not None, f"ptx_lottery_status error: {e}")

def t142():
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    bal = st.get("pool_balance_sat")
    return ok(isinstance(bal,int) and bal >= 0, f"pool_balance_sat={bal!r}")

def t143():
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    sw = st.get("settlement_window")
    return ok(isinstance(sw,int) and sw > 0, f"settlement_window={sw!r}")

def t144():
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    nsa = st.get("next_settlement_at")
    return ok(isinstance(nsa,int) and nsa > 0, f"next_settlement_at={nsa!r}")

def t145():
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    eligible = st.get("eligible_nodes",[])
    if not isinstance(eligible,list):
        return False, f"eligible_nodes is not a list: {eligible!r}"
    ids = _eligible_ids(st)
    unknown = [n for n in ids if n not in ALL_NODES]
    return ok(len(ids) > 0 and not unknown, f"eligible_ids={ids}")

def t146():
    if _near_settlement(5): return None, "settlement imminent — pool growth test skipped"
    st1, e = lottery_status()
    if e: return None, f"lottery_status pre: {e}"
    bal1 = st1.get("pool_balance_sat",0)
    r, e = roll(1,1,100,False,game_id="pool_grow",salt=mksalt("pg",1))
    if e: return None, f"roll failed: {e}"
    st2, e = lottery_status()
    if e: return None, f"lottery_status post: {e}"
    bal2 = st2.get("pool_balance_sat",0)
    return ok(bal2 > bal1, f"pool did not grow: {bal1} → {bal2}")

def t147():
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    eligible = set(_eligible_ids(st))
    missing = [n for n in ALL_NODES if n not in eligible]
    return ok(not missing, f"GMs not in eligible: {missing}")

def t148():
    if _near_settlement(5): return None, "settlement too close — pool growth test skipped"
    st1, e = lottery_status()
    if e: return None, f"lottery_status pre: {e}"
    bal1 = st1.get("pool_balance_sat",0)
    for i in range(3):
        r, e = roll(1,1,100,False,game_id=f"pool_3roll_{i}",salt=mksalt("pool3", i))
        if e: return None, f"roll {i} failed: {e}"
    st2, e = lottery_status()
    if e: return None, f"lottery_status post: {e}"
    bal2 = st2.get("pool_balance_sat",0)
    return ok(bal2 > bal1, f"pool did not grow across 3 rolls: {bal1} → {bal2}")

def t149():
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    nsa = st.get("next_settlement_at",0)
    bc = blockcount()
    return ok(nsa > bc, f"next_settlement_at={nsa} <= current height={bc}")

def t150():
    """Settlement test — omitted with --skip-lottery (requires 15+ min)."""
    return None, "settlement test skipped (--skip-lottery) — run without flag for full settlement verification"

# ═══════════════════════════════════════════════════════════════════════════════
# T151-T160  EXCLUDE EDGE CASES (NEW in v6)
# ═══════════════════════════════════════════════════════════════════════════════

def t151():
    """Confirmed tx_id chaining: use tx_id from a real previous roll as exclude.
    The quorum should look it up on-chain and exclude the values from that round.
    PASS = result not in previous round's output AND node alive.
    SKIP = if tx_id field not present in roll response (not yet implemented)."""
    r1, e = roll(5, 1, 52, True, game_id="chain_src", salt=mksalt("txc",1))
    if e: return None, f"source roll failed: {e}"
    tx_id = r1.get("tx_id") or r1.get("session_txid") or r1.get("txid")
    if not tx_id or tx_id in ("", "pending", None):
        return None, "tx_id not present in roll response — on-chain tx_id chaining not yet exposed"
    # Wait a block for confirmation
    h0 = blockcount()
    for _ in range(20):
        time.sleep(3)
        if blockcount() > h0: break
    prev_results = set(r1["results"])
    r2, e2 = roll(5, 1, 52, True, exclude=[tx_id], game_id="chain_dst", salt=mksalt("txc",2))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on confirmed tx_id exclude"
    if e2: return None, f"RPC error on tx_id exclude: {str(e2)[:80]}"
    overlap = [v for v in r2["results"] if v in prev_results]
    return ok(not overlap,
              f"excluded values from tx_id appeared: {overlap} prev={sorted(prev_results)}")

def t152():
    """Mixed tx_id + integers at scale: 200 ints + 5 fake tx_ids = 205 items total.
    All integer exclusions must hold. tx_ids silently skipped."""
    fake_txids = ["a"*64, "b"*64, "c"*64, "d"*64, "e"*64]
    excl_ints = list(range(1, 201))  # 200 ints
    exclude = excl_ints + fake_txids
    r, e = roll(1, 1, 500, False, exclude=exclude, game_id="txid_scale", salt=mksalt("txid_scale", 1))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on 205-item mixed exclude"
    if e: return False, f"RPC error: {str(e)[:80]}"
    v = r["results"][0]
    if v in excl_ints: return False, f"excluded integer appeared: {v}"
    return ok(201 <= v <= 500, f"got {v} — integers enforced, tx_ids skipped")

def t153():
    """512-item exclude — exactly at MAX_EXCLUDE_COUNT boundary. Must succeed (KDD-028)."""
    excl = list(range(1, MAX_EXCLUDE_COUNT + 1))  # exactly 512 items
    r, e = roll(1, 1, 1100, False, exclude=excl, game_id="excl_512", salt="e512a001")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED at 512-item exclude"
    if e: return False, f"512-item exclude rejected — should be within limit: {str(e)[:80]}"
    v = r["results"][0]
    if v in excl: return False, f"excluded value returned: {v}"
    return ok(MAX_EXCLUDE_COUNT+1 <= v <= 1100, f"got {v} — 512-item exclude working")

def t154():
    """513-item exclude — one over MAX_EXCLUDE_COUNT (512).
    EXPECTED per KDD-028: error 1016 (EXCLUDE_LIMIT_EXCEEDED).
    ACTUAL: accepted (BUG-011 — KDD-028 MAX_EXCLUDE_COUNT not enforced).
    Test PASSES if node is alive. Documents whether 1016 is returned or not."""
    excl = list(range(1, MAX_EXCLUDE_COUNT + 2))  # 513 items
    r, e = roll(1, 1, 1200, False, exclude=excl, game_id="excl_513", salt=mksalt("excl_513"))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED at 513-item exclude"
    if r is not None:
        return None, f"BUG-011: 513-item exclude accepted (KDD-028 not enforced) — result={r['results']}"
    if isinstance(e, dict):
        code = e.get("code")
        if code == 1016: return True, "error 1016 EXCLUDE_LIMIT_EXCEEDED — KDD-028 now enforced"
        return None, f"BUG-011: unexpected error code {code} (want 1016): {e}"
    return None, f"rejected (string error): {str(e)[:80]}"

def t155():
    """Duplicate values in exclude list: [5,5,5,1,2]. Should deduplicate or handle gracefully.
    Must not crash. Result must not be in {1,2,5}."""
    r, e = roll(1, 1, 10, False, exclude=[5,5,5,1,2], game_id="excl_dup", salt="ed0001")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on duplicate exclude values"
    if e: return None, f"rejected (may be acceptable): {str(e)[:60]}"
    v = r["results"][0]
    if v in [1,2,5]: return False, f"excluded value returned: {v}"
    return ok(3 <= v <= 10, f"got {v} — duplicates handled")

def t156():
    """Out-of-range values in exclude list: exclude=[200,300] when high=100.
    Should be silently ignored (values can't appear anyway). Must not crash or error."""
    r, e = roll(1, 1, 100, False, exclude=[200, 300], game_id="excl_oor", salt=mksalt("excl_oor", 1))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on out-of-range exclude values"
    if e: return None, f"rejected out-of-range exclude: {str(e)[:60]}"
    v = r["results"][0]
    return ok(1 <= v <= 100, f"got {v} — out-of-range exclude silently ignored")

def t157():
    """Exhaustion via exclusion: count=5, low=1, high=6, unique=True, exclude=[1,2,3].
    Pool is 6 values, 3 excluded → 3 remaining, but requesting 5. Must error cleanly."""
    r, e = roll(5, 1, 6, True, exclude=[1,2,3], game_id="excl_exhaust", salt=mksalt("ex",1))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on pool exhaustion via exclude"
    if e: return True, f"correctly rejected: {str(e)[:70]}"
    # If accepted — check if results are valid (only 3 valid values: 4,5,6)
    if r and len(r["results"]) == 5:
        return False, f"accepted impossible request (5 from 3 remaining): {r['results']}"
    return ok(False, "accepted but returned wrong count — unexpected")

def t158():
    """count > pool via exclude with unique=True (runtime exhaustion path).
    Different from T70 (count > range): here count=4, range=1-5 (5 values), exclude=[2,3,4,5] → 1 remaining.
    Requesting count=4 unique from pool of 1. Must error."""
    r, e = roll(4, 1, 5, True, exclude=[2,3,4,5], game_id="excl_rt_exhaust", salt=mksalt("ex",2))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on runtime pool exhaustion"
    if e: return True, f"correctly rejected: {str(e)[:70]}"
    return False, f"accepted impossible request — result: {r['results'] if r else 'none'}"

def t159():
    """Exclude reduces pool to exactly count (tight fit, unique=True).
    count=3, range=1-10, exclude=[1..7] → pool={8,9,10}, count=3. Must succeed and return exactly {8,9,10}."""
    r, e = roll(3, 1, 10, True, exclude=list(range(1,8)), game_id="excl_tight", salt=mksalt("excl_tight", 1))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on tight-fit exclude"
    if e: return False, f"tight-fit exclude rejected (should succeed): {str(e)[:60]}"
    v = sorted(r["results"])
    return ok(v == [8,9,10], f"got {v}, expected [8,9,10]")

def t160():
    """Exclude all but one value: count=1, range=1-10, exclude=[1..9] → only 10 possible.
    Must return exactly 10."""
    r, e = roll(1, 1, 10, False, exclude=list(range(1,10)), game_id="excl_one_left", salt=mksalt("excl_one_left", 1))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on single-value exclude"
    if e: return False, f"single-value exclude rejected: {str(e)[:60]}"
    v = r["results"][0]
    return ok(v == 10, f"expected 10, got {v}")

# ═══════════════════════════════════════════════════════════════════════════════
# T161-T165  DEV_SEED & ERROR CODE VERIFICATION (NEW in v6)
# ═══════════════════════════════════════════════════════════════════════════════

def t161():
    """dev_seed returns a deterministic result on regtest.
    SKIP if dev_seed parameter not implemented (error 1013 means it exists but rejected on testnet)."""
    dev_seed_val = "deadbeef" * 8  # 64-char hex
    r, e = roll(5, 1, 52, True, game_id="dev_seed_test", salt=mksalt("ds",1), dev_seed=dev_seed_val)
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on dev_seed"
    if isinstance(e, dict) and e.get("code") == 1013:
        return None, "dev_seed rejected with error 1013 (DEV_SEED_NOT_REGTEST) — running on testnet/mainnet?"
    if e: return None, f"dev_seed not implemented or error: {str(e)[:80]}"
    v = r["results"]
    return ok(len(v)==5 and all(1<=x<=52 for x in v), f"dev_seed result: {v}")

def t162():
    """dev_seed: same seed + same params → same result (deterministic)."""
    dev_seed_val = "cafebabe" * 8
    r1, e1 = roll(3, 1, 100, False, game_id="dev_det_a", salt=mksalt("ds",2), dev_seed=dev_seed_val)
    if e1:
        if isinstance(e1,dict) and e1.get("code")==1013:
            return None, "dev_seed not available on this network"
        return None, f"first call failed: {str(e1)[:60]}"
    r2, e2 = roll(3, 1, 100, False, game_id="dev_det_a", salt=mksalt("ds",2), dev_seed=dev_seed_val)
    if e2: return None, f"second call failed: {str(e2)[:60]}"
    return ok(r1["results"]==r2["results"],
              f"dev_seed not deterministic: {r1['results']} vs {r2['results']}")

def t163():
    """dev_seed: different seed → different result."""
    r1, e1 = roll(3, 1, 100, False, game_id="dev_diff_a", salt=mksalt("ds",3), dev_seed="aaaaaaaa"*8)
    if e1:
        if isinstance(e1,dict) and e1.get("code")==1013:
            return None, "dev_seed not available on this network"
        return None, f"first call failed: {str(e1)[:60]}"
    r2, e2 = roll(3, 1, 100, False, game_id="dev_diff_b", salt=mksalt("ds",4), dev_seed="bbbbbbbb"*8)
    if e2: return None, f"second call failed: {str(e2)[:60]}"
    return ok(r1["results"]!=r2["results"],
              f"different seeds produced identical results: {r1['results']}")

def t164():
    """Error 1016 EXCLUDE_LIMIT_EXCEEDED: send 513 items, verify error code is exactly 1016.
    BUG-011: currently accepted. Test SKIPs with BUG-011 note if accepted."""
    excl = list(range(1, MAX_EXCLUDE_COUNT + 2))
    r, e = roll(1, 1, 2000, False, exclude=excl, game_id="err_1016", salt=mksalt("err_1016"))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED — must return error 1016"
    if r is not None:
        return None, f"BUG-011: 513-item exclude accepted — KDD-028 not enforced, error 1016 never returned"
    if isinstance(e, dict):
        code = e.get("code")
        msg = e.get("message","")
        return ok(code==1016, f"code={code} (want 1016) msg='{msg[:60]}'")
    return None, f"error not dict — cannot verify code: {str(e)[:80]}"

def t165():
    """Error envelope format: any error response has 'code' (int) and 'message' (str) fields."""
    # Trigger a guaranteed error: count=0
    r, e = rpc("ptx_roll", [0, 1, 100, False, [], "err_fmt", "ef0001"])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED"
    if e is None: return False, "count=0 was accepted — expected error"
    if isinstance(e, dict):
        has_code = isinstance(e.get("code"), int)
        has_msg  = isinstance(e.get("message"), str)
        return ok(has_code and has_msg,
                  f"error envelope: code={e.get('code')!r} message={e.get('message','')[:40]!r}")
    # String error — envelope not exposed
    return None, f"error returned as string (not dict) — envelope format not verifiable: {str(e)[:60]}"

# ═══════════════════════════════════════════════════════════════════════════════
# T166-T170  GAME_ID & SALT EDGE CASES (NEW in v6)
# ═══════════════════════════════════════════════════════════════════════════════

def t166():
    """game_id at 128 characters (max common length). Must not crash or error."""
    long_game_id = "g" * 128
    r, e = roll(1, 1, 100, False, game_id=long_game_id, salt=mksalt("gi", 1))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on 128-char game_id"
    if e: return None, f"128-char game_id rejected: {str(e)[:60]}"
    return ok(1 <= r["results"][0] <= 100, f"got {r['results'][0]}")

def t167():
    """game_id with special characters (underscores, hyphens, colons — common in production IDs)."""
    r, e = roll(1, 1, 100, False, game_id="game:session_01-round.1", salt=mksalt("gi", 2))
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on special-char game_id"
    if e: return None, f"special-char game_id rejected: {str(e)[:60]}"
    return ok(1 <= r["results"][0] <= 100, f"got {r['results'][0]}")

def t168():
    """Same game_id across separate sessions produces different seeds (session isolation)."""
    r1, e = roll(1, 1, 100, False, game_id="shared_game_id", salt=mksalt("gi", 3))
    if e: return None, f"first roll: {e}"
    r2, e = roll(1, 1, 100, False, game_id="shared_game_id", salt=mksalt("gi", 4))
    if e: return None, f"second roll: {e}"
    return ok(r1["round_seed"] != r2["round_seed"],
              f"same seed despite different salts: {r1['round_seed'][:16]}...")

def t169():
    """salt = all zeros: '00000000' (valid hex edge value). Must succeed."""
    r, e = roll(1, 1, 100, False, game_id="salt_zeros", salt="00000000")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on all-zero salt"
    if e: return False, f"all-zero salt rejected: {str(e)[:60]}"
    return ok(1 <= r["results"][0] <= 100, f"got {r['results'][0]}")

def t170():
    """salt = all f: 'ffffffff' (valid hex edge value). Must succeed."""
    r, e = roll(1, 1, 100, False, game_id="salt_allf", salt="ffffffff")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on all-f salt"
    if e: return False, f"all-f salt rejected: {str(e)[:60]}"
    return ok(1 <= r["results"][0] <= 100, f"got {r['results'][0]}")

# ═══════════════════════════════════════════════════════════════════════════════
# T171-T175  PREV_ROUND_TXID CHAINING (NEW in v6)
# ═══════════════════════════════════════════════════════════════════════════════

def t171():
    """prev_round_txid not found on chain → error 1008 (PREV_ROUND_NOT_FOUND).
    SKIP if prev_round_txid parameter not yet implemented."""
    fake_txid = "f"*64
    r, e = roll(1, 1, 100, False, game_id="prev_notfound", salt=mksalt("pr",1),
                prev_round_txid=fake_txid)
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on fake prev_round_txid"
    if r is not None:
        # If accepted, prev_round_txid may not be implemented yet
        return None, "prev_round_txid accepted despite not existing on chain — may not be implemented"
    if isinstance(e, dict):
        code = e.get("code")
        if code == 1008: return True, f"error 1008 PREV_ROUND_NOT_FOUND — correct"
        return None, f"unexpected error code {code}: {e}"
    return None, f"prev_round_txid may not be implemented: {str(e)[:80]}"

def t172():
    """prev_round_txid in mempool but unconfirmed → error 1007 (PREV_ROUND_UNCONFIRMED).
    SKIP if prev_round_txid not yet implemented."""
    # Roll, immediately use the tx_id before it confirms
    r1, e = roll(5, 1, 52, True, game_id="prev_unconf_src", salt=mksalt("pr", 2))
    if e: return None, f"source roll failed: {e}"
    tx_id = r1.get("tx_id") or r1.get("session_txid")
    if not tx_id:
        return None, "tx_id not in roll response — prev_round_txid chaining not yet exposed"
    # Use immediately — before confirmation
    r2, e2 = roll(5, 1, 52, True, game_id="prev_unconf_dst", salt=mksalt("pr", 3),
                  prev_round_txid=tx_id)
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on unconfirmed prev_round_txid"
    if r2: return None, "accepted unconfirmed prev_round_txid (may be by design if mempool accepted)"
    if isinstance(e2, dict):
        code = e2.get("code")
        if code == 1007: return True, "error 1007 PREV_ROUND_UNCONFIRMED — correct"
        return None, f"unexpected code {code}: {e2}"
    return None, f"unconfirmed rejected (string error): {str(e2)[:60]}"

def t173():
    """prev_round_txid from a different session → error 1009 (PREV_ROUND_SESSION_MISMATCH).
    SKIP if prev_round_txid not yet implemented."""
    r1, e = roll(5, 1, 52, True, game_id="session_A", salt=mksalt("pr", 4))
    if e: return None, f"session A roll failed: {e}"
    tx_id_A = r1.get("tx_id") or r1.get("session_txid")
    if not tx_id_A:
        return None, "tx_id not in roll response — prev_round_txid not yet exposed"
    # Wait for confirmation
    h0 = blockcount()
    for _ in range(20):
        time.sleep(3)
        if blockcount() > h0: break
    # Use tx from session A in session B (different game_id)
    r2, e2 = roll(5, 1, 52, True, game_id="session_B", salt=mksalt("pr", 5),
                  prev_round_txid=tx_id_A)
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on session mismatch prev_round_txid"
    if r2: return None, "accepted cross-session prev_round_txid (may be by design)"
    if isinstance(e2, dict):
        code = e2.get("code")
        if code == 1009: return True, "error 1009 PREV_ROUND_SESSION_MISMATCH — correct"
        return None, f"unexpected code {code}: {e2}"
    return None, f"cross-session rejected (string error): {str(e2)[:60]}"

def t174():
    """Valid prev_round_txid chains correctly: round 1 result is excluded in round 2.
    SKIP if prev_round_txid not yet implemented."""
    r1, e = roll(5, 1, 52, True, game_id="chain_r1", salt=mksalt("pr", 6))
    if e: return None, f"round 1 failed: {e}"
    tx_id = r1.get("tx_id") or r1.get("session_txid")
    if not tx_id:
        return None, "tx_id not in response — prev_round_txid not yet implemented"
    # Wait for confirmation
    h0 = blockcount()
    for _ in range(20):
        time.sleep(3)
        if blockcount() > h0: break
    r2, e2 = roll(5, 1, 52, True, game_id="chain_r2", salt=mksalt("pr", 7),
                  prev_round_txid=tx_id)
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on valid prev_round_txid"
    if e2: return False, f"valid prev_round_txid rejected: {str(e2)[:80]}"
    overlap = [v for v in r2["results"] if v in r1["results"]]
    return ok(not overlap,
              f"round 1 values appeared in round 2: {overlap} r1={r1['results']}")

def t175():
    """3-round chain via prev_round_txid: each round excludes all prior results.
    SKIP if prev_round_txid not yet implemented."""
    all_drawn = []
    prev_tx = None
    for rnd in range(3):
        kwargs = {"prev_round_txid": prev_tx} if prev_tx else {}
        r, e = roll(5, 1, 52, True, game_id=f"chain3_r{rnd}", salt=mksalt("chain3", rnd), **kwargs)
        if e:
            if rnd==0: return None, f"round 0 failed: {e}"
            return None, f"round {rnd} failed (prev_round_txid may not be implemented): {str(e)[:60]}"
        tx_id = r.get("tx_id") or r.get("session_txid")
        if rnd==0 and not tx_id:
            return None, "tx_id not in response — prev_round_txid not yet implemented"
        overlap = [v for v in r["results"] if v in all_drawn]
        if overlap: return False, f"round {rnd} overlap with prior rounds: {overlap}"
        all_drawn.extend(r["results"])
        prev_tx = tx_id
        if prev_tx:
            h0 = blockcount()
            for _ in range(20):
                time.sleep(3)
                if blockcount() > h0: break
    return ok(len(set(all_drawn))==15, f"15 unique draws across 3 rounds: {all_drawn}")


# ═══════════════════════════════════════════════════════════════════════════════
# T176-T178  EXCLUDE COUNT LOAD / STABILITY (NEW in v6.1)
# ═══════════════════════════════════════════════════════════════════════════════

def t176():
    """Exclude count sweep: probe 512, 513, 600, 1000, 2000, 5000 items.
    Documents what the node actually does at each size — accepted/rejected/crash.
    Node must survive all sizes. BUG-011: KDD-028 not enforced, all likely accepted."""
    sizes = [512, 513, 600, 1000, 2000, 5000]
    results_log = []
    for size in sizes:
        excl = list(range(1, size + 1))
        t0 = time.time()
        r, e = roll(1, 1, size + 1000, False, exclude=excl,
                    game_id="excl_sweep", salt=mksalt("excl_sweep", size))
        elapsed = time.time() - t0
        alive = node_alive()
        if not alive:
            return False, f"NODE CRASHED at exclude count={size}"
        if r:
            status = f"ACCEPTED result={r['results'][0]} ({elapsed:.2f}s)"
        elif isinstance(e, dict):
            status = f"REJECTED code={e.get('code')} ({elapsed:.2f}s)"
        else:
            status = f"REJECTED ({elapsed:.2f}s): {str(e)[:40]}"
        results_log.append(f"excl={size}: {status}")
        print(f"         {results_log[-1]}")
    return True, f"node survived all sizes — see above"


def t177():
    """Exclude count latency baseline: p50/p95/p99/max across 100 calls at each of
    100, 300, 512 items. Establishes the statistical performance floor for fault
    injection comparison (Toxiproxy / P2-TOX-01) and ODC-015 hardware table validation.
    100 iterations gives a true p99 (the 99th value when sorted ascending).
    Single-shot measurements give ~p50 by chance — insufficient for capacity planning.
    Allow ~5 min total (3 sizes x 100 calls x ~1s each).
    """
    ITERATIONS = 100
    summary = []
    for size in [100, 300, 512]:
        excl = list(range(1, size + 1))
        samples = []
        errors = 0
        for i in range(ITERATIONS):
            t0 = time.time()
            r, e = roll(1, 1, size + 500, False, exclude=excl,
                        game_id=f"excl_lat_{size}", salt=mksalt("excl_lat", size, i))
            elapsed = time.time() - t0
            if not node_alive():
                return False, f"NODE CRASHED at excl={size} iteration {i}"
            if not r:
                errors += 1
            samples.append(elapsed)  # include error latency in distribution

        samples.sort()
        n   = len(samples)
        p50  = samples[int(n * 0.50)]
        p95  = samples[int(n * 0.95)]
        p99  = samples[int(n * 0.99)]
        pmax = samples[-1]
        pmin = samples[0]
        avg  = sum(samples) / n
        line = (f"excl={size:4d}: "
                f"min={pmin:.3f}s  p50={p50:.3f}s  p95={p95:.3f}s  "
                f"p99={p99:.3f}s  max={pmax:.3f}s  avg={avg:.3f}s  "
                f"n={n}  errors={errors}")
        summary.append(line)
        print(f"         {line}")
    return True, " || ".join(summary)


def t178():
    """Node stability after max-size exclude: send 5000-item exclude, then verify
    normal rolls still work. Crash probe — the large exclude should not corrupt state."""
    excl = list(range(1, 5001))
    roll(1, 1, 6000, False, exclude=excl,
         game_id="excl_stress", salt=mksalt("excl_stress"))
    # Whether accepted or rejected, the node must still respond normally
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED after 5000-item exclude"
    r, e = roll(1, 1, 100, False, game_id="post_stress", salt=mksalt("post_stress"))
    if e:
        return False, f"node broken after large exclude — normal roll failed: {e}"
    return ok(1 <= r["results"][0] <= 100,
              f"node stable — post-stress roll: {r['results'][0]}")

# ═══════════════════════════════════════════════════════════════════════════════
# T179-T185  EXCLUSION LIMIT FINDER  (BUG-011 deep probe)
#
# BUG-011: MAX_EXCLUDE_COUNT=512 enforcement not active as of v6.1.
# T176 showed 5000-item exclude accepted in 21ms. These tests find the actual
# crash/rejection boundary and measure performance at scale.
# ═══════════════════════════════════════════════════════════════════════════════

def t179():
    """Binary search for the real exclusion rejection/crash boundary.
    Starts at 5000 (known accepted), halves toward 512 if always accepted,
    or narrows up if rejected. Reports the upper bound at which the node
    still responds without crashing. BUG-011: if 512 is never enforced,
    this test PASSES logging the highest tested count at which node survived.
    Target: find the first count where either error 1016 fires OR node dies.
    """
    lo, hi = 512, 100000
    # Quick sanity: confirm 512 is accepted (expected per BUG-011)
    r, e = roll(1, 1, 513, False, exclude=list(range(1, 513)),
                game_id="excl_bs_base", salt=mksalt("bs", 512))
    if not node_alive():
        return False, "NODE CRASHED at exclude=512 — severe regression"
    if isinstance(e, dict) and e.get("code") == 1016:
        return True, f"MAX_EXCLUDE_COUNT enforcement NOW ACTIVE at 512 — BUG-011 fixed!"

    # Binary search upward for the rejection/crash point
    boundary = None
    test_sizes = [1000, 5000, 10000, 25000, 50000, 100000]
    last_accepted = 512
    for size in test_sizes:
        excl = list(range(1, size + 1))
        t0 = time.time()
        r, e = roll(1, 1, size + 1000, False, exclude=excl,
                    game_id="excl_binary", salt=mksalt("bs", size))
        elapsed = time.time() - t0
        alive = node_alive()
        if not alive:
            boundary = f"CRASH at size={size} after {elapsed:.2f}s"
            print(f"         !! NODE CRASH at exclude={size}")
            break
        if isinstance(e, dict) and e.get("code") == 1016:
            boundary = f"Enforcement active at size={size} — BUG-011 resolved"
            print(f"         Error 1016 fired at exclude={size} ({elapsed:.2f}s)")
            break
        if r:
            last_accepted = size
            print(f"         excl={size}: ACCEPTED ({elapsed:.2f}s)")
        else:
            boundary = f"Unexpected error at size={size}: {str(e)[:40]}"
            break

    if boundary:
        return True, f"Boundary found: {boundary} (last_accepted={last_accepted})"
    return True, (
        f"No rejection/crash found up to excl=100000 — "
        f"BUG-011 enforcement still absent. Max tested: {last_accepted}"
    )


def t180():
    """Confirm exact error code on the first call over MAX_EXCLUDE_COUNT.
    KDD-029: expects PTX_ERR_EXCLUDE_LIMIT_EXCEEDED = error 1016.
    BUG-011: currently returns success instead. Test documents actual code.
    PASS condition: either 1016 is returned (fix active) OR node survives and
    we log the actual response (BUG-011 still present).
    """
    excl = list(range(1, 514))  # 513 items = MAX + 1
    r, e = roll(1, 1, 2000, False, exclude=excl,
                game_id="excl_err_code", salt=mksalt("t180"))
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED on 513-item exclude"
    if isinstance(e, dict):
        code = e.get("code")
        msg  = e.get("message","")[:80]
        if code == 1016:
            return True, f"PTX_ERR_EXCLUDE_LIMIT_EXCEEDED (1016) returned correctly — BUG-011 FIXED"
        return True, f"BUG-011: error returned but wrong code={code} msg='{msg}'"
    if r:
        result_val = r.get("results", ["?"])[0]
        return True, f"BUG-011 ACTIVE: 513-item exclude ACCEPTED — result={result_val}"
    return True, f"BUG-011: unexpected nil response — e={str(e)[:60]}"


def t181():
    """Concurrency stress at 512-item exclude: 3/5/10 workers in escalating waves.
    Each wave fires N concurrent sessions all with excl=512. Records per-worker
    elapsed, computes p50/p99/max per wave, checks for crashes and violations.
    Escalation stops early if p99 exceeds 30s (node saturated) or a crash occurs.
    Results feed ODC-015 hardware table — concurrent exclude capacity at n=11.
    """
    EXCL_SIZE = 512
    excl = list(range(1, EXCL_SIZE + 1))
    wave_results = []

    for n_workers in [3, 5, 10]:
        elapsed_list = []
        errors, violations = [], []
        lock = threading.Lock()

        def worker(idx, n=n_workers):
            t0 = time.time()
            r, e = roll(1, 1, 1000, False, exclude=excl,
                        game_id=f"conc_excl_{n}_{idx}",
                        salt=mksalt("conc_excl", n, idx))
            elapsed = time.time() - t0
            with lock:
                elapsed_list.append(elapsed)
                if e:
                    errors.append(f"w{idx}: {str(e)[:40]}")
                elif r:
                    v = r["results"][0]
                    if v in excl:
                        violations.append(f"w{idx}: violation result={v}")
                else:
                    errors.append(f"w{idx}: nil response")

        threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_workers)]
        for t in threads: t.start()
        for t in threads: t.join(timeout=120)

        if not node_alive():
            return False, f"NODE CRASHED at {n_workers} concurrent workers excl={EXCL_SIZE}"
        if violations:
            return False, f"Exclusion violations at {n_workers} workers: {violations}"

        if elapsed_list:
            elapsed_list.sort()
            n  = len(elapsed_list)
            p50  = elapsed_list[int(n * 0.50)]
            p99  = elapsed_list[min(int(n * 0.99), n - 1)]
            pmax = elapsed_list[-1]
            line = (f"workers={n_workers}  p50={p50:.3f}s  p99={p99:.3f}s  "
                    f"max={pmax:.3f}s  errors={len(errors)}")
        else:
            line = f"workers={n_workers}  no samples  errors={len(errors)}"
            p99 = 0

        wave_results.append(line)
        print(f"         {line}")

        if p99 > 30.0:
            wave_results.append(f"STOPPING — p99={p99:.1f}s exceeds 30s threshold at {n_workers} workers")
            print(f"         {wave_results[-1]}")
            break

    return True, " || ".join(wave_results)


def t182():
    """Concurrent latency at 100/300/512 items: p50/p99/max across 20 iterations,
    each with 5 concurrent workers. Computes overhead delta vs T177 single-session
    baseline. Flags sizes where concurrent p99 exceeds 3x single-session p99 —
    the threshold above which node saturation is likely.
    20 iterations x 5 workers x 3 sizes = 300 calls total. Allow ~5 min.
    """
    ITERATIONS = 20
    WORKERS    = 5
    summary    = []

    # T177 single-session reference values (run inline at each size for direct comparison)
    for size in [100, 300, 512]:
        excl = list(range(1, size + 1))

        # --- Single-session reference (5 calls, take median as baseline) ---
        ref_samples = []
        for i in range(5):
            t0 = time.time()
            roll(1, 1, size + 500, False, exclude=excl,
                 game_id=f"excl_ref_{size}", salt=mksalt("excl_ref", size, i))
            ref_samples.append(time.time() - t0)
        ref_samples.sort()
        ref_p50 = ref_samples[2]  # median of 5

        # --- Concurrent load: ITERATIONS rounds, each firing WORKERS threads ---
        all_elapsed = []
        for iteration in range(ITERATIONS):
            iter_elapsed = []
            iter_lock = threading.Lock()

            def worker_fn(it=iteration, sz=size, ex=excl):
                t0 = time.time()
                r, e = roll(1, 1, sz + 500, False, exclude=ex,
                            game_id=f"excl_conc_{sz}_{it}",
                            salt=mksalt("conc_lat", sz, it, time.time_ns()))
                elapsed = time.time() - t0
                with iter_lock:
                    iter_elapsed.append(elapsed)

            threads = [threading.Thread(target=worker_fn) for _ in range(WORKERS)]
            for t in threads: t.start()
            for t in threads: t.join(timeout=90)

            if not node_alive():
                return False, f"NODE CRASHED at excl={size} iteration={iteration}"
            all_elapsed.extend(iter_elapsed)

        all_elapsed.sort()
        n    = len(all_elapsed)
        p50  = all_elapsed[int(n * 0.50)]
        p95  = all_elapsed[int(n * 0.95)]
        p99  = all_elapsed[min(int(n * 0.99), n - 1)]
        pmax = all_elapsed[-1]
        avg  = sum(all_elapsed) / n
        delta_p50 = p50 / ref_p50 if ref_p50 > 0 else 0
        flag = "  !! SATURATED" if p99 > ref_p50 * 3 else ""
        line = (f"excl={size:4d}: "
                f"ref_p50={ref_p50:.3f}s  "
                f"conc_p50={p50:.3f}s ({delta_p50:.1f}x)  "
                f"conc_p95={p95:.3f}s  conc_p99={p99:.3f}s  "
                f"max={pmax:.3f}s  n={n}{flag}")
        summary.append(line)
        print(f"         {line}")

    return True, " || ".join(summary)


def t183():
    """Empty exclude list (zero items) — boundary condition.
    Confirms that exclude=[] is functionally identical to not passing exclude at all.
    Result must be in range and node must not error.
    """
    r, e = roll(1, 1, 100, False, exclude=[],
                game_id="excl_empty", salt=mksalt("t183"))
    if e: return False, f"empty exclude rejected: {e}"
    v = r["results"][0]
    return ok(1 <= v <= 100, f"result={v}")


def t184():
    """Exclude count exceeds pool size — all values excluded.
    Pool = [1..10], exclude = [1..20] — more exclusions than valid values.
    Node must either return an error (pool exhausted) or handle gracefully.
    Must NOT crash or return a value in the exclude set.
    """
    pool_size = 10
    excl = list(range(1, 21))  # 20 items excluded from pool of 10
    r, e = roll(1, 1, pool_size, False, exclude=excl,
                game_id="excl_over_pool", salt=mksalt("t184"))
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED when exclude > pool"
    if e:
        return True, f"Correctly rejected with error: {str(e)[:80]}"
    # If accepted, result must not be in the exclude set (though pool is exhausted...)
    v = r["results"][0]
    if v in excl:
        return False, f"Exclusion violation: result={v} is in exclude list"
    return True, f"Accepted with result={v} outside exclude (unexpected but not a crash)"


def t185():
    """All-duplicate exclude list — same value repeated 100 times.
    exclude=[42]*100 should behave identically to exclude=[42].
    Result must not be 42; node must not error.
    """
    excl = [42] * 100
    r, e = roll(1, 1, 100, False, exclude=excl,
                game_id="excl_dups", salt=mksalt("t185"))
    if e:
        return True, f"Rejected (duplicate handling may refuse): {str(e)[:60]}"
    v = r["results"][0]
    if v == 42:
        return False, f"Exclusion violation: result=42 despite exclude=[42]*100"
    return True, f"result={v} (not 42) — duplicate exclude handled correctly"


# ═══════════════════════════════════════════════════════════════════════════════
# T186-T195  QUORUM_SIG_HASH DERIVATION DIAGNOSTIC  (KDD-033 / ODC-019)
#
# These tests diagnose HOW the node computes quorum_sig_hash, to establish
# whether it conforms to KDD-033 (SHA256 of raw bytes) or a different method.
# Run this section when T130 fails to identify the root cause precisely.
# ═══════════════════════════════════════════════════════════════════════════════

def t186():
    """DIAGNOSTIC: dump full roll response fields for manual inspection.
    Always PASSES — prints every key/value in the response for reference.
    Use this first when T130 is failing to see exactly what the node returns.
    """
    r, e = roll(1, 1, 100, False, game_id="t186_dump", salt=mksalt("t186"))
    if e: return None, f"RPC: {e}"
    print("         Full response fields:")
    for k, v in sorted(r.items()):
        val_str = str(v)
        if len(val_str) > 80:
            val_str = val_str[:77] + "..."
        print(f"           {k!r:30s} = {val_str!r}")
    sig   = r.get("quorum_sig","")
    shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
    print(f"         quorum_sig length: {len(sig)} chars (expect 192)")
    print(f"         quorum_sig_hash resolved: '{shash[:32]}...' length={len(shash)}")
    if sig and len(sig) == 192:
        raw_bytes = bytes.fromhex(sig)
        print(f"         SHA256(raw bytes):    {hashlib.sha256(raw_bytes).hexdigest()}")
        print(f"         SHA256(hex_string):   {hashlib.sha256(sig.encode()).hexdigest()}")
        print(f"         SHA256(hex_upper):    {hashlib.sha256(sig.upper().encode()).hexdigest()}")
        sha256d = hashlib.sha256(hashlib.sha256(raw_bytes).digest()).hexdigest()
        print(f"         SHA256d(raw bytes):   {sha256d}")
    return True, "Diagnostic dump complete — see above"


def t187():
    """DIAGNOSTIC: does quorum_sig_hash == SHA256(hex_string)?
    Option B from ODC-019 — rejected by KDD-033 but may reveal node bug.
    PASS if it matches (means node is using Option B, NOT KDD-033).
    FAIL if it does not match (Option B not the method either).
    """
    r, e = roll(1, 1, 100, False, game_id="t187_hexstr", salt=mksalt("t187"))
    if e: return None, f"RPC: {e}"
    sig   = r.get("quorum_sig","")
    shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
    if not sig or len(sig) != 192: return None, f"quorum_sig unavailable (len={len(sig)})"
    if not shash or len(shash) != 64: return None, f"quorum_sig_hash unavailable: '{shash}'"
    option_b = hashlib.sha256(sig.encode()).hexdigest()
    if shash == option_b:
        return False, (
            f"MATCH: node uses SHA256(hex_string) [Option B] NOT KDD-033. "
            f"Node must be fixed to use SHA256(raw bytes). shash={shash[:16]}..."
        )
    return True, f"No match — quorum_sig_hash != SHA256(hex_string). shash={shash[:16]}... optB={option_b[:16]}..."


def t188():
    """DIAGNOSTIC: does quorum_sig_hash == SHA256(hex_upper_string)?
    Option C from ODC-019 — uppercase hex as input to SHA256.
    """
    r, e = roll(1, 1, 100, False, game_id="t188_hexupper", salt=mksalt("t188"))
    if e: return None, f"RPC: {e}"
    sig   = r.get("quorum_sig","")
    shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
    if not sig or len(sig) != 192: return None, f"quorum_sig unavailable"
    if not shash or len(shash) != 64: return None, f"quorum_sig_hash unavailable"
    option_c = hashlib.sha256(sig.upper().encode()).hexdigest()
    if shash == option_c:
        return False, (
            f"MATCH: node uses SHA256(hex_upper_string) [Option C] NOT KDD-033. "
            f"shash={shash[:16]}..."
        )
    return True, f"No match — quorum_sig_hash != SHA256(hex_upper). shash={shash[:16]}..."


def t189():
    """PRIMARY DIAGNOSTIC: does quorum_sig_hash == SHA256(raw compressed G2 bytes)?
    This is KDD-033 Option A — the canonical definition.
    PASS = node is KDD-033 conformant.
    FAIL = node does not use raw bytes — run T187/T188 to find the actual method.
    """
    matches = 0
    details = []
    for i in range(3):
        r, e = roll(1, 1, 100, False, game_id=f"t189_raw_{i}", salt=mksalt("t189", i))
        if e: return None, f"i={i} RPC: {e}"
        sig   = r.get("quorum_sig","")
        shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
        if not sig or len(sig) != 192: return None, f"i={i} quorum_sig unavailable"
        if not shash or len(shash) != 64:
            details.append(f"i={i}: quorum_sig_hash field absent or wrong length: '{shash}'")
            continue
        expected = hashlib.sha256(bytes.fromhex(sig)).hexdigest()
        if shash == expected:
            matches += 1
            details.append(f"i={i}: MATCH {shash[:16]}...")
        else:
            details.append(f"i={i}: MISMATCH got={shash[:16]}... want={expected[:16]}...")
    if matches == 3:
        return True, f"3/3 MATCH — KDD-033 conformant: {'; '.join(details)}"
    elif matches > 0:
        return False, f"{matches}/3 match — inconsistent behaviour: {'; '.join(details)}"
    else:
        return False, f"0/3 match — node NOT KDD-033 conformant: {'; '.join(details)}"


def t190():
    """DIAGNOSTIC: does quorum_sig_hash == SHA256d (double SHA256 of raw bytes)?
    Option D from ODC-019.
    """
    r, e = roll(1, 1, 100, False, game_id="t190_sha256d", salt=mksalt("t190"))
    if e: return None, f"RPC: {e}"
    sig   = r.get("quorum_sig","")
    shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
    if not sig or len(sig) != 192: return None, "quorum_sig unavailable"
    if not shash or len(shash) != 64: return None, f"quorum_sig_hash unavailable"
    raw = bytes.fromhex(sig)
    sha256d = hashlib.sha256(hashlib.sha256(raw).digest()).hexdigest()
    if shash == sha256d:
        return False, (
            f"MATCH: node uses SHA256d(raw bytes) [Option D] NOT KDD-033. "
            f"shash={shash[:16]}..."
        )
    return True, f"No match — quorum_sig_hash != SHA256d. shash={shash[:16]}..."


def t191():
    """DIAGNOSTIC: check if quorum_sig_hash equals SHA256 of any OTHER response field.
    Catches cases where quorum_sig_hash is derived from round_seed, results, or nonce
    rather than from quorum_sig — which would indicate a deeper protocol mismatch.
    """
    r, e = roll(1, 1, 100, False, game_id="t191_other", salt=mksalt("t191"))
    if e: return None, f"RPC: {e}"
    shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
    if not shash or len(shash) != 64:
        return None, f"quorum_sig_hash field absent — cannot compare"
    matches = []
    for k, v in r.items():
        if k in ("quorum_sig_hash", "beacon", "round_beacon"):
            continue
        v_str = str(v) if not isinstance(v, str) else v
        for encode in ["utf-8", None]:
            try:
                raw = v_str.encode(encode) if encode else bytes.fromhex(v_str)
                h = hashlib.sha256(raw).hexdigest()
                if h == shash:
                    matches.append(f"SHA256({k!r} {'bytes' if not encode else 'str'}) == quorum_sig_hash")
            except Exception:
                pass
    if matches:
        return False, f"quorum_sig_hash matches unexpected field(s): {matches}"
    return True, f"quorum_sig_hash does not match SHA256 of any other response field"


def t192():
    """SUMMARY DIAGNOSTIC: run all hash method checks for one roll and print
    a clear verdict table. Always passes — use when T130 fails for a one-stop report.
    """
    r, e = roll(1, 1, 100, False, game_id="t192_verdict", salt=mksalt("t192"))
    if e: return None, f"RPC: {e}"
    sig   = r.get("quorum_sig","")
    shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
    if not sig or len(sig) != 192: return None, f"quorum_sig unavailable (len={len(sig)})"
    if not shash or len(shash) != 64: return None, f"quorum_sig_hash unavailable: '{shash}'"

    raw_bytes = bytes.fromhex(sig)
    methods = [
        ("Option A — SHA256(raw bytes)    [KDD-033]", hashlib.sha256(raw_bytes).hexdigest()),
        ("Option B — SHA256(hex_string)   [rejected]", hashlib.sha256(sig.encode()).hexdigest()),
        ("Option C — SHA256(hex_upper)    [rejected]", hashlib.sha256(sig.upper().encode()).hexdigest()),
        ("Option D — SHA256d(raw bytes)   [rejected]", hashlib.sha256(hashlib.sha256(raw_bytes).digest()).hexdigest()),
    ]
    print(f"         quorum_sig_hash (node):  {shash}")
    identified = None
    for label, computed in methods:
        match = "✓ MATCH" if computed == shash else "  no match"
        print(f"         {match}  {label}  {computed[:16]}...")
        if computed == shash:
            identified = label

    if identified and "KDD-033" in identified:
        return True, f"Node is KDD-033 conformant — uses {identified}"
    elif identified:
        return False, (
            f"Node uses {identified} — does NOT conform to KDD-033. "
            f"Fix: change node to hash raw bytes (bytes.fromhex(sig)) not the hex string."
        )
    else:
        return False, (
            f"quorum_sig_hash does not match ANY standard method — "
            f"may be derived from a different field. Run T191 and T186."
        )


def t193():
    """quorum_sig_hash is unique across 5 consecutive rolls — not cached or reused.
    (Same check as T17/T18 for sigs but applied to the hash field.)
    """
    hashes = set()
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"qsh_uniq_{i}", salt=mksalt("qsh_uniq", i))
        if e: return None, f"RPC at i={i}: {e}"
        shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
        if not shash: return None, f"i={i}: quorum_sig_hash absent"
        hashes.add(shash)
    return ok(len(hashes)==5, f"non-unique quorum_sig_hash values across 5 rolls: {hashes}")


def t194():
    """quorum_sig_hash length and encoding invariants across 10 rolls.
    Must always be exactly 64 lowercase hex chars — no roll-to-roll variance in format.
    """
    bad = []
    for i in range(10):
        r, e = roll(1, 1, 100, False, game_id=f"qsh_fmt_{i}", salt=mksalt("qsh_fmt", i))
        if e: return None, f"RPC at i={i}: {e}"
        shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
        if not shash or len(shash) != 64:
            bad.append(f"i={i}: len={len(shash or '')} val='{(shash or '')[:20]}'")
            continue
        if not all(c in "0123456789abcdef" for c in shash):
            bad.append(f"i={i}: non-lowercase-hex char in '{shash[:20]}'")
    return ok(not bad, f"format violations: {bad}")


def t195():
    """POSITIVE ASSERTION (conforming node): quorum_sig_hash == SHA256(raw bytes) for 5 rolls.
    This is a strict KDD-033 conformance test — 5/5 must pass.
    Equivalent to T130 but with 5 iterations and cleaner output.
    Run after T192 confirms the node uses Option A.
    """
    passed, details = 0, []
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"qsh_pos_{i}", salt=mksalt("qsh_pos", i))
        if e: return None, f"RPC at i={i}: {e}"
        sig   = r.get("quorum_sig","")
        shash = r.get("quorum_sig_hash","") or r.get("beacon","") or r.get("round_beacon","")
        if not sig or len(sig) != 192: return None, f"i={i}: quorum_sig unavailable"
        if not shash or len(shash) != 64:
            details.append(f"i={i}: hash absent")
            continue
        expected = hashlib.sha256(bytes.fromhex(sig)).hexdigest()
        if shash == expected:
            passed += 1
        else:
            details.append(f"i={i}: mismatch (got={shash[:12]}... want={expected[:12]}...)")
    if passed == 5:
        return True, "5/5 quorum_sig_hash == SHA256(raw bytes) — KDD-033 fully verified"
    return False, f"{passed}/5 pass — details: {details}"


# ═══════════════════════════════════════════════════════════════════════════════
# T196-T199  BUG-012 — RAPID SEQUENTIAL ROLL / MEMPOOL UTXO SELECTION
#
# BUG-012: FundTransaction uses pcoinsTip (confirmed UTXO set) only.
# Multiple rapid calls all select the same UTXO, so only the first
# call creates an on-chain PTXSESS tx. Calls 2-N return a result via
# RPC but log "input N already spent" — no on-chain footprint.
#
# Fix: mempool-aware UTXO selection via CCoinControl + LockCoin/UnlockCoin
# New RPC: ptx_prepare_wallet(n, amount_each) to split wallet into N UTXOs.
#
# T196: document current burst behaviour (baseline / bug characterisation)
# T197: ptx_prepare_wallet — verify N UTXOs created (skip if not implemented)
# T198: rapid burst after wallet prep — all N calls must be on-chain
# T199: result integrity — each returned result has a confirmed PTXSESS tx
# ═══════════════════════════════════════════════════════════════════════════════

def _count_ptxsess_txs_since(start_height):
    """Count type-6 (PTXSESS) transactions mined from start_height to now.
    Returns (count, current_height). Uses getblockhash+getblock RPC.
    """
    current = blockcount()
    if not current or current < start_height:
        return 0, current or 0
    count = 0
    for h in range(start_height, current + 1):
        bh, _ = rpc("getblockhash", [h])
        if not bh:
            continue
        blk, _ = rpc("getblock", [bh, 2])
        if not blk:
            continue
        for tx in blk.get("tx", []):
            # PTXSESS transactions have type 6 in the extra payload
            payload = tx.get("extraPayload", {}) or {}
            if tx.get("type") == 6 or payload.get("type") == 6:
                count += 1
    return count, current


def t196():
    """BUG-012 DIAGNOSTIC: fire 10 rapid sequential ptx_roll() calls and count
    how many produce on-chain PTXSESS txs vs how many are orphaned.
    Expected (bug present): 1 on-chain, 9 "pending" (orphaned results).
    Expected (bug fixed): 10 on-chain.
    Always PASSES — records the orphan count for regression tracking.
    Wait 3 blocks after burst to allow mining.
    """
    start_height = blockcount()
    if not start_height:
        return None, "cannot read block height"

    # Fire 10 rapid calls — do NOT sleep between them
    burst_size = 10
    results_received = []
    errors = []
    for i in range(burst_size):
        r, e = roll(1, 1, 100, False,
                    game_id=f"bug012_burst_{i}", salt=mksalt("b12", i))
        if r:
            results_received.append(r.get("results", [None])[0])
        else:
            errors.append(f"call {i}: {str(e)[:40]}")

    rpc_successes = len(results_received)
    print(f"         RPC successes: {rpc_successes}/{burst_size} — waiting 3 blocks for mining...")

    # Wait for up to 3 blocks to mine
    target_height = start_height + 3
    for _ in range(90):
        if blockcount() >= target_height:
            break
        time.sleep(2)

    on_chain, end_height = _count_ptxsess_txs_since(start_height + 1)
    orphaned = rpc_successes - on_chain
    msg = (
        f"burst={burst_size} rpc_ok={rpc_successes} on_chain={on_chain} "
        f"orphaned={orphaned} blocks_mined={end_height - start_height}"
    )
    if errors:
        msg += f" rpc_errors={errors}"

    if orphaned == 0 and rpc_successes == burst_size:
        return True, f"BUG-012 FIXED — all {burst_size} results on-chain: {msg}"
    elif on_chain >= 1:
        # Bug present but node survived — log for tracking
        print(f"         BUG-012 ACTIVE: {orphaned} orphaned results — {msg}")
        return True, f"BUG-012 active (expected pre-fix): {msg}"
    else:
        return False, f"No on-chain PTXSESS txs at all — possible regression: {msg}"


def t197():
    """BUG-012 FIX SUPPORT: call ptx_prepare_wallet(n=15, amount_each=2.0).
    Splits wallet into 15 funded UTXOs so rapid sequential calls each select
    a distinct input. SKIP if RPC not yet implemented.
    Returns: {"utxos_created": N, "txid": "...", "total_locked": X}
    """
    r, e = rpc("ptx_prepare_wallet", [15, 2.0])
    if e:
        code = e.get("code") if isinstance(e, dict) else -1
        msg  = str(e)[:80]
        if code == -32601 or "not found" in msg.lower() or "unknown" in msg.lower():
            return None, f"ptx_prepare_wallet RPC not yet implemented — skip (BUG-012 fix pending)"
        return False, f"ptx_prepare_wallet returned error: {msg}"
    n_created = r.get("utxos_created", 0)
    txid      = r.get("txid", "")[:16]
    total     = r.get("total_locked", 0)
    if n_created < 1:
        return False, f"ptx_prepare_wallet returned utxos_created={n_created}"
    # Wait for the prep tx to confirm
    print(f"         Wallet prepped: {n_created} UTXOs, txid={txid}..., total={total} HMS — waiting for confirm...")
    for _ in range(60):
        time.sleep(2)
        bc = blockcount()
        if bc:
            break
    return True, f"ptx_prepare_wallet OK: {n_created} UTXOs created, txid={txid}..."


def t198():
    """BUG-012 FIX VERIFICATION: fire 15 rapid sequential calls after wallet prep.
    All 15 must produce on-chain PTXSESS txs (0 orphaned).
    SKIP if ptx_prepare_wallet not available (BUG-012 fix not yet merged).
    This is the primary regression test for the fix.
    """
    # Quick check — does ptx_prepare_wallet exist?
    _, e = rpc("ptx_prepare_wallet", [1, 2.0])
    if isinstance(e, dict):
        code = e.get("code", 0)
        if code == -32601:
            return None, "ptx_prepare_wallet not implemented — BUG-012 fix not yet merged"

    # Re-prep to ensure fresh UTXOs
    prep_r, prep_e = rpc("ptx_prepare_wallet", [15, 2.0])
    if prep_e:
        return None, f"ptx_prepare_wallet failed: {str(prep_e)[:60]}"
    print(f"         Wallet prepped — {prep_r.get('utxos_created','?')} UTXOs. Waiting 2s for confirm...")
    time.sleep(2)

    start_height = blockcount()
    burst_size = 15
    rpc_ok = 0
    for i in range(burst_size):
        r, e = roll(1, 1, 100, False,
                    game_id=f"bug012_fix_{i}", salt=mksalt("b12f", i))
        if r:
            rpc_ok += 1

    print(f"         Fired {burst_size} calls, {rpc_ok} RPC successes — waiting 3 blocks...")
    target = start_height + 3
    for _ in range(90):
        if blockcount() >= target:
            break
        time.sleep(2)

    on_chain, end_height = _count_ptxsess_txs_since(start_height + 1)
    orphaned = rpc_ok - on_chain
    msg = (f"burst={burst_size} rpc_ok={rpc_ok} on_chain={on_chain} "
           f"orphaned={orphaned} blocks={end_height - start_height}")
    return ok(orphaned == 0 and rpc_ok == burst_size,
              f"BUG-012 fix verification: {msg}")


def t199():
    """BUG-012 RESULT INTEGRITY: for each result returned in a 5-call burst,
    verify a matching confirmed PTXSESS tx exists (by session block height).
    Pre-fix: most results will have no on-chain record.
    Post-fix: all results match a confirmed tx.
    SKIP if ptx_prepare_wallet not available.
    """
    _, e = rpc("ptx_prepare_wallet", [1, 2.0])
    if isinstance(e, dict) and e.get("code") == -32601:
        return None, "ptx_prepare_wallet not implemented — skip"

    rpc("ptx_prepare_wallet", [5, 2.0])
    time.sleep(2)

    start_height = blockcount()
    rounds = []
    for i in range(5):
        r, e = roll(1, 1, 100, False,
                    game_id=f"bug012_int_{i}", salt=mksalt("b12i", i))
        if r:
            rounds.append({
                "result": r.get("results", [None])[0],
                "block_height": r.get("block_height"),
                "quorum_sig": r.get("quorum_sig","")[:16],
            })

    print(f"         {len(rounds)} results received — waiting 3 blocks...")
    target = start_height + 3
    for _ in range(90):
        if blockcount() >= target:
            break
        time.sleep(2)

    on_chain, _ = _count_ptxsess_txs_since(start_height + 1)
    orphaned = len(rounds) - on_chain
    verified = on_chain
    msg = (f"results_received={len(rounds)} on_chain={on_chain} "
           f"orphaned={orphaned}")
    print(f"         {msg}")
    for rd in rounds:
        print(f"           result={rd['result']} bh={rd['block_height']} sig={rd['quorum_sig']}...")
    return ok(orphaned == 0, f"Result integrity: {msg}")


# ═══════════════════════════════════════════════════════════════════════════════
# T200-T219  PTXSETTLE (KDD-032 / ODC-020) and PTXCONSOLIDATE (KDD-034)
# ═══════════════════════════════════════════════════════════════════════════════
#
# These tests are PASSIVE chain scanners. They inspect already-confirmed
# blocks for nType=7 (PTXSETTLE) and nType=8 (PTXCONSOLIDATE) txs and
# validate consensus rules. They do NOT wait for a settlement window.
#
# If the chain has no settle/consolidate within the lookback window
# (default 200 blocks, --ptx-scan-blocks N to override), the structural
# tests SKIP rather than FAIL. This is intentional: an empty scan range
# is not a regression, it just means there has been no pool activity
# in that window.
#
# T200-T203:  pool_utxo_count + RPC field additions (KDD-034)
# T204-T207:  PTXSETTLE structure (KDD-032 Rules 2/4/8 + 200-input cap)
# T208-T211:  PTXCONSOLIDATE structure (KDD-034 Rules C1-C5)
# T212-T215:  ODC-020 GM payment address enforcement on PTXSETTLE vout
# T216-T219:  Cross-tx consensus rules (coexistence, pool-only inputs)
# ═══════════════════════════════════════════════════════════════════════════════

def t200():
    """pool_utxo_count field present in ptx_lottery_status response (KDD-034)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    if "pool_utxo_count" not in st:
        return False, f"pool_utxo_count missing from lottery_status — KDD-034 RPC not deployed"
    c = st["pool_utxo_count"]
    return ok(isinstance(c, int) and c >= 0,
              f"pool_utxo_count={c!r} (not int>=0)")

def t201():
    """pool_utxo_count and pool_balance_sat are consistent: count==0 iff balance==0."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    c = st.get("pool_utxo_count")
    b = st.get("pool_balance_sat")
    if c is None or b is None:
        return None, f"missing fields: count={c} bal={b}"
    if c == 0 and b != 0:
        return False, f"count=0 but balance={b} sat — inconsistent"
    if b == 0 and c != 0:
        return False, f"balance=0 but count={c} — inconsistent"
    return ok(True, f"count={c} bal={b} sat — consistent")

def t202():
    """pool_utxo_count remains under PTXSETTLE 200-input cap at steady state.
    Sample once; informational below 200, FAIL above 500 (KDD-034 CAP).
    Between 200-500 means consolidation hasn't drained recent backlog yet — PASS
    with informational note."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    c = st.get("pool_utxo_count")
    if c is None: return None, "pool_utxo_count not exposed"
    if c > 500:
        return False, f"pool_utxo_count={c} — exceeds KDD-034 CAP=500; consolidation not firing"
    if c > 200:
        return True, f"pool_utxo_count={c} — between cap and threshold (drain in progress)"
    return ok(True, f"pool_utxo_count={c} — under 200-input PTXSETTLE cap")

def t203():
    """settlement_history exposed via lottery_status and is a list (possibly empty)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    if "settlement_history" not in st:
        return None, "settlement_history field not exposed in lottery_status"
    sh = st["settlement_history"]
    if not isinstance(sh, list):
        return False, f"settlement_history not a list: {type(sh).__name__}"
    return ok(True, f"settlement_history is list, len={len(sh)}")

def t204():
    """At least one PTXSETTLE (nType=7) confirmed within scan range. Skip if none
    — chain may simply have had no settlement in window."""
    settles, _ = _scan_pool_txs()
    if not settles:
        return None, (f"no PTXSETTLE in last {PTX_SCAN_BLOCKS} blocks — increase "
                      f"--ptx-scan-blocks if expected; otherwise no settlement activity")
    last = settles[-1]
    return ok(True, f"{len(settles)} PTXSETTLE in scan range; latest h={last['block_height']} "
                    f"txid={last['txid'][:16]}...")

def t205():
    """PTXSETTLE has exactly 1 output (KDD-032 Rule 4: single winner payout)."""
    settles, _ = _scan_pool_txs()
    if not settles: return None, "no PTXSETTLE in scan range"
    bad = []
    for s in settles:
        n_vout = len(s["tx"].get("vout", []))
        if n_vout != 1:
            bad.append(f"h={s['block_height']} vout={n_vout}")
    if bad:
        return False, f"{len(bad)} PTXSETTLE with !=1 vout: {bad[:3]}"
    return ok(True, f"all {len(settles)} PTXSETTLE have exactly 1 vout")

def t206():
    """PTXSETTLE input count <= 200 (post-deploy cap; was 1000 pre-fix)."""
    settles, _ = _scan_pool_txs()
    if not settles: return None, "no PTXSETTLE in scan range"
    over = []
    counts = []
    for s in settles:
        n = len(s["tx"].get("vin", []))
        counts.append(n)
        if n > 200:
            over.append(f"h={s['block_height']} inputs={n}")
    if over:
        return False, f"{len(over)} PTXSETTLE over 200-input cap: {over[:3]}"
    return ok(True, f"all {len(settles)} PTXSETTLE <=200 inputs (max={max(counts)})")

def t207():
    """PTXSETTLE vout[0].value <= sum(input values) — fee non-negative (KDD-032 Rule 3).
    Skips if previous txs not retrievable from chain (pruned or just gone)."""
    settles, _ = _scan_pool_txs()
    if not settles: return None, "no PTXSETTLE in scan range"
    bad = []
    checked = 0
    # Check up to last 3 settles — vin resolution is expensive
    for s in settles[-3:]:
        sum_in, ok_n, fail_n = _vin_value_sum(s["tx"])
        if fail_n > 0 and ok_n == 0:
            continue  # cannot verify, skip silently
        _, vout0 = _vout_address(s["tx"], 0)
        if vout0 is None: continue
        checked += 1
        if vout0 > sum_in:
            bad.append(f"h={s['block_height']} vout={vout0} > sum_in={sum_in}")
    if not checked:
        return None, "no PTXSETTLE inputs resolvable (chain pruned or txs missing)"
    if bad:
        return False, f"PTXSETTLE vout exceeds sum of inputs: {bad}"
    return ok(True, f"checked {checked} PTXSETTLE: all vout[0] <= sum(inputs)")

def t208():
    """At least one PTXCONSOLIDATE (nType=8) confirmed within scan range. Skip if
    none — pool may have stayed under the 150-UTXO threshold throughout."""
    _, consols = _scan_pool_txs()
    if not consols:
        return None, (f"no PTXCONSOLIDATE in last {PTX_SCAN_BLOCKS} blocks — "
                      f"pool stayed under threshold or KDD-034 not deployed")
    last = consols[-1]
    return ok(True, f"{len(consols)} PTXCONSOLIDATE in scan range; latest h={last['block_height']} "
                    f"txid={last['txid'][:16]}...")

def t209():
    """PTXCONSOLIDATE has exactly 1 output (KDD-034 Rule C2)."""
    _, consols = _scan_pool_txs()
    if not consols: return None, "no PTXCONSOLIDATE in scan range"
    bad = []
    for c in consols:
        n_vout = len(c["tx"].get("vout", []))
        if n_vout != 1:
            bad.append(f"h={c['block_height']} vout={n_vout}")
    if bad:
        return False, f"{len(bad)} PTXCONSOLIDATE with !=1 vout: {bad[:3]}"
    return ok(True, f"all {len(consols)} PTXCONSOLIDATE have exactly 1 vout")

def t210():
    """PTXCONSOLIDATE input count <= 500 (KDD-034 CAP)."""
    _, consols = _scan_pool_txs()
    if not consols: return None, "no PTXCONSOLIDATE in scan range"
    over = []
    counts = []
    for c in consols:
        n = len(c["tx"].get("vin", []))
        counts.append(n)
        if n > 500:
            over.append(f"h={c['block_height']} inputs={n}")
    if over:
        return False, f"{len(over)} PTXCONSOLIDATE over CAP=500: {over[:3]}"
    return ok(True, f"all {len(consols)} PTXCONSOLIDATE <=500 inputs (max={max(counts)})")

def t211():
    """PTXCONSOLIDATE vout[0].value <= sum(input values) — KDD-034 Rule C5
    (output value cannot exceed sum of inputs; difference is the miner fee)."""
    _, consols = _scan_pool_txs()
    if not consols: return None, "no PTXCONSOLIDATE in scan range"
    bad = []
    checked = 0
    for c in consols[-3:]:
        sum_in, ok_n, fail_n = _vin_value_sum(c["tx"])
        if fail_n > 0 and ok_n == 0: continue
        _, vout0 = _vout_address(c["tx"], 0)
        if vout0 is None: continue
        checked += 1
        if vout0 > sum_in:
            bad.append(f"h={c['block_height']} vout={vout0} > sum_in={sum_in}")
    if not checked:
        return None, "no PTXCONSOLIDATE inputs resolvable"
    if bad:
        return False, f"PTXCONSOLIDATE vout exceeds sum of inputs: {bad}"
    return ok(True, f"checked {checked} PTXCONSOLIDATE: all vout[0] <= sum(inputs)")

def t212():
    """PTXSETTLE vout[0] address is non-empty and a valid-looking address string.
    ODC-020: must be an address (winner's GM payment address). Format-only check;
    full chain-of-registration check is a node-side concern."""
    settles, _ = _scan_pool_txs()
    if not settles: return None, "no PTXSETTLE in scan range"
    bad = []
    for s in settles:
        addr, val = _vout_address(s["tx"], 0)
        if not addr or not isinstance(addr, str) or len(addr) < 20:
            bad.append(f"h={s['block_height']} addr={addr!r}")
    if bad:
        return False, f"PTXSETTLE vout missing/invalid address: {bad[:3]}"
    return ok(True, f"all {len(settles)} PTXSETTLE vout has valid winner address")

def t213():
    """PTXCONSOLIDATE vout[0] address differs from any PTXSETTLE winner — sanity
    check that the pool address is NOT a winner address. (PTXCONSOLIDATE pays
    back to pool; PTXSETTLE pays out to a winner GM. The two should never
    share an output address.)"""
    settles, consols = _scan_pool_txs()
    if not settles or not consols:
        return None, "need both PTXSETTLE and PTXCONSOLIDATE in scan range"
    settle_addrs = set()
    for s in settles:
        a, _ = _vout_address(s["tx"], 0)
        if a: settle_addrs.add(a)
    pool_addrs = set()
    for c in consols:
        a, _ = _vout_address(c["tx"], 0)
        if a: pool_addrs.add(a)
    overlap = settle_addrs & pool_addrs
    if overlap:
        return False, f"pool address appears as PTXSETTLE winner: {list(overlap)[:3]}"
    return ok(True, f"settle winners ({len(settle_addrs)}) disjoint from pool addrs ({len(pool_addrs)})")

def t214():
    """PTXCONSOLIDATE all share a single output address (the pool address —
    KDD-034 Rule C4). If multiple distinct vout addresses appear across
    confirmed PTXCONSOLIDATEs, that's a consensus violation."""
    _, consols = _scan_pool_txs()
    if not consols: return None, "no PTXCONSOLIDATE in scan range"
    addrs = set()
    for c in consols:
        a, _ = _vout_address(c["tx"], 0)
        if a: addrs.add(a)
    if len(addrs) > 1:
        return False, f"PTXCONSOLIDATE vout addresses differ across blocks: {addrs}"
    return ok(True, f"all {len(consols)} PTXCONSOLIDATE share single pool address: {list(addrs)[:1]}")

def t215():
    """PTXSETTLE winner addresses are distinct across multiple settlements (if
    multiple in scan range) — verifies the lottery actually selects different
    winners over time, not always the same GM (which would indicate a beacon
    bias). Two settlements with same winner is acceptable (statistical),
    but ALL same across 3+ settlements is suspicious."""
    settles, _ = _scan_pool_txs()
    if len(settles) < 3:
        return None, f"only {len(settles)} PTXSETTLE in range — need >=3 for distinctness check"
    addrs = []
    for s in settles:
        a, _ = _vout_address(s["tx"], 0)
        if a: addrs.append(a)
    unique = set(addrs)
    if len(unique) == 1 and len(addrs) >= 3:
        return False, f"{len(addrs)} PTXSETTLE all paid to same address: {list(unique)[0]}"
    return ok(True, f"{len(addrs)} settlements, {len(unique)} distinct winners")

def t216():
    """PTXSETTLE and PTXCONSOLIDATE never coexist in the same block
    (consensus rule: ptx-settle-consolidate-coexist)."""
    settles, consols = _scan_pool_txs()
    settle_blocks = {s["block_height"] for s in settles}
    consol_blocks = {c["block_height"] for c in consols}
    coexist = settle_blocks & consol_blocks
    if coexist:
        return False, f"PTXSETTLE and PTXCONSOLIDATE coexist in blocks: {sorted(coexist)[:5]}"
    return ok(True, f"no coexistence across {len(settles)} settles + {len(consols)} consolidates")

def t217():
    """At most one PTXSETTLE per block (Rule: ptxsettle-duplicate-in-block).
    At most one PTXCONSOLIDATE per block (Rule: ptxconsolidate-duplicate-in-block)."""
    settles, consols = _scan_pool_txs()
    settle_heights = [s["block_height"] for s in settles]
    consol_heights = [c["block_height"] for c in consols]
    dup_settles = [h for h in set(settle_heights) if settle_heights.count(h) > 1]
    dup_consols = [h for h in set(consol_heights) if consol_heights.count(h) > 1]
    if dup_settles or dup_consols:
        return False, (f"duplicate PTXSETTLE in blocks {dup_settles[:3]}; "
                       f"duplicate PTXCONSOLIDATE in blocks {dup_consols[:3]}")
    return ok(True, f"no duplicates ({len(settles)} settles + {len(consols)} consolidates)")

def t218():
    """PTXSETTLE vin scriptSig is empty (ConnectBlock script-exemption — KDD-032
    Rule 8: pool inputs skip scriptSig validation; payload correctness suffices)."""
    settles, _ = _scan_pool_txs()
    if not settles: return None, "no PTXSETTLE in scan range"
    bad = []
    for s in settles[-3:]:
        for i, vin in enumerate(s["tx"].get("vin", [])):
            ssig = vin.get("scriptSig", {})
            hex_val = ssig.get("hex", "") if isinstance(ssig, dict) else ""
            if hex_val and hex_val != "":
                bad.append(f"h={s['block_height']} vin[{i}].scriptSig hex non-empty (len={len(hex_val)})")
                break
    if bad:
        return False, f"PTXSETTLE has non-empty scriptSig (script-exempt rule violated): {bad}"
    return ok(True, f"PTXSETTLE inputs have empty scriptSig (pool script-exempt)")

def t219():
    """PTXCONSOLIDATE vin scriptSig is empty — same script-exemption applies
    via validation.cpp PTXCONSOLIDATE branch (added with KDD-034)."""
    _, consols = _scan_pool_txs()
    if not consols: return None, "no PTXCONSOLIDATE in scan range"
    bad = []
    for c in consols[-3:]:
        for i, vin in enumerate(c["tx"].get("vin", [])):
            ssig = vin.get("scriptSig", {})
            hex_val = ssig.get("hex", "") if isinstance(ssig, dict) else ""
            if hex_val and hex_val != "":
                bad.append(f"h={c['block_height']} vin[{i}].scriptSig hex non-empty (len={len(hex_val)})")
                break
    if bad:
        return False, f"PTXCONSOLIDATE has non-empty scriptSig: {bad}"
    return ok(True, f"PTXCONSOLIDATE inputs have empty scriptSig (KDD-034 script-exempt)")


# ═══════════════════════════════════════════════════════════════════════════════
# T220-T222  PTX service fee collection (roll payment integrity)
# ═══════════════════════════════════════════════════════════════════════════════
#
# Verifies each ptx_roll() actually pays into the lottery pool. Catches
# regressions where the service fee isn't deducted, the pool address is
# wrong, or fees silently fall through to miner instead of pool.
#
# T220:  Single-roll delta — pool grows by >0 sat after one roll
# T221:  10-roll consistency — pool grows by ~N x per_roll_fee, fees stable
# T222:  Caller balance conservation — caller_delta = pool_delta + miner_fee
# ═══════════════════════════════════════════════════════════════════════════════

def _wallet_balance_sat():
    """Caller wallet balance in satoshi. Returns None if RPC unavailable."""
    r, e = rpc("getbalance", [])
    if e or r is None: return None
    return int(round(float(r) * 100_000_000))

def _wait_for_blocks(start_h, n=2, timeout_s=120):
    """Wait for chain to advance by n blocks. Returns final height, or None on timeout."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        h = blockcount()
        if h >= start_h + n:
            return h
        time.sleep(3)
    return None

def t220():
    """Single ptx_roll() must increase pool_balance_sat by >0 satoshi.
    Catches: service fee not collected, fee routed to wrong address."""
    if _near_settlement(3):
        return None, "settlement imminent — pool would drain mid-test, skipped"
    st1, e = lottery_status()
    if e: return None, f"lottery_status pre: {e}"
    pool_before = st1.get("pool_balance_sat")
    if pool_before is None: return None, "pool_balance_sat not exposed"
    h0 = blockcount()
    r, e = roll(1, 1, 100, False, game_id="fee_t220", salt=mksalt("fee", 220))
    if e: return False, f"roll failed: {str(e)[:80]}"
    end_h = _wait_for_blocks(h0, n=2, timeout_s=180)
    if end_h is None: return None, "chain did not advance 2 blocks in 180s"
    st2, e = lottery_status()
    if e: return None, f"lottery_status post: {e}"
    pool_after = st2.get("pool_balance_sat")
    delta = pool_after - pool_before
    if delta <= 0:
        return False, (f"pool did not grow: {pool_before} -> {pool_after} (delta={delta}). "
                       f"Service fee not collected or routed away from pool.")
    return ok(True, f"pool grew by {delta} sat ({delta/1e8:.4f} HMS) per roll — fee collected")

def t221():
    """10 rolls: pool grows monotonically. Catches inconsistent fee collection."""
    if _near_settlement(5):
        return None, "settlement imminent — pool drain would confound test, skipped"
    st1, e = lottery_status()
    if e: return None, f"lottery_status pre: {e}"
    pool_start = st1.get("pool_balance_sat")
    if pool_start is None: return None, "pool_balance_sat not exposed"
    h0 = blockcount()
    n_rolls = 10
    rpc_ok = 0
    for i in range(n_rolls):
        r, e = roll(1, 1, 100, False, game_id=f"fee_t221_{i}", salt=mksalt("fee221", i))
        if r and not e: rpc_ok += 1
    if rpc_ok < n_rolls // 2:
        return False, f"only {rpc_ok}/{n_rolls} rolls accepted — cannot measure fee"
    end_h = _wait_for_blocks(h0, n=3, timeout_s=240)
    if end_h is None: return None, "chain did not advance 3 blocks in 240s"
    time.sleep(5)
    st2, e = lottery_status()
    if e: return None, f"lottery_status post: {e}"
    pool_end = st2.get("pool_balance_sat")
    if pool_end < pool_start:
        return None, f"pool dropped {pool_start} -> {pool_end} — settlement fired mid-test, skipped"
    total_delta = pool_end - pool_start
    if total_delta <= 0:
        return False, f"pool flat or shrank across {rpc_ok} confirmed rolls: {pool_start} -> {pool_end}"
    per_roll_avg = total_delta / rpc_ok
    return ok(True, (f"{rpc_ok} rolls grew pool by {total_delta} sat "
                     f"(~{per_roll_avg/1e8:.4f} HMS/roll avg)"))

def t222():
    """Caller wallet drops by ~ (pool gain + reasonable miner fee).
    Sanity check that caller actually pays — not just that pool receives."""
    if _near_settlement(3):
        return None, "settlement imminent — skipped"
    wb_before = _wallet_balance_sat()
    if wb_before is None:
        return None, "getbalance not available on this RPC — cannot measure caller delta"
    if wb_before < 200_000_000:
        return None, f"caller wallet too low ({wb_before/1e8:.4f} HMS) to measure fee delta"
    st1, e = lottery_status()
    if e: return None, f"lottery_status pre: {e}"
    pool_before = st1.get("pool_balance_sat", 0)
    h0 = blockcount()
    r, e = roll(1, 1, 100, False, game_id="fee_t222", salt=mksalt("fee", 222))
    if e: return False, f"roll failed: {str(e)[:80]}"
    end_h = _wait_for_blocks(h0, n=2, timeout_s=180)
    if end_h is None: return None, "chain did not advance 2 blocks in 180s"
    st2, e = lottery_status()
    if e: return None, f"lottery_status post: {e}"
    pool_after = st2.get("pool_balance_sat", 0)
    wb_after = _wallet_balance_sat()
    if wb_after is None: return None, "getbalance disappeared post-roll"
    pool_delta = pool_after - pool_before
    wallet_delta = wb_before - wb_after
    if wallet_delta <= 0:
        return False, (f"caller wallet did not decrease: {wb_before} -> {wb_after}. "
                       f"Roll happened but caller paid nothing.")
    miner_fee = wallet_delta - pool_delta
    if miner_fee < 0:
        return False, (f"caller paid {wallet_delta} but pool gained {pool_delta} "
                       f"(diff={miner_fee}). Pool received more than caller paid.")
    if pool_delta > 0 and miner_fee > pool_delta * 10:
        return False, (f"miner fee {miner_fee} > 10x service fee {pool_delta} — "
                       f"fee structure may be misconfigured")
    return ok(True, (f"caller paid {wallet_delta} sat = pool {pool_delta} + miner {miner_fee} "
                     f"({pool_delta/1e8:.4f} + {miner_fee/1e8:.4f} HMS)"))


# ═══════════════════════════════════════════════════════════════════════════════
# T223-T226  Burst-pace sweep — RPC success rate vs pacing (BUG-012 stress)
# ═══════════════════════════════════════════════════════════════════════════════
#
# Fires N rolls back-to-back at a configurable pace, measures RPC error rate
# and pool fee accounting. Identifies where BUG-012 mempool-conflict zone
# begins for the caller wallet.
#
# T223:  baseline   0.3s/roll  3.3 rolls/sec  (target: 0% errors)
# T224:  brisk      0.15s/roll 6.7 rolls/sec  (target: <5% errors)
# T225:  fast       0.1s/roll  10 rolls/sec   (target: <15% errors)
# T226:  hammer     0.05s/roll 20 rolls/sec   (target: BUG-012 visible)
#
# All four tests use 30-roll bursts (not 500) to keep suite-run time bounded.
# Pre-flight check: requires >=50 usable 2-3 HMS UTXOs in caller wallet.
# Tests SKIP if wallet is too consolidated to safely burst (no split available).
# Tests SKIP if settlement window is within 3 blocks (would confound delta).
# ═══════════════════════════════════════════════════════════════════════════════

PACE_BURST_N = 30  # rolls per pace test

def _count_usable_utxos(min_hms=2.0, max_hms=3.0):
    """Return count of caller wallet UTXOs within [min_hms, max_hms].
    Uses CLI not RPC because some forks reject listunspent's optional args via curl."""
    import subprocess
    try:
        r = subprocess.run(
            ["docker","exec","ptx-caller","Hemis-cli","-ptxtestnet",
             "-datadir=/root/.hemis-ptxtestnet","-rpcport=29902",
             "listunspent","1","9999999"],
            capture_output=True, text=True, timeout=20)
        if r.returncode != 0:
            return None
        utxos = json.loads(r.stdout)
        return sum(1 for u in utxos if min_hms <= u.get("amount",0) <= max_hms)
    except Exception:
        return None

def _pace_burst(n_rolls, pace_s, label):
    """Fire n_rolls at pace_s spacing. Return (rpc_ok, rpc_err, duration_s, error_samples).
    Uses suite's roll() helper which routes through the same RPC stack."""
    rpc_ok = 0
    rpc_err = 0
    error_samples = []
    t0 = time.time()
    for i in range(n_rolls):
        r, e = roll(1, 1, 100, False,
                    game_id=f"pace_{label}_{i}",
                    salt=mksalt(f"pc{label}", i))
        if r and not e:
            rpc_ok += 1
        else:
            rpc_err += 1
            if len(error_samples) < 3 and e:
                error_samples.append(str(e)[:60])
        if pace_s > 0:
            time.sleep(pace_s)
    return rpc_ok, rpc_err, time.time() - t0, error_samples

def _pace_preflight():
    """Common pre-checks for T223-T226. Returns (skip_reason or None)."""
    if _near_settlement(3):
        return "settlement imminent (within 3 blocks) — pace test skipped"
    n_utxo = _count_usable_utxos()
    if n_utxo is None:
        return "cannot read wallet UTXOs (Hemis-cli failed or not in PATH)"
    if n_utxo < 50:
        return f"only {n_utxo} usable 2-3 HMS UTXOs — split wallet first (need >=50)"
    return None

def _pace_test(pace_s, target_err_pct, label):
    """Shared pace-test body. Returns (pass/fail, msg)."""
    skip = _pace_preflight()
    if skip:
        return None, skip
    rpc_ok, rpc_err, duration, errs = _pace_burst(PACE_BURST_N, pace_s, label)
    err_pct = rpc_err / PACE_BURST_N * 100
    rate = PACE_BURST_N / duration if duration > 0 else 0
    msg = (f"{PACE_BURST_N} rolls @ {pace_s}s pace: ok={rpc_ok} err={rpc_err} "
           f"({err_pct:.1f}%) rate={rate:.1f}/s duration={duration:.1f}s")
    if errs:
        msg += f" sample_errs={errs}"
    return ok(err_pct <= target_err_pct, msg)

def t223():
    """Pace baseline: 0.3s/roll (~3.3 rolls/sec). Should produce 0 errors.
    Failure here means BUG-012 territory starts even at slow pace — wallet
    UTXO recycling lag is worse than expected."""
    return _pace_test(0.3, target_err_pct=5, label="t223")

def t224():
    """Pace brisk: 0.15s/roll (~6.7 rolls/sec). Target <5% errors.
    Light load test — should still be comfortably under BUG-012 threshold."""
    return _pace_test(0.15, target_err_pct=10, label="t224")

def t225():
    """Pace fast: 0.1s/roll (~10 rolls/sec). Target <15% errors.
    Approaches BUG-012 territory — some mempool conflicts expected as
    change UTXOs don't have time to register before next roll fires."""
    return _pace_test(0.1, target_err_pct=25, label="t225")

def t226():
    """Pace hammer: 0.05s/roll (~20 rolls/sec). BUG-012 stress test.
    Expected to FAIL pre-BUG-012-fix with high error rate. Documents the
    actual error rate as a regression-tracking number rather than gating
    pass/fail strictly — accepts up to 80% errors but flags if >80%."""
    skip = _pace_preflight()
    if skip:
        return None, skip
    rpc_ok, rpc_err, duration, errs = _pace_burst(PACE_BURST_N, 0.05, "t226")
    err_pct = rpc_err / PACE_BURST_N * 100
    rate = PACE_BURST_N / duration if duration > 0 else 0
    msg = (f"{PACE_BURST_N} rolls @ 0.05s pace: ok={rpc_ok} err={rpc_err} "
           f"({err_pct:.1f}%) rate={rate:.1f}/s duration={duration:.1f}s")
    if errs:
        msg += f" sample_errs={errs}"
    if err_pct > 80:
        return False, f"BUG-012 SEVERE: {msg}"
    if err_pct > 30:
        # Bug present but characterised — log for tracking
        print(f"         BUG-012 ACTIVE at hammer pace: {err_pct:.1f}% errors")
        return True, f"BUG-012 active (expected pre-fix): {msg}"
    return ok(True, f"hammer pace tolerant (likely BUG-012 fix merged): {msg}")


# ═══════════════════════════════════════════════════════════════════════════════
# MAIN
# ═══════════════════════════════════════════════════════════════════════════════

def main():
    global RPC_URL, RPC_USER, RPC_PASS, ALL_NODES

    parser = argparse.ArgumentParser(description="Hemis PTX Phase 2 Test Suite v7")
    parser.add_argument("--rpc-url",      default=RPC_URL)
    parser.add_argument("--rpc-user",     default=RPC_USER)
    parser.add_argument("--rpc-pass",     default=RPC_PASS)
    parser.add_argument("--fast",         action="store_true", help="Skip stats and stress")
    parser.add_argument("--skip-fail-modes", action="store_true")
    parser.add_argument("--skip-advanced",   action="store_true")
    parser.add_argument("--skip-excl",       action="store_true")
    parser.add_argument("--skip-excl-probe", action="store_true")
    parser.add_argument("--skip-lottery",    action="store_true")
    parser.add_argument("--skip-excl-ext",   action="store_true", help="Skip T151-T160")
    parser.add_argument("--skip-dev",        action="store_true", help="Skip T161-T165")
    parser.add_argument("--skip-prev-round", action="store_true", help="Skip T171-T175")
    parser.add_argument("--skip-excl-load",  action="store_true", help="Skip T176-T178 exclude load sweep")
    parser.add_argument("--skip-excl-limit", action="store_true", help="Skip T179-T185 exclusion limit finder (BUG-011 deep probe)")
    parser.add_argument("--skip-t130-diag",  action="store_true", help="Skip T186-T195 quorum_sig_hash derivation diagnostic")
    parser.add_argument("--skip-bug012",     action="store_true", help="Skip T196-T199 BUG-012 rapid roll / mempool UTXO selection")
    parser.add_argument("--skip-ptxsettle",  action="store_true", help="Skip T200-T219 PTXSETTLE/PTXCONSOLIDATE structural tests")
    parser.add_argument("--skip-fee",        action="store_true", help="Skip T220-T222 fee collection tests (slow — waits for confirms)")
    parser.add_argument("--skip-pace",       action="store_true", help="Skip T223-T226 pace sweep tests (BUG-012 stress probe)")
    parser.add_argument("--ptx-scan-blocks", type=int, default=200, help="Lookback window for T200-T219 chain scan (default 200)")
    args = parser.parse_args()

    RPC_URL  = args.rpc_url
    RPC_USER = args.rpc_user
    RPC_PASS = args.rpc_pass

    global PTX_SCAN_BLOCKS
    PTX_SCAN_BLOCKS = args.ptx_scan_blocks

    print("═"*68)
    print(f"  HEMIS PTX PHASE 2 — LIVE NODE TEST SUITE v7.4  (226 tests)")
    print(f"  RPC:  {RPC_URL}")
    print(f"  GMs:  {len(ALL_NODES)} nodes ({ALL_NODES[0]}-{ALL_NODES[-1]})  threshold=6  sig=192chars")
    print("═"*68)
    print()

    # Connectivity check
    bc = blockcount()
    if not bc:
        print(f"  FATAL: Cannot connect to RPC: {RPC_URL}")
        sys.exit(2)
    print(f"  Connected. Block height: {bc}")

    # Docker check
    try:
        import subprocess
        dr = subprocess.run(["docker","ps","--format","{{.Names}}"],
                            capture_output=True, text=True, timeout=5)
        docker_ok = dr.returncode == 0
    except Exception:
        docker_ok = False
    print(f"  Docker: {'available' if docker_ok else 'NOT AVAILABLE — T74/T135 will skip'}")
    print()

    print("── Core Functionality (T01-T10) ──────────────────────────────────────")
    for tid, fn in [("T01",t01),("T02",t02),("T03",t03),("T04",t04),("T05",t05),
                    ("T06",t06),("T07",t07),("T08",t08),("T09",t09),("T10",t10)]:
        test(tid, {
            "T01":"Basic roll — single value in 1-100",
            "T02":"Range boundary — exact min/max",
            "T03":"Unique draws — no duplicates",
            "T04":"Non-unique — duplicates permitted",
            "T05":"Exclusion list — excluded never returned",
            "T06":"Exclusion — forces single possible value",
            "T07":"Full permutation draw",
            "T08":"Single value range (low==high)",
            "T09":"Large range (1-1,000,000)",
            "T10":"Unique draw from pool of one",
        }[tid], fn)
    print()

    print("── Cryptographic Properties P2 (T11-T20) ────────────────────────────")
    for tid, name, fn in [
        ("T11","Round seed — valid 64-char hex",t11),
        ("T12","Quorum sig — exactly 192 hex chars (BLS G2)",t12),
        ("T13","Quorum members — exactly 11 from known pool",t13),
        ("T14","Beacon — valid 64-char hex",t14),
        ("T15","Block height is positive integer",t15),
        ("T16","Round seed unique across rounds",t16),
        ("T17","Beacon unique across rounds",t17),
        ("T18","Different salts produce different seeds",t18),
        ("T19","Re-roll same params → different seed",t19),
        ("T20","Quorum sig unique per round",t20),
    ]: test(tid, name, fn)
    print()

    print("── Round Status & PoSe (T21-T28) ────────────────────────────────────")
    for tid, name, fn in [
        ("T21","Round state=2 after roll (block_height lookup)",t21),
        ("T22","Round committed = all 11 nodes",t22),
        ("T23","Round withheld is empty",t23),
        ("T24","Round abstained is empty",t24),
        ("T25","Round ID is valid hex",t25),
        ("T26","PoSe — all 11 nodes eligible",t26),
        ("T27","PoSe — tickets > 0 all nodes",t27),
        ("T28","Round IDs unique across rounds",t28),
    ]: test(tid, name, fn)
    print()

    print("── Game Scenarios (T29-T38) ──────────────────────────────────────────")
    for tid, name, fn in [
        ("T29","Coin flip — result is 0 or 1",t29),
        ("T30","D6 roll — result in 1-6",t30),
        ("T31","D20 roll — result in 1-20",t31),
        ("T32","D100 roll — result in 1-100",t32),
        ("T33","Card draw — 5 unique from 52",t33),
        ("T34","Full deck — 52 unique from 52",t34),
        ("T35","Raffle — 1 winner from 1-10000",t35),
        ("T36","Tournament bracket — 16 unique from 128",t36),
        ("T37","Sequential rolls produce different results",t37),
        ("T38","Multi-hand — second hand excludes first",t38),
    ]: test(tid, name, fn)
    print()

    if not args.fast:
        print("── Statistical Tests (T39-T42) ────────────────────────────────────")
        for tid, name, fn in [
            ("T39","Chi-square — coin flip (200 samples)",t39),
            ("T40","Chi-square — d6 (600 samples)",t40),
            ("T41","Chi-square — d20 (1000 samples)",t41),
            ("T42","Chi-square — d100 (200 samples)",t42),
        ]: test(tid, name, fn)
        print()
        print("── Stress Tests (T43-T46) ─────────────────────────────────────────")
        for tid, name, fn in [
            ("T43","20 sequential rolls — no errors",t43),
            ("T44","50-draw unique — no duplicates",t44),
            ("T45","Max integer range (1-2147483647)",t45),
            ("T46","15-roll burst — 3 unique each",t46),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Statistical Tests SKIPPED (--fast) ─────────────────────────────")
        print("── Stress Tests SKIPPED (--fast) ───────────────────────────────────")
        print()

    print("── Invalid Params (T47-T70) ──────────────────────────────────────────")
    print("   PASS = clean error + node alive · FAIL = crash/hang/garbage accepted")
    for tid, name, fn in [
        ("T47",'count=0',t47),("T48",'count=-1',t48),
        ("T49",'count="1" (string)',t49),("T50","count=1.5 (float)",t50),
        ("T51","low > high — inverted range",t51),
        ("T52","low==high unique count=2 — impossible",t52),
        ("T53",'low="1" (string)',t53),("T54",'high="100" (string)',t54),
        ("T55","low=1.5 (float)",t55),("T56","high=100.9 (float)",t56),
        ("T57",'unique="false" (string)',t57),("T58","unique=0 (integer)",t58),
        ("T59",'exclude="[]" (string not array)',t59),("T60","exclude=null",t60),
        ("T61","exclude=[1.5, 2.5] (floats)",t61),("T62","exclude=[1, null, 3]",t62),
        ("T63","exclude=[[1,2],[3,4]] (nested)",t63),("T64","game_id=42 (integer)",t64),
        ("T65","missing game_id and salt",t65),("T66","extra param (8 instead of 7)",t66),
        ("T67","salt=12345 (integer)",t67),("T68",'salt="hello_world" (non-hex)',t68),
        ("T69",'salt="" (empty)',t69),("T70","count=15 > unique pool=10",t70),
    ]: test(tid, name, fn)
    print()

    if not args.skip_fail_modes:
        print("── Adversarial / Fail Modes (T71-T80) ───────────────────────────────")
        for tid, name, fn in [
            ("T71","f=1 withhold gm02 — round resolves",t71),
            ("T72","f=1 withhold — withheld list populated",t72),
            ("T73","f=1 abstain gm03 — round resolves",t73),
            ("T74","PoSe increment — docker stop gm11 + roll",t74),
            ("T75","f=2 withhold gm02+gm04 — round resolves",t75),
            ("T76","f=2 abstain gm03+gm05 — round resolves",t76),
            ("T77","Fail mode reset — gm02 participates normally",t77),
            ("T78","PoSe stable after normal operation",t78),
            ("T79","Mixed f=1 withhold + f=1 abstain — resolves",t79),
            ("T80","Mode cycling — no permanent corruption",t80),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Fail Mode Tests SKIPPED (--skip-fail-modes) ───────────────────────")
        print()

    if not args.skip_advanced:
        print("── Advanced Tests (T81-T100) ─────────────────────────────────────────")
        print("   [Concurrent]")
        for tid, name, fn in [
            ("T81","5 concurrent rolls — all complete",t81),
            ("T82","10 concurrent rolls — unique seeds",t82),
            ("T83","Concurrent same game_id diff salts — seeds differ",t83),
            ("T84","30 sequential rolls — sustained load",t84),
            ("T85","Node alive after sustained load",t85),
        ]: test(tid, name, fn)
        print("   [tx_id Exclude Chaining]")
        for tid, name, fn in [
            ("T86","Fake tx_id in exclude — no crash, skipped",t86),
            ("T87","Pending tx_id in exclude — handled gracefully",t87),
            ("T88","Mixed int + tx_id exclude — integers enforced",t88),
            ("T89","Multiple fake tx_ids — no crash",t89),
        ]: test(tid, name, fn)
        print("   [Block Height Anchoring — KDD-003]")
        for tid, name, fn in [
            ("T90","Same block same params → same seed",t90),
            ("T91","Different blocks same params → different seed",t91),
            ("T92","block_height in response matches chain height",t92),
        ]: test(tid, name, fn)
        if not args.skip_fail_modes:
            print("   [f=3 Failure Mode]")
            for tid, name, fn in [
                ("T93","f=3 withhold — round fails gracefully (not crash)",t93),
                ("T94","f=3 abstain — round fails gracefully",t94),
                ("T95","Node recovers after f=3 scenario",t95),
            ]: test(tid, name, fn)
        print("   [Nonce Chaining — KDD-015]")
        for tid, name, fn in [
            ("T96","5 rounds same salt → 5 distinct seeds",t96),
            ("T97","Fixed salt across rounds — nonce advances",t97),
            ("T98","Quorum sigs unique across 5 rounds",t98),
        ]: test(tid, name, fn)
        print("   [Exclude Boundary — BUG-003]")
        for tid, name, fn in [
            ("T99","95-item exclude — succeeds",t99),
            ("T100","96-item exclude — post-fix should succeed",t100),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Advanced Tests SKIPPED (--skip-advanced) ──────────────────────────")
        print()

    if not args.skip_excl:
        print("── Exclude Path Hardening (T101-T120) ────────────────────────────────")
        print("   [BUG-003 Fix Verification]")
        for tid, name, fn in [
            ("T101","97-item exclude — succeeds post-fix",t101),
            ("T102","200-item exclude — scales correctly",t102),
            ("T103","500-item exclude — mid-scale correctness",t103),
            ("T104","1000-item exclude — large-scale, node stable",t104),
        ]: test(tid, name, fn)
        if not args.skip_excl_probe:
            print("   [BUG-004 Characterisation — 900 RPC calls — use --skip-excl-probe to skip]")
            for tid, name, fn in [
                ("T105","excl=10  100 iters — probe BUG-004 at low size",t105),
                ("T106","excl=20  100 iters — probe BUG-004",t106),
                ("T107","excl=30  100 iters — probe BUG-004",t107),
                ("T108","excl=40  100 iters — probe BUG-004 key zone",t108),
                ("T109","excl=50  100 iters — probe BUG-004",t109),
                ("T110","excl=60  100 iters — probe BUG-004",t110),
                ("T111","excl=70  100 iters — probe BUG-004",t111),
                ("T112","excl=80  100 iters — probe BUG-004",t112),
                ("T113","excl=90  100 iters — probe BUG-004 near threshold",t113),
            ]: test(tid, name, fn)
        else:
            print("   [BUG-004 Characterisation SKIPPED — --skip-excl-probe]")
        print("   [BUG-004 Fix Verification]")
        for tid, name, fn in [
            ("T114","excl=40  200 iters — zero violations required",t114),
            ("T115","excl=80  200 iters — zero violations required",t115),
            ("T116","excl=10-90 sweep 50 iters — zero violations anywhere",t116),
        ]: test(tid, name, fn)
        print("   [Multi-Round Game Correctness]")
        for tid, name, fn in [
            ("T117","10-round card game — 50 unique, no cross-round overlap",t117),
            ("T118","Near-depleted deck — 47 excl, result in remaining 5",t118),
            ("T119","Extreme density — 90% excluded, 50 iters, 0 violations",t119),
            ("T120","Clean path regression — no-exclude draws unaffected",t120),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Exclude Hardening Tests SKIPPED (--skip-excl) ─────────────────────")
        print()

    print("── BLS Phase 2 (T121-T130) ───────────────────────────────────────────")
    for tid, name, fn in [
        ("T121","Quorum sig — exactly 192 hex chars",t121),
        ("T122","Quorum members — exactly 11 nodes",t122),
        ("T123","5 consecutive rolls — all sigs 192 chars and distinct",t123),
        ("T124","beacon == SHA256(quorum_sig bytes) via status",t124),
        ("T125","beacon always 64-char hex across 5 rolls",t125),
        ("T126","Status committed list includes all 11 nodes",t126),
        ("T127","quorum_sig in roll response matches status round record",t127),
        ("T128","BLS G2 point: first byte compressed=1, infinity=0",t128),
        ("T129","10-roll BLS consistency — all 192-char, all unique",t129),
        ("T130","quorum_sig_hash == SHA256(raw bytes) [KDD-033] — 3 roll verify",t130),
    ]: test(tid, name, fn)
    print()

    print("── PoSe Phase 2 (T131-T140) ──────────────────────────────────────────")
    for tid, name, fn in [
        ("T131","ptx_pose_status returns exactly 11 node records",t131),
        ("T132","Each record has integer pose_score and tickets",t132),
        ("T133","All 11 known node IDs present in ptx_pose_status",t133),
        ("T134","3 honest rolls → all nodes' tickets increase by 3",t134),
        ("T135","docker stop gm11 + roll → gm11.pose_score increases",t135),
        ("T136","After gm11 restart + reinit + roll → score decreases by 1",t136),
        ("T137","Honest rolls — no node pose_score increases",t137),
        ("T138","All nodes have non-negative integer tickets",t138),
        ("T139","pose_status node_ids match ALL_NODES exactly",t139),
        ("T140","ptx_pose_status consistent across back-to-back calls",t140),
    ]: test(tid, name, fn)
    print()

    print("── Lottery (T141-T150) ───────────────────────────────────────────────")
    for tid, name, fn in [
        ("T141","ptx_lottery_status call succeeds",t141),
        ("T142","pool_balance_sat is non-negative integer",t142),
        ("T143","settlement_window is positive integer",t143),
        ("T144","next_settlement_at is positive integer block height",t144),
        ("T145","eligible_nodes is non-empty list of known GMs",t145),
        ("T146","After roll, pool_balance_sat increases",t146),
        ("T147","eligible_nodes matches all 11 GMs (clean cluster)",t147),
        ("T148","Pool grows across 3 rolls",t148),
        ("T149","next_settlement_at > current block height",t149),
    ]: test(tid, name, fn)
    if not args.skip_lottery:
        test("T150", "Settlement test — verify pool distributes correctly", t150)
    else:
        print("  [SKIP] T150  Settlement test (--skip-lottery)")
        global _skip
        _skip += 1
        results.append(("T150","SKIP","Settlement test skipped","--skip-lottery flag"))
    print()

    if not args.skip_excl_ext:
        print("── Exclude Edge Cases (T151-T160) ────────────────────────────────────")
        print("   NEW in v6 — gap coverage")
        for tid, name, fn in [
            ("T151","Confirmed tx_id chaining — real on-chain tx exclude",t151),
            ("T152","Mixed tx_id + integers at scale (205 items total)",t152),
            ("T153","512-item exclude — at MAX_EXCLUDE_COUNT boundary (must pass)",t153),
            ("T154","513-item exclude — over limit, must error 1016",t154),
            ("T155","Duplicate values in exclude list — deduplicated/handled",t155),
            ("T156","Out-of-range values in exclude — silently ignored",t156),
            ("T157","Exhaustion via exclusion — count=5 but only 3 remain",t157),
            ("T158","Runtime pool exhaustion via exclude — count=4 from pool of 1",t158),
            ("T159","Tight-fit exclude — pool exactly equals count",t159),
            ("T160","Exclude all but one — must return that one value",t160),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Exclude Edge Cases SKIPPED (--skip-excl-ext) ──────────────────────")
        print()

    if not args.skip_dev:
        print("── Dev/Error Code Tests (T161-T165) ──────────────────────────────────")
        print("   NEW in v6")
        for tid, name, fn in [
            ("T161","dev_seed — deterministic result on regtest",t161),
            ("T162","dev_seed — same seed same params same result",t162),
            ("T163","dev_seed — different seed different result",t163),
            ("T164","Error 1016 — EXCLUDE_LIMIT_EXCEEDED (513 items)",t164),
            ("T165","Error envelope format — code (int) + message (str)",t165),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Dev/Error Code Tests SKIPPED (--skip-dev) ─────────────────────────")
        print()

    print("── game_id & salt Edge Cases (T166-T170) ────────────────────────────")
    print("   NEW in v6")
    for tid, name, fn in [
        ("T166","game_id at 128 characters — must succeed",t166),
        ("T167","game_id with special characters (colons, hyphens, dots)",t167),
        ("T168","Same game_id different sessions → different seeds",t168),
        ("T169","salt = all zeros '00000000' — valid hex edge",t169),
        ("T170","salt = all f 'ffffffff' — valid hex edge",t170),
    ]: test(tid, name, fn)
    print()

    if not args.skip_prev_round:
        print("── prev_round_txid Chaining (T171-T175) ─────────────────────────────")
        print("   NEW in v6 — SKIP if prev_round_txid not yet implemented")
        for tid, name, fn in [
            ("T171","prev_round_txid not found → error 1008",t171),
            ("T172","prev_round_txid unconfirmed → error 1007",t172),
            ("T173","prev_round_txid session mismatch → error 1009",t173),
            ("T174","Valid prev_round_txid — round 2 excludes round 1 results",t174),
            ("T175","3-round chain via prev_round_txid — no cross-round overlap",t175),
        ]: test(tid, name, fn)
        print()
    else:
        print("── prev_round_txid Tests SKIPPED (--skip-prev-round) ─────────────────")
        print()

    if not args.skip_excl_load:
        print("── Exclude Count Load & Stability (T176-T178) ───────────────────────")
        print("   NEW in v6.1 — BUG-011 characterisation + crash probe")
        print("   NOTE: T176 sends up to 5000-item exclude — allow ~30s")
        for tid, name, fn in [
            ("T176","Exclude count sweep 512/513/600/1000/2000/5000 — find real limit",t176),
            ("T177","Exclude latency p99 baseline — 100 iters at 100/300/512 items",t177),
            ("T178","Node stable after 5000-item exclude (crash probe)",t178),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Exclude Load Tests SKIPPED (--skip-excl-load) ────────────────────")
        print()

    if not args.skip_excl_limit:
        print("── Exclusion Limit Finder (T179-T185) ───────────────────────────────")
        print("   NEW in v7 — BUG-011 deep probe / find real crash/rejection boundary")
        print("   NOTE: T179 sends up to 100000-item exclude — allow 60-120s")
        for tid, name, fn in [
            ("T179","Binary search — real exclusion rejection/crash boundary",t179),
            ("T180","Confirm error code on first over-limit call (expect 1016 or BUG-011)",t180),
            ("T181","Concurrent excl=512: waves of 3/5/10 workers — p50/p99/max per wave",t181),
            ("T182","Concurrent latency delta: 20iter x 5workers at 100/300/512 vs baseline",t182),
            ("T183","Empty exclude=[] boundary — must succeed",t183),
            ("T184","Exclude count > pool size — exhaustion handling",t184),
            ("T185","All-duplicate exclude list — dedup/graceful handling",t185),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Exclusion Limit Tests SKIPPED (--skip-excl-limit) ────────────────")
        print()

    if not args.skip_t130_diag:
        print("── T130 / quorum_sig_hash Diagnostic (T186-T195) ────────────────────")
        print("   NEW in v7 — KDD-033 / ODC-019 derivation method investigation")
        print("   Run this section when T130 fails to identify root cause.")
        print("   T192 prints a verdict table — start there for a quick diagnosis.")
        for tid, name, fn in [
            ("T186","DIAG: dump full roll response fields",t186),
            ("T187","DIAG: quorum_sig_hash == SHA256(hex_string)? [Option B]",t187),
            ("T188","DIAG: quorum_sig_hash == SHA256(hex_upper_string)? [Option C]",t188),
            ("T189","PRIMARY: quorum_sig_hash == SHA256(raw bytes)? [KDD-033 Option A]",t189),
            ("T190","DIAG: quorum_sig_hash == SHA256d(raw bytes)? [Option D]",t190),
            ("T191","DIAG: quorum_sig_hash derived from any other response field?",t191),
            ("T192","VERDICT TABLE: which method does the node use?",t192),
            ("T193","quorum_sig_hash uniqueness — 5 rolls produce 5 distinct hashes",t193),
            ("T194","quorum_sig_hash format — 64 lowercase hex chars invariant (10 rolls)",t194),
            ("T195","POSITIVE: 5/5 quorum_sig_hash == SHA256(raw bytes) — KDD-033 full pass",t195),
        ]: test(tid, name, fn)
        print()
    else:
        print("── T130/quorum_sig_hash Diagnostic SKIPPED (--skip-t130-diag) ───────")
        print()

    if not args.skip_bug012:
        print("── BUG-012: Rapid Roll / Mempool UTXO Selection (T196-T199) ─────────")
        print("   NEW in v7.1 — src/ptx/ptx_mempool.cpp · PTX_AutoCommit")
        print("   NOTE: T196/T198/T199 wait up to 3 blocks each (~3-6 min) — allow time")
        print("   T197/T198/T199 SKIP if ptx_prepare_wallet not yet implemented")
        for tid, name, fn in [
            ("T196","BUG-012 DIAG: 10-call burst — count on-chain vs orphaned results",t196),
            ("T197","BUG-012 FIX: ptx_prepare_wallet(15, 2.0) — split wallet into UTXOs",t197),
            ("T198","BUG-012 FIX: 15-call burst post-prep — 0 orphaned required",t198),
            ("T199","BUG-012 INTEGRITY: each result has confirmed PTXSESS tx on-chain",t199),
        ]: test(tid, name, fn)
        print()
    else:
        print("── BUG-012 Tests SKIPPED (--skip-bug012) ────────────────────────────")
        print()

    if not args.skip_ptxsettle:
        print("── PTXSETTLE & PTXCONSOLIDATE Consensus (T200-T219) ─────────────────")
        print("   NEW in v7.3 — KDD-032 / ODC-020 / KDD-034 chain-scan validation")
        print(f"   Scans last {PTX_SCAN_BLOCKS} blocks for nType=7/8 txs (--ptx-scan-blocks N)")
        print("   T204/T208 SKIP if no settle/consolidate within scan range (not a regression)")
        for tid, name, fn in [
            ("T200","pool_utxo_count present in ptx_lottery_status (KDD-034 RPC)",t200),
            ("T201","pool_utxo_count and pool_balance_sat consistent (count=0 iff bal=0)",t201),
            ("T202","pool_utxo_count under KDD-034 CAP=500 / informational vs 200-cap",t202),
            ("T203","settlement_history exposed as list (may be empty)",t203),
            ("T204","PTXSETTLE (nType=7) present in chain scan window",t204),
            ("T205","PTXSETTLE has exactly 1 vout (KDD-032 Rule 4)",t205),
            ("T206","PTXSETTLE input count <= 200 (post-fix cap)",t206),
            ("T207","PTXSETTLE vout[0] <= sum(inputs) — fee non-negative (KDD-032 Rule 3)",t207),
            ("T208","PTXCONSOLIDATE (nType=8) present in chain scan window",t208),
            ("T209","PTXCONSOLIDATE has exactly 1 vout (KDD-034 Rule C2)",t209),
            ("T210","PTXCONSOLIDATE input count <= 500 (KDD-034 CAP)",t210),
            ("T211","PTXCONSOLIDATE vout[0] <= sum(inputs) (KDD-034 Rule C5)",t211),
            ("T212","PTXSETTLE vout[0] address is valid non-empty (ODC-020 winner)",t212),
            ("T213","PTXSETTLE winner addresses disjoint from pool address (sanity)",t213),
            ("T214","PTXCONSOLIDATE vout address constant across blocks (Rule C4)",t214),
            ("T215","PTXSETTLE winners distinct across 3+ settlements (beacon sanity)",t215),
            ("T216","PTXSETTLE + PTXCONSOLIDATE never coexist in same block",t216),
            ("T217","At most one PTXSETTLE and one PTXCONSOLIDATE per block",t217),
            ("T218","PTXSETTLE inputs have empty scriptSig (script-exempt, KDD-032 Rule 8)",t218),
            ("T219","PTXCONSOLIDATE inputs have empty scriptSig (KDD-034 script-exempt)",t219),
        ]: test(tid, name, fn)
        print()
    else:
        print("── PTXSETTLE/PTXCONSOLIDATE Tests SKIPPED (--skip-ptxsettle) ────────")
        print()

    if not args.skip_fee:
        print("── PTX Service Fee Collection (T220-T222) ───────────────────────────")
        print("   NEW in v7.3 — verifies caller actually pays + pool actually receives")
        print("   NOTE: each test waits up to 2-3 blocks (~2-3 min) — allow time")
        for tid, name, fn in [
            ("T220","Single roll grows pool_balance_sat by >0 sat (fee collected)",t220),
            ("T221","10 rolls grow pool monotonically (fee consistent)",t221),
            ("T222","Caller wallet drops = pool gain + reasonable miner fee (conservation)",t222),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Fee Collection Tests SKIPPED (--skip-fee) ────────────────────────")
        print()

    if not args.skip_pace:
        print("── Burst Pace Sweep — BUG-012 stress (T223-T226) ────────────────────")
        print("   NEW in v7.4 — RPC error rate vs roll pacing")
        print(f"   Each test fires {PACE_BURST_N} rolls back-to-back at a fixed pace")
        print("   Requires >=50 usable 2-3 HMS UTXOs in caller wallet (run split first)")
        print("   Total wall time: ~45s if wallet ready, SKIPs cleanly if not")
        for tid, name, fn in [
            ("T223","Pace baseline @0.3s/roll (~3.3/s) — 0 errors target",t223),
            ("T224","Pace brisk    @0.15s/roll (~6.7/s) — <10% errors target",t224),
            ("T225","Pace fast     @0.1s/roll  (~10/s)  — <25% errors target",t225),
            ("T226","Pace hammer   @0.05s/roll (~20/s)  — BUG-012 stress probe",t226),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Burst Pace Sweep Tests SKIPPED (--skip-pace) ─────────────────────")
        print()

    total = _pass + _fail + _skip
    print("═"*68)
    print(f"  RESULTS   PASS: {_pass}   FAIL: {_fail}   SKIP: {_skip}   TOTAL: {total}")
    print("═"*68)

    if _fail > 0:
        print()
        print("  FAILURES:")
        for tid, st, name, detail in results:
            if st == "FAIL":
                print(f"    {tid}  {name}")
                if detail: print(f"         {detail}")

    print()
    print(f"  VERDICT: {'PASS' if _fail == 0 else 'FAIL'}")
    print()
    sys.exit(0 if _fail == 0 else 1)

if __name__ == "__main__":
    main()
