#!/usr/bin/env python3
"""W2 fleet validation gates (W2.0a Amendments 2 and 4).

topology_gate  — Amendment 2: a compose that came up is not a mesh that works.
  Builds the actual peer graph from every node's getpeerinfo (intra-fleet IPs)
  and asserts: no isolated node, single connected component spanning all N+1.
  Reports min/max peer counts. A silent partition here would poison every
  downstream formation test.

eligibility_gate — Amendments 1/4: proves the fleet is FORMATION-READY through
  the real KDD-060 selection core, not by reading registration state alone.
  Mechanism: a no-force ptx_debug_ptxdkgpopulate at a BOUNDARY anchor (V11,
  SG-1b-iii: a tip-anchored probe would refuse ptxdkg-anchor-not-boundary
  before V5 is ever reached; boundaries already carrying a formed quorum are
  walked past — V9 duplicate-formation also fires before V5) runs
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
from harness.cluster import W2Cluster, ptxbea_boundary_interval
from harness.node import RPCError


def peer_host(addr: str) -> str:
    """Bare host from a getpeerinfo `addr`, family-agnostic (ODC-066).

    ★ WHY: the old `rsplit(":",1)[0]` leaves the BRACKETS on an IPv6 peer
    (`[2a07::49]:49165` -> `[2a07::49]`), so a v6 fleet node never matches the
    bracket-free keys in names_by_ip and reads as a NON-peer — a dual-stack node
    would present as isolated and false-fail the topology gate.  Fixed BEFORE the
    v6 nodes come up, so v6 is not the probe-artefact class's next instance.
    `[v6]:port` -> `v6`; `v4:port` -> `v4`; a bare host (no port) is returned
    as-is (a bare v6 has >1 colon and no brackets, so we must not rsplit it)."""
    addr = addr.strip()
    if addr.startswith("["):                 # [v6]:port  or  [v6]
        return addr[1:addr.index("]")]
    if addr.count(":") == 1:                  # v4:port
        return addr.rsplit(":", 1)[0]
    return addr                              # bare v4 or bare v6


def topology_gate(cluster: W2Cluster) -> dict:
    subnet = cluster.subnet_base + "."
    # ★ Caller IPs derived from cluster.callers, not hardcoded to one ".10 =
    # caller".  Two bugs lived in that literal: it named the primary "caller"
    # while a multi-caller cluster calls it "caller1" (the union-find is keyed
    # by node NAME, so the mismatched edge raised KeyError and killed the gate),
    # and it mapped only ONE caller, so callers 2..8 were invisible to the
    # connectivity check — the fleet could have been split across them and this
    # gate would still have passed.  Mirrors gen_fleet's allocator: caller_k
    # sits at subnet.(10-(k-1)), walking DOWN from .10.
    names_by_ip = {}
    for k, nd in enumerate(cluster.callers, start=1):
        names_by_ip[f"{cluster.subnet_base}.{10 - (k - 1)}"] = nd.name
    for i in range(1, cluster.n + 1):
        names_by_ip[f"{cluster.subnet_base}.{10+i}"] = f"gm{i:02d}"

    edges = set()
    peer_counts = {}
    for nd in cluster.all_nodes:
        peers = nd.call("getpeerinfo")
        fleet_peers = set()
        for p in peers:
            ip = peer_host(p.get("addr", ""))
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

    # end-to-end selection proof through the real V5 core.  V11 (SG-1b-iii)
    # requires the anchor ON the formation schedule, so probe the latest
    # boundary <= tip; a boundary already carrying a formed quorum refuses at
    # V9 (duplicate-formation, also pre-V5) — walk back one boundary, bounded
    # BELOW by the registration floor.
    # ★ TWO DEFECTS KILLED HERE (2026-08-06), both live-caught on the W2.5b
    # fleet stand-up:
    #   1. FORMATION_N was a hand-copied literal (80, the SG-1b dev value).
    #      W2.5b moved ptxbea's boundary cadence to 30 (KDD-079 decouple) and
    #      the gate refused every anchor as off-boundary (h400 instance).
    #      N now comes from chainparams via the harness parser — no literal.
    #   2. The anchor was picked from the tip with no regard for bootstrap
    #      progress: V4 snapshots the DGM list AT THE ANCHOR, so an anchor
    #      predating the last registration (+2 confirmation depth) sees an
    #      incomplete list and fails quorum-underfull on a healthy fleet
    #      (h240 instance).  The anchor floor is registration-complete
    #      height; if no boundary postdates it yet, WAIT for the next one.
    N = ptxbea_boundary_interval()
    reg_complete = max(
        e.get("dgmstate", {}).get("registeredHeight", 0) for e in lst) + 2
    floor = max(reg_complete, N)

    def next_boundary_after_tip():
        t = gm01.getblockcount()
        nb = (t // N + 1) * N
        print(f"[eligibility] no probeable boundary in [h{floor}, h{t}] — "
              f"waiting for boundary h{nb} (N={N})")
        gm01.wait_for_height(nb, timeout=N * 90)
        return nb

    anchor_h = tip - (tip % N)
    if anchor_h < floor:
        anchor_h = next_boundary_after_tip()
    reason = None
    for _ in range(4):
        anchor_hash = gm01.call("getblockhash", anchor_h)
        spec = {"quorum_hash": anchor_hash, "formation_height": anchor_h,
                "members": 11, "premits": 6}
        try:
            gm01.call("ptx_debug_ptxdkgpopulate", spec)
            raise AssertionError(
                "populate ACCEPTED a fabricated-premit payload — validation regression")
        except RPCError as e:
            reason = e.message
        if "ptxdkg-duplicate-formation" in reason:
            anchor_h -= N
            if anchor_h < floor:
                # every boundary in [floor, tip] carries a formed quorum —
                # probe a fresh one instead of anchoring into pre-bootstrap
                # history (defect 2's exact failure mode).
                anchor_h = next_boundary_after_tip()
            continue
        break
    else:
        raise AssertionError(
            f"no probeable formation boundary after 4 attempts (last "
            f"h{anchor_h}, floor h{floor}): every candidate refused "
            f"duplicate-formation: {reason!r}")
    if "ptxdkg-anchor-not-boundary" in reason:
        raise AssertionError(
            f"boundary-anchored probe refused as off-boundary at h{anchor_h} — "
            f"the RUNNING binary disagrees with the tree's chainparams (N={N}); "
            f"stale image (re-point/rebuild)?: {reason!r}")
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
    Every registered GM collateral must be in the TREASURY's listlockunspent.
    N-generic (22 fixture / 98 fleet).

    ★ RETARGETED gm01 -> TREASURY (caller) for the wallet-less-GM topology, and
    deliberately KEPT rather than retired. Wallet-less GMs cannot stake at all,
    so the GM-side BUG-019 exposure is gone by construction — but the collateral
    moved into the caller wallet and the caller is the staker, so the
    holds-collateral-AND-stakes condition now lives there. This gate is the
    caller-side assertion of the protection, which the daemon supplies
    automatically because LockGamemasterCollaterals() is wallet-scoped, not
    role-scoped (init.cpp:1886 fires it on !vpwallets.empty(), no -gamemaster
    required) and at 870acc7 runs BEFORE the staker thread starts.

    HONEST LIMIT: PASS proves coverage NOW. The historical residuals R1/R2 are
    discharged at 870acc7 (lock precedes staker start on the same code path),
    but this gate asserts the outcome rather than the ordering — it would still
    pass on a build where the ordering regressed and the race simply was not
    lost. Order is proven by the init.cpp source contract, not here."""
    treasury = cluster.treasury
    protx = treasury.protx_list(detailed=True, valid_only=True)
    outs = {(e["collateralHash"], e["collateralIndex"]) for e in protx}
    if len(outs) != expected_n:
        raise AssertionError(
            f"LOCK GATE FAILED: expected {expected_n} registered GMs, "
            f"protx_list gave {len(outs)} (a consumed collateral DEREGISTERS "
            f"its GM — the BUG-019 signal)")
    locked = {(l["txid"], l["vout"])
              for l in treasury.call("listlockunspent")["transparent"]}
    missing = outs - locked
    if missing:
        raise AssertionError(
            f"LOCK GATE FAILED: {len(missing)}/{len(outs)} collaterals NOT in "
            f"{treasury.name}'s listlockunspent — stake-consumable RIGHT NOW")
    print(f"[locks] GATE PASS — {len(outs)}/{len(outs)} registered collaterals "
          f"locked in {treasury.name} (caller-side: BUG-019 follows the wallet)")
    return {"n": len(outs)}


