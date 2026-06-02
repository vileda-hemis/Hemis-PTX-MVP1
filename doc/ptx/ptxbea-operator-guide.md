# ptxbea Operator Guide

ptxbea is a fresh testnet implementing the ODC-022 Solution 1 lottery architecture
(PTXCOALESCE/PTXPAYOUT, `LOTTERY_ACCUM_SCRIPT`). It runs at mainnet block timing (60-second blocks)
and serves as the mainnet deployment rehearsal. This guide covers standing up a Gamemaster node,
registering it, connecting it to the fleet, and enabling GM lottery payouts.

See `ptxbea-known-limitations.md` for limitations that apply before public exposure.

---

## 1. Network parameters

| Parameter | Value |
|---|---|
| Network ID | `ptxbea` |
| Chain flag | `-ptxbea` |
| Block time | 60 seconds (`nTimeSlotLength = 60`) |
| P2P port | 29994 |
| RPC port | 29903 |
| Network magic | `PTX3` (0x50 0x54 0x58 0x33) |
| Settlement window | 60 blocks (~60 minutes) |
| Service fee | 1 HMS / roll |
| Payout miner fee | 10,000 sat (0.0001 HMS) deducted from PTXPAYOUT output |
| LOTTERY_ACCUM_SCRIPT address | `y5GasXtTXAKC3fGZjw2Pm1y92D9DjgYeow` |
| GM collateral | 100 HMS |
| PoS activation | Block 50 |
| UPGRADE_V6_0 | ALWAYS_ACTIVE (required for ProRegPL / `scriptPTXPayment`) |

---

## 2. GM registration

ptxbea GMs register via ProRegPL (`protx_register` or `protx_register_fund`). This is distinct from
the old `feature/lottery-escrow` fleet, where registration was implicit in daemon startup flags.
On ptxbea, `UPGRADE_V6_0 = ALWAYS_ACTIVE` means the full ProTx path is live from genesis and
on-chain registration is required.

### 2.1 Registration parameters

```
protx_register_fund
  "collateralAddress"   HMS address to receive the 100 HMS collateral output
  collateralIndex       output index of the collateral in the funding tx
  "ipAndPort"           GM's public IP and P2P port (e.g. "172.30.0.11:29994")
  "ownerAddress"        owner EC key address (controls registration updates)
  "operatorPubKey"      GM's BLS operator public key (hex)
  "votingAddress"       voting EC key address
  "payoutAddress"       staking reward payout address
  [operatorReward]      optional operator reward percentage
  [operatorPayoutAddr]  optional operator reward address
  "ptxPaymentAddress"   address to receive PTX lottery payouts (see §2.2)
  "ptxNodeId"           human-readable GM label (see §2.3)
```

The `protx_register` variant (pre-funded collateral) takes `collateralHash` and `collateralIndex`
instead of `collateralAddress`.

### 2.2 PTX payment address (`ptxPaymentAddress`)

**Mandatory for lottery eligibility.** A GM without a registered `scriptPTXPayment` is excluded
from winner selection at every settlement boundary regardless of its `lottery_tickets` count. The
winner selection algorithm (`PTX_SelectWinner`) requires a non-empty `scriptPTXPayment` as an
eligibility condition — this is a consensus rule, not a recommendation.

Set `ptxPaymentAddress` to a standard HMS P2PKH address held in the GM's wallet. The address must
be spendable by the GM's wallet to satisfy `ptx_wallet_lottery_status` attribution (KDD-035).

### 2.3 PTX node identity (`ptxNodeId`)

Supplies the human-readable label component of the GM's compound `node_id`. The chain appends a
`:suffix` derived from the collateral outpoint — for example:

```
ptxNodeId = "gm01"  →  node_id = "gm01:a3f8c1d2"   # suffix is illustrative; real value from protx_list
```

The label component must match the `-ptxnodeid=` daemon flag exactly, including case. The full
compound `node_id` is what appears in `ptx_pose_status`, `ptx_lottery_status.eligible_nodes`, and
in the `quorum_members` field of `ptx_roll` responses.

Supply only the label in `ptxNodeId` — no colon. The daemon appends the `:suffix` automatically.

### 2.4 Registration confirmation

After `protx_register_fund` broadcasts, wait for 1 confirmation before proceeding. Verify the GM
appears in the DGM list:

```bash
hemis-cli -ptxbea protx_list valid true
```

