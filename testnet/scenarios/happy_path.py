"""Happy-path end-to-end scenario for the ptx-bea testnet.

Pipeline exercised:
  PTXSESS submission (5 rolls) → PTXCOALESCE accumulation → settlement
  boundary → PTXPAYOUT → winner selection → pose tracker propagation →
  wallet RPC reflection → cross-node consensus agreement

Run:
    python3 -m testnet.scenarios.happy_path [--compose /path/to/compose.yml]
"""

import sys
import os
import argparse
import time

# Allow running as a module from the repo root
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from testnet.harness.cluster import Cluster
from testnet.harness.bootstrap import bootstrap as do_bootstrap
from testnet.harness.runner import ScenarioRunner
from testnet.harness.node import Node, RPCError

# Chain parameters (must match CPTXBeaTestNetParams)
PTX_SERVICE_FEE_SAT = 100_000_000   # 1 HMS in satoshi
PTX_SETTLEMENT_WINDOW = 5            # nPTXSettlementWindow
N_ROLLS = 5


def next_settlement_boundary(current_height: int, window: int = PTX_SETTLEMENT_WINDOW) -> int:
    """Return the next height where height % window == 0."""
    return current_height + (window - current_height % window)


def _find_special_tx_in_block(node: Node, height: int, ntype: int) -> dict | None:
    """Return the first tx with the given nType in the block at height, or None.

    Uses getblock(verbose=1) for txid list then getrawtransaction for each tx —
    getblock verbose=2 does not include nType in the Hemis RPC, but
    getrawtransaction always does.
    """
    try:
        bh = node.getblockhash(height)
        block = node.getblock(bh, 1)  # verbose=1 → txid list only
        for txid in block.get("tx", []):
            if isinstance(txid, str):
                raw = node.call("getrawtransaction", txid, True)
                if raw.get("type") == ntype:
                    return raw
    except Exception:
        pass
    return None


def find_ptxpayout_in_block(caller: Node, height: int) -> dict | None:
    return _find_special_tx_in_block(caller, height, 10)


def find_ptxcoalesce_in_block(caller: Node, height: int) -> dict | None:
    return _find_special_tx_in_block(caller, height, 9)


def wait_for_settlement(caller: Node, current_height: int,
                        window: int = PTX_SETTLEMENT_WINDOW,
                        timeout: int = 180,
                        gm01: "Node | None" = None) -> int:
    """Wait until the chain reaches the next settlement boundary.

    Polls both caller and gm01 (diagnostic). If gm01 advances past the target
    but the caller doesn't, it's a P2P sync gap — the scenario waits for the
    caller to catch up separately rather than timing out.

    This function is falsification target 3b: replacing it with an
    immediate return causes the PTXPAYOUT-present assertion to fire.
    """
    import time
    target = next_settlement_boundary(current_height, window)
    print(f"[scenario] waiting for settlement boundary at height {target} "
          f"(current={current_height})")
    deadline = time.time() + timeout
    while time.time() < deadline:
        caller_h = caller.getblockcount()
        if caller_h >= target:
            return caller_h
        if gm01 is not None:
            gm01_h = gm01.getblockcount()
            caller_peers = len(caller.call("getpeerinfo"))
            print(f"[scenario]   caller h={caller_h} peers={caller_peers}  "
                  f"gm01 h={gm01_h}  target={target}")
            if gm01_h >= target:
                # gm01 produced the block; give caller time to sync
                return caller.wait_for_height(target, timeout=60)
        time.sleep(5)
    raise TimeoutError(
        f"{caller.name}: settlement target {target} not reached "
        f"(stuck at {caller.getblockcount()}) after {timeout}s"
    )


