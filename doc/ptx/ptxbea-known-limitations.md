# ptxbea Known Limitations

This document is the honest-disclosure surface for the ptxbea testnet. Each item is stated against
the actual registers, source code, and design documents — not softened, not extended beyond what
those sources establish. Items carry their register ID where one exists; items without an ID are
tracked informally in the standup log or are pending register assignment.

No item here blocks testnet operation unless explicitly stated.

---

## 1. Trust model — v1 trusted-dealer DKG

**Register reference:** Design doc v3.5 §6.3 staging table; Phase 2/3 dev plan Phase 2 limitations
list.  
**Status:** Accepted for testnet. Phase 3 closure required before production claims of full
independent verification.

The v1 testnet uses a **trusted-dealer key generation** scheme, as explicitly labelled in the design
doc staging table (§6.3: "v1 Testnet — Threshold BLS, trusted-dealer DKG"). This is the documented
starting point.

**What trusted-dealer means operationally.** At the first `ptx_roll` call each daemon session, the
coordinator node generates a fresh random master polynomial, derives the group public key
(`group_pk`), and distributes per-GM scalar shares. `group_pk` is stable for the entire daemon
session. The coordinator is the single party that holds `master_sk`. No per-GM public key shares are
published; no Feldman VSS commitment vector exists.

**Verifiability split.** Every PTXSESS transaction stores the full 96-byte threshold signature
(`quorum_sig`) and all roll parameters on-chain. From that data, any third party **can** verify:

- `beacon == SHA256(quorum_sig)` — the beacon is the SHA256 of the published signature
- `results == PTX_MapBeacon(beacon, count, low, high, unique, exclude_integers)` — results follow
  deterministically from the beacon using the documented mapping algorithm

What a third party **cannot** currently verify from on-chain data: that `quorum_sig` is a legitimate
threshold BLS signature produced by at least `t` of the `n` registered GMs, rather than a signature
produced by the coordinator alone using `master_sk`. This check requires `group_pk`, which is not
published on-chain.

**Single-host fleet.** All 11 GM containers run on a single Proxmox host controlled by the
operator. The hypergeometric quorum-capture security numbers in the design doc (§6.1) apply to a
geographically distributed deployment; they do not apply to this fleet. The phase 2/3 dev plan
documents this explicitly: "Phase 3 moves PTX from a controlled single-host environment to 21+
geographically distributed nodes run by independent operators."

**`ptx_verify` Path A scope (planned, not yet shipped).** The planned `ptx_verify` RPC will
assert that the published results follow deterministically from the published signature. It will
**not** return `quorum_verified` — that field, specified in the design doc §11.1, requires
`group_pk` on-chain and is a Phase 3 deliverable.

**Phase 3 closure.** The design doc stages the fix as "Full Pedersen DKG with resharing" (§6.3
table, Phase 3: "No single party holds the master secret at any point in the ceremony"). The
implementation unit is VSS/Pedersen DKG plus publishing the stable per-session `group_pk` as a
unit — the VSS commitment vector is what ties per-GM shares to the published key, making them
inseparable. Per-session-stable keying means publication costs 48 bytes at session-init.

---

## 2. BUG-015 — 10% supply contraction absent on split-coinstake path

**Register ID:** BUG-015 (OPEN, MEDIUM) — `explorer/Docs/2026-05-19_hemis_bug_tracker.html`  
**Severity:** HIGH monetary-policy implication; does not block ptxbea testnet soak.  
**Status:** Code omission confirmed; live manifestation unverified. Before-mainnet decision required.

`SubtractGmPaymentFromCoinstake` in `gamemaster-payments.cpp` has two branches. When
`stakerOuts == 2` (one staker output), it deducts `gamemasterPayment + 10% × GetBlockValue` from
`vout[1]`. When `stakerOuts > 2` (split coinstake), it deducts only `gamemasterPayment`; the 10%
line is absent — not redirected, simply not present.

A staker producing split-output coinstakes (controlled by `stakeSplitThreshold` wallet config)
skips the 10% contraction for those blocks. Per-block emission is either 90% or 100% of
`GetBlockValue` depending on the staker's wallet setting, making the intended deflation mechanism
wallet-configurable.

