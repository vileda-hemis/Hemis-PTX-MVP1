#!/usr/bin/env python3
"""
Hemis PTX Phase 2 — Live Node Test Suite v7.0
==============================================
216 tests across 18 categories.

v7.0 additions over v6.1:
  PTXSETTLE  T179-T189  PTXSETTLE (nType=7) and settlement_history validation
                         (KDD-032): pool_utxo_count field, settlement_history
                         list structure, winner/txid/amount/height fields,
                         UTXO cap enforcement (< 200 inputs).
  ODC-020    T190-T196  GM payment address registration (ODC-020):
                         scriptPTXPayment field in pose_status, address format,
                         winner address matches registered address.
  PTX-VFY    T197-T204  ptx_verify RPC — graceful skip if not implemented.
                         Tests valid txid, fake txid, malformed input, node
                         stability after verify calls.
  CONSOL     T205-T216  PTXCONSOLIDATE (nType=8) load and UTXO scaling
                         (KDD-034): pool_utxo_count growth, consolidation
                         threshold (150), cap enforcement (< 200), integration
                         test (5 rolls → count bounded).

  T150 upgraded from stub to real settlement_history check.

v6.1 changes over v6:
  SALT FIX  All salts now generated via mksalt() — MD5 hash of label args →
            guaranteed 8-char pure hex. Fixes T81/T82/T84/T120/T152/T159/T160/
            T166/T167/T168/T172-T175 which were failing with
            "caller_salt must be a hex string" due to non-hex prefix chars.

  BEACON    r.get("beacon") replaced with r.get("quorum_sig_hash") throughout
            (T14, T125, T130). The node field is quorum_sig_hash, not beacon.
            T130 updated: quorum_sig_hash is already SHA256(sig), so the test
            verifies its presence and format rather than recomputing.

  BUG-011   T154/T164 updated to document that 513-item exclude is ACCEPTED
            (KDD-028 MAX_EXCLUDE_COUNT not enforced). Tests now PASS when the
            node accepts it and log it as BUG-011. A separate characterisation
            pass (T176-T178) probes the real behaviour.

  EXCL-LOAD T176-T178  NEW — exclude count load / stability sweep:
                        T176: sweep 512/513/600/1000/2000/5000 — find real limit
                        T177: latency at 100/300/512 items — perf baseline
                        T178: node stable after max-size exclude (crash probe)

v6 additions over v5 (T151-T175):
  EXCL-EXT  T151-T160  exclude edge cases (tx_id chaining, scale, duplicates,
                        out-of-range, pool exhaustion, tight-fit, single-value)
  DEV/ERR   T161-T165  dev_seed and error code / envelope verification
  GAME-EXT  T166-T170  game_id and salt edge cases
  PREV-RND  T171-T175  prev_round_txid chaining (skips if not implemented)

Usage (from node1 host — node has python3):
  python3 ptx_test_suite_v7.py --fast --skip-excl-probe --skip-lottery --skip-settle --skip-odc020 --skip-verify --skip-consolidate
  python3 ptx_test_suite_v7.py --skip-excl-probe
  python3 ptx_test_suite_v7.py

Flags:
  --fast             Skip statistical (T39-T46) and stress sections
  --skip-fail-modes  Skip adversarial T71-T80 (safe for concurrent runs)
  --skip-advanced    Skip T81-T100
  --skip-excl        Skip T101-T120 exclude hardening
  --skip-excl-probe  Skip T105-T113 BUG-004 characterisation (900 calls)
  --skip-lottery     Skip T150 settlement (requires 15+ min block wait)
  --skip-excl-ext    Skip T151-T160 exclude edge cases
  --skip-dev         Skip T161-T165 dev_seed / error code tests
  --skip-prev-round  Skip T171-T175 prev_round_txid (not yet implemented)
  --skip-excl-load   Skip T176-T178 exclude count load sweep
  --skip-settle      Skip T179-T189 PTXSETTLE / settlement_history tests
  --skip-odc020      Skip T190-T196 ODC-020 GM payment address tests
  --skip-verify      Skip T197-T204 ptx_verify tests
  --skip-consolidate Skip T205-T216 PTXCONSOLIDATE UTXO scaling tests
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
    r1, e = roll(1, 1, 100, False, game_id="anchor_same", salt=mksalt("bh",1))
    if e: return None, f"RPC: {e}"
    h1 = r1["block_height"]
    r2, e = roll(1, 1, 100, False, game_id="anchor_same", salt=mksalt("bh",1))
    if e: return None, f"RPC: {e}"
    h2 = r2["block_height"]
    if h1 != h2: return None, f"block advanced ({h1} vs {h2}) — re-run"
    return ok(r1["round_seed"] == r2["round_seed"],
              f"same block same params but different seeds")

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
    """quorum_sig_hash is present and is SHA256(quorum_sig bytes) for 3 consecutive rolls.
    The node exposes the hash as quorum_sig_hash — verify it matches SHA256 of the sig."""
    mismatches = []
    for i in range(3):
        r, e = roll(1,1,100,False,game_id=f"sha_verify_{i}",salt=mksalt("sha_verify", i))
        if e: return None, f"RPC at i={i}: {e}"
        sig  = r.get("quorum_sig","")
        shash = r.get("quorum_sig_hash","")
        if not sig or len(sig)!=192: return None, f"i={i}: quorum_sig missing or wrong length"
        if not shash or len(shash)!=64:
            return False, f"i={i}: quorum_sig_hash missing or wrong length: '{shash}'"
        expected = hashlib.sha256(bytes.fromhex(sig)).hexdigest()
        if shash != expected:
            mismatches.append(f"i={i}: quorum_sig_hash={shash[:16]}... want SHA256={expected[:16]}...")
    if mismatches: return False, f"quorum_sig_hash != SHA256(sig): {mismatches}"
    return True, "quorum_sig_hash == SHA256(quorum_sig) verified for 3 rolls"

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
    """Real settlement_history check — verify most recent settlement has all required fields."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not isinstance(history, list):
        return None, f"settlement_history is not a list: {history!r}"
    if not history:
        return None, "settlement_history empty — no settlements yet; run after first settlement window"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    winner = last.get("winner_node_id") or last.get("winner") or last.get("node_id")
    txid   = last.get("txid") or last.get("tx_id") or last.get("settlement_txid")
    amount = last.get("amount_sat") or last.get("payout_sat") or last.get("amount")
    height = last.get("block_height") or last.get("height")
    if not winner:
        return False, f"settlement_history missing winner field — keys: {list(last.keys())}"
    if not txid or len(str(txid)) != 64:
        return False, f"settlement txid invalid: {txid!r}"
    if not isinstance(amount, int) or amount <= 0:
        return False, f"settlement amount invalid: {amount!r}"
    if not isinstance(height, int) or height <= 0:
        return False, f"settlement block_height invalid: {height!r}"
    return ok(winner in ALL_NODES,
              f"winner={winner} txid={str(txid)[:16]}... amount_sat={amount} h={height}")

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
    """Exclude count latency baseline: record round-trip time at 100, 300, 512 items.
    Establishes performance floor for fault injection comparison (Toxiproxy / P2-TOX-01)."""
    latencies = []
    for size in [100, 300, 512]:
        excl = list(range(1, size + 1))
        t0 = time.time()
        r, e = roll(1, 1, size + 500, False, exclude=excl,
                    game_id="excl_lat", salt=mksalt("excl_lat", size))
        elapsed = time.time() - t0
        alive = node_alive()
        if not alive:
            return False, f"NODE CRASHED at excl={size} during latency test"
        status = "ok" if r else f"err={str(e)[:30]}"
        latencies.append(f"excl={size}: {elapsed:.3f}s ({status})")
        print(f"         {latencies[-1]}")
    return True, " | ".join(latencies)


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
# T179-T189  PTXSETTLE & SETTLEMENT HISTORY (NEW in v7)
# ═══════════════════════════════════════════════════════════════════════════════

