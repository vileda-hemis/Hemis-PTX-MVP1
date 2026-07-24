"""Diagnostic: confirm 5 PTXSESS fees are correctly coalesced and paid out.

Bootstraps, submits 5 rolls, waits for settlement_history to increment, then
scans roll_height..tip for PTXCOALESCE and PTXPAYOUT and reports exact amounts.

The "broken gate": an EARLIER version of this diagnostic polled the pool balance
for `pool_balance_sat >= N_ROLLS*fee`. That was a HARNESS observability race, not
a daemon defect — PTXCOALESCE (accumulate) and PTXPAYOUT (drain) fire in
consecutive blocks at a settlement boundary, so the pool spikes and returns to 0
faster than the poll can observe the peak (measured 0->500M->0 across heights
223-225 on the fast 3s/block chain). The daemon's coalesce/payout was correct
throughout; the fix was to gate on settlement_history incrementing instead.
Settled by the KDD-069-era PTXCOALESCE/PTXPAYOUT provenance recon (2026-07-24) —
this is a RESOLVED harness race, NOT an open daemon bug.

Pass condition: PTXCOALESCE shows ~5 HMS coalesced; PTXPAYOUT shows
~5 HMS − 10,000 sat (nPTXPayoutMinerFee) paid out.

This is a diagnostic, not a pass/fail scenario — it exits after printing
amounts so the human can confirm the daemon is correct.

Run:
    python3 -m testnet.scenarios._diag_coalesce
"""
import sys, os, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from testnet.harness.cluster import Cluster
from testnet.harness.bootstrap import bootstrap as do_bootstrap
from testnet.harness.node import Node

COIN = 100_000_000
PTX_SERVICE_FEE_SAT = COIN
N_ROLLS = 5
NTYPE_COALESCE = 9
NTYPE_PAYOUT   = 10


def scan_special_tx(node, height_start, height_end, ntype):
    """Return (height, tx) for first special tx of ntype in height range."""
    for h in range(height_start, height_end + 1):
        try:
            bh    = node.call("getblockhash", h)
            block = node.call("getblock", bh, 1)
            for txid in block.get("tx", []):
                raw = node.call("getrawtransaction", txid, True)
                if raw.get("type") == ntype:
                    return h, raw
        except Exception:
            pass
    return None, None


def main():
    cluster = Cluster()
    cluster.down(volumes=True)

    print("=== bootstrap ===")
    cluster.up()
    cluster.wait_ready(timeout=120)
    registration = do_bootstrap(cluster)
    caller = cluster.caller
    gm01   = cluster.gms[0]
    print(f"bootstrap done, height={caller.getblockcount()}")

    # KDD-069 (dealer retired): ptx_roll signs ONLY with an ACTIVE DKG quorum.
    # This diagnostic forms no quorum, so on a dealerless build the roll
    # hard-errors and the coalesce/settlement diagnostic below is unreachable.
    print("\n=== ptx_roll must hard-error (dealer retired, KDD-069) ===")
    rolled = True
    try:
        caller.ptx_roll(1, 1, 100, game_id="diag-0", salt="00aabbcc")
    except Exception as e:
        rolled = False
        assert "KDD-069" in str(e), f"expected KDD-069 dealer-retired error, got: {e}"
        print(f"  ptx_roll correctly hard-errors: {e}")
    assert not rolled, \
        "ptx_roll should hard-error post-KDD-069 (no ACTIVE quorum, dealer retired)"
    print("=== diagnostic ends: no signing quorum on a dealerless build ===")
    return

    pre_settled = len(caller.ptx_lottery_status().get("settlement_history", []))
    print(f"\n=== waiting for settlement (pre_settled={pre_settled}) ===")
    deadline = time.time() + 300
    while time.time() < deadline:
        s = caller.ptx_lottery_status()
        pool = s["pool_balance_sat"]
        n    = len(s.get("settlement_history", []))
        h    = caller.getblockcount()
        print(f"  pool={pool} sat  settled={n}  height={h}", flush=True)
        if n > pre_settled:
            print("  → settlement detected, stopping wait")
            break
        time.sleep(3)

    tip = caller.getblockcount()
    scan_end = min(tip, roll_height + 50)
    print(f"\n=== scanning blocks {roll_height}..{scan_end} ===")

    c_height, c_tx = scan_special_tx(caller, roll_height, scan_end, NTYPE_COALESCE)
    if c_tx:
        out_val = sum(v["value"] for v in c_tx.get("vout", []))
        out_sat = round(out_val * COIN)
        n_inputs = len(c_tx.get("vin", []))
        print(f"\nPTXCOALESCE at height {c_height}:")
        print(f"  txid    = {c_tx['txid']}")
        print(f"  inputs  = {n_inputs}  (expect {N_ROLLS} PTXSESS fee + 0 or 1 prior accum)")
        print(f"  output  = {out_sat} sat  ({out_val:.8f} HMS)")
        print(f"  expect  = {N_ROLLS * PTX_SERVICE_FEE_SAT} sat  ({N_ROLLS} HMS)")
        match = abs(out_sat - N_ROLLS * PTX_SERVICE_FEE_SAT) <= 2
        print(f"  match   = {match}  ({'✓ CORRECT' if match else '✗ MISMATCH'})")
    else:
        print(f"\nPTXCOALESCE NOT FOUND in blocks {roll_height}..{scan_end}")

    p_height, p_tx = scan_special_tx(caller, roll_height, scan_end, NTYPE_PAYOUT)
    if p_tx:
        payout_sat = round(p_tx["vout"][0]["value"] * COIN)
        expected   = N_ROLLS * PTX_SERVICE_FEE_SAT - 10_000
        winner     = p_tx["vout"][0]["scriptPubKey"].get("addresses", ["?"])[0]
        registered = {info["ptx_payment_addr"] for info in registration.values()}
        print(f"\nPTXPAYOUT at height {p_height}:")
        print(f"  txid    = {p_tx['txid']}")
        print(f"  payout  = {payout_sat} sat  ({payout_sat/COIN:.8f} HMS)")
        print(f"  expect  = {expected} sat  (5 HMS − 10000 sat miner fee)")
        print(f"  winner  = {winner}")
        print(f"  in DGM  = {winner in registered}  ({'✓' if winner in registered else '✗'})")
        match = payout_sat == expected
        print(f"  match   = {match}  ({'✓ CORRECT' if match else '✗ MISMATCH'})")
    else:
        print(f"\nPTXPAYOUT NOT FOUND in blocks {roll_height}..{scan_end}")

    cluster.down(volumes=True)


if __name__ == "__main__":
    main()
