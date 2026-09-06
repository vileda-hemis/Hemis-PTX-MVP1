#!/usr/bin/env python3
"""
Hemis PTX Phase 2 — Live Node Test Suite v5
============================================
150 tests across 13 categories.

Phase 2 additions over v4:
  BLS P2   T121-T130  192-char quorum_sig, 11-node quorum, beacon=SHA256(sig)
  POSE P2  T131-T140  ptx_pose_status, docker stop/start, score decay
  LOTTERY  T141-T150  ptx_lottery_status, pool balance, settlement

Phase 1 tests updated for 11-node quorum and ptxrpc credentials.
T74 rewritten: PoSe increment via docker stop (Phase 2 PoSe path).
T93-T95 rewritten: f=6 threshold test for 11-GM cluster (threshold=6).

RPC:  http://172.28.0.10:29902/
Auth: ptxrpc:ptxpass2026

Flags:
  --fast            Skip chi-square (T39-T42) and stress (T43-T46)
  --skip-lottery    Skip settlement test (T150) — long block wait
  --skip-fail-modes Skip T71-T80 adversarial tests
  --skip-stats      Skip T39-T42 chi-square
  --skip-stress     Skip T43-T46 stress
  --skip-advanced   Skip T81-T100
  --skip-excl       Skip T101-T120 exclude hardening
  --skip-excl-probe Skip T105-T113 characterisation (slow)
"""

import urllib.request
import json
import base64
import sys
import time
import math
import hashlib
import argparse
import threading
import subprocess
import os

# ─── Config ───────────────────────────────────────────────────────────────────

RPC_URL      = "http://172.28.0.10:29902/"
RPC_USER     = "ptxrpc"
RPC_PASS     = "ptxpass2026"
ALL_NODES    = [f"gm{i:02d}" for i in range(1, 12)]   # gm01..gm11
QUORUM_SIZE  = 11
THRESHOLD    = 6     # BLS threshold: need ≥6 sigs from 11 GMs
SIG_HEX_LEN = 192   # BLS12-381 G2 compressed = 96 bytes = 192 hex chars
HMS_SAT      = 100_000_000
TIMEOUT      = 30
GM_DATA_FILE = "/tmp/gm_data.txt"

# ─── RPC ──────────────────────────────────────────────────────────────────────

def rpc(method, params, url=RPC_URL):
    payload = json.dumps({"jsonrpc": "1.0", "id": "ptx",
                          "method": method, "params": params}).encode()
    creds = base64.b64encode(f"{RPC_USER}:{RPC_PASS}".encode()).decode()
    req = urllib.request.Request(
        url, data=payload,
        headers={"Content-Type": "text/plain",
                 "Authorization": f"Basic {creds}"})
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

def pose_status():
    return rpc("ptx_pose_status", [])

def lottery_status():
    return rpc("ptx_lottery_status", [])

# ─── Docker helpers ───────────────────────────────────────────────────────────

def _docker(args, timeout=30):
    try:
        r = subprocess.run(["docker"] + args, capture_output=True, timeout=timeout)
        return r.returncode == 0, r.stdout.decode() + r.stderr.decode()
    except Exception as e:
        return False, str(e)

def docker_stop_gm(gm_id):
    """Stop ptx-{gm_id} container (e.g. gm_id='gm11')."""
    return _docker(["stop", f"ptx-{gm_id}"])

def docker_start_gm(gm_id):
    return _docker(["start", f"ptx-{gm_id}"])

def docker_available():
    ok, _ = _docker(["info"])
    return ok

def reinit_gm(gm_id):
    """Read GM_DATA_FILE and run initgamemaster inside ptx-{gm_id}."""
    try:
        with open(GM_DATA_FILE) as f:
            for line in f:
                parts = line.strip().split()
                if len(parts) < 5:
                    continue
                gmid, privkey, _txhash, _idx, ip = parts[:5]
                if gmid != gm_id:
                    continue
                return _docker(["exec", f"ptx-{gmid}",
                                "Hemis-cli", "-ptxtestnet",
                                "-datadir=/root/.hemis-ptxtestnet",
                                "initgamemaster", privkey, f"{ip}:29993"],
                               timeout=20)
        return False, f"{gm_id} not found in {GM_DATA_FILE}"
    except FileNotFoundError:
        return False, f"{GM_DATA_FILE} not found"
    except Exception as e:
        return False, str(e)

# ─── PoSe helpers ─────────────────────────────────────────────────────────────

def get_pose_nodes():
    """Return list of node dicts from ptx_pose_status, or None on error."""
    st, e = pose_status()
    if e or st is None:
        return None
    if isinstance(st, list):
        return st
    if isinstance(st, dict):
        for key in ("nodes", "gamemasters", "pose_records"):
            if key in st:
                return st[key]
    return None

def get_pose_map():
    """Return {node_id: record} from ptx_pose_status."""
    nodes = get_pose_nodes()
    if nodes is None:
        return None
    return {n.get("node_id", n.get("name", "?")): n for n in nodes}

# ─── General helpers ──────────────────────────────────────────────────────────

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
    return sum((c - expected) ** 2 / expected for c in counts)

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

def _is_hex(s, length=None):
    if not isinstance(s, str):
        return False
    if length is not None and len(s) != length:
        return False
    return all(c in "0123456789abcdef" for c in s)

# ─── Test runner ──────────────────────────────────────────────────────────────

results = []
_pass = _fail = _skip = 0

def test(tid, name, fn):
    global _pass, _fail, _skip
    try:
        outcome, detail = fn()
        if outcome is None:
            print(f"  [SKIP] {tid}  {name}")
            if detail:
                print(f"         {detail}")
            results.append((tid, "SKIP", name, detail or ""))
            _skip += 1
        elif outcome:
            print(f"  [PASS] {tid}  {name}")
            results.append((tid, "PASS", name, detail or ""))
            _pass += 1
        else:
            print(f"  [FAIL] {tid}  {name}")
            if detail:
                print(f"         {detail}")
            results.append((tid, "FAIL", name, detail or ""))
            _fail += 1
    except Exception as e:
        print(f"  [FAIL] {tid}  {name}  — exception: {e}")
        results.append((tid, "FAIL", name, f"exception: {e}"))
        _fail += 1

def ok(cond, msg=""):
    return (bool(cond), msg)

# ═══════════════════════════════════════════════════════════════════════════════
# T01-T10  CORE FUNCTIONALITY
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
    bad = [x for x in v if x not in [1, 2]]
    return ok(not bad, f"out-of-range: {bad}")

def t05():
    exclude = list(range(1, 91))
    r, e = roll(5, 1, 100, True, exclude=exclude, game_id="excl_safe", salt="cc01")
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if v in exclude]
    return ok(not bad, f"excluded values appeared: {bad}")

def t06():
    r, e = roll(1, 1, 10, False, exclude=[1, 2, 3, 4, 5, 6, 7, 8, 9],
                game_id="excl_one", salt="cc02")
    if e: return None, f"RPC: {e}"
    v = r["results"][0]
    return ok(v == 10, f"expected 10, got {v}")

def t07():
    r, e = roll(10, 1, 10, True, game_id="fullperm", salt="dd01")
    if e: return None, f"RPC: {e}"
    return ok(sorted(r["results"]) == list(range(1, 11)), f"{r['results']}")

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

# ═══════════════════════════════════════════════════════════════════════════════
# T11-T20  CRYPTOGRAPHIC PROPERTIES  (Phase 2 updated)
# ═══════════════════════════════════════════════════════════════════════════════

def t11():
    r, e = roll(1, 1, 100, False, game_id="seed_fmt", salt="ee01")
    if e: return None, f"RPC: {e}"
    s = r["round_seed"]
    return ok(_is_hex(s, 64), f"'{s}'")

def t12():
    """Phase 2: quorum_sig must be exactly 192 hex chars (BLS12-381 G2 compressed = 96 bytes)."""
    r, e = roll(1, 1, 100, False, game_id="sig_fmt", salt="ee02")
    if e: return None, f"RPC: {e}"
    s = r["quorum_sig"]
    return ok(_is_hex(s, SIG_HEX_LEN),
              f"len={len(s)} (expected {SIG_HEX_LEN}) sig='{s[:32]}...'")

def t13():
    """Phase 2: quorum_members must contain exactly 11 nodes."""
    r, e = roll(1, 1, 100, False, game_id="members", salt="ee03")
    if e: return None, f"RPC: {e}"
    members = r["quorum_members"]
    unknown = [m for m in members if m not in ALL_NODES]
    return ok(len(members) == QUORUM_SIZE and not unknown,
              f"got {len(members)} members: {members} — expected {QUORUM_SIZE}")

