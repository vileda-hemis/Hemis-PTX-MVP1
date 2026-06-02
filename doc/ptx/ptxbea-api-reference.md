# ptxbea API Reference

This document covers the developer-facing RPC surface of the ptxbea testnet. All signatures,
return types, and behaviour are documented against the current implementation
(`src/rpc/ptx.cpp`), not the design doc spec — where the two differ, the implementation wins and
the discrepancy is noted.

---

## 1. Fee model

**Operator-pays model.** The caller (game server) funds the service fee; the caller's wallet must
hold sufficient UTXOs before any `ptx_roll` call.

| Item | Value | Source |
|---|---|---|
| Service fee | 1 HMS (100,000,000 sat) per roll | `nPTXServiceFee = 1 * COIN` |
| Payout miner fee | 10,000 sat deducted from PTXPAYOUT output | `nPTXPayoutMinerFee = 10000` |
| `ptx_verify` fee | Zero — free read-only call (planned; not yet implemented) | |
| All other status RPCs | Zero | |

The 1 HMS service fee is consumed by each `ptx_roll` call: the PTXSESS transaction routes it to
`LOTTERY_ACCUM_SCRIPT` (`y5GasXtTXAKC3fGZjw2Pm1y92D9DjgYeow`) where it accumulates until a
settlement boundary.

---

## 2. Settlement mechanics

Settlement fires at every block height where `height % nPTXSettlementWindow == 0` (every 60 blocks,
~60 minutes on ptxbea), provided:

1. An accumulator UTXO exists (at least one `ptx_roll` was made in this or a prior window)
2. At least one GM is eligible: `quorum_eligible == true`, `lottery_tickets > 0`, and
   `scriptPTXPayment` non-empty

**PTXCOALESCE** is generated at the end of every block that contains one or more PTXSESS
transactions. It spends the prior accumulator UTXO (if any) plus all new PTXSESS service-fee
outputs from that block, and produces a single new accumulator UTXO. Zero miner fee; no
`extraPayload`.

**PTXPAYOUT** is generated at settlement boundaries when conditions are met. It spends the single
accumulator UTXO and pays `accumulator_value − 10,000 sat` to the winning GM's registered
`scriptPTXPayment`. If no eligible winner exists, the accumulator carries forward (rollover) and
lottery tickets are not reset.

**Winner selection** is ticket-weighted. A GM with N tickets has probability N/total_tickets of
winning. Selection entropy is derived from the parent block hash, not the settlement block hash
(which is unknown at assembly time). The algorithm is fully deterministic and reproducible from
public chain data.

---

## 3. `ptx_roll`

The primary API call. Signs a round via BLS threshold quorum, derives random results, and
auto-commits a PTXSESS transaction to chain.

### Signature

```
ptx_roll count low high unique exclude game_id caller_salt
```

### Parameters

| # | Name | Type | Required | Description |
|---|---|---|---|---|
| 1 | `count` | int | yes | Number of values to draw; must be ≥ 1 |
| 2 | `low` | int | yes | Minimum value, inclusive |
| 3 | `high` | int | yes | Maximum value, inclusive; must be ≥ `low` |
| 4 | `unique` | bool | yes | If true, draws without replacement (Fisher-Yates); pool must be ≥ `count` |
| 5 | `exclude` | array | yes | Integers or 64-char hex tx_ids to exclude; pass `[]` for none |
| 6 | `game_id` | string | yes | Caller-defined game identifier; no format constraint |
| 7 | `caller_salt` | string | yes | Caller entropy; must be a hex string (only `[0-9a-f]`); may be empty string `""` |

**`exclude` note:** integer elements are resolved and excluded correctly. 64-char tx_id string
elements are accepted by parameter validation but are silently ignored at resolution time — tx_id
exclude resolution is deferred. See `ptxbea-known-limitations.md` §10.

**`caller_salt` note:** `IsHex()` validation is strict — only lowercase hex characters `[0-9a-f]`
are accepted. Prefixes such as `"0x"` or any non-hex character will be rejected with
`RPC_INVALID_PARAMS`. Use `f"{n:08x}"` style formatting or pass `""`.

### Response

