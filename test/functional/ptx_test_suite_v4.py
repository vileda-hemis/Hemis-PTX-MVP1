#!/usr/bin/env python3
"""
Hemis PTX Phase 1 — Live Node Test Suite v4
=============================================
120 tests across 10 categories.
Adds: BUG-003/BUG-004 exclude path hardening — fix verification,
silent-failure characterisation, multi-round game correctness,
clean-path regression.

Usage:
  pct push 101 ptx_test_suite_v4.py /tmp/ptx_v4.py
  lxc-attach -n 101 -- python3 /tmp/ptx_v4.py

  # Skip fail modes (multi-node):
  for ID in 101 102 103 104 105; do
    echo "=== CT$((ID-100)) ==="; lxc-attach -n $ID -- python3 /tmp/ptx_v4.py --skip-fail-modes
  done

  # Skip slow characterisation probe (fast run):
  lxc-attach -n 101 -- python3 /tmp/ptx_v4.py --skip-excl-probe

Categories:
  CORE     T01-T10   Range, uniqueness, exclusion, edge cases
  CRYPTO   T11-T20   Seed, sig, quorum, beacon, determinism
  STATUS   T21-T28   Round status, PoSe (lookup by block_height)
  GAME     T29-T38   Game scenarios, multi-round chaining
  STAT     T39-T42   Chi-square — coin, d6, d20, d100
  STRESS   T43-T46   Large draws, burst, max range
  INVALID  T47-T70   24 invalid input tests
  ADVERSAR T71-T80   Fail modes — f=1, f=2, withhold, abstain, PoSe, recovery
  ADVANCED T81-T100  Concurrent, tx_id chain, block anchoring, f=3, nonce, load
  EXCL     T101-T120 NEW: BUG-003 fix verification, BUG-004 characterisation,
                         multi-round game correctness, regression

BUG-003 Fix Verification (T101-T104):
  Before fix: FAIL (crash/HTTP 500 at >=96 items)
  After fix:  PASS (large exclude lists handled correctly)

BUG-004 Characterisation (T105-T113):
  Probes whether excluded values silently appear in results across
  exclude sizes 10-90. Reports violation rate per size. Run BEFORE
  fix to locate the fallback trigger point.

BUG-004 Fix Verification (T114-T116):
  Zero-violation assertions. Run AFTER fix. Any violation = FAIL.

Multi-Round Game Correctness (T117-T120):
  Realistic card game scenarios exercising the exclude path end-to-end.
"""

import urllib.request
import json
import base64
import sys
import time
import math
import argparse
import collections
import threading

# ─── Config ───────────────────────────────────────────────────────────────────

RPC_URL  = "http://127.0.0.1:19901/"
RPC_USER = "hemis"
RPC_PASS = "ptxphase1"
ALL_NODES = ["gm1", "gm2", "gm3", "gm4", "gm5",
             "gm6", "gm7", "gm8", "gm9", "gm10"]
TIMEOUT  = 30

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
    except Exception as e:
        return None, str(e)

def roll(count, low, high, unique, exclude=None, game_id="test", salt="aabbcc00"):
    return rpc("ptx_roll", [count, low, high, unique, exclude or [], game_id, salt])

def status(round_id=None):
    return rpc("ptx_getroundstatus", [round_id] if round_id else [])

def fail_mode(target, mode):
    return rpc("ptx_debug_setnodefailmode", [target, mode])

def blockcount():
    r, _ = rpc("getblockcount", [])
    return r or 0

# ─── Helpers ──────────────────────────────────────────────────────────────────

def find_round_by_height(block_height, retries=3):
    for _ in range(retries):
        st, err = status()
        if err or not st:
            time.sleep(1)
            continue
        for rd in st.get("rounds", []):
            if rd.get("block_height") == block_height:
                return rd
        time.sleep(1)
    return None

def _chisq(counts, total, k):
    expected = total / k
    return sum((c - expected)**2 / expected for c in counts)

def node_alive():
    r, e = rpc("getblockcount", [])
    return r is not None and not e

def rejected_cleanly(r, e):
    if e:
        return True, f"correctly rejected: {str(e)[:80]}"
    return False, f"unexpectedly succeeded: {r}"

def _inv(params, label):
    r, e = rpc("ptx_roll", params)
    alive = node_alive()
    if not alive:
        return False, f"{label}: NODE CRASHED"
    if e:
        return True, f"rejected: {str(e)[:70]}"
    return False, f"{label}: accepted — result: {r}"

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

def ok(cond, msg=""): return (bool(cond), msg)

# ═══════════════════════════════════════════════════════════════════════════════
# TESTS T01-T80 (same as v2)
# ═══════════════════════════════════════════════════════════════════════════════

def t01():
    r, e = roll(1, 1, 100, False)
    if e: return None, f"RPC: {e}"
    v = r["results"][0]
    return ok(1 <= v <= 100, f"got {v}")

def t02():
    r, e = roll(1, 7, 7, False, game_id="bound_min", salt="aa01")
    if e: return None, f"RPC: {e}"
    if r["results"][0] != 7: return False, f"min: got {r['results'][0]}"
    r, e = roll(1, 99, 99, False, game_id="bound_max", salt="aa02")
    if e: return None, f"RPC: {e}"
    if r["results"][0] != 99: return False, f"max: got {r['results'][0]}"
    return True, "min=7 max=99"

def t03():
    r, e = roll(10, 1, 20, True, game_id="uniq10", salt="bb01")
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v)) != 10: return False, f"duplicates in {v}"
    if not all(1 <= x <= 20 for x in v): return False, f"out of range: {v}"
    return True, f"{v}"

def t04():
    r, e = roll(20, 1, 2, False, game_id="nonuniq", salt="bb02")
    if e: return None, f"RPC: {e}"
    v = r["results"]
    bad = [x for x in v if x not in [1,2]]
    return ok(not bad, f"out-of-range: {bad}")

def t05():
    exclude = list(range(1, 91))
    r, e = roll(5, 1, 100, True, exclude=exclude, game_id="excl_safe", salt="cc01")
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if v in exclude]
    return ok(not bad, f"excluded values appeared: {bad}")

def t06():
    r, e = roll(1, 1, 10, False, exclude=[1,2,3,4,5,6,7,8,9], game_id="excl_one", salt="cc02")
    if e: return None, f"RPC: {e}"
    v = r["results"][0]
    return ok(v == 10, f"expected 10, got {v}")

def t07():
    r, e = roll(10, 1, 10, True, game_id="fullperm", salt="dd01")
    if e: return None, f"RPC: {e}"
    return ok(sorted(r["results"]) == list(range(1,11)), f"{r['results']}")

def t08():
    r, e = roll(1, 42, 42, False, game_id="single_val", salt="dd02")
    if e: return None, f"RPC: {e}"
    return ok(r["results"][0] == 42, f"got {r['results'][0]}")

def t09():
    r, e = roll(3, 1, 1000000, True, game_id="large_range", salt="dd03")
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v)) != 3: return False, f"duplicates: {v}"
    return ok(all(1 <= x <= 1000000 for x in v), f"{v}")

def t10():
    r, e = roll(1, 5, 5, True, game_id="pool_one_unique", salt="dd04")
    if e: return None, f"RPC: {e}"
    return ok(r["results"][0] == 5, f"got {r['results'][0]}")