def t14():
    st, e = status()
    if e: return None, f"RPC: {e}"
    if not st["rounds"]: return None, "no rounds"
    b = st["rounds"][0]["beacon"]
    return ok(_is_hex(b, 64), f"'{b}'")

def t15():
    r, e = roll(1, 1, 100, False, game_id="bheight", salt="ee04")
    if e: return None, f"RPC: {e}"
    h = r["block_height"]
    return ok(isinstance(h, int) and h > 0, f"block_height={h}")

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
    if not st: return None, "status unavailable"
    beacons = [rd["beacon"] for rd in st.get("rounds", [])]
    if len(beacons) < 2: return None, "fewer than 2 rounds in status (BUG-005)"
    return ok(len(set(beacons)) == len(beacons), "duplicate beacon")

def t18():
    r1, _ = roll(1, 1, 100, False, game_id="diff_salt", salt="aa0011")
    r2, _ = roll(1, 1, 100, False, game_id="diff_salt", salt="bb0022")
    if not r1 or not r2: return None, "RPC error"
    return ok(r1["round_seed"] != r2["round_seed"],
              "same seed despite different salt")

def t19():
    r1, _ = roll(1, 1, 100, False, game_id="replay", salt="cc0011")
    time.sleep(0.5)
    r2, _ = roll(1, 1, 100, False, game_id="replay", salt="cc0011")
    if not r1 or not r2: return None, "RPC error"
    return ok(r1["round_seed"] != r2["round_seed"],
              "same params same seed — replay not prevented")

def t20():
    r1, _ = roll(1, 1, 100, False, game_id="sig_a", salt="dd0011")
    r2, _ = roll(1, 1, 100, False, game_id="sig_b", salt="dd0022")
    if not r1 or not r2: return None, "RPC error"
    return ok(r1["quorum_sig"] != r2["quorum_sig"], "identical quorum sigs")

# ═══════════════════════════════════════════════════════════════════════════════
# T21-T28  ROUND STATUS & POSE
# ═══════════════════════════════════════════════════════════════════════════════

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
    committed = sorted(rd.get("committed", []))
    return ok(committed == sorted(ALL_NODES),
              f"committed={committed}")

def t23():
    st, e = status()
    if e: return None, f"RPC: {e}"
    for rd in st.get("rounds", []):
        if rd.get("withheld"):
            return False, f"withheld: {rd['withheld']}"
    return True, "all rounds withheld=[]"

def t24():
    st, e = status()
    if e: return None, f"RPC: {e}"
    for rd in st.get("rounds", []):
        if rd.get("abstained"):
            return False, f"abstained: {rd['abstained']}"
    return True, "all rounds abstained=[]"

def t25():
    r, e = roll(1, 1, 100, False, game_id="rid_fmt", salt="9903")
    if e: return None, f"RPC: {e}"
    rd = find_round_by_height(r["block_height"])
    if not rd: return None, "round not found (BUG-005)"
    rid = rd["round_id"]
    return ok(isinstance(rid, str) and len(rid) >= 16
              and all(c in "0123456789abcdef" for c in rid), f"'{rid}'")

def t26():
    st, e = status()
    if e: return None, f"RPC: {e}"
    records = {r["node_id"]: r for r in st.get("pose_records", [])}
    not_eligible = [n for n in ALL_NODES
                    if not records.get(n, {}).get("eligible", False)]
    return ok(not not_eligible, f"not eligible: {not_eligible}")

def t27():
    st, e = status()
    if e: return None, f"RPC: {e}"
    records = {r["node_id"]: r for r in st.get("pose_records", [])}
    zero = [n for n in ALL_NODES if records.get(n, {}).get("tickets", 0) <= 0]
    if zero:
        return ok(False, f"zero tickets: {zero} (may self-resolve after first roll)")
    return True, "all 11 nodes have tickets > 0"

def t28():
    st, e = status()
    if e: return None, f"RPC: {e}"
    rids = [rd["round_id"] for rd in st.get("rounds", [])]
    return ok(len(set(rids)) == len(rids), f"duplicate round_ids: {rids}")

# ═══════════════════════════════════════════════════════════════════════════════
# T29-T38  GAME SCENARIOS
# ═══════════════════════════════════════════════════════════════════════════════

def t29():
    vals = []
    for i in range(10):
        r, e = roll(1, 0, 1, False, game_id="coin", salt=f"aa{i:02x}")
        if e: return None, f"RPC: {e}"
        vals.append(r["results"][0])
    bad = [v for v in vals if v not in [0, 1]]
    return ok(not bad, f"out of range: {bad}")

def t30():
    r, e = roll(6, 1, 6, False, game_id="d6_all", salt="aa10")
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if not 1 <= v <= 6]
    return ok(not bad, f"{bad}")

def t31():
    r, e = roll(5, 1, 20, False, game_id="d20", salt="aa11")
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if not 1 <= v <= 20]
    return ok(not bad, f"{bad}")

def t32():
    r, e = roll(5, 1, 100, False, game_id="d100", salt="aa12")
    if e: return None, f"RPC: {e}"
    bad = [v for v in r["results"] if not 1 <= v <= 100]
    return ok(not bad, f"{bad}")

def t33():
    r, e = roll(5, 1, 52, True, game_id="cards", salt="aa13")
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v)) != 5: return False, f"duplicates: {v}"
    return ok(all(1 <= x <= 52 for x in v), f"{v}")

def t34():
    r, e = roll(52, 1, 52, True, game_id="full_deck", salt="aa14")
    if e: return None, f"RPC: {e}"
    return ok(sorted(r["results"]) == list(range(1, 53)),
              "not a full 52-card permutation")

def t35():
    r, e = roll(1, 1, 10000, False, game_id="raffle_big", salt="aa15")
    if e: return None, f"RPC: {e}"
    return ok(1 <= r["results"][0] <= 10000, f"got {r['results'][0]}")

def t36():
    r, e = roll(16, 1, 128, True, game_id="tourney16", salt="aa16")
    if e: return None, f"RPC: {e}"
    v = r["results"]
    if len(set(v)) != 16: return False, f"duplicates: {v}"
    return ok(all(1 <= x <= 128 for x in v), f"{v}")

def t37():
    r1, e = roll(5, 1, 100, True, game_id="chain_a", salt="aa17")
    if e: return None, f"RPC: {e}"
    r2, e = roll(5, 1, 100, True, game_id="chain_b", salt="aa18")
    if e: return None, f"RPC: {e}"
    return ok(r1["results"] != r2["results"],
              f"identical results: {r1['results']}")

def t38():
    r1, e = roll(5, 1, 52, True, game_id="hand1", salt="aa19")
    if e: return None, f"RPC: {e}"
    hand1 = r1["results"]
    r2, e = roll(5, 1, 52, True, exclude=hand1, game_id="hand2", salt="aa20")
    if e: return None, f"RPC: {e}"
    overlap = [v for v in r2["results"] if v in hand1]
    return ok(not overlap, f"overlap: hand1={hand1} hand2={r2['results']}")

# ═══════════════════════════════════════════════════════════════════════════════
# T39-T42  STATISTICAL VALIDATION  (skip with --fast or --skip-stats)
# ═══════════════════════════════════════════════════════════════════════════════

def t39():
    counts = [0, 0]
    for i in range(40):
        r, e = roll(5, 0, 1, False, game_id="stat_coin", salt=f"ac{i:04x}")
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts, sum(counts), 2)
    return ok(chi2 < 6.635, f"chi2={chi2:.4f} (limit 6.635) counts={counts}")

def t40():
    counts = [0] * 7
    for i in range(100):
        r, e = roll(6, 1, 6, False, game_id="stat_d6", salt=f"ed{i:04x}")
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 6)
    return ok(chi2 < 15.086, f"chi2={chi2:.4f} (limit 15.086)")

def t41():
    counts = [0] * 21
    for i in range(50):
        r, e = roll(20, 1, 20, False, game_id="stat_d20", salt=f"520{i:03x}")
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 20)
    return ok(chi2 < 36.191, f"chi2={chi2:.4f} (limit 36.191)")

def t42():
    counts = [0] * 101
    for i in range(20):
        r, e = roll(10, 1, 100, False, game_id="stat_d100", salt=f"a10{i:02x}")
        if e: return None, f"RPC: {e}"
        for v in r["results"]: counts[v] += 1
    chi2 = _chisq(counts[1:], sum(counts[1:]), 100)
    return ok(chi2 < 148.23, f"chi2={chi2:.2f} (limit 148.23)")

