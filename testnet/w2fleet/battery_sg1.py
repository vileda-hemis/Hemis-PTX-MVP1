#!/usr/bin/env python3
"""SG-1a on-fleet battery (W2.2_SG1A_PREIMPL_APPROVED).

Rows (row_d LAST — decision 4, 2026-07-12: it injects a debug PTXDKG and ends
the pristine hold state; every read-only row runs before it):

  c        ★ all-22 same-anchor identity: one fixed anchor queried via
           ptx_debug_selectquorum on ALL 22 GM nodes -> member arrays
           byte-identical (same 11 proTxHashes, same order, share_index
           1..11); a second anchor MUST select differently (proves the row
           is not trivially green).
  fixture  score-order: the selected node_id order at the fixed anchor is
           demonstrably != sorted(node_id) (the KDD-061 alphabetical trap).
  d        KDD-040 on-fleet (LAST): inject ONE debug PTXDKG (battery_w21
           substrate) -> ACTIVE record; at a later anchor the selection is
           EXACTLY the 11 non-active GMs (N22 = 2x11), intersection empty;
           eligibility arithmetic reported (eligible 22, excluded 11,
           pool 11).

Usage: battery_sg1.py [c|fixture|d|all-readonly|all]
       'all' includes row_d and therefore mutates the hold chain.
"""

import json
import sys

sys.path.insert(0, "/mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet")
from harness.node import Node, RPCError
import battery_w21 as w21  # gm01 + inject_at_tip + wait_block_with_tx + quorum_info

N = 22
GMS = [Node(f"gm{i:02d}", "127.0.0.1", 31000 + i, "ptxw2rpc", "ptxw2pass2026")
       for i in range(1, N + 1)]
gm01 = GMS[0]


def ok(msg):
    print(f"  OK: {msg}")


def die(msg):
    print(f"  RED: {msg}")
    sys.exit(1)


def select_at(node, anchor_hash):
    return node.call("ptx_debug_selectquorum", anchor_hash)


def member_tuple(sel):
    return [(m["share_index"], m["node_id"], m["proTxHash"])
            for m in sel["selected"]]


def pick_anchor(back):
    tip = gm01.getblockcount()
    return gm01.getblockhash(tip - back), tip - back


def row_c():
    """All-22 same-anchor identity + second-anchor-differs."""
    a1, h1 = pick_anchor(6)
    ref = select_at(gm01, a1)
    if not ref["formed"]:
        die(f"anchor h{h1}: formed=false (pool {ref['pool']}) — cannot run identity row")
    ref_members = member_tuple(ref)
    if [m[0] for m in ref_members] != list(range(1, 12)):
        die(f"share_index not 1..11 in order: {ref_members}")
    bad = {}
    for nd in GMS[1:]:
        try:
            got = member_tuple(select_at(nd, a1))
        except Exception as e:  # a node failing to answer IS a row failure
            bad[nd.name] = f"unreachable/rpc: {e}"
            continue
        if got != ref_members:
            bad[nd.name] = "selection differs from gm01"
    if bad:
        die(f"IDENTITY FAILED ({len(bad)}/21 differ): {bad}")
    ok(f"all 22 nodes byte-identical at anchor h{h1} "
       f"(first three: {[m[1] for m in ref_members[:3]]})")

    a2, h2 = pick_anchor(7)
    other = member_tuple(select_at(gm01, a2))
    if other == ref_members:
        die(f"anchors h{h1} and h{h2} selected IDENTICALLY — trivially-green "
            f"suspicion; re-anchor and re-run")
    ok(f"second anchor h{h2} selects differently — row is not trivially green")
    return a1, ref_members


def row_fixture():
    """Selected order != alphabetical(node_id) at the fixed anchor."""
    a1, h1 = pick_anchor(6)
    sel = select_at(gm01, a1)
    ids = [m["node_id"] for m in sel["selected"]]
    if ids == sorted(ids):
        die(f"score order == alphabetical at anchor h{h1} — fixture lost its "
            f"power (KDD-061 seam); re-anchor")
    ok(f"score order != alphabetical at h{h1} (first three: {ids[:3]})")