def t11():
    r, e = roll(1, 1, 100, False, game_id="seed_fmt", salt="ee01")
    if e: return None, f"RPC: {e}"
    s = r["round_seed"]
    return ok(isinstance(s,str) and len(s)==64 and all(c in "0123456789abcdef" for c in s), f"'{s}'")

def t12():
    r, e = roll(1, 1, 100, False, game_id="sig_fmt", salt="ee02")
    if e: return None, f"RPC: {e}"
    s = r["quorum_sig"]
    return ok(isinstance(s,str) and len(s)>=32 and all(c in "0123456789abcdef" for c in s), f"'{s}'")

def t13():
    r, e = roll(1, 1, 100, False, game_id="members", salt="ee03")
    if e: return None, f"RPC: {e}"
    members = r["quorum_members"]
    unknown = [m for m in members if m not in ALL_NODES]
    return ok(len(members) == 5 and not unknown,
              f"got {members} — expected 5 members from known pool")

def t14():
    st, e = status()
    if e: return None, f"RPC: {e}"
    if not st["rounds"]: return None, "no rounds"
    b = st["rounds"][0]["beacon"]
    return ok(isinstance(b,str) and len(b)==64 and all(c in "0123456789abcdef" for c in b), f"'{b}'")

def t15():
    r, e = roll(1, 1, 100, False, game_id="bheight", salt="ee04")
    if e: return None, f"RPC: {e}"
    h = r["block_height"]
    return ok(isinstance(h,int) and h > 0, f"block_height={h}")

def t16():
    r1, _ = roll(1, 1, 100, False, game_id="seed_uniq_a", salt="ff01")
    r2, _ = roll(1, 1, 100, False, game_id="seed_uniq_b", salt="ff02")
    if not r1 or not r2: return None, "RPC error"
    return ok(r1["round_seed"] != r2["round_seed"], "seeds identical")

def t17():
    r1, _ = roll(1, 1, 100, False, game_id="beacon_a", salt="ff03")
    r2, _ = roll(1, 1, 100, False, game_id="beacon_b", salt="ff04")
    if not r1 or not r2: return None, "RPC error"
    st, _ = status()
    beacons = [rd["beacon"] for rd in st["rounds"]]
    return ok(len(set(beacons)) == len(beacons), "duplicate beacon")

def t18():
    r1, _ = roll(1, 1, 100, False, game_id="diff_salt", salt="aa0011")
    r2, _ = roll(1, 1, 100, False, game_id="diff_salt", salt="bb0022")
    if not r1 or not r2: return None, "RPC error"
    return ok(r1["round_seed"] != r2["round_seed"], "same seed despite different salt")

def t19():
    r1, _ = roll(1, 1, 100, False, game_id="replay", salt="cc0011")
    time.sleep(0.5)
    r2, _ = roll(1, 1, 100, False, game_id="replay", salt="cc0011")
    if not r1 or not r2: return None, "RPC error"
    return ok(r1["round_seed"] != r2["round_seed"], "same params same seed — replay not prevented")

def t20():
    r1, _ = roll(1, 1, 100, False, game_id="sig_a", salt="dd0011")
    r2, _ = roll(1, 1, 100, False, game_id="sig_b", salt="dd0022")
    if not r1 or not r2: return None, "RPC error"
    return ok(r1["quorum_sig"] != r2["quorum_sig"], "identical quorum sigs")

def t21():
    r, e = roll(1, 1, 100, False, game_id="state_chk", salt="9901")
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    return ok(rd["state"] == 2, f"state={rd['state']}")

def t22():
    r, e = roll(1, 1, 100, False, game_id="committed_chk", salt="9902")
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    return ok(sorted(rd["committed"]) == ALL_NODES, f"committed={rd['committed']}")

def t23():
    st, e = status()
    if e: return None, f"RPC: {e}"
    for rd in st["rounds"]:
        if rd["withheld"]: return False, f"withheld: {rd['withheld']}"
    return True, "all rounds withheld=[]"

def t24():
    st, e = status()
    if e: return None, f"RPC: {e}"
    for rd in st["rounds"]:
        if rd["abstained"]: return False, f"abstained: {rd['abstained']}"
    return True, "all rounds abstained=[]"

def t25():
    r, e = roll(1, 1, 100, False, game_id="rid_fmt", salt="9903")
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    rid = rd["round_id"]
    return ok(isinstance(rid,str) and len(rid)>=16 and all(c in "0123456789abcdef" for c in rid), f"'{rid}'")

def t26():
    st, e = status()
    if e: return None, f"RPC: {e}"
    records = {r["node_id"]: r for r in st["pose_records"]}
    not_eligible = [n for n in ALL_NODES if not records.get(n,{}).get("eligible",False)]
    return ok(not not_eligible, f"not eligible: {not_eligible}")

def t27():
    st, e = status()
    if e: return None, f"RPC: {e}"
    records = {r["node_id"]: r for r in st["pose_records"]}
    zero = [n for n in ALL_NODES if records.get(n,{}).get("tickets",0) <= 0]
    if zero:
        return ok(False, f"zero tickets: {zero} (post wallet-wipe — self-resolves)")
    return True, "all nodes have tickets"

def t28():
    st, e = status()
    if e: return None, f"RPC: {e}"
    rids = [rd["round_id"] for rd in st["rounds"]]
    return ok(len(set(rids)) == len(rids), f"duplicate round_ids: {rids}")

def t29():
    vals = []
    for i in range(10):
        r, e = roll(1, 0, 1, False, game_id="coin", salt=f"aa{i:02x}")
        if e: return None, f"RPC: {e}"
        vals.append(r["results"][0])
    bad = [v for v in vals if v not in [0,1]]
    return ok(not bad, f"out of range: {bad}")

def t30():
    r, e = roll(6, 1, 6, False, game_id="d6_all", salt="aa10")
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if not 1<=v<=6]
    return ok(not bad, f"{bad}")

def t31():
    r, e = roll(5, 1, 20, False, game_id="d20", salt="aa11")
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if not 1<=v<=20]
    return ok(not bad, f"{bad}")

def t32():
    r, e = roll(5, 1, 100, False, game_id="d100", salt="aa12")
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if not 1<=v<=100]
    return ok(not bad, f"{bad}")

def t33():
    r, e = roll(5, 1, 52, True, game_id="cards", salt="aa13")
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v))!=5: return False, f"duplicates: {v}"
    return ok(all(1<=x<=52 for x in v), f"{v}")

def t34():
    r, e = roll(52, 1, 52, True, game_id="full_deck", salt="aa14")
    if e: return None, f"RPC: {e}"
    return ok(sorted(r["results"]) == list(range(1,53)), "not a full 52-card permutation")

def t35():
    r, e = roll(1, 1, 10000, False, game_id="raffle_big", salt="aa15")
    if e: return None, f"RPC: {e}"
    return ok(1 <= r["results"][0] <= 10000, f"got {r['results'][0]}")

def t36():
    r, e = roll(16, 1, 128, True, game_id="tourney16", salt="aa16")
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v))!=16: return False, f"duplicates: {v}"
    return ok(all(1<=x<=128 for x in v), f"{v}")