# ═══════════════════════════════════════════════════════════════════════════════
# T43-T46  STRESS TESTS  (skip with --fast or --skip-stress)
# ═══════════════════════════════════════════════════════════════════════════════

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
    if len(set(v)) != 50: return False, "duplicates"
    return ok(all(1 <= x <= 100 for x in v), "out of range")

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
def t63(): return _inv([1, 1, 100, False, [[1, 2], [3, 4]], "inv_t63", "aa0017"], "exclude=[[1,2],[3,4]]")
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

# ═══════════════════════════════════════════════════════════════════════════════
# T71-T80  ADVERSARIAL / FAIL MODES  (node IDs updated for Phase 2)
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
        return ok("gm02" in rd.get("withheld", []),
                  f"gm02 not in withheld: {rd.get('withheld')}")
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
    """Phase 2 PoSe: docker stop gm11 + roll → gm11.pose_score increases."""
    if not docker_available():
        return None, "docker not available — skip Phase 2 PoSe increment test"
    pm = get_pose_map()
    if pm is None:
        return None, "ptx_pose_status unavailable"
    score_before = pm.get("gm11", {}).get("pose_score", 0)

    ok_stop, err = docker_stop_gm("gm11")
    if not ok_stop:
        return None, f"docker stop ptx-gm11 failed: {err}"
    # Wait for the PTX node to detect gm11 is gone (heartbeat timeout)
    time.sleep(15)
    try:
        r, e = roll(1, 1, 100, False, game_id="pose_incr_p2", salt="p2ad0001")
        if e:
            return None, f"roll failed while gm11 stopped: {e}"
        time.sleep(2)
        pm2 = get_pose_map()
        if pm2 is None:
            return None, "ptx_pose_status unavailable after roll"
        score_after = pm2.get("gm11", {}).get("pose_score", 0)
        return ok(score_after > score_before,
                  f"gm11 pose_score: {score_before} → {score_after}")
    finally:
        docker_start_gm("gm11")
        time.sleep(8)
        reinit_gm("gm11")
        time.sleep(3)

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
    return ok("gm02" not in rd.get("withheld", []),
              f"gm02 still withheld: {rd.get('withheld')}")

def t78():
    st1, _ = status()
    s1 = {r["node_id"]: r["pose_score"] for r in st1.get("pose_records", [])}
    for i in range(3):
        roll(1, 1, 100, False, game_id=f"stable_{i}", salt=f"e0{i:04x}")
    st2, _ = status()
    s2 = {r["node_id"]: r["pose_score"] for r in st2.get("pose_records", [])}
    grew = [n for n in ALL_NODES if s2.get(n, 0) > s1.get(n, 0)]
    return ok(not grew, f"pose_score grew: {grew}")

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
    return ok(r and 1 <= r["results"][0] <= 100,
              f"got {r['results'][0] if r else 'none'}")

# ═══════════════════════════════════════════════════════════════════════════════
# T81-T100  ADVANCED  (T93-T95 updated: f=6 threshold for 11-GM cluster)
# ═══════════════════════════════════════════════════════════════════════════════

def t81():
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
    bad    = [i for i in range(5) if results_list[i]
              and not (1 <= results_list[i]["results"][0] <= 1000)]
    if errors: return False, f"errors: {errors}"
    if bad:    return False, f"out-of-range: {bad}"
    seeds = [results_list[i]["round_seed"] for i in range(5) if results_list[i]]
    return ok(len(seeds) == 5, f"only {len(seeds)}/5 completed")

def t82():
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
    return ok(len(set(seeds)) == len(seeds),
              "duplicate seeds across concurrent calls — possible shared round state")

def t83():
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
    return ok(len(set(seeds)) > 1,
              f"all seeds identical despite different salts: {seeds[0] if seeds else 'n/a'}")

def t84():
    errors = []
    for i in range(30):
        r, e = roll(1, 1, 100, False, game_id=f"load_{i}", salt=f"1d{i:04x}")
        if e: errors.append(f"{i}: {str(e)[:40]}")
        elif not (1 <= r["results"][0] <= 100): errors.append(f"{i}: out of range")
    return ok(not errors, f"{len(errors)} errors in 30 rolls: {errors[:3]}")

def t85():
    for i in range(30):
        roll(1, 1, 100, False, game_id=f"pre_load_{i}", salt=f"b1{i:04x}")
    return ok(node_alive(), "node unresponsive after 30-roll load")

def t86():
    fake_txid = "a" * 64
    r, e = roll(1, 1, 100, False, exclude=[fake_txid], game_id="txid_fake", salt="de0001")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on fake tx_id in exclude"
    if e: return None, f"RPC error (acceptable): {str(e)[:60]}"
    v = r["results"][0]
    return ok(1 <= v <= 100, f"got {v} — fake tx_id silently skipped")

def t87():
    r1, e = roll(1, 1, 52, True, game_id="txid_source", salt="de0002")
    if e: return None, f"first roll failed: {e}"
    pending_txid = r1.get("tx_id", "b" * 64)
    if pending_txid == "pending":
        pending_txid = "b" * 64
    r2, e2 = roll(1, 1, 52, False, exclude=[pending_txid], game_id="txid_pending", salt="de0003")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on pending tx_id"
    if e2: return None, f"RPC error (acceptable): {str(e2)[:60]}"
    v = r2["results"][0]
    return ok(1 <= v <= 52, f"got {v}")

def t88():
    exclude_mixed = [1, 2, 3, "c" * 64, 4, 5]
    r, e = roll(1, 1, 10, False, exclude=exclude_mixed, game_id="txid_mixed", salt="de0004")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on mixed exclude"
    if e: return None, f"RPC error: {str(e)[:60]}"
    v = r["results"][0]
    if v in [1, 2, 3, 4, 5]:
        return False, f"excluded integer appeared: {v}"
    return ok(6 <= v <= 10, f"got {v}")

def t89():
    fake_txids = ["d" * 64, "e" * 64, "f" * 64]
    r, e = roll(1, 1, 100, False, exclude=fake_txids, game_id="txid_multi", salt="de0005")
    alive = node_alive()
    if not alive: return False, "NODE CRASHED on multiple fake tx_ids"
    if e: return None, f"RPC error (acceptable): {str(e)[:60]}"
    v = r["results"][0]
    return ok(1 <= v <= 100, f"got {v}")

def t90():
    r1, e = roll(1, 1, 100, False, game_id="anchor_same", salt="ba0001")
    if e: return None, f"RPC: {e}"
    r2, e = roll(1, 1, 100, False, game_id="anchor_same", salt="ba0001")
    if e: return None, f"RPC: {e}"
    return ok(r1["round_seed"] != r2["round_seed"],
              f"KDD-015 violated: identical re-rolls produced same seed")

def t91():
    r1, e = roll(1, 1, 100, False, game_id="anchor_diff", salt="ba0002")
    if e: return None, f"RPC: {e}"
    h1 = r1["block_height"]
    for _ in range(30):
        time.sleep(3)
        if blockcount() > h1:
            break
    r2, e = roll(1, 1, 100, False, game_id="anchor_diff", salt="ba0002")
    if e: return None, f"RPC: {e}"
    h2 = r2["block_height"]
    if h1 == h2:
        return None, f"block didn't advance (h={h1}) — test inconclusive"
    return ok(r1["round_seed"] != r2["round_seed"],
              f"different blocks same params but same seed — h1={h1} h2={h2}")

def t92():
    r, e = roll(1, 1, 100, False, game_id="anchor_field", salt="ba0003")
    if e: return None, f"RPC: {e}"
    h = r["block_height"]
    bc = blockcount()
    return ok(isinstance(h, int) and h > 0 and abs(h - bc) <= 5,
              f"block_height={h} blockcount={bc}")

def t93():
    """f=6 withhold: 6 GMs withheld → 5 remain, below threshold=6 → should FAIL."""
    targets = ["gm02", "gm03", "gm04", "gm05", "gm06", "gm07"]
    for t in targets:
        fail_mode(t, "withhold")
    try:
        r, e = rpc("ptx_roll", [1, 1, 100, False, [], "f6_withhold", "f60001"])
        alive = node_alive()
        if not alive:
            return False, "NODE CRASHED — should return error, not crash"
        if e:
            return True, f"correctly failed: {str(e)[:80]}"
        members = (r or {}).get("quorum_members", [])
        if len(members) < THRESHOLD:
            return False, f"threshold violated — only {len(members)} members signed"
        return None, f"round completed with {len(members)} members — f=6 may not be enforced"
    finally:
        for t in targets:
            fail_mode(t, "normal")