The omission is confirmed in both the ptxbea fork and the upstream `Hemis-Blockchain/Hemis` source.
Whether any production staker currently produces split-output coinstakes is unconfirmed — Section B
of `inherited_features.py` sampled 3 blocks under default wallet config, all were non-split (burn
fired). Forced reproduction on ptxbea with `stakeSplitThreshold` lowered is owed before closing
this as "not manifesting in production."

The `inherited_features.py` Section B test gates its assertions on `coinstake_vouts` count,
correctly asserting either outcome — it describes actual behaviour; it does not bless it.

**Before mainnet:** intentional design decision required — should the 10% contraction fire on all
coinstakes regardless of output structure? If yes, add the `nSubsidy` deduction to the else branch.
If per-block variance is acceptable, document it explicitly as monetary policy.

---

## 3. KDD-038 — PoSe withhold enforcement disabled

**Register ID:** KDD-038 — `PTX_LE_STANDUP.md` §KDD-038  
**Status:** Accepted for testnet. A consensus-derivable withhold mechanism is required before
mainnet.

`RecordWithhold` was removed from `ptx_roll` (commit `a08d39e`) with no block-processing
replacement. The honest-participation side of PoSe is consensus-consistent — when a PTXSESS
confirms in a block, all validators call `RecordHonestParticipation` for the listed quorum members.
The withhold-penalty side is disabled.

A GM can withhold signatures in every quorum round indefinitely without losing lottery eligibility.
The anti-freeloading mechanism specified in KDD-023 is off.

**Why deferred.** "Who failed to sign" is not free-floating chain data. Consensus cannot trust the
caller's assertion alone. A consensus-visible withhold record requires either an explicit
withhold-accusation transaction co-attested by a threshold of other quorum participants, or a signed
bitmask in the PTXSESS payload that validators can check against the independently-derivable quorum
selection. Both approaches require a separate design document before production.

**Scope of current green.** All unit tests and scenario tests are passing. None exercises withhold
penalties end-to-end. Unit tests that call `RecordWithhold` do so directly on the tracker, bypassing
`ptx_roll` — they test the tracker API in isolation, not the penalty path. Step 14 / Step 16.7
happy-path scenarios have all 11 GMs participating; no withholding occurs.

---

## 4. Single-staker liveness exposure

**Register ID:** None — parked item in `PTX_LE_STANDUP.md` §Task 4 (Step 16.7). Register entry
pending.  
**Status:** Parked for 16.9 go-live gate. Not a correctness concern; a liveness one.

gm01 holds all staking coins on the current fleet. gm01 going down stalls chain extension entirely.
Same family as ODC-021 (coordinator SPOF). Not a consensus or settlement correctness issue — if
gm01 is down, the chain simply stops producing blocks; no invalid state is created. Correctness of
the PTX pipeline is unaffected.

**Before semi-public exposure:** distribute stake across 2–3 GMs so no single container failure
stalls the chain.

---

## 5. DKG contribution cache TTL tightens to one block at 60 s

**Register ID:** None — parked watch-item in `PTX_LE_STANDUP.md` §Task 6 (Step 16.7). Register
entry pending.  
**Status:** Benign at current fleet config. Re-evaluate before production-quorum or 1440-block
settlement deployment.

`MAX_CONTRIBUTION_CACHE_TIME = 60,000 ms` (`llmq/quorums_dkgsessionmgr.h:20`). At the ~3 s
testnet this equalled ~20 blocks of headroom. At 60 s blocks it equals exactly 1 block. The cache
is re-requestable and ptxbea uses `LLMQ_TEST` with 3 members, so re-request cost is trivial.

This watch-item cannot be dismissed as "fine on mainnet" because: (a) mainnet PTX is not yet
deployed — no production-quorum evidence exists; (b) mainnet will use production quorums
(`LLMQ_400_60`, 400 members) where re-request cost at 60 s cadence may differ materially; (c) the
1440-block settlement step (future, post-16.9) may alter DKG phase block counts, changing the
TTL/phase ratio again.

**Action:** re-evaluate when quorum size or block-time configuration changes, specifically at the
1440-block settlement step and before any production-quorum testnet.

---

## 6. ODC-023 — Beacon advances before funding check

**Register ID:** ODC-023 (OPEN) — `explorer/Docs/2026-05-19_hemis_bug_tracker.html`  
**Status:** Open design question. Does not affect consensus correctness or on-chain settlement
integrity. Does not block testnet operation.