Each registered GM should show `ptxNodeId` and `ptxPaymentAddress` in its entry. If either is
missing, the GM cannot win lottery payouts.

---

## 3. Daemon configuration

### 3.1 Caller node (`hemis.conf`)

```ini
ptxbea=1
rpcuser=ptxbearpc
rpcpassword=ptxbeapass2026
rpcallowip=0.0.0.0/0
server=1
listen=1
addnode=172.30.0.11    # connect to gm01 for P2P (single-leaf topology)
dnsseed=0

# PTX signing identity — caller is not a GM
ptxnodeid=caller

# Registered GM fleet (RPC fanout targets, not P2P peers)
# Suffixes are illustrative; real compound node_ids come from: hemis-cli -ptxbea protx_list valid true
ptxnode=gm01:a3f8c1d2@172.30.0.11:29903
ptxnode=gm02:b4e9d3f1@172.30.0.12:29903
ptxnode=gm03:c5f0e4g2@172.30.0.13:29903
# ... one entry per GM
```

The `-ptxnode=` format is `id@host:port` where `id` is the full compound node_id
(`label:suffix` from ProRegPL) and `host:port` is the GM's RPC endpoint. PTX signing uses RPC
fanout directly to each GM — it does not use P2P. The caller's single P2P link to gm01 is
sufficient to relay PTXSESS transactions into the mesh.

### 3.2 GM node (`hemis.conf`)

```ini
ptxbea=1
rpcuser=ptxbearpc
rpcpassword=ptxbeapass2026
rpcallowip=0.0.0.0/0
server=1
listen=1

# PTX identity — must match ptxNodeId supplied at ProRegPL registration
# (suffix is illustrative; real value from protx_list after registration)
ptxnodeid=gm01:a3f8c1d2
```

GMs do not need `-ptxnode=` entries. GMs are signing participants, not coordinators.

### 3.3 Staking

Set the staking address to the GM's funded wallet address. Ensure `stakingbalance` is non-zero
before relying on this node to extend the chain. On ptxbea, gm01 holds bootstrap coins and stakes
by default. See the single-staker liveness limitation in `ptxbea-known-limitations.md` §4.

---

## 4. Spork key management

GM payment (staking rewards split between staker and GM) is gated by SPORK_7 and SPORK_21. Both
default to disabled (value `4070908800`).

The ptxbea spork keypair was generated at commit `354dc4b` and the public key is baked into
`CPTXBeaTestNetParams` at `chainparams.cpp:885`. The operator holds the corresponding private key.

To enable GM payment after fleet bootstrap:

```bash
# From any node with sporkkey= in hemis.conf:
hemis-cli -ptxbea spork SPORK_21 0    # enable GM payment
hemis-cli -ptxbea spork SPORK_7 0     # enable GM enforcement
```

Confirm activation:

```bash
hemis-cli -ptxbea spork active | grep -E "SPORK_7|SPORK_21"
```

The spork broadcast propagates via P2P gossip. Allow ~120 s before expecting all nodes to reflect
the updated status.

---

## 5. Bootstrap sequence

The canonical bootstrap order is implemented in `testnet/harness/bootstrap.py`. The manual
sequence is (note: this order will be updated after the `fund_stakers` multi-staker seeding step
lands in bootstrap.py — reconcile against the harness before use):

1. Mine 49 PoW blocks to a known address on gm01
2. Wait for PoS activation at block 50
3. Wait for UPGRADE_V4_0 at block 265 (TimeProtocolV2 — enforces 60-second slot floor)
4. Register all GMs via `protx_register_fund` with `ptxPaymentAddress` and `ptxNodeId` set
5. Wait 1 confirmation per registration; verify DGM list
6. Fund the caller wallet via `sendmany` — use 2 HMS × N UTXOs for UTXO-split funding (each
   roll consumes one UTXO; pre-splitting avoids contention under sequential calls)
7. Enable SPORK_21 and SPORK_7 via the spork keypair
8. Issue a test `ptx_roll` — confirm `tx_id` is a 64-char hex (not an error code)

**Critical:** step 4 must complete before step 8. A GM that is not registered, or is registered
without `ptxPaymentAddress`, cannot win lottery payouts. The bootstrap script asserts this
explicitly.