def run_happy_path(runner: ScenarioRunner) -> None:
    cluster = runner.cluster
    caller = cluster.caller
    gm01 = cluster.gms[0]

    # ── Step 1: bootstrap ─────────────────────────────────────────────────
    print("[scenario] === Step 1: bootstrap ===")
    cluster.up()
    cluster.wait_ready(timeout=120)
    runner.checkpoint("fleet up")

    registration = do_bootstrap(cluster)
    runner.checkpoint("bootstrap complete")

    # Verify old fleet untouched
    cluster.assert_old_fleet_untouched()
    runner.checkpoint("old fleet verified untouched")

    # ── Step 2: submit 5 PTXSESS rolls ───────────────────────────────────
    print(f"[scenario] === Step 2: submit {N_ROLLS} ptx_roll calls ===")
    tx_ids = []
    for i in range(N_ROLLS):
        result = caller.ptx_roll(1, 1, 100, game_id=f"happy-path-{i+1}", salt=f"0{i}aabbcc")
        tx_id = result["tx_id"]
        runner.assert_true(
            len(tx_id) == 64,
            f"roll {i+1}: tx_id should be 64-char hex, got {tx_id!r}"
        )
        tx_ids.append(tx_id)
        print(f"[scenario] roll {i+1}: tx_id={tx_id[:16]}... result={result['results']}")
    runner.checkpoint(f"{N_ROLLS} PTXSESS submitted")

    # ── Step 3: wait for PTXCOALESCE via accumulator poll ────────────────
    # Don't scan a fixed block range: fast-staking GMs may produce several
    # blocks before PTXSESS txs propagate from caller via P2P. Instead, poll
    # ptx_lottery_status.pool_balance_sat until the accumulator reflects the
    # 5 rolls, then find the PTXCOALESCE block containing them.
    print("[scenario] === Step 3: wait for PTXCOALESCE ===")

    # FALSIFICATION TARGET 3a: change N_ROLLS to N_ROLLS-1 at BOTH sites below
    # (the wait condition AND the assertion). Changing only the assertion leaves the
    # wait blocking on the full N_ROLLS amount and the failure is trivial/non-informative.
    # Both must be changed together to prove the harness catches an undershoot.
    roll_height = caller.getblockcount()  # snapshot before wait

    def accumulator_ready():
        try:
            s = caller.ptx_lottery_status()
            return s["pool_balance_sat"] >= N_ROLLS * PTX_SERVICE_FEE_SAT  # 3a site 1
        except Exception:
            return False

    caller.wait_for_condition(accumulator_ready, "accumulator reflects 5 rolls", timeout=180)
    status = caller.ptx_lottery_status()
    pool_sat = status["pool_balance_sat"]

    # Find the PTXCOALESCE that captured the 5 rolls: scan forward from roll_height
    # (not back from tip) to find the first coalesce at or after the rolls confirmed.
    # Scanning back from tip risks landing on a later empty coalesce when settlement
    # window=5 means multiple PTXCOALESCE blocks may exist in a 20-block window.
    tip = caller.getblockcount()
    coalesce_tx = None
    coalesce_height = None
    for h in range(roll_height, min(tip + 1, roll_height + 30)):
        t = find_ptxcoalesce_in_block(caller, h)
        if t:
            coalesce_tx, coalesce_height = t, h
            break

    runner.assert_true(
        coalesce_tx is not None,
        f"PTXCOALESCE not found in blocks {roll_height}..{roll_height+30}"
    )
    print(f"[scenario] PTXCOALESCE found at height {coalesce_height}: "
          f"txid={coalesce_tx['txid'][:16]}...")

    # FALSIFICATION TARGET 3a: change N_ROLLS to N_ROLLS-1 here to verify
    # the harness reads live chain state, not a hardcoded value.
    runner.assert_equal(
        pool_sat,
        N_ROLLS * PTX_SERVICE_FEE_SAT,
        f"pool_balance_sat after {N_ROLLS} rolls"
    )
    print(f"[scenario] accumulator = {pool_sat} sat = "
          f"{pool_sat / 1e8:.2f} HMS ✓")
    runner.checkpoint("PTXCOALESCE verified, accumulator correct")

    # ── Step 4: wait for settlement boundary and verify PTXPAYOUT ────────
    print("[scenario] === Step 4: wait for settlement + PTXPAYOUT ===")
    current = caller.getblockcount()

    # FALSIFICATION TARGET 3b: replacing this call with `pass` causes the
    # PTXPAYOUT assertion below to fail — proves wait_for_settlement is not
    # a no-op and the PTXPAYOUT check isn't trivially satisfied.
    settlement_height = wait_for_settlement(caller, current, gm01=gm01)

    payout_tx = find_ptxpayout_in_block(caller, settlement_height)
    runner.assert_true(
        payout_tx is not None,
        f"PTXPAYOUT not found at settlement boundary height {settlement_height}"
    )
    print(f"[scenario] PTXPAYOUT found at height {settlement_height}: "
          f"txid={payout_tx['txid'][:16]}...")

    # Winner must be one of the 11 registered GMs
    winner_script = payout_tx["vout"][0]["scriptPubKey"]["addresses"][0]
    registered_pay_addrs = {
        info["ptx_payment_addr"] for info in registration.values()
    }
    runner.assert_true(
        winner_script in registered_pay_addrs,
        f"PTXPAYOUT recipient {winner_script} not in registered GM payment addresses"
    )
    print(f"[scenario] winner payment address: {winner_script} ✓")

    # Payout amount == accumulator value - miner fee (exact).
    # Use round() to avoid float-precision issues converting HMS→sat.
    payout_sat = round(payout_tx["vout"][0]["value"] * 100_000_000)
    expected_payout = N_ROLLS * PTX_SERVICE_FEE_SAT - 10_000  # nPTXPayoutMinerFee=10000 sat
    runner.assert_equal(
        payout_sat, expected_payout,
        "PTXPAYOUT amount == accumulator − nPTXPayoutMinerFee"
    )
    print(f"[scenario] payout amount = {payout_sat} sat = {payout_sat/1e8:.4f} HMS ✓")
    runner.checkpoint("PTXPAYOUT verified at settlement boundary")

    # After payout the accumulator resets
    status_post = caller.ptx_lottery_status()
    runner.assert_equal(
        status_post["pool_balance_sat"], 0,
        "pool_balance_sat should be 0 after PTXPAYOUT"
    )

    # ── Step 5: cross-node consensus agreement ────────────────────────────
    print("[scenario] === Step 5: cross-node consensus agreement ===")

    # All 11 nodes should report identical lottery state
    settlement_tip = caller.getblockcount()
    for node in cluster.gms:
        node.wait_for_height(settlement_tip, timeout=60)

    lottery_states = {}
    for node in cluster.all_nodes:
        s = node.ptx_lottery_status()
        # After payout: pool=0, settlement_history has one entry
        lottery_states[node.name] = (
            s["pool_balance_sat"],
            len(s.get("settlement_history", [])),
        )

    runner.assert_all_agree(
        lottery_states,
        "all nodes agree on lottery state (pool_balance_sat, settlement_history length)"
    )
    # Cross-node winner agreement: all nodes must agree on the winner address.
    # This assertion is the direct proof that the pose-tracker consensus fix (718ab98)
    # works — the fix ensures all validators compute the same PTX_SelectWinner result,
    # so the winning GM's payout address is identical in every node's settlement_history.
    winner_addrs = {}
    for node in cluster.all_nodes:
        s = node.ptx_lottery_status()
        history = s.get("settlement_history", [])
        winner_addrs[node.name] = history[0].get("gm", "") if history else ""
    runner.assert_all_agree(
        winner_addrs,
        "all nodes agree on winner GM address"
    )
    print(f"[scenario] all {len(cluster.all_nodes)} nodes agree on lottery state ✓")
    print(f"[scenario] winner address consensus confirmed across all 12 nodes ✓")
    runner.checkpoint("cross-node lottery state agreement")

    # ── Step 6: pose tracker consistency ─────────────────────────────────
    print("[scenario] === Step 6: pose tracker consistency ===")
    pose_data = {}
    for node in cluster.all_nodes:
        records = node.ptx_pose_status()
        # Key: sorted list of (node_id, eligible) pairs
        pose_data[node.name] = tuple(
            sorted((r["node_id"], r["eligible"]) for r in records)
        )
    runner.assert_all_agree(
        pose_data,
        "all nodes agree on pose tracker (node_ids + eligibility)"
    )
    print(f"[scenario] pose tracker consistent across all {len(cluster.all_nodes)} nodes ✓")
    runner.checkpoint("pose tracker consistency verified")

    # ── Step 7: settlement_history RPC reflection ─────────────────────────
    print("[scenario] === Step 7: settlement_history reflection ===")
    history = caller.ptx_lottery_status().get("settlement_history", [])
    runner.assert_true(len(history) >= 1, "settlement_history should have ≥1 entry")
    last = history[0]  # newest-first
    runner.assert_true("height" in last and last["height"] == settlement_height,
                       f"settlement_history[0].height should be {settlement_height}")
    runner.assert_true("txid" in last and len(last["txid"]) == 64,
                       "settlement_history[0].txid should be 64-char hex")
    print(f"[scenario] settlement_history entry: height={last['height']} "
          f"txid={last['txid'][:16]}... ✓")
    runner.checkpoint("settlement_history RPC verified")

    # ── Step 8: wallet-RPC attribution (KDD-035) ─────────────────────────
    # Each GM's scriptPTXPayment was generated on that GM's own node's wallet
    # (bootstrap uses gm.getnewaddress(), not gm01.getnewaddress()). This is the
    # distributed-key topology required for KDD-035 validation:
    #   - The winning GM's wallet reflects the win (its wallet holds the winner key)
    #   - A non-winning GM's wallet does NOT reflect that win (different key)
    print("[scenario] === Step 8: wallet-RPC attribution (KDD-035) ===")

    # Identify the winning GM by name, not zip-index.
    # Match winner_script against each label's ptx_payment_addr, then find the
    # node whose name matches the winning label. Name-based, not position-based —
    # zip-index would silently query the wrong wallet if fleet order and label
    # sort order ever diverged.
    winning_gm_label = None
    for label, info in registration.items():
        if info["ptx_payment_addr"] == winner_script:
            winning_gm_label = label
            break
    runner.assert_true(
        winning_gm_label is not None,
        f"Could not identify winning GM for address {winner_script}"
    )
    winning_gm_node = next(
        gm for gm in cluster.gms if gm.name == winning_gm_label
    )
    print(f"[scenario] winning GM: {winning_gm_label} (node {winning_gm_node.name})")

    # Wait for the winning GM to sync the settlement block.
    winning_gm_node.wait_for_height(settlement_height, timeout=60)

    # Winning GM's ptx_lottery_history must show an entry at settlement_height.
    # Step 12 spec: ptx_lottery_history is newest-first — but match by height
    # rather than assuming [0] in case of prior wins from a multi-settlement run.
    winner_history = winning_gm_node.ptx_lottery_history()
    win_at_height = next(
        (e for e in winner_history if e.get("height") == settlement_height), None
    )
    runner.assert_true(
        win_at_height is not None,
        f"{winning_gm_label}: expected ptx_lottery_history entry at height {settlement_height}"
    )
    print(f"[scenario] {winning_gm_label} wallet reflects win: "
          f"height={win_at_height['height']} ✓")

    # A non-winning GM's wallet must NOT have an entry at settlement_height.
    # KDD-035 claim: "you don't see other GMs' wins" — NOT "you have no wins ever."
    # Asserting globally-empty would false-fail in multi-settlement scenarios.
    non_winner_node = next(gm for gm in cluster.gms if gm.name != winning_gm_node.name)
    non_winner_node.wait_for_height(settlement_height, timeout=60)
    non_winner_history = non_winner_node.ptx_lottery_history()
    non_winner_entry = next(
        (e for e in non_winner_history if e.get("height") == settlement_height), None
    )
    runner.assert_true(
        non_winner_entry is None,
        f"{non_winner_node.name}: should NOT have ptx_lottery_history entry "
        f"at height {settlement_height} (doesn't hold winner's payout key)"
    )
    print(f"[scenario] {non_winner_node.name} wallet correctly absent from this win ✓")
    runner.checkpoint("wallet-RPC attribution (KDD-035) verified")

    print("[scenario] === HAPPY PATH COMPLETE ===")


def main():
    parser = argparse.ArgumentParser(description="Run the ptx-bea happy-path scenario")
    parser.add_argument(
        "--compose",
        default=None,
        help="Path to docker-compose.yml (default: docker-bea/docker-compose.yml)"
    )
    args = parser.parse_args()

    cluster_kwargs = {}
    if args.compose:
        cluster_kwargs["compose_file"] = args.compose

    cluster = Cluster(**cluster_kwargs)
    runner = ScenarioRunner(cluster)
    runner.run(run_happy_path)


if __name__ == "__main__":
    main()