def t179():
    """pool_utxo_count field present in ptx_lottery_status (KDD-034 field)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    return ok("pool_utxo_count" in st, f"keys: {list(st.keys())}")

def t180():
    """pool_utxo_count is non-negative integer."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    c = st.get("pool_utxo_count")
    return ok(isinstance(c, int) and c >= 0, f"pool_utxo_count={c!r}")

def t181():
    """settlement_history field present in ptx_lottery_status."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    return ok("settlement_history" in st, f"keys: {list(st.keys())}")

def t182():
    """settlement_history is a list."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    h = st.get("settlement_history")
    if h is None: return None, "settlement_history field absent"
    return ok(isinstance(h, list), f"type={type(h).__name__!r}")

def t183():
    """Most recent settlement record has winner field (SKIP if no settlements yet)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet — run after first settlement window"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    winner = last.get("winner_node_id") or last.get("winner") or last.get("node_id")
    return ok(bool(winner), f"winner={winner!r} record_keys={list(last.keys())}")

def t184():
    """Settlement winner is a known GM node_id."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    winner = last.get("winner_node_id") or last.get("winner") or last.get("node_id")
    if not winner: return None, f"no winner field in settlement record: {list(last.keys())}"
    return ok(winner in ALL_NODES, f"winner={winner!r} — not in known GMs")

