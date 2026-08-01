"""Bootstrap a fresh ptx-w2 chain at N GMs — RECONCILED to HEAD (W2.0a Amendment 1).

Descends from testnet/harness/bootstrap.py (the 11-GM ptx-bea recipe). The
registration CONSENSUS surface is byte-identical 801c557->40b109c (source-diff:
rpcevo.cpp, providertx.*, deterministicgms.*, wallet.cpp all unchanged; the
only specialtx_validation delta is the additive PTXDKG validator). What HEAD
adds is on the CONSUMER side, and it is why this file is a reconcile, not a
copy:

  1. KDD-060 eligibility (PTX_DKG_IsGMPTXEligible, ptx_dkg.cpp:109) requires a
     non-empty node_id — provided here as before via the ProRegPL label.
  2. CalculateScores (deterministicgms.cpp:251) SILENTLY SKIPS any GM whose
     confirmedHash is null, and PTX_DKG_InitSession hard-asserts it. A GM is
     confirmed only once its registration is nGMCollateralMinConf=1 deep,
     applied at the NEXT block (deterministicgms.cpp:597-600) — i.e. eligible
     from tip >= registeredHeight + 2. The old recipe waited one block after
     registration: registered, yes; selectable, NOT guaranteed. This flow
     waits for confirmation explicitly (wait_for_gm_confirmation) — the
     "looks registered but fails eligibility" failure Amendment 1 targets.
  3. Registration is paced in batches of K with a one-block wait between
     batches (N=60 in a single mempool window exercises wallet unconfirmed-
     chain limits the 11-GM recipe never saw).

Everything load-bearing in the original is preserved verbatim in spirit and
cited to its origin: external-collateral protx_register with the KDD-033
suffix (bootstrap.py:98-109 rationale), the lockunspent guard against
FundSpecialTx eating future collateral (:136-148), per-GM own-wallet
scriptPTXPayment for wallet attribution (:164-168, KDD-035), the two-phase
compound-id restart (:227-296).
"""

import time
import json
from typing import Dict, List, Optional
from .cluster import W2Cluster
from .node import Node, RPCError

# CPTXBeaTestNetParams (chainparams.cpp:848-930) — unchanged 801c557->HEAD
POW_BLOCKS = 49            # PoS activates at 50
POS_ACTIVATION = 50
GM_COLLATERAL = 100        # HMS (nGMCollateralAmt, chainparams.cpp:872)
GM_MIN_CONF = 1            # nGMCollateralMinConf (chainparams.cpp:875)
REG_BATCH = 10             # confirm-every-K registration pacing (W2.0a plan §5)
CALLER_SPLIT_AMOUNT = 2.0
CALLER_SPLIT_COUNT = 500


def wait_for_height(node: Node, target: int, timeout: int = 900) -> int:
    return node.wait_for_height(target, timeout=timeout)


def mine_pow_blocks(gm01: Node, n: int = POW_BLOCKS) -> str:
    addr = gm01.getnewaddress()
    print(f"[bootstrap] mining {n} PoW blocks to {addr}")
    t0 = time.time()
    gm01.generatetoaddress(n, addr)
    print(f"[bootstrap] {n} PoW blocks in {time.time()-t0:.1f}s; "
          f"height={gm01.getblockcount()}")
    return addr


def wait_for_pos(gm01: Node, timeout: int = 900) -> int:
    print(f"[bootstrap] waiting for PoS activation at {POS_ACTIVATION}")
    wait_for_height(gm01, POS_ACTIVATION, timeout=timeout)
    h = wait_for_height(gm01, POS_ACTIVATION + 1, timeout=timeout)
    print(f"[bootstrap] PoS active; height={h}")
    return h


