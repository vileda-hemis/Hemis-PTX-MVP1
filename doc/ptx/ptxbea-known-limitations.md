# ptxbea Known Limitations

This document is the honest-disclosure surface for the ptxbea testnet. Each item is stated against
the actual registers, source code, and design documents — not softened, not extended beyond what
those sources establish. Items carry their register ID where one exists; items without an ID are
tracked informally in the standup log or are pending register assignment.

No item here blocks testnet operation unless explicitly stated.

---

## 1. Trust model — distributed key generation (the v1 trusted-dealer scheme is retired)

**Register reference:** KDD-069 (trusted dealer retired, commit 8a2200e, 2026-07-24); W1.2 ceremony
phases (PTXDKG transaction, commit bae1dcf, 2026-06-12); KDD-072 rotation. The design doc v3.5 §6.3
staging table row "v1 Testnet — Threshold BLS, trusted-dealer DKG" describes the historical scheme
below.  
**Status:** current for ptxtestnet as of `v0.5.1-testnet`. Section rescoped 2026-09-13 (the
verifiability statements were verified against `v0.5.0-testnet` on that date) and its tail
corrected 2026-09-15: the three paragraphs that followed still described the retired scheme's
fleet and staged the DKG as a future fix.

**Current scheme.** Quorums of eleven gamemasters form at boundary heights through a multi-phase
distributed key generation among the selected members, threshold six. The ceremony ends in a mined
`PTXDKG` transaction whose payload carries the group public key (`group_pk_bytes`), the hash of the
verification vector (`vvec_hash`), the member list, and at least `t` member-signed commitments. A
node builds its quorum record from that transaction (`ptx_quorum_store.cpp`), and consensus verifies
every settlement's `quorum_sig` against the record's key (`specialtx_validation.cpp`,
`ptx-bad-quorum-sig`). No party holds a master key; each member holds only its own share.

**Historical (ptxbea v1, 2026-06-02 to 2026-07-24).** The first testnet used a trusted-dealer scheme:
at the first `ptx_roll` of a daemon session the coordinator generated a random master polynomial,
derived `group_pk`, and distributed per-GM scalar shares. The coordinator alone held `master_sk`, no
verification vector existed, and `group_pk` was not published on chain. That path was removed with
KDD-069 and no longer exists in the source; the paragraph that used to stand here described it as
current.

**Verifiability split.** Every PTXSESS transaction stores the full 96-byte threshold signature
(`quorum_sig`) and all roll parameters on-chain. From that data, any third party **can** verify:

- `beacon == SHA256(quorum_sig)` — the beacon is the SHA256 of the published signature
- `results == PTX_MapBeacon(beacon, count, low, high, unique, exclude_integers)` — results follow
  deterministically from the beacon using the documented mapping algorithm
- `quorum_sig` verifies under the quorum's group public key. That key is published on chain in the
  `PTXDKG` transaction that created the quorum (`group_pk_bytes` in the payload, keyed by
  `quorum_hash`), and consensus verifies every settlement against it. A verifier needs the chain and
  a BLS12-381 library, not a node; the public `/v2` page does not perform this check only because it
  runs without a node.

What a third party **cannot** verify from chain data: that the group key corresponds to shares
genuinely held by at least `t` of the `n` members. The `PTXDKG` transaction carries the hash of the
verification vector and member-signed commitment hashes, not the vectors themselves, so the ceremony
is attested on chain rather than re-verifiable from it. Nor can a verifier without a node establish
which quorum the selection rule named for the seed height; that rule is advisory (section 12).