In `ptx_roll` (`src/rpc/ptx.cpp:282`), `PTX_SetLastBeacon(beacon)` is called before
`PTX_AutoCommit` at line 311. If `PTX_AutoCommit` subsequently fails (throwing
`RPC_PTX_SETTLEMENT_FAILED = -32050`), the caller's local beacon state has advanced but no PTXSESS
transaction was submitted to chain.

The next round's nonce is derived from this unrecorded beacon. Whether this breaks, degrades, or
has no effect on the verifiability chain is unverified — the severity depends on whether
nonce-chain continuity is a stated verifiability requirement and whether the caller is expected to
recover from partial failures by re-anchoring. This is an open design question, not an established
consequence.

**Does not affect:** consensus correctness, on-chain settlement integrity, or lottery winner
selection. The on-chain record is only created when `PTX_AutoCommit` succeeds; a failed roll leaves
no on-chain trace.

---

## 7. Mainnet PTX fee defaults unset in CMainParams

**Register ID:** None — before-mainnet gate item, `PTX_LE_STANDUP.md` lines 1570–1574. Register
entry pending.  
**Status:** Does not affect ptxbea. Must be set before mainnet PTX activation.

`CChainParams` base class defaults (`src/chainparams.h:138–140`):

```
nPTXServiceFee{0}        — rolls cost nothing on mainnet if unset
nPTXPayoutMinerFee{0}    — payout miners unpaid if unset
nPTXSettlementWindow{1440}  — correct for mainnet
```

`CMainParams` does not override `nPTXServiceFee` or `nPTXPayoutMinerFee`. Both must be set
explicitly in `CMainParams` as part of the mainnet activation gate. `nPTXSettlementWindow` defaults
to 1440, which is the intended mainnet value.

---

## 8. Q2 — Destination of the 10% coinstake deduction unverified

**Register ID:** None — open item, `PTX_LE_STANDUP.md` settled-decisions table.  
**Status:** Open, unverified. Do not claim it is either burned or treasury in any document.

The 10% `nSubsidy` deduction applied in the `stakerOuts == 2` branch of
`SubtractGmPaymentFromCoinstake` (see BUG-015) has an unverified destination. Where these coins go
— whether they are permanently not created, burned to an unspendable address, or directed to a
treasury — has not been confirmed by tracing the code path through to its on-chain effect.

**Before mainnet:** resolve Q2 and document the monetary policy explicitly. Do not document either
outcome as established until it is confirmed by code trace or on-chain verification.

---

## 9. Per-block emission variance (related to BUG-015)

**Register ID:** None — `PTX_LE_STANDUP.md` settled-decisions table (see also BUG-015).  
**Status:** Current behaviour documented as-is; correctness of design intent unconfirmed.

Inheriting from the PIVX/DASH lineage: the 10% contraction fires only on non-split coinstakes
(`stakerOuts == 2`). Per-block emission is therefore either `block_value − GM_payment − 10% ×
block_value` or `block_value − GM_payment` depending on the staker's wallet `stakeSplitThreshold`
setting. This is the current behaviour in both the fork and upstream. Do not assert it is correct
or intentional without a design decision. See BUG-015 for the mechanism.

---

## 10. `exclude` tx_id form deferred

**Register ID:** BUG-065 (2026-09-06).  
**Status:** Integer excludes work. tx_id excludes are **REJECTED** at parameter validation —
**on `d0effa0` and later only.**

> **Which binary are you running?** The refusal is NOT in `v0.4.2-testnet` or any earlier release.
> On every published binary to date a tx_id exclusion is still **accepted and silently dropped**,
> with the false attestation described below. Do not use the tx_id form until you are running a
> release cut at or after `d0effa0`. `ptx_roll` on those binaries gives you no signal at all — the
> call succeeds.

**This section was correct and complete before the behaviour was measured** — it named the static
`PTX_ResolveExclude` in `src/rpc/ptx.cpp`, quoted its "deferred to Phase 2" log line, and noted that
the working implementation in `src/ptx/ptx_exclude.cpp` was never wired in. What it did not say, and
what measurement showed, is that *silently ignored* understated the problem.

