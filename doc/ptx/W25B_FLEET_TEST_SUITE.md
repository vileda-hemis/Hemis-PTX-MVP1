# W2.5b Extended Fleet Test Suite — DESIGN (build on request, not yet built)

*Written 2026-07-29 against the live N98 fleet at HEAD `3a95e22`. **Design only** —
nothing here is built or run. The suite is scoped to be built INCREMENTALLY in the
tier order below.*

## Why this suite exists (the framing that sets its priorities)

**ODC-055 proved that real-wire DKG delivery had never been exercised anywhere in
the project.** Unit ceremonies are in-process; the N22 fleet's ceremonies completed
by a dense-mesh accident. The fleet is therefore the **first substrate on which any
PTX property can be claimed over real transport at multi-quorum scale** — and the
suite's core purpose is to re-establish, on the wire, what was only ever proven
in-process, plus the fault/lifecycle/soak dimensions only a live fleet can reach.

Every test below carries four tags:
- **RE-RUN** (an existing unit/phase-0 test re-validated over real wire) vs **FLEET-NEW**
- **HARNESS**: exists / BUILD-NEEDED
- **RED**: how the test is proven able to fail
- **SAFETY**: `SAFE` (main fleet) / **`DESTRUCTIVE`** (disposable fleet required)

★ **Destructive tests must not run on the main fleet.** Killing members, partitioning,
and starving demand all mutate quorum lifecycle state irreversibly (a reformed quorum
never comes back; a rotation consumed is gone). The main fleet is the *soak and
demo* substrate. See "Fleet policy" at the end.

---

## TIER 0 — PRE-FLIGHT GATES (run before any test; no gate, no run)

### P0.1 — ★ Caller funding pre-flight (CASE 1: harness guard)
- **Validates**: the suite *can complete* — prevents the spurious-failure mode where
  a caller runs dry mid-suite and system-correct tests fail for environment reasons.
  A dry caller mid-run is an environment fault masquerading as a bug.
