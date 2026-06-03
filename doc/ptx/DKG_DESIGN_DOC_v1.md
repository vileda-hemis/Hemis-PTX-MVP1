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

**DKG (distributed key generation) replaces this.** With Pedersen VSS-based DKG, no single party
ever holds `master_sk`. Each quorum of GMs collectively generates its own threshold keypair via
a multi-round ceremony, producing a shared `group_pk` that is verifiably established. The operator
becomes a stateless orchestrator, not a keyholder. The coordinator SPOF (ODC-021) is eliminated.

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
- **Implementation plan** — deferred pending resolution of the reuse question (§9.4). No
  implementation plan will be written until that question is resolved.
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
is implementation-dependent and cannot be confirmed until the reuse question (§9.4) is resolved
and a prototype ceremony is benchmarked.

**Future ODC:** ODC-025 tracks this as the open choice for rotation-N final value.

### §9.3 Multi-quorum membership (§4)

Deferred as a future additive extension. It does not block DKG launch. It extends the DKG
architecture after single-quorum behaviour is proven at scale.

**Future ODC:** ODC-024 tracks this as the open design choice.

### §9.4 The reuse question (gating open question — do not answer here)

**This is the single most important open question for DKG implementation timeline.**

Hemis PTX inherits from PIVX/Dash. Dash LLMQ (Long-Living Masternode Quorums) has a complete
production DKG implementation — VSS commitment broadcast, key share distribution, complaint
handling, justification, and final key publication — built on the same Chia BLS stack that
ptx-bea uses.

**The unresolved question:** is the LLMQ DKG infrastructure reusable for PTX quorums, or does
PTX's architecture (specifically the synchronous `ptx_fanout` bypass, the caller-driven signing
round, and the per-roll BLS ceremony) diverge sufficiently from LLMQ's assumptions that DKG would
need to be built from a lower-level base?

**This question gates the entire implementation timeline.** If LLMQ is reusable: DKG implementation
is primarily integration and adaptation work (weeks to months). If LLMQ is not reusable: DKG
requires building a ceremony from BLS primitives against the existing quorum infrastructure (months
to significant months).

**Resolution:** a separate read-only source investigation of the Dash LLMQ DKG codebase will
answer this before any implementation plan is written. Do not estimate or plan DKG implementation
until this question is answered.

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
| ODC-021 | Coordinator SPOF | §2 | Resolved by DKG |
| ODC-024 | Multi-quorum membership — deferred future extension | §9.3 | Open |
| ODC-025 | Rotation-N final value — pending ceremony duration | §9.2 | Open |
