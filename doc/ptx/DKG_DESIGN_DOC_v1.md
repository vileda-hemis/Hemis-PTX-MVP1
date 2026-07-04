# DKG Design Document — v1

**Status:** Draft for review. Decisions recorded; open items explicitly flagged.
**Authored:** 2026-06-03
**Peer of:** `ODC_022_DESIGN_DOC_v3.md` (lottery pool), `PTX_LE_STANDUP.md` (operational log)
**Supersedes:** the operator-trusted framing in `PTX_LE_STANDUP.md §E` (now retired — see §2)
**Register entries:** KDD-039 through KDD-047, ODC-024, ODC-025; ODC-021 status → resolved-by-DKG

---

## §1 Purpose & Scope

### §1.1 What DKG replaces

The current PTX implementation uses **trusted-dealer keying**: the originating operator generates
`master_sk` once, distributes BLS key shares to the 11 GMs, and holds the only copy. A permanent
loss of that operator stops all PTX rolls. The coordinator is a single point of failure for the
signing stack.

**DKG (distributed key generation) replaces this.** With Feldman VSS + GJKR commit-then-reveal
hardening (KDD-051), no single party ever holds `master_sk`. Each quorum of GMs collectively
generates its own threshold keypair via a multi-round ceremony, producing a shared `group_pk`
that is verifiably established. The operator becomes a stateless orchestrator, not a keyholder.
The coordinator SPOF (ODC-021) is eliminated.
*(Construction corrected from "Pedersen" to measured-Feldman+GJKR per KDD-051, 2026-06-03.)*

### §1.2 Scope

This document covers the design decisions for:

- **Quorum keying** — how a quorum generates and holds its threshold keypair
- **Quorum formation** — how GMs are selected into quorums
- **Quorum rotation** — how quorums refresh their keys over time
- **Quorum disband** — how an inquorate quorum is retired
- **GM economics** — what drives participation (masternode rewards + PTX lottery bonus)

### §1.3 Out of scope

- **The RNG/lottery protocol itself** — already designed and built; see `ODC_022_DESIGN_DOC_v3.md`
  for PTXCOALESCE/PTXPAYOUT and the accumulator model.
- **Implementation plan** — unblocked as of 2026-06-03 (§9.4 resolved; see §9.4 for detail).
  Scoped to three build items: (a) distributed DKG ceremony + PTX-key bridge, (b) rotation and
  lifecycle machinery, (c) ceremony-orchestration correctness validation (differential testing +
  audit). Implementation plan is the next design artifact.
- **Claims reconciliation** — the pass over present-tense "trustless / verifiable" claims in
  existing docs is a separate deliverable (see `PTX_LE_STANDUP.md §E3`).

---

## §2 Decided: Sequencing — DKG Before Public Testnet

**Decision:** DKG ships **before** public testnet launch. The public testnet IS the decentralised
launch. There is no operator-trusted intermediate network.

**Rationale:** The network-works success criteria are:

1. Self-sufficient with no coordinator SPOF
2. All registered GMs can participate in quorums (no operator-selected subset)
3. PTX callers are quorum-agnostic (they do not specify which quorum handles their roll)
4. An inquorate quorum disbands and its members reform into new quorums

These criteria ARE the DKG deliverables. "The network works" means "DKG is done." There is no
meaningful intermediate state where the network is publicly useful without DKG — an
operator-trusted testnet would require explicit trust in the originating operator for every roll,
which contradicts the product proposition and creates false impressions about the security model.

**Retired as moot:**

