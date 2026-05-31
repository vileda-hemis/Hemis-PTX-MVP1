"""Bootstrap a fresh ptx-bea chain: PoW→PoS transition, ProRegPL registration,
caller funding, and compound node_id capture for Phase 2 restart.

Verified bootstrap sequence (non-deadlocking, from pre-implementation analysis):
  - nCoinbaseMaturity=10, nStakeMinDepth=20 on CPTXBeaTestNetParams
  - Coins from blocks 1-30 have depth ≥20 at height 50 → stake-eligible
  - Mine 49 PoW blocks to gm01; PoS fires automatically at height 50
  - Chain self-extends; caller funded via sendmany split UTXOs

Two-phase ProRegPL bootstrap (required for PTXPAYOUT):
  Phase 1: fleet started with label-only node_ids (e.g. gm01)
  Registration: protx_register_fund captures compound node_id (gm01:abc12345)
  Phase 2: fleet restarted with compound node_ids in .env; tracker keys align
"""

import time
import json
from typing import Dict, Optional
from .cluster import Cluster
from .node import Node, RPCError

# Chain parameters for CPTXBeaTestNetParams
POW_BLOCKS = 49            # mine this many PoW blocks (PoS activates at 50)
POS_ACTIVATION = 50        # first PoS block height
COINBASE_MATURITY = 10
STAKE_MIN_DEPTH = 20
GM_COLLATERAL = 100        # HMS per GM
CALLER_SPLIT_AMOUNT = 2.0  # HMS per UTXO
CALLER_SPLIT_COUNT = 500   # UTXOs pre-split for rolling


def wait_for_height(node: Node, target: int, timeout: int = 300) -> int:
    return node.wait_for_height(target, timeout=timeout)


def mine_pow_blocks(gm01: Node, n: int = POW_BLOCKS) -> str:
    """Mine n PoW blocks to gm01's wallet. Returns the mining address."""
    addr = gm01.getnewaddress()
    print(f"[bootstrap] mining {n} PoW blocks to {addr}")
    t0 = time.time()
    gm01.generatetoaddress(n, addr)
    elapsed = time.time() - t0
    height = gm01.getblockcount()
    print(f"[bootstrap] {n} PoW blocks mined in {elapsed:.1f}s; height={height}")
    return addr


def wait_for_pos(gm01: Node, timeout: int = 120) -> int:
    """Wait for PoS to activate (height ≥ POS_ACTIVATION) and chain to self-extend."""
    print(f"[bootstrap] waiting for PoS activation at height {POS_ACTIVATION}")
    h = wait_for_height(gm01, POS_ACTIVATION, timeout=timeout)
    # Wait one more PoS block to confirm staking is working
    h = wait_for_height(gm01, POS_ACTIVATION + 1, timeout=timeout)
    print(f"[bootstrap] PoS active; height={h}")
    return h


def fund_caller(gm01: Node, caller: Node,
                split_count: int = CALLER_SPLIT_COUNT,
                amount_each: float = CALLER_SPLIT_AMOUNT) -> str:
    """Transfer split UTXOs from gm01 to caller wallet. Returns funding txid."""
    print(f"[bootstrap] funding caller with {split_count}×{amount_each} HMS UTXOs")

    # Generate split_count distinct addresses on caller
    print(f"[bootstrap] generating {split_count} receiving addresses on caller")
    amounts = {}
    for _ in range(split_count):
        addr = caller.getnewaddress()
        amounts[addr] = amount_each

    txid = gm01.sendmany("", amounts)
    print(f"[bootstrap] funding tx: {txid}")

    # Wait for 1 confirmation on BOTH gm01 (sender) and caller (receiver).
    # Waiting only on gm01 races the caller's block sync — caller.listunspent
    # returns 0 if the caller hasn't processed the confirmation block yet.
    height = gm01.getblockcount()
    wait_for_height(gm01, height + 1, timeout=120)
    wait_for_height(caller, height + 1, timeout=60)
    utxos = caller.listunspent(1)
    print(f"[bootstrap] caller funded: {len(utxos)} UTXOs ready")
    return txid


def _find_vout(gm01: Node, txid: str, addr: str, amount: float) -> int:
    """Return the vout index of the output paying `amount` to `addr` in txid."""
    raw = gm01.call("getrawtransaction", txid, True)
    for vout in raw["vout"]:
        addrs = vout.get("scriptPubKey", {}).get("addresses", [])
        if addr in addrs and abs(vout["value"] - amount) < 1e-6:
            return vout["n"]
    raise RuntimeError(f"collateral output {amount} HMS to {addr} not found in {txid}")