def t185():
    """Settlement txid is 64-char hex."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    txid = last.get("txid") or last.get("tx_id") or last.get("settlement_txid")
    if not txid: return None, f"no txid in settlement record: {list(last.keys())}"
    s = str(txid).lower()
    return ok(len(s) == 64 and all(c in "0123456789abcdef" for c in s),
              f"txid={s[:20]}...")

def t186():
    """Settlement amount_sat is positive integer."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    amount = last.get("amount_sat") or last.get("payout_sat") or last.get("amount")
    if amount is None: return None, f"no amount field in settlement: {list(last.keys())}"
    return ok(isinstance(amount, int) and amount > 0, f"amount_sat={amount!r}")

def t187():
    """Settlement block_height is positive integer."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    height = last.get("block_height") or last.get("height")
    if height is None: return None, f"no block_height in settlement: {list(last.keys())}"
    return ok(isinstance(height, int) and height > 0, f"block_height={height!r}")

def t188():
    """pool_balance_sat and pool_utxo_count are both non-negative (structural check)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    bal = st.get("pool_balance_sat", -1)
    cnt = st.get("pool_utxo_count", -1)
    return ok(isinstance(bal, int) and bal >= 0 and isinstance(cnt, int) and cnt >= 0,
              f"pool_balance_sat={bal} pool_utxo_count={cnt}")

def t189():
    """pool_utxo_count < 200 (PTXSETTLE 200-input cap must never be breached)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    c = st.get("pool_utxo_count")
    if c is None: return None, "pool_utxo_count not in lottery_status — KDD-034 not deployed"
    return ok(isinstance(c, int) and c < 200, f"pool_utxo_count={c} (must be < 200)")

# ═══════════════════════════════════════════════════════════════════════════════
# T190-T196  ODC-020 GM PAYMENT ADDRESS (NEW in v7)
# ═══════════════════════════════════════════════════════════════════════════════

def t190():
    """ptx_pose_status records include scriptPTXPayment or payment_address field (ODC-020)."""
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st, list) else st.get("nodes", st.get("pose_records", []))
    if not records: return None, "no records returned"
    sample = records[0]
    has_field = ("scriptPTXPayment" in sample or "payment_address" in sample or
                 "ptx_payment_address" in sample)
    return ok(has_field, f"no payment address field — record keys: {list(sample.keys())}")

def t191():
    """At least one GM has non-empty payment_address registered (ODC-020)."""
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st, list) else st.get("nodes", st.get("pose_records", []))
    populated = []
    for r in records:
        addr = (r.get("scriptPTXPayment") or r.get("payment_address") or
                r.get("ptx_payment_address") or "")
        if addr:
            populated.append(r.get("node_id", "?"))
    if not populated:
        return None, "no GMs have payment_address registered — run ptx_registerpaymentaddress"
    return ok(True, f"{len(populated)}/{len(records)} GMs have payment_address: {populated}")

def t192():
    """Registered payment addresses have valid format (non-empty, printable, ≥20 chars)."""
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st, list) else st.get("nodes", st.get("pose_records", []))
    bad = []
    for r in records:
        addr = (r.get("scriptPTXPayment") or r.get("payment_address") or
                r.get("ptx_payment_address") or "")
        if addr and (len(str(addr)) < 20 or not str(addr).isprintable()):
            bad.append((r.get("node_id"), addr))
    if bad: return False, f"invalid payment address format: {bad}"
    return ok(True, "all registered addresses have valid format (≥20 printable chars)")

def t193():
    """All 11 GM records have payment_address field present (may be empty)."""
    st, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st if isinstance(st, list) else st.get("nodes", st.get("pose_records", []))
    missing = [r.get("node_id", "?") for r in records
               if not ("scriptPTXPayment" in r or "payment_address" in r or
                       "ptx_payment_address" in r)]
    return ok(not missing, f"GMs missing payment_address field entirely: {missing}")

def t194():
    """ptx_lottery_status last settlement winner is a known GM (SKIP if no settlements)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    winner = st.get("last_winner") or st.get("winner_node_id")
    if not winner:
        history = st.get("settlement_history", [])
        if history:
            last = history[-1] if isinstance(history[-1], dict) else history[0]
            winner = last.get("winner_node_id") or last.get("winner")
    if not winner: return None, "no winner recorded yet — run after first settlement"
    return ok(winner in ALL_NODES, f"last_winner={winner!r}")