**What was actually happening.** A tx_id was accepted, written into BOTH payloads by
`PTX_BuildExcludeLists`, folded into `params_hash` by `PTX_HashParams`, and therefore committed to
by the `round_seed` **the quorum signed** — and then dropped before the draw. Measured 2026-09-06:
excluding a roll that produced `[6,3,1,7,8,4]` from a 1–8 draw leaves a forced pool of `{2,5}` for
`count=2`; the roll returned `[7,1]`, both inside the excluded set, while the payload recorded the
exclusion. **The chain was attesting to something it had not done** — a false attestation, not a
no-op.

**Resolution: refuse, do not stub.** `ptx_roll` now throws
`-32602 tx_id exclusions are not implemented — use integer exclusions` in front validation, before
the commitment is built — free to hit, nothing broadcast. `exclude_txids` is consequently empty in
every payload, so the seed cannot commit to an exclusion that is ignored.

**Why still deferred rather than implemented.** Resolving a tx_id requires a chain lookup, and `/v2`
verifies with no node, no index and no chain. Wiring `ptx_exclude.cpp` in would make every roll
using the feature **unverifiable by the node-free verifier** — a decision about what `/v2`
guarantees, not a missing patch.

Integer excludes work correctly through the full path.

---

## 11. Accelerated chain parameters not at mainnet values

**Register ID:** None — `PTX_LE_STANDUP.md` chainparams reconciliation (Step 16.7).  
**Status:** Intentional for dev-fleet operation. Must converge before mainnet deployment.

The following `CPTXBeaTestNetParams` values are inherited from the PTX dev-fleet lineage and are
**not** standard testnet values — upstream `CTestNetParams` keeps them at mainnet levels:

| Parameter | ptxbea value | Mainnet value |
|---|---|---|
| `nCoinbaseMaturity` | 10 | 100 |
| `nStakeMinDepth` | 20 | 600 |
| `nStakeMinAge` | 0 | 60 × 60 s |
| `nGMCollateralAmt` | 100 COIN | 1,000,000 COIN |
| `nGMCollateralMinConf` | 1 | 15 |
| `nBudgetFeeConfirmations` | 3 | 6 |

None of these has a tracked convergence step. Each must reach its mainnet value before mainnet
deployment. The accelerated values are appropriate for the dev fleet and testnet soak; they are not
appropriate for mainnet.

Additionally, `nPTXSettlementWindow = 60` on ptxbea (60-block / ~60-minute cadence). The intended
mainnet value is 1440 (daily settlement). Recalibration and re-test at 1440 blocks is a separate
future step, post-16.9.

---

## 12. Quorum selection is advisory, not consensus-enforced (ODC-073)

**Register ID:** ODC-073.
**Status:** Accepted for testnet by decision; enforcement deferred, revisited for mainnet.

**Do not build around the property that a roll cannot be aimed at a chosen quorum — it can.**
The honest `ptx_roll` RPC routes each roll to a quorum by a tip-hash rule (`PTX_SelectDKGSigningCtx`),
so an ordinary caller does not pick the quorum. But that routing runs **only in the RPC**; no
consensus rule enforces it. What consensus checks (the BUG-033 commit gate) is only that the named
quorum is a **real quorum that is ACTIVE at the roll's seed height** — *any* active quorum, not the
routed one. A caller building its own commitment can therefore **name any active quorum directly**.

Why this is accepted rather than a defect:

- **The threat is bounded (§9.1).** Naming a quorum only lets a caller send its rolls to a quorum it
  wants — which helps only if that quorum is **already compromised** (a compromised quorum can bias
  its own outputs; targeting concentrates rolls onto it). It **cannot make an honest quorum lie or
  forge a result** — the threshold signature is still required and still verified.
- **The reachable set is bounded.** The `nSeedHeight` window (`nPTXSeedHeightWindow`, 60 blocks on
  ptxbea) limits how far back a roll may anchor, so a caller cannot reach a quorum that was active
  only in the distant past.
- **Legitimate uses exist** — keep-alive rolls, diagnostics, and testing all name a quorum
  deliberately, and a testnet's purpose is exercising the machinery.

**Decision (testnet):** selection stays advisory; the `nSeedHeight` bound plus this documented
statement, not consensus enforcement. Enforcing the routing at the commit gate would freeze the
current selection rule into consensus before the selection rule is settled (the demand-transition
test, ODC-074's capacity-aware argument, and the health beacon may all change what selection should
consider), so it is deferred. **Mainnet** revisits enforcement if ODC-074 (erosion + free targeting
making weak quorums cheap to finish off) proves material at real quorum counts.