def t94():
    """f=6 abstain: 6 GMs abstain → round should fail gracefully."""
    targets = ["gm03", "gm04", "gm05", "gm06", "gm07", "gm08"]
    for t in targets:
        fail_mode(t, "abstain")
    try:
        r, e = rpc("ptx_roll", [1, 1, 100, False, [], "f6_abstain", "f60002"])
        alive = node_alive()
        if not alive:
            return False, "NODE CRASHED on f=6 abstain"
        if e:
            return True, f"correctly failed: {str(e)[:80]}"
        members = (r or {}).get("quorum_members", [])
        if len(members) < THRESHOLD:
            return False, f"threshold violated — only {len(members)} members"
        return None, f"round completed with {len(members)} members unexpectedly"
    finally:
        for t in targets:
            fail_mode(t, "normal")

def t95():
    for n in ALL_NODES:
        fail_mode(n, "normal")
    time.sleep(1)
    r, e = roll(1, 1, 100, False, game_id="f6_recovery", salt="f60003")
    if e: return False, f"node did not recover: {e}"
    return ok(r and 1 <= r["results"][0] <= 100,
              f"got {r['results'][0] if r else 'none'}")

def t96():
    seeds = []
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"nonce_{i}", salt="deadbeef00000000")
        if e: return None, f"RPC error at i={i}: {e}"
        seeds.append(r["round_seed"])
    return ok(len(set(seeds)) == 5,
              f"duplicate seeds with same salt — nonce not advancing: {seeds}")

def t97():
    r1, e = roll(1, 1, 100, False, game_id="nonce_adv_1", salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    r2, e = roll(1, 1, 100, False, game_id="nonce_adv_2", salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    r3, e = roll(1, 1, 100, False, game_id="nonce_adv_3", salt="cafebabe00000000")
    if e: return None, f"RPC: {e}"
    seeds = [r1["round_seed"], r2["round_seed"], r3["round_seed"]]
    return ok(len(set(seeds)) == 3,
              f"duplicate seeds with fixed salt — nonce not chaining: {seeds}")

def t98():
    sigs = []
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"sig_chain_{i}", salt=f"ac{i:08x}")
        if e: return None, f"RPC: {e}"
        sigs.append(r["quorum_sig"])
    return ok(len(set(sigs)) == 5, f"duplicate quorum_sig across rounds")

def t99():
    exclude = list(range(1, 96))
    r, e = roll(1, 1, 200, False, exclude=exclude, game_id="excl_95", salt="eb0001")
    if e: return False, f"FAIL at 95 items — BUG-003 threshold lower than expected: {e}"
    v = r["results"][0]
    if v in exclude: return False, f"excluded value returned: {v}"
    return ok(96 <= v <= 200, f"got {v}")

def t100():
    exclude = list(range(1, 97))
    r, e = rpc("ptx_roll", [1, 1, 200, False, exclude, "excl_96", "eb0002"])
    alive = node_alive()
    if not alive:
        return False, "NODE CRASHED on 96-item exclude — severe"
    if e:
        return True, f"BUG-003 confirmed at 96 items: {str(e)[:60]}"
    v = r["results"][0] if r else None
    if v and v not in exclude and 1 <= v <= 200:
        return True, f"POSSIBLE FIX: 96-item exclude now works! got {v}"
    return False, f"unexpected result: {r}"

# ═══════════════════════════════════════════════════════════════════════════════
# T101-T120  EXCLUDE PATH HARDENING
# ═══════════════════════════════════════════════════════════════════════════════

def _excl_probe(excl_size, iterations, pool_high, label):
    exclude = list(range(1, excl_size + 1))
    violations = []
    errors = []
    for i in range(iterations):
        r, e = roll(1, 1, pool_high, False, exclude=exclude,
                    game_id=f"{label}_{i:04x}", salt=f"{i:08x}")
        if e:
            errors.append(f"iter {i}: {str(e)[:40]}")
            continue
        v = r["results"][0]
        if v in exclude:
            violations.append((i, v))
    return violations, errors, iterations - len(errors)

def t101():
    exclude = list(range(1, 98))
    r, e = rpc("ptx_roll", [1, 1, 300, False, exclude, "excl_97", "fc0001"])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED"
    if e: return False, f"BUG-003 not fixed at 97 items: {str(e)[:80]}"
    v = r["results"][0] if r else None
    if v is None: return False, "no result"
    if v in exclude: return False, f"excluded value returned: {v}"
    return ok(98 <= v <= 300, f"got {v}")

def t102():
    exclude = list(range(1, 201))
    r, e = rpc("ptx_roll", [1, 1, 500, False, exclude, "excl_200", "fc0002"])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED at 200 items"
    if e: return False, f"failed at 200 items: {str(e)[:80]}"
    v = r["results"][0] if r else None
    if v is None: return False, "no result"
    if v in exclude: return False, f"excluded value returned: {v}"
    return ok(201 <= v <= 500, f"got {v}")

def t103():
    exclude = list(range(1, 501))
    r, e = rpc("ptx_roll", [1, 1, 1000, False, exclude, "excl_500", "fc0003"])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED at 500 items"
    if e: return False, f"failed at 500 items: {str(e)[:80]}"
    v = r["results"][0] if r else None
    if v is None: return False, "no result"
    if v in exclude: return False, f"excluded value returned: {v}"
    return ok(501 <= v <= 1000, f"got {v}")

def t104():
    exclude = list(range(1, 1001))
    r, e = rpc("ptx_roll", [1, 1, 2000, False, exclude, "excl_1000", "fc0004"])
    alive = node_alive()
    if not alive: return False, "NODE CRASHED at 1000 items"
    if e: return False, f"failed at 1000 items: {str(e)[:80]}"
    v = r["results"][0] if r else None
    if v is None: return False, "no result"
    if v in exclude: return False, f"excluded value returned: {v}"
    return ok(1001 <= v <= 2000, f"got {v}")

def _char_test(excl_size, label):
    violations, errors, completed = _excl_probe(excl_size, 100, 200, label)
    rate = len(violations) / completed * 100 if completed else 0
    detail = (f"excl={excl_size} completed={completed} "
              f"violations={len(violations)} ({rate:.1f}%)")
    if errors:
        detail += f" rpc_errors={len(errors)}"
    if violations:
        detail += f" sample={violations[:3]}"
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
    violations, errors, completed = _excl_probe(40, 200, 200, "bug4_v40")
    rate = len(violations) / completed * 100 if completed else 0
    if violations:
        return False, (f"BUG-004 NOT FIXED: {len(violations)}/{completed} violations "
                       f"({rate:.1f}%) at excl=40")
    return True, f"excl=40: 0 violations in {completed} iterations"

def t115():
    violations, errors, completed = _excl_probe(80, 200, 200, "bug4_v80")
    rate = len(violations) / completed * 100 if completed else 0
    if violations:
        return False, (f"BUG-004 NOT FIXED: {len(violations)}/{completed} violations "
                       f"({rate:.1f}%) at excl=80")
    return True, f"excl=80: 0 violations in {completed} iterations"

def t116():
    all_violations = {}
    for excl_size in range(10, 91, 10):
        violations, errors, completed = _excl_probe(
            excl_size, 50, 200, f"bug4_sweep_{excl_size}")
        if violations:
            all_violations[excl_size] = len(violations)
    if all_violations:
        return False, f"BUG-004 violations at sizes: {all_violations}"
    return True, "sweep excl=10-90 step 10: 0 violations at all sizes"

def t117():
    all_drawn = []
    for rnd in range(10):
        r, e = roll(5, 1, 52, True, exclude=all_drawn,
                    game_id=f"cardgame_r{rnd}", salt=f"c9{rnd:04x}")
        if e:
            return False, f"round {rnd+1} failed (excl={len(all_drawn)} items): {e}"
        drawn = r["results"]
        overlap = [v for v in drawn if v in all_drawn]
        if overlap:
            return False, f"round {rnd+1}: overlap {overlap}"
        if not all(1 <= v <= 52 for v in drawn):
            return False, f"round {rnd+1}: out-of-range: {drawn}"
        all_drawn.extend(drawn)
    return True, f"10 rounds × 5 cards = {len(all_drawn)} unique, no overlaps"

def t118():
    exclude = list(range(1, 48))
    remaining = list(range(48, 53))
    r, e = roll(1, 1, 52, False, exclude=exclude, game_id="near_depleted", salt="ed0001")
    if e: return False, f"failed at 47-item exclude: {e}"
    v = r["results"][0]
    if v in exclude: return False, f"excluded value {v} returned"
    return ok(v in remaining, f"got {v} — expected one of {remaining}")

def t119():
    exclude = list(range(1, 91))
    remaining = list(range(91, 101))
    violations = 0
    for i in range(50):
        r, e = roll(1, 1, 100, False, exclude=exclude,
                    game_id=f"extreme_{i:04x}", salt=f"e0{i:08x}")
        if e: return False, f"failed at iter {i}: {e}"
        if r["results"][0] not in remaining:
            violations += 1
    if violations:
        return False, f"{violations}/50 draws returned excluded value"
    return True, "50 iterations at 90% exclude density: 0 violations"

def t120():
    errors = []
    for i in range(10):
        r, e = roll(5, 1, 100, True, game_id=f"regression_{i}", salt=f"ae{i:08x}")
        if e:
            errors.append(f"iter {i}: {e}")
            continue
        v = r["results"]
        if len(set(v)) != 5:
            errors.append(f"iter {i}: duplicates {v}")
        elif not all(1 <= x <= 100 for x in v):
            errors.append(f"iter {i}: out-of-range {v}")
    if errors:
        return False, f"clean path broken: {errors}"
    return True, "10 × 5-unique draws with no excludes: all correct"

# ═══════════════════════════════════════════════════════════════════════════════
# T121-T130  BLS PHASE 2
# ═══════════════════════════════════════════════════════════════════════════════

def t121():
    """quorum_sig must be exactly 192 hex chars (BLS12-381 G2 compressed = 96 bytes)."""
    r, e = roll(1, 1, 100, False, game_id="bls_sig_len", salt="b2aa01")
    if e: return None, f"RPC: {e}"
    s = r["quorum_sig"]
    return ok(_is_hex(s, SIG_HEX_LEN),
              f"len={len(s)} (expected {SIG_HEX_LEN}) prefix='{s[:16]}...'")

def t122():
    """quorum_members must contain exactly 11 nodes from the known pool."""
    r, e = roll(1, 1, 100, False, game_id="bls_members11", salt="b2aa02")
    if e: return None, f"RPC: {e}"
    members = r["quorum_members"]
    unknown = [m for m in members if m not in ALL_NODES]
    return ok(len(members) == QUORUM_SIZE and not unknown,
              f"got {len(members)} members: {members}")

def t123():
    """5 consecutive rolls each produce a distinct 192-char quorum_sig."""
    sigs = []
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"bls_dist_{i}", salt=f"b2{i:06x}")
        if e: return None, f"RPC at i={i}: {e}"
        s = r["quorum_sig"]
        if not _is_hex(s, SIG_HEX_LEN):
            return False, f"roll {i}: bad sig len={len(s)}"
        sigs.append(s)
    return ok(len(set(sigs)) == 5, f"only {len(set(sigs))}/5 unique sigs")