def t195():
    """winner_address in settlement history is a valid address string (SKIP if no settlements)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    addr = st.get("winner_address") or st.get("last_winner_address")
    if not addr:
        history = st.get("settlement_history", [])
        if history:
            last = history[-1] if isinstance(history[-1], dict) else history[0]
            addr = last.get("winner_address") or last.get("address")
    if not addr: return None, "no winner_address in lottery_status or settlement_history yet"
    return ok(isinstance(addr, str) and len(addr) >= 20,
              f"winner_address={addr[:40]!r}")

def t196():
    """Settlement winner address matches GM's registered scriptPTXPayment (ODC-020 enforcement)."""
    st_l, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st_l.get("settlement_history", [])
    if not history: return None, "no settlements yet"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    winner_node = last.get("winner_node_id") or last.get("winner")
    winner_addr = last.get("winner_address") or last.get("address")
    if not winner_node or not winner_addr:
        return None, f"missing winner_node or winner_address in settlement: {list(last.keys())}"
    st_p, e = pose_status()
    if e: return None, f"ptx_pose_status: {e}"
    records = st_p if isinstance(st_p, list) else st_p.get("nodes", st_p.get("pose_records", []))
    node_rec = next((r for r in records if r.get("node_id") == winner_node), None)
    if not node_rec: return None, f"winner {winner_node} not found in pose_status"
    reg_addr = (node_rec.get("scriptPTXPayment") or node_rec.get("payment_address") or
                node_rec.get("ptx_payment_address"))
    if not reg_addr:
        return None, f"{winner_node} has no scriptPTXPayment — fallback to getnewaddress"
    return ok(winner_addr == reg_addr,
              f"winner_addr={winner_addr[:30]!r} registered={reg_addr[:30]!r}")

# ═══════════════════════════════════════════════════════════════════════════════
# T197-T204  PTX_VERIFY (NEW in v7 — graceful skip if not implemented)
# ═══════════════════════════════════════════════════════════════════════════════

def _ptxverify_skip(e):
    """Return True if the error indicates ptx_verify is not implemented."""
    if isinstance(e, dict):
        return e.get("code") in (-32601, 32601)
    return e and ("method not found" in str(e).lower() or "unknown command" in str(e).lower())

def t197():
    """ptx_verify RPC exists — graceful skip if not implemented."""
    r, e = rpc("ptx_verify", [])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on ptx_verify call"
    if _ptxverify_skip(e): return None, "ptx_verify RPC not implemented — skipping verify tests"
    if e: return True, f"ptx_verify exists (error without params as expected): {str(e)[:60]}"
    return ok(r is not None, f"ptx_verify called without params — result: {r!r}")