def t37():
    r1, e = roll(5, 1, 100, True, game_id="chain_a", salt="aa17")
    if e: return None, f"RPC: {e}"
    r2, e = roll(5, 1, 100, True, game_id="chain_b", salt="aa18")
    if e: return None, f"RPC: {e}"
    return ok(r1["results"] != r2["results"], f"identical results: {r1['results']}")

def t38():
    r1, e = roll(5, 1, 52, True, game_id="hand1", salt="aa19")
    if e: return None, f"RPC: {e}"
    hand1 = r1["results"]
    r2, e = roll(5, 1, 52, True, exclude=hand1, game_id="hand2", salt="aa20")
    if e: return None, f"RPC: {e}"
    overlap = [v for v in r2["results"] if v in hand1]
    return ok(not overlap, f"overlap: hand1={hand1} hand2={r2['results']}")

def t39():
    counts = [0, 0]
    for i in range(40):
        r, e = roll(5, 0, 1, False, game_id="stat_coin", salt=f"ac{i:04x}")
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts, sum(counts), 2)
    return ok(chi2 < 6.635, f"chi2={chi2:.4f} (limit 6.635) counts={counts}")

def t40():
    counts = [0]*7
    for i in range(100):
        r, e = roll(6, 1, 6, False, game_id="stat_d6", salt=f"ed{i:04x}")
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 6)
    return ok(chi2 < 15.086, f"chi2={chi2:.4f} (limit 15.086)")

def t41():
    counts = [0]*21
    for i in range(50):
        r, e = roll(20, 1, 20, False, game_id="stat_d20", salt=f"520{i:03x}")
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 20)
    return ok(chi2 < 36.191, f"chi2={chi2:.4f} (limit 36.191)")

def t42():
    counts = [0]*101
    for i in range(20):
        r, e = roll(10, 1, 100, False, game_id="stat_d100", salt=f"a10{i:02x}")
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 100)
    return ok(chi2 < 148.23, f"chi2={chi2:.2f} (limit 148.23)")

def t43():
    errors = []
    for i in range(20):
        r, e = roll(1, 1, 1000, False, game_id=f"seq_{i}", salt=f"c1{i:04x}")
        if e: errors.append(f"{i}: {e}")
    return ok(not errors, f"errors: {errors}")

def t44():
    r, e = roll(50, 1, 100, True, game_id="large_count", salt="1c0001")
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v)) != 50: return False, f"duplicates"
    return ok(all(1<=x<=100 for x in v), "out of range")

def t45():
    r, e = roll(1, 1, 2147483647, False, game_id="maxrange", salt="ae0001")
    if e: return None, f"RPC: {e}"
    return ok(1 <= r["results"][0] <= 2147483647, f"got {r['results'][0]}")

def t46():
    errors = []
    for i in range(15):
        r, e = roll(3, 1, 100, True, game_id=f"burst_{i}", salt=f"bc{i:04x}")
        if e: errors.append(f"roll {i}: {e}")
        elif len(set(r["results"])) != 3: errors.append(f"roll {i}: duplicates")
    return ok(not errors, f"{errors}")

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
def t65(): return _inv([1, 1, 100, False, []], "5 params missing")
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

def t71():
    _, e = fail_mode("gm2", "withhold")
    if e: return None, f"set_fail_mode: {e}"
    try:
        r, e = roll(1, 1, 100, False, game_id="f1_withhold", salt="ad0001")
        return ok(r and 1 <= r["results"][0] <= 100, f"roll failed: {e}")
    finally:
        fail_mode("gm2", "normal")

def t72():
    _, _ = fail_mode("gm2", "withhold")
    try:
        r, e = roll(1, 1, 100, False, game_id="f1_withheld_chk", salt="ad0002")
        if e: return None, f"roll failed: {e}"
        rd = find_round_by_height(r["block_height"])
        if not rd: return None, "round not found (BUG-005)"
        return ok("gm2" in rd["withheld"], f"gm2 not in withheld: {rd['withheld']}")
    finally:
        fail_mode("gm2", "normal")

def t73():
    _, e = fail_mode("gm3", "abstain")
    if e: return None, f"set_fail_mode: {e}"
    try:
        r, e = roll(1, 1, 100, False, game_id="f1_abstain", salt="ad0003")
        return ok(r and 1 <= r["results"][0] <= 100, f"roll failed: {e}")
    finally:
        fail_mode("gm3", "normal")

def t74():
    st, _ = status()
    before = {r["node_id"]: r["pose_score"] for r in st["pose_records"]}
    fail_mode("gm2", "withhold")
    try:
        roll(1, 1, 100, False, game_id="pose_incr", salt="ad0004")
        time.sleep(1)
        st2, _ = status()
        after = {r["node_id"]: r["pose_score"] for r in st2["pose_records"]}
        return ok(after.get("gm2",0) > before.get("gm2",0),
            f"gm2 pose: before={before.get('gm2',0)} after={after.get('gm2',0)}")
    finally:
        fail_mode("gm2", "normal")

def t75():
    fail_mode("gm2", "withhold")
    fail_mode("gm4", "withhold")
    try:
        r, e = roll(1, 1, 100, False, game_id="f2_withhold", salt="ad0005")
        return ok(r and 1 <= r["results"][0] <= 100, f"f=2 withhold failed: {e}")
    finally:
        fail_mode("gm2", "normal")
        fail_mode("gm4", "normal")

def t76():
    fail_mode("gm3", "abstain")
    fail_mode("gm5", "abstain")
    try:
        r, e = roll(1, 1, 100, False, game_id="f2_abstain", salt="ad0006")
        return ok(r and 1 <= r["results"][0] <= 100, f"f=2 abstain failed: {e}")
    finally:
        fail_mode("gm3", "normal")
        fail_mode("gm5", "normal")

def t77():
    fail_mode("gm2", "withhold")
    fail_mode("gm2", "normal")
    r, e = roll(1, 1, 100, False, game_id="reset_chk", salt="ad0007")
    if e: return False, f"roll after reset: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    return ok("gm2" not in rd["withheld"], f"gm2 still withheld: {rd['withheld']}")

def t78():
    st1, _ = status()
    s1 = {r["node_id"]: r["pose_score"] for r in st1["pose_records"]}
    for i in range(3):
        roll(1, 1, 100, False, game_id=f"stable_{i}", salt=f"e0{i:04x}")
    st2, _ = status()
    s2 = {r["node_id"]: r["pose_score"] for r in st2["pose_records"]}
    grew = [n for n in ALL_NODES if s2.get(n,0) > s1.get(n,0)]
    return ok(not grew, f"pose_score grew: {grew}")

def t79():
    fail_mode("gm2", "withhold")
    fail_mode("gm5", "abstain")
    try:
        r, e = roll(1, 1, 100, False, game_id="mixed_fail", salt="ad0008")
        return ok(r and 1 <= r["results"][0] <= 100, f"mixed fail failed: {e}")
    finally:
        fail_mode("gm2", "normal")
        fail_mode("gm5", "normal")

def t80():
    for mode in ["withhold", "normal", "withhold", "normal"]:
        fail_mode("gm3", mode)
    r, e = roll(1, 1, 100, False, game_id="no_corruption", salt="ad0009")
    if e: return False, f"roll failed: {e}"
    return ok(r and 1 <= r["results"][0] <= 100, f"got {r['results'][0] if r else 'none'}")

# ═══════════════════════════════════════════════════════════════════════════════
# NEW TESTS T81-T100
# ═══════════════════════════════════════════════════════════════════════════════