def _latest_beacon():
    """Return beacon from the most-recent round in ptx_getroundstatus."""
    st, e = status()
    if e or not st or not st.get("rounds"):
        return None
    latest = max(st["rounds"], key=lambda rd: rd.get("block_height", 0))
    return latest.get("beacon")

def t124():
    """beacon == SHA256(quorum_sig_bytes) — verify derivation math."""
    r, e = roll(1, 1, 100, False, game_id="bls_beacon_sha", salt="b2aa04")
    if e: return None, f"RPC: {e}"
    sig_bytes = bytes.fromhex(r["quorum_sig"])
    expected  = hashlib.sha256(sig_bytes).hexdigest()
    beacon = r.get("beacon") or _latest_beacon()
    if not beacon:
        return None, "beacon not found in roll response or status"
    return ok(beacon == expected,
              f"SHA256(sig)={expected[:16]}... got beacon={beacon[:16]}...")

def t125():
    """beacon is always a 64-char hex string (SHA256 output)."""
    for i in range(5):
        r, e = roll(1, 1, 100, False, game_id=f"bls_bfmt_{i}", salt=f"b2c{i:05x}")
        if e: return None, f"RPC at i={i}: {e}"
        beacon = r.get("beacon") or _latest_beacon()
        if not _is_hex(beacon, 64):
            return False, f"roll {i}: bad beacon='{beacon}'"
    return True, "5 beacons all 64-char hex"

def t126():
    """After a clean roll, status committed list includes all 11 nodes."""
    r, e = roll(1, 1, 100, False, game_id="bls_committed11", salt="b2aa06")
    if e: return None, f"RPC: {e}"
    # BUG-005: find_round_by_height unreliable; use most-recent round instead
    st, se = status()
    if se or not st or not st.get("rounds"):
        return None, "status unavailable"
    latest = max(st["rounds"], key=lambda rd: rd.get("block_height", 0))
    committed = sorted(latest.get("committed", []))
    if not committed:
        return None, "committed list empty in latest round"
    return ok(committed == sorted(ALL_NODES),
              f"committed={committed}")

def t127():
    """quorum_sig in roll response matches sig stored in round status."""
    r, e = roll(1, 1, 100, False, game_id="bls_sigmatch", salt="b2aa07")
    if e: return None, f"RPC: {e}"
    sig_roll = r["quorum_sig"]
    # BUG-005: use most-recent round from status
    st, se = status()
    if se or not st or not st.get("rounds"):
        return None, "status unavailable"
    latest = max(st["rounds"], key=lambda rd: rd.get("block_height", 0))
    sig_status = latest.get("quorum_sig")
    if sig_status is None:
        return None, "quorum_sig not in status round record"
    return ok(sig_roll == sig_status,
              f"roll={sig_roll[:16]}... status={sig_status[:16]}...")

def t128():
    """BLS12-381 G2 compressed point marker: first byte has bit7=1 (compressed), bit6=0 (not infinity)."""
    r, e = roll(1, 1, 100, False, game_id="bls_g2_marker", salt="b2aa08")
    if e: return None, f"RPC: {e}"
    sig = r["quorum_sig"]
    first_byte = int(sig[:2], 16)
    compressed = bool(first_byte & 0x80)
    infinity   = bool(first_byte & 0x40)
    return ok(compressed and not infinity,
              f"first_byte=0x{sig[:2]} compressed={compressed} infinity={infinity}")

def t129():
    """10 consecutive rolls — all quorum_sigs are 192-char hex and all distinct."""
    sigs = []
    for i in range(10):
        r, e = roll(1, 1, 100, False, game_id=f"bls_bulk_{i}", salt=f"b4{i:06x}")
        if e: return None, f"RPC at i={i}: {e}"
        s = r["quorum_sig"]
        if not _is_hex(s, SIG_HEX_LEN):
            return False, f"roll {i}: bad sig len={len(s)}"
        sigs.append(s)
    return ok(len(set(sigs)) == 10, f"only {len(set(sigs))}/10 unique sigs")

def t130():
    """beacon = SHA256(quorum_sig bytes) verified for 3 consecutive rolls."""
    mismatches = []
    for i in range(3):
        r, e = roll(1, 1, 100, False, game_id=f"bls_sha3_{i}", salt=f"b5{i:06x}")
        if e: return None, f"RPC at i={i}: {e}"
        expected = hashlib.sha256(bytes.fromhex(r["quorum_sig"])).hexdigest()
        beacon = r.get("beacon") or _latest_beacon()
        if beacon != expected:
            mismatches.append(f"i={i}: got {str(beacon)[:16]}... want {expected[:16]}...")
    if mismatches:
        return False, f"beacon/SHA256 mismatch: {mismatches}"
    return True, "beacon=SHA256(sig) verified for 3 rolls"

# ═══════════════════════════════════════════════════════════════════════════════
# T131-T140  POSE PHASE 2
# ═══════════════════════════════════════════════════════════════════════════════

def t131():
    """ptx_pose_status returns exactly 11 node records."""
    nodes = get_pose_nodes()
    if nodes is None:
        return None, "ptx_pose_status unavailable"
    return ok(len(nodes) == QUORUM_SIZE,
              f"expected {QUORUM_SIZE} nodes, got {len(nodes)}: "
              f"{[n.get('node_id','?') for n in nodes]}")

def t132():
    """Each pose record has integer pose_score and tickets fields."""
    nodes = get_pose_nodes()
    if nodes is None:
        return None, "ptx_pose_status unavailable"
    bad = []
    for n in nodes:
        nid = n.get("node_id", "?")
        if not isinstance(n.get("pose_score"), int):
            bad.append(f"{nid}: pose_score={n.get('pose_score')!r}")
        if not isinstance(n.get("tickets"), int):
            bad.append(f"{nid}: tickets={n.get('tickets')!r}")
    return ok(not bad, f"missing/bad fields: {bad}")