- The *operator-trusted MVP framing* (`PTX_LE_STANDUP.md §E`, "initial release is an explicitly
  operator-trusted MVP; cryptographic trustlessness is Phase 3") is retired. DKG precedes public
  launch; there is no explicitly-trusted public network.
- *Option-1 authenticated-transport workaround* (ODC-022 Solution 1 context, external-GM signing
  pre-DKG) was already rejected in B.1 close (`PTX_LE_STANDUP.md §B.1`). This decision
  corroborates that rejection: a pre-DKG signing ceremony would create false-verifiability risk
  with no path to public launch.

**ODC-021 status update:** ODC-021 (coordinator SPOF) is resolved by this decision. DKG delivers
the fix; no separate ODC-021 resolution action is required.

**KDD:** KDD-039

---

## §3 Decided: Quorum Parameters

**n = 11 GMs per quorum, t = 6 signing threshold (6-of-11).** Both decided. *(See §9.1 for the
explicit resolution of the t=6/t=8/all-11 discrepancy.)*

**n=11 rationale:** confirmed by existing fleet topology (ptx-bea) and security testing reference.
A quorum of 11 is the atomic unit; PTX requires exactly 11 GMs to form a quorum.

**t=6 rationale (decided 2026-06-03, starting value — see upgrade path below):** t=6 (6-of-11,
54%) chosen for maximum liveness and resilience:

- Survives 5 GMs simultaneously offline before becoming inquorate. At 37 quorums, this is
  substantial headroom for node churn, connectivity blips, and maintenance windows.
- Minimises disband frequency. Each disband triggers a 60+-block reform cycle (DKG ceremony +
  formation overhead); fewer disbands means lower overall churn. t=8 requires only 3 simultaneous
  offline GMs to trigger disband — too fragile given realistic GM availability distributions.
- Minimises empty-pool-reform-failure risk. If a quorum disbands, its 11 members return to pool.
  With ~385–440 GMs day-1 (§10), the pool rarely has 11 idle GMs available for immediate reform
  if many quorums are simultaneously disbanding. Lower t → fewer disbands → lower reform pressure.
  The wargame found t=7 the security/fragility knee; t=6 is chosen as the resilient starting point
  with one step of headroom below the knee.

**Trade-off accepted:** t=6 is the lowest collusion bar of the considered options (6/11 = 54%,
a bare majority). An adversary controlling 6 of 11 GMs in a quorum can bias that quorum's output.
The trade-off is accepted: (a) the probability of 6/11 compromise in a randomly-selected quorum
is substantially lower than 6/11 overall-GM compromise (quorum randomness is the first defence);
(b) per-quorum impact is bounded (§4, single-quorum-per-GM); (c) operational fragility at t=8
was judged a greater real-world risk than collusion at t=6 given day-1 network scale.

**Disband trigger:** a quorum falls below t=6 available members (i.e., ≤5) → inquorate.
The quorum survives down to exactly t=6 members; disband fires at below-t (≤5). The consecutive
inquorate block threshold is **n_disband = 30 blocks** (~30 min at 60s/block). See §7.3 and §9.1.

**Availability rule:** a minimum of 11 available, PoSe-valid, registered GMs must exist to form
any quorum. Below 11 available GMs → PTX is unavailable ("no quorum"); ptx_roll returns an
appropriate error. This is not a degraded mode — PTX either has a full quorum or it does not.

**t is upgrade-gated, not spork-controlled.** t is baked into each quorum's DKG ceremony at
formation time — a quorum's threshold keypair is generated for exactly t-of-n and cannot be
changed mid-lifecycle without a full re-DKG. Spork control over t would therefore be
meaningless for live quorums (existing quorums cannot retune) and deceptive (operators would
see a spork value that doesn't reflect current quorum behaviour). More fundamentally: allowing
runtime operator control over a security threshold contradicts KDD-039's removal of
operator-controlled security properties. t is therefore a **consensus constant**, changeable
only via a coordinated network upgrade (applies to quorums forming *after* the upgrade height —
a clean transition with no mixed-threshold quorums). The upgrade path to t=7 is pre-documented
by the wargame finding; the case is already made.

**KDD:** KDD-048

---

## §4 Decided: Single-Quorum-per-GM

**Decision:** Each GM is a member of at most one quorum at any time.

**Rationale:**

A GM that has been selected into a quorum is exclusively committed to that quorum's lifecycle
(formation, key rotation, signing). Multi-membership — where one GM participates in two or more
quorums simultaneously — was considered and rejected.

**Why single-membership is more attack-resistant:** If a GM is compromised, single-membership
confines the damage to exactly one quorum. Multi-membership would allow a single compromised GM
to degrade security across multiple quorums simultaneously; with multi-membership a coordinated
attacker needs to compromise fewer distinct GMs to threaten a larger fraction of the network's
quorums.

**Why multi-membership was rejected:**

| Consideration | Assessment |
|---|---|
| DKG complexity | 2–3× additional complexity on the most security-critical code path |
| Security | Worse than single-membership (one compromised GM reaches multiple quorums) |
| Throughput necessity | Not needed at launch (35–40 quorums, 385–440 GMs covers all registered GMs day-1; see §10) |
| "Possible, therefore do it" | Explicitly rejected. The test is needed/cost/risk, not feasibility. |

**Rejected alternative recorded:** multi-membership is a possible future additive extension
(registered as ODC-024) once single-quorum behaviour is proven at scale and DKG complexity is
understood. It is not a launch requirement.

**KDD:** KDD-040

---

## §5 Decided: GM Economics

### §5.1 Masternode rewards — the backbone

GMs earn standard masternode block rewards: **2.6749… HMS per block**, independent of PTX activity.
This is the same reward any Hemis masternode earns (1000 HMS collateral on mainnet).

This is the participation backbone. A GM operator is profitable simply by being a registered,
PoSe-valid masternode — regardless of whether they are in a quorum, regardless of whether any
ptx_roll calls are made. PTX quorum selection and lottery activity are a bonus on top; they do
not determine whether running a GM is economically viable.

**Design intent:** de-risking the "will anyone run GMs?" question. Operators do not need to
forecast PTX demand to justify their collateral. The economic floor is the masternode reward,
which is unconditional.

**KDD:** KDD-042

### §5.2 PTX lottery — the bonus

Callers pay **1 HMS per ptx_roll call** (originator's wallet, per call). These fees:

1. Are collected via PTXCOALESCE into the lottery accumulator (see `ODC_022_DESIGN_DOC_v3.md §2`)
2. Accumulate across the payout window
3. Are disbursed by PTXPAYOUT to a single ticket-weighted lottery winner at window close

The roll fee is:

- **Spork-adjustable** — no fork required to change the fee value. This allows the fee to be
  calibrated post-launch without a consensus upgrade.
- **Atomic with the roll result** — a failed roll (any post-funding failure that causes
  `unlockFundedInputs` to fire, per BUG-018 fix at commit `801c557`) does not charge the caller.
  The fee lives inside the settlement transaction; if the roll does not land on-chain, the UTXO
  is released and no fee is paid. There is no separate refund path.
- **The anti-spam mechanism** — the roll cost is the sole rate-limiting mechanism for ptx_roll.
  No allowlists, no rate limits, no per-caller quotas. Cost = deterrent.

**KDD:** KDD-043 (x-ref BUG-018)

---

## §6 Decided: Ticket-Based Lottery

**Decision:** GM lottery eligibility and win probability are determined by a ticket ledger.

**Ticket earning:**

| Source | Rate |
|---|---|
| Registered as PoSe-valid GM (per payout window) | 1 eligibility ticket |
| Signing a ptx_roll (per roll signed) | 5 signing tickets |

The 1:5 ratio means signing GMs accumulate tickets 5× faster than passive GMs in a given
window — participation is rewarded proportionally more than mere registration.

**Luck is accepted for signing-ticket earning:** a GM in a busy quorum (high roll volume) or a
lucky quorum (happens to be selected for many rolls) earns more signing tickets per window.
This is an accepted outcome. Ticket earning rate is a function of quorum activity, which varies.
GMs are not penalised for being in a quiet quorum.

**Per-GM ledger, not per-quorum:** tickets are tracked against the individual GM, not the quorum.
When a GM rotates, is ejected, is disbanded, or returns to the waiting pool, its accumulated
tickets carry forward. Tickets are never lost due to lifecycle events.

**Win probability:** proportional to total tickets held at payout. A GM with 100 tickets has
twice the win probability of a GM with 50 tickets. The ledger resets to zero for all GMs at
PTXPAYOUT.

**Payout windows:** 60 blocks on testnet, 1440 blocks on mainnet (~24h at 60s/block).

**KDD:** KDD-041

---

## §7 Decided: Quorum Lifecycle

### §7.1 Formation

**Decision:** quorums form in batches of 11 from the waiting pool, via DKG. Random selection from
the set of available GMs not currently assigned to a quorum.

**Waiting pool:** newly registered GMs, GMs returned from a disbanded quorum, and GMs that were
ejected (re-selectable) all enter the waiting pool. While in the pool, a GM earns the masternode
block reward and earns one eligibility ticket per payout window. It does not earn signing tickets
(signing requires quorum membership).

**Batch-only formation:** exactly 11 GMs are selected simultaneously to form each new quorum. No
partial formation, no incremental assembly. A quorum is either fully formed (11 members, DKG
ceremony complete) or does not exist.

**Rationale for batch-only:** allows a partial-reformation attack vector where an adversary
controlling some GMs could repeatedly trigger partial quorum attempts to stall formation or probe
other participants' key material. Batch-only eliminates this: the formation set is fixed before
any DKG ceremony step begins.

**KDD:** KDD-044

### §7.2 Rotation

**Decision:** healthy quorums rotate their keys via **same-set re-DKG** — the existing 11 members
re-run the DKG ceremony among themselves to produce a fresh threshold keypair. Quorum membership
does not change during rotation; only the keypair refreshes.

**Rotation interval:** N = 1440 blocks + per-quorum drift (the staggering mechanism). Drift is
assigned at formation and is unique per quorum, ensuring no two quorums rotate simultaneously.
**N=1440 is a starting value only — it is to-confirm pending measured DKG ceremony duration
(see §9.2).**

**Staggering rationale:** with 35–40 quorums and N≈1440, approximately one quorum rotates per
~39 blocks. Rotations are never network-synchronised. At any given moment, at most 1 quorum is
mid-ceremony (unavailable for signing); the other ~36–39 are fully available. Network-synchronised
rotation — all quorums rotating at the same time — would produce periodic total PTX outage and is
explicitly rejected.

**Same-set rationale:** dissolve-and-rebatch (where rotation produces a new randomly-composed
quorum) was considered and rejected. Rebatching injects luck into quorum position: a working GM
that was correctly participating would be arbitrarily pooled and lose its quorum-member signing
opportunity, which is the class of outcome the governing principle below rejects.

**Governing principle (position, not outcome):** *Luck is acceptable for lottery outcomes. An
arbitrary penalty resulting in complete loss of a GM's signed position is not.* A GM that is
performing correctly should not lose its quorum membership due to random rebatching. Same-set
rotation preserves this: a GM that has been behaving correctly retains its position. Ejection
(§8) and disband (§7.3) are the only involuntary exits, and both are triggered by verifiable
behaviour (missed signings) or confirmed inquorate status — not arbitrary chance.

**During rotation:** the quorum is unavailable for signing while the DKG ceremony is in progress.
The routing layer skips mid-ceremony quorums (see §7.4).

**KDD:** KDD-045

### §7.3 Disband

**Decision:** a quorum that has been inquorate (below t=6 signing-capable members, i.e. ≤5
available) for **n_disband = 30 consecutive blocks** (~30 minutes at 60s/block) is disbanded.

**n_disband=30 rationale:** the cost asymmetry strongly favours waiting. A dead quorum sitting
inquorate-but-not-disbanded for 30 min costs almost nothing — it is one of 35–40 quorums, the
routing layer skips it (§7.4), and the other ~36–39 quorums continue serving rolls. By contrast,
false-disbanding on a transient network event (partition, propagation stall, brief unreachability)
triggers a disband→reform cascade — 60+-block recovery cycles per quorum — in response to a blip
that heals itself. 30 blocks gives a transient event time to heal and *un-trigger* the disband:
if enough GMs return within the window and the inquorate counter resets, no disband fires.
n_disband=5 was explicitly rejected as an eager failure-detector vulnerable to converting transient
network events into self-inflicted cascades.

**n_disband is tunable policy, not a consensus constant.** It is not baked into any quorum's DKG
ceremony and does not affect the threshold keypair. It can be shortened on measured evidence from
testnet without a network upgrade. The conservative-long starting value is the deliberate bias.

**On disband — dissolve to general pool:**
- Surviving members return to the **general waiting pool**. They are not preferentially re-assembled.
  The next quorum they join is a fresh random batch-of-11 from the pool (§7.1) — the disbanded
  quorum's partial survivor set is never re-formed together.
- Members are fully reusable: they cycle through the pool and are re-selectable into any future
  formation. The set is not re-formed; individual members are.
- Accumulated lottery tickets carry forward (pause, not loss — governing principle from §7.2).
- The quorum's threshold keypair is abandoned. The DKG re-ceremony at the next formation produces
  a fresh keypair; the abandoned keypair is irrecoverable by design.
- Ejected members are PoSe-decayed according to §8.

**Contrast with rotation (§7.2):** rotation IS same-set re-DKG — the distinction is fundamental.
Rotation refreshes a *healthy* quorum's keypair with stable membership (same 11 GMs, new key).
Disband scatters a *failed* quorum's survivors to the pool for uniform random re-batching.
Re-assembling a failed quorum's survivors would reintroduce the partial-reformation attack surface
that batch-only formation (§7.1) eliminates. The two "reuse" senses are therefore different:
GM members are reusable across formations (yes); the same GM set is not re-formed on disband (no).

**The one residual involuntary event:** a blameless GM whose quorum disbands around it — because
other members were ejected and the quorum fell below t — is returned to the pool through no fault
of its own. This is accepted: the consequence is pause (return to pool, tickets intact,
re-selectable) not loss. Under the governing principle, temporary interruption of position is
acceptable; permanent destruction is not.

**KDD:** KDD-047 (n_disband=30 and dissolve-to-pool are addenda to this entry)

### §7.4 Routing

**Decision:** the network routes ptx_roll calls only to quorums that are in a fully-signing-ready
state (formation complete, not mid-rotation ceremony). Mid-ceremony quorums are skipped.

**Caller-agnostic routing:** callers specify neither a quorum nor a signing endpoint. The routing
layer selects among available quorums. This is a design requirement: a caller should have no
ability to target a specific quorum, which would enable quorum-targeting attacks and would require
callers to track quorum topology.

---

## §8 Decided: Ejection and PoSe Discipline

**Decision:** ejection from a quorum is based on **missed signing opportunities**, not elapsed
blocks.

**Counter definition:** the PoSe score increments only when a GM that was presented with a signing
request failed to respond (a missed signing opportunity). A GM in a quiet quorum that receives
few signing requests accumulates no penalty for the quiet period. Penalising GMs for elapsed time
without signing requests would punish GMs arbitrarily for being in low-traffic quorums, which
violates the governing principle from §7.2.

**Thresholds:**

| Event | Threshold | Consequence |
|---|---|---|
| Missed signing opportunities in rolling window | 15 of 60 | Ejected from quorum; returned to waiting pool; re-selectable |
| Continuous PoSe-fail accumulation | 120 | PoSe-banned; requires manual `protx update` to recover |

**No slashing. No MAD (mutually-assured destruction).** Ejection returns the GM to the pool where
it is immediately re-selectable. PoSe-ban requires operator action to recover but does not destroy
collateral. The penalty regime is recoverable and not terminal.

**Rationale:** ejection discipline must be harsh enough to remove non-participating GMs from
quorums (preventing stalled signing) but recoverable enough to not destroy honest operators who
experience transient connectivity issues. Collateral loss would deter participation and is
disproportionate to the offence; recoverable ejection achieves the liveness goal without that
deterrent cost.

**KDD:** KDD-046

---

## §9 Open / To-Confirm

*Items in this section are NOT YET DECIDED. They must be confirmed before any implementation
begins on the affected subsystem.*

### §9.1 Quorum parameters — all decided, section closed

All three parameters are now decided. This subsection records the resolution history; no open
items remain here.

- **n=11 (decided):** Confirmed by fleet topology and security testing reference. See §3.

- **t=6 (decided 2026-06-03, KDD-048):** Full rationale in §3. Resolution of the discrepancy:
  - **The security testing reference doc's t=8 is NOT adopted.** t=8 (3 GMs offline = inquorate)
    was judged operationally fragile: frequent disbands at day-1 scale, each triggering a 60+-block
    recovery cycle, with empty-pool-reform-failure risk when multiple quorums disband concurrently.
    Wargame finding: t=7 is the security/fragility knee; t=6 is the resilient starting point below
    the knee.
  - **The current ptx-bea all-11 deployment is superseded.** All-11 signing (no threshold below n)
    is the trusted-dealer regime: the coordinator fans out to all 11 and requires all 11 to respond.
    With DKG, the threshold is a cryptographic property of the shared keypair, not a connectivity
    requirement. all-11 does not carry over.
  - **Upgrade path pre-documented:** t=7 is the documented upgrade target if day-1 operational
    data shows collusion risk outweighs fragility risk. Upgrade is network-upgrade-gated (consensus
    constant); applies to quorums forming after the upgrade height. See §3 for full upgrade-path
    rationale.

- **n_disband=30 consecutive inquorate blocks (decided 2026-06-03, KDD-047 addendum):** Decided.
  See §7.3 for full rationale. Conservative-long by deliberate choice; tunable policy (not baked
  into DKG, not consensus-critical), so it can be shortened on measured evidence.

### §9.2 Rotation interval N (§7.2)

**Pending measured DKG ceremony duration.** The constraint is:

```
rotation_spacing (≈ interval / quorum_count ≈ 1440/37 ≈ 39 blocks) 
    > DKG_ceremony_duration (in blocks)
```

If a DKG ceremony takes longer than the rotation spacing, two quorums could be simultaneously
mid-ceremony, degrading availability below the ~97% target. The 1440-block starting value must
be validated against actual measured ceremony duration before being locked. The ceremony duration
is implementation-dependent and requires a prototype ceremony benchmark. The reuse question
(§9.4) is now resolved; the benchmark remains outstanding (ODC-025 still open).

**Future ODC:** ODC-025 tracks this as the open choice for rotation-N final value.

> **[2026-06-03, KDD-051]** The ceremony-duration benchmark gating rotation-N MUST run on the
> GJKR-hardened ceremony (commit phase + one added gossip round included), not plain Feldman.
> Benchmarking the unhardened ceremony would measure rotation-N against a construction PTX will
> not ship.

### §9.3 Multi-quorum membership (§4)

Deferred as a future additive extension. It does not block DKG launch. It extends the DKG
architecture after single-quorum behaviour is proven at scale.

**Future ODC:** ODC-024 tracks this as the open design choice.

> **[Partially resolved 2026-06-03 per KDD-053]** Roll-selection input (Option D,
> ungrindable block-hash-based) and failover model (verifiable re-route vs. re-roll) are
> decided. Multi-quorum membership and scaling (quorum-count targets) remain open under
> ODC-024.

### §9.4 The reuse question — RESOLVED 2026-06-03

**Resolution:** 2026-06-03. Read-only source investigation of hemis-ptx (file:line evidence).
Outcome: **MIXED** — neither "integrate existing DKG" nor "build threshold crypto from scratch."

---

#### Present and reusable

**Threshold BLS primitives — done and exercised.** Both chiabls (LLMQ layer) and blst (PTX layer)
threshold stacks are compiled, linked, and exercised in the running binary. PTX already has working
Shamir share generation, partial signing, and Lagrange recovery in G2:
- `ptx_bls.cpp:22` — `PTX_BLS_Init()`: master polynomial + per-GM share evaluation
- `ptx_bls.cpp:121` — `PTX_BLS_PartialSign()`: partial signature with a key share
- `ptx_bls.cpp:146` — `PTX_BLS_Recover()`: Lagrange interpolation in G2 over BLS12-381

The hard, dangerous cryptography is **not** being built from scratch — it exists and runs. This
is the critical de-risker: the catastrophic-uncertainty case (primitive-from-scratch) is ruled out.

**A complete reference DKG implementation exists in-tree.** `src/llmq/quorums_dkgsession*` is
present, compiled, active, threaded, and P2P-wired. All four DKG messages are dispatched:
QCONTRIB, QCOMPLAINT, QJUSTIFICATION, QPCOMMITMENT (`quorums_dkgsessionhandler.cpp:131–142`).
Phase advancement fires on `UpdatedBlockTip()`. It runs live for ChainLocks. This is a studyable,
adaptable reference for multi-party DKG over gossip in our own lineage, language, and BLS
substrate — and the live recovery path (`quorums_signing_shares.cpp:603–662`,
`recoveredSig.Recover()`) gives an in-tree target for differential-testing PTX's Lagrange
recovery against.

**Quorum params are parameterized.** `n=11, t=6` is a new entry in `Consensus::LLMQParams`
(`chainparams.cpp`) — no hardcoded size constraints to fight.

---

#### Not present — the build work

**No wire from DKG output to PTX keys.** PTX built a parallel, trusted-dealer key stack
(`PTXBLSState`, coordinator holds the master polynomial — `ptx_bls.h:27`) that is
namespace-isolated from LLMQ (zero `llmq::` dependencies in `src/ptx/`). LLMQ DKG produces
keys consumed only by ChainLocks; nothing feeds into the PTX signing path. The bridge must be
built.

**No rotation machinery in the lineage.** No DIP-0024, no rotating-quorum support anywhere in
`src/llmq/`. Same-set re-DKG rotation + interval+drift trigger are net-new.

**Trigger model mismatch.** LLMQ DKG is height-deterministic: triggers at `height % dkgInterval`
(`quorums_dkgsessionhandler.cpp:107–129`). PTX needs pool-availability-triggered formation and
interval+drift rotation — this must be built.

---

#### Resolution framing

This is **neither** "integrate existing DKG" (the wire to PTX doesn't exist) **nor** "build
threshold crypto from scratch" (the threshold primitives are done and exercised — worst-case is
ruled out). It is: **build the distributed DKG ceremony + the PTX-key bridge + the
rotation/trigger lifecycle**, adapting the in-tree LLMQ DKG as reference, on top of working
blst threshold primitives.

Dominant remaining risks: (a) **ceremony-orchestration correctness** — adapting the pattern without
introducing a keying bug → differential testing against the in-tree LLMQ recovery path + external
cryptographic audit are non-negotiable; audit has months of lead time, engage early.
(b) **The net-new rotation/lifecycle machinery.** Both tractable.

Feasibility note: the threshold-primitives-present finding rules out the catastrophic-uncertainty
case. A 12–18-month mainnet timeline is achievable given the build (not configure, not from-scratch)
scope, **provided the validation discipline holds** (differential testing + audit).

---

#### Gate lifted

§9.4 is resolved. The **implementation plan is now unblocked** and is the next design artifact.
Scope: the three build items above — (a) ceremony + PTX-key bridge, (b) rotation/lifecycle
machinery, (c) correctness validation discipline (differential testing + audit). No implementation
plan was written prior to this resolution, per the standup gate in §1.3.

### §9.5 Complaint/justify timing window — P2 and P3 phase duration (ODC-026)

**Open design choice. Resolution is DOWNSTREAM of and must be consistent with the
wall-clock/block-time timing audit (Carry-forwards D and E, grouped with ODC-025).**

The P2 (Complaint) and P3 (Justify) phases each require a duration long enough that all
well-behaved QUAL members can complete their processing and broadcast their messages
before the phase closes, under realistic P2P propagation conditions. Setting the window
too short risks honest members missing the deadline; too long stalls the ceremony.

The duration may be specified in blocks (deterministic, consensus-derivable, but couples
ceremony progress to block production rate) or wall-clock seconds (decoupled from block
rate, but introduces a non-deterministic element that complicates replay and unit testing).
This choice is the same wall-clock-vs-block trade-off that the timing audit must resolve.
Choosing the P2/P3 window duration before the audit concludes would prejudge the audit's
core question; ODC-026 is therefore intentionally downstream of the audit, not parallel to
it. Once the audit establishes the timing model, ODC-026 closes immediately by applying
that model to the P2/P3 window lengths.

**What is not open:** the marking rules that apply when a phase closes are decided
(KDD-055). What is open is only the duration of the window before `ClosePhase2()` /
`ClosePhase3()` is called.

**Implementation posture during W1.2:** `ClosePhase2()` and `ClosePhase3()` are called
explicitly by the session driver. No internal timer or block-height gate is compiled into
the P2/P3 functions. The duration parameter is a deployment configuration, not a protocol
constant, until this ODC is resolved.

**Dependency:** wall-clock/block-time timing audit (Carry-forward D), grouped with
ODC-025 (rotation-N / ceremony duration benchmark) and Carry-forward E (W3.2 audit scope).
ODC-026 resolves last, after the timing model is established.

**ODC:** ODC-026

---

### §9.6 vvec_hash scope in PTXDKGPayload premature commitment (ODC-027)

**Open design choice. W1.2 uses vvec[0]-only; full-vvec scope deferred to post-audit
W3.2 input.**

`PTXDKGPayload.vvec_hash` is a `uint256` computed as SHA256 over the concatenated
compressed G1 bytes of each effective-QUAL member's `vvec[0]` — only the degree-zero
coefficient, which is the DKG share of the group public key. This commits only the G1
points that were summed to produce `group_pk_bytes`.

The open question is whether `vvec_hash` should instead commit the **full** vvec
(all `t` coefficients per member), covering the Feldman VSS verification points used
during P2 complaint/justify. A full-vvec hash would allow independent on-chain
auditors to reconstruct the VSS check without replaying P2/P3 messages, at the cost
of a larger hash preimage and a more complex `CheckPTXDKGTx` deserialisation path.

This extension is not required for W1.2 functional correctness. Resolution depends on
audit scope decisions (W3.2 input): if an on-chain VSS-replay audit is planned,
full-vvec is the right default; if audit is off-chain, vvec[0]-only suffices.

**Raised:** 2026-06-12. Registered from P4P5_PREIMPL_APPROVED.md S4 / GF5 finding.
W1.2 default (vvec[0]-only) decided as S4 (approved).

**Update (2026-06-12, W1.3 recon):** Confirmed the per-member vvecs are NOT in
PTXDKGPayload — only vvec_hash (irreversible digest) and each member's vvec[0] via
group_pk_bytes. Consequence: Feldman VSS share-correctness is not on-chain verifiable.
W1.3 validation (KDD-059) therefore adopts attestation-counting, not share-correctness
verification. Resolving ODC-027 toward full-vvec-on-chain would upgrade the on-chain
guarantee from attestation to share-correctness. Remains OPEN, deferred to W3.2 audit.

**ODC:** ODC-027

---

### §9.7 sk_share commitment in PTXDKGPhase4Msg (ODC-028)

**Open design choice. Not required for W1.2; deferred to W3.2 audit input.**

Whether `PTXDKGPhase4Msg` (the premature commitment message embedded in
`PTXDKGPayload.premit_commitments`) should include a 48-byte G1 element
`g^{sk_share_i}` — a proof-of-knowledge of the member's DKG secret share — is
undecided.

Including `g^{sk_share_i}` would allow any node with access to the PTXDKG transaction
to verify that a premature-commitment signer knows a share consistent with the group
public key, without knowing the share itself (a discrete-log proof). This strengthens
the accountability model and is a standard inclusion in GJKR-style DKG audit trails.

The cost is 48 bytes per commitment in the PTXDKG payload (~288 bytes at t=6) and
requires `CheckPTXDKGTx` to verify the G1 element compresses cleanly. Full
proof-of-knowledge (Schnorr-on-G1) is heavier and is a separate question.

Resolution depends on W3.2 audit requirements. Not required for W1.2 threshold signing
to function correctly.

**Raised:** 2026-06-12. Registered from P4P5_PREIMPL_APPROVED.md GF5 finding.

**ODC:** ODC-028

---

### §9.8 PTXDKG submission model (ODC-029)

**Open design choice. Source is silent; resolve before W1.3.**

Whether PTXDKG transits the mempool as a relayed transaction, or is constructed and
included directly by a ceremony coordinator who is also a block-builder, is undecided.
`DKG_IMPLEMENTATION_PLAN_v1.md` IMP-D3 says "mined" — model-neutral, consistent with
either mechanism. The coordinator role spec §6 establishes "any node can submit" as a
principle but describes the roll-settlement function, not PTXDKG specifically, and does
not choose between relay and direct-injection.

**Candidate decisions:**

(a) **Mempool-relay:** PTXDKG broadcasts to the P2P network; any GM or ceremony observer
submits; miners include from their mempool. Requires `AcceptToMemoryPool` to accept PTXDKG.
Anti-spam and relay DoS rules are consensus-adjacent (a W1.3/hardening item if this model
is chosen).

(b) **Direct-block-inject:** the ceremony coordinator who is also a block-builder includes
PTXDKG directly in the next block it produces, bypassing the mempool (analogous to
PTXCOALESCE/PTXPAYOUT). Requires `blockassembler.cpp` machinery and explicit mempool
rejection.

**W1.2 scope boundary:** `PTX_DKG_BuildPTXDKGTx` is CONSTRUCT-ONLY — it returns a
`CMutableTransaction` but does not submit. The submission call site is gated on this ODC.

**Entanglement with W1.3 validation:** the relay/DoS rules and the deferred PTXDKG
threshold-signature consensus validation (second-highest chain-split risk, §8 of the P5
pre-impl report) co-depend — choose the model before writing W1.3 validation rules.

**Raised:** 2026-06-12. Registered from P4P5_PREIMPL_APPROVED.md C1 finding. Implemented
as construct-only in commit bae1dcf (W1.2 P5).

**ODC:** ODC-029

---

### §9.9 PTXDKG acceptance window and per-formation uniqueness (ODC-030)

**Open.** Two lifecycle-bound acceptance rules deliberately deferred to W2.

1. **Staleness bound:** LLMQ enforces max-age via `cacheDkgInterval` (specialtx_validation.cpp:565–567); PTX has no interval analog and no formation cadence yet. Inventing a consensus constant before W2 defines rotation cadence is guessing. W1.3 ships V1–V3 (existence, height-consistency, ancestor) with **no max-age bound**.

2. **Cross-block per-formation uniqueness:** "one accepted PTXDKG per formation absent rotation/disband" needs a chain-state index (quorum_hash → accepted txid) maintained in Connect/DisconnectBlock, and its legality condition is exactly W2's rotation semantics. Interim risk is bounded: a *conflicting* second group_pk for the same formation requires ≥ t members signing both — outside the KDD-059 trust boundary; a *duplicate* identical PTXDKG is chain bloat, mitigated by the one-per-block rule (W1.3_VALIDATION_SPEC_v1 §4.4) and the §C1 node-side guard (W1.3_VALIDATION_SPEC_v1 §5.5).

**Resolve with:** W2 lifecycle design. The two clauses may split into separate ODCs if W2 resolves them on different timelines.

**Raised:** 2026-06-13. Registered from W1.3_VALIDATION_SPEC_v1 §6.

**ODC:** ODC-030

### §9.10 V1–V4 anchoring-chain coverage bound to Package 3 (ODC-031)

**PARTIALLY CLOSED (C6/C7, 2026-07-04).** `CheckPTXDKGTx`'s contextual anchoring checks — V1
`LookupBlockIndex`, V2 height, V3 `pindexPrev->GetAncestor` reorg-safety, V4
`GetListForBlock` — cannot be exercised by unit tests in the test_ptx-linked context:
they need a real `CBlockIndex` at a formation height with a populated DGM snapshot, and
the `TestChainSetup` chain-fixture layer does not RUN in test_ptx (C-3 spike finding;
the runtime face of the documented umbrella rot). The only path that exercises them is
driving a real PTXDKG through the acceptance path, which requires Package 3's tx_verify
exemption + block-inject wiring (`W1.3_VALIDATION_SPEC_v1 §4`).

**Decision (historical, C-2).** C-2 shipped the validator with V1–V4 reachable but
**unexercised**, and V5–V8/structural unit-tested — the anchoring falsification bound to
Package 3.

**Closure (C6/C7).** The falsification was discharged on a real regtest chain (the h126
banked workbench; Python-over-RPC harness + per-check stub→RED via rebuild). ODC-031 is now
**PARTIALLY CLOSED** — the reachable checks are proven; the remainder is bound to W2.2 by a
hard dependency (a connect-valid payload = datadir-v2, which needs W2 formation
orchestration). Deferred-not-dropped. It is not "closed" and not "open".

- **COMPLETE (falsification-proven, C6/C7):** V1 (unknown quorum_hash), V2 (formation-height
  mismatch), **V3-PREDICATE** (non-ancestor anchor — the *static* half only), **V4**
  (corruption-only guard — source-trace both paths + confirmatory smoke + stub→RED;
  `W1.3_C7_V4_PROPAGATION_PROOF_v1`; PROVEN COMPLETE, NOT deferred), F-5 (mempool rejection),
  populate-refusal (contextual validate-before-inject).
- **BOUND TO W2.2 (hard dependency = datadir-v2; deferred-not-dropped):**
  **V3-REORG-TRANSITION** (anchor was active, orphaned mid-flight, still correctly rejected —
  the chain-split-critical half of V3, NOT covered by V3-PREDICATE), **C3-INVOCATION** (two
  valid PTXDKGs → `ptxdkg-duplicate` through the real `ProcessSpecialTxsInBlock`; explicitly
  NOT unit-covered — FA-2b proved the unit gate is blind to the invocation; exercised at W2.2
  via F-6 accept-path + the datadir-v2 duplicate case), **F-6** (accept path), **F-9**
  (stale-slot skip).

There is no flat "V1–V4 complete": V3's reorg-transition half and V4 are distinct — V4 is
COMPLETE, V3-REORG-TRANSITION is bound to W2.2.

**Raised:** 2026-06-30 (architecture-chat). Registered from PTX_LE_STANDUP C-2 close-out.
Partially closed 2026-07-04 (C6/C7 close-out).

**ODC:** ODC-031

### §9.11 Can `TestChainSetup` run in test_ptx? (ODC-032)

**Open (deferred).** The C-3 spike found that any `TestChainSetup`-derived chain fixture
hangs in the test_ptx binary at fixture block-mining (the `test/test_Hemis.cpp` genesis
`ActivateBestChain` → first `CreateAndProcessBlock` path), before any registration code
runs — the runtime face of the documented umbrella rot (test_ptx links the fixtures but
they were never exercised in it). Whether the chain-fixture layer can be made to run in
test_ptx is a separate, scoped question. If resolved, the anchoring checks could move to
unit coverage; but the ODC-031 W2.2 remainder binding (V3-REORG-TRANSITION / C3-INVOCATION /
F-6 / F-9, all datadir-v2-bound) stands regardless — ODC-031 is partially closed, not open.

**Raised:** 2026-06-30 (architecture-chat). Candidate/deferred.

**ODC:** ODC-032

---

## §10 Scale Context

**Day-1 mainnet target:** 35–40 quorums, approximately 385–440 GMs (all registered GMs
participating, given the single-quorum-per-GM rule).

At this scale:
- Throughput is not the constraint. 35–40 quorums can serve demand far in excess of realistic
  day-1 roll volume.
- The design priority order is: **correctness first, security second, simplicity third,
  throughput last.**
- Per §7.2: at 37 quorums and N=1440-block rotation, one quorum rotates approximately every
  39 blocks (~39 minutes at 60s/block). PTX availability ≈ 97% at any instant.
- 1000 HMS mainnet collateral. 100 HMS on ptxbea testnet (inherited from existing chainparams).

---

## §11 Decided: DKG Construction — Feldman VSS with GJKR Commit-Then-Reveal Hardening

**KDD-051 (2026-06-03). Source measurement, not terminology.**

**Measured finding.** The LLMQ reference (`src/llmq/quorums_dkgsession*`) implements plain
Feldman VSS: `vvec[j] = G·a_j`, single generator, no blinding polynomial
(`bls_worker.cpp:78–96`). The full verification vector — including `vvec[0] = g^{a_0}` — is
broadcast in plaintext in Phase 1 (`quorums_dkgsession.cpp:176–177`), stored on receipt
(`:300`), with no prior hiding commitment. The "Pedersen VSS" term in earlier documents was a
misnomer; the construction is Feldman.

**Vulnerability (GJKR rushing-bias).** Because `g^{a_0}` is visible before the complaint
window closes, and a member can cause its own disqualification by withholding its contribution
(`MarkBadMember`, `quorums_dkgsession.cpp:407–435`), a rushing member can observe other
members' free-coefficient commitments, then decide whether to remain in the qualified set
(QUAL) — biasing the final `group_pk`, which is summed over QUAL at Phase 4 (`:966–984`).
Precondition: ≥1 valid member beyond `t`, so self-exclusion does not drop below threshold —
realistic in a well-attended 6-of-11 ceremony. The adversary cannot learn `master_sk`, but
can bias the distribution of `group_pk` by selecting among 2^k candidate keys for k
controlled members.

**Decision.** The PTX ceremony uses Feldman commitments hardened with a GJKR commit-then-reveal
phase. The load-bearing security property is **QUAL-locks-before-reveal**:

1. Each member broadcasts a binding hash commitment to its full contribution (verification
   vector + encrypted shares) in a new commit phase, before any vvec is revealed.
2. The qualified set QUAL is finalised — commitment presence/validity plus the complaint and
   justification rounds — entirely within the committed-but-hidden phase, before any member
   reveals its actual vvec.
3. vvec reveal (the present Phase 1 behaviour) occurs only after QUAL is locked. A member
   that committed cannot self-exclude based on observing others' revealed `g^{a_0}`, because
   reveal has not happened when QUAL closes; a member that withholds its reveal after
   committing is itself disqualified, which confers no advantage since it committed before
   observing anything.

This makes `group_pk` unbiasable by ceremony participants — load-bearing for PTX's
provable-fairness claim, and the property most likely to be examined first by the W3.2 audit.

**Cost.** One added gossip round on a per-formation / per-rotation ceremony (daily cadence
per quorum, ~39-block rotation-spacing budget per ODC-025). Within tolerance; not in the
per-roll signing path (signing unchanged). The added round must be included in the ODC-025
ceremony-duration benchmark.

**Status:** Decided. Construction confirmed by source measurement. Prerequisite for W1.2
ceremony implementation.

**KDD:** KDD-051

---

## §12 Decided: PTXDKG Member Set — Committed node_id List, Chain-Determined Order

**KDD-052 (2026-06-03). Resolves OPEN-2 (proTxHash vs node_id).**

**Decision.** The PTXDKG transaction commits the ordered member set as `node_id` (compound
label:suffix per KDD-033). `node_id` is the committed identity because it is consistent with
all existing PTX consensus identity — pose tracker, `PTX_SelectWinner`, PTXPAYOUT validation
all key on `node_id` (`ptx_winner_selection.cpp:47,50,54`; `ptx_pose.*`; `ptx_payout.cpp:31`)
— and is human-readable for explorers and auditors where a raw `proTxHash` is opaque.

**Ordering basis (ungrindable).** Members are committed in the order given by
`SHA256(SHA256(proTxHash, confirmedHash), formation_block_hash)` descending — the LLMQ
`CalculateQuorum` basis (`deterministicgms.cpp` scoring; precedent: `quorums_commitment` /
`specialtx_validation.cpp:575–579`), keyed to the record's committed formation height.
Share-index `i` = 1-indexed position in this committed order. The ordering inputs
(`proTxHash`, `confirmedHash`, formation block hash) are all chain-fixed and none
operator-choosable — an operator cannot influence its share index via label choice. This
preserves the GJKR-aligned posture (KDD-051): no participant influence over
ceremony-determined parameters.

**Why not alphabetical node_id sort.** The current trusted-dealer share-index assignment
(`ptx_bls.cpp:32–35`) sorts `node_id`s alphabetically. The label half of `node_id` is
operator-chosen, making alphabetical position operator-influenceable. For a randomness
beacon, ceremony parameters must not be operator-grindable; the chain-determined score basis
is used instead.

**Order correctness is a formation-time consensus property.** The committed order is
validated when the PTXDKG tx is mined — the DGM list at formation height is available then,
so `node_id`→`proTxHash` resolution for the score recomputation is done at validation. The
order is not re-verified at read time: a later beacon verifier checks the threshold signature
against the committed `group_pk`, which is what the beacon's validity rests on; it does not
re-derive the quorum's share-index ordering. The committed `node_id` list therefore gives
durable, human-readable membership without a read-time dependency on retained historical DGM
state.

**Identity model.** The DGM list is the canonical join table carrying both `proTxHash` and
`node_id` (KDD-033). The PTXDKG record commits `node_id`; `proTxHash` is used only as the
internal, formation-time ordering key, resolved via the DGM list at validation.

**W1.2 carry-forwards:**
1. Share-index assignment changes: `PTX_BLS_Init` (`ptx_bls.cpp:32–35`) currently assigns
   share index by alphabetical `node_id` sort. W1.2 replaces this with chain-determined
   score order.
2. `confirmedHash` precondition: the ordering score uses `confirmedHash`; a member must be
   confirmed at formation height. PTX quorum formation inherits the LLMQ "members must be
   confirmed" eligibility rule — must be enforced in W1.2.

**Status:** Decided. Resolves OPEN-2. Prerequisite for W1.2 (PTXDKG tx format + ceremony
share-index assignment).

**KDD:** KDD-052

---

## §13 Decided: Multi-Quorum Roll Selection and Failover

**KDD-053 (2026-06-03). Partially resolves ODC-024 (selection + failover decided;
membership/scaling remain open).**

**Scope.** Applies when N > 1 active quorums exist (ODC-024 era). Single-quorum (W1.2, N=1)
has no selection — there is one quorum — and this KDD does not affect it. The rules are
decided now to close the design hole; the code is ODC-024-era.

**Selection (deterministic, ungrindable — "Option D").** For each roll, the serving quorum
is computed, never chosen:

```
ordering = deterministic_shuffle(active_set,
               H(anchor_block_hash ‖ game_id ‖ roll_index_in_block))
primary  = ordering[0]
```

The selection input uses only chain/protocol-determined fields. Caller-controlled fields
(caller pubkey, salt) are excluded — even though they appear in the roll seed — so the
caller cannot grind selection. `roll_index_in_block` = the roll's PTXSESS position in the
block's tx ordering (chain-determined). Rejected alternatives: roll-seed input
(caller-grindable via salt — a steering vector); block-height-alone (ungrindable but
long-range predictable, enables pre-positioning). Block-hash-based input is ungrindable and
unpredictable; including roll-distinguishing bits spreads within-block load across quorums.
Caller and verifier compute the same ordering from the same on-chain inputs.

**Active set and N.** The active-quorum set — and thus N — is evaluated as of the roll's
anchor block height from on-chain quorum state: a quorum is in the set if ACTIVE at that
height (formation height ≤ anchor height, not DISBANDED at anchor height). Caller (at roll
time) and verifier (later) read the same set at the same fixed height, so N and membership
are identical for both. The absolute count of quorums is an emergent operational property
(scales with GM population at n=11 per quorum, KDD-048), not a set parameter.

**Failover.** The governing rule: failure-triggering must never enable outcome selection.

1. **Verifiable re-route (same seed, deterministic fallback):** fires if and only if the
   primary's unavailability is chain-evident — DISBANDED or inquorate in on-chain state at
   the anchor height. The roll falls to the next quorum in `ordering`. A verifier confirms
   both the primary's dead state and the fallback's correctness from chain alone. The caller
   cannot fake the trigger (can't mark a quorum disbanded — that's KDD-047's 30-block
   consensus-observed inquorum) nor choose the destination (next-in-ordering is
   predetermined).

2. **Re-roll (new seed, fresh selection):** the recovery for all caller-observed,
   non-chain-evident failures (timeout, partial non-response, mid-roll threshold miss). A new
   seed re-runs selection over the current active set — a fresh draw, not a re-route of the
   fixed seed, conferring no outcome selection. Caller-side timeouts never trigger a
   same-seed re-route.

**Why the asymmetry closes steering:** the verifiable path (re-route) is the one the caller
can't abuse (can't fake trigger, can't pick destination); the abusable path
(caller-triggered) is forced onto re-roll, which is a fresh random draw, not selection
between existing outcomes. Malicious-member-induced failure (a caller who is also a
primary-quorum member withholding to force re-roll) is priced by existing PoSe/ejection
(KDD-046): withholding costs a missed signing and yields only an unaimed fresh draw.

**Disband-window coverage.** A quorum can be unresponsive up to 30 blocks before formal
disband (KDD-047). During that window the fallback chain (chain-evident cases) and re-roll
(caller-observed cases) keep rolls flowing without waiting for formal disband; once
disbanded, the quorum leaves the active set and subsequent selection routes around it
automatically.

**Implementation note (not over-specified).** `deterministic_shuffle` may be a seeded
Fisher-Yates over the active set or an ordering by `H(input ‖ quorum_id)` — both satisfy
determinism with identical security properties; the choice is deferred to implementation.
New pure function `PTX_SelectQuorum(anchor, game_id, roll_index, active_set)` → `ordering`,
testable in isolation. Rides on the W2.1 per-quorum registry (which multi-quorum requires
regardless).

**Status:** Decided (rules). Partially resolves ODC-024 — selection and failover settled;
multi-quorum membership and scaling (quorum-count targets) remain open under ODC-024. Code
is ODC-024-era; not W1.2 scope.

**KDD:** KDD-053

---

## §14 Decided: DKG Ceremony Crypto-Stack Boundary — Arithmetic vs. Transport

**KDD-054 (2026-06-04). Narrows and formalises IMP-D1.**

**Boundary rule.** IMP-D1 mandates blst as the DKG ceremony BLS substrate. That constraint
applies to **ceremony arithmetic** only. Transport and authentication may use the inherited
`src/bls/` (chiabls/RELIC) layer where the blst scalar never enters chiabls as a field element.

**Ceremony ARITHMETIC (blst-only):** polynomial evaluation, verification vector computation
(`vvec[k] = g^{coeffs[k]}`), share encryption input (`f_i(j)`), Feldman VSS check
(`g^{share} == Π vvec[k]^{j^k}`), share aggregation, group key computation, threshold
recovery. Every scalar in this path is a `blst_scalar` or `blst_fr`; no chiabls type touches it.

**Ceremony MESSAGE AUTH and SHARE TRANSPORT (chiabls permitted):** operator-key signing of
P2P messages (all phases, P0 through P5) uses `CBLSSecretKey::Sign` / `CBLSPublicKey::VerifyInsecure`
— chiabls, as established in P0 and P1. Share transport uses `CBLSIESMultiRecipientBlobs`
(`bls_ies.h`, chiabls/RELIC stack). The IMP-D1 scalar-representation seam is not crossed
because the share crosses the boundary as 32 opaque bytes only:
`blst_bendian_from_scalar` (out of blst) → IES encrypt → IES decrypt → `blst_scalar_from_bendian`
(back into blst). The chiabls/RELIC layer operates on opaque bytes; it never treats the value
as a BLS12-381 field element.

This narrows the standup's prior "no `src/bls/` in PTX code" rule to its actual intent:
**no chiabls in the arithmetic path**. The message-authentication and transport paths are
explicitly permitted.

**Load-bearing invariant — IES authentication gap.**
`CBLSIESMultiRecipientBlobs` is unauthenticated AES-256-CBC. The KDF is non-standard: the
first 32 bytes of the 48-byte compressed G1 ECDH output are used directly as the AES-256 key
(no HKDF, no MAC). Therefore:

> **Any PTX use of `CBLSIESMultiRecipientBlobs` MUST bind the ciphertext bytes in an outer
> signature.** P1 satisfies this: `PTXDKGPhase1Msg::GetSignHash` (`ptx_dkg.cpp:358–363`)
> covers the length-prefixed blob bytes, and the operator-key sig over that hash is verified
> before any blob is accepted. The IES provides confidentiality only; integrity is the outer
> sig's responsibility. A future reuse of this transport without an outer sig silently inherits
> a malleability gap.

**Audit note (W3.2 scope).** The inherited chiabls/RELIC stack carries two disclaimers:
(1) `src/chiabls/README.md:13` — "NOTE: THIS LIBRARY IS NOT YET FORMALLY REVIEWED FOR
SECURITY"; (2) `relic.h` — "RELIC is at most alpha-quality software. Implementations may not
be correct or secure... There are many configuration options which make the library horribly
insecure." Both apply equally to all chiabls consumers (LLMQ, ProTx, deterministicGMManager,
V6_0 pipeline, `quorums_dkgsession.h` which already includes `bls_ies.h`). No incremental
exposure from P1's reuse. The build configuration (`relic_conf.h`: BLS12-381, `ARITH GMP`,
`SEED UDEV`, `RAND HASHD`) is the standard production subset, not a "horribly insecure"
configuration. One flag for auditors: `ARITH GMP` not `ARITH GMP_SEC` — chiabls scalar
arithmetic is not constant-time (applies to the whole inherited stack, not P1 specifically).
The disclaimers are the kind a Trail-of-Bits/NCC engagement is designed to formally close.

**Status:** Decided. Formalises what P0 and P1 already implement. Governs P2–P5 and any
future PTX transport reuse.

**KDD:** KDD-054

---

## §15 Decided: DKG P2/P3 Complaint–Justify Resolution — Bad-Member Marking Rules

**KDD-055 (2026-06-04). Decides the three P2/P3 resolution branches and the bad-member
marking rule for each.**

**Background.** Phase 2 (Complaint) is where the Feldman per-share check first runs.
Each QUAL member (complainant C) verifies that dealer D's decrypted share satisfies
`g^{share} == Π vvec_D[k]^{j^k}`, where `j` = C's `share_index` (1-indexed per KDD-052)
and `vvec_D` is the already-commitment-checked P1 vvec in `phase1_vvecs[D]`. A failure
triggers a complaint; Phase 3 (Justify) is D's opportunity to resolve it. The three
branches below, together with the effective-QUAL constraint, constitute the complete
resolution rule.

**Branch 1 — Complaint (P2): signed assertion, not verified evidence.**

A QUAL member C whose Feldman check on dealer D's share fails broadcasts a signed complaint
identifying D and the specific share index `j` (C's own `share_index` — the evaluation
point under dispute).

A complaint is a SIGNED ASSERTION, not independently verifiable evidence. The share was
encrypted to C's key; no third party holds C's decrypted value and no third party can
confirm C's check failed. The protocol therefore trusts the signed complaint to trigger a
justify round, and relies on Branch 2's false-accuser penalty as the economic deterrent
against frivolous accusations. This is deliberate design, not a gap. An auditor reading a
complaint-without-proof should find this rationale here, not treat it as a hole.

**Branch 2 — Justify received, check passes (P3): complainant C → bad_members.**

D reveals the disputed plaintext share `s = f_D(j)` as a raw `blst_scalar` (big-endian
wire encoding, 32 bytes — the same encoding as P1 shares; no encryption, revealed in
clear). The network recomputes the Feldman check against `phase1_vvecs[D]` (the vvec D
revealed in P1, already commitment-checked against D's Phase 0 hash — D cannot present a
new vvec):

```
g^s == Π_{k=0}^{t-1} vvec_D[k]^{j^k}
```

If the check passes: a passing justify proves D's share at `j` is valid. C's complaint was
therefore unfounded, and C is marked bad regardless of whether C was malicious or merely
wrong (e.g. C's own decryption was corrupted). The false-accuser penalty applies to
unfounded complaints; the protocol does not distinguish malice from error because it cannot.

```
C → bad_members.  D stays in effective-QUAL.
```

A dealer cannot escape a valid complaint by revealing a different on-curve scalar. The
vvec IS D's polynomial commitment (revealed in P1, commitment-checked against D's Phase 0
hash). Any scalar `s` satisfying `g^s == Π vvec_D[k]^{j^k}` is BY DEFINITION the correct
share at `j` — there is exactly one such value. If D's P1-transmitted blob decrypted to
something other than `f_D(j)`, that is a transport/encryption fault, not a polynomial
fault, and is unprovable (only C could decrypt the blob — the same unverifiability as the
complaint). The protocol's position: the vvec is ground truth, justify resolves against
the vvec, and a P1-blob-vs-justify-value mismatch is not adjudicable. This is consistent
with treating the vvec as the binding commitment throughout.

**Branch 3a — Justify received, check fails (P3): D → bad_members.**

If the Feldman check on D's revealed share fails: D's polynomial at index `j` does not
match D's committed vvec. D's share was invalid.

```
D → bad_members.  C stays in effective-QUAL.
```

**Branch 3b — No valid justify by ClosePhase3: D → bad_members.**

If D fails to provide a valid justification by `ClosePhase3` — no message, a malformed
message, or a message rejected for any of the following reasons — the complaint is
unresolved:

- bad signature;
- wrong `quorum_hash`;
- `j` does not match any outstanding complaint against D from a QUAL member (`j`
  unparseable/out-of-range, or `j` in range but no complaint on record for that `j`);
- a justify naming a `(D, C, j)` triple for which no complaint against D is outstanding
  (D justifying something nobody complained about);
- revealed scalar is not a valid `blst_scalar`.

At `ClosePhase3`, any QUAL member with at least one unresolved complaint outstanding is
swept into `bad_members`:

```
D → bad_members.  C stays in effective-QUAL.
```

(LLMQ analog: `VerifyAndCommit`'s final sweep marks any member with
`complaintsFromOthers` non-empty after the justify round closes.)

**Effective-QUAL invariant.**

All P2/P3 logic operates over effective-QUAL = `qual − bad_members` at the time of each
operation. A member already in `bad_members` (from Phase 1 commitment-mismatch, a
non-reveal, or an earlier P2/P3 resolution) is:

- not Feldman-checked by any peer (no complaint will be filed against it);
- not able to file a complaint (its complaint message is rejected);
- not able to justify (its justify message is rejected).

This builds directly on the Phase 1 `bad_members` monotonicity invariant (FIX-1, P1):
once bad, always bad for the remainder of the ceremony.

**Per-`j` disambiguation (multiple complaints against the same dealer).**

A dealer D facing multiple complaints (from different QUAL members C1, C2, … each with a
distinct `share_index` j1, j2, …) must justify each disputed share separately. Each
complaint names a specific `j`; each justification answers a specific `j`. The justify
message identifies the disputed share by dealer, complainant, and `share_index j`, plus
the revealed scalar; the exact message structure is specified in the P2/P3 implementation
report.

**Non-scope — PoSe bridge (deferred).**

This KDD decides ceremony-local `bad_members` only. Whether a P2/P3 complaint resolution
ALSO emits a PoSe penalty event (via `ptx_pose.h`, per the KDD-046 recoverable-penalty
regime) is a SEPARATE, DEFERRED decision. A DKG complaint is the first
consensus-derivable misbehaviour evidence with a named accused — the natural future bridge
to the PoSe tracker that KDD-038 identified as missing. That bridge is explicitly NOT
decided or implemented here. Park for post-W1.2 design; do not assume it in P2/P3 code.

**Non-scope — Complaint/justify timing window (ODC-026).**

The block-height or wall-clock duration of the P2 and P3 windows is NOT decided here. See
ODC-026 (§9.5). The P2/P3 implementation uses explicit `ClosePhase2()`/`ClosePhase3()`
calls; no internal timing gate is compiled in. The duration question is a deployment
parameter resolved downstream of the wall-clock/block-time timing audit.

**Status:** Decided. Governs P2/P3 implementation and all future ceremony code that touches
complaint or justification processing.

**KDD:** KDD-055

---

## §16 Decided: PTXDKG Transaction nType Assignment

**KDD-056 (2026-06-12). Assigns nType=11 to the PTXDKG special transaction; nTypes 7 and
8 are deliberately left as gaps — PTXSETTLE/PTXCONSOLIDATE semantics must not be
resurrected.**

PTXDKG is an entirely net-new nType. The current `transaction.h` `TxType` enum ends at
`PTXPAYOUT=10`; nTypes 7 and 8 are **absent** from the active codebase. They were
`PTXSETTLE=7` and `PTXCONSOLIDATE=8` on the abandoned `feature/ptx-phase2-bls` branch
(commits f45bbcf, 737214d), dropped after the Phase 0 incident on 2026-05-26 (see
standup §history lines 5, 367, 689; project_ptxsettle.md). No dispatch code for 7 or 8
exists in `specialtx_validation.cpp` — the `default: DoS(10, "bad-tx-type")` path
rejects them cleanly.

nType=11 is chosen to deliberately leave 7 and 8 as gaps. They are not reused so that the
abandoned PTXSETTLE/PTXCONSOLIDATE semantics are never resurrected into the active enum.
The name "PTXSETTLE" is a particular hazard: it overlaps conceptually with the live
codebase's "settlement window" and "PTXPAYOUT settlement" (entirely different concept,
different code path). Gaps are harmless; semantic resurrection is not.