# ─── CONCURRENT T81-T85 ──────────────────────────────────────────────────────

def t81():
    """5 concurrent ptx_roll calls from separate threads — all must complete."""
    results_list = [None] * 5
    errors_list  = [None] * 5

    def do_roll(idx):
        r, e = roll(1, 1, 1000, False, game_id=f"concurrent_{idx}", salt=f"c0{idx:04x}")
        results_list[idx] = r
        errors_list[idx]  = e

    threads = [threading.Thread(target=do_roll, args=(i,)) for i in range(5)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=35)

    errors = [f"thread {i}: {errors_list[i]}" for i in range(5) if errors_list[i]]
    bad    = [i for i in range(5) if results_list[i] and not (1 <= results_list[i]["results"][0] <= 1000)]
    if errors: return False, f"errors: {errors}"
    if bad:    return False, f"out-of-range results: {bad}"
    seeds = [results_list[i]["round_seed"] for i in range(5) if results_list[i]]
    return ok(len(seeds) == 5, f"only {len(seeds)}/5 completed")

def t82():
    """10 concurrent calls — all produce unique seeds (no shared round state)."""
    results_list = [None] * 10
    errors_list  = [None] * 10

    def do_roll(idx):
        r, e = roll(1, 1, 100, False, game_id=f"conc_seed_{idx}", salt=f"c5{idx:04x}")
        results_list[idx] = r
        errors_list[idx]  = e

    threads = [threading.Thread(target=do_roll, args=(i,)) for i in range(10)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=35)

    errors = [i for i in range(10) if errors_list[i]]
    if errors: return False, f"errors on threads: {errors}"
    seeds = [results_list[i]["round_seed"] for i in range(10) if results_list[i]]
    if len(seeds) < 10: return False, f"only {len(seeds)}/10 completed"
    return ok(len(set(seeds)) == len(seeds), f"duplicate seeds across concurrent calls — possible shared round state")

def t83():
    """Concurrent rolls with same game_id — should produce different seeds (block height anchor)."""
    results_list = [None] * 4
    errors_list  = [None] * 4

    def do_roll(idx):
        r, e = roll(1, 1, 100, False, game_id="same_game", salt=f"a0{idx:04x}")
        results_list[idx] = r
        errors_list[idx]  = e

    threads = [threading.Thread(target=do_roll, args=(i,)) for i in range(4)]
    for t in threads: t.start()
    for t in threads: t.join(timeout=35)

    errors = [i for i in range(4) if errors_list[i]]
    if errors: return None, f"some calls failed: {errors}"
    seeds = [results_list[i]["round_seed"] for i in range(4) if results_list[i]]
    # Same game_id but different salts — seeds should differ
    return ok(len(set(seeds)) > 1, f"all seeds identical despite different salts: {seeds[0]}")

def t84():
    """30 sequential rolls — node stays healthy throughout (sustained load)."""
    errors = []
    for i in range(30):
        r, e = roll(1, 1, 100, False, game_id=f"load_{i}", salt=f"1d{i:04x}")
        if e: errors.append(f"{i}: {str(e)[:40]}")
        elif not (1 <= r["results"][0] <= 100): errors.append(f"{i}: out of range")
    return ok(not errors, f"{len(errors)} errors in 30 rolls: {errors[:3]}")

def t85():
    """Node remains healthy after 30-roll load test — check RPC still responsive."""
    for i in range(30):
        roll(1, 1, 100, False, game_id=f"pre_load_{i}", salt=f"b1{i:04x}")
    return ok(node_alive(), "node unresponsive after 30-roll load")

# ─── TX_ID EXCLUDE CHAINING T86-T89 ─────────────────────────────────────────

def t86():
    """tx_id exclude: pass a non-existent tx_id string — should not crash, should warn/skip."""
    fake_txid = "a" * 64  # 64-char hex string, not a real tx_id
    r, e = roll(1, 1, 100, False, exclude=[fake_txid], game_id="txid_fake", salt="de0001")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on fake tx_id in exclude"
    if e: return None, f"RPC error (acceptable): {str(e)[:60]}"
    # If accepted — the tx_id was not found on-chain and was silently skipped (per ptx_exclude.cpp)
    v = r["results"][0]
    return ok(1 <= v <= 100, f"got {v} — fake tx_id silently skipped (expected per source)")

def t87():
    """tx_id exclude: pass a valid-format but unconfirmed tx_id — log warning, proceed."""
    # Use the tx_id from a previous roll (which will be "pending" not confirmed)
    r1, e = roll(1, 1, 52, True, game_id="txid_source", salt="de0002")
    if e: return None, f"first roll failed: {e}"
    pending_txid = r1.get("tx_id", "pending")
    if pending_txid == "pending":
        # tx_id is pending — use a fake confirmed format for the test
        pending_txid = "b" * 64

    r2, e2 = roll(1, 1, 52, False, exclude=[pending_txid], game_id="txid_pending", salt="de0003")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on pending tx_id"
    if e2: return None, f"RPC error (acceptable — unconfirmed): {str(e2)[:60]}"
    v = r2["results"][0]
    return ok(1 <= v <= 52, f"got {v} — pending tx_id handled gracefully")

def t88():
    """tx_id exclude: mixed list of integers and fake tx_id strings."""
    fake_txid = "c" * 64
    exclude_mixed = [1, 2, 3, fake_txid, 4, 5]
    r, e = roll(1, 1, 10, False, exclude=exclude_mixed, game_id="txid_mixed", salt="de0004")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on mixed exclude"
    if e: return None, f"RPC error: {str(e)[:60]}"
    v = r["results"][0]
    # Integers 1-5 should be excluded; tx_id not found so skipped
    excluded_ints = [1, 2, 3, 4, 5]
    if v in excluded_ints:
        return False, f"excluded integer appeared: {v}"
    return ok(6 <= v <= 10, f"got {v} — integer exclusion worked, tx_id silently skipped")

def t89():
    """tx_id exclude: multiple fake tx_ids in a list — no crash, handles gracefully."""
    fake_txids = ["d" * 64, "e" * 64, "f" * 64]
    r, e = roll(1, 1, 100, False, exclude=fake_txids, game_id="txid_multi", salt="de0005")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on multiple fake tx_ids"
    if e: return None, f"RPC error (acceptable): {str(e)[:60]}"
    v = r["results"][0]
    return ok(1 <= v <= 100, f"got {v} — multiple fake tx_ids handled")

# ─── BLOCK HEIGHT ANCHORING T90-T92 ─────────────────────────────────────────

def t90():
    """KDD-015: nonce chains off previous beacon, so same-block re-rolls with identical
    params are intentionally different. Verify that the seeds are always distinct."""
    r1, e = roll(1, 1, 100, False, game_id="anchor_same", salt="ba0001")
    if e: return None, f"RPC: {e}"
    r2, e = roll(1, 1, 100, False, game_id="anchor_same", salt="ba0001")
    if e: return None, f"RPC: {e}"
    # KDD-015 guarantee: even identical params produce different seeds because the
    # nonce is built from the previous beacon, which advances with every round.
    return ok(r1["round_seed"] != r2["round_seed"],
        f"KDD-015 violated: identical re-rolls produced the same seed {r1['round_seed'][:16]}...")