def fund_caller(gm01: Node, caller: Node,
                split_count: int = CALLER_SPLIT_COUNT,
                amount_each: float = CALLER_SPLIT_AMOUNT) -> str:
    print(f"[bootstrap] funding caller with {split_count}x{amount_each} HMS UTXOs")
    amounts = {}
    for _ in range(split_count):
        amounts[caller.getnewaddress()] = amount_each
    txid = gm01.sendmany("", amounts)
    height = gm01.getblockcount()
    wait_for_height(gm01, height + 1, timeout=900)
    wait_for_height(caller, height + 1, timeout=120)
    print(f"[bootstrap] caller funded: {len(caller.listunspent(1))} UTXOs")
    return txid


def _find_vout(gm01: Node, txid: str, addr: str, amount: float) -> int:
    raw = gm01.call("getrawtransaction", txid, True)
    for vout in raw["vout"]:
        addrs = vout.get("scriptPubKey", {}).get("addresses", [])
        if addr in addrs and abs(vout["value"] - amount) < 1e-6:
            return vout["n"]
    raise RuntimeError(f"collateral output {amount} to {addr} not in {txid}")


def register_gms(gm01: Node, cluster: W2Cluster,
                 gm_labels: List[str]) -> Dict[str, dict]:
    """External-collateral protx_register for each GM, batched.

    KDD-033: suffix = hash(collateralOutpoint) fingerprints the specific
    collateral UTXO; requires a stable external outpoint known before the
    registration tx is built (self-funded registration is circular — see
    origin bootstrap.py:98-109).
    """
    collateral_amt = float(GM_COLLATERAL)
    n_gms = len(gm_labels)

    print(f"[bootstrap] funding {n_gms} collateral outputs ({collateral_amt} HMS each)")
    collateral_addrs = [gm01.getnewaddress() for _ in range(n_gms)]
    funding_txid = gm01.sendmany("", {a: collateral_amt for a in collateral_addrs})
    print(f"[bootstrap] collateral funding tx: {funding_txid}")

    # ★ LOCK FIRST, CONFIRM SECOND (2026-07-30).  These outputs become stakeable
    # at nStakeMinDepth (20 blocks on ptxbea) and gm01 IS the staker, so every
    # block between funding and locking is a chance for the staker to eat a
    # future collateral.  Locking after the confirmation wait means winning a
    # >20-block race; under load (a concurrent build starving RPC) that race was
    # LOST — the staker consumed 5 of 98 collaterals at h189-h205 and
    # lockunspent then failed with "expected unspent output", killing the
    # bootstrap.  The outputs are in the wallet the moment sendmany returns, so
    # lock them immediately: the window closes to zero instead of being raced.
    # Same family as BUG-019 R1 — anything not yet locked is fair game.
    collateral_outpoints = []
    for coll_addr in collateral_addrs:
        vout_n = _find_vout(gm01, funding_txid, coll_addr, collateral_amt)
        collateral_outpoints.append({"txid": funding_txid, "vout": vout_n})
    gm01.call("lockunspent", False, True, collateral_outpoints)
    print(f"[bootstrap] locked {len(collateral_outpoints)} collateral UTXOs "
          f"(pre-confirmation — closes the staker race)")

    current = gm01.getblockcount()
    wait_for_height(gm01, current + 1, timeout=900)

    def _funding_confirmed():
        try:
            return gm01.call("getrawtransaction", funding_txid, True).get(
                "confirmations", 0) >= 1
        except Exception:
            return False
    gm01.wait_for_condition(_funding_confirmed, "collateral funding confirmed",
                            timeout=120)

    # Re-assert after confirmation: locks are in-memory only, and a restart or
    # wallet reload between here and registration would drop them.
    # ★ lockunspent ERRORS with "output already locked" if ANY outpoint in the
    # batch is already held, so re-locking the whole set unconditionally fails
    # on the happy path.  Lock only what is actually missing — the same idiom
    # relock_collaterals() uses (cluster.py) — then assert the full set.
    locked_now = {(l["txid"], l["vout"])
                  for l in gm01.call("listlockunspent")["transparent"]}
    todo = [o for o in collateral_outpoints
            if (o["txid"], o["vout"]) not in locked_now]
    if todo:
        gm01.call("lockunspent", False, True, todo)
        print(f"[bootstrap] re-locked {len(todo)} collateral(s) dropped since funding")
    locked_now = {(l["txid"], l["vout"])
                  for l in gm01.call("listlockunspent")["transparent"]}
    missing = [o for o in collateral_outpoints
               if (o["txid"], o["vout"]) not in locked_now]
    if missing:
        raise AssertionError(
            f"collateral lock incomplete: {len(missing)}/{len(collateral_outpoints)} "
            f"not in listlockunspent — the staker may have consumed them")

    results = {}
    for i, (gm, label, outpoint) in enumerate(
            zip(cluster.gms, gm_labels, collateral_outpoints)):
        ip_port = f"{cluster.subnet_base}.{11 + i}:29994"
        owner_addr  = gm01.getnewaddress()
        voting_addr = gm01.getnewaddress()
        payout_addr = gm01.getnewaddress()
        # scriptPTXPayment on the GM's OWN wallet (KDD-035 wallet attribution;
        # origin :164-168): the winning GM's wallet reflects the win.
        ptx_pay_addr = gm.getnewaddress()

        bls = gm01.bls_generate()
        print(f"[bootstrap] registering {label} at {ip_port} "
              f"({i+1}/{n_gms}, collateral {funding_txid[:12]}:{outpoint['vout']})")
        reg = gm01.call(
            "protx_register",
            funding_txid, outpoint["vout"], ip_port,
            owner_addr, bls["public"], voting_addr, payout_addr,
            0, "", ptx_pay_addr, label
        )
        if isinstance(reg, dict):
            compound   = reg.get("ptxNodeId", label)
            protx_hash = reg.get("txid", "")
        else:
            compound   = label
            protx_hash = str(reg)
        results[label] = {
            "compound_node_id": compound,
            "ptx_payment_addr": ptx_pay_addr,
            "protx_hash": protx_hash,
            "operator_pubkey": bls["public"],
            "operator_secret": bls["secret"],
        }
        print(f"[bootstrap] {label} -> {compound} protx={protx_hash[:16]}...")

        # Amendment-1 pacing: flush the mempool every REG_BATCH registrations
        # so N=60 never stacks the full set as one unconfirmed chain.
        if (i + 1) % REG_BATCH == 0 and (i + 1) < n_gms:
            h = gm01.getblockcount()
            print(f"[bootstrap] batch of {REG_BATCH} done — waiting 1 block")
            wait_for_height(gm01, h + 1, timeout=900)

    return results