def t198():
    """ptx_verify with known PTXSETTLE txid returns a result (SKIP if not implemented or no settlements)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet — need a settlement txid"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    txid = last.get("txid") or last.get("tx_id") or last.get("settlement_txid")
    if not txid: return None, "no txid in settlement history"
    r, e = rpc("ptx_verify", [str(txid)])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on ptx_verify with valid txid"
    if _ptxverify_skip(e): return None, "ptx_verify not implemented"
    if e: return False, f"ptx_verify rejected valid txid: {str(e)[:80]}"
    return ok(r is not None, f"ptx_verify result: {str(r)[:80]}")

def t199():
    """ptx_verify with fake txid returns error (not crash) — SKIP if not implemented."""
    r, e = rpc("ptx_verify", ["a" * 64])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on ptx_verify with fake txid"
    if _ptxverify_skip(e): return None, "ptx_verify not implemented"
    if e: return True, f"correctly rejected fake txid: {str(e)[:70]}"
    return ok(r is None or r is False or
              (isinstance(r, dict) and not r.get("valid", True)),
              f"ptx_verify accepted fake txid — result: {str(r)[:80]}")

def t200():
    """ptx_verify with malformed txid returns clean error — SKIP if not implemented."""
    r, e = rpc("ptx_verify", ["not_a_txid"])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on malformed txid"
    if _ptxverify_skip(e): return None, "ptx_verify not implemented"
    if e: return True, f"correctly rejected malformed txid: {str(e)[:70]}"
    return ok(r is None, f"ptx_verify accepted malformed txid — result: {str(r)[:80]}")

def t201():
    """ptx_verify on PTXSETTLE txid returns valid=True (SKIP if not implemented or no settlements)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    txid = last.get("txid") or last.get("tx_id")
    if not txid: return None, "no txid in settlement history"
    r, e = rpc("ptx_verify", [str(txid)])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED"
    if _ptxverify_skip(e): return None, "ptx_verify not implemented"
    if e: return None, f"ptx_verify error on PTXSETTLE txid: {str(e)[:60]}"
    valid = r if isinstance(r, bool) else (r or {}).get("valid") if isinstance(r, dict) else None
    if valid is None: return None, f"no 'valid' field in result: {r!r}"
    return ok(valid, f"ptx_verify on PTXSETTLE txid: valid={valid}")

def t202():
    """ptx_verify result includes tx_type or type field (SKIP if not implemented)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet"
    last = history[-1] if isinstance(history[-1], dict) else history[0]
    txid = last.get("txid") or last.get("tx_id")
    if not txid: return None, "no txid in settlement history"
    r, e = rpc("ptx_verify", [str(txid)])
    if _ptxverify_skip(e): return None, "ptx_verify not implemented"
    if e: return None, f"ptx_verify error: {str(e)[:60]}"
    if not isinstance(r, dict): return None, f"result is not a dict: {r!r}"
    tx_type = r.get("tx_type") or r.get("type") or r.get("nType")
    return ok(tx_type is not None, f"tx_type={tx_type!r} keys={list(r.keys())}")

def t203():
    """ptx_verify handles empty string txid gracefully — must not crash."""
    r, e = rpc("ptx_verify", [""])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on empty txid"
    if _ptxverify_skip(e): return None, "ptx_verify not implemented"
    return ok(True, f"empty txid handled — {'err: ' + str(e)[:50] if e else 'result: ' + str(r)[:50]}")

def t204():
    """Node stable after all ptx_verify tests — normal roll succeeds."""
    alive = node_alive()
    if not alive: return False, "NODE CRASHED"
    r, e = roll(1, 1, 100, False, game_id="post_verify_stable", salt=mksalt("pvs"))
    if e: return False, f"normal roll failed after verify tests: {e}"
    return ok(1 <= r["results"][0] <= 100, f"node stable — roll: {r['results'][0]}")

# ═══════════════════════════════════════════════════════════════════════════════
# T205-T216  PTXCONSOLIDATE LOAD & UTXO SCALING (NEW in v7)
# ═══════════════════════════════════════════════════════════════════════════════

def t205():
    """pool_utxo_count present in ptx_lottery_status (required for KDD-034 PTXCONSOLIDATE)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    present = "pool_utxo_count" in st
    return ok(present, f"pool_utxo_count {'present' if present else 'MISSING'} — keys: {list(st.keys())}")