**Operator concentration (ptxtestnet, 2026-09).** The network is no longer single-host: the
registered gamemasters are run by several independent operators on their own hardware (the
launch fleet, PJH, and the Nodes24 group, roughly 24 / 3 / 31 at the last count), so the
hypergeometric quorum-capture numbers in the design doc (§6.1) now describe the right shape of
deployment. What remains true is that one operator still runs the largest share of the pool and
all three coordinators, so a quorum drawn from the pool is more likely to be majority-one-operator
than those numbers assume for a fully independent set. (The earlier text here — "all 11 GM
containers run on a single Proxmox host" — described the retired ptxbea v1 fleet.)

**Verification tooling.** A node-free verifier can check `beacon`, `results` and `quorum_sig`
against the on-chain `group_pk` today (the split above); there is no `ptx_verify` RPC and no
`quorum_verified` field — the earlier paragraph that staged `group_pk` publication as a "Phase 3
deliverable" predated KDD-069 and W1.2, both of which have shipped. The one thing no tool can
establish from chain data is share possession by `t` of `n`; that is a property of the ceremony,
attested by the member-signed commitments in the `PTXDKG` transaction, not re-verifiable from it.

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
**Status:** Historical for ptxbea; reduced on ptxtestnet. Not a correctness concern; a liveness one.

On the ptxbea v1 fleet gm01 held all staking coins, so gm01 going down stalled chain extension
entirely (same family as ODC-021, the coordinator SPOF). On ptxtestnet stake is held on the three
launch wallet hosts and by external operators, and blocks have continued through single-host
outages and migrations. What remains is concentration rather than a single point: most stake is
still one operator's. Not a consensus or settlement correctness issue in either case — a stalled
chain creates no invalid state, and the PTX pipeline is unaffected.

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

> **★ 2026-09-10 (KDD-129, `v0.5.0-testnet`).** The window is now read per height: `PTXSettlementWindow(nHeight)` returns `nPTXSettlementWindow` below `consensus.nPTXCadenceActivationHeight` and `nPTXSettlementWindowV2` from it. Mainnet is unaffected — `CMainParams` leaves the activation height at `NO_ACTIVATION_HEIGHT`, so the 1440 default is the only value it ever uses — but a future mainnet activation gate now has a second consensus field to set deliberately. `ptxtestnet` is the only network with a switch (5 → 1440 at h15840); `ptxbea` stays at 60.

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

> **Which binary are you running?** The refusal ships in **`v0.4.3-testnet`**. On
> `v0.4.2-testnet` and every earlier release a tx_id exclusion is **accepted and silently dropped**,
> with the false attestation described below, and `ptx_roll` gives you no signal at all — the call
> succeeds. Upgrade to `v0.4.3-testnet` or do not use the tx_id form.

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

## 13. Operator port requirement — inbound P2P must be reachable, or your gamemaster is never asked to sign

**Register ID:** KDD-085 (sign-over-P2P, shipped), KDD-110 (address family), BUG-086 (the coordinator's
handling of unreachable members, fixed in `v0.5.1-testnet`).
**Status:** Current. Corrected 2026-09-15 — this section previously required RPC port 29995 to be
reachable "until KDD-085 lands"; KDD-085 landed in `v0.4.x`, and the requirement it removed had
been stated here as current for a month.

**ptxtestnet ports — open ONE of them at the address you register:**

| Purpose | Port | Must be reachable by |
|---|---|---|
| P2P | **29994** | the network, inbound — this is the signing path |
| RPC | **29995** | nobody. Loopback only. Do not open it, do not forward it |

**The load-bearing requirement:** a signing request reaches your gamemaster over the **P2P**
connection between it and the coordinator, at the address in your DGM registration. If your host
does not accept inbound TCP on 29994 at that address, the coordinator can only reach you while an
outbound connection from your node to it happens to exist; otherwise it dials you, the dial times
out, and your gamemaster is skipped for that round. It stays registered, enabled, synced and
`Ready` on chain — and never signs. Measured 2026-09-12: five registered gamemasters on one
provider did not accept inbound connections and were the reason every roll in their quorums took
10–14 s on `v0.5.0-testnet` (BUG-086). `v0.5.1-testnet` makes the coordinator stop paying for
them; it does not make them sign.

**What an operator must do:**
1. `listen=1`, and open **29994** inbound in every firewall and cloud security group in front of
   the address you registered. Nothing else. See the operator guide's ports section.
2. Do **not** open 29995. It is a local admin interface; exposing it publishes your RPC.
3. Verify from **outside** the host that 29994 answers at your registered address. `self-check.sh`
   does this; a local probe reaches localhost and hides exactly this fault.

**Why the earlier text was wrong:** before KDD-085 the signing fan-out dialled each member's RPC
port, so 29995 had to be open and this section said so. KDD-085 moved signing to P2P and the
operator guide was corrected at the time; this section was not.

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


## 14. Open consensus and gate items surfaced by the 2026-09-09 verification pass

The following register items were confirmed against source on 2026-09-09 as open in the shipped
tree. They are listed by **class and status only**; reproduction detail is deliberately withheld,
because third parties operate nodes on this network. (Items found *fixed* in v0.4.4-testnet during
the same pass — the dseg-partition pair BUG-074/075 and the tx_id-exclusion leak BUG-035 — are not
limitations and are not listed here.)

**BUG-052 — denial of service (consensus validation ordering). OPEN, fix scoped, not built.**
A malformed transaction can force an expensive signature verification before the cheaper checks that
would reject it, from an unauthenticated peer at no cost, and the rejection is not currently
penalised. The fix is scoped (defer the expensive step to after the cheap checks, then score the
rejection) but not yet implemented. Trigger detail withheld. Register: BUG-052 / KDD-103.

**BUG-062 — chain-halt condition (registration-collateral check asymmetry). OPEN; consequence mitigated.**
A registration transaction's collateral is checked both at mempool acceptance and at block connect,
but against different views of the coin set: at mempool acceptance the transaction's own inputs have
not yet been applied, so a collateral the transaction itself spends still reads as unspent and the
check passes; at block connect the inputs are applied before the check runs, the collateral reads as
spent, and the block is rejected. A transaction shaped that way is therefore accepted into the
mempool yet rejected from every block. The chain-halt consequence is mitigated (the block assembler
now evicts the offending transaction rather than discarding the block), but the underlying
accept-versus-connect asymmetry remains. Trigger detail withheld. Register: BUG-062 (mechanism
corrected 2026-09-13; the earlier wording "validated only at block-connect time" was inaccurate).

**BUG-051 — verifiability: the written seed formula does not match the implementation. OPEN.**
This matters to integrators and costs nothing to disclose. On-chain verification is unaffected —
every settlement verifies and the chain is internally consistent. But the published seed formula
(KDD-002) names a `caller_pubkey` input that the implementation does not use on a commitment: the
field carries the caller *salt*, which reaches the seed only via the nonce. A verifier built from
the written specification will therefore derive the wrong seed on every roll commitment and cannot
distinguish a naming defect from a forged payload. **Until the specification and code are reconciled,
build verifiers from the implementation (`src/rpc/ptx.cpp`, `src/ptx/ptx_seed.cpp`), not from the
KDD-002 formula as written.** Register: BUG-051.

**BUG-031 — mainnet activation gate (does not affect this testnet). OPEN, undetermined.**
A structural gate owed to the mainnet PTX-activation path: a fork binary may refuse to start on a
pre-activation mainnet datadir. It does not affect ptxbea / ptxtestnet, where PTX is always active.
The source state was not isolated during the pass; it stays open and undetermined. Register: BUG-031.

**ODC-041 — mainnet activation gate: PTX interacting with an active payment/governance regime. OPEN investigation.**
An untested interaction reserved for mainnet, where PTX would run alongside an active budget and
governance regime. Not a defect and not resolved; its testnet manifestations (the dseg-partition
pair) are fixed. Register: ODC-041.
