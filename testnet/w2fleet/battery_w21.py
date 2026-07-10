#!/usr/bin/env python3
"""W2.1 falsification battery — quorum registry + PTXDKG persistence.

Runs against a restored W2.0a N=22 bank with gm01 up SOLO (the chain carries
the DGM state; single-node is sufficient for every row here).  Rows are
selectable by name so stub->RED cycles can re-run a single row against a
stubbed binary and observe the RED.

Rows (W2.1 plan §5; falsified-NOW set):
  relock  — BUG-019 interim guard: lock all N collateral UTXOs + gate
  t1      — persist-on-connect: C6-mode inject -> mined -> record correct
            (incl. KDD-061 share_index score-order != label-order assert)
  t2      — reorg-undo: invalidateblock -> record GONE (stub undo -> stale -> RED)
  t8      — reconsiderblock -> record re-persisted identically
  t3a     — restart -> record present (evodb survives)
  t3b     — -reindex -> record rebuilt from chain (ReplayBlock path)
  t6      — under-strength: exclude 2 -> formed 11 / completed 9, gapped indices
  t4      — active-at-height boundary via ptx_quorum_list (C3)
  t5      — ODC-030 duplicate-formation: second populate at same anchor REFUSED
  t7      — member containment: bogus member_node_ids -> populate REFUSED

NEVER touches ptx-bea-*/ptxtestnet.  Fleet: ptx-w2 only.
"""
import json
import subprocess
import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from harness.node import Node, RPCError  # noqa: E402

REG_JSON = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/registration-N22.json"
COMPOSE = ["docker", "compose", "-f",
           "/mnt/pve/Node14TB/hemis-ptx/docker-w2/docker-compose.generated.yml",
           "-p", "ptx-w2"]
GM01_DATADIR = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs/gm01"
IMAGE = "hemis-ptx-w2:w21-dev"

gm01 = Node("gm01", "127.0.0.1", 31001, "ptxw2rpc", "ptxw2pass2026")


def die(msg):
    print(f"FAIL: {msg}")
    sys.exit(1)


def ok(msg):
    print(f"  ok: {msg}")


def load_reg():
    with open(REG_JSON) as f:
        return json.load(f)


def operator_secrets():
    return [v["operator_secret"] for v in load_reg().values()]


def wait_next_block(start_height, timeout=900):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = gm01.getblockcount()
        if h > start_height:
            return h
        time.sleep(2)
    die(f"no block staked within {timeout}s (height stuck at {start_height})")


def wait_block_with_tx(txid, start_height, timeout=900):
    t0 = time.time()
    while time.time() - t0 < timeout:
        h = gm01.getblockcount()
        for hh in range(start_height + 1, h + 1):
            bh = gm01.getblockhash(hh)
            blk = gm01.getblock(bh, 1)
            if txid in blk["tx"]:
                return hh, bh
        time.sleep(2)
    die(f"txid {txid} not mined within {timeout}s")


NOSTAKE = "ptx-w21-nostake"


def cli(container, *args):
    """RPC via in-container Hemis-cli (for temp containers w/o published ports)."""
    r = subprocess.run(["docker", "exec", container, "Hemis-cli", "-ptxbea",
                        "-rpcuser=ptxw2rpc", "-rpcpassword=ptxw2pass2026",
                        *args], capture_output=True, text=True)
    return r.returncode, (r.stdout or r.stderr).strip()


def start_nostake():
    """Swap gm01 for a temp container with staking OFF (deterministic reorgs;
    the daemon has no runtime staking toggle)."""
    subprocess.run(COMPOSE + ["stop", "gm01"], check=True, capture_output=True)
    subprocess.run(["docker", "run", "--rm", "-d", "--name", NOSTAKE,
                    "-v", f"{GM01_DATADIR}:/root/.hemis-ptxbea",
                    "-e", "RPCUSER=ptxw2rpc", "-e", "RPCPASSWORD=ptxw2pass2026",
                    "-e", "PTX_NODE_ID=gm01:0b0eb391",
                    IMAGE, "-staking=0"], check=True, capture_output=True)
    for _ in range(40):
        rc, _out = cli(NOSTAKE, "getblockcount")
        if rc == 0:
            return
        time.sleep(3)
    die("nostake container never became RPC-ready")