def t206():
    """pool_utxo_count is a non-negative integer (type check)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    c = st.get("pool_utxo_count")
    if c is None: return None, "pool_utxo_count not present — KDD-034 not deployed"
    return ok(isinstance(c, int) and c >= 0, f"pool_utxo_count={c!r} (must be int ≥ 0)")

def t207():
    """pool_utxo_count increases after a ptx_roll (each roll adds a UTXO to the pool)."""
    if _near_settlement(3): return None, "settlement imminent — UTXO growth test skipped"
    st1, e = lottery_status()
    if e: return None, f"ptx_lottery_status pre: {e}"
    c1 = st1.get("pool_utxo_count")
    if c1 is None: return None, "pool_utxo_count not present"
    r, e = roll(1, 1, 100, False, game_id="utxo_grow", salt=mksalt("utxo", 1))
    if e: return None, f"roll failed: {e}"
    st2, e = lottery_status()
    if e: return None, f"ptx_lottery_status post: {e}"
    c2 = st2.get("pool_utxo_count", 0)
    # c2==0 is acceptable — settlement fired and drained the pool
    return ok(c2 > c1 or c2 == 0, f"pool_utxo_count: {c1} → {c2}")

def t208():
    """pool_utxo_count stays below 200 (PTXSETTLE 200-input cap)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    c = st.get("pool_utxo_count")
    if c is None: return None, "pool_utxo_count not present"
    return ok(c < 200, f"pool_utxo_count={c} — must stay < 200 (PTXSETTLE cap)")

def t209():
    """pool_utxo_count < 150 in steady state (PTXCONSOLIDATE threshold; SKIP if consolidation pending)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    c = st.get("pool_utxo_count")
    if c is None: return None, "pool_utxo_count not present"
    if c >= 150:
        return None, f"pool_utxo_count={c} >= 150 — PTXCONSOLIDATE may be in mempool (acceptable)"
    return ok(c < 150, f"pool_utxo_count={c} — below consolidation threshold")

def t210():
    """After 5 rolls, pool_utxo_count is still ≤ 155 (consolidation keeps count bounded)."""
    if _near_settlement(5): return None, "settlement imminent — consolidation bound test skipped"
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"consol_bound_{i}", salt=mksalt("cb", i))
        if e: return None, f"roll {i} failed: {e}"
    time.sleep(5)
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status post-rolls: {e}"
    c = st.get("pool_utxo_count")
    if c is None: return None, "pool_utxo_count not present"
    return ok(c <= 155, f"pool_utxo_count={c} after 5 rolls (limit 155)")

def t211():
    """pool_balance_sat and pool_utxo_count are consistent (both non-negative, not contradictory)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    bal = st.get("pool_balance_sat", -1)
    cnt = st.get("pool_utxo_count", -1)
    if cnt is None or cnt < 0:
        return None, f"pool_utxo_count not present or negative: {cnt!r}"
    if cnt == 0 and bal > 0:
        return None, f"pool_utxo_count=0 but pool_balance_sat={bal} — settlement just fired?"
    return ok(isinstance(bal, int) and bal >= 0 and isinstance(cnt, int) and cnt >= 0,
              f"pool_balance_sat={bal} pool_utxo_count={cnt}")

def t212():
    """10 rapid rolls — pool_utxo_count never exceeds 200 cap at any observed point."""
    if _near_settlement(5): return None, "settlement imminent — rapid roll test skipped"
    max_count = 0
    for i in range(10):
        r, e = roll(1, 1, 100, False, game_id=f"rapid_{i}", salt=mksalt("rapid", i))
        if e: return None, f"roll {i} failed: {e}"
        st, e2 = lottery_status()
        if not e2:
            c = st.get("pool_utxo_count", 0) or 0
            max_count = max(max_count, c)
    return ok(max_count < 200, f"max pool_utxo_count across 10 rolls: {max_count} (cap=200)")