def register_gms(gm01: Node, cluster: Cluster, gm_labels: list) -> Dict[str, dict]:
    """Register each GM via external-collateral protx_register.

    The suffix = hash(collateralOutpoint) is load-bearing per KDD-033 — it
    fingerprints the specific collateral UTXO so compound ids are distinct
    even when labels collide. This requires a stable, external outpoint known
    before the registration tx is built. protx_register_fund (self-funded)
    creates a circular dependency (suffix → payload → txid → suffix) and was
    never the intended registration path for suffix computation.

    Flow:
      1. Fund each GM's collateral in a single sendmany tx (100 HMS each)
      2. Wait 1 block for confirmation (nGMCollateralMinConf=1)
      3. Resolve each outpoint (txid:n) from the funding tx
      4. protx_register against the real outpoint — suffix is meaningful
    """
    collateral_amt = float(GM_COLLATERAL)  # 100.0 HMS
    n_gms = len(gm_labels)

    # Step 1: generate collateral addresses + keys; fund all in one sendmany
    print(f"[bootstrap] funding {n_gms} collateral outputs ({collateral_amt} HMS each)")
    collateral_addrs = [gm01.getnewaddress() for _ in range(n_gms)]
    amounts = {addr: collateral_amt for addr in collateral_addrs}
    funding_txid = gm01.sendmany("", amounts)
    print(f"[bootstrap] collateral funding tx: {funding_txid}")

    # Step 2: wait for the funding tx to appear in the wallet's UTXO set.
    # wait_for_height ensures the block exists; the wallet rescan may lag slightly.
    # Poll getrawtransaction until confirmations >= 1 before locking (lockunspent
    # requires the UTXO to be in the wallet's confirmed coin set).
    current = gm01.getblockcount()
    wait_for_height(gm01, current + 1, timeout=120)

    def _funding_confirmed():
        try:
            return gm01.call("getrawtransaction", funding_txid, True).get("confirmations", 0) >= 1
        except Exception:
            return False

    gm01.wait_for_condition(_funding_confirmed, "collateral funding tx confirmed", timeout=60)

    # Step 2b: lock all collateral UTXOs so FundSpecialTx won't spend them as fees.
    # Each ProReg references (not spends) the collateral, but FundSpecialTx selects
    # fee inputs from all available wallet UTXOs. If it picks a future GM's collateral
    # UTXO, that GM's protx_register fails with "protx-dup" (mapNextTx already has it).
    # Unlock each one just before its registration (ProReg only references, not spends it).
    collateral_outpoints = []
    for coll_addr in collateral_addrs:
        vout_n = _find_vout(gm01, funding_txid, coll_addr, collateral_amt)
        collateral_outpoints.append({"txid": funding_txid, "vout": vout_n})
    # Hemis lockunspent: lockunspent unlock transparent [txs]
    # unlock=False → lock, transparent=True → transparent UTXOs
    gm01.call("lockunspent", False, True, collateral_outpoints)
    print(f"[bootstrap] locked {len(collateral_outpoints)} collateral UTXOs")

    # Step 3 + 4: register each GM
    results = {}
    for i, (gm, label, collateral_addr, outpoint) in enumerate(
            zip(cluster.gms, gm_labels, collateral_addrs, collateral_outpoints)):
        ip_port = f"172.30.0.{11 + i}:29994"

        vout_n = outpoint["vout"]
        # Collateral stays locked through the whole loop. lockunspent only affects
        # FundSpecialTx's coin selector — protx_register references the collateral
        # via explicit outpoint (pcoinsTip->GetUTXOCoin), unaffected by lock state.

        owner_addr    = gm01.getnewaddress()  # funded/registered from gm01
        voting_addr   = gm01.getnewaddress()
        payout_addr   = gm01.getnewaddress()
        # scriptPTXPayment on the GM's OWN wallet — each GM holds its own payout key.
        # This enables wallet-scoped RPC assertions (KDD-035): the winning GM's wallet
        # reflects the win; other GMs' wallets do not. Single-wallet registration
        # collapses this into gm01-holds-all, making wallet attribution untestable.
        ptx_pay_addr  = gm.getnewaddress()  # gm's own wallet, not gm01's

        bls = gm01.bls_generate()
        operator_pubkey = bls["public"]

        print(f"[bootstrap] registering {label} at {ip_port} "
              f"(collateral {funding_txid[:12]}:{vout_n}) ...")
        reg = gm01.call(
            "protx_register",
            funding_txid, vout_n, ip_port,
            owner_addr, operator_pubkey, voting_addr, payout_addr,
            0, "", ptx_pay_addr, label
        )

        # protx_register returns the txid directly (not a dict) unless ptxNodeId/payee present
        if isinstance(reg, dict):
            compound    = reg.get("ptxNodeId", label)
            protx_hash  = reg.get("txid", "")
        else:
            # plain string txid — no ptxNodeId echo (shouldn't happen when label is set)
            compound   = label
            protx_hash = str(reg)

        results[label] = {
            "compound_node_id": compound,
            "ptx_payment_addr": ptx_pay_addr,
            "protx_hash": protx_hash,
            "operator_pubkey": operator_pubkey,
        }
        print(f"[bootstrap] {label} → compound_id={compound} protx={protx_hash[:16]}...")

    return results


