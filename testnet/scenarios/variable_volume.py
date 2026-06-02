"""Variable-volume end-to-end scenario for the ptx-bea testnet.

Pipeline exercised beyond happy_path:
  - PTXCOALESCE tx-size scaling: accumulator stays 1 UTXO but PTXCOALESCE vin
    grows one input per PTXSESS in the block.  High-volume blocks stress
    PTXCOALESCE tx size, bounded by block size, not extraPayload (empty).
  - Empty-block carry-through: zero-PTXSESS blocks produce no PTXCOALESCE;
    accumulator carries through unchanged.  Exercised by zero-burst slots with
    ≥70s sleep in the schedule.
  - Exact PTXPAYOUT amount asserted against on-chain confirmed PTXSESS count,
    not the optimistic submitted count.  confirmed_rolls ≠ total_submitted
    diagnoses a throughput/timing issue, not a settlement bug.

Matches design doc §12 burst-load case.

Run:
    python3 -m testnet.scenarios.variable_volume [--compose /path/to/compose.yml]
    SCENARIO_SEED=<int> python3 -m testnet.scenarios.variable_volume [...]
"""

import sys
import os
import math
import random
import argparse
import time

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from testnet.harness.cluster import Cluster
from testnet.harness.bootstrap import bootstrap as do_bootstrap
from testnet.harness.runner import ScenarioRunner
from testnet.harness.node import Node, RPCError

from testnet.scenarios.happy_path import (
    next_settlement_boundary,
    find_ptxpayout_in_block,
    count_ptxsess_in_block,
    find_all_ptxcoalesce_in_window,
    wait_for_settlement,
    PTX_SERVICE_FEE_SAT,
    PTX_SETTLEMENT_WINDOW,
)

# Chain / consensus constants — must match CPTXBeaTestNetParams and consensus.h.
PTX_PAYOUT_MINER_FEE = 10_000   # nPTXPayoutMinerFee in satoshi
MAX_BLOCK_SIZE = 2_000_000      # MAX_BLOCK_SIZE_CURRENT (consensus/consensus.h)

V4_ACTIVATION_HEIGHT = 265

# Throughput probe
N_PROBE = 20    # rolls submitted for the throughput-measurement block
MIN_CAP = 3     # minimum acceptable per-block throughput; below this, abort fast

# Variable schedule
N_SLOTS = 20    # number of (burst, sleep) entries
DEFAULT_SEED = 20260601


# ── Schedule generation ────────────────────────────────────────────────────────