```json
{
  "results"         : [int, ...],
  "round_seed"      : "hex",
  "quorum_sig"      : "hex (96 bytes = 192 hex chars)",
  "quorum_sig_hash" : "hex (SHA256 of quorum_sig = beacon value)",
  "quorum_members"  : ["node_id_string", ...],
  "block_height"    : int,
  "tx_id"           : "hex (64 chars)"
}
```

**Field notes:**

- `results` — the drawn values, all in `[low, high]`. Length equals `count`.
- `round_seed` — the combined seed used as the BLS message input.
- `quorum_sig` — 96-byte compressed G2 point (BLS12-381); the threshold BLS signature.
- `quorum_sig_hash` — `SHA256(quorum_sig)`. Equals the beacon value used for result derivation.
  This is the field to use when verifying the result derivation chain (beacon → results). The
  `beacon` field name used in older test suite versions referred to this same value.
- `quorum_members` — array of node_id strings (e.g. `"gm01:a3f8c1d2"`), not public keys.
  **Design doc discrepancy:** the design doc (v3.5 §8.2) specifies `pubkey[]` for this field. The
  current implementation returns the compound node_id strings registered via ProRegPL. A third
  party cannot reconstruct the quorum BLS public key from these strings alone — see
  `ptxbea-known-limitations.md` §1 (trust model).
- `block_height` — chain height at the time of the call; anchors the round against the block.
- `tx_id` — the on-chain PTXSESS transaction id. Use this to look up the round in a block
  explorer or as the argument to `ptx_getroundstatus`.

### Error codes

| Code | Name | Condition |
|---|---|---|
| `-32050` | `RPC_PTX_SETTLEMENT_FAILED` | `PTX_AutoCommit` could not build, fund, sign, or submit the PTXSESS transaction. No `tx_id` is returned — the field is absent on error, not populated with a sentinel. Common causes: insufficient caller wallet balance, wallet unavailable. |
| `-1` | `RPC_MISC_ERROR` | PTX not enabled (`ptxnodeid=` not set), no registered nodes, BLS threshold not met, BLS recovery or verification failed. |
| `-8` | `RPC_INVALID_PARAMS` | Parameter validation failure: `count < 1`, `low > high`, `exclude` not an array, `caller_salt` not hex, exclude string not 64 chars. |

On `RPC_PTX_SETTLEMENT_FAILED`, the BLS signing and result derivation may have succeeded — the
failure is in the on-chain submission step. The round is internally resolved but has no on-chain
record. See `ptxbea-known-limitations.md` §6 (ODC-023) for the beacon-advance implication.

### Example

```bash
curl -s -u ptxbearpc:ptxbeapass2026 \
  --data-binary '{"jsonrpc":"1.0","id":"r","method":"ptx_roll",
    "params":[1,1,100,false,[],"mygame","00aabbcc"]}' \
  -H 'content-type: text/plain;' \
  http://127.0.0.1:29903/
```

---

## 4. `ptx_getroundstatus`

Returns in-memory round state and pose records. Covers up to the last 10 rounds if no `round_id`
is specified.

### Signature

```
ptx_getroundstatus ( round_id )
```

`round_id` is optional. If omitted, returns up to the last 10 rounds in the in-memory store.

### Response

```json
{
  "rounds": [
    {
      "round_id"   : "string",
      "round_seed" : "hex",
      "beacon"     : "hex",
      "threshold"  : int,
      "state"      : int,
      "committed"  : ["node_id", ...],
      "withheld"   : ["node_id", ...],
      "abstained"  : ["node_id", ...],
      "count"      : int,
      "low"        : int,
      "high"       : int,
      "unique"     : bool,
      "exclude"    : [...],
      "results"    : [int, ...]   // only present when state == RESOLVED
    }
  ],
  "pose_records": [ /* see ptx_pose_status */ ]
}
```

`state` values: `0 = COMMIT_PHASE`, `1 = RESOLVED`.

This RPC operates on in-memory state. Round history is not persisted across daemon restarts and is
capped at the last 10 rounds.

**Note:** `ptx_verify` (independent on-chain result verification) is not yet implemented. See
`ptxbea-known-limitations.md` §1 for scope. When implemented, `ptx_verify` will verify that
results follow deterministically from the on-chain `quorum_sig`; it will not verify signing
legitimacy (`quorum_verified` field) until Phase 3.