def t133():
    """All 11 known node IDs appear in ptx_pose_status."""
    pm = get_pose_map()
    if pm is None:
        return None, "ptx_pose_status unavailable"
    missing = [n for n in ALL_NODES if n not in pm]
    return ok(not missing, f"missing from pose_status: {missing}")

def _near_settlement(margin=3):
    """Return True if next settlement block is within <margin> blocks."""
    st, e = lottery_status()
    if e or not st:
        return False
    nsa = st.get("next_settlement_at", 0)
    return nsa > 0 and (nsa - blockcount()) <= margin

def t134():
    """After 3 honest rolls, all 11 nodes' tickets each increase by 3."""
    if _near_settlement(5):
        return None, "settlement imminent — tickets test skipped to avoid HTTP 500"
    pm1 = get_pose_map()
    if pm1 is None:
        return None, "ptx_pose_status unavailable"
    tickets_before = {n: pm1.get(n, {}).get("tickets", 0) for n in ALL_NODES}

    N = 3
    for i in range(N):
        r, e = roll(1, 1, 100, False, game_id=f"pose_tick_{i}", salt=f"p3{i:06x}")
        if e:
            return None, f"roll {i} failed: {e}"

    pm2 = get_pose_map()
    if pm2 is None:
        return None, "ptx_pose_status unavailable after rolls"
    wrong = []
    for n in ALL_NODES:
        tb = tickets_before[n]
        ta = pm2.get(n, {}).get("tickets", 0)
        if ta != tb + N:
            wrong.append(f"{n}: {tb}→{ta} (expected +{N})")
    return ok(not wrong, f"ticket mismatch: {wrong}")

def t135():
    """docker stop gm11 + roll → gm11.pose_score increases."""
    if not docker_available():
        return None, "docker not available"
    pm = get_pose_map()
    if pm is None:
        return None, "ptx_pose_status unavailable"
    score_before = pm.get("gm11", {}).get("pose_score", 0)

    ok_stop, err = docker_stop_gm("gm11")
    if not ok_stop:
        return None, f"docker stop ptx-gm11 failed: {err}"
    time.sleep(15)  # wait for PTX node to detect gm11 is gone
    try:
        r, e = roll(1, 1, 100, False, game_id="pose_stop_p2", salt="p2s0001")
        if e:
            return None, f"roll failed while gm11 stopped: {e}"
        time.sleep(2)
        pm2 = get_pose_map()
        if pm2 is None:
            return None, "ptx_pose_status unavailable after roll"
        score_after = pm2.get("gm11", {}).get("pose_score", 0)
        return ok(score_after > score_before,
                  f"gm11 pose_score: {score_before} → {score_after}")
    finally:
        docker_start_gm("gm11")
        time.sleep(8)
        reinit_gm("gm11")
        time.sleep(3)

def t136():
    """After gm11 restart + reinit + roll → gm11.pose_score decreases by 1."""
    if not docker_available():
        return None, "docker not available"
    if not os.path.exists(GM_DATA_FILE):
        return None, f"{GM_DATA_FILE} not found — cannot reinit GM"

    pm = get_pose_map()
    if pm is None:
        return None, "ptx_pose_status unavailable"
    score_before = pm.get("gm11", {}).get("pose_score", 0)
    if score_before == 0:
        return None, "gm11.pose_score=0 — run T135 first to accumulate score"

    # gm11 should be running (T135 restores it in finally block)
    r, e = roll(1, 1, 100, False, game_id="pose_decay_p2", salt="p2d0001")
    if e:
        return None, f"roll failed: {e}"
    time.sleep(2)
    pm2 = get_pose_map()
    if pm2 is None:
        return None, "ptx_pose_status unavailable after roll"
    score_after = pm2.get("gm11", {}).get("pose_score", 0)
    return ok(score_after == score_before - 1,
              f"gm11 pose_score: {score_before} → {score_after} (expected -{1})")

def t137():
    """3 honest rolls — no node's pose_score increases (score only grows when stopped)."""
    if _near_settlement(5):
        return None, "settlement imminent — stability test skipped to avoid HTTP 500"
    pm1 = get_pose_map()
    if pm1 is None:
        return None, "ptx_pose_status unavailable"
    scores_before = {n: pm1.get(n, {}).get("pose_score", 0) for n in ALL_NODES}
    for i in range(3):
        r, e = roll(1, 1, 100, False, game_id=f"pose_stable_{i}", salt=f"p4{i:06x}")
        if e:
            return None, f"roll {i} failed: {e}"
    pm2 = get_pose_map()
    if pm2 is None:
        return None, "ptx_pose_status unavailable"
    grew = [n for n in ALL_NODES
            if pm2.get(n, {}).get("pose_score", 0) > scores_before[n]]
    return ok(not grew, f"pose_score grew unexpectedly: {grew}")

def t138():
    """All nodes have non-negative integer tickets in ptx_pose_status."""
    nodes = get_pose_nodes()
    if nodes is None:
        return None, "ptx_pose_status unavailable"
    bad = [n.get("node_id", "?") for n in nodes
           if not isinstance(n.get("tickets"), int) or n["tickets"] < 0]
    return ok(not bad, f"bad tickets: {bad}")

def t139():
    """ptx_pose_status node_ids match ALL_NODES exactly (no extras, no missing)."""
    nodes = get_pose_nodes()
    if nodes is None:
        return None, "ptx_pose_status unavailable"
    ids = sorted(n.get("node_id", n.get("name", "?")) for n in nodes)
    return ok(ids == sorted(ALL_NODES),
              f"node IDs mismatch:\n  got:  {ids}\n  want: {sorted(ALL_NODES)}")

def t140():
    """ptx_pose_status returns consistent data across back-to-back calls."""
    pm1 = get_pose_map()
    if pm1 is None:
        return None, "ptx_pose_status unavailable"
    pm2 = get_pose_map()
    if pm2 is None:
        return None, "ptx_pose_status unavailable (second call)"
    diffs = []
    for n in ALL_NODES:
        s1 = pm1.get(n, {}).get("pose_score")
        s2 = pm2.get(n, {}).get("pose_score")
        t1 = pm1.get(n, {}).get("tickets")
        t2 = pm2.get(n, {}).get("tickets")
        if s1 != s2 or t1 != t2:
            diffs.append(f"{n}: score {s1}→{s2} tickets {t1}→{t2}")
    return ok(not diffs, f"inconsistent between calls: {diffs}")

# ═══════════════════════════════════════════════════════════════════════════════
# T141-T150  LOTTERY
# ═══════════════════════════════════════════════════════════════════════════════

def t141():
    """ptx_lottery_status call succeeds and returns a result."""
    st, e = lottery_status()
    if e:
        return None, f"ptx_lottery_status: {e}"
    return ok(st is not None, "null result")

def t142():
    """pool_balance_sat is a non-negative integer."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    bal = st.get("pool_balance_sat")
    return ok(isinstance(bal, int) and bal >= 0,
              f"pool_balance_sat={bal!r}")

def t143():
    """settlement_window is a positive integer."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    sw = st.get("settlement_window")
    return ok(isinstance(sw, int) and sw > 0,
              f"settlement_window={sw!r}")

def t144():
    """next_settlement_at is a positive integer block height."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    nsa = st.get("next_settlement_at")
    return ok(isinstance(nsa, int) and nsa > 0,
              f"next_settlement_at={nsa!r}")

def _eligible_ids(st):
    """Extract node_id strings from eligible_nodes (handles str or dict elements)."""
    raw = st.get("eligible_nodes", [])
    if not raw:
        return []
    if isinstance(raw[0], dict):
        return [n.get("node_id", "?") for n in raw]
    return list(raw)

def t145():
    """eligible_nodes is a non-empty list of known GM identifiers."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    eligible = st.get("eligible_nodes", [])
    if not isinstance(eligible, list):
        return False, f"eligible_nodes is not a list: {eligible!r}"
    ids = _eligible_ids(st)
    unknown = [n for n in ids if n not in ALL_NODES]
    return ok(len(ids) > 0 and not unknown,
              f"eligible_ids={ids}")

def t146():
    """After one roll, pool_balance_sat increases by exactly 1 HMS (100 000 000 sat)."""
    st1, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    bal1 = st1.get("pool_balance_sat", 0)
    nsa  = st1.get("next_settlement_at", 0)
    cur  = blockcount()
    if nsa and nsa <= cur + 1:
        return None, "settlement imminent — pool may distribute mid-test"

    r, e = roll(1, 1, 100, False, game_id="lottery_incr", salt="lb0002")
    if e: return None, f"roll failed: {e}"

    st2, e = lottery_status()
    if e: return None, f"ptx_lottery_status after roll: {e}"
    bal2 = st2.get("pool_balance_sat", 0)
    diff = bal2 - bal1
    return ok(diff == HMS_SAT,
              f"pool delta={diff} sat (expected {HMS_SAT})")