The enum comment in `transaction.h` records this explicitly:

```cpp
PTXDKG = 11,       // DKG ceremony result: group_pk + vvec_hash + signed premature commitments
                   // nTypes 7 (PTXSETTLE) and 8 (PTXCONSOLIDATE) deliberately left as gaps —
                   // do not reuse; see KDD-056.
```

`IsPTXDKGTx()` is defined as `IsSpecialTx() && nType == TxType::PTXDKG` (the
`IsSpecialTx()` form because PTXDKG carries a populated `extraPayload`). Next free nType
from 12 onward.

**Status:** Decided. Implemented in commit bae1dcf (W1.2 Phase 5). No rollback path —
re-numbering nTypes is a consensus-breaking hard fork.

**KDD:** KDD-056

---

## §17 Decided: P5 sk_share_i Write Path — Option A (Shared Store)

**KDD-057 (2026-06-12). PTX_DKG_StoreSkShare writes the DKG-produced share to
g_ptx_my_bls_sk_bytes — the same global as the gm_bls_keyset RPC — so the existing
PTX_BLS_PartialSign path works unchanged.**

Two candidate paths existed:

- **Option A (chosen):** DKG share → `g_ptx_my_bls_sk_bytes` (same store as
  `gm_bls_keyset`). No new signing path needed; `PTX_BLS_PartialSign` uses the global
  as-is; end-to-end test `P5_EndToEnd_SigningPathWorks` validates the round-trip.