def soak_gate(cluster: W2Cluster, expected_n: int, blocks: int = 30,
              poll: float = 20.0, timeout: int = 3600) -> dict:
    """SG-0 Piece 3: the protx==N SOAK — the gm44-class deregistration
    tripwire. A quietly-dying collateral deregisters its GM (collateral-spend
    => DGM delete), dropping protx_list below N. A single snapshot cannot see
    it; this holds the assert over a REAL window of staked blocks. Any drop
    below N is an immediate FAIL (a live deregistration — a gm44-class
    recurrence that must be understood before trusting the fleet)."""
    gm01 = cluster.gms[0]
    start_h = gm01.getblockcount()
    target_h = start_h + blocks
    deadline = time.time() + timeout
    checks = []
    print(f"[soak] protx=={expected_n} over +{blocks} blocks "
          f"(h{start_h} -> h{target_h})")
    while True:
        h = gm01.getblockcount()
        n_reg = len(gm01.protx_list(detailed=False))
        checks.append((h, n_reg))
        if n_reg != expected_n:
            raise AssertionError(
                f"SOAK GATE FAILED at h{h}: protx_list == {n_reg}, expected "
                f"{expected_n} — LIVE DEREGISTRATION (gm44-class). STOP.")
        if h >= target_h:
            break
        if time.time() > deadline:
            raise AssertionError(
                f"SOAK GATE FAILED: chain stalled — only h{h} of h{target_h} "
                f"after {timeout}s (protx held {expected_n} throughout)")
        time.sleep(poll)
    print(f"[soak] GATE PASS — protx=={expected_n} held over "
          f"h{start_h} -> h{checks[-1][0]} ({len(checks)} checks, every check "
          f"{expected_n}/{expected_n})")
    return {"start": start_h, "end": checks[-1][0], "checks": len(checks)}


