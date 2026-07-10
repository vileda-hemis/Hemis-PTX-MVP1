#!/usr/bin/env python3
"""W2 fleet validation gates (W2.0a Amendments 2 and 4).

topology_gate  — Amendment 2: a compose that came up is not a mesh that works.
  Builds the actual peer graph from every node's getpeerinfo (intra-fleet IPs)
  and asserts: no isolated node, single connected component spanning all N+1.
  Reports min/max peer counts. A silent partition here would poison every
  downstream formation test.

eligibility_gate — Amendments 1/4: proves the fleet is FORMATION-READY through
  the real KDD-060 selection core, not by reading registration state alone.
  Mechanism: a no-force ptx_debug_ptxdkgpopulate at a tip anchor runs
  CheckPTXDKGTx contextually (SetPendingTx validates at the tip,
  ptx_dkg_pending.cpp:18-42): V4 snapshots the DGM list at the anchor, V5 runs
  PTX_DKG_SelectQuorumFromList. The debug payload's premits carry FABRICATED
  proTxHashes (rpc/ptx.cpp PTX_Debug_BuildPTXDKGTxFromSpec), so:
    - >= 11 eligible+confirmed GMs at the anchor  -> V5 succeeds, V6 rejects
      with 'ptxdkg-committer-not-in-quorum'  == PASS (selection core assembled
      a full canonical quorum from this fleet's registrations)
    - fewer                                        -> 'ptxdkg-quorum-underfull'
      == FAIL (registered != eligible — exactly the insidious state)
  The slot is left untouched either way (populate refused).
  Plus per-GM state checks: DGM count == N, compound ptxNodeId on every GM,
  confirmation depth (tip >= registeredHeight+2) on every GM.

chain_extends — restore-proof helper: the restored fleet must EXTEND (PoS
  staking alive), not merely answer RPC at the banked tip.
"""

import sys
import time
from harness.cluster import W2Cluster
from harness.node import RPCError


def topology_gate(cluster: W2Cluster) -> dict:
    subnet = cluster.subnet_base + "."
    names_by_ip = {f"{cluster.subnet_base}.10": "caller"}
    for i in range(1, cluster.n + 1):
        names_by_ip[f"{cluster.subnet_base}.{10+i}"] = f"gm{i:02d}"

    edges = set()
    peer_counts = {}
    for nd in cluster.all_nodes:
        peers = nd.call("getpeerinfo")
        fleet_peers = set()
        for p in peers:
            ip = p.get("addr", "").rsplit(":", 1)[0]
            if ip.startswith(subnet) and ip in names_by_ip:
                fleet_peers.add(names_by_ip[ip])
        peer_counts[nd.name] = len(fleet_peers)
        for q in fleet_peers:
            edges.add(tuple(sorted((nd.name, q))))

    isolated = [n for n, c in peer_counts.items() if c == 0]

    # union-find connectivity over all N+1 nodes
    parent = {nd.name: nd.name for nd in cluster.all_nodes}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for a, b in edges:
        parent[find(a)] = find(b)
    components = len({find(nd.name) for nd in cluster.all_nodes})

    result = {
        "nodes": len(cluster.all_nodes),
        "edges": len(edges),
        "min_peers": min(peer_counts.values()),
        "max_peers": max(peer_counts.values()),
        "isolated": isolated,
        "components": components,
        "connected": components == 1 and not isolated,
    }
    print(f"[topology] N+1={result['nodes']} edges={result['edges']} "
          f"peers(min/max)={result['min_peers']}/{result['max_peers']} "
          f"components={components} isolated={isolated or 'none'}")
    if not result["connected"]:
        raise AssertionError(f"TOPOLOGY GATE FAILED: {result}")
    print("[topology] GATE PASS — single connected component, no isolated node")
    return result


def eligibility_gate(cluster: W2Cluster, expected_n: int) -> dict:
    gm01 = cluster.gms[0]
    tip = gm01.getblockcount()

    # per-GM registration-state legs
    lst = gm01.protx_list(detailed=True, valid_only=True)
    assert len(lst) == expected_n, f"DGM list {len(lst)} != {expected_n}"
    bad_nid, bad_conf = [], []
    for e in lst:
        st = e.get("dgmstate", {})
        nid = st.get("ptxNodeId", "")
        if ":" not in nid or len(nid.split(":", 1)[1]) != 8:
            bad_nid.append((e.get("proTxHash", "?")[:12], nid))
        if tip < st.get("registeredHeight", 1 << 30) + 2:
            bad_conf.append(e.get("proTxHash", "?")[:12])
    assert not bad_nid, f"non-compound node_ids: {bad_nid}"
    assert not bad_conf, f"not confirmation-deep: {bad_conf}"
    print(f"[eligibility] {expected_n} GMs registered, compound-id'd, "
          f"confirmation-deep at tip={tip}")

    # end-to-end selection proof through the real V5 core
    anchor_hash = gm01.getbestblockhash()
    anchor_h = tip
    spec = {"quorum_hash": anchor_hash, "formation_height": anchor_h,
            "members": 11, "premits": 6}
    try:
        gm01.call("ptx_debug_ptxdkgpopulate", spec)
        raise AssertionError(
            "populate ACCEPTED a fabricated-premit payload — validation regression")
    except RPCError as e:
        reason = e.message
    # Either reason proves V5 assembled a full quorum-of-11: V10
    # member-containment (W2.1 C4) runs only after the underfull check on the
    # assembled selection, and the fake-mode dbggm ids always trip it before
    # V6-V8's committer check is reached.  Pre-C4 binaries fall through to the
    # committer reason instead.
    if ("ptxdkg-committer-not-in-quorum" in reason
            or "ptxdkg-member-not-in-quorum" in reason):
        print(f"[eligibility] GATE PASS — V5 selection assembled a full "
              f"quorum-of-11 at anchor h{anchor_h}; refusal reason: {reason!r}")
        return {"pass": True, "reason": reason, "anchor_height": anchor_h}
    if "ptxdkg-quorum-underfull" in reason:
        raise AssertionError(
            f"ELIGIBILITY GATE FAILED — <11 eligible GMs at anchor h{anchor_h} "
            f"(registered != eligible): {reason!r}")
    raise AssertionError(f"unexpected populate refusal: {reason!r}")