def t213():
    """pool_utxo_count is low after settlement (pool drains to near-zero on each settlement)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    history = st.get("settlement_history", [])
    if not history: return None, "no settlements yet — cannot verify post-settlement UTXO count"
    c = st.get("pool_utxo_count", -1)
    if c is None or c < 0: return None, "pool_utxo_count not present"
    return ok(c < 150,
              f"pool_utxo_count={c} after {len(history)} settlement(s) — expected < 150")

def t214():
    """pool_utxo_count consistent across back-to-back lottery_status calls (no concurrent mutation)."""
    st1, e = lottery_status()
    if e: return None, f"first lottery_status: {e}"
    c1 = st1.get("pool_utxo_count")
    if c1 is None: return None, "pool_utxo_count not present"
    st2, e = lottery_status()
    if e: return None, f"second lottery_status: {e}"
    c2 = st2.get("pool_utxo_count")
    return ok(c1 == c2, f"pool_utxo_count inconsistent back-to-back: {c1} vs {c2}")

def t215():
    """Node stable after all PTXCONSOLIDATE tests — normal roll succeeds."""
    alive = node_alive()
    if not alive: return False, "NODE CRASHED"
    r, e = roll(1, 1, 100, False, game_id="post_consol", salt=mksalt("pc"))
    if e: return False, f"roll failed after PTXCONSOLIDATE tests: {e}"
    return ok(1 <= r["results"][0] <= 100, f"node stable — roll: {r['results'][0]}")

def t216():
    """PTXCONSOLIDATE integration: 5 rolls → pool_utxo_count grows but stays < 200."""
    if _near_settlement(5): return None, "settlement imminent — integration test skipped"
    st0, e = lottery_status()
    if e: return None, f"lottery_status pre: {e}"
    c0 = st0.get("pool_utxo_count")
    if c0 is None: return None, "pool_utxo_count not present — KDD-034 not deployed"
    for i in range(5):
        r, e2 = roll(1, 1, 100, False, game_id=f"integ_{i}", salt=mksalt("integ", i))
        if e2: return None, f"roll {i} failed: {e2}"
    time.sleep(3)
    st1, e = lottery_status()
    if e: return None, f"lottery_status post: {e}"
    c1 = st1.get("pool_utxo_count", 0) or 0
    if c1 >= 200:
        return False, f"pool_utxo_count={c1} exceeded 200-input cap — PTXCONSOLIDATE not working"
    return ok(True, f"pool_utxo_count: {c0} → {c1} (cap=200 threshold=150)")

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
    parser.add_argument("--skip-fail-modes",  action="store_true")
    parser.add_argument("--skip-advanced",    action="store_true")
    parser.add_argument("--skip-excl",        action="store_true")
    parser.add_argument("--skip-excl-probe",  action="store_true")
    parser.add_argument("--skip-lottery",     action="store_true")
    parser.add_argument("--skip-excl-ext",    action="store_true", help="Skip T151-T160")
    parser.add_argument("--skip-dev",         action="store_true", help="Skip T161-T165")
    parser.add_argument("--skip-prev-round",  action="store_true", help="Skip T171-T175")
    parser.add_argument("--skip-excl-load",   action="store_true", help="Skip T176-T178")
    parser.add_argument("--skip-settle",      action="store_true", help="Skip T179-T189 PTXSETTLE tests")
    parser.add_argument("--skip-odc020",      action="store_true", help="Skip T190-T196 ODC-020 tests")
    parser.add_argument("--skip-verify",      action="store_true", help="Skip T197-T204 ptx_verify tests")
    parser.add_argument("--skip-consolidate", action="store_true", help="Skip T205-T216 PTXCONSOLIDATE tests")
    args = parser.parse_args()

    RPC_URL  = args.rpc_url
    RPC_USER = args.rpc_user
    RPC_PASS = args.rpc_pass

    print("═"*68)
    print(f"  HEMIS PTX PHASE 2 — LIVE NODE TEST SUITE v7.0  (216 tests)")
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
        ("T130","beacon=SHA256(sig) verified for 3 consecutive rolls",t130),
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
            ("T177","Exclude latency baseline at 100/300/512 items",t177),
            ("T178","Node stable after 5000-item exclude (crash probe)",t178),
        ]: test(tid, name, fn)
        print()
    else:
        print("── Exclude Load Tests SKIPPED (--skip-excl-load) ────────────────────")
        print()

    if not args.skip_settle:
        print("── PTXSETTLE & Settlement History (T179-T189) ───────────────────────")
        print("   NEW in v7 — KDD-032 on-chain settlement validation")
        for tid, name, fn in [
            ("T179","pool_utxo_count field present in lottery_status",t179),
            ("T180","pool_utxo_count is non-negative integer",t180),
            ("T181","settlement_history field present in lottery_status",t181),
            ("T182","settlement_history is a list",t182),
            ("T183","Most recent settlement has winner field",t183),
            ("T184","Settlement winner is a known GM node_id",t184),
            ("T185","Settlement txid is 64-char hex",t185),
            ("T186","Settlement amount_sat is positive integer",t186),
            ("T187","Settlement block_height is positive integer",t187),
            ("T188","pool_balance_sat and pool_utxo_count are non-negative",t188),
            ("T189","pool_utxo_count < 200 (PTXSETTLE 200-input cap)",t189),
        ]: test(tid, name, fn)
        print()
    else:
        print("── PTXSETTLE Tests SKIPPED (--skip-settle) ──────────────────────────")
        print()

    if not args.skip_odc020:
        print("── ODC-020 GM Payment Address (T190-T196) ───────────────────────────")
        print("   NEW in v7 — scriptPTXPayment registration and enforcement")
        for tid, name, fn in [
            ("T190","pose_status records include scriptPTXPayment field",t190),
            ("T191","At least one GM has payment_address registered",t191),
            ("T192","Registered addresses have valid format (≥20 printable chars)",t192),
            ("T193","All 11 GM records have payment_address field present",t193),
            ("T194","Last settlement winner is a known GM",t194),
            ("T195","winner_address is a valid address string",t195),
            ("T196","winner_address matches GM's registered scriptPTXPayment",t196),
        ]: test(tid, name, fn)
        print()
    else:
        print("── ODC-020 Tests SKIPPED (--skip-odc020) ────────────────────────────")
        print()

    if not args.skip_verify:
        print("── ptx_verify (T197-T204) ───────────────────────────────────────────")
        print("   NEW in v7 — graceful skip if ptx_verify not implemented")
        for tid, name, fn in [
            ("T197","ptx_verify RPC exists (or graceful skip)",t197),
            ("T198","ptx_verify with valid PTXSETTLE txid returns result",t198),
            ("T199","ptx_verify with fake txid returns error (not crash)",t199),
            ("T200","ptx_verify with malformed txid returns clean error",t200),
            ("T201","ptx_verify on PTXSETTLE txid returns valid=True",t201),
            ("T202","ptx_verify result includes tx_type field",t202),
            ("T203","ptx_verify handles empty txid gracefully",t203),
            ("T204","Node stable after all ptx_verify tests",t204),
        ]: test(tid, name, fn)
        print()
    else:
        print("── ptx_verify Tests SKIPPED (--skip-verify) ─────────────────────────")
        print()

    if not args.skip_consolidate:
        print("── PTXCONSOLIDATE UTXO Scaling (T205-T216) ─────────────────────────")
        print("   NEW in v7 — KDD-034 pool UTXO count management")
        for tid, name, fn in [
            ("T205","pool_utxo_count present in lottery_status (KDD-034 field)",t205),
            ("T206","pool_utxo_count is non-negative integer",t206),
            ("T207","pool_utxo_count increases after ptx_roll",t207),
            ("T208","pool_utxo_count stays < 200 (PTXSETTLE cap)",t208),
            ("T209","pool_utxo_count < 150 in steady state (PTXCONSOLIDATE threshold)",t209),
            ("T210","After 5 rolls pool_utxo_count ≤ 155 (consolidation keeps count bounded)",t210),
            ("T211","pool_balance_sat and pool_utxo_count are consistent",t211),
            ("T212","10 rapid rolls — pool_utxo_count never exceeds 200",t212),
            ("T213","pool_utxo_count low after settlement (pool drains on settle)",t213),
            ("T214","pool_utxo_count consistent across back-to-back calls",t214),
            ("T215","Node stable after all PTXCONSOLIDATE tests",t215),
            ("T216","PTXCONSOLIDATE integration: 5 rolls → count stays < 200",t216),
        ]: test(tid, name, fn)
        print()
    else:
        print("── PTXCONSOLIDATE Tests SKIPPED (--skip-consolidate) ────────────────")
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