- **Option B (rejected):** DKG share → a dedicated `g_ptx_my_dkg_sk_bytes` global;
  signing path would need a selector to choose between RPC-provisioned and DKG-provisioned
  key. Additional complexity with no benefit at W1.2 scope.

Option A is simpler and avoids a signing-path fork. The critical consequence is that
`g_ptx_my_bls_sk_bytes` now has **two write sites**: (1) the `gm_bls_keyset` RPC handler
(original) and (2) `PTX_DKG_StoreSkShare` (added in P5). The W1.3 replay-protection guard
(standup §C1) MUST cover **both** write sites — not only the RPC path. The required
commit-time scope note (C5 deliverable) is present verbatim in the `StoreSkShare`
implementation comment in `ptx_dkg.cpp`.

The globals were relocated from `static` in `rpc/ptx.cpp` to defined in `ptx_bls.cpp` /
extern-declared in `ptx_bls.h` to make the second write site accessible without a circular
include. This relocation is a deliberate refactor called out explicitly in commit bae1dcf.

**Status:** Decided. Implemented in commit bae1dcf (W1.2 Phase 5).

**KDD:** KDD-057

---

## §18 Decided: PTXDKG Submission Model (resolves ODC-029)

**KDD-058 (2026-06-12). PTXDKG is submitted by DIRECT BLOCK INJECTION,
following the LLMQCOMM precedent. NOT mempool-relayed.**