BANK_MARGIN_STAMP = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/.bank-margin-ok"


def bank_gate(cluster: W2Cluster) -> dict:
    """KDD-064 BANK INVARIANT — an ABORT-GATE, not an observation.  The state
    about to be banked MUST carry >=1 coin at depth >= 120 PER PRODUCER
    (gm01 + caller) at the bank tip: a bank without it replays the
    permanent-deadlock question at every restore's gate crossing (depth is
    block-measured — no stakeable coin means no block means depths are frozen
    forever; a restore just replays into the freeze).  On PASS this writes the
    stamp file bank_fleet.sh REQUIRES (fresh) before it will bank; on FAIL it
    raises — DO NOT BANK."""
    # ★ PRODUCER SET RETARGETED (Phase 2).  The premise "one coin per producer,
    # gm01 + caller" assumed GMs stake.  Under the wallet-less-GM topology GMs
    # have no wallet and no staker thread, so the producer set is exactly the
    # STAKING CALLERS — and asking a wallet-less GM for listunspent would fail
    # with "Method not found (disabled)" rather than return 0.  The invariant
    # itself is unchanged: every node that can produce must hold a deep coin, or
    # a restore replays into the permanent-deadlock freeze.
    def deep_count(node):
        us = node.call("listunspent", 1, 9999999)
        return sum(1 for u in us if u["confirmations"] >= 120)

    producers = [n for n in cluster.all_nodes if n.name.startswith("caller")]
    if not producers:
        producers = [cluster.treasury]
    h = cluster.treasury.getblockcount()
    deep = {p.name: deep_count(p) for p in producers}
    print(f"[bank] tip={h} deep(>=120) coins per producer: {deep}")
    starved = [name for name, c in deep.items() if c < 1]
    if starved:
        raise AssertionError(
            f"BANK GATE FAILED — producer(s) {starved} have NO depth>=120 coin "
            f"at the bank tip (counts={deep}).  Banking this state plants a "
            f"permanent post-restore deadlock.  ABORT the re-bank.")
    import time as _t
    stamp = " ".join(f"{k}_deep={v}" for k, v in sorted(deep.items()))
    with open(BANK_MARGIN_STAMP, "w") as f:
        f.write(f"height={h} {stamp} ts={int(_t.time())}\n")
    print(f"[bank] GATE PASS — margin stamped ({BANK_MARGIN_STAMP})")
    return {"height": h, "gm01_deep": g01, "caller_deep": cal}


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
    if which == "soak":  # not in "all": a real-duration gate, invoked explicitly
        blocks = int(sys.argv[3]) if len(sys.argv) > 3 else 30
        soak_gate(c, n, blocks=blocks)
    if which == "bank":  # not in "all": run IMMEDIATELY BEFORE stopping to re-bank
        bank_gate(c)
    c.assert_poc_untouched("validate_fleet end")
    print("[validate] ALL REQUESTED GATES PASS; PoC untouched")
