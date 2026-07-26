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

> **[Reinforced 2026-07-10 — KDD-045 CONFIRMED on a second, independent ground. The
> original fairness rationale above is unchanged.]** The unbounded-quorum-count
> constraint (quorum count is not parameter-bounded; hundreds–thousands of concurrent
> quorums must be viable) supplies a second, independent ground for KDD-045.
> Fresh-draw (dissolve-and-rebatch) is a GLOBAL policy — per-cycle work proportional
> to total quorums: consensus-critical pool-wide draw re-derivation on every
> validating node (O(pool) per cycle), plus a thundering herd of Q simultaneous
> ceremonies at the boundary that would reinstate per-quorum stagger. Static
> membership + periodic same-set re-DKG is PER-QUORUM-LOCAL: validating a rotation
> commitment is O(11) ("same 11 as the existing record"), and each quorum runs one
> ceremony per N on its own drift-staggered cadence regardless of Q — per-node cost
> is constant in Q (each GM is in at most one quorum, KDD-040). The 2026-07-09
> "supersede KDD-045 for the cycle model" flag was a single-scale artifact: it
> evaluated the Dash template at Dash's parameter-BOUNDED quorum count (one formation
> per dkgInterval, active set capped at signingActiveQuorumCount); under unbounded Q
> the inversion is complete — the cycle SCHEDULE survives, the Dash SELECTION does
> not. **KDD-045 CONFIRMED, now on two grounds: fairness + scaling.** That flag is
> RESOLVED → KDD-045 KEPT (decision 2026-07-10; the flag itself remains in the record).
>
> **Precision note (rotation semantics):** KDD-045 rotation is a FULL re-DKG producing
> a FRESH KEYPAIR; the old keypair is ABANDONED. It is NOT proactive resharing of the
> same key. Security consequence: shares stolen before a rotation are shares of a DEAD
> key — worthless; an attacker must corrupt ≥ t=6 of the same 11 members within ONE
> refresh window (window ≈ N). The refresh cadence N is the remaining security-vs-cost
> tunable (ODC-025; 1440 starting value; bounded below by ceremony duration M, M < N)
> — open, on the architecture surface.

> **[Amended 2026-07-10 — HANDOVER-AT-ACCEPT ADOPTED. The "During rotation:
> unavailable" sentence above is superseded by this amendment; the original text is
> retained per append-only discipline.]** On rotation, the OLD keypair remains ACTIVE
> and SERVICING until the successor PTXDKG record CONNECTS (is mined/accepted), then
> the swap is atomic at that block-connect (new record ACTIVE, old record SUPERSEDED,
> one connect event). **The quorum is never unavailable during rotation.**
>
> **Structural rationale (decisive):** the W2.1 registry forces the
> two-records-and-a-swap shape — quorum_hash is the record identity (the
> formation-anchor block hash, ptx_quorum_store.h:105) and ProcessBlock never
> overwrites an existing quorum_hash (the persist-boundary guard backing ODC-030/V9
> duplicate-formation). A same-set re-DKG therefore ALWAYS lands as a NEW record with
> the old record persisting alongside; the implementation plan's earlier "mutate
> group_pk in place on one record" sketch is unbuildable against the store as shipped.
> Handover-at-accept completes the shape the store imposes, and is a SMALLER build
> than revoke-at-start: trigger → MarkForming only (node-local, zero persisted
> change); accept → persist path + ConsumeFormingOnConnect + one new
> old-ACTIVE→SUPERSEDED transition at the same connect (undo mechanism shared with
> W2.4 disband); abort → ClearForming with the old record untouched.
>
> **Safety (recorded):** (1) no two-valid-keys ambiguity — FORMING is node-local and
> never persisted, so during the ceremony the old record is the only persisted ACTIVE
> one; GetActiveQuorumsAtHeight (state==ACTIVE) yields exactly one of the pair at any
> height on every node; rule = old-until-new-connects, then new, chain-derived. A
> post-swap roll against the old key fails at RESOLUTION (the old quorum is not
> selectable), not merely at signature verify. (2) historical verification intact —
> retirement is a state change, records are never deleted; a past beacon still
> verifies against its committed group_pk. Caveat (not handover-specific; disband has
> the identical shape): point-in-time "active as of a past height" is not
> reconstructible; additively fixable via superseded_height under nVersion 2 if ever
> needed. (3) compromise-window extension negligible — the old key lives ~N+M instead
> of ~N (+M/N ≈ +3.3% at N=1440/M≈47); old-key shares are useless against the new key
> (independent polynomials); no new attack class. Abort is SAFER by construction: the
> old key never left service and nothing persisted during an aborted ceremony — zero
> residue, retry at the next boundary.
>
> **Consequence for the rotation cadence:** the availability floor on N dissolves —
> N is bounded ONLY by the security ceiling (compromise window ≈ N). The Q=1
> launch-era rotation outage is eliminated. N remains open (ODC-025; 1440 starting
> value; hard M < N only).

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

> **[Amended 2026-07-10 — handover-at-accept adopted (§7.2 amendment).]** "Mid-ceremony
> quorums are skipped" no longer arises from ROTATION: a rotating quorum keeps servicing
> on its old keypair until the successor PTXDKG connects, so rotation never removes a
> quorum from the routable set. The skip rule survives only for quorums with no
> serviceable keypair at all — i.e. INITIAL formation (no predecessor key exists yet;
> nothing to route to until the first PTXDKG connects, which the ACTIVE-record predicate
> already expresses) — and for non-ACTIVE states (SUPERSEDED/DISBANDED). The router rule
> is simply: route over records with state == ACTIVE; no mid-rotation special case
> remains. The original sentence is retained above per append-only discipline.

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

**CLOSED (2026-07-08, W2.1 C5 close-out).** The anticipated split executed: clause 2
closed at W2.1; clause 1 split into its own open ODC (ODC-033, §9.12) — exactly the
"may split into separate ODCs if W2 resolves them on different timelines" path this
entry reserved.

1. **Staleness bound — SPLIT OUT → ODC-033 (§9.12), still open.** LLMQ enforces max-age via `cacheDkgInterval` (specialtx_validation.cpp:565–567); PTX has no interval analog and no formation cadence yet. Inventing a consensus constant before W2 defines rotation cadence is guessing. Continues unchanged under ODC-033, bound to W2.2/W2.3 cadence design.

2. **Cross-block per-formation uniqueness — CLOSED (W2.1 C1/C4, `0b403fc`/`55c1a5a`).** Implemented exactly as this clause specified: the chain-state index is `CPTXQuorumStore` (quorum_hash → record incl. accepted_txid), maintained at Connect (`ProcessBlock` write) / Disconnect (`UndoBlock` explicit-erase). Enforcement is a PAIR: the validation-surface check V9 in `CheckPTXDKGTx` (`ptxdkg-duplicate-formation`, DoS 100 — the reject with observability) + the persist-boundary guard in `CPTXQuorumStore::ProcessBlock` (defense-in-depth). The legality condition under rotation ("absent rotation/disband") is honored by construction: the record erases on disconnect and W2.3 rotation will transition state rather than accept a second formation at the same anchor. Falsified: battery T5 (duplicate populate REFUSED) + stub→RED (check bypassed → duplicate wrongly accepted) + positive (first formation accepts — no over-reject); re-falsified 2026-07-08 post-CC-restart. See §22.4. The C4 commit also landed the adjacent **member-containment check** (`ptxdkg-member-not-in-quorum`, load-bearing for KDD-061 — every committed member must hold a derivable share_index; V10, after V5) as part of the same enforceability split.

**Resolved with:** W2.1 registry (KDD-062) for clause 2; clause 1 → ODC-033.

**Raised:** 2026-06-13. Registered from W1.3_VALIDATION_SPEC_v1 §6. Closed 2026-07-08 (clause 2 at W2.1 C4 `55c1a5a`; clause 1 split → ODC-033 at W2.1 C5).

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