LLMQCOMM is the exact structural analogue — ceremony-result tx, populated
extraPayload, no vin, no vout — and is block-injected via GetMinableCommitmentTx
in CreateNewBlock(). PTXDKG fits the same pattern.

Mechanism (W1.3 implementation, pending):
(a) Exempt IsPTXDKGTx() from the empty-vin/vout checks in consensus/tx_verify.cpp
    (parallel to the existing IsQuorumCommitmentTx() exemption at tx_verify.cpp:59,63).
    Without this, PTXDKG fails bad-txns-vin-empty BEFORE CheckSpecialTx runs.
(b) Reject PTXDKG from the mempool (parallel to the llmqcomm/ptxcoalesce/ptxpayout
    rejections at validation.cpp:386-395).
(c) Inject via a GetMinablePTXDKGTx-style hook in CreateNewBlock() (blockassembler.cpp),
    in the existing post-mempool injection block alongside LLMQCOMM/PTXCOALESCE/PTXPAYOUT
    (blockassembler.cpp:200-273).

Rejected alternative — mempool-relay: would require either changing the tx schema
to add real vin/vout (alters the payload hash) or a mempool-acceptance exemption
with no precedent for any PTX special tx. Direct-inject is the path of least
structural change and the only model with a working in-codebase template.

I1/I2 compliance — coupling to KDD-059: block-producer injection is MECHANISM,
not privilege — because the PTXDKG result is deterministic and CheckPTXDKGTx
rejects any incorrect injection, any honest producer injects identical bytes and
a producer cannot get a wrong result accepted. This DEPENDS ON CheckPTXDKGTx
verifying ceremony correctness (the threshold-sig validation, KDD-059): the
structural-only check shipped in bae1dcf is necessary but not sufficient for the
determinism guarantee. ODC-029's I2-compliance and the threshold validation are
coupled — the submission mechanism is the reason the validation is
security-critical rather than completeness-only.