---

## 5. `ptx_lottery_status`

Chain-wide lottery state for explorer and monitoring consumption. Reads from `LotteryState`
(persisted in evodb).

### Response

```json
{
  "pool_balance_sat"  : int,
  "settlement_window" : int,
  "current_height"    : int,
  "next_settlement_at": int,
  "total_rolls"       : int,
  "eligible_nodes"    : [
    {
      "node_id"               : "string",
      "pose_score"            : int,
      "eligible"              : bool,
      "tickets"               : int,
      "penalized_this_window" : bool
    }
  ],
  "last_settle": {
    "height"       : int,
    "winner_protx" : "hex",
    "amount_sat"   : int,
    "amount"       : "string (HMS, 8dp)",
    "txid"         : "hex",
    "gm"           : "string (Base58Check, absent if no registered address)"
  },
  "settlement_history": [
    {
      "height"       : int,
      "winner_protx" : "hex",
      "amount"       : "string (HMS, 8dp)",
      "txid"         : "hex",
      "gm"           : "string (absent if no registered address)"
    }
  ]
}
```

`last_settle.height == 0` and `winner_protx` all-zero indicates no settlement has occurred yet.
`settlement_history` is newest-first, capped at 20 entries.

---

## 6. `ptx_pose_status`

All GM pose records from the in-memory tracker.

### Response

Array of pose records:

```json
[
  {
    "node_id"               : "string",
    "pose_score"            : int,
    "eligible"              : bool,
    "tickets"               : int,
    "penalized_this_window" : bool
  }
]
```

`eligible` is false when `pose_score` exceeds the ban threshold. `penalized_this_window` is true
if the GM incurred a penalty in the current lottery window.

---

## 7. `ptx_gm_pose`

Single GM pose record by node_id.

```
ptx_gm_pose "node_id"
```

Returns the same structure as a single entry from `ptx_pose_status`. Returns an error if the
node_id is not found in the tracker.

---

## 8. `ptx_wallet_lottery_status` (ENABLE_WALLET)

Wallet-scoped lottery view. Shows only GMs whose `scriptPTXPayment` is spendable by this wallet
(payout-key ownership per KDD-035, not operational ownership).

Returns `my_gms` (GMs this wallet can receive payouts for), `my_wins` (historical wins at this
wallet's addresses), and `my_balance` (total HMS received from lottery payouts in this wallet).

---

## 9. `ptx_wallet_operated_gms` (ENABLE_WALLET)

Lists GMs where this wallet holds the owner or voting EC key (from ProRegPL). Predicate:
`HaveKey(keyIDOwner) || HaveKey(keyIDVoting)`. The BLS operator key is not checked — `CKeyStore`
is EC-only (KDD-036).

---

## 10. ProTx RPCs — GM registration

Standard ProTx commands with PTX extensions:

```
protx_register        collateralHash collateralIndex ipAndPort ownerAddress
                      operatorPubKey votingAddress payoutAddress
                      [operatorReward operatorPayoutAddress ptxPaymentAddress ptxNodeId]

protx_register_fund   collateralAddress ipAndPort ownerAddress operatorPubKey
                      votingAddress payoutAddress
                      [operatorReward operatorPayoutAddress ptxPaymentAddress ptxNodeId]

protx_register_prepare  (same as protx_register; returns unsigned tx for external signing)
protx_register_submit   signedTx
```

`ptxPaymentAddress` — HMS address for lottery payouts. Optional in the RPC signature; mandatory
for lottery eligibility. A GM registered without this address will never win a settlement.

`ptxNodeId` — label component of the compound node_id. Supply only the label (no colon); the chain
appends the `:suffix` from the collateral outpoint automatically. Must match the `-ptxnodeid=`
daemon flag exactly.

---

## 11. Error code reference

| Code | Name | Defined in |
|---|---|---|
| `-32050` | `RPC_PTX_SETTLEMENT_FAILED` | `src/rpc/protocol.h:56` |
| `-8` | `RPC_INVALID_PARAMS` | standard JSON-RPC |
| `-1` | `RPC_MISC_ERROR` | standard JSON-RPC |