def gmauth_gate(cluster: W2Cluster, registration_path: str) -> dict:
    """SG-0 Piece 1: GMAUTH-live. Every GM must be running as a DETERMINISTIC
    gamemaster whose active operator key matches its registration entry.
    Per GM asserts: getgamemasterstatus answers on the deterministic path
    (dgmstate present), status == "Ready", and dgmstate.operatorPubKey ==
    the registration JSON's operator_pubkey. A legacy-GM answer, an RPC
    error ("This is not a gamemaster." = flags not wired), or a pubkey
    mismatch is a FAIL. Public keys only — no secret is read or printed.
    """
    import json
    with open(registration_path) as f:
        reg = json.load(f)
    bad = {}
    for i, nd in enumerate(cluster.gms, start=1):
        g = f"gm{i:02d}"
        try:
            st = nd.call("getgamemasterstatus")
        except Exception as e:
            # RPCError AND transport failures: a GM whose daemon refuses to
            # run (e.g. operator-key/ProTx service mismatch exits at init)
            # must be NAMED here, not hang the harness upstream.
            bad[g] = f"unreachable/rpc: {e}"
            continue
        status = st.get("status")
        op = (st.get("dgmstate") or {}).get("operatorPubKey")
        want = reg.get(g, {}).get("operator_pubkey")
        if op is None:
            bad[g] = "no dgmstate — not on the deterministic path"
        elif status != "Ready":
            bad[g] = f"status={status!r}"
        elif op != want:
            bad[g] = "operatorPubKey != registration operator_pubkey"
    if bad:
        raise AssertionError(f"GMAUTH GATE FAILED ({len(bad)}/{cluster.n}): {bad}")
    print(f"[gmauth] GATE PASS — {cluster.n}/{cluster.n} GMs deterministic + "
          f"Ready, operator pubkeys match registration")
    return {"n": cluster.n}


def lock_gate(cluster: W2Cluster, expected_n: int) -> dict:
    """SG-0 Piece 2: BUG-019 (a) lock-coverage ASSERT (a gate, not a print).
    Every registered GM collateral must be in gm01's listlockunspent. N-generic
    (22 fixture / 60 fleet). HONEST LIMIT: PASS proves coverage NOW; it cannot
    retro-protect the pre-RPC residuals R1/R2 (staker-starts-before-auto-lock,
    init-abort-skips-auto-lock — see relock_collaterals docstring). Those are
    BUG-019 (d)'s to close (lock before staker start), owed pre-testnet."""
    gm01 = cluster.gms[0]
    protx = gm01.protx_list(detailed=True, valid_only=True)
    outs = {(e["collateralHash"], e["collateralIndex"]) for e in protx}
    if len(outs) != expected_n:
        raise AssertionError(
            f"LOCK GATE FAILED: expected {expected_n} registered GMs, "
            f"protx_list gave {len(outs)} (a consumed collateral DEREGISTERS "
            f"its GM — the BUG-019 signal)")
    locked = {(l["txid"], l["vout"])
              for l in gm01.call("listlockunspent")["transparent"]}
    missing = outs - locked
    if missing:
        raise AssertionError(
            f"LOCK GATE FAILED: {len(missing)}/{len(outs)} collaterals NOT in "
            f"listlockunspent — stake-consumable RIGHT NOW")
    print(f"[locks] GATE PASS — {len(outs)}/{len(outs)} registered collaterals "
          f"in listlockunspent (start->lock residual: see BUG-019 (d))")
    return {"n": len(outs)}


def chain_extends(cluster: W2Cluster, blocks: int = 2, timeout: int = 600) -> int:
    gm01 = cluster.gms[0]
    start = gm01.getblockcount()
    print(f"[extend] waiting for +{blocks} PoS blocks from {start}")
    h = gm01.wait_for_height(start + blocks, timeout=timeout)
    print(f"[extend] chain extends: {start} -> {h}")
    return h


if __name__ == "__main__":
    n = int(sys.argv[1])
    which = sys.argv[2] if len(sys.argv) > 2 else "all"
    c = W2Cluster(n)
    if which == "gmauth":
        # gmauth must FAIL FAST naming a down GM (a bad operator key exits
        # the daemon at init) — a short readiness window, then let the gate
        # itself report unreachable nodes, instead of wait_ready's ceiling.
        try:
            c.wait_ready(timeout=60)
        except TimeoutError as e:
            print(f"[gmauth] proceeding with non-ready nodes ({e})")
    else:
        c.wait_ready()
    if which in ("all", "topology"):
        topology_gate(c)
    if which in ("all", "eligibility"):
        eligibility_gate(c, n)
    if which in ("all", "gmauth"):
        reg = (sys.argv[3] if len(sys.argv) > 3 else
               f"/mnt/pve/Node14TB/hemis-ptx/w2-fleet/registration-N{n}.json")
        gmauth_gate(c, reg)
    if which in ("all", "locks"):
        lock_gate(c, n)
    if which in ("all", "extend"):
        chain_extends(c)
    c.assert_poc_untouched("validate_fleet end")
    print("[validate] ALL REQUESTED GATES PASS; PoC untouched")