**Status:** Decided. Implementation pending (W1.3). Construct-only path
(PTX_DKG_BuildPTXDKGTx) shipped bae1dcf.

**KDD:** KDD-058

---

## §19 Decided: PTXDKG On-Chain Validation Semantics — Attestation-Counting

**KDD-059 (2026-06-12). W1.3 CheckPTXDKGTx contextual validation verifies
ATTESTATION, not ceremony-correctness.**

On-chain guarantee: "≥ t registered quorum members each signed, with their
DGM-registered operator key, agreement that this group_pk is the ceremony result."

Verified (contextual path, pindexPrev != nullptr):
- Each premit_commitments entry's sig verifies against the signer's pubKeyOperator,
  resolved from the DGM registry at formation_height by the entry's proTxHash key.
- Each committer was a registered GM at formation_height.
- ≥ t distinct valid attestations from distinct registered members.
- All premit group_pk_bytes agree with each other and with payload.group_pk_bytes.
- group_pk_bytes decompresses to a valid G1 point (existing structural check).

NOT verified (stated security boundary):
- Share-correctness / honest Feldman VSS — NOT on-chain verifiable: the per-member
  vvecs are not in the payload (only vvec_hash, an irreversible digest; see ODC-027).
- Threshold-recoverability — the payload carries no recovered threshold signature
  (unlike LLMQ's quorumSig). The premit commitments ARE the threshold evidence.

Security boundary: the on-chain proof is ACCOUNTABILITY-grade (named, non-repudiable,
authorized attestations from ≥ t members), not CRYPTOGRAPHIC-CORRECTNESS-grade. This
matches the system's existing t-of-n trust assumption — a threshold scheme already
trusts that fewer than t members collude; attestation-counting does not weaken it,
but the chain record does not independently catch ≥ t-member collusion. Stronger
on-chain proofs are deferred: share-correctness → ODC-027 (full vvecs on-chain);
threshold-recoverability → ODC-028 (recovered-threshold artifact). Both W3.2 audit scope.

Member identity is keyed off proTxHash (premit map key) + DGM lookup. member_node_ids
is informational/cosmetic for consensus — NOT validated (ordering and string content
ignored; the W1.2 count check is the only consensus use).

Structural consequence: CheckPTXDKGTx bifurcates on pindexPrev (null → structural-only,
unchanged; non-null → structural + contextual sig/registry checks), gains a cs_main
requirement, and performs a DGM-registry lookup — the pattern used in CheckProRegTx /
CheckProUpServTx / VerifyLLMQCommitment. The "artifact-only" characterization holds for
the W1.2 structural checks but NOT for the W1.3 sig verification.

**Status:** Decided. Implementation pending (W1.3 validation design spec).

**KDD:** KDD-059

> **Addendum (2026-06-13):** Membership predicate amended by KDD-060 (§20): committer must be ∈ CalculateQuorum(11, quorum_hash) at the formation block, not merely DGM-registered.

---

## §20 Decided: Canonical Quorum Selection and Membership Predicate

**KDD-060 (2026-06-13). The PTXDKG quorum at a formation is exactly
`GetListForBlock(formation block).CalculateQuorum(11, quorum_hash)` — one
function, raw modifier, for both formation and validation. CheckPTXDKGTx
verifies committer membership in THIS set, not mere DGM registration.
Amends KDD-059.**

### 20.1 The gap being closed

KDD-059's predicate "each committer was a registered GM at formation_height" is strictly weaker than quorum membership. The DGM registry at a height is a superset of the score-selected top-11 (`CalculateQuorum`, deterministicgms.cpp:224–243, scores ALL valid confirmed GMs via `ForEachGM(true, …)` then truncates to `maxSize`). Under the bare-registration predicate, any ≥ t registered-but-unselected GMs (rank 12+) could co-sign mutually-agreeing premit attestations and inject a PTXDKG that passes every check — enshrining an attacker-chosen group_pk for a formation they were never part of. This breaks the KDD-058 determinism coupling directly: block-inject is I2-safe only because `CheckPTXDKGTx` rejects wrong results.

### 20.2 The canonical-selection contract

For formation anchor block B (identified by `quorum_hash = B.GetBlockHash()`):

```
quorum(B) := deterministicGMManager->GetListForBlock(pindex_B)
                 .CalculateQuorum(11, B.GetBlockHash())
```

- **One implementation.** Both ceremony formation (W2) and consensus validation (W1.3) call this function. No reimplementation, no "equivalent" scorer, ever.
- **Raw modifier.** `quorum_hash` is consumed as the raw 32-byte uint256 modifier. PTX MUST NOT use `GetAllQuorumMembers` (deterministicgms.cpp:991–997), which wraps the modifier as `SerializeHash(pair(llmqType, blockHash))` — a different value producing a different quorum.
- **Ordering and share_index.** `CalculateQuorum` output is descending by score with a deterministic `collateralOutpoint <` tie-break under `std::sort` (deterministicgms.cpp:224–244; collaterals are unique, so total order). This descending order IS the KDD-052 score order. `share_index := position + 1` in this output (position 0 → share_index 1).
- **Score formula (inherited, now sole):** per-GM score = `SHA256( confirmedHashWithProRegTxHash[32] || modifier[32] )` where `confirmedHashWithProRegTxHash = SHA256(proTxHash[32] || confirmedHash[32])` (deterministicgms.cpp:260–266; deterministicgms.h:113–120). Null-confirmedHash GMs are excluded by `CalculateScores` itself (deterministicgms.cpp:250).

### 20.3 Scorer retirement (consequence, not optional cleanup)

`PTX_DKG_ComputeMemberScore` (ptx_dkg.cpp:107–118) and `PTX_DKG_SortMembers` (ptx_dkg.cpp:121–145) are RETIRED — removed, not quarantined. Recon proof this is required for correctness, not just hygiene:

- `SortMembers` uses `std::stable_sort` with **no tie-break**; `CalculateQuorum` uses `std::sort` with the `collateralOutpoint` tie-break. The two are byte-equivalent **only on tie-free inputs** — under a score tie at the 11/12 boundary they select different quorums and assign different share_index values.
- Score-formula parity (`ComputeMemberScore` ≡ `CalculateScores` inner loop) holds iff fed the cached `confirmedHashWithProRegTxHash` — so existing phase0 determinism/sensitivity tests can migrate to `CalculateQuorum`/`CalculateScores`-derived expectations rather than being deleted blind.
- Retirement inventory: production callers = exactly one (`InitSession` ptx_dkg.cpp:156), which is itself production-uncalled. Test callers: ptx_dkg_phase0_tests.cpp T0-1/2/4/5/6 (lines 163–164, 183–184, 217–245, 262). Comments naming SortMembers: ptx_dkg.h:75, ptx_dkg.h:310 — updated with the rebase.

### 20.4 Distinctness and the key↔inner binding (new finding)

The serializer's `std::map` deserialization hint-inserts and silently drops duplicate keys (serialize.h:1222–1240); the post-insert map size is what the ≥ t check counts — so duplicate *keys* cannot inflate the count. **But map-key distinctness alone is insufficient**: `PTXDKGPhase4Msg` carries its own `proTxHash` field (it is in the sign-hash preimage, ptx_dkg.cpp:972–986). Without V7a (check sequence: doc/ptx/W1.3_VALIDATION_SPEC_v1.md §3.2), one member's valid premit could be inserted under several different map keys — distinct keys, identical inner attestation — and each copy's signature verifies (the sig covers the *inner* proTxHash, not the key). V7a (`key == p4.proTxHash`) pins key == attested identity; with it, ≥ t distinct keys ⇒ ≥ t distinct attesting members. This sharpens KDD-059's "distinct" clause: distinctness is key-enforced **conditional on V7a**, which the W1.2 structural path does not perform.

> **Correction (2026-07-01, C-4 — reconciles to the shipped V7f design).** The paragraph above assumed V7f resolves the operator key from the *inner* proTxHash. The shipped validator (decision-4) reads the operator key off the **map key** — the V6 quorum map, `ptx_dkg.cpp` `PTX_DKG_VerifyPremits` — and the C-2 falsification cycle (Stub-2) proved the consequence: the "one premit duplicated under N distinct keys" inflation is **NOT caught by V7a** under this design. **V7g backstops it** — the duplicated signature does not verify under another member's operator key (reject `ptxdkg-bad-premit-sig`). So ≥ t-distinct-attestation soundness rests on **map-key dedup + V7b (quorum membership) + V7g (sig verifies under the map-key's operator key) + V7d/e (field agreement)**. V7a's isolating role is the **inner-mislabel case** ("member K signs with inner P ≠ K"): V7a is the *sole* catcher of that (Stub-2: that test accepts with V7a removed). V7a is **kept** — cheap, spec-mandated, binds the signed inner identity to the map key (defense-in-depth). No implemented behaviour changes; this corrects the "load-bearing for the inflation attack" framing only. The earlier framing held only if V7f keyed on the inner proTxHash.