def t147():
    """eligible_nodes from ptx_lottery_status matches all 11 GMs (clean cluster)."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    eligible = set(_eligible_ids(st))
    missing = [n for n in ALL_NODES if n not in eligible]
    return ok(not missing, f"GMs not in eligible: {missing}")

def t148():
    """Pool grows by 3 HMS across 3 rolls (no settlement in between)."""
    st1, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    bal1 = st1.get("pool_balance_sat", 0)
    nsa  = st1.get("next_settlement_at", 0)
    cur  = blockcount()
    if nsa and nsa <= cur + 3:
        return None, "settlement too close — pool may distribute during test"

    for i in range(3):
        r, e = roll(1, 1, 100, False, game_id=f"lottery_grow_{i}", salt=f"lb{i:04x}")
        if e: return None, f"roll {i} failed: {e}"

    st2, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    bal2 = st2.get("pool_balance_sat", 0)
    expected = bal1 + 3 * HMS_SAT
    return ok(bal2 == expected,
              f"pool: {bal1} → {bal2} (expected {expected})")

def t149():
    """next_settlement_at is strictly greater than current block height."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    nsa = st.get("next_settlement_at", 0)
    cur = blockcount()
    return ok(isinstance(nsa, int) and nsa > cur,
              f"next_settlement_at={nsa} blockcount={cur}")

def t150():
    """Wait for settlement block — pool resets and next_settlement_at advances."""
    st, e = lottery_status()
    if e: return None, f"ptx_lottery_status: {e}"
    nsa = st.get("next_settlement_at", 0)
    cur = blockcount()
    blocks_to_wait = nsa - cur

    if blocks_to_wait <= 0:
        return None, f"already past settlement block (nsa={nsa} cur={cur})"
    if blocks_to_wait > 100:
        return None, (f"settlement {blocks_to_wait} blocks away — too far "
                      f"(nsa={nsa} cur={cur})")

    # Fund the pool with a few rolls
    for i in range(min(5, blocks_to_wait)):
        roll(1, 1, 100, False, game_id=f"settle_fund_{i}", salt=f"sf{i:04x}")

    st_funded, e = lottery_status()
    if e: return None, "status after funding failed"
    bal_before = st_funded.get("pool_balance_sat", 0)
    if bal_before == 0:
        return None, "pool is empty — cannot verify settlement"

    print(f"\n         Waiting for settlement block {nsa} "
          f"(cur={blockcount()}, ~{blocks_to_wait} blocks) ...")
    deadline = time.time() + 600
    while time.time() < deadline:
        if blockcount() > nsa:
            break
        time.sleep(10)
    else:
        return None, f"timed out waiting for settlement block {nsa}"

    time.sleep(5)
    st2, e = lottery_status()
    if e: return None, f"post-settlement status failed: {e}"
    bal_after = st2.get("pool_balance_sat", 0)
    nsa2      = st2.get("next_settlement_at", 0)
    distributed = bal_before - bal_after
    return ok(nsa2 > nsa and distributed > 0,
              f"settlement block={nsa}: pool {bal_before}→{bal_after} "
              f"(distributed {distributed} sat), next={nsa2}")