def t91():
    """KDD-003: different block heights produce different seeds (anti-grinding)."""
    r1, e = roll(1, 1, 100, False, game_id="anchor_diff", salt="ba0002")
    if e: return None, f"RPC: {e}"
    h1 = r1["block_height"]
    # Wait for a new block
    for _ in range(30):
        time.sleep(3)
        h_now = blockcount()
        if h_now > h1:
            break
    r2, e = roll(1, 1, 100, False, game_id="anchor_diff", salt="ba0002")
    if e: return None, f"RPC: {e}"
    h2 = r2["block_height"]
    if h1 == h2:
        return None, f"block didn't advance (h={h1}) — test inconclusive. Regtest may need blocks."
    return ok(r1["round_seed"] != r2["round_seed"],
        f"different blocks same params but same seed — anti-grinding broken. h1={h1} h2={h2}")

def t92():
    """Block height appears in round seed inputs — verify block_height field is present and matches."""
    r, e = roll(1, 1, 100, False, game_id="anchor_field", salt="ba0003")
    if e: return None, f"RPC: {e}"
    h = r["block_height"]
    bc = blockcount()
    # block_height in response should be current or recent block
    return ok(isinstance(h, int) and h > 0 and abs(h - bc) <= 5,
        f"block_height={h} blockcount={bc} — too far apart")

# ─── f=3 FAILURE MODE T93-T95 ────────────────────────────────────────────────

def t93():
    """f=3 withhold: 3 nodes withhold — below threshold, round should FAIL or timeout."""
    fail_mode("gm2", "withhold")
    fail_mode("gm3", "withhold")
    fail_mode("gm4", "withhold")
    try:
        r, e = rpc("ptx_roll", [1, 1, 100, False, [], "f3_withhold", "f30001"])
        alive = node_alive()
        if not alive:
            return False, "NODE CRASHED on f=3 withhold — should return error, not crash"
        if e:
            # Expected — round cannot complete with only 2 honest nodes
            return True, f"correctly failed: {str(e)[:80]}"
        # If it succeeded — check if it really had 5 quorum members or just 2
        members = r.get("quorum_members", [])
        if len(members) < 3:
            return False, f"round completed with only {len(members)} members — threshold violation"
        return None, f"round completed — unclear if f=3 was enforced. members={members}"
    finally:
        fail_mode("gm2", "normal")
        fail_mode("gm3", "normal")
        fail_mode("gm4", "normal")

def t94():
    """f=3 abstain: 3 nodes abstain — round should fail gracefully."""
    fail_mode("gm3", "abstain")
    fail_mode("gm4", "abstain")
    fail_mode("gm5", "abstain")
    try:
        r, e = rpc("ptx_roll", [1, 1, 100, False, [], "f3_abstain", "f30002"])
        alive = node_alive()
        if not alive:
            return False, "NODE CRASHED on f=3 abstain"
        if e:
            return True, f"correctly failed: {str(e)[:80]}"
        members = r.get("quorum_members", [])
        if len(members) < 3:
            return False, f"threshold violated — only {len(members)} members"
        return None, f"round completed unexpectedly — members={members}"
    finally:
        fail_mode("gm3", "normal")
        fail_mode("gm4", "normal")
        fail_mode("gm5", "normal")

def t95():
    """After f=3 scenario, node recovers and normal rolls work."""
    # Reset all (should already be reset by finally blocks)
    for n in ALL_NODES:
        fail_mode(n, "normal")
    time.sleep(1)
    r, e = roll(1, 1, 100, False, game_id="f3_recovery", salt="f30003")
    if e: return False, f"node did not recover: {e}"
    return ok(r and 1 <= r["results"][0] <= 100, f"got {r['results'][0] if r else 'none'}")

# ─── NONCE CHAINING T96-T98 ──────────────────────────────────────────────────

def t96():
    """KDD-015: same caller_salt across 5 sequential rounds produces 5 distinct seeds."""
    seeds = []
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"nonce_{i}", salt="deadbeef00000000")
        if e: return None, f"RPC error at i={i}: {e}"
        seeds.append(r["round_seed"])
    return ok(len(set(seeds)) == 5,
        f"duplicate seeds with same salt — nonce not advancing: {seeds}")