def wait_for_dgm_stability(gm01: Node, expected: int, timeout: int = 900) -> None:
    print(f"[bootstrap] waiting for {expected} GMs in DGM list")

    def check():
        try:
            return len(gm01.protx_list(detailed=False, valid_only=True)) >= expected
        except RPCError:
            return False
    gm01.wait_for_condition(check, f"DGM list has {expected}", timeout=timeout)


def wait_for_gm_confirmation(gm01: Node, expected: int, timeout: int = 900) -> None:
    """Amendment 1 (the reconcile's core): wait until EVERY GM is confirmed —
    tip >= registeredHeight + GM_MIN_CONF + 1, the point confirmedHash is set
    (deterministicgms.cpp:597-600) and the GM becomes visible to
    CalculateScores/KDD-060 selection. Registration alone is NOT eligibility."""
    print(f"[bootstrap] waiting for confirmedHash on all {expected} GMs")

    def check():
        try:
            lst = gm01.protx_list(detailed=True, valid_only=True)
            if len(lst) < expected:
                return False
            tip = gm01.getblockcount()
            return all(
                tip >= e["dgmstate"]["registeredHeight"] + GM_MIN_CONF + 1
                for e in lst)
        except (RPCError, KeyError):
            return False
    gm01.wait_for_condition(check, "all GMs confirmation-deep", timeout=timeout)
    print(f"[bootstrap] all {expected} GMs confirmation-deep at "
          f"tip={gm01.getblockcount()}")