def generate_schedule(rng: random.Random, observed_cap: int) -> list[tuple[int, float]]:
    """Return list of (burst_size, sleep_seconds) pairs (wall-clock cadenced).

    Three entry types:
      zero-burst (20%): burst=0, sleep=70s — guarantees carry-through if the
          slot spans a full 60s block boundary with no concurrent submissions.
      light      (20%): burst 1-5, sleep=8s
      burst      (60%): burst cap//4 .. cap-1, sleep=8s — stresses PTXCOALESCE vin
    """
    schedule = []
    for _ in range(N_SLOTS):
        r = rng.random()
        if r < 0.20:
            schedule.append((0, 70.0))
        elif r < 0.40:
            schedule.append((rng.randint(1, 5), 8.0))
        else:
            lo = max(1, observed_cap // 4)
            hi = max(lo + 1, observed_cap - 1)
            schedule.append((rng.randint(lo, hi), 8.0))
    return schedule


def validate_schedule(schedule: list[tuple[int, float]], observed_cap: int) -> bool:
    """Return True iff schedule has ≥1 zero-block slot and ≥1 high-volume slot."""
    has_zero = any(size == 0 and sleep >= 60.0 for size, sleep in schedule)
    has_high = any(size >= max(1, observed_cap // 4) for size, _ in schedule)
    return has_zero and has_high


# ── Scenario ───────────────────────────────────────────────────────────────────

def run_variable_volume(runner: ScenarioRunner) -> None:
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

    cluster.assert_old_fleet_untouched()
    runner.checkpoint("old fleet verified untouched")

    # ── Step 1b: warm chain past UPGRADE_V4_0 (height 265) ────────────────
    # Rolls must be submitted post-V4_0 so the settlement boundary lands in the
    # clean 60s zone.  Same rationale and timeout as happy_path Step 1b.
    WARM_TARGET = V4_ACTIVATION_HEIGHT + 1  # 266
    h_now = gm01.getblockcount()
    if h_now < WARM_TARGET:
        print(f"[scenario] === Step 1b: warming to height {WARM_TARGET} "
              f"(current={h_now}) ===")
        gm01.wait_for_height(WARM_TARGET, timeout=12_000)
    print(f"[scenario] chain at height {gm01.getblockcount()} — post-V4_0 ✓")
    runner.checkpoint(f"chain warmed past V4_0 (height {WARM_TARGET})")

    # ── Step 1c: throughput probe ─────────────────────────────────────────
    # window_start is set HERE, before probe submissions, so probe PTXSESS fall
    # inside the reconciliation window.  PTXPAYOUT sweeps the full accumulator
    # (probe + variable rolls); the confirmed_rolls scan [window_start..B]
    # inclusive covers the same set — no asymmetry.
    window_start = gm01.getblockcount() + 1
    h_probe_start = window_start - 1
    print(f"[scenario] === Step 1c: throughput probe "
          f"(window_start={window_start}, submitting {N_PROBE} rolls) ===")

    # ptx_roll(count, low, high, game_id, salt) → call("ptx_roll", count, low,
    # high, False, [], game_id, salt) — confirmed against node.py:71 and
    # ptx.cpp RPC signature "count low high unique exclude game_id caller_salt".
    for i in range(N_PROBE):
        result = caller.ptx_roll(1, 1, 100,
                                 game_id=f"probe-{i+1}",
                                 salt=f"{i:08x}")
        runner.assert_true(
            len(result["tx_id"]) == 64,
            f"probe roll {i+1}: tx_id should be 64-char hex, got {result['tx_id']!r}"
        )

    # Wait for exactly one block to measure per-block throughput.  The low-cap
    # guard fires HERE (after ≤90s) — before the drain-wait which would time
    # out misleadingly at very low cap.
    print(f"[scenario] {N_PROBE} probe rolls submitted — "
          f"waiting for block {h_probe_start + 1} to measure cap")
    gm01.wait_for_height(h_probe_start + 1, timeout=90)
    observed_cap = count_ptxsess_in_block(caller, h_probe_start + 1)
    print(f"[scenario] observed per-block cap: {observed_cap}/block")

    runner.assert_true(
        observed_cap >= MIN_CAP,
        f"throughput too low: {observed_cap}/block (need ≥{MIN_CAP}) — "
        f"fleet not suitable for variable-volume scenario"
    )

    # Drain all probe rolls.  Timeout sized from measured cap to avoid the
    # 720s-fixed ceiling masking a legitimate drain at low-but-above-MIN_CAP cap.
    drain_blocks = math.ceil(N_PROBE / observed_cap) + 2
    drain_timeout = drain_blocks * 90

    _drain_scan_from = [window_start]
    _drain_count = [0]

    def all_probe_confirmed():
        current = gm01.getblockcount()
        while _drain_scan_from[0] <= current:
            _drain_count[0] += count_ptxsess_in_block(caller, _drain_scan_from[0])
            _drain_scan_from[0] += 1
        return _drain_count[0] >= N_PROBE

    caller.wait_for_condition(
        all_probe_confirmed,
        f"all {N_PROBE} probe rolls confirmed in window",
        timeout=drain_timeout
    )
    total_submitted = N_PROBE
    print(f"[scenario] probe drained: {N_PROBE} rolls confirmed, "
          f"observed_cap={observed_cap}/block")
    runner.checkpoint(f"throughput probe: cap={observed_cap}/block, {N_PROBE} rolls drained")

    # ── Step 1d: generate and validate schedule ───────────────────────────
    print("[scenario] === Step 1d: schedule generation ===")

    SEED_ENV = os.getenv("SCENARIO_SEED")
    SEED = int(SEED_ENV) if SEED_ENV else DEFAULT_SEED
    USER_FORCED_SEED = SEED_ENV is not None

    schedule = None
    for attempt in range(10):
        rng = random.Random(SEED)
        candidate = generate_schedule(rng, observed_cap)
        if validate_schedule(candidate, observed_cap):
            schedule = candidate
            break
        if USER_FORCED_SEED:
            raise RuntimeError(
                f"SCENARIO_SEED={SEED} produces a schedule that fails validation "
                f"(missing zero-block or high-volume entry).  "
                f"Unset SCENARIO_SEED to auto-walk, or choose a different seed."
            )
        SEED += 1
    if schedule is None:
        raise RuntimeError(
            "Could not generate a valid schedule in 10 attempts — "
            "check N_SLOTS and distribution parameters"
        )

    # Print the full schedule before committing ~50 minutes.  ≥1 zero-block and
    # ≥1 high-volume are confirmed here; abort if the seed fails, not 50 min in.
    high_threshold = max(1, observed_cap // 4)
    zero_slots = [i for i, (s, _) in enumerate(schedule) if s == 0]
    high_slots = [i for i, (s, _) in enumerate(schedule) if s >= high_threshold]
    print(f"\n[scenario] SEED={SEED}  observed_cap={observed_cap}/block  "
          f"N_SLOTS={N_SLOTS}  high_threshold={high_threshold}")
    print(f"[scenario] schedule ({len(schedule)} entries, "
          f"{len(zero_slots)} zero-block, {len(high_slots)} high-volume):")
    for idx, (burst, sleep) in enumerate(schedule):
        if burst == 0:
            tag = "  [ZERO — carry-through]"
        elif burst >= high_threshold:
            tag = f"  [HIGH — ≥{high_threshold} expected vins]"
        else:
            tag = ""
        print(f"  [{idx:02d}] burst={burst:4d}  sleep={sleep:.0f}s{tag}")
    print(f"[scenario] ≥1 zero-block: slots {zero_slots} ✓  "
          f"≥1 high-volume: slots {high_slots} ✓\n")
    runner.checkpoint("schedule validated and printed")

    # ── Step 2: variable roll submission ──────────────────────────────────
    # Compute B after probe drain so remaining window is known.
    B = next_settlement_boundary(gm01.getblockcount())
    print(f"[scenario] === Step 2: variable roll submission ===")
    print(f"[scenario] settlement boundary B={B}  "
          f"current={gm01.getblockcount()}  window_start={window_start}")

    roll_seq = [0]

    for idx, (burst_size, sleep_s) in enumerate(schedule):
        # Stop before the last-block race: if B-1 is already produced, a new
        # burst might miss B and land in B+1 (outside the scan window).
        if gm01.getblockcount() >= B - 1:
            print(f"[scenario] height {gm01.getblockcount()} ≥ B-1={B-1} — "
                  f"stopping submission to avoid post-B stragglers")
            break

        if burst_size == 0:
            print(f"[scenario] slot {idx:02d}: 0 rolls (carry-through pause) — "
                  f"sleeping {sleep_s:.0f}s")
            time.sleep(sleep_s)
            continue

        submitted_this_slot = 0
        for j in range(burst_size):
            roll_seq[0] += 1
            n = roll_seq[0]
            result = caller.ptx_roll(1, 1, 100,
                                     game_id=f"vv-{n}",
                                     salt=f"{n:08x}")
            runner.assert_true(
                len(result["tx_id"]) == 64,
                f"slot {idx} roll {j+1}: tx_id not 64-char hex: {result['tx_id']!r}"
            )
            submitted_this_slot += 1

        total_submitted += submitted_this_slot
        print(f"[scenario] slot {idx:02d}: {submitted_this_slot} rolls submitted "
              f"(total={total_submitted}  height={gm01.getblockcount()}) — "
              f"sleeping {sleep_s:.0f}s")
        time.sleep(sleep_s)

    print(f"[scenario] submission complete: total_submitted={total_submitted}")
    runner.checkpoint(f"variable rolls submitted (total={total_submitted})")

    # ── Step 3: wait for first PTXCOALESCE in window ──────────────────────
    # Proves at least one burst landed and was swept into the accumulator.
    # Timeout: N_SLOTS * 90s — generous upper bound covering a pathological
    # schedule that opens with many zero-burst slots before the first burst.
    print("[scenario] === Step 3: wait for first PTXCOALESCE ===")

    _coal_scan_from = [window_start]

    def coalesce_appeared():
        # getblockcount failures propagate (retry once with backoff) rather than
        # returning False — consistent with count_ptxsess_in_block error handling.
        # find_all_ptxcoalesce_in_window uses _find_special_tx_in_block internally
        # and handles its own per-tx transient errors; it does not raise.
        try:
            current = gm01.getblockcount()
        except Exception:
            time.sleep(1)
            current = gm01.getblockcount()  # propagates on second failure
        while _coal_scan_from[0] <= current:
            h = _coal_scan_from[0]
            if find_all_ptxcoalesce_in_window(caller, h, h):
                return True
            _coal_scan_from[0] += 1
        return False

    caller.wait_for_condition(
        coalesce_appeared,
        "first PTXCOALESCE appeared in window",
        timeout=N_SLOTS * 90,
    )
    runner.checkpoint("first PTXCOALESCE confirmed in window")

    # ── Step 4: wait for settlement + PTXPAYOUT ───────────────────────────
    print("[scenario] === Step 4: wait for settlement + PTXPAYOUT ===")
    current = caller.getblockcount()
    settlement_height = wait_for_settlement(caller, current, gm01=gm01)
    payout_tx = find_ptxpayout_in_block(caller, settlement_height)

    runner.assert_true(
        payout_tx is not None,
        f"PTXPAYOUT not found at settlement boundary height {settlement_height}"
    )
    print(f"[scenario] PTXPAYOUT found at height {settlement_height}: "
          f"txid={payout_tx['txid'][:16]}...")

    payout_sat = round(payout_tx["vout"][0]["value"] * 100_000_000)

    # ── Step 5a: exact amount assertion (confirmed-count basis) ───────────
    # Scan [window_start .. settlement_height] INCLUSIVE.  Block B is included
    # because blockassembler.cpp:240-263 appends PTXCOALESCE for B's own PTXSESS
    # before building PTXPAYOUT in the same block, and ProcessSpecialTxsInBlock
    # (specialtx_validation.cpp:1044-1078) applies CheckAndApplyPTXCoalesce
    # before CheckAndApplyPTXPayout.  Excluding B would undercount any rolls
    # that landed in the settlement block itself.
    print("[scenario] === Step 5a: exact amount assertion ===")

    confirmed_rolls = sum(
        count_ptxsess_in_block(caller, h)
        for h in range(window_start, settlement_height + 1)
    )
    print(f"[scenario] on-chain: confirmed_rolls={confirmed_rolls}  "
          f"total_submitted={total_submitted}  "
          f"window=[{window_start}..{settlement_height}]")

    expected_payout_sat = confirmed_rolls * PTX_SERVICE_FEE_SAT - PTX_PAYOUT_MINER_FEE
    runner.assert_equal(
        payout_sat, expected_payout_sat,
        f"PTXPAYOUT amount == confirmed_rolls({confirmed_rolls}) × "
        f"service_fee({PTX_SERVICE_FEE_SAT}) − miner_fee({PTX_PAYOUT_MINER_FEE})"
    )
    print(f"[scenario] payout={payout_sat} sat = "
          f"{confirmed_rolls}×{PTX_SERVICE_FEE_SAT}−{PTX_PAYOUT_MINER_FEE} ✓")

    # Throughput check — separate assertion, separate failure message so the
    # two failure modes are distinguishable:
    #   amount fails + throughput fails → rolls not confirmed in window (timing)
    #   amount passes + throughput fails → scan-range or probe-accounting error
    runner.assert_equal(
        confirmed_rolls, total_submitted,
        f"throughput: all {total_submitted} submitted rolls confirmed in window "
        f"[{window_start}..{settlement_height}]; "
        f"if amount assertion passed but this fails, check scan range or probe "
        f"accounting; if both fail, rolls were not confirmed in the window"
    )
    print(f"[scenario] throughput: {confirmed_rolls}=={total_submitted} ✓")
    runner.checkpoint("exact amount + throughput assertions passed")

    # ── Step 5b: PTXCOALESCE tx-size assertion ────────────────────────────
    # Assert: a high-volume PTXCOALESCE occurred (≥ cap//4 vins) and its
    # serialised size is under MAX_BLOCK_SIZE_CURRENT.  This is an adequacy
    # check — the rationale is tx-size scaling, not a max-stress bound.
    print("[scenario] === Step 5b: PTXCOALESCE tx-size assertion ===")

    coalesces = find_all_ptxcoalesce_in_window(caller, window_start, settlement_height)
    runner.assert_true(
        len(coalesces) > 0,
        f"expected ≥1 PTXCOALESCE in window [{window_start}..{settlement_height}]"
    )

    print(f"[scenario] PTXCOALESCE vin distribution ({len(coalesces)} blocks):")
    for h, tx in sorted(coalesces, key=lambda ht: ht[0]):
        print(f"  height {h}: {len(tx['vin'])} vin(s)  txid={tx['txid'][:16]}...")

    max_vin_height, max_vin_tx = max(coalesces, key=lambda ht: len(ht[1]["vin"]))
    max_vin_count = len(max_vin_tx["vin"])

    runner.assert_true(
        max_vin_count >= high_threshold,
        f"highest-vin PTXCOALESCE has {max_vin_count} vins at height {max_vin_height}; "
        f"expected ≥{high_threshold} (cap//4={observed_cap // 4}) — "
        f"no high-volume block confirmed in window"
    )

    raw_hex = caller.call("getrawtransaction", max_vin_tx["txid"], False)
    tx_size_bytes = len(raw_hex) // 2

    runner.assert_true(
        tx_size_bytes < MAX_BLOCK_SIZE,
        f"PTXCOALESCE at height {max_vin_height} ({max_vin_count} vins, "
        f"{tx_size_bytes} B) exceeds MAX_BLOCK_SIZE_CURRENT={MAX_BLOCK_SIZE} B"
    )
    print(f"[scenario] max PTXCOALESCE: height={max_vin_height}  "
          f"vins={max_vin_count}  size={tx_size_bytes} B < {MAX_BLOCK_SIZE} B ✓")
    runner.checkpoint("PTXCOALESCE tx-size assertion passed")

    # ── Step 6: winner address validation ─────────────────────────────────
    print("[scenario] === Step 6: winner address validation ===")
    winner_script = payout_tx["vout"][0]["scriptPubKey"]["addresses"][0]
    registered_pay_addrs = {
        info["ptx_payment_addr"] for info in registration.values()
    }
    runner.assert_true(
        winner_script in registered_pay_addrs,
        f"PTXPAYOUT recipient {winner_script} not in registered GM payment addresses"
    )
    print(f"[scenario] winner: {winner_script} ✓")

    status_post = caller.ptx_lottery_status()
    runner.assert_equal(
        status_post["pool_balance_sat"], 0,
        "pool_balance_sat should be 0 after PTXPAYOUT"
    )

    # ── Step 7: cross-node consensus agreement ─────────────────────────────
    print("[scenario] === Step 7: cross-node consensus agreement ===")
    settlement_tip = caller.getblockcount()
    for node in cluster.gms:
        node.wait_for_height(settlement_tip, timeout=60)

    lottery_states = {}
    for node in cluster.all_nodes:
        s = node.ptx_lottery_status()
        lottery_states[node.name] = (
            s["pool_balance_sat"],
            len(s.get("settlement_history", [])),
        )
    runner.assert_all_agree(
        lottery_states,
        "all nodes agree on lottery state (pool_balance_sat, settlement_history length)"
    )

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
    runner.checkpoint("cross-node lottery state agreement")

    # ── Step 8: pose tracker consistency ──────────────────────────────────
    print("[scenario] === Step 8: pose tracker consistency ===")
    pose_data = {}
    for node in cluster.all_nodes:
        records = node.ptx_pose_status()
        pose_data[node.name] = tuple(
            sorted((r["node_id"], r["eligible"]) for r in records)
        )
    runner.assert_all_agree(
        pose_data,
        "all nodes agree on pose tracker (node_ids + eligibility)"
    )
    print(f"[scenario] pose tracker consistent across all {len(cluster.all_nodes)} nodes ✓")
    runner.checkpoint("pose tracker consistency verified")

    # ── Step 9: settlement_history RPC reflection ──────────────────────────
    # settlement_history is newest-first (ptx.cpp:808 confirmed).  Index by
    # settlement_height rather than [0] to be robust against any prior
    # settlement entry on this fleet.
    print("[scenario] === Step 9: settlement_history reflection ===")
    history = caller.ptx_lottery_status().get("settlement_history", [])
    settle_entry = next(
        (e for e in history if e.get("height") == settlement_height), None
    )
    runner.assert_true(
        settle_entry is not None,
        f"settlement_history has no entry at height {settlement_height}"
    )
    runner.assert_true(
        "txid" in settle_entry and len(settle_entry["txid"]) == 64,
        f"settlement_history entry at height {settlement_height} has no valid txid"
    )
    print(f"[scenario] settlement_history: height={settle_entry['height']} "
          f"txid={settle_entry['txid'][:16]}... ✓")
    runner.checkpoint("settlement_history RPC verified")

    # ── Step 10: wallet-RPC attribution (KDD-035) ──────────────────────────
    # Index winner_history by settlement_height for the same reason as Step 9.
    print("[scenario] === Step 10: wallet-RPC attribution (KDD-035) ===")

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
    winning_gm_node.wait_for_height(settlement_height, timeout=60)

    winner_history = winning_gm_node.ptx_lottery_history()
    win_at_height = next(
        (e for e in winner_history if e.get("height") == settlement_height), None
    )
    runner.assert_true(
        win_at_height is not None,
        f"{winning_gm_label}: expected ptx_lottery_history entry at "
        f"height {settlement_height}"
    )
    print(f"[scenario] {winning_gm_label} wallet reflects win at height "
          f"{win_at_height['height']} ✓")

    # KDD-035: a non-winning GM's wallet must NOT contain an entry at this height.
    non_winner_node = next(gm for gm in cluster.gms if gm.name != winning_gm_node.name)
    non_winner_node.wait_for_height(settlement_height, timeout=60)
    non_winner_history = non_winner_node.ptx_lottery_history()
    non_winner_entry = next(
        (e for e in non_winner_history if e.get("height") == settlement_height), None
    )
    runner.assert_true(
        non_winner_entry is None,
        f"{non_winner_node.name}: should NOT have ptx_lottery_history entry at "
        f"height {settlement_height} (does not hold winner's payout key)"
    )
    print(f"[scenario] {non_winner_node.name} wallet correctly absent ✓")
    runner.checkpoint("wallet-RPC attribution (KDD-035) verified")

    print("[scenario] === VARIABLE VOLUME COMPLETE ===")


def main():
    parser = argparse.ArgumentParser(
        description="Run the ptx-bea variable-volume scenario"
    )
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
    runner.run(run_variable_volume)


if __name__ == "__main__":
    main()