### 20.5 KDD-059 relationship

KDD-059's verified-list clause "Each committer was a registered GM at formation_height" is **superseded** by "Each committer ∈ quorum(B)". All other KDD-059 content stands (attestation-counting semantics, accountability boundary, ODC-027/028 deferrals, member_node_ids cosmetic, bifurcation/cs_main consequence). §19 addendum points here; no in-place §19 edit.

**Status:** Decided 2026-06-13 (implementation pending W1.3). Amends KDD-059 (§19).

**KDD:** KDD-060

### 20.6 W1.3 Package 1 addendum — eligibility predicate, node_id boundary, snapshot-anchoring invariant (2026-06-15)

**Implemented at:** `a9191d0` (`feature/ptx-dkg`). Governs
`PTX_DKG_IsGMPTXEligible`, `PTX_DKG_BuildMemberVectorFromList`, and the
`PTX_DKG_InitSession` new contract.

#### 20.6.1 PTX eligibility predicate

`PTX_DKG_IsGMPTXEligible(dgm)` ≡ `!dgm->pdgmState->node_id.empty()`
(`ptx_dkg.cpp:109`). Exported (not static, declared in `ptx_dkg.h`) so the
Package 2 validator calls the same function; never re-inlined.

**node_id-only by design.** The predicate answers "can this GM run the ceremony"
(sign P0–P4 messages, contribute BLS shares). It is deliberately NOT the
winner-selection predicate, which additionally requires non-empty `scriptPTXPayment`
(`ptx_winner_selection.cpp` Amendment 2). A GM without a payout script is a valid
ceremony participant.

**Filter placement.** `PTX_DKG_BuildMemberVectorFromList` applies the filter
**before** `CalculateQuorum` (`ptx_dkg.cpp:120–128`): `ForEachGM(eligible)` →
`AddGM` into a fresh `CDeterministicGMList` → `CalculateQuorum(11, quorum_hash)`.
Post-filter-drop (score all, then remove ineligible entries) is rejected: it alters
the candidate set that scores compete within, changing which GMs occupy quorum
positions and breaking agreement between formation and validation.

#### 20.6.2 node_id containment proof — cosmetic for consensus

`node_id` is cosmetic for quorum selection and ordering. Three properties establish
the boundary:

**(a) Score isolation.** `CalculateScores` (`deterministicgms.cpp:246–270`) reads
only `confirmedHashWithProRegTxHash` and the `quorum_hash` modifier. `node_id` is
absent from the score preimage. Format variation, collision, and post-registration
mutation cannot change quorum selection or share_index ordering.

**(b) Presence-only gate.** `PTX_DKG_IsGMPTXEligible` is a boolean gate on
emptiness; the value beyond empty is irrelevant to membership. Selection, ordering,
and validation key off `proTxHash` (globally unique by collateral construction),
never off node_id value.

**(c) node_id boundary established at registration.** Format, uniqueness, and
mutability are all enforced by the registration layer; PTX presence-only is the
correct consumer boundary.

- **Format:** `ValidateProRegNodeId` (`specialtx_validation.cpp:118–172`) runs when
  `nVersion >= 3 && !node_id.empty()` (line 243). Enforces: exactly-one-colon
  `label:suffix`; label 3–24 bytes, charset `[a-zA-Z0-9_-]`, no leading/trailing
  edge chars, no all-numeric label, reserved-word blocklist; suffix =
  `hex_lower(SHA256(serialize(collateral))[0:4])` — chain-derived, operator cannot
  forge. PTX does **not** re-validate format — re-asserting it in the validator is
  consensus-critical split risk with no security benefit.