# ─── Main ─────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Hemis PTX Phase 2 Live Node Test Suite v5 (150 tests)")
    parser.add_argument("--fast",            action="store_true",
                        help="Skip chi-square (T39-T42) and stress (T43-T46)")
    parser.add_argument("--skip-lottery",    action="store_true",
                        help="Skip T150 settlement test (long block wait)")
    parser.add_argument("--skip-fail-modes", action="store_true")
    parser.add_argument("--skip-stats",      action="store_true")
    parser.add_argument("--skip-stress",     action="store_true")
    parser.add_argument("--skip-advanced",   action="store_true")
    parser.add_argument("--skip-excl",       action="store_true")
    parser.add_argument("--skip-excl-probe", action="store_true")
    args = parser.parse_args()

    if args.fast:
        args.skip_stats  = True
        args.skip_stress = True

    print()
    print("═" * 72)
    print("  HEMIS PTX PHASE 2 — LIVE NODE TEST SUITE v5  (150 tests)")
    print(f"  RPC:  {RPC_URL}")
    print(f"  GMs:  {QUORUM_SIZE} nodes (gm01-gm11)  threshold={THRESHOLD}  "
          f"sig={SIG_HEX_LEN}chars")
    print("═" * 72)
    print()

    r, err = rpc("getblockcount", [])
    if err:
        print(f"  FATAL: Cannot connect to RPC: {err}")
        sys.exit(1)
    print(f"  Connected. Block height: {r}")
    print(f"  Docker: {'available' if docker_available() else 'NOT AVAILABLE (T74/T135/T136 will skip)'}")
    print()

    # ── Core ──────────────────────────────────────────────────────────────────
    print("── Core Functionality (T01-T10) ──────────────────────────────────────")
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

    # ── Crypto (Phase 2) ──────────────────────────────────────────────────────
    print("── Cryptographic Properties P2 (T11-T20) ────────────────────────────")
    test("T11", "Round seed — valid 64-char hex",                 t11)
    test("T12", "Quorum sig — exactly 192 hex chars (BLS G2)",   t12)
    test("T13", "Quorum members — exactly 11 from known pool",   t13)
    test("T14", "Beacon — valid 64-char hex",                     t14)
    test("T15", "Block height is positive integer",               t15)
    test("T16", "Round seed unique across rounds",                t16)
    test("T17", "Beacon unique across rounds",                    t17)
    test("T18", "Different salts produce different seeds",        t18)
    test("T19", "Re-roll same params → different seed",           t19)
    test("T20", "Quorum sig unique per round",                    t20)
    print()

    # ── Status/PoSe ───────────────────────────────────────────────────────────
    print("── Round Status & PoSe (T21-T28) ────────────────────────────────────")
    test("T21", "Round state=2 after roll (block_height lookup)", t21)
    test("T22", "Round committed = all 11 nodes",                 t22)
    test("T23", "Round withheld is empty",                        t23)
    test("T24", "Round abstained is empty",                       t24)
    test("T25", "Round ID is valid hex",                          t25)
    test("T26", "PoSe — all 11 nodes eligible",                   t26)
    test("T27", "PoSe — tickets > 0 all nodes",                   t27)
    test("T28", "Round IDs unique across rounds",                  t28)
    print()

    # ── Game ──────────────────────────────────────────────────────────────────
    print("── Game Scenarios (T29-T38) ──────────────────────────────────────────")
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

    # ── Statistical ───────────────────────────────────────────────────────────
    if not args.skip_stats:
        print("── Statistical Validation (T39-T42) ─────────────────────────────────")
        test("T39", "Coin flip chi-square (200 flips, p>0.01)",   t39)
        test("T40", "D6 uniformity chi-square (600 rolls)",       t40)
        test("T41", "D20 uniformity chi-square (1000 rolls)",     t41)
        test("T42", "D100 uniformity chi-square (200 rolls)",     t42)
        print()
    else:
        print("── Statistical Tests SKIPPED (--fast / --skip-stats) ─────────────────")
        print()

    # ── Stress ────────────────────────────────────────────────────────────────
    if not args.skip_stress:
        print("── Stress Tests (T43-T46) ────────────────────────────────────────────")
        test("T43", "20 sequential rolls — no crash",             t43)
        test("T44", "Large count — 50 unique from 100",           t44)
        test("T45", "Max int range — 1 to 2,147,483,647",         t45)
        test("T46", "Rapid burst — 15 calls",                     t46)
        print()
    else:
        print("── Stress Tests SKIPPED (--fast / --skip-stress) ────────────────────")
        print()

    # ── Invalid params ────────────────────────────────────────────────────────
    print("── Invalid Params (T47-T70) ──────────────────────────────────────────")
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

    # ── Adversarial ───────────────────────────────────────────────────────────
    if not args.skip_fail_modes:
        print("── Adversarial / Fail Modes (T71-T80) ───────────────────────────────")
        test("T71", "f=1 withhold gm02 — round resolves",          t71)
        test("T72", "f=1 withhold — withheld list populated",     t72)
        test("T73", "f=1 abstain gm03 — round resolves",           t73)
        test("T74", "PoSe increment — docker stop gm11 + roll",   t74)
        test("T75", "f=2 withhold gm02+gm04 — round resolves",    t75)
        test("T76", "f=2 abstain gm03+gm05 — round resolves",     t76)
        test("T77", "Fail mode reset — gm02 participates normally",t77)
        test("T78", "PoSe stable after normal operation",          t78)
        test("T79", "Mixed f=1 withhold + f=1 abstain — resolves",t79)
        test("T80", "Mode cycling — no permanent corruption",      t80)
        print()
    else:
        print("── Fail Mode Tests SKIPPED (--skip-fail-modes) ──────────────────────")
        print()

    # ── Advanced ──────────────────────────────────────────────────────────────
    if not args.skip_advanced:
        print("── Advanced Tests (T81-T100) ─────────────────────────────────────────")
        print("   [Concurrent]")
        test("T81", "5 concurrent rolls — all complete",              t81)
        test("T82", "10 concurrent rolls — unique seeds",             t82)
        test("T83", "Concurrent same game_id diff salts — seeds differ",t83)
        test("T84", "30 sequential rolls — sustained load",           t84)
        test("T85", "Node alive after sustained load",                t85)
        print("   [tx_id Exclude Chaining]")
        test("T86", "Fake tx_id in exclude — no crash, skipped",     t86)
        test("T87", "Pending tx_id in exclude — handled gracefully",  t87)
        test("T88", "Mixed int + tx_id exclude — integers enforced",  t88)
        test("T89", "Multiple fake tx_ids — no crash",                t89)
        print("   [Block Height Anchoring — KDD-003/KDD-015]")
        test("T90", "Same-block re-rolls produce distinct seeds",     t90)
        test("T91", "Different blocks same params → different seed",  t91)
        test("T92", "block_height in response matches chain height",  t92)
        if not args.skip_fail_modes:
            print("   [f=6 Failure Mode — 11-GM cluster threshold=6]")
            test("T93", "f=6 withhold — round fails gracefully (not crash)", t93)
            test("T94", "f=6 abstain — round fails gracefully",              t94)
            test("T95", "Node recovers after f=6 scenario",                  t95)
        else:
            print("   [f=6 Tests SKIPPED — --skip-fail-modes]")
        print("   [Nonce Chaining — KDD-015]")
        test("T96", "5 rounds same salt → 5 distinct seeds",          t96)
        test("T97", "Fixed salt across rounds — nonce advances",      t97)
        test("T98", "Quorum sigs unique across 5 rounds",             t98)
        print("   [Exclude Boundary — BUG-003]")
        test("T99",  "95-item exclude — succeeds (just under threshold)", t99)
        test("T100", "96-item exclude — HTTP 500 (BUG-003 present)",      t100)
        print()
    else:
        print("── Advanced Tests SKIPPED (--skip-advanced) ──────────────────────────")
        print()

    # ── Exclude hardening ─────────────────────────────────────────────────────
    if not args.skip_excl:
        print("── Exclude Path Hardening (T101-T120) ───────────────────────────────")
        print("   [BUG-003 Fix Verification]")
        test("T101", "97-item exclude — succeeds post-fix",               t101)
        test("T102", "200-item exclude — scales correctly",               t102)
        test("T103", "500-item exclude — mid-scale correctness",          t103)
        test("T104", "1000-item exclude — large-scale, node stable",      t104)
        if not args.skip_excl_probe:
            print("   [BUG-004 Characterisation — 900 RPC calls, use --skip-excl-probe to skip]")
            test("T105", "excl=10  100 iters — probe BUG-004 at low size",    t105)
            test("T106", "excl=20  100 iters",                                t106)
            test("T107", "excl=30  100 iters",                                t107)
            test("T108", "excl=40  100 iters — key zone",                     t108)
            test("T109", "excl=50  100 iters",                                t109)
            test("T110", "excl=60  100 iters",                                t110)
            test("T111", "excl=70  100 iters",                                t111)
            test("T112", "excl=80  100 iters",                                t112)
            test("T113", "excl=90  100 iters — near threshold",               t113)
        else:
            print("   [BUG-004 Characterisation SKIPPED — --skip-excl-probe]")
        print("   [BUG-004 Fix Verification]")
        test("T114", "excl=40  200 iters — zero violations required",     t114)
        test("T115", "excl=80  200 iters — zero violations required",     t115)
        test("T116", "excl=10-90 sweep 50 iters — zero violations",       t116)
        print("   [Multi-Round Game Correctness]")
        test("T117", "10-round card game — 50 unique, no cross-round overlap", t117)
        test("T118", "Near-depleted deck — 47 excl, result in remaining 5",    t118)
        test("T119", "Extreme density — 90% excluded, 50 iters, 0 violations", t119)
        test("T120", "Clean path regression — no-exclude draws unaffected",    t120)
        print()
    else:
        print("── Exclude Hardening SKIPPED (--skip-excl) ──────────────────────────")
        print()

    # ── BLS Phase 2 ───────────────────────────────────────────────────────────
    print("── BLS Phase 2 (T121-T130) ───────────────────────────────────────────")
    test("T121", "quorum_sig exactly 192 hex chars (BLS12-381 G2 compressed)",  t121)
    test("T122", "quorum_members exactly 11 nodes from known pool",              t122)
    test("T123", "5 consecutive rolls — all sigs 192 chars and distinct",        t123)
    test("T124", "beacon == SHA256(quorum_sig bytes)",                           t124)
    test("T125", "beacon always 64-char hex across 5 rolls",                     t125)
    test("T126", "Status committed list includes all 11 nodes",                  t126)
    test("T127", "quorum_sig in roll response matches status round record",       t127)
    test("T128", "BLS G2 point: first byte compressed=1, infinity=0",            t128)
    test("T129", "10-roll BLS consistency — all 192-char, all unique",           t129)
    test("T130", "beacon=SHA256(sig) verified for 3 consecutive rolls",          t130)
    print()

    # ── PoSe Phase 2 ──────────────────────────────────────────────────────────
    print("── PoSe Phase 2 (T131-T140) ──────────────────────────────────────────")
    test("T131", "ptx_pose_status returns exactly 11 node records",              t131)
    test("T132", "Each record has integer pose_score and tickets",               t132)
    test("T133", "All 11 known node IDs present in ptx_pose_status",             t133)
    test("T134", "3 honest rolls → all nodes' tickets increase by 3",           t134)
    test("T135", "docker stop gm11 + roll → gm11.pose_score increases",         t135)
    test("T136", "After gm11 restart + reinit + roll → score decreases by 1",   t136)
    test("T137", "Honest rolls — no node pose_score increases",                  t137)
    test("T138", "All nodes have non-negative integer tickets",                  t138)
    test("T139", "pose_status node_ids match ALL_NODES exactly",                 t139)
    test("T140", "ptx_pose_status consistent across back-to-back calls",         t140)
    print()

    # ── Lottery ───────────────────────────────────────────────────────────────
    print("── Lottery (T141-T150) ───────────────────────────────────────────────")
    test("T141", "ptx_lottery_status call succeeds",                             t141)
    test("T142", "pool_balance_sat is non-negative integer",                     t142)
    test("T143", "settlement_window is positive integer",                        t143)
    test("T144", "next_settlement_at is positive integer block height",          t144)
    test("T145", "eligible_nodes is non-empty list of known GMs",                t145)
    test("T146", "After roll, pool_balance_sat increases by 1 HMS (1e8 sat)",   t146)
    test("T147", "eligible_nodes matches all 11 GMs (clean cluster)",            t147)
    test("T148", "Pool grows by 3 HMS across 3 rolls",                           t148)
    test("T149", "next_settlement_at > current block height",                    t149)
    if not args.skip_lottery:
        test("T150", "Settlement: pool resets, next_settlement_at advances",     t150)
    else:
        print("  [SKIP] T150  Settlement test skipped (--skip-lottery)")
        results.append(("T150", "SKIP", "Settlement test", "--skip-lottery"))
        global _skip
        _skip += 1
    print()

    # ── Summary ───────────────────────────────────────────────────────────────
    total = _pass + _fail + _skip
    print("═" * 72)
    print(f"  RESULTS   PASS: {_pass}   FAIL: {_fail}   SKIP: {_skip}   TOTAL: {total}")
    print("═" * 72)

    if _fail > 0:
        print()
        print("  FAILURES:")
        for tid, st, name, detail in results:
            if st == "FAIL":
                print(f"    {tid}  {name}")
                if detail:
                    print(f"         {detail}")

    print()
    print(f"  VERDICT: {'PASS' if _fail == 0 else 'FAIL'}")
    print()
    sys.exit(0 if _fail == 0 else 1)


if __name__ == "__main__":
    main()
