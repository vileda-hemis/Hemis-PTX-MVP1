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

**KDD:** KDD-063

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
| KDD-057 | P5 sk_share_i write path Option A — shared g_ptx_my_bls_sk_bytes store; W1.3 replay guard must cover both write sites | §17 | Decided 2026-06-12 |
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
| KDD-067 | sk_share clear/rotate path — PTX_BLS_SetSkShare is refuse-unless-empty (§C1 replay guard, correct as replay protection), and PTX_DKG_StoreSkShare routes through it, so ClosePhase5 turns a refused overwrite into phase=ABORTED. Consequence: **a member that completes one ceremony burns out for the rest of that process lifetime** — any later re-selection WITHOUT a restart aborts it at FINALIZE (the slot is in-memory and cleared on restart; see ODC-035). Unit-demonstrated (P5_ReSelection_SecondStoreRefusedAborts). NOT an SG-3 blocker (SG-3 signs with an existing quorum, no re-selection). A legitimate re-selection must clear the prior share first | §23 | Open (owed W2.3 rotation / W2.4 disband-top-up) |
| ODC-034 | Fleet quorum saturation — 22 GMs with two ACTIVE quorums exhausts the KDD-040 pool; no formation possible until a disband path exists. Blocks every downstream gate needing a fresh ceremony. Recurring (also SG-1a, quorum #2 h518); interim remedy bank-restore — CONFIRMED accurate: a restore does compose down/up, restarting every daemon and clearing every in-memory sk_share slot, so fresh ceremonies are possible afterward. The SAME restart-clears-slots mechanism is what makes ODC-035 a silent-membership-loss problem | §9.13 | Open (bound W2.4) |
| ODC-035 | DKG share material is PROCESS-LIFETIME ONLY — g_ptx_my_bls_sk_bytes is an in-memory global (ptx_bls.cpp:21) with no load/save path (grep-confirmed). A GM restart silently and irrecoverably drops that member from its quorum while the on-chain CPTXQuorumRecord still lists it in_qual: DURABLE membership, EPHEMERAL key material, NO reconciliation. Consequence: a quorum can fall below threshold with no on-chain or RPC indication. Measured 2026-07-23 pre-SG-3-signing: fc8e0f0d(h1040) 9/9 in_qual live, 57e7c7b4(h960) 6/6 live (both PASS-capable at t=6 — no loss yet, but nothing prevents it). Owed: share persistence, or an on-chain/RPC liveness signal, or both (scope, do not solve here). Cross-ref KDD-067 (its permanence is bounded by this), ODC-034 (bank-restore relies on the same restart-clears-slots mechanism) ; NOTE: SG-3 signing gate (2026-07-23) demonstrated a quorum signing CORRECTLY while carrying this risk — a GM restart would still silently drop a signer; risk unchanged | §22 | Open (owed W2.3/W2.4) |
| ODC-036 | DKG signing threshold was derived from COORDINATOR REGISTRY SIZE (g_ptx_nodes → n/2+1 = 12 at 22 nodes) instead of QUORUM SCOPE (formed_size 11 → t=6). Wrong on any fleet where registered-nodes != quorum-size; reached FOUR sites (PTX_LoadDKGSigningCtx usability, round.threshold→commit-reveal, the enough-partial-sigs check, and the Lagrange slice). Caught LIVE at the first fleet signing attempt (fail-closed hard-error on the valid 9-in_qual fc8e0f0d); invisible to 271/271 because the unit tests passed threshold in EXPLICITLY. FIXED (2026-07-23): PTX_SelectDKGSigningCtx derives t=majority(formed_size) internally (threshold param deleted, so registry-independence is structural) and returns ctx.threshold — the single source the roll's four sites read; St1-St4 pin the derivation, RED-by-inversion. ★ ADDENDUM (owed): deriving t from formed_size is correct ONLY while t == n/2+1. KDD-048 chose t=6 by WARGAME, not formula, and pre-documents a t=7-at-n=11 upgrade; on that upgrade the derivation silently returns 6 for a ceremony that baked 7, and KDD-048's upgrade semantics let t=6 and t=7 quorums coexist. The derivation is a STOPGAP with a known scheduled break. OWED: persist t in CPTXQuorumRecord at formation — CHEAP (record is nVersion-tagged with the additive `if (nVersion>=2)` pattern; v1 records fall back to the derivation, no reindex) | §21 | Fixed (derivation); persist-t OWED |
| ODC-037 | GM BANLIST RESIDUE survives daemon restart, chainstate wipe, AND bank-restore (banlist.dat is not touched by any of them). A node that earned bans in a prior episode CANNOT re-integrate after being wiped: it presents as persistent `socket send error Broken pipe` CONNECTION failures, NOT as a ban, because it is rejected before any RPC/log surface makes the cause visible. Reason category: MISBEHAVIOUR (`node misbehaving` = DoS-score-to-threshold, the BUG-020-lineage view-dependent stake-failure path). EVIDENCE (2026-07-23): the original caller 172.31.0.10 banned on 7/22 GMs (gm01/02/05/11/12/16/22 — exactly its addnode targets), reason `node misbehaving`, 24h expiry (created+86400); ban predates the node's post-wipe uptime, so retries do NOT re-earn (rejected at connection, cannot misbehave further) — persistence is banlist.dat survival, not a retry loop. This is WHY the caller-resync failed and caller2/caller3 on fresh IPs (.33/.34) were required. OWED: add banlist clearing (clearbanned / remove banlist.dat, or setban remove for the affected IPs) as an EXPLICIT step in the restore procedure (restore_fleet.sh), so a restored fleet does not silently exclude a wiped-and-rejoining node. ★ AMENDMENT (2026-07-23, append-only): CORRECTION — the ban carries a 24h TTL (banned_until 1784845560, ban_created 1784759160, 86400s span); exclusion is BOUNDED, not permanent — the prior wording ('cannot re-integrate', 'persistence') OVERSTATED it. The DURABLE finding is DIAGNOSTIC OPACITY: for up to 24h a wiped node is rejected BEFORE any RPC or log surface shows a ban, presenting only as broken-pipe connection failure — the cause is invisible from the excluded node. REMEDIES in order of touch: (a) wait out the TTL — zero-touch, no GM contact; (b) setban remove on the affected GMs — live RPC, no restart; (c) clearbanned / remove banlist.dat in restore_fleet.sh — the procedural fix for the restore path. SG-3 CONSEQUENCE (honest): fresh-IP caller2/caller3 were NOT strictly required — waiting ~24h would have restored .10; the fresh nodes were the right call under time pressure and gave a cleaner non-GM coordinator, but the ban was not the blocker it appeared to be | §22 | Open (owed — restore-procedure step) |
| ODC-038 | ★ FLEET BINARY DIVERGENCE — DO NOT DEPLOY TO GMs. As of 2026-07-23 the coordinators (172.31.0.33/.34) run hemis-ptx-w2:0101a44-dbg while ALL 22 GMs run hemis-ptx-w2:8719b7c-dbg. This is INTENTIONAL: the SG-3 signing repoint (0101a44) is COORDINATOR-SIDE ONLY — group_pk/share_index are read from committed data, the GMs' gm_bls_sign already signs the DKG-stored share, so GMs need nothing from it. ★ HAZARD: a GM restart CLEARS its in-memory sk_share (ODC-035 — process-lifetime only, no persistence). Both quorums (fc8e0f0d fh=1040, 57e7c7b4 fh=960) are ACTIVE, so KDD-040 blocks reformation and NO disband path exists (ODC-034) → a fleet-wide GM deploy/restart DESTROYS BOTH QUORUMS PERMANENTLY, and the only recovery is a bank-restore to a pre-h960 snapshot (losing all landed results after it). SAFE to deploy to GMs ONLY after W2.4 disband exists, or as a DELIBERATE restore where losing both quorums is the intent. A routine 'update the fleet to the latest image' is a footgun here | §22 | Open (blocks GM deploy until W2.4) |
| KDD-068 | Discipline: a pure function tested with a HAND-PASSED parameter does not cover the call site that COMPUTES it. PTX_SelectDKGSigningCtx passed 271/271 with threshold=6 handed in, while the ptx_roll call site computed the wrong value (ODC-036) — untested because the derivation lived at the caller. Rule: extract the derivation into the pure function and pin it; a parameter the test controls is a parameter the test cannot falsify | §21 | Recorded 2026-07-23 |
| KDD-069 | ★ TRUSTED DEALER RETIRED — DKG signing is the ONLY path. Removed (coordinator-side): `PTX_BLS_Init` (central polynomial + all shares), its alphabetical Lagrange basis `g_ptx_bls_state.node_index` / `PTXBLSState` / `cs_ptx_bls`, the `gm_bls_keyset` coordinator→GM fan-out (`PTX_FanOutKeySet` + `PTX_BLS_GetShareBytes`), `PTX_AssignQuorum` + `PTX_NodeScore` + `PTXQuorumAssignment` (AssignQuorum-only), `PTX_BLS_Threshold`, and the dead `PTX_GetBLSState` (zero callers). KEPT: `PTX_BLS_PartialSign`/`Recover`/`Verify` (shared with the DKG path), `g_ptx_nodes` (fanout address book), the GM sk-share slot, `gm_bls_sign`. `ptx_roll` collapses to DKG-only: signs ONLY with an ACTIVE quorum's committed material (group_pk + score-order share_index from CPTXQuorumRecord), with **two hard-errors and no fallback** — "quorum present but unusable" and "no ACTIVE quorum for height" (both "dealer retired, KDD-069"). `signing_source` is now invariantly "dkg" (retained for response-schema stability). ★ **CAPABILITY LOSS (intended trade, recorded so it is not rediscovered):** a bare fleet with NO ACTIVE quorums can no longer be rolled at all — the dealer was what made a fresh, quorumless fleet rollable; a rollable fleet now requires forming a DKG quorum first. **NO CONSENSUS SURFACE:** the dealer was coordinator-side (dies in `rpc/ptx.cpp` + `ptx_bls`/`ptx_fanout`/`ptx_quorum` helpers); `CheckPTXDKGTx`, `CPTXQuorumStore`, and block validation never read it — validators are unaffected. **FLEET IMPACT:** the ptx-bea PoC runs the dealer on a DIFFERENT branch/images (unaffected); the W2 fleet already DKG-signs (SG-3), so no live W2 dependency. **.py surface:** the 3 ptx-bea scenarios + the 120-test dealer-era `ptx_test_suite_v4.py` were updated to assert the KDD-069 hard-error (v4 via a `DEALERLESS` probe that runs T01 live and auto-skips 119; their full disposition is OWED) — syntax-verified only, NOT runtime-validated (no fleet run authorised). Landed in 3 commits: **B** tests `3c616d9`, **A** removal `8a2200e`, **C** register (this). Cross-ref KDD-052 (seam structurally eliminated), KDD-057 (§C1 rationale amended: replay/double-store, not dealer hijack), KDD-070 (slot mechanism — the owed clear/authorized-overwrite path), KDD-071 (share liveness, if it survives recon). | §12/§17 | Decided & landed 2026-07-24 (commits 3c616d9 / 8a2200e) |
| KDD-037 | Genesis ALWAYS_ACTIVE base case (back-filled). ptxbea is the first live chain with `UPGRADE_V6_0 = ALWAYS_ACTIVE` booted from genesis. `GetListForBlock(genesis)` threw — there was no empty-list base case (`IsActivationHeight(0+1)` is false) — fixed with the `isGenesisAlwaysActive` guard (deterministicgms.cpp). Addendum (same class): `ConnectBlock(genesis)` set `view.SetBestBlock` but not `evoDb->WriteBestBlock`, so block-1's `evoDb->VerifyBestBlock(genesis)` failed — fixed by adding the genesis `WriteBestBlock`, mirroring the normal-exit pair. Any future chain with ALWAYS_ACTIVE-from-genesis requires both. Scope: genesis→block-1 class closed; LLMQ first-activation deferred. | §— (row-only) | Landed 2026-05-31 (commits 67517d2 + 2aa1ea3); row back-filled 2026-07-23 from the two commits + the standup's own drafted "(for register)" text |
| KDD-064 | ★ Stake min-depth maturity gate — **ALLOCATED-THEN-ABANDONED** (back-filled). The intended PTX-specific stake maturity gate (stopping fork-created coins from being re-staked before `nStakeMinDepth`) was **never committed**: it was built only in the throwaway dev image `dbde8f22` (bundled with BUG-019(d)), found inert on 2026-07-19 (gated behind `UPGRADE_ZC_PUBLIC`, whose activation height is a never-reached sentinel — 71,000,000 mainnet / 100,000,000 public-testnet, chainparams.cpp), and dropped. What EXISTS at HEAD is entirely **inherited, and none of it is a KDD-064 deliverable**: the `UPGRADE_V3_4`-gated stake-modifier-V2 depth check at `consensus/params.h:266-267` (from the 2023 import at `1f34501`) plus the advisory wallet-side filters at `wallet.cpp:2172` (GetStakingBalance) and `:2700` (StakeableCoins). The height-aware wallet wrap the standup describes **never landed**. ★ Therefore **PTX has no maturity gate of its own** — the standup's 23 references describe an *intent*, not a shipped rule; a reader seeing `params.h:266-267` would wrongly assume an enforced PTX maturity gate exists. **SHA UNRESOLVED because no commit exists** (not because it wasn't found). Cross-ref BUG-020 (dev-net-only fork-restake artifact, same nStakeMinDepth/maxreorg interaction). **Audit note:** this row is the single **ALLOCATED-THEN-ABANDONED** entry in the 033–068 span — it corrects the 2026-07-23 gap-audit's provisional "zero ALLOCATED-THEN-ABANDONED in span" reading. | §— (row-only) | Intent built 2026-07-18, found inert & abandoned 2026-07-19 (standup); **no commit ever landed — SHA UNRESOLVED (none exists)**; row back-filled 2026-07-23 |
| KDD-065 | PTX ceremony member-connection wiring via a map-key-only pseudo-type (back-filled). `LLMQ_TYPE_PTX_CEREMONY = 200` is a TierTwoConnMan **map-key ONLY** (connman path type-opaque; sweep-proven). HARD CONSTRAINT: never inserted into `consensus.llmqs`, never keys a params lookup; **no consensus surface**. Hooks in the formation-thread lifecycle: `setQuorumNodes(members-minus-self)` at session start (ThreadBody head), `removeQuorumNodes` at the single teardown chokepoint (thread epilogue) — STARTED==EXITED 30/30 under SG-2b-0 fork-flap thrash; idempotent erase keyed by the captured quorum_hash. Closes SG-2b-0 CP-3's register-marked GMAUTH-inert-relay blocker **and** the member-connection/announce-miss gap in one invocation. Bundled VERDICT-A (topology CLOSED): relay-partition safety under ceremony conversion is production-fixed by inherited tiertwo machinery (Dash-mainnet-proven); the fleet no-self-heal is a shared-/16 test artifact (net.cpp:1732-47) with no production analog; **no shared-connman patch** (that is the caller-bridge category error). Registered-not-fixed: post-teardown stale-GM-edge lingering (net_gamemasters.cpp:388-398, gated on non-GM outbound ≥ target, pinned at 1 by the /16 cap) — /16-rooted fleet soak artifact, SG-2b-1 concern, chain-dark-guard-detected. Source: `consensus/params.h:100` (enum + constraint comment), `ptx_formation.cpp:12/17/162/247`. **Reconstructed from commit 7391617 + source only — appears in no prior doc and not in the standup.** | §— (row-only) | Landed 2026-07-21 (commit 7391617); row back-filled 2026-07-23 |
| KDD-034 | *(pointer — not a DKG-register decision)* PTXCONSOLIDATE (nType=8) + ConnectBlock pool-input pre-check; Phase-2 consensus tx. Recorded by commit 737214d (2026-05-25) and the standup. Not registered here. | — | Pointer added 2026-07-23 |
| KDD-035 | *(pointer — not a DKG-register decision)* PTX wallet/RPC feature. Registered in the tracked `doc/ptx/ptxbea-api-reference.md` + `ptxbea-operator-guide.md`; source `rpc/ptx.cpp`, `ptx/ptx_wallet.h`. Not registered here. | — | Pointer added 2026-07-23 |
| KDD-036 | *(pointer — not a DKG-register decision)* PTX wallet/RPC feature. Registered in the tracked `doc/ptx/ptxbea-api-reference.md`; source `rpc/ptx.cpp`, `ptx/ptx_wallet.h`. Not registered here. | — | Pointer added 2026-07-23 |
| ODC-023 | *(pointer — not a DKG-register decision)* ptxbea known-limitation. Registered in the tracked `doc/ptx/ptxbea-known-limitations.md` (+ api-reference, operator-guide). Not registered here. | — | Pointer added 2026-07-23 |