- **Uniqueness:** enforced at ProRegTx time by case-insensitive linear scan
  (`specialtx_validation.cpp:310–322`): `ToLower(node_id)` compared across all live
  GMs → `REJECT_DUPLICATE "bad-protx-dup-node-id"`. NOT in the `AddUniqueProperty`
  set (`deterministicgms.cpp:403–408`); the linear scan is O(N_GMs) and sufficient
  because node_id is write-once. Two live GMs with the same node_id cannot coexist on
  a valid chain.

- **Mutability:** node_id is **write-once**. Set in `CDeterministicGMState(const
  ProRegPL&)` (`deterministicgms.h:66`). `ProUpServTx` writes only `addr` and
  `scriptOperatorPayout` (`deterministicgms.cpp:690–691`); `ProUpRegTx` writes only
  `pubKeyOperator`, `keyIDVoting`, `scriptPayout` (`deterministicgms.cpp:732–734`).
  No rotation path exists. The pose-layer `records_` keying on `node_id` is
  therefore stable: tickets bound at registration cannot be orphaned.

#### 20.6.3 member_node_ids empty-entry concern — CLOSED

**Concern:** could `payload.member_node_ids` contain an empty string, passing the
W1.2 structural count check (`specialtx_validation.cpp:631–634`) while encoding an
invalid member identity?

**Closed by the eligibility filter.** `PTX_DKG_IsGMPTXEligible` gates
`BuildMemberVectorFromList`; no GM with an empty node_id can enter the quorum
(`ptx_dkg.cpp:120–128`). `session.members[]` is populated from the filtered vector
via `InitSession`; all members carry a non-empty node_id. `BuildPTXDKGTx` copies
`m.node_id` from `session.members` for QUAL members only (`ptx_dkg.cpp:1305, 1315`).
No path from formation to payload produces an empty entry. **No ODC raised** — the
concern cannot arise with the filter in place; the structural count check is
sufficient.

#### 20.6.4 Snapshot-anchoring invariant — binds both validator and W2 formation

**Invariant:** both the W1.3 consensus validator and the W2 ceremony formation MUST
source the GM list from `GetListForBlock(pindexQuorum)`, where `pindexQuorum` is the
block identified by `payload.quorum_hash`. Neither may read from live DGM state, the
tip, or any block other than the formation anchor.

**Validator half (W1.3 Package 2, in scope).** Encoded in the V1–V5 sequence
(`W1.3_VALIDATION_SPEC_v1.md §3.2`):
- V1: `pindexQuorum = LookupBlockIndex(payload.quorum_hash)` — anchor block resolved
  from the payload, not from pindexPrev.
- V2: `pindexQuorum->nHeight == payload.formation_height` — redundancy/consistency
  check (hash is the real anchor; height is a sanity cross-check).
- V3: `pindexPrev->GetAncestor(pindexQuorum->nHeight) == pindexQuorum` — reorg
  safety; only blocks on the chain being validated are accepted.
- V4: `dgmList = deterministicGMManager->GetListForBlock(pindexQuorum)` — reads the
  formation-block snapshot. **Not from pindexPrev.**
- V5: `quorum11 = PTX_DKG_SelectQuorumFromList(dgmList, payload.quorum_hash)` —
  reconstructs the canonical quorum via the **shared selection core** (filter the
  snapshot by `PTX_DKG_IsGMPTXEligible`, then `CalculateQuorum(11, quorum_hash)`), the
  SAME core formation uses through `PTX_DKG_BuildMemberVectorFromList` (which wraps it),
  so formation and validation reconstruct **byte-identical membership**. The validator
  calls the core directly (not the `PTXDKGMember`-mapping wrapper) and checks proTxHash
  membership against this set via V6/V7b. **Never bare `CalculateQuorum` on the
  unfiltered `dgmList`** — that would score empty-node_id GMs formation excludes and
  split the chain. The candidate-set predicate is **non-PoSe-banned ∧
  non-null-confirmedHash ∧ node_id-non-empty**; only node_id-non-empty is contributed by
  the PTX filter (PoSe and confirmedHash are inherited from `CalculateScores`).

  > **Corrected 2026-07-01 (C-4).** The prior V5 wording — bare
  > `dgmList.CalculateQuorum(11, …)` and "the validator does NOT call
  > BuildMemberVectorFromList" — was the pre-Package-1 encoding and a chain-split bug;
  > **retracted**. Shipped at C-1 (`d52106b`, `PTX_DKG_SelectQuorumFromList` extracted)
  > and C-2 (`5134955`, the validator calls it). The validator still does not call the
  > `PTXDKGMember`-mapping wrapper, but it shares the same selection core.

Reading from pindexPrev or live state instead of pindexQuorum is incorrect: it
evaluates a potentially different GM list and would reject a valid ceremony or accept
a post-formation-changed one.

**Formation half (W2, out of W1.3 scope).** `PTX_DKG_BuildMemberVectorFromList` takes
a caller-supplied `CDeterministicGMList` (`ptx_dkg.cpp:118`). The W2 ceremony
formation entry point MUST supply the result of `GetListForBlock(pindexQuorum)` — the
snapshot at the formation anchor block — not a live list at ceremony-run time. If the
ceremony reads live state and any mutable DGM field (addr via ProUpServ, pubKeyOperator
via ProUpReg) changed between formation and confirmation, formation and validation see
different candidate sets. node_id is write-once and cannot cause this drift
specifically; the general anchoring constraint binds the entire formation path
regardless.

`PTX_DKG_BuildMemberVectorFromList` has no production caller as of `a9191d0`. This
constraint is registered here so W2 formation (W2.2, `DKG_IMPLEMENTATION_PLAN_v1.md
§3`) anchors to `pindexQuorum`, not tip state.

---

## Appendix: Register cross-reference

| KDD | Title | §| Status |
|---|---|---|---|
| KDD-039 | DKG before public testnet | §2 | Decided |
| KDD-040 | Single-quorum-per-GM; multi-membership rejected | §4 | Decided |
| KDD-041 | Ticket-based lottery, 1:5 ratio | §6 | Decided |
| KDD-042 | GM reward model — masternode backbone + PTX bonus | §5.1 | Decided |
| KDD-043 | Roll fee — 1 HMS, spork-adjustable, atomic-with-result | §5.2 | Decided |
| KDD-044 | Quorum formation — batch-of-11 from pool | §7.1 | Decided |
| KDD-045 | Quorum rotation — same-set re-DKG, staggered | §7.2 | Decided |
| KDD-046 | Ejection/PoSe discipline — 15-of-60, 120-ban | §8 | Decided |
| KDD-047 | Disband — inquorate → pool, tickets carry; n_disband=30; dissolve-to-pool | §7.3 | Decided |
| KDD-048 | Quorum params: t=6 decided; upgrade-gated consensus constant, not spork | §3, §9.1 | Decided |
| KDD-049 | PTX_BLS_Verify explicit group_pk — pure function, no global state read | — | Decided — impl plan 2026-06-03; commit 66251c8 |
| KDD-050 | Test extraction interface — in-daemon subset check; ENABLE_PTX_TEST_ACCESSORS compile gate, default off | — | Decided — impl plan 2026-06-03 |
| KDD-051 | DKG construction — Feldman VSS + GJKR commit-then-reveal hardening; QUAL-locks-before-reveal | §11 | Decided — measured 2026-06-03 |
| KDD-052 | PTXDKG member set — committed node_id list, chain-determined (score) order; resolves OPEN-2. Selection/ordering contract concretized by KDD-060 (§20); score-order principle unchanged. | §12 | Decided 2026-06-03 |
| KDD-053 | Multi-quorum roll selection (Option D, ungrindable) + failover (verifiable re-route / re-roll asymmetry); partially resolves ODC-024 | §13 | Decided 2026-06-03 |
| KDD-054 | DKG ceremony crypto-stack boundary: arithmetic vs transport — blst-only arithmetic; chiabls permitted for auth/transport; IES outer-sig invariant | §14 | Decided 2026-06-04 |
| KDD-055 | DKG P2/P3 complaint–justify resolution — bad-member marking rules; false-accuser penalty; vvec-ground-truth invariant; PoSe bridge deferred | §15 | Decided 2026-06-04 |
| KDD-056 | PTXDKG nType=11 assignment; nTypes 7/8 deliberately left as gaps — abandoned PTXSETTLE/PTXCONSOLIDATE semantics must not be resurrected | §16 | Decided 2026-06-12 |
| KDD-057 | P5 sk_share_i write path Option A — shared g_ptx_my_bls_sk_bytes store; W1.3 replay guard must cover both write sites | §17 | Decided 2026-06-12 |
| KDD-058 | PTXDKG submission model — direct block-inject (LLMQCOMM precedent); resolves ODC-029; coupled to KDD-059 | §18 | Decided (impl pending W1.3) |
| KDD-059 | PTXDKG validation semantics — attestation-counting; accountability not correctness; share-correctness/recovery deferred to ODC-027/028 | §19 | Decided (impl pending W1.3) |
| KDD-060 | Canonical quorum selection: `GetListForBlock(B).CalculateQuorum(11, quorum_hash)` — one function, raw modifier, formation + validation; membership predicate fix; scorer retirement; V7a distinctness sharpening. Amends KDD-059. | §20 | Decided 2026-06-13 (impl pending W1.3) |
| ODC-021 | Coordinator SPOF | §2 | Resolved by DKG |
| ODC-024 | Multi-quorum membership — deferred future extension | §9.3 | Open (partially resolved per KDD-053: selection + failover decided) |
| ODC-025 | Rotation-N final value — pending ceremony duration | §9.2 | Open |
| ODC-026 | Complaint/justify timing window — P2/P3 phase duration; downstream of wall-clock/block-time audit | §9.5 | Open |
| ODC-027 | vvec_hash scope in PTXDKGPayload — vvec[0]-only for W1.2 (S4 approved); full-vvec scope deferred to W3.2 audit input | §9.6 | Open |
| ODC-028 | sk_share commitment in PTXDKGPhase4Msg — g^{sk_share_i} G1 proof-of-share; not required for W1.2; deferred to W3.2 audit input | §9.7 | Open |
| ODC-029 | PTXDKG submission model — direct-block-inject decided (KDD-058, 2026-06-12); block-inject wiring pending W1.3 | §9.8, §18 | Closed |
| ODC-030 | PTXDKG acceptance window (no max-age in W1.3) + cross-block per-formation uniqueness — both deferred to W2 lifecycle | §9.9 | Open |
| ODC-031 | V1–V4 anchoring-chain coverage not unit-testable in test_ptx; exercised via Package 3 wiring on a real chain. COMPLETE (C6/C7): V1, V2, V3-PREDICATE, V4, F-5, populate-refusal. BOUND TO W2.2 (datadir-v2): V3-REORG-TRANSITION, C3-INVOCATION, F-6, F-9. C3-INVOCATION not unit-covered (FA-2b) | §9.10 | Partially closed (C6/C7, 2026-07-04) |
| ODC-032 | Can TestChainSetup run in test_ptx (the fixture block-mining hang; runtime face of umbrella rot); if resolved the anchoring checks could move to unit coverage, but the ODC-031 W2.2 remainder binding stands (ODC-031 partially closed) | §9.11 | Open (deferred) |