def row_d():
    """KDD-040 on-fleet — LAST (mutates the hold chain: one debug PTXDKG).
    Tolerates the record already existing (rerun after a partial run)."""
    pre = select_at(gm01, gm01.getbestblockhash())
    if pre["active_excluded"] == 0:
        qh, fh, txid = w21.inject_at_tip()
        ok(f"debug PTXDKG accepted at anchor h{fh}")
        mh, mbh = w21.wait_block_with_tx(txid, fh)
        ok(f"ACTIVE record mined at h{mh}")
    elif pre["active_excluded"] == 11:
        ql = gm01.call("ptx_quorum_list")
        quorums = ql["quorums"] if isinstance(ql, dict) and "quorums" in ql else ql
        qh = quorums[0]["quorum_hash"] if isinstance(quorums[0], dict) else quorums[0]
        ok(f"reusing existing ACTIVE record {qh[:16]}... (prior injection)")
    else:
        die(f"unexpected pre-state: active_excluded={pre['active_excluded']}")
    rec = w21.quorum_info(qh)
    active_ids = {m["pro_tx_hash"] for m in rec["members"]}
    if len(active_ids) != 11:
        die(f"record has {len(active_ids)} members, expected 11")

    later, lh = pick_anchor(0)
    sel = select_at(gm01, later)
    if sel["eligible"] != 22 or sel["active_excluded"] != 11 or sel["pool"] != 11:
        die(f"arithmetic wrong at h{lh}: eligible={sel['eligible']} "
            f"excluded={sel['active_excluded']} pool={sel['pool']} (want 22/11/11)")
    if not sel["formed"]:
        die(f"pool==11 but formed=false at h{lh}")
    chosen = {m["proTxHash"] for m in sel["selected"]}
    if chosen & active_ids:
        die(f"KDD-040 VIOLATION: active members re-selected: {chosen & active_ids}")
    if len(chosen) != 11:
        die(f"selected {len(chosen)} != 11")
    # N22 = 2x11: the selection must be EXACTLY the complement — the union
    # partitions the full 22.
    if len(chosen | active_ids) != 22:
        die(f"selection+active != the full 22 ({len(chosen | active_ids)})")
    ok(f"exclusion exact at h{lh}: selected 11 == the 11 non-active GMs, "
       f"intersection empty (eligible 22 / excluded 11 / pool 11)")


def row_v5():
    """V5-swap part (ii), BOTH DIRECTIONS, after row_d's ACTIVE record exists.
    The debug builder now mirrors V5's pool (SG-1a builder alignment), so:
    (x) REJECT: members_override carrying ONE active-quorum member (the rest
        the correct complement) -> V5's pool-reconstruction cannot contain
        the active member -> ptxdkg-member-not-in-quorum.
    (y) ACCEPT: the plain builder — its selection IS the excluded complement
        (pool-aware) -> V5 ACCEPTS -> quorum #2 mines.
    End-state note: the hold chain then has TWO ACTIVE debug quorums and a
    ZERO spare pool (selectquorum at a later anchor: formed=false, pool=0 —
    asserted as the live threshold-gate bonus). Bank-restore resets anytime.
    """
    later, lh = pick_anchor(0)
    pre = select_at(gm01, later)
    if pre["active_excluded"] != 11:
        die(f"row_v5 expects exactly one ACTIVE quorum (row_d) — "
            f"active_excluded={pre['active_excluded']}")
    complement = [m["node_id"] for m in pre["selected"]]

    # the active quorum's member ids, from the registry
    ql = gm01.call("ptx_quorum_list")
    quorums = ql["quorums"] if isinstance(ql, dict) and "quorums" in ql else ql
    if len(quorums) != 1:
        die(f"expected exactly 1 quorum record, got {len(quorums)}")
    qh0 = quorums[0]["quorum_hash"] if isinstance(quorums[0], dict) else quorums[0]
    active_ids = [m["node_id"] for m in w21.quorum_info(qh0)["members"]]

    # (x) complement with one active member swapped in -> V5 REJECTS.
    tainted = complement[:10] + [active_ids[0]]
    qh, fh, txid = w21.inject_at_tip(members_override=tainted,
                                     expect_reject="ptxdkg-member-not-in-quorum")
    if txid is not None:
        die("(x) ACCEPTED a PTXDKG containing an active-quorum member — "
            "V5 exclusion NOT enforced")
    ok("(x) V5 REJECTS a committed member from the active quorum "
       "(ptxdkg-member-not-in-quorum)")

    # (y) the plain pool-aware builder -> the excluded complement -> ACCEPT.
    qh2, fh2, txid2 = w21.inject_at_tip()
    ok(f"(y) V5 ACCEPTS the pool-aware (excluded-complement) selection at h{fh2}")
    mh2, _ = w21.wait_block_with_tx(txid2, fh2)
    ok(f"quorum #2 mined at h{mh2} — both directions enforced")

    post = select_at(gm01, gm01.getbestblockhash())
    if post["pool"] != 0 or post["formed"]:
        die(f"post-state wrong: pool={post['pool']} formed={post['formed']} "
            f"(want 0/false with 22 GMs in 2 quorums)")
    ok("bonus: threshold gate live — pool=0, formed=false with zero spare GMs")


ROWS_READONLY = {"c": row_c, "fixture": row_fixture}

if __name__ == "__main__":
    which = sys.argv[1] if len(sys.argv) > 1 else "all-readonly"
    if which in ROWS_READONLY:
        ROWS_READONLY[which]()
    elif which == "d":
        row_d()
    elif which == "v5":
        row_v5()
    elif which == "all-readonly":
        row_c(); row_fixture()
        print("SG1A READ-ONLY ROWS GREEN (row_d/v5 not run — they mutate the chain)")
    elif which == "all":
        row_c(); row_fixture(); row_d(); row_v5()
        print("SG1A ALL ROWS GREEN (d then v5 ran LAST — hold chain now carries "
              "two debug quorums, spare pool 0)")
    else:
        print(__doc__)
        sys.exit(1)