def assert_all_eligible(gm01: Node, expected: int) -> None:
    """KDD-060 leg check from exposed state: every registered GM carries a
    compound node_id (ptxNodeId 'label:8hex'). The end-to-end selection proof
    (validate_fleet.eligibility_gate) complements this via the real V5 core."""
    lst = gm01.protx_list(detailed=True, valid_only=True)
    assert len(lst) == expected, f"DGM list {len(lst)} != {expected}"
    bad = []
    for e in lst:
        nid = e.get("dgmstate", {}).get("ptxNodeId", "")
        if ":" not in nid or len(nid.split(":")[1]) != 8:
            bad.append((e.get("proTxHash", "?")[:12], nid))
    assert not bad, f"GMs without compound node_id: {bad}"
    print(f"[bootstrap] all {expected} GMs carry compound ptxNodeId")


def build_phase2_env(registration: Dict[str, dict]) -> dict:
    env = {}
    for label, info in registration.items():
        env[label.upper().replace("-", "_") + "_NODE_ID"] = info["compound_node_id"]
    return env


def rewrite_env(cluster: W2Cluster, env_overrides: dict, env_path: str):
    lines = [f"RPCUSER={cluster.rpc_user}", f"RPCPASSWORD={cluster.rpc_pass}",
             "CALLER_NODE_ID=caller"]
    for k, v in sorted(env_overrides.items()):
        lines.append(f"{k}={v}")
    with open(env_path, "w") as f:
        f.write("\n".join(lines) + "\n")


def bootstrap(cluster: W2Cluster,
              env_path: str,
              gm_labels: Optional[List[str]] = None,
              phase1_warmup_blocks: int = 5,
              fund_caller_utxos: int = CALLER_SPLIT_COUNT) -> Dict[str, dict]:
    """Full two-phase bootstrap at N = cluster.n."""
    if gm_labels is None:
        gm_labels = [f"gm{i:02d}" for i in range(1, cluster.n + 1)]
    assert len(gm_labels) == cluster.n

    gm01 = cluster.gms[0]
    caller = cluster.caller

    print(f"[bootstrap] === Phase 1 (N={cluster.n}): PoW + funding + ProRegPL ===")
    mine_pow_blocks(gm01, POW_BLOCKS)
    wait_for_pos(gm01)
    for node in cluster.all_nodes:
        wait_for_height(node, POS_ACTIVATION, timeout=300)

    current = gm01.getblockcount()
    wait_for_height(gm01, current + phase1_warmup_blocks, timeout=4500)

    if fund_caller_utxos:
        fund_caller(gm01, caller, split_count=fund_caller_utxos)

    registration = register_gms(gm01, cluster, gm_labels)

    current = gm01.getblockcount()
    wait_for_height(gm01, current + 1, timeout=900)
    wait_for_dgm_stability(gm01, len(gm_labels))
    wait_for_gm_confirmation(gm01, len(gm_labels))   # Amendment 1
    assert_all_eligible(gm01, len(gm_labels))        # Amendment 1

    print("[bootstrap] === Phase 2: restart with compound node_ids ===")
    env_overrides = build_phase2_env(registration)
    print(f"[bootstrap] compound ids: {json.dumps(env_overrides, indent=1)}")
    rewrite_env(cluster, env_overrides, env_path)
    cluster.restart_with_env()
    cluster.wait_ready()

    tip = max(nd.getblockcount() for nd in cluster.all_nodes if nd.is_rpc_ready())
    for node in cluster.all_nodes:
        wait_for_height(node, tip, timeout=300)

    def caller_p2p_ready():
        try:
            return len(cluster.caller.call("getpeerinfo")) >= 1
        except Exception:
            return False
    cluster.caller.wait_for_condition(caller_p2p_ready,
                                      "caller has >=1 P2P peer", timeout=120)
    print("[bootstrap] === bootstrap complete ===")
    return registration