def stop_nostake_restore_gm01():
    subprocess.run(["docker", "stop", NOSTAKE], capture_output=True)
    subprocess.run(COMPOSE + ["up", "-d", "gm01"], check=True, capture_output=True)
    for _ in range(40):
        try:
            gm01.getblockcount()
            return
        except Exception:
            time.sleep(3)
    die("gm01 did not come back after nostake window")


def quorum_info(qh):
    return gm01.call("ptx_quorum_info", qh)


def quorum_info_absent(qh) -> bool:
    try:
        gm01.call("ptx_quorum_info", qh)
        return False
    except RPCError:
        return True


# ---------------------------------------------------------------------------
def row_relock():
    """BUG-019 (a) interim guard — N-generic core lives in
    harness.cluster.relock_collaterals (also auto-invoked by wait_ready after
    every harness-driven fleet start). This battery is the N22 fixture, so it
    pins expect_n=22. HONEST LIMIT: the daemon auto-locks at init (gmconflock);
    this is the belt + asserted coverage. Residuals R1/R2 (pre-RPC: staker
    starts before the auto-lock; init-abort skips it) are BUG-019 (d)'s to
    close, owed pre-testnet — see relock_collaterals docstring."""
    from harness.cluster import relock_collaterals
    try:
        n = relock_collaterals(gm01, expect_n=22)
    except AssertionError as e:
        die(str(e))
    ok(f"all {n} collaterals locked (listlockunspent gate passed)")


def inject_at_tip(exclude=None, members_override=None, expect_reject=None,
                  premits=6):
    """C6-mode populate at the current tip anchor.  Returns (qh, height, txid)."""
    tip_h = gm01.getblockcount()
    qh = gm01.getbestblockhash()
    spec = {"quorum_hash": qh, "formation_height": tip_h,
            "operator_keys": operator_secrets(), "premits": premits}
    if exclude:
        spec["exclude"] = exclude
    if members_override is not None:
        spec["members_override"] = members_override
    try:
        r = gm01.call("ptx_debug_ptxdkgpopulate", spec, False)
    except RPCError as e:
        if expect_reject and expect_reject in str(e.message):
            return qh, tip_h, None
        die(f"populate: unexpected reject: {e.message}")
    if expect_reject:
        die(f"populate ACCEPTED but expected reject '{expect_reject}' "
            f"(txid {r['txid']})")
    return qh, tip_h, r["txid"]


def row_t1():
    """Persist-on-connect + record-shape + KDD-061 score-order assert."""
    qh, fh, txid = inject_at_tip()
    ok(f"validated populate ACCEPTED at anchor h{fh} (accept-path pre-proof)")
    mh, mbh = wait_block_with_tx(txid, fh)
    ok(f"PTXDKG mined at h{mh}")
    rec = quorum_info(qh)
    assert rec["version"] == 1, rec
    assert rec["quorum_hash"] == qh
    assert rec["formation_height"] == fh
    assert rec["formed_size"] == 11 and rec["completed_size"] == 11
    assert rec["state"] == "active"
    assert rec["provenance"] == 0
    assert rec["accepted_txid"] == txid
    assert rec["mined_height"] == mh and rec["mined_block_hash"] == mbh
    assert rec["last_rotation_height"] == -1 and rec["drift_offset"] == -1
    ms = rec["members"]
    assert len(ms) == 11
    assert [m["share_index"] for m in ms] == list(range(1, 12))
    assert all(m["in_qual"] for m in ms)
    ids = [m["node_id"] for m in ms]
    if ids == sorted(ids):
        die("score order == alphabetical/label order at this anchor — "
            "index-source regression NOT excluded (KDD-061 seam); re-anchor")
    ok(f"record shape correct; score order != label order "
       f"(first three: {ids[:3]})")
    print(json.dumps({"qh": qh, "txid": txid, "mined_height": mh,
                      "mined_block_hash": mbh}))
    return qh, mbh