def wait_for_dgm_stability(gm01: Node, expected_count: int, timeout: int = 120) -> None:
    """Wait until all GMs appear in the DGM list at chain tip."""
    print(f"[bootstrap] waiting for {expected_count} GMs in DGM list")

    def check():
        try:
            lst = gm01.protx_list(detailed=False, valid_only=True)
            return len(lst) >= expected_count
        except RPCError:
            return False

    gm01.wait_for_condition(check, f"DGM list has {expected_count} entries", timeout=timeout)
    print(f"[bootstrap] DGM list stable with {expected_count} GMs")


def build_phase2_env(registration: Dict[str, dict]) -> dict:
    """Build the .env overrides for Phase 2 restart with compound node_ids."""
    env = {}
    for label, info in registration.items():
        env_key = label.upper().replace("-", "_") + "_NODE_ID"
        env[env_key] = info["compound_node_id"]
    return env


def bootstrap(cluster: Cluster,
              gm_labels: Optional[list] = None,
              phase1_warmup_blocks: int = 5) -> Dict[str, dict]:
    """Full two-phase bootstrap sequence.

    Phase 1: start → mine PoW → PoS → fund caller → register GMs
    Phase 2: restart with compound node_ids

    Returns registration dict (label → compound_id, payment_addr, etc.)
    """
    if gm_labels is None:
        gm_labels = [f"gm{i:02d}" for i in range(1, 12)]

    gm01 = cluster.gms[0]
    caller = cluster.caller

    # ── Phase 1 ────────────────────────────────────────────────────────────
    print("[bootstrap] === Phase 1: PoW mining + funding + ProRegPL ===")

    mine_pow_blocks(gm01, POW_BLOCKS)
    wait_for_pos(gm01)

    # Wait for all nodes to sync to PoS activation
    for node in cluster.all_nodes:
        wait_for_height(node, POS_ACTIVATION, timeout=60)

    # Let chain advance a few blocks to mature some coins before funding
    current = gm01.getblockcount()
    wait_for_height(gm01, current + phase1_warmup_blocks, timeout=120)

    fund_caller(gm01, caller)

    registration = register_gms(gm01, cluster, gm_labels)

    # Wait for all registration txs to confirm (1 block)
    current = gm01.getblockcount()
    wait_for_height(gm01, current + 1, timeout=60)
    wait_for_dgm_stability(gm01, len(gm_labels))

    # ── Phase 2 ────────────────────────────────────────────────────────────
    print("[bootstrap] === Phase 2: restart with compound node_ids ===")

    env_overrides = build_phase2_env(registration)
    print(f"[bootstrap] compound ids: {json.dumps(env_overrides, indent=2)}")

    # Tear down (keep volumes — chain state persists), rewrite env, restart
    cluster.down(volumes=False)
    cluster.up(env_overrides=env_overrides)
    cluster.wait_ready(timeout=120)

    # Wait for all nodes to resume at the chain tip
    tip = max(n.getblockcount() for n in cluster.all_nodes if n.is_rpc_ready())
    for node in cluster.all_nodes:
        wait_for_height(node, tip, timeout=60)

    # Wait for the caller to have at least 1 P2P peer (its addnode=gm01 connection).
    # wait_ready() checks RPC only. RelayTx broadcasts PTXSESS over P2P; if the
    # caller has no P2P peers when ptx_roll runs, the PTXSESS stays in the caller's
    # mempool only and never reaches the staking GMs — accumulator stays at 0.
    def caller_p2p_ready():
        try:
            return len(cluster.caller.call("getpeerinfo")) >= 1
        except Exception:
            return False

    cluster.caller.wait_for_condition(
        caller_p2p_ready, "caller has at least 1 P2P peer", timeout=60
    )
    print("[bootstrap] === bootstrap complete ===")
    return registration