---

## 13. Operator port requirement — RPC must be reachable, or signing silently fails

**Register ID:** ODC-073 / A (DGM-derived fan-out) / KDD-085 (the mainnet fix).
**Status:** Interim convention; enforced by the self-check, removed by KDD-085.

**ptxbea standard ports — open BOTH at the address you register:**

| Purpose | Port | Must be reachable by |
|---|---|---|
| P2P | **29994** | the network (peers) — definitionally open on a public chain |
| RPC (signing) | **29995** | the other gamemasters' fan-out, at your registered address |

The ports sit **below** the kernel ephemeral range (32768–60999) deliberately: the Hemis 5147x
family (mainnet/testnet/regtest) sits *inside* that range, and binding there invites a startup race
where the kernel hands your listening port to an outbound connection first. RPC = P2P + 1 restores
the PIVX-lineage adjacency (cf. testnet 51474/51475) without that exposure.

**The load-bearing requirement — read this if you read nothing else:** the signing fan-out reaches
each quorum member over **RPC at the host it advertises in its DGM registration, on port 29995**
(override: `-ptxfanoutport`). The DGM record advertises only the P2P endpoint; the RPC port is a
**convention**, not on-chain. So a gamemaster that registers successfully but has RPC **firewalled,
bound to localhost, or on a non-standard port** looks **fully healthy on-chain** — registered,
enabled, synced, Ready — and yet **silently fails every signing request**. This is exactly the
failure the fleet has hit twice (the IPv6 incident: nodes healthy on-chain, unreachable for
signing).

**What an operator must do:**
1. Bind RPC so it is reachable at your registered address, not just localhost: `rpcbind=<addr>`
   (or `rpcbind=[::]` for dual-stack) plus `rpcallowip` for the fleet/peer range. `listen=1`.
2. Open **29994 (P2P)** and **29995 (RPC)** in the firewall to the addresses that need them.
3. Verify from **outside** the host that RPC answers at `your-registered-address:29995` — the
   self-check does this; do not trust a local `hemis-cli` probe, which reaches localhost and hides
   exactly this fault.

**Why it's an interim convention:** the RPC-port assumption re-introduces an address-reachability
requirement on a permissionless network (operators with non-standard ports/firewalls violate it).
**KDD-085 (sign-over-P2P)** removes it entirely — the fan-out would reach the member at its
on-chain-advertised address over the P2P protocol it already must expose, so no separate RPC
exposure is needed. Until KDD-085 lands, the port convention above is the requirement.

---

## Share loss is permanent until quorum rotation — there is no re-share path (ODC-071)

A gamemaster's DKG key share (`ptx_shares.dat` in the datadir) is secret material produced by an
interactive ceremony. It is **not derivable from the chain, not recomputable, and not recoverable
from peers**. If it is lost, that member cannot sign for its quorum again — ever. The quorum
continues at reduced margin (each loss is one signer closer to the 6-of-11 threshold failing) until
scheduled rotation (W2.4) replaces the quorum wholesale with a fresh ceremony.

**What loses the share:** deleting or restoring the datadir from a backup taken before the
ceremony; disk failure; migrating hosts without carrying `ptx_shares.dat`; any procedure that
regenerates the datadir. (A plain restart or `-reindex` does NOT lose it as of the BUG-039 fix —
the file store survives both.)

**What an affected operator should do:**
1. Confirm the state: `ptx_quorum_health` shows `member: true, share_current: false` for the
   affected quorum, and the log prints the "in_qual … but NO share is held" warning at startup.
2. There is no operator action that recovers the share. Do not re-register; do not reindex —
   neither helps, and the member's chain registration remains valid.
3. Keep the node running and Ready. The member remains eligible for future formations; the next
   ceremony it is selected into produces a fresh share, held and persisted normally.
4. Expect the degraded state to last until the affected quorum rotates out on the schedule
   (quorum age-based; see the rotation cadence for the network).

**What to back up:** `ptx_shares.dat` is per-host secret material. If you snapshot datadirs, the
snapshot must post-date the newest ceremony the node completed, or restoring it forfeits the newer
shares (see above — permanently).

The proper mainnet answer (a proactive re-share protocol or Dash-QDATA-shaped P2P share recovery)
is design work tracked as ODC-071.