def row_t2_t8(qh, mbh, expect_stale=False):
    """Reorg-undo (T2) then reconsider (T8), in a staking-OFF temp container."""
    start_nostake()
    try:
        rc, out = cli(NOSTAKE, "invalidateblock", mbh)
        if rc != 0:
            die(f"invalidateblock failed: {out}")
        rc, out = cli(NOSTAKE, "ptx_quorum_info", qh)
        if expect_stale:
            if rc != 0:
                die("undo-stub run: record GONE — stub did not take?")
            print("RED-CONFIRMED: stubbed undo left a STALE quorum record "
                  "after invalidateblock (the phantom-quorum corruption row)")
            cli(NOSTAKE, "reconsiderblock", mbh)
            return
        if rc == 0:
            die(f"T2: record still present after invalidateblock — undo failed: {out}")
        ok("T2: record erased by disconnect (undo proven live)")
        rc, out = cli(NOSTAKE, "reconsiderblock", mbh)
        if rc != 0:
            die(f"reconsiderblock failed: {out}")
        rc, out = cli(NOSTAKE, "ptx_quorum_info", qh)
        if rc != 0:
            die(f"T8: record absent after reconnect: {out}")
        rec = json.loads(out)
        assert rec["mined_block_hash"] == mbh
        ok("T8: record re-persisted on reconnect, identical mined block")
    finally:
        stop_nostake_restore_gm01()


def row_t3a(qh):
    subprocess.run(["docker", "restart", "ptx-w2-gm01"], check=True,
                   capture_output=True)
    for _ in range(60):
        try:
            rec = quorum_info(qh)
            ok(f"T3a: record present after daemon restart (evodb survives)")
            return
        except Exception:
            time.sleep(3)
    die("T3a: gm01 did not answer with the record after restart")


def row_t3b(qh):
    """-reindex: wipe-and-rebuild evodb from block files (ReplayBlock path)."""
    subprocess.run(COMPOSE + ["stop", "gm01"], check=True, capture_output=True)
    subprocess.run(["docker", "run", "--rm", "-d", "--name", "ptx-w21-reindex",
                    "-v", f"{GM01_DATADIR}:/root/.hemis-ptxbea",
                    "-e", "RPCUSER=ptxw2rpc", "-e", "RPCPASSWORD=ptxw2pass2026",
                    "-e", "PTX_NODE_ID=gm01:0b0eb391",
                    IMAGE, "-reindex"], check=True, capture_output=True)
    try:
        deadline = time.time() + 600
        rec = None
        while time.time() < deadline:
            r = subprocess.run(["docker", "exec", "ptx-w21-reindex", "Hemis-cli",
                                "-ptxbea", "-rpcuser=ptxw2rpc",
                                "-rpcpassword=ptxw2pass2026",
                                "ptx_quorum_info", qh],
                               capture_output=True, text=True)
            if r.returncode == 0:
                rec = json.loads(r.stdout)
                break
            time.sleep(5)
        if rec is None:
            die("T3b: reindex node never served the record")
        assert rec["quorum_hash"] == qh and rec["state"] == "active"
        assert [m["share_index"] for m in rec["members"]] == list(range(1, 12))
        ok("T3b: record REBUILT FROM CHAIN by -reindex (load-from-chain proven)")
    finally:
        subprocess.run(["docker", "stop", "ptx-w21-reindex"],
                       capture_output=True)
        subprocess.run(COMPOSE + ["up", "-d", "gm01"], check=True,
                       capture_output=True)
        for _ in range(60):
            try:
                gm01.getblockcount()
                return
            except Exception:
                time.sleep(3)


def row_t6():
    """Under-strength: exclude 2 selected members -> 11/9, gapped indices.

    Selection discovery via build_only (real mode returns member_node_ids in
    selection order), then ONE under-strength inject at a fresh anchor —
    no reorg dance, no duplicate-formation collision.
    """
    tip_h = gm01.getblockcount()
    qh = gm01.getbestblockhash()
    probe = gm01.call("ptx_debug_ptxdkgpopulate",
                      {"quorum_hash": qh, "formation_height": tip_h,
                       "operator_keys": operator_secrets(),
                       "build_only": True}, False)
    sel = probe["member_node_ids"]
    if len(sel) != 11:
        die(f"t6 probe: selection size {len(sel)} != 11")
    drop = [sel[2], sel[5]]  # ranks 3 and 6 -> expected gaps at 3 and 6
    r = gm01.call("ptx_debug_ptxdkgpopulate",
                  {"quorum_hash": qh, "formation_height": tip_h,
                   "operator_keys": operator_secrets(), "exclude": drop}, False)
    wait_block_with_tx(r["txid"], tip_h)
    rec = quorum_info(qh)
    assert rec["formed_size"] == 11 and rec["completed_size"] == 9, rec
    qual_idx = sorted(m["share_index"] for m in rec["members"] if m["in_qual"])
    non_qual = sorted(m["share_index"] for m in rec["members"] if not m["in_qual"])
    assert non_qual == [3, 6], f"expected gaps at ranks 3,6 got {non_qual}"
    assert qual_idx == [1, 2, 4, 5, 7, 8, 9, 10, 11], qual_idx
    ok(f"T6: born-under-strength represented — formed 11 / completed 9, "
       f"GAPPED share indices preserved (missing ranks {non_qual})")