- **Mechanism**: compute the suite's total requirement — `Σ(tests) rolls × per-roll
  cost × margin(2×)` — and assert **per caller** that its **spendable** balance covers
  **its planned share**. ★ Spendable, not total: `listunspent(minconf≥1)` filtered for
  unlocked/mature/confirmed — locked collateral, immature coinbase, and unconfirmed
  change do NOT count.
- ★ **Per-caller, not aggregate**: tests that pin rolls to a named caller (SG-3
  independence, anti-targeting) cannot be rescued by a rich sibling.
- **On insufficient**: **HALT** before test 1 with the shortfall named and the top-up
  command emitted (`sendtoaddress` from the funded caller — the established pattern).
- **HARNESS**: BUILD-NEEDED (small: a `funding_preflight.py` reading the suite manifest's
  per-test roll counts). Depends on the suite manifest existing.
- **RED**: set a caller's share above its balance → the gate must HALT (not warn-and-run).
- **SAFETY**: SAFE.

### P0.2 — Fleet-health gate
- 102 containers up; 98/98 GMs enabled; height spread 0; **ACTIVE quorum count == 8**;
  no laggards; driver alive. Reuses `validate_fleet.py` + `fleet_watch` state.
- ★ **Chain-liveness sub-gate (added 2026-07-29 from a live incident)**: assert the tip
  has advanced in the last N minutes. See BUG-022 below — a stalled chain freezes every
  height-driven mechanism and would present as mass test failure.
- **HARNESS**: exists (validate_fleet gates) + small liveness assert.
- **SAFETY**: SAFE.

---

## TIER 1 — ROLL CORRECTNESS over real wire (cheap, highest re-validation value)

*All RE-RUN of the phase-0/v7 suite, whose prior passes were in-process or
single-quorum. The point is not novelty — it is that **none of these has ever been
demonstrated end-to-end over real P2P with a real DKG quorum**.*

| # | Test | Validates | Tag | RED |
|---|---|---|---|---|
| R1 | count/low/high sweep (1..N draws over varied ranges) | mapping correctness on-wire | RE-RUN | assert a value outside [low,high] |
| R2 | `unique=true` no repeats | uniqueness within a roll | RE-RUN | duplicate in results |
| R3 | exclude-set honoured | excluded values never appear | RE-RUN | excluded value present |
| R4 | `low==high` (degenerate range) | forced single value | RE-RUN | any other value |
| R5 | `count == range size` + unique | full permutation | RE-RUN | short/duplicated set |
| R6 | ★ `count > range` + unique | **clean rejection**, not a hang/partial | RE-RUN | accepts or hangs |
| R7 | empty/inverted range (`low>high`) | clean rejection | RE-RUN | accepts |
| R8 | game_id / caller_salt variation | varied input → varied valid output | RE-RUN | identical outputs across distinct seeds |
| R9 | ★ **determinism**: same round_seed → same result | the beacon is a pure function of the seed | RE-RUN | differing results |
| R10 | ★★ **verifiability**: returned `quorum_sig` verifies against the quorum's on-chain `group_pk`; beacon == SHA256(sig); results == MapBeacon(beacon) | **the whole threshold-BLS chain, END-TO-END ON REAL WIRE — never proven before** | **FLEET-NEW in substance** | tamper one byte of sig/group_pk → verify must fail |

- **HARNESS**: exists (`node.ptx_roll` + `ptx_getroundstatus`); R10 needs a BLS verify
  helper in the harness (BUILD-NEEDED, small — or shell out to a debug RPC).
- **SAFETY**: all SAFE (rolls are non-destructive; they *help* the anti-idle posture).

---

## TIER 2 — §7.4 ROUTING / DISTRIBUTION (live, cheap, high evidentiary value)

| # | Test | Validates | Tag | Harness | RED | Safety |
|---|---|---|---|---|---|---|
| D1 | Distribution evenness over a large sample (≥500 rolls, χ²-style against uniform over 8) | §7.4 spreads; ODC-052's structural fix under real demand | FLEET-NEW | exists (driver JSONL) | force newest-wins routing → mass concentration | SAFE |
| D2 | Routing determinism: same tip → same quorum (multiple callers, one block) | `hash(tip) mod ACTIVE` is chain-derived, not caller-derived | FLEET-NEW | exists | different quorums within one block | SAFE |
| D3 | ★ Single-block concentration residual (N rolls in one block) | the **accepted** Moment-1 residual, now *measured* rather than asserted | FLEET-NEW | exists | — (characterization, not pass/fail; record the number) | SAFE |
| D4 | ★ Anti-targeting (§7.4 INV-3): grind caller_salt against a target quorum | a caller **cannot steer** its server | RE-RUN (unit inversion exists) | exists | make selection key on round_seed → grinding succeeds | SAFE |
| D5 | ★ ODC-052 sparsity: throttle demand below 1-roll-per-25-blocks | quorums read idle → REFORM (the parameter bound, live) | FLEET-NEW | BUILD-NEEDED (driver `--pause`/rate control) | — | ★ **DESTRUCTIVE** (induces reforms) |

---

## TIER 3 — FAULT INJECTION: the FAIL-SAFE validation (KDD-077 §4) ★ fleet-unique

★ **All DESTRUCTIVE — disposable fleet.** ★ **Every test in this tier carries the same
overriding assertion: a broken quorum produces SILENCE or a clean ABORT, never a wrong,
forged, or partial-but-accepted beacon.** That is the safety property the whole system
is built around; this tier is its first real exercise.

| # | Test | Validates | Tag | Harness | RED |
|---|---|---|---|---|---|
| F1 | Kill 1 member (10 live ≥ t=6) | quorum keeps signing; rolls still served | FLEET-NEW | BUILD-NEEDED (member-kill) | rolls fail |
| F2 | Kill to exactly t=6 | minimum viable still signs (ODC-036 threshold, live) | FLEET-NEW | member-kill | signing fails at t |
| F3 | ★★ Kill to t−1=5 | **FAIL-SAFE**: no signature possible → clean failure, quorum silent → idle → retirement. **NO wrong beacon** | FLEET-NEW | member-kill | any beacon produced below t |
| F4 | Kill all 11 | quorum dead → retirement → reform from pool | FLEET-NEW | member-kill | fleet wedges instead of reforming |
| F5 | Kill a member MID-CEREMONY | sub-11 completion (≥t) or clean abort-and-retry | FLEET-NEW | member-kill + boundary timing | partial/divergent finalization |
| F6 | ★ **Partition** a quorum (iptables/network split) | delivery under partition; ★ does ODC-055's catch-up recover on re-connect? | FLEET-NEW | BUILD-NEEDED (partition) | messages lost permanently after heal |
| F7 | Recovery: killed quorum reforms; rolls re-route (KDD-053 failover) | end-to-end self-healing | FLEET-NEW | member-kill + driver | rolls to a dead quorum hang |

---

## TIER 4 — LIFECYCLE (as quorums age past R=1440) ★ contains the highest-stakes drill

| # | Test | Validates | Tag | Harness | Safety |
|---|---|---|---|---|---|
| L1 | Rotation on schedule at rotation age | KDD-045 cadence live at L=8 | FLEET-NEW | exists (time) | SAFE (natural) |
| L2 | ★★ **GUARD 2 under genuine rotation contention** | **THE B-vs-A validation.** Force several quorums due at one boundary → does the fairness floor bound starvation? ★ **If it does not, KDD-079 REOPENS to Option A** — the decision's security claim rests here | FLEET-NEW | BUILD-NEEDED (age-forcing: either wait, or a disposable fleet with small R) | ★ DESTRUCTIVE (needs contrived R) |
| L3 | YIELD: terminal-eligible quorum yields its slot | KDD-075 precedence, live | FLEET-NEW | gate params | DESTRUCTIVE |
| L4 | REFORM under induced idleness | KDD-074 idle arm, live | FLEET-NEW | driver throttle | DESTRUCTIVE |
| L5 | Limiter drain order under multiple-eligible | one reform per window, LRA-first (IH row, live) | RE-RUN (unit exists) | driver + kills | DESTRUCTIVE |
| L6 | ★ Correlated departures across MULTIPLE quorums | the limiter serializes; ODC-054-adjacent | FLEET-NEW | member-kill | DESTRUCTIVE |

---

## TIER 5 — CONSENSUS VALIDATION (V-sequence, on-chain)

| # | Test | Validates | Tag | Harness | Safety |
|---|---|---|---|---|---|
| C1 | Malformed/invalid PTXDKG (bad group_pk, insufficient signers) | V1–V8 reject | RE-RUN (unit) | BUILD-NEEDED (tx-inject; `ptx_debug_ptxdkgpopulate` exists as a base) | DESTRUCTIVE-ish (rejected txs are harmless; use disposable) |
| C2 | Replay of a valid PTXDKG | duplicate-formation reject (ODC-030 guard) | RE-RUN | tx-inject | as above |
| C3 | Any-staker inclusion (non-member mines the PTXDKG) | KDD-058-A block-inject model, live | FLEET-NEW | exists | SAFE |
| C4 | Attestation counting (≥t attest → accepted) | KDD-059 semantics on-chain | RE-RUN | tx-inject | disposable |
| C5 | ★ **PTXPAYOUT accumulator-input validation** | **discovered live 2026-07-29 — see BUG-022** | FLEET-NEW | exists (natural) | SAFE (observational) |

---

## TIER 6 — DELIVERY UNDER STRESS (ODC-055's domain)

| # | Test | Validates | Tag | Harness |
|---|---|---|---|---|
| X1 | Rapid back-to-back formations | catch-up holds across consecutive ceremonies | FLEET-NEW | exists (short boundary interval on disposable) |
| X2 | ★ Member verifies VERY late (past the phase window) | correctly excluded → sub-11 but ≥t, clean; no divergence | FLEET-NEW | BUILD-NEEDED (delayed-start member) |
| X3 | Born-restricted relay under churn (members join/leave mid-ceremony) | relay restriction holds; no leak to non-members | FLEET-NEW | member-kill/restart |
| X4 | ★ **ODC-055 regression**: the "relay reached 0 peers → catch-up" path fires and recovers in **every** ceremony of the run | the fix stays fixed under all above | FLEET-NEW | exists (log grep — the unconditional lines) |

---

## TIER 7 — SOAK / SCALE (12h+, main-fleet-safe)

| # | Test | Validates | Harness |
|---|---|---|---|
| S1 | Memory / FD stability across the run (per-container RSS + fd counts, sampled) | no leaks under sustained ceremony+roll load | BUILD-NEEDED (sampler; small) |
| S2 | ★ Chain liveness + **root-disk headroom** over the run | ODC-049 class + the disk-hygiene constraint (root was 95% at h719) | BUILD-NEEDED (small) |
| S3 | Sustained demand for hours: distribution stays even, success rate ~100% | driver + D1 over duration | exists |
| S4 | ★ Many rotation cycles: does the fleet **hold at 8** through repeated rotate/reform, or drift? | lifecycle stability over duration — the long-run question | exists (fleet_watch) |

---

## TIER 8 — ADVERSARIAL / EDGE

| # | Test | Validates | Tag | Harness | Safety |
|---|---|---|---|---|---|
| A1 | Roll against a quorum mid-reform | re-routes or fails cleanly; **no hang** | FLEET-NEW | driver + kills | DESTRUCTIVE |
| A2 | Huge count/range | resource bounds; no OOM/DoS | RE-RUN | exists | SAFE |
| A3 | Simultaneous rolls from all callers | contention; no double-serve, no round-id collision | FLEET-NEW | exists | SAFE |
| A4 | ★ Caller-independence adversarial (D4 hardened: sustained grinding) | §7.4 INV-3 under effort | FLEET-NEW | exists | SAFE |
| A5 | ★★ **INSUFFICIENT FUNDS (CASE 2 — system-under-test, NOT the pre-flight)** | see below | FLEET-NEW | BUILD-NEEDED (a deliberately-drained caller) | SAFE |

### A5 detail — insufficient-funds behaviour (a real mainnet case, not a harness concern)
- **Assertions**: (a) roll **REJECTED cleanly** — clear error, no hang, no crash;
  (b) ★ **NO quorum resource consumed** — the ceremony/signing path must not be
  triggered for an unpayable roll (no wasted quorum work); (c) **no partial state** —
  no half-committed round, no orphaned round_id; (d) the error is **diagnostic**
  ("insufficient funds", not a generic failure); (e) **recoverable** — the same caller
  succeeds after top-up (the reject leaves no stuck state).
- ★ **Boundary**: exactly-enough (spendable == fee) → succeeds; one unit short → clean
  reject.
- **RED**: neuter the funds check → an underfunded roll consumes quorum work / hangs /
  leaves a round → assertions (b)/(c) fail.
- ★ **Distinct from P0.1**: P0.1 guards *the run*; A5 tests *the system*.

---

## HARNESS BUILD PREREQUISITES (the gating list)

| Build | Needed by | Size | Notes |
|---|---|---|---|
| **Suite manifest + runner** (declarative test list, per-test roll budget) | everything | M | Prereq for P0.1's budget computation |
| **Funding pre-flight** (`funding_preflight.py`) | P0.1 | S | spendable-only accounting, per-caller |
| **Member-kill** (stop/start named GMs, quorum-aware) | F1–F5, F7, L5, L6, X3, A1 | S | `docker stop` + harness wrappers; must relock on restart (BUG-019) |
| **Network partition** (iptables between container sets) | F6 | M | Most invasive; disposable fleet only |
| **Demand throttle/pause** (driver rate control at runtime) | D5, L4 | S | Driver flag + signal, or stop/restart with new gaps |
| **Tx injection** (invalid/replayed PTXDKG) | C1, C2, C4 | M | `ptx_debug_ptxdkgpopulate` is the base; needs invalid-variant builders |
| **BLS verify helper** (verify returned sig vs on-chain group_pk) | R10 | S | The single highest-value small build in the list |
| **Delayed-start member** (join a ceremony late) | X2 | S | Container start delay, boundary-timed |
| **Resource sampler** (RSS/fd/disk over time) | S1, S2 | S | Also useful outside the suite |

★ **Recommended build order** (cheapest evidence first): BLS verify helper + manifest/runner
+ funding pre-flight → **Tier 1 + Tier 2** (SAFE, main fleet, immediate re-validation value)
→ resource sampler → **Tier 7** (rides the existing soak) → member-kill → **Tier 3 + Tier 4
on a disposable fleet** (the fleet-unique value, including ★ L2/GUARD-2) → partition,
tx-inject → Tiers 5/6 remainder.

## Fleet policy — SAFE vs DESTRUCTIVE

- **Main fleet (N98, the soak/demo substrate)**: Tiers 0, 1, 2 (except D5), 7, and A2–A5.
  These are non-mutating: rolls, observation, and read-only checks.
- ★ **Disposable fleet required**: all of Tier 3, Tier 4 (L2–L6), D5, Tier 5's injection
  tests, and Tier 6's X1–X3. Killing members, partitioning, forcing idleness, and injecting
  invalid txs all permanently alter lifecycle state.
- ★ **The disposable fleet can be smaller and faster**: a second project (`ptx-w2b`, its own
  subnet/ports — the isolation discipline `gen_fleet` already enforces) at N=33/L=3 with a
  **short rotation interval** exercises L2's contention in minutes instead of days. That
  contrivance is legitimate *for lifecycle timing* — but the GUARD-2 verdict must be read
  with the R/B ratio noted, since Guard 2's whole point is behaviour as L approaches R/B.

---

## ★ BUG-022 (discovered live during this design, 2026-07-29) — chain-stall from PTXPAYOUT

Not part of the suite's design intent — found while diagnosing a live stall, recorded here
because **C5 exists because of it**:

```
h719, 04:06:00Z
ERROR: CheckAndApplyPTXPayout: PTXPAYOUT input <txid>:0 != accumulator <other>:0
ERROR: ConnectBlock: Special tx processing failed with ptxpayout-wrong-input
ThreadStakeMinter() exception → ThreadStakeMinter exiting
```

**What happened**: gm01 built a block containing a PTXPAYOUT whose input did not match the
expected accumulator UTXO; `ConnectBlock` rejected its own candidate; the **staker thread
took the exception and EXITED**, and with the fleet's only active producer gone the chain
froze at h719 (~2.5h). ★ Two distinct issues: **(1) the PTXPAYOUT input/accumulator
mismatch** (correctness — the payout builder and the accumulator view disagreed, plausibly
a race between accumulator spend and payout construction under sustained roll demand), and
**(2) a staker thread that dies permanently on a single block-construction exception**
(liveness — one bad candidate should skip, not terminate the producer). Issue (2) is
arguably the more serious: it converts any transient construction fault into a fleet-wide
halt, and it is the same *shape* as ODC-050's "fail-safe must not become fail-dead".

**Status**: gm01 restarted + relocked; recovery observed. Full register write-up owed
(proposed **BUG-022**, and issue (2) may warrant its own ODC given the liveness class).
This is the **second** defect the fleet has surfaced that no unit test could reach.