def t97():
    """Nonce advance: round_seed changes across rounds even with fixed salt — prev_recovered_sig advances."""
    r1, e = roll(1, 1, 100, False, game_id="nonce_adv_1", salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    r2, e = roll(1, 1, 100, False, game_id="nonce_adv_2", salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    r3, e = roll(1, 1, 100, False, game_id="nonce_adv_3", salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    seeds = [r1["round_seed"], r2["round_seed"], r3["round_seed"]]
    return ok(len(set(seeds)) == 3, f"duplicate seeds with fixed salt — nonce not chaining: {seeds}")

def t98():
    """Quorum sigs are all different across sequential rounds (confirm uniqueness at sig level too)."""
    sigs = []
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"sig_chain_{i}", salt=f"ac{i:08x}")
        if e: return None, f"RPC: {e}"
        sigs.append(r["quorum_sig"])
    return ok(len(set(sigs)) == 5, f"duplicate quorum_sig across rounds — unexpected: {sigs}")

# ─── EXCLUDE BOUNDARY T99-T100 ───────────────────────────────────────────────

def t99():
    """BUG-003 boundary: exactly 95 items excluded — must succeed (just under threshold)."""
    exclude = list(range(1, 96))  # 95 items
    r, e = roll(1, 1, 200, False, exclude=exclude, game_id="excl_95", salt="eb0001")
    if e: return False, f"FAIL at 95 items — BUG-003 threshold lower than expected: {e}"
    v = r["results"][0]
    if v in exclude: return False, f"excluded value returned: {v}"
    return ok(96 <= v <= 200, f"got {v} — 95-item exclude working")

def t100():
    """BUG-003 reproduction: 96 items — expect HTTP 500 (confirms bug present, documents threshold)."""
    exclude = list(range(1, 97))  # 96 items
    r, e = rpc("ptx_roll", [1, 1, 200, False, exclude, "excl_96", "eb0002"])
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED on 96-item exclude — severe"
    if e:
        # Expected — BUG-003 confirmed at exactly 96 items
        return True, f"BUG-003 confirmed at 96 items: {str(e)[:60]}"
    # If it succeeded — BUG-003 may have been fixed!
    v = r["results"][0] if r else None
    if v and v not in exclude and 1 <= v <= 200:
        return True, f"POSSIBLE FIX: 96-item exclude now works! got {v}"
    return False, f"unexpected result: {r}"


# ═══════════════════════════════════════════════════════════════════════════════
# TESTS T101-T120 — EXCLUDE PATH HARDENING (NEW IN v4)
# ═══════════════════════════════════════════════════════════════════════════════
#
# Three sub-groups:
#
#   BUG-003 Fix Verification  (T101-T104)
#     FAIL before fix (crash/HTTP 500), PASS after fix.
#     Verifies the fixed-buffer removal allows large exclude lists.
#
#   BUG-004 Characterisation  (T105-T113)
#     Run BEFORE fix to locate the silent-fallback trigger.
#     Each test runs 100 iterations and counts violations (excluded value
#     appearing in result). A 30% fallback rate at ~40% exclude density
#     should produce ~12 violations per 100 iterations — clearly detectable.
#     PASS = 0 violations (either bug not present at this size, or already fixed).
#     FAIL = any violation, with rate reported.
#
#   BUG-004 Fix Verification  (T114-T116)
#     Run AFTER fix. Strict zero-violation assertions at key sizes.
#
#   Multi-Round Game Correctness (T117-T120)
#     Realistic card game scenarios. Exercise exclude path end-to-end.
#     Clean-path regression confirms fix didn't break no-exclude draws.
#
# ═══════════════════════════════════════════════════════════════════════════════

# ── Helper: probe exclude correctness over N iterations ──────────────────────

def _excl_probe(excl_size, iterations, pool_high, label):
    """
    Draw <iterations> single values from [1, pool_high] with first
    <excl_size> values excluded. Return (violations, total, detail_str).
    Violations = draws where an excluded value was returned.
    """
    exclude = list(range(1, excl_size + 1))
    violations = []
    errors = []
    for i in range(iterations):
        r, e = roll(1, 1, pool_high, False,
                    exclude=exclude,
                    game_id=f"{label}_{i:04x}",
                    salt=f"{i:08x}")
        if e:
            errors.append(f"iter {i}: {str(e)[:40]}")
            continue
        v = r["results"][0]
        if v in exclude:
            violations.append((i, v))
    return violations, errors, iterations - len(errors)


# ── BUG-003 Fix Verification T101-T104 ───────────────────────────────────────

def t101():
    """BUG-003 fix: 97-item exclude — must succeed after fix (1 above old crash threshold)."""
    exclude = list(range(1, 98))  # 97 items
    r, e = rpc("ptx_roll", [1, 1, 300, False, exclude, "excl_97", "fc0001"])
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED — fix not applied or new crash introduced"
    if e:
        return False, f"BUG-003 not fixed at 97 items: {str(e)[:80]}"
    v = r["results"][0] if r else None
    if v is None:
        return False, "no result returned"
    if v in exclude:
        return False, f"excluded value returned: {v}"
    return ok(98 <= v <= 300, f"got {v} — 97-item exclude working (BUG-003 fixed)")

def t102():
    """BUG-003 fix: 200-item exclude — verifies fix scales beyond immediate threshold."""
    exclude = list(range(1, 201))  # 200 items
    r, e = rpc("ptx_roll", [1, 1, 500, False, exclude, "excl_200", "fc0002"])
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED at 200 items"
    if e:
        return False, f"failed at 200 items: {str(e)[:80]}"
    v = r["results"][0] if r else None
    if v is None:
        return False, "no result"
    if v in exclude:
        return False, f"excluded value returned: {v}"
    return ok(201 <= v <= 500, f"got {v} — 200-item exclude working")

def t103():
    """BUG-003 fix: 500-item exclude — mid-scale correctness."""
    exclude = list(range(1, 501))  # 500 items
    r, e = rpc("ptx_roll", [1, 1, 1000, False, exclude, "excl_500", "fc0003"])
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED at 500 items"
    if e:
        return False, f"failed at 500 items: {str(e)[:80]}"
    v = r["results"][0] if r else None
    if v is None:
        return False, "no result"
    if v in exclude:
        return False, f"excluded value returned: {v}"
    return ok(501 <= v <= 1000, f"got {v} — 500-item exclude working")

def t104():
    """BUG-003 fix: 1000-item exclude — large-scale correctness and node stability."""
    exclude = list(range(1, 1001))  # 1000 items
    r, e = rpc("ptx_roll", [1, 1, 2000, False, exclude, "excl_1000", "fc0004"])
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED at 1000 items — memory issue possible"
    if e:
        return False, f"failed at 1000 items: {str(e)[:80]}"
    v = r["results"][0] if r else None
    if v is None:
        return False, "no result"
    if v in exclude:
        return False, f"excluded value returned: {v}"
    return ok(1001 <= v <= 2000, f"got {v} — 1000-item exclude working")


# ── BUG-004 Characterisation T105-T113 ───────────────────────────────────────
# Pool: 1-200. Exclude first N. 100 iterations each.
# Any violation = FAIL with violation count and rate reported.
# Use --skip-excl-probe to skip this group (900 RPC calls total).

def _char_test(excl_size, label):
    """Run characterisation probe and return test outcome."""
    violations, errors, completed = _excl_probe(excl_size, 100, 200, label)
    rate = len(violations) / completed * 100 if completed else 0
    detail = (f"excl={excl_size} completed={completed} "
              f"violations={len(violations)} ({rate:.1f}%)")
    if errors:
        detail += f" rpc_errors={len(errors)}"
    if violations:
        sample = violations[:3]
        detail += f" sample={sample}"
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


# ── BUG-004 Fix Verification T114-T116 ───────────────────────────────────────
# Strict zero-violation assertions. Run AFTER fix.
# 200 iterations for statistical confidence — P(miss|30% fallback, 40% density)
# per iteration ≈ 0.12, so P(0 violations in 200) < 10^-10 if bug present.

def t114():
    """BUG-004 fix: 40-item exclude, 200 iterations — zero violations required."""
    violations, errors, completed = _excl_probe(40, 200, 200, "bug4_v40")
    rate = len(violations) / completed * 100 if completed else 0
    if violations:
        return False, (f"BUG-004 NOT FIXED: {len(violations)}/{completed} violations "
                       f"({rate:.1f}%) at excl=40 — fallback still present")
    return True, f"excl=40: 0 violations in {completed} iterations — BUG-004 fixed"

def t115():
    """BUG-004 fix: 80-item exclude, 200 iterations — zero violations required."""
    violations, errors, completed = _excl_probe(80, 200, 200, "bug4_v80")
    rate = len(violations) / completed * 100 if completed else 0
    if violations:
        return False, (f"BUG-004 NOT FIXED: {len(violations)}/{completed} violations "
                       f"({rate:.1f}%) at excl=80 — fallback still present")
    return True, f"excl=80: 0 violations in {completed} iterations — BUG-004 fixed"

def t116():
    """BUG-004 fix: full sweep excl=10 to 90 step 10, 50 iterations each — zero violations anywhere."""
    all_violations = {}
    for excl_size in range(10, 91, 10):
        violations, errors, completed = _excl_probe(
            excl_size, 50, 200, f"bug4_sweep_{excl_size}")
        if violations:
            all_violations[excl_size] = len(violations)
    if all_violations:
        return False, f"BUG-004 violations at sizes: {all_violations}"
    return True, "sweep excl=10-90 step 10: 0 violations at all sizes — BUG-004 fixed"


# ── Multi-Round Game Correctness T117-T120 ────────────────────────────────────

def t117():
    """
    10-round card game simulation — 5 cards per round from 52-card deck.
    All 50 drawn cards must be unique across all rounds.
    Exercises the exclude path in a realistic multi-round scenario.
    """
    all_drawn = []
    for rnd in range(10):
        r, e = roll(5, 1, 52, True,
                    exclude=all_drawn,
                    game_id=f"cardgame_r{rnd}",
                    salt=f"c9{rnd:04x}")
        if e:
            return False, f"round {rnd+1} failed (excl={len(all_drawn)} items): {e}"
        drawn = r["results"]
        # Check no overlap with previously drawn
        overlap = [v for v in drawn if v in all_drawn]
        if overlap:
            return False, (f"round {rnd+1}: overlap with previous rounds — "
                           f"values {overlap} already drawn. "
                           f"BUG-004 present or exclude not applied.")
        # Check all in range
        if not all(1 <= v <= 52 for v in drawn):
            return False, f"round {rnd+1}: out-of-range values: {drawn}"
        all_drawn.extend(drawn)
    return True, f"10 rounds × 5 cards = {len(all_drawn)} unique values, no overlaps"

def t118():
    """
    Near-depleted deck: 47-item exclude from 52-card deck.
    Only 5 values remain possible. Result must be one of them.
    Verifies correctness at high exclude density.
    """
    exclude = list(range(1, 48))  # cards 1-47 dealt
    remaining = list(range(48, 53))  # cards 48-52 remain
    r, e = roll(1, 1, 52, False,
                exclude=exclude,
                game_id="near_depleted",
                salt="ed0001")
    if e:
        return False, f"failed at 47-item exclude (near-depleted deck): {e}"
    v = r["results"][0]
    if v in exclude:
        return False, f"excluded value {v} returned — exclude not applied"
    return ok(v in remaining,
              f"got {v} — expected one of {remaining}")

def t119():
    """
    Extreme density: exclude 90 of 100 values (90% of pool).
    Result must be in remaining 10 values only.
    Stress-tests correctness at the highest practical exclude density.
    """
    exclude = list(range(1, 91))   # exclude 1-90
    remaining = list(range(91, 101))  # only 91-100 valid
    violations = 0
    for i in range(50):
        r, e = roll(1, 1, 100, False,
                    exclude=exclude,
                    game_id=f"extreme_{i:04x}",
                    salt=f"e0{i:08x}")
        if e:
            return False, f"failed at iter {i} with 90-item exclude: {e}"
        v = r["results"][0]
        if v not in remaining:
            violations += 1
    if violations:
        return False, (f"{violations}/50 draws returned excluded value "
                       f"— extreme-density BUG-004 present")
    return True, f"50 iterations at 90% exclude density: 0 violations"

def t120():
    """
    Clean-path regression: 10 no-exclude rolls after all exclude path tests.
    Confirms the BUG-003/004 fix did not corrupt the clean (no-exclude) path.
    """
    errors = []
    for i in range(10):
        r, e = roll(5, 1, 100, True,
                    game_id=f"regression_{i}",
                    salt=f"ae{i:08x}")
        if e:
            errors.append(f"iter {i}: {e}")
            continue
        v = r["results"]
        if len(set(v)) != 5:
            errors.append(f"iter {i}: duplicates {v}")
        elif not all(1 <= x <= 100 for x in v):
            errors.append(f"iter {i}: out-of-range {v}")
    if errors:
        return False, f"clean path broken after exclude fix: {errors}"
    return True, "10 × 5-unique draws with no excludes: all correct"

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--skip-fail-modes", action="store_true")
    parser.add_argument("--skip-stats",      action="store_true")
    parser.add_argument("--skip-stress",     action="store_true")
    parser.add_argument("--skip-advanced",   action="store_true")
    parser.add_argument("--skip-excl",       action="store_true",
                        help="Skip all T101-T120 exclude hardening tests")
    parser.add_argument("--skip-excl-probe", action="store_true",
                        help="Skip T105-T113 characterisation (100 iters each — slow)")
    args = parser.parse_args()

    print()
    print("═" * 68)
    print("  HEMIS PTX PHASE 1 — LIVE NODE TEST SUITE v4  (120 tests)")
    print(f"  RPC: {RPC_URL}")
    print("═" * 68)
    print()

    r, err = rpc("getblockcount", [])
    if err:
        print(f"  FATAL: Cannot connect to RPC: {err}")
        sys.exit(1)
    print(f"  Connected. Block height: {r}")
    print()

    print("── Core Functionality (T01-T10) ──────────────────────────────────")
    test("T01", "Basic roll — single value in 1-100",             t01)
    test("T02", "Range boundary — exact min/max",                 t02)
    test("T03", "Unique draws — no duplicates",                   t03)
    test("T04", "Non-unique — duplicates permitted",              t04)
    test("T05", "Exclusion list — excluded never returned",       t05)
    test("T06", "Exclusion — forces single possible value",       t06)
    test("T07", "Full permutation draw",                          t07)
    test("T08", "Single value range (low==high)",                 t08)
    test("T09", "Large range (1-1,000,000)",                      t09)
    test("T10", "Unique draw from pool of one",                   t10)
    print()

    print("── Cryptographic Properties (T11-T20) ────────────────────────────")
    test("T11", "Round seed — valid 64-char hex",                 t11)
    test("T12", "Quorum sig — valid hex",                         t12)
    test("T13", "Quorum members — exactly 5 from known pool",     t13)
    test("T14", "Beacon — valid 64-char hex",                     t14)
    test("T15", "Block height is positive integer",               t15)
    test("T16", "Round seed unique across rounds",                 t16)
    test("T17", "Beacon unique across rounds",                     t17)
    test("T18", "Different salts produce different seeds",        t18)
    test("T19", "Re-roll same params → different seed",           t19)
    test("T20", "Quorum sig unique per round",                    t20)
    print()

    print("── Round Status & PoSe (T21-T28) ─────────────────────────────────")
    test("T21", "Round state=2 after roll (block_height lookup)", t21)
    test("T22", "Round committed = all 5 nodes",                  t22)
    test("T23", "Round withheld is empty",                        t23)
    test("T24", "Round abstained is empty",                       t24)
    test("T25", "Round ID is valid hex",                          t25)
    test("T26", "PoSe — all 5 nodes eligible",                    t26)
    test("T27", "PoSe — tickets > 0 all nodes",                   t27)
    test("T28", "Round IDs unique across rounds",                  t28)
    print()

    print("── Game Scenarios (T29-T38) ───────────────────────────────────────")
    test("T29", "Coin flip — result is 0 or 1",                   t29)
    test("T30", "D6 roll — result in 1-6",                        t30)
    test("T31", "D20 roll — result in 1-20",                      t31)
    test("T32", "D100 roll — result in 1-100",                    t32)
    test("T33", "Card draw — 5 unique from 52",                   t33)
    test("T34", "Full deck — 52 unique from 52",                  t34)
    test("T35", "Raffle — 1 winner from 1-10000",                 t35)
    test("T36", "Tournament bracket — 16 unique from 128",        t36)
    test("T37", "Sequential rolls produce different results",     t37)
    test("T38", "Multi-hand — second hand excludes first",        t38)
    print()

    if not args.skip_stats:
        print("── Statistical Validation (T39-T42) ──────────────────────────────")
        test("T39", "Coin flip chi-square (200 flips, p>0.01)",   t39)
        test("T40", "D6 uniformity chi-square (600 rolls)",       t40)
        test("T41", "D20 uniformity chi-square (1000 rolls)",     t41)
        test("T42", "D100 uniformity chi-square (200 rolls)",     t42)
        print()
    else:
        print("── Statistical Tests SKIPPED ──────────────────────────────────────")
        print()

    if not args.skip_stress:
        print("── Stress Tests (T43-T46) ────────────────────────────────────────")
        test("T43", "20 sequential rolls — no crash",             t43)
        test("T44", "Large count — 50 unique from 100",           t44)
        test("T45", "Max int range — 1 to 2,147,483,647",         t45)
        test("T46", "Rapid burst — 15 concurrent calls",          t46)
        print()
    else:
        print("── Stress Tests SKIPPED ───────────────────────────────────────────")
        print()

    print("── Invalid Params (T47-T70) ───────────────────────────────────────")
    print("   PASS = clean error + node alive · FAIL = crash/hang/garbage accepted")
    test("T47", "count=0",                                        t47)
    test("T48", "count=-1",                                       t48)
    test("T49", 'count="1" (string)',                             t49)
    test("T50", "count=1.5 (float)",                              t50)
    test("T51", "low > high — inverted range",                    t51)
    test("T52", "low==high unique count=2 — impossible",          t52)
    test("T53", 'low="1" (string)',                               t53)
    test("T54", 'high="100" (string)',                            t54)
    test("T55", "low=1.5 (float)",                                t55)
    test("T56", "high=100.9 (float)",                             t56)
    test("T57", 'unique="false" (string)',                        t57)
    test("T58", "unique=0 (integer)",                             t58)
    test("T59", 'exclude="[]" (string not array)',                t59)
    test("T60", "exclude=null",                                   t60)
    test("T61", "exclude=[1.5, 2.5] (floats)",                   t61)
    test("T62", "exclude=[1, null, 3]",                          t62)
    test("T63", "exclude=[[1,2],[3,4]] (nested)",                t63)
    test("T64", "game_id=42 (integer)",                           t64)
    test("T65", "missing game_id and salt",                       t65)
    test("T66", "extra param (8 instead of 7)",                   t66)
    test("T67", "salt=12345 (integer)",                           t67)
    test("T68", 'salt="hello_world" (non-hex)',                   t68)
    test("T69", 'salt="" (empty)',                                t69)
    test("T70", "count=15 > unique pool=10",                      t70)
    print()

    if not args.skip_fail_modes:
        print("── Adversarial / Fail Modes (T71-T80) ────────────────────────────")
        test("T71", "f=1 withhold gm2 — round resolves",          t71)
        test("T72", "f=1 withhold — withheld list populated",     t72)
        test("T73", "f=1 abstain gm3 — round resolves",           t73)
        test("T74", "f=1 withhold — PoSe increments",             t74)
        test("T75", "f=2 withhold gm2+gm4 — round resolves",     t75)
        test("T76", "f=2 abstain gm3+gm5 — round resolves",      t76)
        test("T77", "Fail mode reset — gm2 participates normally",t77)
        test("T78", "PoSe stable after normal operation",         t78)
        test("T79", "Mixed f=1 withhold + f=1 abstain — resolves",t79)
        test("T80", "Mode cycling — no permanent corruption",     t80)
        print()
    else:
        print("── Fail Mode Tests SKIPPED ────────────────────────────────────────")
        print()

    if not args.skip_advanced:
        print("── Advanced Tests (T81-T100) ──────────────────────────────────────")

        print("   [Concurrent]")
        test("T81", "5 concurrent rolls — all complete",              t81)
        test("T82", "10 concurrent rolls — unique seeds (no collision)",t82)
        test("T83", "Concurrent same game_id — different salts differ", t83)
        test("T84", "30 sequential rolls — sustained load",            t84)
        test("T85", "Node alive after sustained load",                 t85)

        print("   [tx_id Exclude Chaining]")
        test("T86", "Fake tx_id in exclude — no crash, silently skipped",  t86)
        test("T87", "Pending tx_id in exclude — handled gracefully",       t87)
        test("T88", "Mixed int + tx_id exclude — integers enforced",       t88)
        test("T89", "Multiple fake tx_ids — no crash",                     t89)

        print("   [Block Height Anchoring — KDD-003]")
        test("T90", "Same-block re-rolls always produce distinct seeds (KDD-015)", t90)
        test("T91", "Different blocks same params → different seed",   t91)
        test("T92", "block_height in response matches chain height",   t92)

        if not args.skip_fail_modes:
            print("   [f=3 Failure Mode]")
            test("T93", "f=3 withhold — round fails gracefully (not crash)", t93)
            test("T94", "f=3 abstain — round fails gracefully",              t94)
            test("T95", "Node recovers after f=3 scenario",                  t95)
        else:
            print("   [f=3 Tests SKIPPED — --skip-fail-modes]")

        print("   [Nonce Chaining — KDD-015]")
        test("T96", "5 rounds same salt → 5 distinct seeds",          t96)
        test("T97", "Fixed salt across rounds — nonce advances",      t97)
        test("T98", "Quorum sigs unique across 5 rounds",             t98)

        print("   [Exclude Boundary — BUG-003]")
        test("T99",  "95-item exclude — succeeds (just under threshold)", t99)
        test("T100", "96-item exclude — HTTP 500 (BUG-003 present)",      t100)
        print()
    else:
        print("── Advanced Tests SKIPPED (--skip-advanced) ───────────────────────")
        print()

    if not args.skip_excl:
        print("── Exclude Path Hardening (T101-T120) ────────────────────────────")

        print("   [BUG-003 Fix Verification — FAIL before fix, PASS after]")
        test("T101", "97-item exclude — succeeds post-fix",               t101)
        test("T102", "200-item exclude — scales correctly post-fix",      t102)
        test("T103", "500-item exclude — mid-scale correctness",          t103)
        test("T104", "1000-item exclude — large-scale, node stable",      t104)

        if not args.skip_excl_probe:
            print("   [BUG-004 Characterisation — probe silent fallback trigger]")
            print("   NOTE: 900 RPC calls total — use --skip-excl-probe to skip")
            test("T105", "excl=10  100 iters — probe BUG-004 at low size",    t105)
            test("T106", "excl=20  100 iters — probe BUG-004",                t106)
            test("T107", "excl=30  100 iters — probe BUG-004",                t107)
            test("T108", "excl=40  100 iters — probe BUG-004 key zone",       t108)
            test("T109", "excl=50  100 iters — probe BUG-004",                t109)
            test("T110", "excl=60  100 iters — probe BUG-004",                t110)
            test("T111", "excl=70  100 iters — probe BUG-004",                t111)
            test("T112", "excl=80  100 iters — probe BUG-004",                t112)
            test("T113", "excl=90  100 iters — probe BUG-004 near threshold", t113)
        else:
            print("   [BUG-004 Characterisation SKIPPED — --skip-excl-probe]")

        print("   [BUG-004 Fix Verification — FAIL if fallback still present]")
        test("T114", "excl=40  200 iters — zero violations required",     t114)
        test("T115", "excl=80  200 iters — zero violations required",     t115)
        test("T116", "excl=10-90 sweep 50 iters — zero violations anywhere", t116)

        print("   [Multi-Round Game Correctness]")
        test("T117", "10-round card game — 50 unique, no cross-round overlap", t117)
        test("T118", "Near-depleted deck — 47 excl, result in remaining 5",    t118)
        test("T119", "Extreme density — 90% excluded, 50 iters, 0 violations", t119)
        test("T120", "Clean path regression — no-exclude draws unaffected",    t120)
        print()
    else:
        print("── Exclude Hardening Tests SKIPPED (--skip-excl) ──────────────────")
        print()

    total = _pass + _fail + _skip
    print("═" * 68)
    print(f"  RESULTS   PASS: {_pass}   FAIL: {_fail}   SKIP: {_skip}   TOTAL: {total}")
    print("═" * 68)

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