**W2.1 delta (2026-07-08, C5 close-out) — F-6 DISCHARGED; datadir-v2 premise superseded.**
The W2.1 C0 `operator_keys` populate mode (`727742c`) produces a **connect-valid** payload
with no ceremony and no transport — which removes the hard dependency ("connect-valid
payload = datadir-v2 = W2 formation orchestration") that bound this remainder to W2.2.
- **F-6 (accept path): DISCHARGED.** Exercised end-to-end on the N=22 ptxbea fleet, twice
  (C0 landing falsification — the first connect-valid PTXDKG ever accepted — and the C4
  re-falsification t1 run, 2026-07-08): validated no-force populate ACCEPTED (full
  `CheckPTXDKGTx` pass at tip) → mined through the real assembler keep path → connected
  through the real `ProcessSpecialTxsInBlock` → persisted record verified field-by-field
  (`ptx_quorum_info`).
- **STILL OWED (not run — do not narrow): V3-REORG-TRANSITION, C3-INVOCATION, F-9.** All
  three are now *mechanically unblocked* (C0 gives connect-valid payloads; `build_only`
  gives raw tx_hex for crafted blocks; the W2.0a fleet + battery machinery exist) but were
  NOT exercised at W2.1 — the owed binding moves from "blocked on datadir-v2" to "runnable
  on the W2.1 workbench, owed at W2.2 or earlier".

**Raised:** 2026-06-30 (architecture-chat). Registered from PTX_LE_STANDUP C-2 close-out.
Partially closed 2026-07-04 (C6/C7 close-out); F-6 discharged 2026-07-08 (W2.1 C5 close-out).

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

### §9.12 PTXDKG staleness / max-age bound (ODC-033 — split from ODC-030 clause 1)

**Open.** A PTXDKG may currently be accepted with an arbitrarily old formation anchor:
V1–V3 prove the anchor exists, matches the payload height, and sits on the connecting
chain — but there is **no max-age bound**, so a formation whose member selection was
snapshotted against a long-stale DGM list remains acceptable indefinitely. LLMQ's analog
is `cacheDkgInterval` max-age (specialtx_validation.cpp:565–567); PTX has no interval
because formation cadence does not exist yet — it is exactly W2.2/W2.3's rotation
semantics. Inventing the consensus constant ahead of that design is guessing.

Interim risk (updated from the ODC-030 assessment, tighter now): a conflicting second
group_pk for the same formation requires ≥ t members signing both (outside the KDD-059
trust boundary); a duplicate identical PTXDKG is now **consensus-rejected** (ODC-030
clause 2 closed — `ptxdkg-duplicate-formation` + the persist-boundary guard), not merely
bloat-bounded. The residual exposure is stale-anchor acceptance only.

**Resolve with:** W2.2 formation cadence / W2.3 rotation design — the bound's legality
condition and its constant come from whichever workstream defines the cadence.

**Raised:** 2026-06-13 as ODC-030 clause 1; split into its own ODC 2026-07-08 (W2.1 C5
close-out, per §9.9's anticipated split).

**ODC:** ODC-033

---

### §9.13 Fleet quorum saturation — no formation possible until disband exists (ODC-034)

**Open — roadmap dependency, not a footnote.** KDD-040 excludes every member of every
ACTIVE quorum from the formation pool. With N=22 GMs and two ACTIVE quorums of 11, the
pool is empty and **no further formation can occur**: `PTX_Formation_SelectAtAnchor`
takes its deterministic pool<11 skip at every boundary. Observed live on the W2 fleet at
boundaries h1280/h1360/h1440 (`PTX formation: pool below threshold at anchor … —
deterministic skip`), with both quorums ACTIVE (fh=960 mined h987; fh=1040 mined h1067).

This **blocks every downstream gate that requires a fresh ceremony**, not merely rotation
work: the fleet cannot form a new quorum at all until a disband path exists.

**Not a first.** The same saturation occurred in the SG-1a era — quorum #2 was mined at
h518 against the exact complement set ("exclusion EXACT (22/11/11, complement, empty
intersection)") — and the remedy used then was a **bank-restore to a clean pool**. So it
is a recurring operational condition with a known stopgap, which is why it has not
previously been registered; it is registered now because the stopgap is not a fix.

**Resolve with:** W2.4 disband/top-up (the real fix — returns members to the pool).
Interim: bank-restore. Raising N buys headroom (⌊N/11⌋ concurrent quorums) but does not
remove the terminal condition.

**Raised:** 2026-07-23 (SG-3 first sub-gate, observed live on the W2 fleet).

**ODC:** ODC-034

---

### §9.14 Fleet binary divergence — DO NOT DEPLOY the SG-3 image to GMs (ODC-038)

**Open — a deploy footgun; blocks GM deploy until W2.4.** As of 2026-07-23 the two
COORDINATOR nodes (172.31.0.33 / .34) run `hemis-ptx-w2:0101a44-dbg` (the SG-3
signing repoint + threshold fix), while ALL 22 GMs run the earlier
`hemis-ptx-w2:8719b7c-dbg`. The divergence is INTENTIONAL and CORRECT.

**Why it is intentional.** The SG-3 repoint (commit 0101a44) is COORDINATOR-SIDE
ONLY: the coordinator reads group_pk and share_index from already-committed
CPTXQuorumRecord data and interpolates the threshold signature. The GMs' side is
unchanged — `gm_bls_sign` already signs whatever DKG-produced share sits in
`g_ptx_my_bls_sk_bytes`. The GMs need NOTHING from 0101a44. Deploying it to them
buys nothing and risks everything below.

**★ The destruction chain (why a routine GM deploy is catastrophic here).**
Deploying a new image to a GM restarts its daemon. In sequence:
  1. A GM restart CLEARS its in-memory sk_share — `g_ptx_my_bls_sk_bytes` is a
     process-lifetime-only global with no persistence (ODC-035).
  2. Both quorums are ACTIVE on-chain — fc8e0f0d (fh=1040) and 57e7c7b4 (fh=960)
     — and their 22 members are exactly the whole fleet.
  3. KDD-040 (single-quorum-per-GM) therefore excludes every GM from the
     formation pool, so a fleet whose GMs lost their shares CANNOT reform the
     quorums.
  4. No disband path exists to return members to the pool (ODC-034), so the
     block does not clear on its own.
  => A fleet-wide GM deploy/restart DESTROYS BOTH QUORUMS PERMANENTLY. The only
     recovery is a bank-restore to a pre-h960 snapshot — which also discards
     every result landed after that height.

**When a GM deploy becomes safe.** Only (a) after W2.4 disband exists (members
can return to the pool and quorums can reform), or (b) as a DELIBERATE restore
where losing both quorums is the explicit intent (e.g. a fresh pre-h960 bank).
A routine "update the fleet to the latest image" is NOT safe until then.

**Resolve with:** W2.4 disband/top-up (removes the KDD-040 reformation block),
and ODC-035's share persistence (removes the restart-clears-shares hazard) —
either alone downgrades this from catastrophic to recoverable.

**Raised:** 2026-07-23 (SG-3 gate close).

**ODC:** ODC-038

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

**§C1 replay guard landed (W1.3, append-only update).** Both write sites now route through
a single guarded setter `PTX_BLS_SetSkShare` (`ptx_bls.cpp`, under `cs_ptx_my_bls_sk`):
`gm_bls_keyset` (`rpc/ptx.cpp`) and `PTX_DKG_StoreSkShare` (`ptx_dkg.cpp`). Behaviour is
**refuse-unless-empty** — a first-set (empty slot) stores; overwrite of an already-set share
is REFUSED (silent replay / second-coordinator takeover defense). No site writes
`g_ptx_my_bls_sk_bytes/_set` directly (grep-verified zero bypass). Count **reaffirmed as TWO**
(KDD-057's "two write sites" was always correct — no correction); `ptx_fanout.cpp:291` is the
RPC's fan-out *driver*, not a third write site.

*Why safe at W1.3:* the share is written only on local ceremony COMPLETION (`StoreSkShare`
fires at `phase==FINALIZE`; `gm_bls_keyset` is a single atomic RPC), and a failed/aborted
formation leaves the slot clean — so there is no wedged state and no fail-after-set attack
surface now (recon-confirmed).

*W2 forward-coupling (the failure path this guard is currently safe FROM, which W2 opens):*
once rotation/disband/re-formation can run with a share already set, a formation that fails
*after* a share is set wedges the node, and a malicious participant could grief formation to
wedge honest members. **W2 MUST provide BOTH:** (a) an explicit **authorized-overwrite** path
for legitimate replacement (clear-then-set / triggered bypass) — a plain overwrite is refused
by this guard; and (b) an **abort-clears-slot / recovery** mechanism so a failed rotation
self-cleans. **No runtime clear path exists today** — `g_ptx_my_bls_sk_set` is cleared only at
static init (`ptx_bls.cpp:22`), i.e. by daemon restart; **W2 must BUILD a clear path**, not
wire an existing one. The forming-vs-active discriminant is W2-bound (the committed/active
mark does not exist at HEAD).

**§C1 RATIONALE AMENDMENT (2026-07-24, KDD-069 — append-only; the text above is
unchanged and records what was true pre-069).** The trusted dealer is retired (KDD-069,
commit 8a2200e). With `gm_bls_keyset` removed, `PTX_BLS_SetSkShare` now has **exactly ONE**
write site: `PTX_DKG_StoreSkShare`, on local ceremony completion (the "two write sites" count
above was correct for its era; post-069 it is one). **The guard's rationale changes — this is
a rewrite of what §C1 defends, not a narrowing:** §C1 was framed against the *coordinator
write path* (the `gm_bls_keyset` unconditional-overwrite hijack). **That path no longer exists
— there is no coordinator write edge to the slot at all.** The refuse-unless-empty guard now
defends against **ceremony replay / double-store** (a member re-running a ceremony, or a
future rotation/re-selection overwriting a live share — KDD-070), NOT against dealer hijack. A
reader seeing "silent replay / second-coordinator takeover defense" against a dealerless
codebase would misread what it guards; it is recorded here so the code comments
(`ptx_bls.cpp` / `ptx_bls.h` / `ptx_dkg.h`, updated in commit 8a2200e) and this register agree.
The W2 forward-coupling above (authorized-overwrite + abort-clears-slot, no runtime clear
today) is unchanged and is exactly the KDD-070 slot mechanism.

**KDD:** KDD-057 (rationale amended by KDD-069)

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

## §21 Decided: Lagrange Index-Space Reconciliation — recovery-x = committed formation share_index (fix bound to DKG-go-live)

**KDD-061 (2026-07-06). Threshold recovery MUST evaluate each partial signature's
Lagrange point at that signer's COMMITTED FORMATION share_index — the score-order
CalculateQuorum rank (KDD-052/KDD-060) — NOT at list position and NOT at alphabetical
rank. The recovery-x for a signer must equal the formation-x for that signer, always.
This is a RECORD entry: the seam is real but latent; the cryptographic fix binds to
DKG-go-live (W2), not W1.3.**

**The seam (recon-confirmed at `b62d11d`).** Two index spaces exist for the same signer:

- *Trusted dealer* (`PTX_BLS_Init`, `ptx_bls.cpp:70-74, 95-115`): shares created at
  x = 1-indexed **alphabetical** node_id rank; recovery (`rpc/ptx.cpp:255`,
  `PTX_BLS_GetNodeIndex`) looks up the same alphabetical map. Self-consistent —
  today's live path is index-correct for the shares it consumes.
- *DKG* (`ptx_dkg.cpp:314-318`; `PTX_DKG_ComputeSkShare`): shares created and
  aggregated at x = **score-order share_index** (`members[i].share_index = i + 1` in
  CalculateQuorum output order, `ptx_dkg.cpp:261` — the single assignment site).

Alphabetical rank ≠ score rank in general. A DKG-produced share recovered via the
dealer-alphabetical lookup interpolates at wrong evaluation points and produces a
structurally-valid-but-wrong group signature — caught only by the generic
`PTX_BLS_Verify` failure, with nothing pointing at the index mismatch.

**Reachability: LATENT at HEAD.** No live flow pairs the two spaces: `ClosePhase5`
has no production caller (debug RPC + tests only); DKG never writes `g_ptx_bls_state`
(`PTX_GetBLSState` has zero call sites); the GM signs with whatever occupies the
sk-share slot and never reports its own index — the coordinator alone derives it.
The seam opens exactly when DKG output replaces the trusted dealer as recovery's
source. **The fix lands with that transition; W1.3 records the mapping (this entry).**

**Preserve-gaps under QUAL exclusion (source-confirmed).** share_index is assigned
once at `PTX_DKG_InitSession` (`ptx_dkg.cpp:261`) and never rewritten; exclusion
marks members via `qual`/`bad_members` sets only — `session.members` is never
mutated or re-sorted, and `PTX_DKG_ComputeSkShare` aggregates every survivor at its
ORIGINAL share_index across effective-QUAL dealers. Survivors therefore keep
**gapped** indices (e.g. {1,2,4,5,…}). Gaps are harmless to Lagrange — interpolation
needs *correct* x, not *contiguous* x. Below-threshold (survivors < t=6) →
`ABORTED` and re-form (`ptx_dkg.cpp:1062-1066`) — the only case that forces a new
formation; at/above t=6 the quorum completes under-strength and is threshold-secure.
(Renumbering survivors contiguously would be a correctness hazard — already-dealt
shares would no longer match their evaluation points without a full re-deal. The
code does not renumber; any future change that does MUST re-deal.)

**HARD W2 CONSTRAINT — the payload MUST materialize share_index per member.**
Today `PTXDKGPayload.member_node_ids` is committed in share_index ORDER but does not
materialize the index VALUES (`ptx_dkg.h:214-231`; built `ptx_dkg.cpp:1387-1408`),
and `PTXDKGPhase4Msg` carries none. Under exclusion, list position ≠ share_index − 1
(e.g. surviving indices {1,2,4,…} collapse to positions {0,1,2,…}), so recovery
CANNOT reconstruct the evaluation points from the payload alone. W2 MUST either
commit explicit **(node_id, share_index) pairs** or make share_index re-derivable
from `quorum_hash` + the committed exclusion record. This makes every evaluation
point AUDITABLE from committed data — the "verifiable" in the verifiable beacon.
A W2 payload format without materialized indices makes correct recovery impossible
under exclusion; this constraint is the reason this entry exists at W1.3.

**Testability requirement (binds to the DKG-go-live fix).** The reconciled recovery
path must surface an index mismatch DIAGNOSTICALLY — an explicit index-consistency
check (recovery-x vs committed formation share_index per signer) — not as the
generic `PTX_BLS_Verify` "verification failed". Otherwise a future index bug is
undiagnosable: silent-wrong, caught only by verify.

**W2 lifecycle note (falls out of preserve-gaps).** A quorum may COMPLETE
UNDER-STRENGTH — formed-11, completed-9 after exclusions, still ≥ t=6. The W2
registry MUST be able to represent a born-under-strength quorum and decide its
lifecycle: topped-up, or run under-strength to rotation (KDD-045). Recorded here
alongside the seam because both fall out of the same preserve-gaps fact.

**Status:** Decided 2026-07-06. RECORD-only at W1.3 (no cryptographic change —
correctly, since the seam cannot be exercised until DKG-go-live). Fix + diagnostic
check bind to W2 DKG-go-live; payload materialization is a hard W2 format constraint.

---

## §22 Decided: W2.1 Quorum Registry — persisted PTXDKG record, event-based ACTIVE mark, state-machine skeleton

**KDD-062 (2026-07-08). The quorum registry is a chain-derived evodb store: one
versioned `CPTXQuorumRecord` per ACCEPTED PTXDKG, written at block-connect and
explicit-erased at block-disconnect (LLMQ mined-commitment pattern). ACTIVE is
an EVENT-based predicate — "an accepted PTXDKG connected on the current chain"
— not a provenance-based one. The registry owns the §C1 forming-vs-active
discriminant: the committed/active mark IS the persisted record with
`state == ACTIVE`.**

### 22.1 Persisted record (v1, version-tagged)

`src/ptx/ptx_quorum_store.h` `CPTXQuorumRecord`, serialized under a leading
`nVersion` — future fields land ADDITIVE under a bumped version; a reader keys
on the version; undo never deserializes the record (erases by key from the
disconnected block's payload) so undo is version-agnostic. evodb keys:
`("pq_r", quorum_hash) → record`; `("pq_h", htobe32(UINT32_MAX − mined_height))
→ quorum_hash` (most-recent-first iteration, the LLMQ inversed-height trick).

Fields: quorum_hash (anchor = identity), formation_height, group_pk, vvec_hash,
**the FULL selected-11 in KDD-052/060 score order with per-member (node_id,
proTxHash, share_index 1..11, in_qual)** — the KDD-061 materialization, derived
at connect through the SAME `PTX_DKG_SelectQuorumFromList` call the V5 check
just ran (one-function contract; satisfies KDD-061's "re-derivable from
quorum_hash" arm with no payload format change — whether the PAYLOAD also
materializes indices stays a W2.2 tx-format decision); formed_size /
completed_size (born-under-strength representable: formed-11/completed-k,
k ≥ t, gaps preserved by construction); state; provenance; accepted_txid;
mined_block_hash/height; reserved lifecycle fields (last_rotation_height,
drift_offset, consecutive_inquorate_blocks) written as sentinels.

**Provenance (reserved, always UNSET at W2.1):** provenance is NOT
chain-derivable — a debug-injected and a ceremony-formed PTXDKG are
byte-identical by design, and the record is consensus-derived state
(`-reindex` must reproduce records byte-identically on every node). The field
exists so W2.2 has a landing slot if distinguishing formed-vs-injected ever
matters; any setter must preserve reindex-determinism. The LEGIT producer of
an ACTIVE quorum is W2.2 formation; the W2.1 debug-injected substrate
exercises the same connect event, and W2.2 inherits the ACTIVE predicate
unchanged (it produces the same event; nothing to fight).

### 22.2 State machine (full design; producer-pending discipline)

States: `FORMING` (node-local ONLY, never persisted — nothing is on chain
until accept; producer W2.2), `ACTIVE` (the §C1 mark; producer LIVE —
accepted-PTXDKG-at-connect), `ROTATING` (schema only; W2.3), `DISBANDED`
(W2.4). UNDER-STRENGTH is a **marker, not a state** (`completed_size <
formed_size` on an ACTIVE record) — it composes with states instead of
forking the transition table; top-up vs run-to-rotation stays KDD-045/W2.4.

| # | Transition | Producer | W2.1 status |
|---|---|---|---|
| T-A | (none)→ACTIVE at connect of accepted PTXDKG | LIVE (C0 debug inject → mine) | **FALSIFIED** (battery T1) |
| T-B | ACTIVE→(none) at disconnect | LIVE (invalidateblock) | **FALSIFIED** (T2 + STUB-UNDO RED) |
| T-C | re-connect re-persists | LIVE (reconsiderblock) | **FALSIFIED** (T8) |
| T-D | (none)→FORMING | W2.2 formation trigger | producer-pending (`MarkForming`) |
| T-E | FORMING→ACTIVE consumption | W2.2 (ceremony completes) | producer-pending (`ConsumeFormingOnConnect` — live no-op hook on an always-empty map) |
| T-F | FORMING→aborted (§C1 abort-clears-slot authorization) | W2.2 abort | producer-pending (`ClearForming`); the sk-share CLEAR PATH itself is built at W2.2 |
| T-G | ACTIVE→ROTATING→ACTIVE′ | W2.3 | enum + reserved fields only |
| T-H | ACTIVE→DISBANDED | W2.4 (KDD-047, 30 inquorate) | producer-pending (`MarkDisbanded`); W2.4 wires the block event AND its disconnect-undo |
| T-I | (condition) member DGM-deregistration mid-life (BUG-019 class) | any collateral spend | record snapshots identity at formation (nothing dangles); ejection semantics W2.4 |

**Producer-pending contract:** T-D/E/F/H functions compile, are reviewed, have
no production caller (T-E's hook runs against an always-empty map), and are
NOT claimed tested — no synthetic direct-state-injection test exists. Each is
register-marked "falsification bound to W2.2 / W2.4."

**State-mutation persistence (W2.3/W2.4 forward-look):** ROTATING/DISBANDED
transitions will be block-event-driven and need their own connect-write +
disconnect-undo of the state mutation. The versioned record makes that
additive; the mechanism (state-diff keys vs record-rewrite + undo journal) is
deliberately NOT decided at W2.1 — decided by the workstream that produces the
events.

### 22.3 Undo is explicit-erase (not DGM cache-only)

The DGM manager's undo erases only in-memory caches — safe there because its
evodb keys are block-hash-keyed and reachable only by active-chain walk. This
store has EXISTENCE semantics (`Exists(quorum_hash)` backs ODC-030 uniqueness)
and a height-iteration index: a stale key after reorg is *visible* (phantom
quorum; wrongly-firing uniqueness reject). The LLMQ explicit-erase undo is the
correct member of the pattern family; the STUB-UNDO battery RED demonstrates
the corruption cache-only undo would cause. E-5 held: no re-pend on disconnect
(re-submission is W2.2's; register-marked).

### 22.4 ODC-030 enforceability split (registry-level)

NOW (implemented at the persist boundary in C1; validation-surface twins at
C4): one-accepted-PTXDKG-per-quorum_hash (`ptxdkg-duplicate-formation` — §9.9
clause 2 verbatim) and committed-member containment
(`ptxdkg-member-not-in-quorum` — load-bearing for KDD-061: every committed
member must hold a derivable share_index; recon-confirmed absent from V1–V8,
which count-check `member_node_ids` only). DEFERRED, register-marked:
staleness/max-age (needs W2.2/W2.3 cadence — §9.9 clause 1; split executed at
C5 → ODC-033, §9.12) and cross-quorum membership / single-quorum-per-GM
(KDD-040 — a W2.2 formation-side pool rule; two overlapping quorums are
mechanically injectable NOW via the debug RPC, but enforcing a consensus
reject would invent overlap-legality semantics ahead of W2.2/W2.3 rotation —
deferral is semantic, not a testability gap).

**Status:** Decided 2026-07-08 (W2.1 build). Registry/store landed at W2.1
C1/C2; falsified rows and producer-pending marks per the table above.

**KDD:** KDD-062

---

## §23 Decided: Handover-at-Accept Rotation

**KDD-063 (2026-07-10). Splits the handover mechanics out of the KDD-045 §7.2
amendment into its own entry; KDD-045 (same-set re-DKG, fairness + scaling grounds)
is unchanged and cross-referenced.**

**Decision.** On KDD-045 same-set re-DKG rotation, the old keypair stays ACTIVE and
servicing until the successor PTXDKG record connects; the swap is atomic at that
block-connect (new record ACTIVE, old record SUPERSEDED). The quorum is never
unavailable during rotation.

**Structural ground.** The W2.1 store forces two-records-and-a-swap: quorum_hash is
the immutable record identity and ProcessBlock never overwrites an existing
quorum_hash (V9/ODC-030 persist-boundary guard), so in-place group_pk mutation is
unbuildable; handover is the natural, smaller build (trigger → MarkForming only,
node-local; accept → persist path + ConsumeFormingOnConnect + one
old-ACTIVE→SUPERSEDED transition at the same connect; abort → ClearForming, old
record untouched).

**Safety.** No two-keys ambiguity (FORMING never persisted; exactly one of the pair
ACTIVE per height; rule = old-until-new-connects, chain-derived; a post-swap old-key
roll fails at resolution). Historical verification intact (records never deleted; a
past beacon verifies against its committed group_pk). Compromise-window extension
~+M/N (~+3.3% at N=1440/M≈47) — negligible. Abort safer (zero persisted residue;
retry next boundary).

**Payoff.** The availability floor on N dissolves — N is security-ceiling-only
(compromise window ≈ N). The Q=1 launch-era daily rotation outage is eliminated.
M-provisionality no longer gates the N decision (hard M < N only).

**Owed to W2.3:** ROTATING→SUPERSEDED repurpose; the old-ACTIVE→SUPERSEDED-at-connect
transition + its undo (state-mutation undo mechanism shared with W2.4 disband T-H);
the rotation-validation arm (a rotation's members == the predecessor record's 11, not
a fresh draw at the new anchor).

**Cross-ref:** KDD-045 (§7.2 + its 2026-07-10 amendments), KDD-062 (§22 store
semantics), ODC-025 (cadence N, open).

> **[P-b4 discharge note, 2026-07-25 — appended, no prior line edited]** Two of
> the three "Owed to W2.3" items above are DISCHARGED at KDD-072 P-b4:
> **(1) ROTATING→SUPERSEDED repurpose** — enum value 2 renamed (persisted bytes
> stable), producer = `MarkSuperseded` at successor-connect; **(2) the
> old-ACTIVE→SUPERSEDED-at-connect transition + its undo** —
> `MarkSuperseded`/`RestoreActiveOnUndo`, idempotent pair, stamps
> `superseded_height` (record v2) for the ODC-042 as-of predicate. The third
> item (**the rotation-validation arm** — members == predecessor's 11) remains
> owed to KDD-072 **P-b3** (V12).

**KDD:** KDD-063

---

**KDD-070 (2026-07-24). The GM sk-share slot mechanism — keyed, persisted, block-connect-promoted multi-share slot with reorg-safe retention. Supersedes KDD-067's owed-note (KDD-045→KDD-063 precedent).**

*Context:* KDD-069 retired the trusted dealer. `gm_bls_keyset` is gone, so the GM slot (`g_ptx_my_bls_sk_bytes[32]`, `ptx_bls.cpp:18`, under `cs_ptx_my_bls_sk`) has exactly ONE write edge — `PTX_DKG_StoreSkShare` at FINALIZE — by construction, and §C1's rationale is already amended (KDD-069) to ceremony-replay/double-store protection. This entry designs the replacement for the single global. NOT implemented — bound to W2.3 rotation (first consumer) and W2.4 disband.

**1 — Governing constraint (first).** The PENDING slot is writable ONLY from the ceremony FINALIZE call-path (`PTX_DKG_StoreSkShare` → a new internal `SetPendingShare`), enforced by the **call graph**: no RPC handler has an edge to it. **Promotion fires on block-connect ONLY, never on operator/RPC input.** KDD-069 already removed the sole RPC write edge (`gm_bls_keyset`), so this is now a **structural** property, not a guard to maintain — there is no operator-reachable path to create or promote a share. Any future RPC that writes the slot reintroduces the §C1 hazard and is forbidden.

★ **P5 correction (2026-07-24) — the guarantee is "no rpc-reachable write path," NOT "one write edge."** The original "single write edge, by construction" framing (Context, above) was accurate for P1 but is imprecise now that the mechanism is built. At HEAD `g_ptx_my_shares` has a **FULL ENUMERATION of EIGHT mutators** (every function that inserts / erases / role-changes / clears the map), not one: `PTX_BLS_SetSkShare` (guarded, §C1), `PTX_BLS_Promote`, `PTX_BLS_ExpirePending`, `PTX_BLS_DiscardSuperseded`, `PTX_BLS_UndoPromote`, `PTX_BLS_LoadShares`, `PTX_BLS_ReconcileShares`, `PTX_BLS_WipeShares`. **Reconciling with the P4 report (do not conclude a report was wrong):** P4 listed *five touch-sites* because it was answering a NARROWER question — *does the revert create a NEW write path?* (answer: it mutates in place, so no map insert appears among the write/erase sites it enumerated). That was a different question, not a different count. This §1 enumeration is the COMPLETE mutator set the P5 check asserts over. The undo revert (`UndoPromote`) **must** mutate the map directly rather than through the guarded setter — the setter refuses on §C1 (predecessor already present) — so it is a second, *unguarded* mutator by necessity. A cross-TU write path therefore cannot be "static" or "file-local"; the invariant is **no RPC-reachable write path**, and the enforcing mechanism is the **P5 structural check** (`P5_ShareStore_NoRpcReachableMutator`, `ptx_dkg_phase5_tests.cpp`), which asserts over the COMPLETE eight-mutator set that none is referenced under `src/rpc` (negative limb) while each is defined in `ptx_bls.cpp` (positive limb, so a rename fails loudly instead of the check passing vacuously). If the build omits the injected `-DPTX_SRCDIR`, the check HARD-FAILS ("could not run") rather than passing vacuously. A narrower `ptx_bls.h` (rpc includes only the sign/recover surface, never the mutators) is a recommended follow-up — it would make an rpc call a compile error, strictly stronger than the grep — **owed, not done in P5.**

**2 — State shape.** Replace the single global with `std::map<uint256 /*quorum_hash*/, HeldShare>` under `cs_ptx_my_bls_sk`, where `HeldShare { uint8_t bytes[32]; int formation_height; Role role; int promotion_height; }`, `Role ∈ {CURRENT, PENDING, SUPERSEDED_RETAINED}`. Keyed by quorum_hash (successor/predecessor differ — ProcessBlock never overwrites, §7.2). **Invariants stated PER-QUORUM, not globally:** at most one CURRENT per quorum_hash; at most one PENDING per in-flight rotation; SUPERSEDED_RETAINED read-only pending discard. **Do NOT bound the map at 2 entries** — the "≤2 concurrent" figure is a *consequence of KDD-040 (one-quorum-per-GM) today*, a fact about the present fleet, not a design assumption. W2.5 multi-quorum may relax KDD-040 (a GM in L quorums → up to L CURRENT); the per-quorum invariants hold unchanged at any L. Record KDD-040's bound as today's fact; never encode it in the type.

**3 — Persistence lands WITH promotion (not deferred to ODC-035).** The offline-across-promotion case is a *silent drop during a scheduled protocol operation*: a restart between FINALIZE and the successor's connect loses the memory-only pending, the member cannot promote, yet the chain lists it in the successor's membership (ODC-035, now during rotation). It also undermines rotation-as-heartbeat (a signal that silently drops members degrades what it measures). Therefore, in scope now:
- **Where:** persist to **`evoDb`**, per Dash's inherited precedent — `DB_QUORUM_SK_SHARE = "q_Qsk"` (`quorums.cpp:26`, written `:112`, read on restart `:129`). ★ **Cross-apply check (2026-07-24, gated this recommendation):** evoDb would be UNSAFE only if bank snapshots both (a) include evodb AND (b) are ever applied from one node to a *different* node — a cross-applied snapshot would hand the target the source's sk_share, and one node holding two members' shares silently breaks the threshold assumption. **Checked `bank_fleet.sh`/`restore_fleet.sh` + the datadir-surgery heal recipe: (a) YES (evodb is in the datadir, banked with the whole `datadirs/`), (b) NO** — bank/restore move the ENTIRE `datadirs/` as one fleet-wide unit (node-preserving), and surgery WIPES chainstate/evodb and does a fresh IBD from the network (never a donor-copy). So evoDb is safe here, and *reinforced*: it rides the same wipe/restore/reconciliation lifecycle a separate store would have to duplicate (surgery wipes evodb but KEEPS `wallets/` — a separate `ptx_shares.dat` would survive the wipe and strand stale shares). **Two load-bearing caveats:** (i) safety is contingent on the **node-preserving invariant** (snapshots fleet-wide-node-preserving; surgery = wipe-resync, never donor-copy) — a manual cross-copy defeats it; (ii) §4 reconciliation keys on quorum-hash *existence*, not membership, so it does NOT backstop a cross-applied datadir whose quorum still exists on the shared chain — the invariant is the protection, not reconciliation. If either invariant is ever relaxed, move shares to a **separate node-local store**, which must then duplicate what evoDb gives for free: startup reconciliation-against-chain (§4), the block-disconnect undo hook (§9), and the surgery/restore wipe lifecycle.
- **What serializes:** the FULL `HeldShare` (bytes + formation_height + role + promotion_height), keyed by quorum_hash — NOT just the 32 bytes (role/promotion_height are load-bearing for restart recovery §8 and depth-discard §6).
- **At-rest protection:** the operator BLS private key is ALREADY plaintext in node config (`-gmoperatorprivatekey`, `tiertwo/init.cpp:39`), so plaintext shares in evoDb are not a *new* exposure class — a node reading evoDb already reads the operator key. Precedent acceptable for parity; record that both secrets share an unencrypted at-rest posture; encrypt-at-rest for both is a separate, larger decision, NOT a KDD-070 blocker.
- **Subsumes ODC-035?** *Partially.* Persisting CURRENT **is** the ODC-035 persistence fix and falls out of KDD-070 — CURRENT-share persistence is **not a separate piece**. What remains open: the **on-chain/RPC liveness signal** (member genuinely offline, share intact but process down) — that is **KDD-071**.

**4 — Startup reconciliation and wipe (new, from persistence).** A bank-restore rewinds the chain; on-disk shares survive it — the member then holds material for quorums absent on the restored chain, and under §C1 those stale slots BLOCK fresh formation (the KDD-067 wall, now durable instead of restart-cleared). Specify: (a) **startup reconciliation** — on daemon start (and on tip-changes that could be restore/reorg), discard any HeldShare whose quorum_hash is not a record on the active chain (orphans); selective, unlike restart-clears-all — keeps live quorums' shares (the §3 fix), drops orphans; (b) **explicit wipe** — a callable clearing ALL held shares, invokable by `restore_fleet.sh`. **Cross-ref ODC-034** (bank-restore relied on restart-clears-slots — the wipe is its explicit successor) and **ODC-037** (restore already owes a banlist-clear step; KDD-070 adds a SECOND — share-wipe). `restore_fleet.sh` now owes both.

**5 — Explicit signing selection; ONLY CURRENT signs.** `gm_bls_sign` takes a `quorum_hash` and selects the HeldShare by key. **Not** the minimal "always sign CURRENT-implicitly" shape — it makes correctness depend on the coordinator never asking the old quorum to sign during handover, and KDD-063 confirms that failure is live. ★ **Only a CURRENT share signs.** SUPERSEDED_RETAINED is retained for the UNDO (§9), NOT for availability: signing with it would mean **two live keys for the same membership for maxreorg+margin blocks after every rotation**, defeating the point of key refresh (an attacker who stole the old share before rotation could still sign). **Refuse SUPERSEDED with a clear error** ("share for quorum <hash> is superseded, not signable"). The reorg case is covered by the **role flip, not by signing the superseded share**: on a promotion-unwinding disconnect (§9) the un-promote makes the old share CURRENT again, and only THEN is it signable — so during a valid reorg the correct share is CURRENT by the time a roll references its quorum. **Not held:** hard-error ("this node holds no share for quorum <hash>") — never sign with a different quorum's share, never fall back. Refuse PENDING (its group_pk is not yet ACTIVE).

**6 — Retention margin.** SUPERSEDED_RETAINED held until the successor is beyond **`maxreorg + MARGIN`** deep, DEPTH-based (`tip − promotion_height ≥ DEFAULT_MAX_REORG_DEPTH + MARGIN`; maxreorg=100, `consensus.h:35`). Exactly-100 is unsafe: a disconnect at depth 99 with the old share already discarded strands the member permanently. **MARGIN = 20 (retain to depth 120).** The margin must exceed the skew between a node observing "depth ≥ 100" and the last block a reorg could still land; ~20 blocks (~20 min at 60s) covers observation/clock skew and a near-maxreorg reorg arriving as the node crosses the threshold, while trivial in storage (one 32-byte share + metadata). Depth-based because the reorg bound is depth-based.

**7 — Pending expiry (PROVISIONAL).** A ceremony can FINALIZE yet its successor never connect (unmined, or superseded by a later rotation). Unbounded PENDING is a stale share a much-later connect could wrongly promote. Specify: discard PENDING when `tip − formation_height > PENDING_TTL`, checked by the §4 reconciliation sweep + a per-block-connect check. ★ **PENDING_TTL is bounded by the FINALIZE→successor-connect latency, NOT by the rotation interval N.** N is the interval to the *next* rotation, not a bound on connect latency; using N would leave promotable material live far too long. The real bound is the maximum plausible delay from FINALIZE to the successor connecting, which under **KDD-058-A (any-staker inclusion from the replicated minable-commitments store)** should be a **few blocks** (any staker can mine the successor as soon as it validates). **The tension is explicit:** too long leaves stale promotable material live (a late connect wrongly promotes); too short drops members spuriously (a slow-but-legitimate connect finds its pending already expired). **Value PROVISIONAL pending measured landing latency; measurement is OWED at W2.3's first live rotation** (measure FINALIZE→connect over real rotations, set PENDING_TTL to a safe multiple of the observed max). **Late connect (after expiry):** promotes NOTHING for this member (no pending) → member dropped from the successor as an offline member would be (§8), recovers only via re-selection — correct: an expired pending means the member already treated the rotation as failed. **Key isolation:** a PENDING keyed to quorum X **must not** promote on a connect for quorum Y — promotion matches the connecting record's quorum_hash to the PENDING key exactly; mismatch is a no-op.

**8 — Lifecycle** (trigger · local/block-driven · offline behaviour): **store-pending** — ceremony FINALIZE; node-local, persisted immediately; offline for the ceremony → never computes the share → misses rotation (**GAPPED** — a DKG share can't be self-derived; recovery = re-selection). **promote** (pending→current, old→superseded) — successor BLOCK-CONNECT; block-driven; **NOW RECOVERABLE** (restart across connect: reconciliation sees successor ACTIVE + persisted PENDING → promotes — the case persistence closes). **retain-superseded** — same connect; block-driven; recoverable. **discard-superseded** — successor beyond maxreorg+margin (§6); block-driven; safe/idempotent. **abort-clear** — ceremony aborts (<t QUAL / ClosePhase fail); node-local, clears PENDING only; offline → no-op. **expire-pending** — PENDING_TTL exceeded (§7); block-driven/reconciliation; idempotent. **Recoverable vs gapped:** persistence makes promote/retain/discard/expire offline cases recoverable; the one **remaining gap** is store-pending-while-offline-for-the-whole-ceremony (material never produced can't be persisted; recovery = re-selection) — a *liveness* concern (KDD-071), not a slot-mechanism gap.

**9 — Undo.** `CPTXQuorumStore::UndoBlock` (`ptx_quorum_store.cpp:164`) today only ERASES the record on disconnect. KDD-070 requires an ADDITIONAL state-mutation revert on a promotion-unwinding disconnect: successor de-activated, predecessor SUPERSEDED→ACTIVE, AND the member's slot un-promoted (SUPERSEDED_RETAINED(pred)→CURRENT, discard reverted CURRENT(succ)). ★ **This revert is the primitive SHARED with KDD-063 handover and W2.4 disband T-H — one implementation, three consumers.** What un-promotes the slot: the disconnect hook, keyed by the disconnected block's payload quorum_hash, flips the HeldShare roles back — which is WHY §6 retains SUPERSEDED to maxreorg+margin (the un-promote needs the old share present) and WHY §5 refuses to sign it until the flip makes it CURRENT.

**10 — W2.4 degenerate path.** disband→reform = `retain-clear-then-store`: reuses retain/discard/undo, adds only "store-into-empty on reform" (fresh formation into a quorum_hash with no CURRENT — §C1 first-set). No store-pending, no promote-swap (the quorum just stops). **The reorg-retention arm IS still needed** — a disband can be reorged out, so the dead share must be retained-then-discarded across maxreorg+margin, else a disband-revert strands the member. So "retain-clear-then-store," not naive "clear-then-store."

**11 — Register.** KDD-070 = this design; KDD-067 gets a pointer (SUPERSEDED by KDD-070), per the KDD-045→KDD-063 split precedent. **KDD-071 reserved** for the liveness/disband trigger *if it survives* recon — and note **rotation-as-heartbeat (KDD-045 periodic same-set re-DKG, consensus-scheduled via the drift fields) may answer most of KDD-071**: a mandatory periodic rotation a member fails to join is an on-chain-observable "couldn't muster the member" signal — most of what a disband trigger needs. So KDD-071 may never be needed separately; recorded so.

★ **IMPLEMENTATION CLOSE-OUT (P1–P5, 2026-07-24).** The slot mechanism is BUILT and UNIT-VERIFIED across five packages: **P1** `51fd2dd` (keyed multi-share store + §C1 per-key guard), **P2** `068ec4a` (evoDb RAW persistence, startup reconciliation, wipe, memory-only flag), **P3** `e2bfaed` (PENDING role, store-pending, key-isolated promotion, TTL expiry), **P4** `9ecc76c` (SUPERSEDED retention, depth-discard at maxreorg+MARGIN=120, keyed idempotent undo revert), **P5** (this — structural §1 check + close-out). ≈36 KDD-070 unit tests; 312 `test_ptx` cases green.

- **What is built vs what is owed — plainly:** the mechanism is COMPLETE and has **ZERO production callers**. Five entry points have **no call site** (grep-checkable, repo-wide): `PTX_BLS_Promote`, `PTX_BLS_ExpirePending`, `PTX_BLS_DiscardSuperseded`, `PTX_BLS_UndoPromote` (the `UndoBlock`→revert), and `PTX_DKG_StoreSkShare(…, PENDING)` (store-pending at FINALIZE). The functions exist, are tested, and are **unreachable** — promotion/expiry/discard/undo are reversible/functional in CODE but do not fire in PRACTICE until the block-connect / block-disconnect / FINALIZE call sites are written. **First consumer: W2.3** (rotation) / **W2.4** (disband + the record-side revert §9).
- **Two mutators clarification (from P4, §1 P5 correction above):** `g_ptx_my_shares` now has eight mutators, one guarded (`SetSkShare`) and seven not, including the necessarily-unguarded `UndoPromote` revert. §1's guarantee is *no rpc-reachable write path* (P5-enforced over the complete set), not the P1-era "single write edge."
- **Share-slot half only (§9):** P4's `UndoPromote` builds the SLOT-side revert. The **record-side** revert (successor de-activated, predecessor SUPERSEDED→ACTIVE in `CPTXQuorumStore::UndoBlock`) is **NOT built** — it is the primitive shared with KDD-063 handover / W2.4 disband T-H and lands with whichever arrives first. `UndoPromote` is keyed, idempotent, and order-independent so it composes with that revert at the same disconnect.
- **What remains UNVERIFIABLE in test_ptx (owed to fleet, W2.4):** the on-start reconciliation TRIGGER (init ordering — the function is unit-tested, its wiring at `LoadTierTwo` is inspection-only); durability across a REAL process restart (unit tests use an in-memory evoDb); and the three block-driven triggers (promote on connect, discard/undo on disconnect, expire on tip-advance) — the undo trigger needs a genuine reorg, the weakest coverage of the set (ODC-032: block connect/disconnect not simulable without chain fixtures). **KDD-070 is UNIT-VERIFIED ONLY; fleet verification is bound to W2.4.** A GM deploy destroys both quorums (ODC-034/038) — do not deploy, restart a GM, or roll to exercise it.

**Status:** ★ **P1–P5 BUILT, unit-verified only** (was: Design recorded, NOT implemented). Mechanism complete with ZERO production callers — first consumer W2.3 / W2.4; fleet verification bound to W2.4. Cross-ref KDD-063 (shares the undo primitive — record-side half owed), KDD-069 (P1-era single write edge, now superseded by the two-mutators/no-rpc-reachable framing), KDD-067 (superseded), ODC-034/037 (restore owes wipe + banlist-clear), ODC-035 (persistence subsumed; liveness → KDD-071), KDD-040/045 (the ≤2 bound is KDD-040's, relaxed by W2.5).

**KDD:** KDD-070

---

**KDD-072 (2026-07-24). PTXDKG payload linkage — versioned payload + SIGNED predecessor for rotation validation. The first consensus-tx-format change in PTX.**

*Context:* W2.3 rotation (KDD-045/063) cannot validate without a consensus-visible way to (a) mark a PTXDKG payload as a rotation and (b) name its predecessor quorum. `PTXDKGPayload` (ptx_dkg.h:351-366) has neither and is **unversioned** — the only special-tx payload in the tree without an `nVersion`. Every node must branch identically, so this is a **consensus tx-format change, the first in PTX.** This entry (a recon gate) settles the format; the rotation validator and KDD-070's five call sites depend on it. NOT implemented.

**1 — Decision: version the payload (not a new nType).** Add `uint16_t nVersion{CURRENT_VERSION}` as the first serialized field of `PTXDKGPayload`; rotation fields land additively under `if (nVersion >= 2)`. A rotation **is** a PTXDKG (same group_pk/vvec/members/premits artifact) plus a predecessor link and a same-set constraint — not a new artifact class. It versions in place (the CFinalCommitment idiom), not forks a new nType (the ProReg-vs-ProUpServ idiom, which separates *different* artifacts). ~90% of `CheckPTXDKGTx`'s V-sequence is shared with one branch point (§5). A new nType=12 would duplicate V1-V4/V9/V11 (lockstep-drift risk) and fragment the one-per-block rule (`CheckPTXDKGBlockRules`, keyed on nType==PTXDKG) and store routing across two types. nType 12 stays unclaimed; 7/8 remain reserved-off (KDD-056).

**2 — Prior art (this conforms; it does not deviate).** Every special-tx payload in the tree carries `uint16_t nVersion{CURRENT_VERSION}` as its first field with a uniform gate `if (nVersion == 0 || nVersion > CURRENT_VERSION) DoS(100, "bad-*-version")`: ProRegPL / ProUpServPL / ProUpRegPL / ProUpRevPL (providertx.h:22/82/110/149) and CFinalCommitment / LLMQCommPL (llmq/quorums_commitment.h:29/74), gated at specialtx_validation.cpp:198/342/399/495/539/601; additive fields under `if (nVersion >= K)` (providertx.h:54/57). **PTXDKGPayload is the sole unversioned special-tx payload** — versioning removes an anomaly.

**3 — ★ THE UNSIGNED-PREDECESSOR HOLE (the centrepiece).** The premit signatures do **not** cover a predecessor. `PTXDKGPhase4Msg::GetSignHash` binds `SHA256(quorum_hash ‖ proTxHash ‖ group_pk_bytes ‖ vvec_hash)` (ptx_dkg.cpp:30-44), and `PTX_DKG_VerifyPremits` asserts `p4.quorum_hash == payload.quorum_hash` (:192) — premits attest the **successor's own anchor and nothing about a predecessor.** So if `predecessor_quorum_hash` were merely a payload field (unsigned), this re-cast passes **every** check in the sequence: the attacker attaches the predecessor link to a formation they legitimately produced, V12's `members == predecessor.members` passes, V6-V8 premits verify, and the ACTIVE quorum is marked SUPERSEDED **without its operators consenting to a rotation.** ★ **PRECONDITION — the bar, stated so the entry reads neither more nor less alarming than it is:** the attacker must POSSESS a legitimately-formed, legitimately-premitted formation whose deterministic draw *coincides* with a live ACTIVE quorum's members — i.e. (i) a **coincident draw** (the selection function at the attacker's formation anchor selects the same 11 as the target) AND (ii) **≥t cooperating operators from that draw** to produce the premits. That is not nothing; and on an 11-of-22 fleet with a deterministic selection function it is **not negligible either** — a coincident draw is a real event, not a cryptographic impossibility. **The fix is correct regardless of likelihood — the point is what an attacker needs, not how often it occurs.** **Fix: bind `predecessor_quorum_hash` into the Phase 4 sign-hash under the version** (`GetSignHash` v2 = `SHA256(quorum_hash ‖ proTxHash ‖ group_pk_bytes ‖ vvec_hash ‖ predecessor_quorum_hash)`), so the members explicitly attest "this is a rotation of X." ★ **Consequence, stated plainly: the change is NOT payload-only — it reaches into the ceremony's signed data, so the formation driver must know at Phase 4 that it is a rotation and which predecessor.** This is the widest blast-radius element of KDD-072 and the reason it is a design gate, not a struct edit.

**4 — Field set.** `nVersion` + `predecessor_quorum_hash` (present under `if (nVersion >= 2)`). **`predecessor_quorum_hash != 0` is the rotation signal** — a zero `uint256` is never a valid formation anchor, so no separate is-rotation flag is needed (the tree's presence-gated-by-version idiom over an extra bool). Nothing else the validator cannot derive: the successor's members come from the predecessor record via the link; group_pk/vvec/premits are already carried. **Backward link only** — the successor's `predecessor_quorum_hash` serves **both** validation (V12) **and** the record-side revert: `UndoBlock` deserializes the disconnecting successor's payload, reads the predecessor, and flips it `SUPERSEDED→ACTIVE`. No forward pointer on the predecessor. But per §3 the field is a **signed** field, not a bare struct member.

**5 — V-sequence by substitution (materially simpler than gating).** V5, V10, and V6-V8 all consume the same fresh-draw `quorum11` (specialtx_validation.cpp: V5 builds it :736-739, V10 checks containment against it :751-761, V6-V8 verify premits against it :764). So the rotation path **substitutes the input** rather than special-casing three checks: run **V1, V2, V3, V11, V9 unchanged**; then **branch** — if `predecessor_quorum_hash != 0`, a new **V12** (predecessor record exists, `state == ACTIVE` **as-of-pindexPrev**, and the link is bound in the premits per §3) sets **`quorum11 := predecessor.members`** and **skips V4+V5** (no GM-list fetch, no fresh draw); else run V4+V5 to produce `quorum11`. Then **V10 and V6-V8 run verbatim** against the substituted `quorum11`. V10's containment (`member_node_ids ⊆ quorum11`, no dups) is exactly correct for a same-set re-DKG whose effective-QUAL is a ≥t subset of the predecessor's selected-11; V6-V8 then require the premits to be the predecessor's operators. **V5 is *replaced* for a rotation, not gated-and-skipped-around; V10/V6-V8 need no per-check gating.** This is why substitution beats "insert V12 + special-case three later checks." V12 inherits V5's **state-as-of-height obligation** (D-SG1a-2, specialtx_validation.cpp:727-730) for the predecessor lookup.

**6 — ★ Predecessor-uniqueness index (take it, not defence-in-depth).** Add a **V9-style "at most one successor per predecessor" guard, keyed on the predecessor** (mirroring V9's cross-block formation-uniqueness against the persisted index, :699-710). Reasoning recorded: the ACTIVE-state check in §5 is correct **only while** the state-as-of-pindexPrev obligation (D-SG1a-2) is honoured — and **an obligation is what a later refactor breaks silently.** V9 is the precedent for making uniqueness *explicit* rather than *inferred from state*; a predecessor-keyed uniqueness index makes "one rotation per predecessor" a structural invariant that survives a refactor of the state read.

**7 — Adversarial disposition.**
- **Same-draw re-cast (§3):** closed by binding the predecessor into the Phase 4 sign-hash — a formation's premits no longer verify as a rotation.
- **Marker-stripping a genuine rotation:** fails closed — stripping the (signed) predecessor invalidates the premits (V6-V8), and even absent the signature the validator treats it as a formation → V5 fresh draw → member mismatch → V10 rejects. Cannot be used to bypass the predecessor check.
- **Two rotations racing one predecessor:** first connects → predecessor SUPERSEDED; the second fails V12's ACTIVE-as-of-height check **and** the §6 uniqueness index. The index is the durable guarantee if the state read regresses.
- **DISBANDED / SUPERSEDED / nonexistent predecessor:** all fall out of one guard — `GetQuorumRecord(predecessor)` must succeed AND `state == ACTIVE`; DISBANDED(3) and SUPERSEDED(2) fail the ACTIVE test, nonexistent fails the lookup.
- **Chain A→B→C:** holds — only ACTIVE can be a predecessor, ACTIVE→SUPERSEDED is one-way per successor-connect; the share-side already proved the A→B→C stack (KDD-070 P4 multi-block test).

**8 — ★ Package the version bump AHEAD of rotation.** Split KDD-072 into **P-a — bare version introduction** (`nVersion` added to `PTXDKGPayload`, current 6-field layout becomes v1; validators gate `bad-ptxdkg-version`; no rotation semantics) landed FIRST, then **P-b — rotation linkage** (`predecessor_quorum_hash` under v2, the Phase 4 sign-hash extension §3, V12 + substitution §5, the uniqueness index §6, the record-side revert). Reasoning: the unversioned→versioned break is **free exactly once**, costs a fleet re-form, and ODC-038 already makes re-form the standing recovery posture. Landing v1→v2 as a bare bump first **separates the wire-format break from rotation semantics** — if rotation's design shifts during P-b, the wire is not re-broken. ★ **Forward-bind:** after P-a, every payload change is additive-under-version or takes an activation height; the free break is spent.

**9 — Activation: none needed, with the reason.** The version field is self-describing — v1 and v2 payloads coexist and validators branch on the carried version, so v2's rotation rule activates implicitly at the first v2 payload. **No `NetworkUpgradeActive` gate** (the mechanism exists, UpgradeIndex/params.h:26+, but is not needed here). This is **safe only because** the fleet is reset through the unversioned→versioned break, so no v1-*unversioned* payloads coexist with the new deserializer. ★ **V11 lesson recorded so a future reader does not generalise:** V11 needs a mainnet height-gate because it changed the meaning of a *height* (specialtx_validation.cpp:688-692); a *self-describing version* does not. But the free-un-gated break here is a one-time property of a resettable fleet with no surviving v1 payloads — once a durable chain holds PTXDKG records, this same transition would require an activation height.

**10 — Timing / the two on-chain v1 payloads.** The fleet holds two mined PTXDKG payloads on-chain — **fc8e0f0d (h1040)** and **57e7c7b4 (h960)** — in the current unversioned layout. Under the new deserializer (nVersion first) they **misparse on reindex** (the first bytes of `quorum_hash` read as the version). The cost is therefore **a fleet re-form**, not "nothing" — and it is **the cheapest this will ever be** (no public testnet, no durable chain, no activation height, no resync semantics; the re-form is already ODC-038's recovery path). This is the concrete argument for §8: pay the one free break now.

**Status:** Design recorded (recon gate; the format decision, not the build). NOT implemented — **P-a (version introduction) then P-b (predecessor + Phase 4 sign-hash + V12 + uniqueness index + record-side revert)** owed to W2.3. Cross-ref KDD-045/063 (rotation / handover-at-accept), KDD-070 (the five share call sites this unblocks; the record-side revert §4/§7), KDD-056 (nType 7/8 reserved-off), KDD-059/060 (premit V6-V8, share_index), KDD-052 (score-order membership), V11 (the un-gated-rule lesson §9), ODC-038 (fleet re-form is the standing recovery §10), ODC-032 (block-connect not simulable — P-b ships unit-verified only), ODC-040 (reorg-revert proof needs a breakable fleet), D-SG1a-2 (the state-as-of-height obligation §5/§6 inherit).

**KDD:** KDD-072

---

**ODC-042 (2026-07-25). ★ D-SG1a-2 becomes a LIVE consensus bug at first supersede — the as-of-height state read must land before or with V12. (P-b recon finding; consensus-correctness tier with V11/ODC-041.)**

**Substance.** P-b introduces the first state mutation ever applied to quorum records (predecessor ACTIVE→SUPERSEDED at successor-connect, KDD-063/072). `GetActiveQuorumsAtHeight` (ptx_quorum_store.cpp:199-237) filters records on **CURRENT** state (height-filtered only by `mined_height`); V5 builds its fresh-formation pool from `GetActiveQuorumsAtHeight(anchor height)` (specialtx_validation.cpp:741-747), and the anchor lags pindexPrev by up to N−1 blocks. A rotation connecting inside that window flips its predecessor SUPERSEDED → the validator's pool frees the predecessor's 11 members that the formation-time pool excluded (D-SG1a-1) → selection diverges → an honest formation self-rejects — and more generally **the same block validates differently depending on when it is evaluated relative to an unrelated rotation's connect. That is a chain split** — the SG-1a self-poisoning class (the store's own comment records the live-caught precedent, ptx_quorum_store.cpp:86-93).

**Boundaries, stated precisely.** Sequential connect and reindex are SAFE (mutations replay in connect order, so current state == as-of state during in-order replay); the anchor-lag window and reorg edges are NOT.

**Fix (packaged P-b4).** Stamp `superseded_height` in `CPTXQuorumRecord` (additive under the record's own version idiom, ptx_quorum_store.h:131-132 — record v2); as-of predicate = `state == ACTIVE || superseded_height > h`; the record-side revert clears state→ACTIVE + stamp→−1 (payload-derivable at UndoBlock, deterministic — V12 guarantees the predecessor was ACTIVE at connect; no undo journal). ★ **ORDERING CONSTRAINT — the point of the entry: this fix MUST land BEFORE OR WITH V12 (P-b3), never after** — V12 inherits the as-of obligation the moment it performs a predecessor lookup, and the first supersede on any chain opens the window. bf-fleet verifiable (rotation connect + `invalidateblock` reorg — the ODC-032 unit gap closed by the fleet for exactly this shape).

**Cross-ref:** KDD-072 §5/§6 (V12 inherits D-SG1a-2), KDD-063 (the swap transition), KDD-070 P4 (the slot-side revert this composes with), D-SG1a-2 (specialtx_validation.cpp:737-740 — the recorded obligation this entry escalates), ODC-032 (why the connect/disconnect arm is fleet-only), KDD-073 (the sites that consume this read).

**ODC:** ODC-042

> **[FLEET-VERIFIED 2026-07-25 — appended, no prior line edited]** ODC-042's fix is **verified on a real chain**, not only in unit fixtures. In the KDD-072 P-b drill (below), a fresh formation anchored at **h960** was built while ac5c28 was ACTIVE, then **rebuilt byte-identically after** ac5c28 was superseded at **h1010**, and **connected clean at h1015** — two disjoint ACTIVE quorums resulting. The as-of predicate answered `ACTIVE-as-of-960` for a record whose *current* state is SUPERSEDED, at both the builder and the validator, with the store connect guard agreeing (no connect-time reject). **Pre-P-b4 code self-rejects this**: a raw current-state read frees ac5c28's 11 members at 960, redraws a different selection, and V10 rejects the committed list. The chain-split fix is now verified where it lives. Status: **fix landed (P-b4, 75f3c74) and FLEET-VERIFIED (drill 2026-07-25)**.

---

**KDD-073 (2026-07-25). The three-reconstruction-sites constraint on V12 — substitution is not one edit. (P-b recon finding; binding constraint on P-b3's build.)**

**Constraint.** KDD-072 §5's V12 substitution (`quorum11 := predecessor.members`, skip V4+V5) must land ATOMICALLY at every selection-reconstruction site, not only the validator. The sites at HEAD: **(1)** the V5 validator (specialtx_validation.cpp:741-749); **(2)** the store connect guard (ptx_quorum_store.cpp:94-127 — the same BuildPool+Select re-run as a persist-boundary guard); **(3)** the debug builder (rpc/ptx.cpp:675) if rotations become debug-injectable. **Ground:** the store's comment (ptx_quorum_store.cpp:86-93) records the live-caught battery_sg1 precedent — one site pool-aware, another raw → a VALID payload passed populate+assembler and was REJECTED at connect ("11 committed, 4 matched"), a self-poisoning divergence. V12 re-creates exactly that topology if substitution lands validator-only: **a valid rotation passes CheckPTXDKGTx and dies in ProcessBlock.**

**Test obligation (binding on P-b3).** The falsification must exercise **CONNECT, not just validation** — a rotation accepted by CheckPTXDKGTx must also connect through ProcessBlock's guard; a validator-only test proves nothing about the constraint this entry records. Also binds the member-materialization helper (predecessor.members → `CDeterministicGMCPtr` at a pinned block, **record order preserved** for share_index continuity per SG-3/KDD-061): ONE shared function used by validator + store guard + driver — the KDD-060 one-function contract extended to rotation.

**Cross-ref:** KDD-072 §5, KDD-060 (one-function selection contract), SG-1a/D-SG1a-1 (the divergence class + recorded lesson), KDD-061 (share_index continuity), ODC-042 (the as-of-height read these same sites consume).

**KDD:** KDD-073

---

**KDD-072 P-b recon notes (2026-07-25 — appended; deliberately NOT new entries).**

- ★ **The §3 hole test is P-b's load-bearing security test**, RED-proven by inversion: strip `predecessor_quorum_hash` from the Phase 4 sign-hash → the §3 re-cast attack SUCCEEDS (a formation's premits validate as a rotation) → restore the binding → it FAILS (`ptxdkg-bad-premit-sig`). A test showing only the happy rotation path proves nothing about the hole.
- **Open check (KDD-072 item-4 adjunct):** the KDD-058-A minable-commitments store may need predecessor-aware dedup — two rotations naming one predecessor can both enter the replicated store; the loser is consensus-rejected at connect (V12-ACTIVE + the §6 index) so this is NOT a consensus risk, but the losing commitment may sit re-offered in every assembler until eviction. Efficiency/hygiene check owed to the P-b plan.

---

**KDD-072 P-b DRILL RESULT (2026-07-25). First fleet exercise of the rotation consensus arc — all five predictions confirmed, zero surprises.**

*Substrate:* bf fleet on the **`bbe5e0e` debug binary**, restored from reset point `bf-N22-conv1-24f9de3-q8of11-ac5c28`. A v2 rotation of **ac5c28** (anchor h800) was injected through the P-b3b debug arm — which resolves members via the **same** `PTX_DKG_ResolveRotationQuorum` V12 runs — mined at **h1010**, reverted, replayed, then the ODC-042 scenario, then a clean restore. Every observable below is from a real chain.

**Why this drill matters:** P-b1/P-b2/P-b4/P-b3a each shipped **dormant**, deferring their block-driven verification to exactly this point (ODC-032: connect/disconnect triggers are not unit-simulable). This is the payment.

**P1 CONFIRMED — V12 accepts + MarkSuperseded.** `populated: true` (validate-before-inject ⇒ `CheckPTXDKGTx` accepted a v2 rotation), committed members = ac5c28's exact 11 in share_index order. At connect: `MarkSuperseded: quorum SUPERSEDED at height 1010`; ac5c28 → `state: superseded`, record upgraded to **v2** on rewrite; successor `cecea35e…` ACTIVE. **The store connect guard agreed with V5/V12** — no connect-time reject, i.e. the KDD-073 shared-code property held on-chain and the battery_sg1 window never opened.

**P2 CONFIRMED — Promote is a key-isolation NO-OP.** Zero promote log lines; `PTX_BLS_Promote` returns 0 silently when no PENDING share exists. Correct: a PENDING share is created only by a real ceremony FINALIZE, and **KDD-070 §1 forbids any RPC path to seed one**.

**P4 CONFIRMED — the reorg-revert path, the arc's weakest-covered piece, fired and cleaned.** `invalidateblock` → `RestoreActiveOnUndo: quorum supersede REVERTED (SUPERSEDED->ACTIVE)` + `UndoBlock: erased PTXDKG quorum record on disconnect … height=1010`; ac5c28 ACTIVE, `superseded_height` cleared, successor gone.

**P5 CONFIRMED — replay determinism on a real reorg.** `reconsiderblock` re-connected and re-flipped to the **identical height 1010** (20:40:00 original, 20:43:22 replay).

**★ P3 — RECORD-VS-SLOT DIVERGENCE: A DRILL ARTIFACT, NOT A SYSTEM PROPERTY.** After the injected rotation connected, the chain showed: the **ACTIVE successor holding no signing shares at all** (`gm04 → "holds no CURRENT sk_share for quorum cecea35e…"`) while the **SUPERSEDED predecessor still signs** (gm04/gm18 return valid partials for ac5c28). ★ **This is CORRECT for debug injection and a future reader finding this state cold must not read it as a bug:** the successor ran **no ceremony**, so no member ever derived its shares; and the predecessor was never promoted because there was no PENDING share to promote (P2). **A REAL rotation (P-b6) does not produce this** — the ceremony derives the successor's shares (stored PENDING at FINALIZE) and `PTX_BLS_Promote` flips PENDING→CURRENT while retiring the predecessor's to SUPERSEDED_RETAINED. The divergence is an artifact of injecting a rotation without a ceremony, nothing more.

**★ ODC-042 VERIFIED ON A REAL CHAIN (the step that pays for the fix).** Fresh formation anchored **h960**: members drawn were **disjoint** from ac5c28's 11 (KDD-040 pool exclusion working — 22 GMs = 11 + 11); **rebuilt identically after** the h1010 supersede; **connected at h1015**. Final state: two disjoint ACTIVE quorums (11/11 each) plus ac5c28 superseded. See the ODC-042 fleet-verified note above.

**Design note for P-b6 (not actionable now):** the drill supplied all 11 operator SKs, so the successor committed **11/11**, "healing" ac5c28's 3 non-qual members (gm14/gm17/gm11). A real ceremony re-derives QUAL organically and **may commit fewer than the predecessor's full set** — **P-b6 must treat a successor forming with less than the full predecessor set as NORMAL, not failure** (the >= t rule governs, exactly as at fresh formation).

> **[CORRECTION, 2026-07-26 — appended, the note above stands: the record shows the reasoning moved.]** The framing "the drill supplied all 11 operator SKs, so… healing" is **wrong** — the healing is **organic, not an injection artifact**. KDD-045 (precision note): rotation is a **FULL re-DKG producing a FRESH KEYPAIR; the old keypair is ABANDONED — NOT proactive resharing of the same key.** The source consequence: V12 substitutes `quorum11 := predecessor.members` — the predecessor's **full recorded 11, including its own non-qual members** — and a re-DKG derives every share from scratch, **independent of the predecessor's shares**. So a member who held **no** predecessor share (gm11/gm14/gm17) can participate fully and land `in_qual` in the successor: a real autonomous rotation (P-b6b) produces the same "healing" the drill showed. **What was right in the note stays right:** `<full-predecessor-set is NORMAL` and >= t governs — the drill supplying all 11 SKs only removed the ceremony's *natural* QUAL attrition (hence exactly 11/11); it did not *cause* the non-qual members' return. **What was wrong:** attributing the return to the injection rather than to re-DKG semantics. Caught by the **P-b6 recon (item 2)** reading KDD-045's re-DKG semantics against this note — a source-over-narrative correction, the same class as P-b6b's residue-guard correction.

**★ COVERAGE BOUNDARY, honestly stated.** The drill proves the block-wiring **FIRES, REVERTS, and REPLAYS** on real connect/disconnect. It does **NOT** prove: the real rotation **trigger** (nothing feeds `session.predecessor_quorum_hash`), the ceremony-driven **PENDING→CURRENT promotion**, or **drift/deadline** behaviour. All three are **P-b6**.

**Reset-point hygiene:** restore returned the fleet to **h936, ac5c28 ACTIVE 11/8, shares reloaded byte-identically** (`a7b5309c…` = the snapshot baseline); the bank tar's md5 was unchanged throughout. The drill **fully unwinds and is repeatable**; the reset point is uncontaminated. Operational findings from the drill are consolidated in `BF_FLEET_NOTES.md` (restore-owed set), and the RPC observability gap is registered as ODC-043.

**Cross-ref:** KDD-072 §3/§5 (the hole and the substitution now exercised), ODC-042 (fleet-verified), KDD-073 (three-sites property held on-chain), KDD-070 §1/§8 (Promote no-op; the real promotion owed to P-b6), ODC-032 (the deferral this drill discharges for connect/disconnect), KDD-063 (the swap performed end-to-end), ODC-043 (observability gap surfaced here).

> **[P-b5 SIBLING NOTE, 2026-07-25 — rides the P-b5 commit; owed, no number (the ODC-043-sibling pattern).]** The KDD-058-A minable-commitments store needs **predecessor-aware erase**: `EraseMined` erases by the mined tx's own quorum_hash, so when rotation #1 mines, a competing rotation #2 of the **same predecessor** (different anchor → different quorum_hash) stays in the store — and `GetMinableTx`'s keep-but-skip policy re-runs a **full `CheckPTXDKGTx` (incl. all premit sig verifies) on it at every block template, forever** (removal paths are erase-on-mined for its own quorum and the debug clear only). Not consensus (it can never mine — V12d/as-of reject it at generate-time revalidation); a per-template CPU + log-noise cost, unbounded in time. Fix is one loop in `EraseMined`: when a mined PTXDKG carries a predecessor, also erase held entries naming the **same predecessor**. Node-local mining policy, deliberately kept OUT of P-b5's consensus commit.

---

**ODC-043 (2026-07-25). `ptx_quorum_info` does not surface the record-v2 lifecycle stamps — an observability gap, not a defect.**

`superseded_height` and `disbanded_height` exist in `CPTXQuorumRecord` v2 (KDD-072 P-b4) and `MarkSuperseded` logs the height at connect, but **`ptx_quorum_info` displays neither**. The P-b drill (2026-07-25) had to verify the supersede height from a `debug.log` line rather than the RPC. ★ **Why it matters before P-b6:** rotations under the real trigger run **unattended** — log-only visibility means grepping 22 nodes' logs to answer *"when was this quorum superseded?"*, the first question a rotation drill or a post-incident review asks. **Fix:** expose both fields in `ptx_quorum_info` — display-only; the values are already persisted, consensus-correct, and reindex-deterministic. **NOT a defect:** nothing behaves wrongly, the data is simply unreachable through the interface a verifier uses. Small; worth landing before P-b6 so its rotations are observable through the RPC surface rather than the logs.

**Cross-ref:** KDD-072 P-b4 (the fields), the P-b drill result (where the gap surfaced), P-b6 (the consumer), KDD-062 (the record store).

**ODC:** ODC-043

> **[SIBLING NOTE, 2026-07-25 — appended with the ODC-043 code commit; same gap, one method over, no new number.]** `ptx_quorum_list` builds its **own** summary object and does not share `QuorumRecordToJson`, so it does not gain the stamps from the ODC-043 fix. Why it matters: the as-of predicate legitimately returns a **SUPERSEDED** record from `GetActiveQuorumsAtHeight(h)` when `h < superseded_height` — so the "active at h" list can correctly contain a `superseded`-state record, and **without the stamp shown, a reader sees a superseded quorum in an "active" list and misreads it as a bug** (it is the ODC-042 as-of semantics working). Owed: surface `superseded_height` in the list summary too (or route the list through the shared builder). Small, same observability class as ODC-043, useful before P-b6's unattended rotations.

---

**KDD-072 ARC-CLOSING DRILL (2026-07-26). Autonomous rotation verified end-to-end on bf — the last deferred verification class in the arc, closed. KDD-072 COMPLETE.**

*Substrate:* bf fleet, `bfe6030` (P-b6b) debug binary, restored from `bf-N22-conv1`. No debug injection — the rotation fired **autonomously**.

**Verified end-to-end.** The trigger fired itself (twice — h960, h1040); the h1040 ceremony converged over the wire to **`PREMIT→FINALIZE qual=11`**; members stored **PENDING** at FINALIZE (`StoreSkShare`); the successor `95350179…` connected at h1067 → **MarkSuperseded**(ac5c28) + **Promote** (PENDING→CURRENT); the successor is **ACTIVE and SIGNING** (gm04/gm18/gm11 return valid partials) while the predecessor is **retired and no longer signable** — KDD-070 §5's two-live-keys bound enforced live. The P3 record-vs-slot divergence the P-b debug drill documented as an injection artifact is **resolved**: a real rotation yields a working, signing successor with the old key gone.

**★ The h960/h1040 discriminator (why confirming the mesh first mattered).** h960 aborted at **HASH_COMMIT (phase 0)** on an unsettled mesh (~24 blocks post-restore) — upstream of *all* rotation-specific code (predecessor sign-hash at phase 4, PENDING role at phase 5), the known SG-2b post-restore convergence tail. h1040 converged on a **confirmed-settled mesh** (21 established member sessions, held throughout). The difference proves the h960 abort was **environmental, not a rotation-convergence defect** — and demonstrates **autonomous rotation-ceremony convergence, which the fresh-formation stand-up did NOT prove** (rotation did not exist until bfe6030). Confirming the mesh settled *before* h1040 is what made the two causes distinguishable; a second abort on a settled mesh would have been the serious signal, and it did not occur.

**Healing organic and complete.** The successor committed **11/11**, including ac5c28's original 3 non-qual members (gm14/gm17/gm11). A full re-DKG derives fresh shares independent of the predecessor's, so a member holding no predecessor share participates fully and lands in_qual — the corrected "healing" note vindicated on a real chain.

**ODC-043 confirmed live:** `ptx_quorum_info` surfaced `superseded_height 1067` via RPC (no log grep); `last_rotation_height 1067` stamped on the successor. **P-b5:** a second rotation of ac5c28 was rejected (`ptxdkg-rotation-predecessor-not-active` — V12b firing first in the post-mine sequence, since ac5c28 is superseded by then; pq_p IS set but masked by the earlier guard, and its first-reject role is the racing case, unit-proven `Pb5_V12d`). Both guards correct: as-of primary in the natural post-mine sequence, pq_p the durable backstop.

**★ THIS CLOSES THE LAST DEFERRED VERIFICATION CLASS IN KDD-072. Every piece of the rotation arc is now fleet-proven; rotation is autonomous.**

**Coverage recorded honestly:** DiscardSuperseded-at-depth (`maxreorg+MARGIN=120`, ~h1187) pending — unit-proven and structurally on the every-block path (the height-only sweep signature); **no residues and no stranded PENDING this run** (all 11 completed, so residue-retire and ExpirePending had nothing to sweep — the residue case needs a member who *missed* the rotation); the reorg re-allow not re-run (fleet-proven at the P-b debug drill 2026-07-25). Restore clean and lossless (h936, ac5c28 ACTIVE 8/11, gm04 share `a7b5309c…` reloaded byte-identical); reset point uncontaminated; bank tar md5 `58b3e914` unchanged throughout. Evidence: `autonomous_rotation_evidence.txt`.

**Cross-ref:** P-b6a/P-b6b (the trigger + sweeps this exercised), the P-b debug drill 2026-07-25 (the injection predecessor this completes), ODC-042 (fleet-verified there), ODC-043 (confirmed live here), P-b5 (uniqueness), KDD-070 §5/§8 (promotion + two-live-keys bound), KDD-045 (re-DKG healing), ODC-038 (live fleet untouched throughout).

---

**ODC-044 (2026-07-26). `MarkDisbanded` repeats the ODC-042 bug — a state mutation without an as-of stamp. Known defect, W2.4's first fix.**

`CPTXQuorumStore::MarkDisbanded` (ptx_quorum_store.cpp:274-289) sets `rec.state = DISBANDED` and zeroes `consecutive_inquorate_blocks`, but **never writes `rec.disbanded_height`** — it logs the height only. So the as-of predicate's DISBANDED arm (`state == DISBANDED && disbanded_height > h`, built vacuous in P-b6b) reads the **−1 sentinel → the record answers inactive at every height**. This is the exact ODC-042 hazard P-b4 fixed for supersede: **a consensus state mutation without a pindex-derived as-of stamp splits the chain** (the as-of read diverges from the record's actual transition height across the anchor-lag/reorg windows). Currently latent — `MarkDisbanded` has zero production callers — so it cannot fire until W2.4 wires a producer; but wiring the producer *without* the stamp fix would ship the split.

**Fix (W2.4's first job, the MarkSuperseded/P-b4 pattern):** (a) stamp `rec.disbanded_height = disband_height` (pindex-derived, reindex-deterministic, block-atomic — exactly as `MarkSuperseded` stamps `superseded_height`); (b) add the **disband undo twin** — a `DISBANDED→ACTIVE` revert on disconnect; (c) wire the `consecutive_inquorate_blocks` counter producer; (d) disband share-cleanup — the P-b6b residue-retire keys on `state==SUPERSEDED`, so a disbanded quorum's role-CURRENT shares (keypair abandoned per §7.3) are not swept; widen the sweep to DISBANDED or add a pass.

★ **Size honesty — the disband path is slightly MORE than the supersede path, not an exact copy.** For supersede, P-b4's `RestoreActiveOnUndo` **already existed**, so the ODC-042 fix there was *only* the stamp. Disband has **no** pre-built undo — its `DISBANDED→ACTIVE` twin is built **from scratch** — so the disband path is stamp **+** producer **+** a new undo **+** the share-sweep widening: **three-to-four pieces where supersede needed one.** "P-b4 pattern" applies to the *shape* (pindex-derived stamp, undo-on-disconnect, reindex-determinism), not the full *extent*. Flagged so the W2.4 builder isn't surprised the "small" package has more than one moving part.

**NOT a design question** — the field, the as-of arm, and the fix pattern all exist; this is the stub not following the pattern P-b4 established. Caught by the W2.4 recon reading `MarkDisbanded` against the ODC-042 fix.

**Cross-ref:** ODC-042 (the pattern this repeats), KDD-072 P-b4 (`MarkSuperseded`/`RestoreActiveOnUndo` — the template), P-b6b (the vacuous DISBANDED arm + the residue sweep that misses DISBANDED), KDD-047/§7.3 (the disband trigger that will feed it), the transition table T-H (:1839).

**ODC:** ODC-044

---

**W2.4 SCOPE NOTE (2026-07-26 — recon finding; corrects the stale roadmap framing, no new number).** `PTX_ROADMAP.md` frames W2.3 rotation as "the largest remaining item, gated by W2.4" — **stale: rotation is complete and fleet-proven** (KDD-072, autonomous rotation verified b885c33). The roadmap's "W2.4 owes the record-side undo revert (predecessor SUPERSEDED→ACTIVE)" is likewise **discharged** — that is `RestoreActiveOnUndo`, landed in P-b4. **W2.4 is now the keystone remaining consensus item, and it is SMALLER than the roadmap implies:**
- **Disband wiring: small** — the P-b4 forward-hook pattern (with ODC-044's size caveat: stamp + undo-from-scratch + counter producer + sweep-widening). The `disbanded_height` field and the as-of DISBANDED arm are already built.
- **Top-up is NOT a build.** §7.3 explicitly rejects re-assembling a disbanded quorum's survivors (it reintroduces the partial-reformation attack surface batch-only formation eliminates); disband **dissolves members to the general pool → fresh batch-of-11 formation** (the fresh-draw path the rotation arc left working). `InitSession` is set-agnostic so a changed-set ceremony is *technically* runnable, but the design doesn't call for one. The KDD-045 §7.2 "under-strength top-up vs run-to-rotation" question is **policy, not ceremony code** (run-under-strength composes with the arc's proven rotation; disband-early dissolves to pool).
- **★ THE ONE REAL DESIGN QUESTION — the tier-setter:** the disband *state transition* is consensus (`MarkDisbanded` writes persisted record state; the as-of arm exists), but its *trigger* — "inquorate (≤5 available) for n_disband=30 consecutive blocks" — is a signing-availability property the chain cannot directly observe. **Is per-quorum signing failure recorded on-chain so `consecutive_inquorate_blocks` increments deterministically (→ disband is validated consensus, byte-identical across nodes, ODC-042-class as-of rigour required), or is inquorate observed node-locally (→ nodes could diverge on consensus record state — a split)?** No inquorate-counter producer exists yet; this is genuinely undecided and sets whether W2.4 is a consensus or policy package. `n_disband=30` itself is explicitly tunable policy (§7.3), independent of this.
- **The three open decisions for the build session** (like P-b6b's three, inputs not resolved): **(1)** inquorate detection — deterministic on-chain signal vs node-local (the tier-setter above); **(2)** KDD-045 §7.2 top-up policy — run-under-strength-to-rotation vs disband-early; **(3)** rotation×disband precedence — the SUPERSEDED-vs-DISBANDED single-enum state collision (a quorum can't be both), and whether a doomed mid-rotation ceremony blocks or runs independent of disband. The rotation arc is fleet-proven and must not break.

**Cross-ref:** §7.3/KDD-047 (disband design), KDD-045 §7.2 (top-up open question), ODC-044 (the MarkDisbanded fix), ODC-034 (pool saturation — disband is what frees the pool), KDD-072 (rotation, done), the transition table T-H.

---

**KDD-074 (2026-07-26). W2.4 RETIREMENT trigger — retire-on-absence. A SECOND lifecycle event, ALONGSIDE §7.3/KDD-047 disband, which remains DETERMINED and OWED.**

★ **Read this first: this entry does NOT decide, replace, amend, or reduce the need for disband.** PTX has **two distinct lifecycle events with two distinct triggers**:

| Event | Trigger | Catches | Status |
|---|---|---|---|
| **DISBAND** (§7.3/KDD-047) | inquorate — ≤5 available for n_disband=30 blocks | a **failing** quorum (incl. dead-but-busy: routed rolls, failing them) | **DECIDED, DEFERRED — unbuilt** |
| **RETIREMENT** (this entry) | no PTX tx with `quorum_hash == X` for N blocks | an **idle** quorum (liveness-churn) | **DECIDED — W2.4 builds this** |

**W2.4 ships RETIREMENT. W2.4 does NOT ship §7.3 disband.** A malfunctioning-but-demanded quorum is exactly what disband exists to catch, and retire-on-absence catches it only on the wrong (idle) timer — later, and for the wrong reason. Anyone reading this entry as "the disband problem is solved" has misread it.

**Decision (retirement).** A quorum is retired when **no PTX transaction carrying `quorum_hash == X` has confirmed for N blocks** — deterministic, chain-computed absence. Retirement writes a consensus transition to a new **`RETIRED` state (enum value 4)**, distinct from `DISBANDED`, with the ODC-044 treatment (pindex-derived as-of stamp + disconnect undo twin), **rate-limited to one quorum per window, least-recently-active first**. Retired members dissolve to the pool and reform via the existing fresh batch-of-11 path (§7.1/§7.3).

**1 — Why retirement can be built now, and disband cannot.** Absence is an **observation**; failure is an **accusation**. A success artifact is self-proving (a threshold signature verifies); **there is no proof of a non-signature**, so "member X was asked and didn't answer" can only be *asserted* — and the only asserter is the coordinator, the node-local observer whose judgement must not reach consensus state. ★ **This codebase already made that mistake and fixed it:** the `ptx_roll` comment records caller-side PoSe updates causing *"a consensus split at every settlement boundary"*, fixed by making the tracker *"consensus-derived from the chain."* Absence needs no accuser: every node computes the same answer from chain data. **That is why retirement is buildable today and disband is not — not because disband matters less.**

**2 — Why RETIRED is distinct from DISBANDED.** Different causes, different consequences: an idle quorum's members are blameless and must face no §8 consequence; a failed quorum's may. Collapsing them destroys the "why did this quorum leave" answer (the observability principle of ODC-043). Enum values 0–3 are used, so **value 4 is free**.

**3 — ★ Why the rate limit is REQUIRED.** Absence is **globally correlated** — demand is shared, so a lull, caller outage, or router fault makes every quorum idle at once, all crossing N together: 35–40 concurrent 60+-block reform cycles, the *"empty-pool-reform-failure"* of §9.1, and a total service outage. Failure is uncorrelated per quorum; absence is not. §7.3 already rejected this class — *"n_disband=5 was explicitly rejected as an eager failure-detector vulnerable to converting transient network events into self-inflicted cascades"* — and a demand lull is exactly a self-healing blip. One-per-window, least-recently-active (the KDD-072 P-b6b tie-break shape, built and proven) caps the cascade at one reform. **Per-quorum retirement is cheap** (an in-flight round is coordinator-side in memory — nothing stranded; members keep tickets, stay re-selectable); **the cost is entirely aggregate, and the limiter contains it.**

**4 — ★ THE DISBAND DEFERRAL AND ITS REAL COST — NOT a free elective.** Failure-detection is a **determined, required** trigger (§7.3/KDD-047), deferred because it is hard, not because it is optional. Its cost is **undiminished by this decision**, and this decision **adds one**:
- **(a) Accountability machinery is exactly as large later as now.** Unprovable accusations need a complaint/justify-class mechanism (accusation + defence + penalty for false accusation). Nothing here shrinks it.
- **(b) The failure signal still does not exist.** `RecordWithhold`/`RecordAbstain`/`RecordInvalidCommit` have **zero production callers**; the `ptx_roll` comment marks proper withhold handling *"a future iteration item."* A new consensus tx surface to carry failed-round reports is still owed.
- **(c) The unverified-success-artifact gap it would build on is still owed** — consensus checks `quorum_sig_hash.IsNull()` only; `PTX_BLS_Verify` exists but is **never called in validation**, so the on-chain threshold signature is structurally checked, never cryptographically verified.
- **(d) ★ NEW COST CREATED BY THIS DECISION:** shipping retirement introduces a **trigger-precedence question that does not exist today** — a quorum both idle-for-N and failing-for-M: which fires, and does one pre-empt the other? Disband's future build must resolve it. **This decision makes disband slightly harder, not easier.**

**What IS free is narrow and stated exactly:** the RETIRED/DISBANDED enum split means disband's future **schema addition** costs nothing (no migration, no state-value conflict). That is the only "free later" claim this entry makes.

**Rationale.** Retirement is the smallest safe path **to liveness-churn** — deterministic and accountability-free. It is **not** a path to failure-detection and must not be scored as one. Build the mechanism whose trigger is provable; leave the mechanism whose trigger is an accusation until it can be made safe.

**Consequences / owed.** N and the rate-limit window are **tunable policy** (like `n_disband=30`); N must be generous — on a low-demand dev fleet (bf) every quorum is idle almost always. Requires the **`quorum_hash` attribution field** on the PTX payload (additive under the KDD-072 P-a idiom) — without it the signal is uncomputable; shared benefit with the success path and routing auditability. **Still owed after W2.4:** §7.3/KDD-047 disband-on-failure (with (a)–(d) above), and the unverified-`quorum_sig` gap.

**Cross-ref:** KDD-047/§7.3 (disband — **not amended by this entry**; determined, unbuilt, still required), ODC-044 (the stamp+undo pattern this transition needs), ODC-034 (pool saturation — retirement is what frees the pool), KDD-072 P-b6b (tie-break shape reused as the rate limit), KDD-072 P-a (additive-versioning idiom for the attribution field), §9.1 (reform cost, empty-pool risk), §7.1/§7.3 (dissolve-to-pool + fresh batch-of-11 — already built).

**KDD:** KDD-074

> **[NAMING FLAG, 2026-07-26 — appended; recorded, NOT applied. The rename is a Decision-3 item.]** ★ **The state named `RETIRED` above is probably misnamed, and the misnaming works against the entry's own distinction.**
>
> **The drift:** this mechanism was framed throughout its design discussion as *"**reform** on idle"*, and became *"**retire** on idle" / `RETIRED`* when written up here. The words are not equivalent. **"Retire" imports a member-consequence the idle case does not have.** In the idle case the **quorum instance ends and a fresh one reforms from the pool**; the **members are untouched** — blameless, tickets carried forward, immediately re-selectable (§7.3's dissolve-to-pool semantics). Nothing about the *members* is retired.
>
> ★ **Why this is not pedantry:** KDD-074's `RETIRED`-vs-`DISBANDED` split **exists precisely to encode the consequence difference** — idle members face no §8 consequence, failed members might (§2 of this entry). A state named `RETIRED` **undercuts the very distinction it was created to carry**: a reader encountering it infers the members were stood down, which is the `DISBANDED`-flavoured consequence the split explicitly denies. The name must carry *no-member-consequence* semantics, or the split communicates the opposite of its purpose.
>
> **Candidates (the exact word is a Decision-3 choice, not made here):** `REFORMED`, `DISSOLVED`, `RECYCLED`. Two inputs on the wording, recorded so the choice is informed:
> - **`REFORMED` has precedent tied to this exact mechanic** — §9.1 already speaks of the *"60+-block **reform** cycle"* and *"empty-pool-**reform**-failure risk"*, i.e. reform is the established word for what follows dissolution.
> - ★ **`DISSOLVED` collides.** §7.3 uses *"On **disband** — **dissolve** to general pool"* — dissolve is already **disband's** vocabulary, so reusing it for the idle state would blur exactly the two states this split separates.
>
> **It also sharpens ODC-045's disposition (b).** *"Force-**reform** a can-never-rotate quorum"* reads correctly and reveals why (b) works: a fresh draw produces a **fresh keypair**, which is precisely the key-refresh the can-never-rotate quorum is being denied — reformation *is* the remedy for its unbounded-compromise window. *"Force-**retire**"* implied mere removal and obscured that. The reframe strengthens (b) on the merits; it does not decide it.
>
> **HELD for Decision 3 — deliberately not applied now.** The rename touches the enum, and the enum is simultaneously in play for Decision 3's **three-way precedence** (`SUPERSEDED` / `DISBANDED` / this state) and for ODC-045's disposition. Changing the name here and the semantics there would split one decision across two commits. ★ **Recorded now because the rename is currently FREE** — doc plus unwritten code, no persisted records carry the value, so no migration — **and that window closes the moment Decision 3's build lands the enum.** The mechanic KDD-074 decided is unchanged and correct; only the label is in question.

---

**ODC-045 (2026-07-26). ★ THE CAN-NEVER-ROTATE HOLE — permanent member departure defeats rotation permanently. A SECURITY-PROPERTY GAP, disposition OPEN.**

**Substance.** P-b3a's `PTX_DKG_ResolveRotationQuorum` applies **reject-not-exclude**: if any predecessor member is absent from the DGM list at the rotation anchor — **deregistered, collateral spent, or operator key changed (ProUpReg)** — the **whole rotation is rejected**. P-b6a's driver propagates that refusal by simply not starting the ceremony (`if (!PTX_Formation_SelectRotationMembers(...)) return;`). Together they create a state **no existing trigger catches**:
- A quorum that **permanently** loses a member **can NEVER rotate.** Every boundary `RotationDueAt` reports due; every boundary the resolver rejects; the ceremony never starts — **indefinitely**, logging a failed rotation each time.
- It **keeps signing** (if >= t of its in_qual remain available), so it is **NOT idle** -> **KDD-074 retirement will not remove it**.
- It is **not below t**, so **§7.3/KDD-047 disband will not either** (even once built).
- ★ **It never refreshes its key -> an UNBOUNDED key-compromise window.** This defeats KDD-045's core security purpose: the rotation interval is supposed to bound how long a stolen share stays useful (*"shares stolen before a rotation are shares of a DEAD key — worthless"*). For a can-never-rotate quorum that bound becomes **infinite** — a stolen share stays live for the quorum's entire remaining existence.

★ **This is a SECURITY-PROPERTY GAP, not a liveness annoyance.** The unbounded-compromise-window is precisely the condition KDD-045 exists to prevent. The failed-rotation log spam is the visible symptom; the security regression is the finding.

**How it was found:** while surfacing W2.4 Decision 2's inputs (top-up policy). ★ **Decision 2's obvious question DISSOLVED under source examination:** an under-strength-but-producing quorum needs **no special handling** — it signs (threshold is `formed_size/2+1 = 6`, unchanged by attrition) and **heals at its next rotation**, because V12 substitutes the predecessor's **full recorded 11 including non-qual members** and a full re-DKG derives fresh shares. The arc-closing drill proved this end-to-end: an **8/11** quorum rotated to a successor committing **11/11**, with all three previously-non-qual members in_qual and signing. *(Caveat recorded: headroom shrinks as in_qual approaches t — at k=6 the quorum needs all six members to answer every roll.)* **The REAL Decision 2 is this can-never-rotate gap, which does not dissolve.**

**DISPOSITION: OPEN — four candidates, none chosen. Coupled to Decision 3 (rotation x retirement/disband precedence) and to be decided fresh.**
- **(a) Top-up / changed-set re-DKG** — replace the departed member from the pool. The literal "top-up"; largest build (a changed-set ceremony, which the design has so far avoided). ★ **Open sub-question, not settled here:** the recon flagged a conflict with §7.1 batch-only formation, **but replacing ONE deregistered member is not the partial-reformation attack §7.1 rejected** (that argument targets *re-assembling a failed quorum's survivors*). Whether §7.1's reasoning applies to single-member replacement is **undecided**.
- **(b) Force retire/disband a can-never-rotate quorum** — reuses machinery already built; *"rotation impossible"* is **chain-computable** (the resolver's inputs — the predecessor record and two DGM lists — are all on-chain, so every node computes the same answer). ★ **Open sub-questions:** is "can't rotate" a **clean terminal condition, or transient** (e.g. mid-ProUpReg, where the key may settle)? And does a **multi-quorum GM departure make it correlated** across quorums — in which case it needs KDD-074's rate-limit shape?
- **(c) Relax reject-not-exclude** — rotate with the survivors (changed set). Cheapest to state, but **breaks the same-set invariant V12 enforces** and would reopen the KDD-073 three-sites substitution atomicity — the opposite direction from the arc's design.
- **(d) Accept and do nothing** — zero build; the quorum runs unrotated until it goes idle (KDD-074) or falls below t (§7.3). The honest null option, but it leaves the security gap unaddressed.

**Cross-ref:** KDD-072 P-b3a (`ResolveRotationQuorum` reject-not-exclude — the cause), KDD-072 P-b6a (not-start propagation), KDD-045 (the security purpose defeated — the rotation-bounds-compromise argument), KDD-074 (retirement — does **not** catch this: the quorum isn't idle), §7.3/KDD-047 (disband — does **not** catch it: the quorum isn't below t), §7.1 (batch-only formation — the (a) tension, possibly inapplicable), KDD-073 (the same-set atomicity (c) would reopen), W2.4 Decision 3 (precedence — coupled).

**ODC:** ODC-045

---

**KDD-075 (2026-07-26). W2.4 Decision 3, Item 1 — THE PRECEDENCE PRINCIPLE: terminal-eligibility yields rotation at ceremony-start. ONE rule, not two arbitrations.**

**Decision.** A quorum **yields its rotation at CEREMONY-START if it is eligible for a TERMINAL transition** — retirement (idle >= N_retire) **or** disband (inquorate >= n_disband, future/owed). Rotation starts **only** for a quorum that is neither — healthy, demanded, quorate. Rotation is a *refresh*; you don't refresh a quorum that is leaving (idle -> retire) or that cannot complete the refresh anyway (inquorate -> the ceremony aborts sub-threshold at ClosePhase0's t-gate, ptx_dkg.cpp:501).

**Enforcement point: `RotationDueAt`, at ceremony-start** — where all three triggers evaluate the **same `CPTXQuorumRecord` simultaneously** (the due-loop already iterates full records carrying the idle counter; the yield is one comparison in that loop, no new plumbing). ★ **Why ceremony-start and not state-change time:** rotation's *decision* and its *state change* are ~107 blocks apart (ceremony start -> successor connect, measured on the arc drill h960->h1067), while retirement's are simultaneous. Any arbitration at state-change time therefore races across that gap (Hazard B below); at ceremony-start there is no gap to race across.

★ **THE KEYING — correctness-critical, verbatim into the build:** the yield keys on **ELIGIBILITY** (idle past N_retire / inquorate past n_disband), **NOT on "the terminal transition fired."** A retirement-eligible quorum yields its rotation **even if the KDD-074 rate-limiter defers its actual retirement this window** — it stays ACTIVE-but-not-rotating, queued, re-evaluated (and re-yielding) each boundary until its retirement turn. **Keying on "fired" reopens Hazard A through the limiter:** a rate-deferred quorum would read as not-retired -> rotate -> mint a successor with a zeroed counter -> never retire. Keying on eligibility closes it. **Residual, ACCEPTED:** bounded key-staleness while queued — a yielding quorum holds its old key a few windows longer, bounded by queue depth × window. ★ **Honest clause: not necessarily SMALL.** During a CORRELATED-idle event (the demand-lull case the KDD-074 rate-limiter exists for), many quorums become retirement-eligible at once and queue behind one-per-window — **the queue depth IS the correlated-eligible count, which is large exactly when it matters**; the last-in-queue quorum holds its old key for (queue-length × window). Bounded (contrast ODC-045's *unbounded* can-never-rotate) and self-resolving (the queue drains as demand returns or quorums retire), and acceptable — the stale keys belong to idle quorums nobody is asking to sign — but the record must not imply the staleness is always small.

**The two hazards this dissolves (both source-confirmed before locking):**
- **Hazard A — the counter-reset loop.** `RotationDueAt` keys on **age alone** (`nHeight - formation_height >= interval`, no demand term), so an idle quorum still rotates on schedule; a successor is a **new record** (`formation_height` = its own anchor, `consecutive_inquorate_blocks{0}`), so under rotation-wins a perpetually-idle quorum rotates every interval, **resets its idle counter each time, never retires — ODC-034 pool saturation never resolves.** Yield-at-start dissolves it: an eligible quorum never mints the counter-resetting successor.
- **Hazard B — V12b kills a healthy rotation.** If retirement fired inside the ~107-block in-flight window, the predecessor goes non-ACTIVE and the arriving successor PTXDKG fails V12b (`ptxdkg-rotation-predecessor-not-active`) — **a converged 11-member ceremony destroyed by a lifecycle transition.** Yield-at-start dissolves it: an eligible quorum never opens the window, so no in-flight rotation exists to kill.

★ **CORRECTION recorded (source over intent):** the initially-proposed *"rotation wins over disband"* was **WRONG.** ClosePhase0's t-gate (`phase0_commits.size() < t -> ABORTED`) means an inquorate quorum **cannot complete a rotation ceremony** — there is nothing for rotation-wins to protect, and blocking disband behind an abort-guaranteed ceremony would actively delay the correct outcome. The symmetric yield handles disband identically to retirement, and the source supports it.

**Disband note:** disband remains owed/future (KDD-074), but the precedence rule is defined NOW so it slots in without revisiting — a disband-eligible quorum yields rotation at start, same clause, same keying. This is the transition-rule half of what Decision 3 exists to settle; the enum-value half (the RETIRED->REFORMED rename flag) lands with Decision 3's remaining items.

**Mechanics note (why no explicit arbiter is needed):** `MarkSuperseded` already refuses unless ACTIVE (ptx_quorum_store.cpp:407); the retire-writer carries the same guard (KDD-074's ODC-044-pattern transition). With yield-at-start preventing the races that matter, **first-transition-wins via refuse-unless-ACTIVE is sufficient arbitration** — no new rule, no new state.

**One clarification bound into the record:** "yields" means *no rotation* — the boundary's else-arm still attempts a **fresh formation** at that anchor (`SelectAtAnchor`; deterministic skip if pool < 11). Harmless — a fresh formation binds no predecessor and cannot be V12b-killed — but the yield must not be misread as "no ceremony of any kind."

**Cross-ref:** KDD-074 (retirement + rate limit; the limiter interaction the keying closes; item 2 inherits this principle), ODC-045 (disposition — item 2), KDD-072 P-b6b (`RotationDueAt` — the enforcement site; the age-only due test), P-b4/ODC-042 (as-of predicate; the RETIRED arm this implies), V12b (the rejection Hazard B would have triggered), §7.3 (n_disband), ODC-034 (the saturation Hazard A would have left unsolved), KDD-074 naming flag (the enum half, still held).

**KDD:** KDD-075

---

## Appendix: Register cross-reference

*Program status / roadmap (to first testnet), including live fleet-state snapshots and pre-testnet blockers, lives in the tracked `doc/ptx/PTX_ROADMAP.md`. This register tracks decisions; the roadmap tracks status.*

*Register scope (added 2026-07-23): this cross-reference covers the DKG / W-series and begins at **KDD-033 / ODC-021**. Lower numbers (KDD-001–032, ODC-001–020) are Phase-1/2 decisions and are **not** recorded here — they live in the tracked `doc/ptx/ptxbea-*.md` docs and in in-source `KDD-0xx` comments. (The append-only working standup `PTX_LE_STANDUP.md` is **not** tracked in this repo and does not travel with a clone.) Rows below marked "back-filled 2026-07-23" record earlier decisions whose register rows were owed; no prior row was edited (append-only).*

| KDD | Title | §| Status |
|---|---|---|---|
| KDD-039 | DKG before public testnet | §2 | Decided |
| KDD-040 | Single-quorum-per-GM; multi-membership rejected | §4 | Decided |
| KDD-041 | Ticket-based lottery, 1:5 ratio | §6 | Decided |
| KDD-042 | GM reward model — masternode backbone + PTX bonus | §5.1 | Decided |
| KDD-043 | Roll fee — 1 HMS, spork-adjustable, atomic-with-result | §5.2 | Decided |
| KDD-044 | Quorum formation — batch-of-11 from pool | §7.1 | Decided |
| KDD-045 | Quorum rotation — same-set re-DKG, staggered | §7.2 | Decided — reinforced 2026-07-10 (second ground: unbounded-quorum scaling; supersede-flag resolved → kept); amended 2026-07-10: handover-at-accept adopted (old key serves until successor connects; rotation never unavailable); handover mechanics split to KDD-063 (2026-07-10) |
| KDD-046 | Ejection/PoSe discipline — 15-of-60, 120-ban | §8 | Decided |
| KDD-047 | Disband — inquorate → pool, tickets carry; n_disband=30; dissolve-to-pool | §7.3 | Decided |
| KDD-048 | Quorum params: t=6 decided; upgrade-gated consensus constant, not spork | §3, §9.1 | Decided |
| KDD-049 | PTX_BLS_Verify explicit group_pk — pure function, no global state read | — | Decided — impl plan 2026-06-03; commit 66251c8 |
| KDD-050 | Test extraction interface — in-daemon subset check; ENABLE_PTX_TEST_ACCESSORS compile gate, default off | — | Decided — impl plan 2026-06-03 |
| KDD-051 | DKG construction — Feldman VSS + GJKR commit-then-reveal hardening; QUAL-locks-before-reveal | §11 | Decided — measured 2026-06-03 |
| KDD-052 | PTXDKG member set — committed node_id list, chain-determined (score) order; resolves OPEN-2. Selection/ordering contract concretized by KDD-060 (§20); score-order principle unchanged. INDEX-SPACE SEAM CLOSED LIVE 2026-07-23 (SG-3 signing gate): a threshold sig over NON-CONTIGUOUS score-order x's {2,3,4,5,6,7,9,10,11} verified against the committed group_pk — the retired alphabetical dealer mapping could not produce it. ★ AMENDMENT (2026-07-24, KDD-069): the seam is now **STRUCTURALLY ELIMINATED, not merely reconciled**. The alphabetical basis was `g_ptx_bls_state.node_index`, written ONLY by the dealer's `PTX_BLS_Init`; with the dealer removed (commit 8a2200e) that global and its sole writer are gone, so **the alphabetical index space no longer exists in the codebase** — there is no second index space to reconcile against. This is a STRONGER claim than "closed-live": absence, not agreement. Depends on `PTX_BLS_Init` being gone from tests too (the subset test was rewritten off it, commit 3c616d9), else a residual alphabetical basis would falsify the elimination. | §12 | Decided 2026-06-03; closed-live 2026-07-23; structurally eliminated 2026-07-24 (KDD-069) |
| KDD-053 | Multi-quorum roll selection (Option D, ungrindable) + failover (verifiable re-route / re-roll asymmetry); partially resolves ODC-024 | §13 | Decided 2026-06-03 |
| KDD-054 | DKG ceremony crypto-stack boundary: arithmetic vs transport — blst-only arithmetic; chiabls permitted for auth/transport; IES outer-sig invariant | §14 | Decided 2026-06-04 |
| KDD-055 | DKG P2/P3 complaint–justify resolution — bad-member marking rules; false-accuser penalty; vvec-ground-truth invariant; PoSe bridge deferred | §15 | Decided 2026-06-04 |
| KDD-056 | PTXDKG nType=11 assignment; nTypes 7/8 deliberately left as gaps — abandoned PTXSETTLE/PTXCONSOLIDATE semantics must not be resurrected | §16 | Decided 2026-06-12 |
| KDD-057 | P5 sk_share_i write path Option A — shared g_ptx_my_bls_sk_bytes store; W1.3 replay guard must cover both write sites. ★ **AMENDMENT (2026-07-24, KDD-069/070 — append-only; the text above records the pre-069/070 model):** the store is NO LONGER the single global `g_ptx_my_bls_sk_bytes` — KDD-070 P1 replaced it with the keyed `g_ptx_my_shares` map. "Both write sites" is also superseded: post-069 there is ONE guarded write site (`PTX_DKG_StoreSkShare` → `PTX_BLS_SetSkShare`), and post-070 the map has **eight mutators** (one guarded, seven not — including the necessarily-unguarded undo revert). The invariant is now "no rpc-reachable write path," enforced by the P5 structural check (`P5_ShareStore_NoRpcReachableMutator`). See **KDD-070 §1**. | §17 | Decided 2026-06-12; amended 2026-07-24 (KDD-069/070 — symbol + write-path model now current) |
| KDD-058 | PTXDKG submission model — direct block-inject (LLMQCOMM precedent); resolves ODC-029; coupled to KDD-059 | §18 | Decided (impl pending W1.3) |
| KDD-059 | PTXDKG validation semantics — attestation-counting; accountability not correctness; share-correctness/recovery deferred to ODC-027/028 | §19 | Decided (impl pending W1.3) |
| KDD-060 | Canonical quorum selection: `GetListForBlock(B).CalculateQuorum(11, quorum_hash)` — one function, raw modifier, formation + validation; membership predicate fix; scorer retirement; V7a distinctness sharpening. Amends KDD-059. | §20 | Decided 2026-06-13 (impl pending W1.3) |
| KDD-061 | Lagrange index-space reconciliation — recovery-x = committed formation share_index (score-order, KDD-052/060); preserve-gaps under QUAL exclusion (<t=6 → abort/re-form); HARD W2 constraint: payload materializes per-member share_index (list order alone is lossy under exclusion); index mismatch must fail diagnostically, not via generic verify | §21 | Decided 2026-07-06 (RECORD at W1.3; fix bound to DKG-go-live) |
| KDD-062 | W2.1 quorum registry — versioned evodb record per accepted PTXDKG (connect-write/disconnect-explicit-erase, LLMQ pattern); event-based ACTIVE = the §C1 committed/active mark; connect-time share_index materialization (KDD-061 re-derivable arm); provenance reserved-UNSET (reindex-determinism); state-machine skeleton with producer-pending T-D/E/F/H bound to W2.2/W2.4 | §22 | Decided 2026-07-08 (W2.1 build) |
| KDD-063 | Handover-at-accept rotation — old keypair ACTIVE+servicing until successor PTXDKG connects; atomic swap at connect (new ACTIVE, old SUPERSEDED); rotation never unavailable; N = security-ceiling-only; W2.3 owed: SUPERSEDED repurpose, swap transition + undo (shared W2.4), rotation-validation arm. Cross-ref KDD-045. | §23 | Decided 2026-07-10 |
| ODC-021 | Coordinator SPOF | §2 | Resolved by DKG |
| ODC-024 | Multi-quorum membership — deferred future extension | §9.3 | Open (partially resolved per KDD-053: selection + failover decided) |
| ODC-025 | Rotation-N final value — pending ceremony duration | §9.2 | Open |
| ODC-026 | Complaint/justify timing window — P2/P3 phase duration; downstream of wall-clock/block-time audit | §9.5 | Open |
| ODC-027 | vvec_hash scope in PTXDKGPayload — vvec[0]-only for W1.2 (S4 approved); full-vvec scope deferred to W3.2 audit input | §9.6 | Open |
| ODC-028 | sk_share commitment in PTXDKGPhase4Msg — g^{sk_share_i} G1 proof-of-share; not required for W1.2; deferred to W3.2 audit input | §9.7 | Open |
| ODC-029 | PTXDKG submission model — direct-block-inject decided (KDD-058, 2026-06-12); block-inject wiring pending W1.3 | §9.8, §18 | Closed |
| ODC-030 | PTXDKG acceptance window + cross-block per-formation uniqueness. Clause 2 (uniqueness) CLOSED at W2.1: CPTXQuorumStore index + V9 `ptxdkg-duplicate-formation` / persist-boundary pair, falsified T5 + stub→RED. Clause 1 (staleness) split → ODC-033 | §9.9 | Closed (2026-07-08, W2.1 C4/C5; clause 1 continues as ODC-033) |
| ODC-031 | V1–V4 anchoring-chain coverage not unit-testable in test_ptx; exercised via Package 3 wiring on a real chain. COMPLETE (C6/C7): V1, V2, V3-PREDICATE, V4, F-5, populate-refusal. F-6 DISCHARGED at W2.1 (C0 connect-valid substrate; accept path end-to-end ×2). STILL OWED, mechanically unblocked, bound W2.2: V3-REORG-TRANSITION, C3-INVOCATION, F-9. C3-INVOCATION not unit-covered (FA-2b) | §9.10 | Partially closed (C6/C7 2026-07-04; F-6 discharged 2026-07-08) |
| ODC-032 | Can TestChainSetup run in test_ptx (the fixture block-mining hang; runtime face of umbrella rot); if resolved the anchoring checks could move to unit coverage, but the ODC-031 W2.2 remainder binding stands (ODC-031 partially closed) | §9.11 | Open (deferred) |
| ODC-033 | PTXDKG staleness/max-age bound — split from ODC-030 clause 1; no max-age on formation-anchor age (stale-anchor acceptance is the residual exposure; duplicate/conflict arms now covered by ODC-030 closure); constant + legality condition come from W2.2/W2.3 cadence design | §9.12 | Open (bound W2.2/W2.3) |
| KDD-066 | DKG signing-quorum selection (PROVISIONAL) — coordinator picks the ACTIVE quorum with the highest formation_height, ties broken by lowest quorum_hash lexicographically. Deterministic, but a de facto multi-quorum selection policy: it does NOT implement the registered N>1 design (deterministic shuffle over H(anchor_block_hash \|\| game_id \|\| roll_index), chain-determined and ungrindable). Coordinator recovery side only — NO consensus surface. Marked provisional in-source at the selection site (ptx_quorum_store.h / PTX_SelectDKGSigningCtx) ; first LIVE exercise 2026-07-23 (SG-3 signing gate) in the UNAMBIGUOUS case only (1040>960, no tiebreak); status UNCHANGED, still owed to W2.1 | §21 | Provisional (2026-07-23, SG-3 first sub-gate; owed to W2.1) |
| KDD-067 | sk_share clear/rotate path — PTX_BLS_SetSkShare is refuse-unless-empty (§C1 replay guard, correct as replay protection), and PTX_DKG_StoreSkShare routes through it, so ClosePhase5 turns a refused overwrite into phase=ABORTED. Consequence: **a member that completes one ceremony burns out for the rest of that process lifetime** — any later re-selection WITHOUT a restart aborts it at FINALIZE (the slot is in-memory and cleared on restart; see ODC-035). Unit-demonstrated (P5_ReSelection_SecondStoreRefusedAborts). NOT an SG-3 blocker (SG-3 signs with an existing quorum, no re-selection). A legitimate re-selection must clear the prior share first. ★ **SUPERSEDED by KDD-070 (2026-07-24):** KDD-067 recorded the WALL (refuse-unless-empty burns out a re-selecting member); **KDD-070 is the full slot mechanism that removes it** (keyed persisted multi-share slot, block-connect promotion, reorg-safe retention, startup reconciliation + wipe). See KDD-070 for the design. | §23 | Superseded by KDD-070 (was: open, owed W2.3/W2.4) |
| ODC-034 | Fleet quorum saturation — 22 GMs with two ACTIVE quorums exhausts the KDD-040 pool; no formation possible until a disband path exists. Blocks every downstream gate needing a fresh ceremony. Recurring (also SG-1a, quorum #2 h518); interim remedy bank-restore — CONFIRMED accurate: a restore does compose down/up, restarting every daemon and clearing every in-memory sk_share slot, so fresh ceremonies are possible afterward. The SAME restart-clears-slots mechanism is what makes ODC-035 a silent-membership-loss problem | §9.13 | Open (bound W2.4) |
| ODC-035 | DKG share material is PROCESS-LIFETIME ONLY — g_ptx_my_bls_sk_bytes is an in-memory global (ptx_bls.cpp:21) with no load/save path (grep-confirmed). A GM restart silently and irrecoverably drops that member from its quorum while the on-chain CPTXQuorumRecord still lists it in_qual: DURABLE membership, EPHEMERAL key material, NO reconciliation. Consequence: a quorum can fall below threshold with no on-chain or RPC indication. Measured 2026-07-23 pre-SG-3-signing: fc8e0f0d(h1040) 9/9 in_qual live, 57e7c7b4(h960) 6/6 live (both PASS-capable at t=6 — no loss yet, but nothing prevents it). Owed: share persistence, or an on-chain/RPC liveness signal, or both (scope, do not solve here). Cross-ref KDD-067 (its permanence is bounded by this), ODC-034 (bank-restore relies on the same restart-clears-slots mechanism) ; NOTE: SG-3 signing gate (2026-07-23) demonstrated a quorum signing CORRECTLY while carrying this risk — a GM restart would still silently drop a signer; risk unchanged. ★ **AMENDMENT (2026-07-24, KDD-070 — append-only; the text above records the pre-070 state and is now FALSE for the CODE):** the symbol `g_ptx_my_bls_sk_bytes` no longer exists (KDD-070 P1 → keyed `g_ptx_my_shares`), and a persist/load path DOES exist (`PTX_BLS_PersistShare` / `PTX_BLS_LoadShares`, evoDb RAW layer, KDD-070 P2). The **PERSISTENCE half** of the remedy is BUILT and unit-verified — but its production call site is UNWRITTEN (first consumer W2.3), so it is **NOT yet effective on the fleet**: the deployed 8719b7c GMs still carry the process-lifetime defect this row describes. The **LIVENESS-SIGNAL half** (member offline, share intact but process down) remains OPEN → **KDD-071**. ★ **CONFLICT RESOLVED:** this row and the KDD-070 row disagreed on whether persistence exists — KDD-070 is current for the CODE; this "no persistence" text is current only for the DEPLOYED fleet until KDD-070's call site is wired and the GMs are redeployed. ★ Cross-ref **ODC-040** (fleet homogeneity coverage limit — why the fleet has not surfaced this: no operator reboots to trigger the drop). | §22 | Open — persistence BUILT (KDD-070 P2, unwired → not on fleet); liveness OPEN → KDD-071 (amended 2026-07-24) |
| ODC-036 | DKG signing threshold was derived from COORDINATOR REGISTRY SIZE (g_ptx_nodes → n/2+1 = 12 at 22 nodes) instead of QUORUM SCOPE (formed_size 11 → t=6). Wrong on any fleet where registered-nodes != quorum-size; reached FOUR sites (PTX_LoadDKGSigningCtx usability, round.threshold→commit-reveal, the enough-partial-sigs check, and the Lagrange slice). Caught LIVE at the first fleet signing attempt (fail-closed hard-error on the valid 9-in_qual fc8e0f0d); invisible to 271/271 because the unit tests passed threshold in EXPLICITLY. FIXED (2026-07-23): PTX_SelectDKGSigningCtx derives t=majority(formed_size) internally (threshold param deleted, so registry-independence is structural) and returns ctx.threshold — the single source the roll's four sites read; St1-St4 pin the derivation, RED-by-inversion. ★ ADDENDUM (owed): deriving t from formed_size is correct ONLY while t == n/2+1. KDD-048 chose t=6 by WARGAME, not formula, and pre-documents a t=7-at-n=11 upgrade; on that upgrade the derivation silently returns 6 for a ceremony that baked 7, and KDD-048's upgrade semantics let t=6 and t=7 quorums coexist. The derivation is a STOPGAP with a known scheduled break. OWED: persist t in CPTXQuorumRecord at formation — CHEAP (record is nVersion-tagged with the additive `if (nVersion>=2)` pattern; v1 records fall back to the derivation, no reindex) | §21 | Fixed (derivation); persist-t OWED |
| ODC-037 | GM BANLIST RESIDUE survives daemon restart, chainstate wipe, AND bank-restore (banlist.dat is not touched by any of them). A node that earned bans in a prior episode CANNOT re-integrate after being wiped: it presents as persistent `socket send error Broken pipe` CONNECTION failures, NOT as a ban, because it is rejected before any RPC/log surface makes the cause visible. Reason category: MISBEHAVIOUR (`node misbehaving` = DoS-score-to-threshold, the BUG-020-lineage view-dependent stake-failure path). EVIDENCE (2026-07-23): the original caller 172.31.0.10 banned on 7/22 GMs (gm01/02/05/11/12/16/22 — exactly its addnode targets), reason `node misbehaving`, 24h expiry (created+86400); ban predates the node's post-wipe uptime, so retries do NOT re-earn (rejected at connection, cannot misbehave further) — persistence is banlist.dat survival, not a retry loop. This is WHY the caller-resync failed and caller2/caller3 on fresh IPs (.33/.34) were required. OWED: add banlist clearing (clearbanned / remove banlist.dat, or setban remove for the affected IPs) as an EXPLICIT step in the restore procedure (restore_fleet.sh), so a restored fleet does not silently exclude a wiped-and-rejoining node. ★ AMENDMENT (2026-07-23, append-only): CORRECTION — the ban carries a 24h TTL (banned_until 1784845560, ban_created 1784759160, 86400s span); exclusion is BOUNDED, not permanent — the prior wording ('cannot re-integrate', 'persistence') OVERSTATED it. The DURABLE finding is DIAGNOSTIC OPACITY: for up to 24h a wiped node is rejected BEFORE any RPC or log surface shows a ban, presenting only as broken-pipe connection failure — the cause is invisible from the excluded node. REMEDIES in order of touch: (a) wait out the TTL — zero-touch, no GM contact; (b) setban remove on the affected GMs — live RPC, no restart; (c) clearbanned / remove banlist.dat in restore_fleet.sh — the procedural fix for the restore path. SG-3 CONSEQUENCE (honest): fresh-IP caller2/caller3 were NOT strictly required — waiting ~24h would have restored .10; the fresh nodes were the right call under time pressure and gave a cleaner non-GM coordinator, but the ban was not the blocker it appeared to be. ★ Cross-ref **ODC-040** (fleet homogeneity coverage limit — single operator, no varied ban history, so the diagnostic-opacity window is unmeasured). | §22 | Open (owed — restore-procedure step) |
| ODC-038 | ★ FLEET BINARY DIVERGENCE — DO NOT DEPLOY TO GMs. As of 2026-07-23 the coordinators (172.31.0.33/.34) run hemis-ptx-w2:0101a44-dbg while ALL 22 GMs run hemis-ptx-w2:8719b7c-dbg. This is INTENTIONAL: the SG-3 signing repoint (0101a44) is COORDINATOR-SIDE ONLY — group_pk/share_index are read from committed data, the GMs' gm_bls_sign already signs the DKG-stored share, so GMs need nothing from it. ★ HAZARD: a GM restart CLEARS its in-memory sk_share (ODC-035 — process-lifetime only, no persistence). Both quorums (fc8e0f0d fh=1040, 57e7c7b4 fh=960) are ACTIVE, so KDD-040 blocks reformation and NO disband path exists (ODC-034) → a fleet-wide GM deploy/restart DESTROYS BOTH QUORUMS PERMANENTLY, and the only recovery is a bank-restore to a pre-h960 snapshot (losing all landed results after it). SAFE to deploy to GMs ONLY after W2.4 disband exists, or as a DELIBERATE restore where losing both quorums is the intent. A routine 'update the fleet to the latest image' is a footgun here. ★ **AMENDMENT (2026-07-24, KDD-072 P-a — the failure mode CHANGED and got worse):** once KDD-072 P-a is at HEAD, the HEAD binary is **WIRE-INCOMPATIBLE with the live chain** — the versioned `PTXDKGPayload` (nVersion first field) **misparses the two on-chain v1 payloads at h960 (57e7c7b4) and h1040 (fc8e0f0d)**: a fresh IBD rejects those blocks and cannot sync past them, and any disconnect reaching them hits `GetTxPayload` misparse in `CPTXQuorumStore::UndoBlock` ("evodb integrity failure"). Operational consequences: **(1)** "deploy HEAD to the fleet" now IMPLICITLY REQUIRES a **bank-restore to a pre-h960 snapshot FIRST** (`w2-fleet-N22-h824` or `-h231`, both < h960 → discard both quorums, pool freed without W2.4 — the ODC-034 interim remedy). **(2)** the deploy MUST BE **ATOMIC** — a staged/rolling deploy **forks the fleet on the first new formation**, because `8719b7c` nodes reject the new-layout payload while P-a nodes accept it. **(3)** correct order: **stop fleet → restore all to h824 → deploy P-a atomically → start → re-form**; any other order splits or wedges the fleet. Recorded here (not as a follow-up) because ODC-038 previously warned only that deploying LOSES THE QUORUMS; after P-a the failure is different and worse (wire-incompatible, cannot sync, forks on staged deploy), and the entry must change with the change that causes it. | §22 | Open (blocks GM deploy until W2.4; ★ after KDD-072 P-a, HEAD is wire-incompatible with the live chain — restore-first-then-atomic-deploy) |
| ODC-039 | ★ OPEN QUESTION (monetary policy, INHERITED — not a defect ruling): **coinstake-split burn escape.** The 10% subsidy burn fires ONLY when the coinstake has exactly 2 staker outputs (`SubtractGmPaymentFromCoinstake`, gamemaster-payments.cpp:362-364); the `stakerOuts > 2` branch (:365-375) subtracts only the GM payment and **skips the burn**. Validation does NOT enforce the burn — `nExpectedMint = GetBlockValue()` carries no burn term (validation.cpp:1670) and `IsBlockValueValid` is a ceiling, `nMinted <= nExpectedValue` (gamemaster-payments.cpp:226) — so a split coinstake that skips the burn stays under the full-value ceiling and **every validator accepts it deterministically** (no fork). CONSEQUENCE: a staker who sets `stakeSplitThreshold` below their UTXO size produces split coinstakes and pays NO burn — **the burn is voluntary in practice.** ★ THE QUESTION (undecided here): is escaping the burn by splitting INTENDED (a deliberate incentive to split UTXOs) or a HOLE in the monetary policy? **Not decided — recorded for a policy owner.** ★ INHERITED: both the burn (`nSubsidy *= 0.10`) and the split logic predate PTX — traced to the initial import `1f34501` and the masternode→gamemaster rename `b9ca558`, NOT any PTX commit; so the question belongs to **Hemis-core monetary policy, not PTX** — PTX only SURFACED it. Cross-ref **BUG-015** (the diagnostic that surfaced it; it went unobserved because the fleet's uniform `stakeSplitThreshold` never produced a split coinstake) and **ODC-040** (the homogeneity reason it went unobserved). | §— (row-only) | OPEN QUESTION (inherited policy; owner = Hemis-core, not PTX); recorded 2026-07-24 |
| ODC-040 | ★ **FLEET HOMOGENEITY COVERAGE LIMIT** — a named structural limit of what the W2 fleet can prove. The fleet is **22 identical nodes, one /16 subnet, one operator, one config**; a set of pre-testnet paths CANNOT be exercised because the fleet has no heterogeneity to exercise them with: **BUG-015** (uniform `stakeSplitThreshold` → no split coinstakes → burn-escape unobserved), the **/16 netgroup artefact** (single subnet → the no-self-heal is a test artefact with no production analog; KDD-065), **ODC-035** (no operator reboots → the silent share-drop never triggers), **ODC-037** (single operator, no varied ban history → the banlist-residue diagnostic-opacity is unmeasured), and **V11** (no chain with real history → the un-gated formation-boundary's split-on-resync cannot appear). ★ **PAYLOAD (plainly): a green run on this fleet is NOT evidence for any of these paths.** ★ GUARDRAIL 1 — this is why these paths are UNPROVEN, not why they EXIST: V11's cause is temporal (no chain history), ODC-035's is a code defect (no persistence); homogeneity is the COVERAGE gap, not the origin. ★ GUARDRAIL 2 — this is a CROSS-CUTTING POINTER, not a merge: each named blocker keeps its own entry and owed task; burying the action items is exactly the failure the BUG back-fill (2026-07-24) just corrected. | §— (row-only) | Coverage limit recorded 2026-07-24 (cross-cutting; each blocker retains its own row) |
| ODC-041 | ★ **OPEN INVESTIGATION** (pre-mainnet gate — NOT a defect, NOT resolved): **PTX × an ACTIVE payment/governance regime — an untested interaction.** Mainnet forks from a chain where GM (masternode) block-payments, the 10% coinstake burn, and the treasury/superblock/governance system are **ALL ACTIVE** (SPORK_7/8/9/13/21 on). The W2 fleet runs **every one of these OFF** at compiled defaults (spork census, 2026-07-24), so the interaction between an active payment/governance regime and PTX **has never executed anywhere.** Every "PTX is independent of GM payments" claim to date was observed **with the payment path switched off** — independence verified only in the *absence* of the thing it must be independent of. ★ **Enumerated untested couplings, each an open question (not answered here):** **(1) Coinstake contention** — the burn lives inside `SubtractGmPaymentFromCoinstake` (gamemaster-payments.cpp:358-375), which manipulates coinstake vouts; PTX rolls generate PTXSESS service fees that also flow through coinstake/fee accounting. Do they touch the same outputs, and does execution order matter? Never run together. **(2) Block validity under enforcement** — with SPORK_8 on, a block **MUST** carry a GM payee or it is rejected (gamemaster-payments.cpp:273/859). Does a block carrying a PTXDKG (nType 11) or a PTXSESS still satisfy that enforcement? The one-per-block PTXDKG rule and the mandatory-GM-payee rule have never been evaluated in the same block. **(3) Schedule collision** — superblocks (SPORK_13) alter reward structure on a cadence. Does a superblock height coinciding with a formation boundary (`PTX_Formation_IsBoundary`) or a rotation deadline (KDD-045/072) interact? Two independent schedules that have never coexisted. **(4) ★ Fork-point (sets the tier) — RESOLVED:** PTX activates via a **height-gate on the EXISTING Hemis mainnet** (`UPGRADE_V6_0.nActivationHeight` configured, chainparams.cpp:311; ptxbea/testnets are `ALWAYS_ACTIVE`-from-genesis per KDD-037) — a chain **with history** whose economic rules (payments/burn/treasury) were **live before PTX arrived**. This is the **V11 class** (PTX consensus rules meeting a chain whose economic rules predate them, with activation-height + resync semantics), **NOT the fresh-chain case** — that is the finding that sets severity. The activation model is **not consolidated in any one doc**, and the V11 boundary height-gate it requires is **unbuilt** (roadmap pre-testnet blocker) — both recorded as gaps. **Cross-ref** ODC-039 (the burn — dormant on the fleet, live on mainnet; the entry point to this surface), ODC-040 (fleet homogeneity — this is its **largest** instance: the entire inherited economic layer is a dimension the fleet cannot exercise), `ptxbea-known-limitations.md:202-207` (the "burned or treasury?" open question — becomes live the moment payments are on), V11 (same class per (4)). ★ **DISPOSITION:** a **pre-mainnet investigation gate**, answerable **only** in a payment-live/governance-active test environment which **does not exist** (the sacrificial/heterogeneous fleet, still unbuilt). **NOT a W2 blocker; does NOT gate KDD-072 P-b.** Registered now because the spork census surfaced it and it was written down nowhere — deferring the *answer* is correct; deferring the *record* is how it gets lost. | §— (row-only) | OPEN INVESTIGATION recorded 2026-07-24 (pre-mainnet; answerable only in a payment-live env — unbuilt) |
| KDD-072 | ★ PTXDKG payload linkage — versioned payload + SIGNED predecessor for rotation (first PTX consensus-tx-format change). Version `PTXDKGPayload` (the sole unversioned special-tx payload; conforms to the `uint16_t nVersion` idiom of ProReg/CFinalCommitment); `predecessor_quorum_hash != 0` = rotation signal, backward link only. ★ CENTREPIECE — the unsigned-predecessor hole: premits bind only the successor anchor (ptx_dkg.cpp:30-44 / :192), so a formation whose draw *coincides* with an ACTIVE quorum's members (precondition: coincident draw + ≥t cooperating operators) can be re-cast as its rotation with every check passing; fix = bind predecessor into the Phase 4 sign-hash → reaches ceremony signed data, **NOT payload-only**. V-sequence by substitution (V1,V2,V3,V11,V9 unchanged; V12 sets `quorum11 := predecessor.members`, skips V4+V5; V10/V6-V8 reused). Predecessor-keyed uniqueness index (explicit, not state-inferred — D-SG1a-2 is a refactor-breakable obligation). Package P-a bare version bump AHEAD of P-b rotation (the unversioned→versioned break is free once = a fleet re-form; two on-chain v1 payloads fc8e0f0d/57e7c7b4 misparse). No activation gate (self-describing version; safe only via reset — V11 lesson bounded). | §— (detail above) | Design recorded 2026-07-24 (recon gate; P-a/P-b owed W2.3) |
| KDD-068 | Discipline: a pure function tested with a HAND-PASSED parameter does not cover the call site that COMPUTES it. PTX_SelectDKGSigningCtx passed 271/271 with threshold=6 handed in, while the ptx_roll call site computed the wrong value (ODC-036) — untested because the derivation lived at the caller. Rule: extract the derivation into the pure function and pin it; a parameter the test controls is a parameter the test cannot falsify | §21 | Recorded 2026-07-23 |
| KDD-069 | ★ TRUSTED DEALER RETIRED — DKG signing is the ONLY path. Removed (coordinator-side): `PTX_BLS_Init` (central polynomial + all shares), its alphabetical Lagrange basis `g_ptx_bls_state.node_index` / `PTXBLSState` / `cs_ptx_bls`, the `gm_bls_keyset` coordinator→GM fan-out (`PTX_FanOutKeySet` + `PTX_BLS_GetShareBytes`), `PTX_AssignQuorum` + `PTX_NodeScore` + `PTXQuorumAssignment` (AssignQuorum-only), `PTX_BLS_Threshold`, and the dead `PTX_GetBLSState` (zero callers). KEPT: `PTX_BLS_PartialSign`/`Recover`/`Verify` (shared with the DKG path), `g_ptx_nodes` (fanout address book), the GM sk-share slot, `gm_bls_sign`. `ptx_roll` collapses to DKG-only: signs ONLY with an ACTIVE quorum's committed material (group_pk + score-order share_index from CPTXQuorumRecord), with **two hard-errors and no fallback** — "quorum present but unusable" and "no ACTIVE quorum for height" (both "dealer retired, KDD-069"). `signing_source` is now invariantly "dkg" (retained for response-schema stability). ★ **CAPABILITY LOSS (intended trade, recorded so it is not rediscovered):** a bare fleet with NO ACTIVE quorums can no longer be rolled at all — the dealer was what made a fresh, quorumless fleet rollable; a rollable fleet now requires forming a DKG quorum first. **NO CONSENSUS SURFACE:** the dealer was coordinator-side (dies in `rpc/ptx.cpp` + `ptx_bls`/`ptx_fanout`/`ptx_quorum` helpers); `CheckPTXDKGTx`, `CPTXQuorumStore`, and block validation never read it — validators are unaffected. **FLEET IMPACT:** the ptx-bea PoC runs the dealer on a DIFFERENT branch/images (unaffected); the W2 fleet already DKG-signs (SG-3), so no live W2 dependency. **.py surface:** the 3 ptx-bea scenarios + the 120-test dealer-era `ptx_test_suite_v4.py` were updated to assert the KDD-069 hard-error (v4 via a `DEALERLESS` probe that runs T01 live and auto-skips 119; their full disposition is OWED) — syntax-verified only, NOT runtime-validated (no fleet run authorised). Landed in 3 commits: **B** tests `3c616d9`, **A** removal `8a2200e`, **C** register (this). Cross-ref KDD-052 (seam structurally eliminated), KDD-057 (§C1 rationale amended: replay/double-store, not dealer hijack), KDD-070 (slot mechanism — the owed clear/authorized-overwrite path), KDD-071 (share liveness, if it survives recon). | §12/§17 | Decided & landed 2026-07-24 (commits 3c616d9 / 8a2200e) |
| KDD-070 | ★ GM sk-share SLOT MECHANISM — keyed, persisted, block-connect-promoted multi-share slot with reorg-safe retention. Replaces the single `g_ptx_my_bls_sk_bytes` global with `map<quorum_hash, HeldShare{bytes,formation_height,role,promotion_height}>` (roles CURRENT/PENDING/SUPERSEDED_RETAINED; per-quorum invariants, NOT a global ≤2 bound — that is KDD-040's, relaxed by W2.5). PENDING writable only from the ceremony FINALIZE call-path (structural post-069, no RPC edge); promotion on block-connect only. **Persistence lands WITH promotion** (evoDb, Dash `DB_QUORUM_SK_SHARE` precedent — cross-apply-checked SAFE: bank snapshots are node-preserving, surgery is wipe-resync, never donor-copy; subsumes ODC-035's persistence remedy, not its liveness remedy). Startup reconciliation discards orphan shares + an explicit wipe for restore_fleet.sh (ODC-034/037 restore now owes wipe + banlist-clear). `gm_bls_sign` takes quorum_hash; **ONLY CURRENT signs** (SUPERSEDED retained for undo, refused for signing — no two live keys post-rotation; reorg covered by the un-promote role flip). SUPERSEDED retained to maxreorg+MARGIN, MARGIN=20 (depth 120), depth-based. PENDING_TTL PROVISIONAL — bounded by FINALIZE→connect latency (few blocks under KDD-058-A any-staker inclusion), NOT rotation interval N; measurement OWED at W2.3 first rotation. Undo = the state-mutation revert SHARED with KDD-063 handover + W2.4 disband T-H (one impl, three consumers). W2.4 disband = degenerate retain-clear-then-store (reorg-retention arm still needed). Supersedes KDD-067. ★ **P1–P5 BUILT 2026-07-24** (51fd2dd / 068ec4a / e2bfaed / 9ecc76c / P5), ≈36 tests, 312 test_ptx green; SLOT-side undo only (record-side revert owed to KDD-063/W2.4); §1 guarantee = no-rpc-reachable write path over 8 mutators (P5 structural check, not "single write edge"); ZERO production callers (Promote/ExpirePending/DiscardSuperseded/UndoPromote/StoreSkShare-PENDING all unwritten) — mechanism complete + unreachable, first consumer W2.3; unit-verified only, fleet bound to W2.4. | §23 | ★ P1–P5 built, unit-verified 2026-07-24 (callers owed W2.3/W2.4) |
| KDD-037 | Genesis ALWAYS_ACTIVE base case (back-filled). ptxbea is the first live chain with `UPGRADE_V6_0 = ALWAYS_ACTIVE` booted from genesis. `GetListForBlock(genesis)` threw — there was no empty-list base case (`IsActivationHeight(0+1)` is false) — fixed with the `isGenesisAlwaysActive` guard (deterministicgms.cpp). Addendum (same class): `ConnectBlock(genesis)` set `view.SetBestBlock` but not `evoDb->WriteBestBlock`, so block-1's `evoDb->VerifyBestBlock(genesis)` failed — fixed by adding the genesis `WriteBestBlock`, mirroring the normal-exit pair. Any future chain with ALWAYS_ACTIVE-from-genesis requires both. Scope: genesis→block-1 class closed; LLMQ first-activation deferred. | §— (row-only) | Landed 2026-05-31 (commits 67517d2 + 2aa1ea3); row back-filled 2026-07-23 from the two commits + the standup's own drafted "(for register)" text |
| KDD-064 | ★ Stake min-depth maturity gate — **ALLOCATED-THEN-ABANDONED** (back-filled). The intended PTX-specific stake maturity gate (stopping fork-created coins from being re-staked before `nStakeMinDepth`) was **never committed**: it was built only in the throwaway dev image `dbde8f22` (bundled with BUG-019(d)), found inert on 2026-07-19 (gated behind `UPGRADE_ZC_PUBLIC`, whose activation height is a never-reached sentinel — 71,000,000 mainnet / 100,000,000 public-testnet, chainparams.cpp), and dropped. What EXISTS at HEAD is entirely **inherited, and none of it is a KDD-064 deliverable**: the `UPGRADE_V3_4`-gated stake-modifier-V2 depth check at `consensus/params.h:266-267` (from the 2023 import at `1f34501`) plus the advisory wallet-side filters at `wallet.cpp:2172` (GetStakingBalance) and `:2700` (StakeableCoins). The height-aware wallet wrap the standup describes **never landed**. ★ Therefore **PTX has no maturity gate of its own** — the standup's 23 references describe an *intent*, not a shipped rule; a reader seeing `params.h:266-267` would wrongly assume an enforced PTX maturity gate exists. **SHA UNRESOLVED because no commit exists** (not because it wasn't found). Cross-ref BUG-020 (dev-net-only fork-restake artifact, same nStakeMinDepth/maxreorg interaction). **Audit note:** this row is the single **ALLOCATED-THEN-ABANDONED** entry in the 033–068 span — it corrects the 2026-07-23 gap-audit's provisional "zero ALLOCATED-THEN-ABANDONED in span" reading. | §— (row-only) | Intent built 2026-07-18, found inert & abandoned 2026-07-19 (standup); **no commit ever landed — SHA UNRESOLVED (none exists)**; row back-filled 2026-07-23 |
| KDD-065 | PTX ceremony member-connection wiring via a map-key-only pseudo-type (back-filled). `LLMQ_TYPE_PTX_CEREMONY = 200` is a TierTwoConnMan **map-key ONLY** (connman path type-opaque; sweep-proven). HARD CONSTRAINT: never inserted into `consensus.llmqs`, never keys a params lookup; **no consensus surface**. Hooks in the formation-thread lifecycle: `setQuorumNodes(members-minus-self)` at session start (ThreadBody head), `removeQuorumNodes` at the single teardown chokepoint (thread epilogue) — STARTED==EXITED 30/30 under SG-2b-0 fork-flap thrash; idempotent erase keyed by the captured quorum_hash. Closes SG-2b-0 CP-3's register-marked GMAUTH-inert-relay blocker **and** the member-connection/announce-miss gap in one invocation. Bundled VERDICT-A (topology CLOSED): relay-partition safety under ceremony conversion is production-fixed by inherited tiertwo machinery (Dash-mainnet-proven); the fleet no-self-heal is a shared-/16 test artifact (net.cpp:1732-47) with no production analog; **no shared-connman patch** (that is the caller-bridge category error). Registered-not-fixed: post-teardown stale-GM-edge lingering (net_gamemasters.cpp:388-398, gated on non-GM outbound ≥ target, pinned at 1 by the /16 cap) — /16-rooted fleet soak artifact, SG-2b-1 concern, chain-dark-guard-detected. Source: `consensus/params.h:100` (enum + constraint comment), `ptx_formation.cpp:12/17/162/247`. **Reconstructed from commit 7391617 + source only — appears in no prior doc and not in the standup.** ★ Cross-ref **ODC-040** (fleet homogeneity coverage limit — the /16 netgroup no-self-heal is a single-subnet artefact with no production analog). | §— (row-only) | Landed 2026-07-21 (commit 7391617); row back-filled 2026-07-23 |
| KDD-034 | *(pointer — not a DKG-register decision)* PTXCONSOLIDATE (nType=8) + ConnectBlock pool-input pre-check; Phase-2 consensus tx. Recorded by commit 737214d (2026-05-25) and the standup. Not registered here. | — | Pointer added 2026-07-23 |
| KDD-035 | *(pointer — not a DKG-register decision)* PTX wallet/RPC feature. Registered in the tracked `doc/ptx/ptxbea-api-reference.md` + `ptxbea-operator-guide.md`; source `rpc/ptx.cpp`, `ptx/ptx_wallet.h`. Not registered here. | — | Pointer added 2026-07-23 |
| KDD-036 | *(pointer — not a DKG-register decision)* PTX wallet/RPC feature. Registered in the tracked `doc/ptx/ptxbea-api-reference.md`; source `rpc/ptx.cpp`, `ptx/ptx_wallet.h`. Not registered here. | — | Pointer added 2026-07-23 |
| ODC-023 | *(pointer — not a DKG-register decision)* ptxbea known-limitation. Registered in the tracked `doc/ptx/ptxbea-known-limitations.md` (+ api-reference, operator-guide). Not registered here. | — | Pointer added 2026-07-23 |
| ODC-042 | ★ D-SG1a-2 LIVE at first supersede — P-b's ACTIVE→SUPERSEDED is the first state mutation on quorum records; `GetActiveQuorumsAtHeight` reads CURRENT state while V5's pool query runs at the anchor (lags pindexPrev ≤ N−1) → a rotation connecting in the window frees 11 members into the validator's pool that formation excluded → honest formation self-rejects; same block validates differently by evaluation timing → chain split (SG-1a class). Fix: `superseded_height` in record v2 + as-of predicate `ACTIVE ∨ superseded_height > h`; revert payload-derivable at UndoBlock. ★ MUST land before/with V12 (P-b4 ≤ P-b3), never after. Sequential reindex safe; anchor-lag + reorg windows not. bf-fleet verifiable | §— (entry above) | Recorded 2026-07-25 (consensus-correctness; tier V11/ODC-041) |
| KDD-073 | Three-reconstruction-sites constraint on V12 — KDD-072 §5 substitution must land atomically at the V5 validator + the store connect guard (ptx_quorum_store.cpp:94-127) + the debug builder if rotations become injectable, via ONE shared materialization helper (predecessor.members → GM ptrs at a pinned block, record-order share_index per SG-3/KDD-061); ground = the ptx_quorum_store.cpp:86-93 battery_sg1 lesson (validator-only substitution → valid rotation passes CheckPTXDKGTx, dies in ProcessBlock). Test obligation: falsification must exercise CONNECT, not just validation | §— (entry above) | Recorded 2026-07-25 (binding constraint on P-b3) |
| KDD-072 P-b DRILL | First fleet verification of the rotation arc (2026-07-25, bf/bbe5e0e): all 5 predictions confirmed — V12+MarkSuperseded on connect, Promote key-isolation no-op, RestoreActiveOnUndo clean revert, identical replay; ★ ODC-042 verified on a real chain (formation anchored h960 connected at h1015 post-supersede, members unchanged); P3 record-vs-slot divergence documented as a DEBUG-INJECTION ARTIFACT (a real rotation via P-b6 does not produce it); coverage boundary: proves the wiring fires/reverts/replays, NOT the trigger, ceremony-driven promotion, or drift | §— (entry above) | Drill complete 2026-07-25; fleet-verified |
| ODC-043 | `ptx_quorum_info` does not surface record-v2 `superseded_height` / `disbanded_height` — observability gap surfaced by the P-b drill (supersede verified from a log line, not the RPC). Display-only RPC addition; values already persisted + consensus-correct. Useful before P-b6, whose rotations run unattended. NOT a defect | §— (entry above) | Owed (small), registered 2026-07-25 |

---

## Bug register (formal table — added 2026-07-24)

*Why a table (rationale recorded on evidence, not preference):* bugs were tracked in **prose only**, and that form **lost six of the nine** PTX bugs — 006, 015, 016, 017, 021, 022 had no register presence, while only 018/019/020 survived as scattered mentions. Over the same span the **KDD and ODC tables held** every entry. The table form is chosen on that evidence. Append-only: existing prose for 018/019/020 is left in place; the rows below point to it, no prior text was edited. Range confirmed from commit history + the standup: **006 is the lowest, 022 the highest** (no BUG-023+ exists anywhere). Numbering gaps (001–005, 007–014) are recorded as pointer rows so the range has no silent hole (same treatment as the KDD-034/035/036 pointers). Register scope note applies: bug numbering predates this DKG register; earlier ptxbea bugs live in `ptxbea-*.md` + in-source comments.

**BUG-021 (2026-07-20). PTX getdata connection-wedge + dead serve leg — a dead-code defect invisible to unit tests (full entry).**

The 5 `MSG_PTX_QUORUM_*` inventory types were absent from `IsTierTwoInventoryTypeKnown` (net_processing.cpp:1200-1217). A PTX getdata was therefore rejected BY TYPE (the guard at :1229) **without advancing the `ProcessGetData` loop iterator** — the loop body incl. its `it++` (:1238) never runs, and `erase(begin(), it)` (:1273) erases nothing. The inv PERSISTS at `vRecvGetData`'s head, so `ProcessMessages:2348` (`if (!vRecvGetData.empty()) return true`) fires on every pass → **that peer's message processing FREEZES permanently** (one-directional wedge), plus the `:2404` `fMoreWork` busy-spin. Two consequences: (a) the PTX serve arms (:1007-1047, added inside `PushTierTwoGetDataRequest`) were **unreachable dead code**; (b) any PTX getdata **wedged the peer**.

★ **PRODUCTION-REACHABLE BY CONSTRUCTION** — rejection is by TYPE, not storedness: inv/getdata is the ONLY body-transfer path (relayHook pushes inventory only), the inv side admits PTX (:879-884) and AskFors (:1724), and getdata assembly is unfiltered (:2721-35). The first live active-driver formation would have wedged the quorum mesh **pairwise** and delivered **zero bodies**. Fix `f4eb31e` adds the 5 types to the admission list.

★ **PATTERN — invisible to unit tests; THIRD instance of the class.** Unreachable dead code is indistinguishable from working code without a **live peer**: the serve arms compiled, linked, and passed every unit test precisely because nothing exercised the transfer path. This is the third instance of the "unit-green because the gap is only reachable live" class — (1) the **ODC-036** threshold defect (invisible to 271/271 because the tests passed threshold in EXPLICITLY; caught only at the first fleet signing), (2) the **.py scenario assertions** (several asserted on paths never runtime-validated — syntax-verified only), (3) this. Lesson recorded: transport / serve-arm code needs a live-peer test to distinguish dead from working; unit-green is not coverage for it.

| BUG | Date | Summary | Commit | Status |
|---|---|---|---|---|
| BUG-001–005 | — | *(numbering gap — no such bug)* no commit exists; PTX bug numbering as recorded begins at 006. Recorded so the range has no silent hole. | — | Pointer added 2026-07-24 |
| BUG-006 | 2026-05-15 | ptxbea input-validation fixes + test-suite salt / assumption corrections. Grouped in one commit as **006/007/008** (007/008 have no standalone commit). PTX. | da55bb5 | Landed; row back-filled 2026-07-24 |
| BUG-007–008 | 2026-05-15 | *(folded into BUG-006)* same commit `da55bb5`, no standalone commit; recorded so the range has no silent hole. | da55bb5 | Pointer added 2026-07-24 |
| BUG-009–014 | — | *(numbering gap — no such bug)* no commit exists between 008 and 015. | — | Pointer added 2026-07-24 |
| BUG-015 | 2026-06-01 | Section B gate on coinstake vout count — burn-split diagnostic. Default wallet config never triggers the split path (gm01 UTXOs above `stakeSplitThreshold`), so burn always fired across all 3 Section B samples. PTX (lottery/roll). ★ **Owed task still open:** set `stakeSplitThreshold` below gm01 UTXOs, force a `stakerOuts>2` block, confirm burn absent. ★ Cross-ref **ODC-039** (the burn-escape open question it surfaced) and **ODC-040** (fleet homogeneity — uniform `stakeSplitThreshold` is why no split coinstake was ever produced). | a86ee16 | Landed (diagnostic + gate); owed task OPEN; back-filled 2026-07-24 |
| BUG-016 | 2026-06-02 | `ptx_roll` B.1 input-validation pass. PTX. | 486d186 | Landed; row back-filled 2026-07-24 |
| BUG-017 | 2026-06-02 | `ptx_roll` B.1 input-validation pass (same commit as 016). PTX. | 486d186 | Landed; row back-filled 2026-07-24 |
| BUG-018 | 2026-06-02 | Lock UTXOs during `FundTransaction` to prevent concurrent-roll collision. Prose x-ref: KDD-043 §5.2 (row migrates that mention). PTX. | 801c557 | Landed; prose migrated to row 2026-07-24 |
| BUG-019 | 2026-07-18 | GM collateral lock — `LockGamemasterCollaterals()` extracted and HOISTED before `ThreadStakeMinter` (019(d) durable fix; closes R1 staker-snapshot race + R2 by construction; RED-by-revert proven). Prose x-ref: T-I row (member DGM-deregistration mid-life). PTX. | 8fe12c2 | Landed (019(d)); prose migrated to row 2026-07-24 |
| BUG-020 | 2026-07-20 | View-dependent stake-failure ban DOWNGRADE (local ban-policy) — un-seals valid-but-lighter forks. ★ **RECLASSIFIED** (in the 019(d) commit body + standup) from pre-testnet blocker to **dev-net-only artifact**: nStakeMinDepth=300 mainnet/public-testnet ≫ maxreorg=100, so the DoS-ban cannot fire until a fork is already 3× past maxreorg and unhealable for independent reasons — a symptom of an already-fatal fork, not a cause. Prose x-ref: KDD-064, ODC-037. PTX. | dd4e5f7 | Landed + reclassified; prose migrated to row 2026-07-24 |
| BUG-021 | 2026-07-20 | PTX getdata connection-wedge + dead serve leg — 5 `MSG_PTX_QUORUM_*` types absent from `IsTierTwoInventoryTypeKnown` → permanent per-peer message freeze + `fMoreWork` busy-spin + unreachable serve arms; production-reachable by construction (would wedge the quorum mesh pairwise, zero bodies). **See full entry above.** Third dead-code-invisible-to-unit-tests instance (with ODC-036 + the .py assertions). PTX. | f4eb31e | Landed; full entry + row 2026-07-24 |
| BUG-022 | 2026-07-20 | *(cross-ref — inherited, NOT PTX code)* pre-existing `cs_wallet`→`cs_main` lock-order inversion in `AddDGMEntryToList` (rpcevo) — the sole daemon-side outlier vs 92 canonical `LOCK2(cs_main, cs_wallet)` sites; one-line hoist. Caught by the `DEBUG_LOCKORDER`-armed build on FIRST armed deploy (SG-2b-0 prereq), 5/5 deterministic. ★ Its register value is the **testing-posture finding** — the armed safety floor works as intended — not a PTX defect. | dfc00dd | Landed; cross-ref row 2026-07-24 |