**Pre-UPGRADE_V4_0 timing warning.** Between PoS activation (block 50) and UPGRADE_V4_0 (block
265), the chain may race through hundreds of blocks in seconds (free-time staking path). Bootstrap
confirmation timeouts must use the pre-V4_0 sizing regime (300 s/block × safety margin), not the
post-V4_0 60 s/block regime. See `testnet/harness/bootstrap.py` for the calibrated values.

---

## 6. Caller wallet funding pattern

Each `ptx_roll` call builds, funds, signs, and submits a PTXSESS transaction that consumes one
wallet UTXO as input for the 1 HMS service fee plus miner fee. A single large UTXO serialises
calls — the second call fires before the first call's change confirms, triggering
`RPC_PTX_SETTLEMENT_FAILED (-32050)`.

Recommended funding pattern: `sendmany` from gm01 to ~2500 addresses on the caller wallet, 2 HMS
each. Verify with `listunspent` before running scenarios. Refund when UTXO count drops below ~100.

---

## 7. Verifying fleet health

```bash
# Chain tip and peer count
hemis-cli -ptxbea getblockcount
hemis-cli -ptxbea getpeerinfo | grep -c addr

# PTX lottery state
hemis-cli -ptxbea ptx_lottery_status

# Pose tracker — all 11 GMs should appear as eligible
hemis-cli -ptxbea ptx_pose_status

# Confirm accumulator is non-zero after rolls
hemis-cli -ptxbea ptx_lottery_status | grep pool_balance_sat

# Verify GM payment address is set for all registered GMs
hemis-cli -ptxbea protx_list valid true | grep -A5 ptxPaymentAddress
```

After a successful settlement, `ptx_lottery_status` will show `last_settle.height` non-zero and
`settlement_history` populated with the PTXPAYOUT txid, winner address, and amount.

---

## 8. Coordinator recovery

### 8.1 Crash recovery — 1-step

If the coordinator (`ptx-bea-caller`) crashes or is restarted while GMs and the chain remain up,
recovery is a single step: **restart the coordinator daemon.** PTX rolls resume automatically on
the first `ptx_roll` call after restart via the lazy-init path:

1. The coordinator detects `g_ptx_bls_state.initialized == false` and calls `PTX_BLS_Init`,
   generating a fresh master polynomial and per-GM key shares.
2. `PTX_FanOutKeySet` delivers the new shares to all 11 GMs via RPC. Each GM's `gm_bls_keyset`
   handler unconditionally overwrites its stored share — there is no "already initialised" guard
   on the GM side, no rejection path, no GM restart required.
3. Signing resumes normally.

The chain continues producing PoS blocks throughout coordinator downtime (GMs stake
independently). LotteryState (accumulator, last settlement), ProRegPL registrations, and PoSe
history (`ptx_pose.dat`) are all preserved and reload at startup. In-flight rolls at crash time
are lost and must be re-issued by the caller.

### 8.2 Crash recovery ≠ permanent coordinator loss (ODC-021)

Restart recovery is fast and requires no fleet action. However, the PTX signing coordinator
remains a single logical endpoint — if it is **permanently unavailable** (not just restarted),
PTX rolls cannot proceed even though the chain and GMs are healthy. This is ODC-021 (coordinator
SPOF), the same family as the single-staker liveness concern. It is deferred to Phase 3.

Distributing stake across multiple GMs (see `ptxbea-known-limitations.md` §4) removes the
chain-extension SPOF. It does not address ODC-021.

### 8.3 Beacon chain break at restart boundary (ODC-023)

`g_ptx_last_beacon` is in-memory and resets to all-zero on restart. The first post-restart roll's
nonce is derived from the zero beacon rather than the last on-chain PTXSESS beacon, breaking
nonce-chain continuity at the restart boundary. This is a known open design question, not an
operational error — rolls function correctly and results are verifiable from the on-chain
`quorum_sig`. See `ptxbea-known-limitations.md` §6 (ODC-023) for the full scope.

---

## 10. Known limitations

See `ptxbea-known-limitations.md` for the full list. The items most relevant to operators:

- **§1 Trust model** — single-host trusted-dealer fleet; signing legitimacy not independently
  verifiable until Phase 3 Pedersen DKG
- **§3 KDD-038** — PoSe withhold enforcement disabled; withholding GMs face no penalty on this
  fleet
- **§4 Single-staker liveness** — park stake across multiple GMs before semi-public operation
- **§11 Accelerated params** — collateral (100 HMS), maturity (10 blocks), and other chain
  parameters are dev-fleet values, not mainnet values