def row_t4():
    """Active-at-height boundaries via ptx_quorum_list (needs >=1 record)."""
    lst = gm01.call("ptx_quorum_list")
    if not lst["quorums"]:
        die("T4: no records — run t1 first")
    tip = gm01.getblockcount()
    for q in lst["quorums"]:
        mh = q["mined_height"]
        at_before = gm01.call("ptx_quorum_list", mh - 1)
        at_mined = gm01.call("ptx_quorum_list", mh)
        qhs_before = {x["quorum_hash"] for x in at_before["quorums"]}
        qhs_mined = {x["quorum_hash"] for x in at_mined["quorums"]}
        assert q["quorum_hash"] not in qhs_before, \
            f"{q['quorum_hash']} active before its mined height"
        assert q["quorum_hash"] in qhs_mined
    at_tip = gm01.call("ptx_quorum_list", tip)
    assert len(at_tip["quorums"]) == len(lst["quorums"])
    ok(f"T4: active-at-height boundaries correct for "
       f"{len(lst['quorums'])} record(s); height-ordered listing")


def row_t5(expect_accept=False):
    """ODC-030 duplicate-formation: second PTXDKG at an ALREADY-ACCEPTED anchor."""
    lst = gm01.call("ptx_quorum_list")
    if not lst["quorums"]:
        die("T5: no accepted record — run t1 first")
    q = lst["quorums"][0]
    qh, fh = q["quorum_hash"], q["formation_height"]
    spec = {"quorum_hash": qh, "formation_height": fh,
            "operator_keys": operator_secrets()}
    try:
        r = gm01.call("ptx_debug_ptxdkgpopulate", spec, False)
    except RPCError as e:
        if expect_accept:
            die(f"stub run: populate rejected anyway: {e.message}")
        if "ptxdkg-duplicate-formation" not in str(e.message):
            die(f"T5: wrong reject: {e.message}")
        ok("T5: duplicate formation REFUSED at validated populate "
           "(ptxdkg-duplicate-formation)")
        return
    if not expect_accept:
        die(f"T5: duplicate populate ACCEPTED (txid {r['txid']}) — "
            "uniqueness check dead")
    print("RED-CONFIRMED: stubbed uniqueness accepted a duplicate formation")
    gm01.call("ptx_debug_ptxdkgpopulate",
              {"quorum_hash": qh, "formation_height": fh, "group_pk": "generate",
               "build_only": True}, False)  # no-op; leave slot as stub seated it


def row_t7(expect_accept=False):
    """Member containment: committed member not in the canonical selection."""
    qh, fh, txid = inject_at_tip(
        members_override=["nonexistent:deadbeef"] + [],
        expect_reject=None if expect_accept else "ptxdkg-member-not-in-quorum")
    if expect_accept:
        if txid is None:
            die("stub run: containment reject fired anyway")
        print("RED-CONFIRMED: stubbed containment accepted a bogus member")
    else:
        ok("T7: bogus committed member REFUSED (ptxdkg-member-not-in-quorum)")


ROWS = {"relock": row_relock, "t1": row_t1, "t4": row_t4,
        "t5": row_t5, "t6": row_t6, "t7": row_t7}

if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        sys.exit(0)
    if args[0] == "t1chain":  # t1 -> t2 -> t8 -> t3a -> t3b as one chain
        row_relock()
        qh, mbh = row_t1()
        row_t2_t8(qh, mbh)
        row_t3a(qh)
        row_t3b(qh)
        print("T1/T2/T8/T3a/T3b ALL GREEN")
    elif args[0] == "t2stale":  # undo-stub RED run: needs qh + mbh from t1
        row_t2_t8(args[1], args[2], expect_stale=True)
    elif args[0] == "t5stub":
        row_t5(expect_accept=True)
    elif args[0] == "t7stub":
        row_t7(expect_accept=True)
    else:
        for a in args:
            print(f"== {a} ==")
            ROWS[a]()
        print("GREEN")
