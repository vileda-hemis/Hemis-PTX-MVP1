# DKG Implementation Plan — v1

**Status:** Approved. Decisions recorded; build order set. No W1.2 code until W1.1 and
W3.1-Test1 gates pass.
**Authored:** 2026-06-03
**Peer of:** `DKG_DESIGN_DOC_v1.md` (decisions), `ODC_022_DESIGN_DOC_v3.md` (lottery pool)
**Depends on:** `DKG_DESIGN_DOC_v1.md §9.4` (resolved reuse question — scopes the three build items)
**Register entries:** IMP-D1 through IMP-D4 (new items surfaced by this plan; see §7 and §8)

---

## §1 Build context — what exists, what must be built

This plan is grounded in the §9.4 source investigation. All file:line references are checkable.

### §1.1 What already exists and is exercised

| Asset | Location | Relevance |
|---|---|---|
| Shamir polynomial share generation | `ptx_bls.cpp:22` (`PTX_BLS_Init`) | Same arithmetic the DKG ceremony performs; trusted-dealer path is the test-vector generator |
| Per-GM partial signing (blst) | `ptx_bls.cpp:121` (`PTX_BLS_PartialSign`) | **UNCHANGED by DKG** — shares are the same 32-byte blst_scalar format regardless of how they were produced |
| Lagrange recovery in G2 | `ptx_bls.cpp:146` (`PTX_BLS_Recover`) | **UNCHANGED by DKG** |
| Group-pk verification | `ptx_bls.cpp` (`PTX_BLS_Verify`) | **UNCHANGED** — takes a blst_p1_affine; DKG just produces this without a trusted dealer |
| LLMQ DKG reference (5-phase P2P ceremony) | `src/llmq/quorums_dkgsession*` | Active, threaded, P2P-wired; the structural pattern to adapt |
| LLMQ recovery reference | `quorums_signing_shares.cpp:603` (`recoveredSig.Recover`) | Live cross-check target for differential testing |
| PoSe tracker | `ptx_pose.h:12` (`PTXPoSeTracker`) | Exists; needs extension for quorum-membership signing-window counters |
| Lottery ticket ledger | `ptx_pose.h:16` (`lottery_tickets`) | Exists in PoSe record; lifecycle wiring to be added in W2 |

### §1.2 What does NOT exist and must be built

| Gap | Built in |
|---|---|
| DKG ceremony over blst substrate | W1 |
| Bridge: ceremony output → `PTX_BLS_PartialSign` / `Verify` | W1 |
| PTXDKG special transaction (on-chain group_pk + vvec hash) | W1 |
| `g_ptx_bls_state` global singleton → per-quorum registry | W1/W2 boundary |
| Quorum state machine (FORMING / ACTIVE / ROTATING / DISBANDED) | W2 |
| Waiting pool | W2 |
| Formation trigger (pool-availability-triggered, batch-of-11) | W2 |
| Rotation trigger (interval + per-quorum drift offset) | W2 |
| Disband (30-block inquorate counter → dissolve to pool) | W2 |
| Ejection (15-of-60 missed signings) | W2 |
| Multi-quorum router (ptx_roll → active quorum) | W2 |
| `PTX_AssignQuorum` (per-roll ephemeral) → route to pre-formed quorum | W2 |

### §1.3 The bridge — the key architectural finding

The trusted-dealer flow today:

1. **Coordinator** runs `PTX_BLS_Init` → generates `master_sk`, `group_pk`, and `shares[i]`
2. **Coordinator** runs `PTX_FanOutKeySet` → sends each GM its share (32-byte scalar) over HTTP
3. **Per roll** — `PTX_BLS_PartialSign`, `PTX_BLS_Recover`, `PTX_BLS_Verify` unchanged

With DKG, only steps 1 and 2 change:

1. **Each GM** participates in the ceremony → each ends with its own `sk_share_i`; no party
   ever holds `master_sk`
2. **Each GM** stores `sk_share_i` in the same 32-byte blst_scalar format as today; the
   `gm_bls_sign` RPC handler is unchanged
3. **Per roll** — the signing path is **identical** — `PTX_BLS_PartialSign`, `PTX_BLS_Recover`,
   `PTX_BLS_Verify` are not modified

The bridge is narrow: replace `PTX_BLS_Init + PTX_FanOutKeySet` with a ceremony that delivers
the same artefacts. Every line of code downstream of key distribution is preserved.

---

## §2 Workstream 1 — DKG ceremony + PTX-key bridge

**Goal:** replace `PTX_BLS_Init` with a distributed ceremony where no party holds `master_sk`,
producing the same artefacts the signing path already consumes — without modifying
`PTX_BLS_PartialSign`, `PTX_BLS_Recover`, or `PTX_BLS_Verify`.

**Dependency:** none — W1 is the prerequisite for everything else.

---

### W1.1 — Pre-implementation gates (decisions + coordinator-role definition)

W1.2 (ceremony code) is BLOCKED until all four items below are resolved. Three are now
decided (recorded here); one requires a written spec deliverable.

---

#### IMP-D1 — BLS substrate for the ceremony: blst — DECIDED 2026-06-03

**Decision:** the DKG ceremony arithmetic is implemented in blst. The LLMQ DKG
(`src/llmq/quorums_dkgsession*`, chiabls) is used as a structural reference only — the
5-phase pattern, message types, complaint and justification logic — but the arithmetic
(polynomial generation, verification vector computation, share encryption, commitment
aggregation) is re-expressed in blst from scratch.

**Rationale:** option (b) — running the ceremony in chiabls and extracting scalars to blst
at the bridge — places a cross-library scalar-representation seam directly in the key
material. This is exactly the silent-keying-bug class that §6 warns about as the
highest-risk item: a ceremony that produces shares which verify in isolation but fail
cross-library, with no observable signing failure. One blst codebase eliminates that seam
entirely and is auditable as a single thing. The extra build work is accepted; no ambiguity
in the keying is worth the saving.

**Consequence for the LLMQ reference:** use `CDKGSession` and `CDKGSessionHandler` as
structural references (phase sequencing, message relay logic, bad-member handling patterns)
but do not call chiabls functions in the PTX ceremony path. The ceremony code lives in
`src/ptx/`, isolated from `src/llmq/` and `src/bls/`.

---

#### IMP-D2 — Ceremony transport: P2P — DECIDED 2026-06-03

**Decision:** ceremony messages (contribution, complaint, justification, premature commitment)
are gossiped over the existing P2P mesh, adapting the LLMQ message type infrastructure
(QCONTRIB / QCOMPLAINT / QJUSTIFICATION / QPCOMMITMENT or PTX-namespaced equivalents).
The existing HTTP fanout (`ptx_fanout.cpp`) is not used for the ceremony.

**Rationale:** a public decentralised network cannot depend on all GMs being reachable by a
single coordinator over HTTP. P2P gossip is the correct architecture for a network where any
GM can be behind NAT, and where the coordinator's HTTP access to all GMs is not guaranteed.
The extra work over HTTP fanout is accepted.

**Implementation note:** the existing LLMQ P2P dispatch infrastructure
(`quorums_dkgsessionhandler.cpp:131–142`, `net_processing.cpp`) can be reused or adapted.
New PTX-DKG message types (e.g., `PTXQCONTRIB`, `PTXQCOMPLAINT`) should be namespaced
separately from LLMQ to avoid routing conflicts, since LLMQ DKG continues running for
ChainLocks in parallel.

---

#### IMP-D3 — On-chain ceremony record: PTXDKG special transaction — DECIDED 2026-06-03

**Decision:** after the ceremony, a new special transaction type **PTXDKG** is mined. It
commits: the quorum's `group_pk` (blst_p1_affine, 48 bytes compressed G1), the vvec hash,
the member set (proTxHash list), and the formation height. Validity requires ≥ t valid
signed premature commitments from the named members.

> **[Reconciled 2026-06-03 per KDD-052]** The PTXDKG record commits the `node_id` member set
> (not a `proTxHash` list). The DGM list carries both identities; `proTxHash` is used only as
> the internal ordering key (chain-determined score), resolved at validation. "proTxHash list"
> in the original IMP-D3 text is superseded by `node_id`.

**Rationale:** an off-chain `group_pk` reintroduces coordinator trust — any observer that
did not witness the ceremony must trust the coordinator's announcement of `group_pk`. This
directly contradicts KDD-039 (no operator-trusted network). On-chain is required.

**Risk note:** the PTXDKG consensus-validation path is the second-highest-risk item (§6).
Incorrect validation logic can produce a chain split. It must be in the audit scope (W3.2)
and reviewed with the same rigour as the ceremony itself. This is non-negotiable.

---

#### Coordinator role definition — REQUIRED DELIVERABLE of W1.1

**This is the fourth W1.1 gate, and the only one that produces new written output.** The plan
references a "coordinator" in several places (Phase-4-offline recovery, W2.2 formation
trigger, the bridge reading `group_pk`). DKG's purpose is to eliminate the privileged
coordinator SPOF (KDD-039, ODC-021 resolved-by-DKG). Before writing ceremony code, the
coordinator's residual role must be precisely defined in writing — because if the
implementation retains a coordinator that holds key material or that others must trust for
correctness, the result is a dressed-up trusted dealer, not DKG.

**Required answer for the trustless property:**

The coordinator role after DKG is **stateless and un-privileged**:

- **No key material.** The coordinator never holds `master_sk` (it doesn't exist as a single
  value) and never holds any GM's `sk_share_i` (each GM derives and holds its own share from
  the ceremony). Losing the coordinator process loses zero key material.
- **No trust requirement for correctness.** The coordinator does not decide which members'
  contributions are valid (GMs verify each other's contributions during the ceremony). The
  coordinator does not decide the final `group_pk` (it is the deterministic result of the
  aggregated premature commitments, verifiable by any observer from the PTXDKG transaction).
- **Residual role: un-privileged relay and roll initiator.** The coordinator's remaining
  functions are: (a) detecting formation conditions and broadcasting a formation proposal
  (any node can do this — it is not a privileged act; consensus is established through the
  ceremony, not through the proposal); (b) collecting partial signatures for `ptx_roll` and
  running Lagrange recovery (any node with RPC access to the quorum's GMs and the on-chain
  `group_pk` can do this); (c) submitting the settlement transaction.
- **Coordinator is replaceable.** Any operator can stand up a new coordinator process,
  read `group_pk` from the chain, and begin signing rolls. There is no coordinator state
  that must be recovered or transferred.

**The falsifiable test for this property:** if the coordinator process crashes after the
ceremony completes, can another process take over roll signing without any key-material
transfer from the first process? The answer must be yes — this is a required measured test
at the W1.2 validation gate, not a design claim. If the test fails, the implementation has
retained a keying SPOF and W1.2 is not done.

**Deliverable:** a one-page written coordinator-role spec (peer of this document, in
`doc/ptx/`) before any W1.2 ceremony code is written.

**[COMPLETE 2026-06-03]** Delivered as `doc/ptx/COORDINATOR_ROLE_SPEC_v1.md`. Defines "no
coordinator as a protocol entity" via two invariants — I1 (no SPOF) and I2 (no
protocol-privileged party). I2 resolved descriptive against source: `gm_bls_sign`
(`rpc/ptx.cpp:484–529`) has no requester authentication, only transport-level HTTP Basic —
the property holds structurally today, no handler change required for W1.2. All four W1.1
gates now closed (IMP-D1/D2/D3 decided; W3.1-Test1 PASS; coordinator-role spec written) →
**W1.2 is unblocked.**

---

### W1.2 — Ceremony implementation (single quorum, static fleet, no rotation)

**BLOCKED on: W1.1 all four items complete (IMP-D1/D2/D3 recorded; coordinator-role spec
written; W3.1-Test1 PASS). ✅ ALL FOUR GATES CLOSED 2026-06-03 — W1.2 UNBLOCKED.**

**Scope:** a single quorum of 11 GMs runs the DKG ceremony and produces the artefacts the
existing signing path consumes. The trusted-dealer `PTX_BLS_Init` and `PTX_FanOutKeySet` are
gated off for DKG-formed quorums. `PTX_BLS_PartialSign`, `PTX_BLS_Recover`, and
`PTX_BLS_Verify` are NOT modified.

**Five-phase ceremony (blst substrate; LLMQ as structural reference):**

- **Phase 1 — Contribution:** each GM generates a random polynomial of degree `t-1 = 5`
  over Zr (blst scalar arithmetic), computes a verification vector (t G1 points, one per
  coefficient), and sends an encrypted secret contribution — its polynomial evaluated at
  each other member's index — to each of the other 10 GMs via P2P.
- **Phase 2 — Complaint:** each GM verifies received contributions against the sender's
  verification vector. Failed verification → broadcast a complaint.
- **Phase 3 — Justification:** a complained-about GM broadcasts its original plaintext
  contribution. Cannot justify → declared bad, excluded.
- **Phase 4 — Commitment:** each GM computes `sk_share_i` = sum of valid contributions at
  index i, computes the combined `group_pk` = sum of valid members' vvec[0] in G1, and
  broadcasts a signed premature commitment.
- **Phase 5 — Finalize:** once ≥ t consistent premature commitments are received, the
  ceremony is complete. Each GM stores its `sk_share_i` in the same 32-byte blst_scalar
  format as `gm_bls_keyset` writes today. The PTXDKG transaction is constructed and
  submitted.

**`g_ptx_bls_state` at W1.2:** still serves one quorum only. All ceremony code accesses it
through an accessor function (never the global directly), preparing for the W2.1 refactor
to a per-quorum registry.

**W1.2 minimal measurable milestone:**
- 11 GMs complete the full ceremony without manual intervention.
- Each GM holds an `sk_share_i` it did not receive from any coordinator.
- `group_pk` is on-chain in a PTXDKG transaction, verifiable by any node.
- One `ptx_roll` succeeds end-to-end using DKG-produced keys; `PTX_BLS_Verify` passes.
- **Coordinator-replacement test:** crash the coordinator after ceremony; a second process
  starts cold, reads `group_pk` from chain, signs a roll. No key-material transfer. This
  test must be measured, not reasoned about.

**Validation gate (measured):**
- 100 consecutive rolls with DKG-produced quorum. Zero failures.
- Coordinator-replacement test passes.
- W3.1-Test2 differential test passes (gate for W1.3).
- Ceremony duration measured in wall-clock time and blocks per phase (ODC-025 input).
  **[KDD-051]** Measurement must cover the GJKR-hardened ceremony (commit phase + one added
  gossip round), not plain Feldman. Benchmarking the unhardened ceremony would measure
  rotation-N against a construction PTX will not ship.

**[KDD-052, W1.2 carry-forwards]**

1. **Share-index assignment changes.** `PTX_BLS_Init` currently assigns share index by
   alphabetical `node_id` sort (`ptx_bls.cpp:32–35`). W1.2 replaces this with
   chain-determined score order (`SHA256(SHA256(proTxHash, confirmedHash), formation_block_hash)`
   descending, 1-indexed). The W3.1-Test1 subset-exhaustion test is unaffected by the basis
   (it tests subset-consistency for a given assignment) but should be re-run after the change
   to confirm.
2. **`confirmedHash` precondition.** The ordering score uses `confirmedHash`; a member must
   be confirmed at formation height or the score input is null and ordering is undefined. PTX
   quorum formation inherits the LLMQ "members must be confirmed" eligibility rule. Confirm
   enforced in W1.2.

---

### W1.3 — PTXDKG consensus validation + bridge hardening + abort handling

**BLOCKED on: W3.1-Test2 PASS gate.**

**Status (2026-06-13): Spec approved — doc/ptx/W1.3_VALIDATION_SPEC_v1.md (KDD-060, ODC-030). Implementation pending.**

#### Validation design (KDD-058 + KDD-059, decided 2026-06-12; membership predicate amended by KDD-060, 2026-06-13 — see W1.3_VALIDATION_SPEC_v1.md §2)

ODC-029 RESOLVED → KDD-058: direct block-inject (LLMQCOMM precedent). Wiring
(tx_verify vin/vout exemption for IsPTXDKGTx, mempool rejection, blockassembler
GetMinablePTXDKGTx hook) is W1.3, sequenced AFTER the validation design.

Validation semantics → KDD-059: attestation-counting (≥ t registered members signed
agreement on group_pk). NOT artifact-only — needs DGM-registry lookup for member
pubkeys; CheckPTXDKGTx bifurcates on pindexPrev, gains cs_main. Boundary: accountability
not correctness; share-correctness (ODC-027) and threshold-recovery (ODC-028) deferred
to W3.2 audit.

Recon findings (a9a4ab7):
- member_node_ids is cosmetic for consensus — identity keys off proTxHash + DGM lookup.
  node_id is registration data (label:8hex, suffix=SHA256(collateralOutpoint)[0:4]),
  recoverable from DGM state but redundant given proTxHash+sig.
- INTEGRATION SEAM (latent bug, flag for integration arc): rpc/ptx.cpp Lagrange recovery
  resolves indices from g_ptx_bls_state.node_index, built by PTX_BLS_Init via ALPHABETICAL
  node_id sort (trusted-dealer path). DKG assigns share_index by SCORE order (SortMembers,
  KDD-052). Two different index spaces. When DKG replaces the trusted dealer, the recovery
  path's index source MUST switch from alphabetical-node_id to score-order share_index, or
  partial sigs recover under wrong evaluation points. P5 end-to-end passes because the
  ceremony's indices are internally self-consistent; the seam is the rpc recovery path's
  use of g_ptx_bls_state.

Open items: ODC-029 resolved (KDD-058). Still open: ODC-027 (full-vvec, W3.2),
ODC-028 (recovery artifact, W3.2), the index seam (integration), W1.3 validation
implementation, §C1 replay guard (both write sites).

#### Remaining W1.3 scope

- Wire `PTX_BLS_Verify` to use the on-chain `group_pk` from the PTXDKG record, not only
  in-memory state (required for nodes that did not observe the ceremony directly).
- Ceremony-abort handling: if fewer than t valid premature commitments are received, abort
  cleanly; return all GMs to pool. Ceremony is fully complete or fully aborted — no partial
  state persists.
- Replay protection: a quorum that has completed a ceremony cannot be re-keyed without an
  explicit rotation or disband trigger. This permanently closes the silent-overwrite
  vulnerability noted in `PTX_LE_STANDUP.md §C1` (`gm_bls_keyset` unconditional overwrite).
  BOTH write sites must be covered: `gm_bls_keyset` RPC AND `PTX_DKG_StoreSkShare` (KDD-057).
- **Ceremony-side KDD-060 rebase (spec §5):** `PTX_DKG_InitSession` contract change —
  caller supplies members in `CalculateQuorum` output order; InitSession removes the
  `SortMembers` call (line 156) and never re-sorts. Retire `PTX_DKG_ComputeMemberScore`
  (ptx_dkg.cpp:107–118) and `PTX_DKG_SortMembers` (ptx_dkg.cpp:121–145) entirely —
  removed, not quarantined: `stable_sort`/no-tiebreak vs `std::sort`/collateral-tiebreak
  diverge under score ties, making quarantine wrong, not just untidy (KDD-060 §20.3).
- **`PTX_DKG_BuildMemberVector` helper (spec §5.2):** thin wrapper:
  `GetListForBlock(pindexQuorum)` + `CalculateQuorum(11, pindexQuorum->GetBlockHash())`,
  maps each `CDeterministicGMCPtr` → `PTXDKGMember`. The only new selection code; lands
  in W1.3 so the canonical-ordering contract is executable and testable. W2 formation
  calls it; the validator does not (validator needs only the proTxHash set).
- **Phase0 test migration (spec §5.4):** T0-1/2/4/5/6 migrate to `CalculateQuorum`/
  `CalculateScores`-derived expectations; T0-6 (null-confirmedHash rejection) migrates
  to the InitSession precondition assert. slot+1 assertions (phase0:225–227,
  phase2:278–285) remain valid — assignment formula `members[i].share_index = i + 1`
  is unchanged. Migration lands in the W1.3 implementation commits, not a separate
  cleanup commit.
- **§C1 interim guard shape (spec §5.5):** both write sites of `g_ptx_my_bls_sk_bytes`
  gain refuse-silent-overwrite guards — `PTX_DKG_StoreSkShare` refuses if a key is set
  and the session's quorum_hash matches stored provenance (quorum_hash stored alongside
  the key bytes as provenance at write time); `gm_bls_keyset` RPC gains an
  explicit `force` flag. W2 trigger semantics are deferred; the guard exists in W1.3.
- **Block-inject wiring sequencing (spec §4):** the wiring commit (`IsPTXDKGTx()` vin/vout
  exemption, mempool rejection, `GetMinablePTXDKGTx` hook) lands in the SAME commit as
  or AFTER the validation commit — never before. KDD-058/059 coupling: injection is
  I2-safe only once `CheckPTXDKGTx` rejects wrong results.
- **One-PTXDKG-per-block rule (spec §4.4):** block-level structural check — at most one
  PTXDKG tx per block (cheap check in the block special-tx processing path). Cross-block
  per-formation uniqueness is ODC-030 (DKG_DESIGN_DOC_v1 §9.9), deferred to W2.

**Validation gate (measured):**
- Regtest: inject a bad member (invalid contribution). Quorum completes with member excluded;
  ≥ t valid members remain; `group_pk` reflects only valid members.
- Regtest: force < t members to reach Phase 4. Ceremony aborts; all 11 GMs return to pool;
  no partial key material persists.

---

## §3 Workstream 2 — Rotation and lifecycle machinery

**Goal:** quorum state machine, pool, formation, rotation, disband, ejection, and multi-quorum
router. All net-new — no lineage machinery exists.

**Dependency: W1.2 and W1.3 complete.**

---

### W2.1 — Quorum registry

**`PTXQuorumRecord`** — persistent per-quorum state:
```
quorum_id                    : uint256 (hash of PTXDKG commitment)
members                      : vector<string> (11 node_ids)  // KDD-052: on-chain PTXDKG record commits this node_id set, ordered by chain-determined score (not alphabetical)
group_pk                     : blst_p1_affine
formation_height             : int
drift_offset                 : int (unique per quorum)
last_rotation_height         : int
state                        : enum { FORMING, ACTIVE, ROTATING, DISBANDED }
consecutive_inquorate_blocks : int
```

The global `g_ptx_bls_state` singleton (`ptx_bls.h:41`) becomes a
`std::map<uint256, PTXQuorumRecord>` keyed by `quorum_id`. `PTX_AssignQuorum`
(`ptx_quorum.cpp:67`) — currently per-roll ephemeral selection from `g_ptx_nodes` — is
replaced by a router selecting from ACTIVE-state quorums, deterministically seeded by
`round_seed`.

**Validation gate:** form two quorums in regtest; observe both registry entries; both sign
rolls independently; router skips the FORMING quorum.

---

### W2.2 — Formation trigger

Pool-availability-triggered formation: when `pool.size() >= 11` (registered PoSe-valid GMs
not in any quorum), a new quorum may form. Batch selection is deterministic Fisher-Yates
over the eligible pool using the triggering block hash as entropy.

Drift assignment: each quorum receives a `drift_offset` in `[0, N-1]` derived from the
block hash, verified unique across all active quorums. **IMP-D4 (collision strategy):
decided at implementation — retry-with-increment is acceptable; not security-critical.**

**Validation gate:** 22 nodes in regtest; two quorums form sequentially; drift offsets
differ; formation does not fire when pool < 11; PoSe-ineligible nodes excluded from pool.

---

### W2.3 — Rotation trigger

At `height = last_rotation_height + N + drift_offset`, the quorum transitions
ACTIVE → ROTATING and re-runs the DKG ceremony with its existing 11 members. Old
`sk_share_i` values are replaced; `group_pk` changes. Router skips ROTATING quorums.
Only one quorum should be ROTATING at any time; collisions defer by a fixed increment.

**ODC-025 decision point:** after the first completed rotation in regtest, measure ceremony
block duration and validate `rotation_spacing ≈ N / quorum_count ≈ 39 blocks > ceremony_duration`.
Confirm N=1440 or revise. Close ODC-025 with the measured value.

> **[2026-06-03, KDD-051]** The ceremony-duration benchmark gating rotation-N MUST run on the
> GJKR-hardened ceremony (commit phase + one added gossip round included), not plain Feldman.
> Benchmarking the unhardened ceremony would measure rotation-N against a construction PTX will
> not ship.

**Validation gate (measured):**
- In regtest with small N (e.g., 60 blocks): ACTIVE → ROTATING → ACTIVE with new `group_pk`.
- Router skips ROTATING quorum during ceremony.
- New `group_pk` differs from old and verifies correctly.
- Ceremony block duration measured and reported to ODC-025. **[KDD-051]** Measurement must
  cover the GJKR-hardened ceremony — commit phase included.

---

### W2.4 — Disband and ejection

**Ejection (KDD-046):** a GM missing 15 of its last 60 signing opportunities is ejected.
Extend `PTXPoSeTracker` (`ptx_pose.h:20`) with a per-GM rolling 60-opportunity window
(distinct from `pose_score`). On ejection: GM returns to pool; if remaining quorum has
< t=6 signing-capable members, `consecutive_inquorate_blocks` counter starts.

**Disband (KDD-047):** at `consecutive_inquorate_blocks = 30`:
- All surviving members return to pool; lottery tickets intact (`ptx_pose.h:16`).
- `PTXQuorumRecord` marked DISBANDED; `group_pk` and shares cleared.
- Pool may trigger a new formation (W2.2).

**Validation gate (measured — anti-false-trigger and true-trigger both measured):**
- 5 of 11 GMs offline 30+ blocks: disband fires at exactly block 30.
- GMs return at block 29: counter resets, disband does NOT fire.
- 14 missed signings: no ejection. 15: ejection fires.
- Post-disband: surviving members' lottery tickets preserved.

---

## §4 Workstream 3 — Validation (first-class; runs alongside W1 and W2)

**W3 starts at W1.2, not after.** Validation that starts after the build is not
validation — it is hoping. The measured-beats-reasoned discipline from ptx-bea B2 applies
with higher stakes here than anywhere else in the codebase.

---

### W3.1 — Differential test harness

**Test 1 — Known-polynomial baseline (start NOW, before W1.2):**
- Generate Shamir shares via `PTX_BLS_Init` with hardcoded test-vector polynomials (all
  coefficients fixed; all edge cases: t=6, t=1, t=n; all 11 shares; exactly t shares;
  every possible subset of t from n).
- Sign a known message with each share via `PTX_BLS_PartialSign`.
- Recover via `PTX_BLS_Recover` → verify via `PTX_BLS_Verify`. PASS required.
- **[Superseded 2026-06-03 per IMP-D5 / KDD-050: chiabls oracle step removed. Test 1 is a
  same-stack subset exhaustion — all C(n,t) subsets recover to byte-identical group signatures,
  each passing PTX_BLS_Verify. chiabls rejected: RELIC unreviewed + DST mismatch.]**
- **Gate: all C(n,t) subsets produce byte-identical recovered_sig, each passing PTX_BLS_Verify.
  PASS confirmed: commit bcb4222 (2026-06-03).**

**Test 2 — DKG-produced shares (immediately after W1.2 milestone):**
- Extract `sk_share_i` values from a regtest ceremony (test-mode accessor).
- Differential verification: PTX Lagrange vs chiabls Recover across all 462 possible
  6-of-11 subsets.
- **Gate: zero discrepancies. This is the hard gate blocking W1.3.**
- **[Superseded 2026-06-03 per IMP-D5 / KDD-050: chiabls removed as oracle. Test 2 is
  the same-stack subset check as Test 1, differing only in share provenance —
  ceremony-produced shares via the KDD-050 compile-gated accessor (ENABLE_PTX_TEST_ACCESSORS).
  chiabls rejected: RELIC backend carrying upstream "NOT YET FORMALLY REVIEWED FOR SECURITY"
  disclaimer; DST mismatch (BLS_SIG_HEMIS_PTX_... vs BLS_SIG_BLS12381G2_...) makes
  byte comparison non-viable regardless. Gate criterion unchanged: all C(n,t) subsets
  recover to byte-identical group signatures, each passing PTX_BLS_Verify.]**

**Test 3 — Corpus (throughout W2 and into the audit window):**
- 10,000 randomly-generated ceremonies (random polynomials, random valid/invalid member sets).
- Each: full differential verification.
- Gate: zero failures across the corpus.

The differential test is non-negotiable. It is the primary protection against a keying
bug in the ceremony's blst-substrate arithmetic. Code review will not reliably catch a
ceremony that produces shares which verify locally but disagree with an honest reference.

---

### W3.2 — External cryptographic audit (inquiry starts NOW — parallel action)

**Highest lead-time item in the plan.** Firms book months out.

**Audit scope:**
- The DKG ceremony implementation: all five phases, complaint/justification logic, premature
  commitment aggregation, final commitment construction.
- The PTX-key bridge: path from ceremony output to `PTX_BLS_PartialSign` / `PTX_BLS_Verify`.
- PTXDKG special transaction validation (consensus-critical; chain-split risk if wrong).
- The coordinator-role spec (W1.1 deliverable): confirm implementation matches the stated
  no-privilege, no-key-material spec.
- Any deviation from the decided Feldman VSS + GJKR commit-then-reveal construction (KDD-051).
  *(Corrected from "standard Pedersen VSS" per KDD-051, 2026-06-03.)*

**Out of scope:** blst library (audited by supranational); chiabls (not modified); Lagrange
recovery implementation (in production for trusted-dealer keying — treat as established).

**Engagement timeline:**
- **Now:** identify firms (Trail of Bits, NCC Group, Kudelski Security, Least Authority,
  Zellic). Draft one-page scope brief from this plan. Begin inquiry; get availability and
  lead-time estimates.
- **At W1.3 stable:** hand off ceremony + bridge + PTXDKG code.
- **Hard gate:** no public testnet launch until audit complete and findings addressed.

Record engagement in `PTX_LE_STANDUP.md` once a firm is confirmed.

---

### W3.3 — Regtest lifecycle stress scenarios (after W2.4)

| Scenario | Exercises |
|---|---|
| One GM offline during Phase 1 (no contribution) | Ceremony completes with 10 valid members (≥ t); bad-member flag set |
| One GM sends invalid contribution (bad vvec) | Complaint fires; justification fails; GM excluded |
| One GM double-sends (equivocates) | Both contributions relayed; member excluded (LLMQ pattern) |
| Coordinator offline after Phase 4 | Second coordinator aggregates commitments; ceremony completes |
| 6 GMs offline during rotation | Rotation aborts; quorum stays ACTIVE on old key until retry |
| Formation with exactly 11 in pool (minimum viable) | Quorum forms; roll succeeds |
| Formation with 10 in pool (one short) | Formation does not fire; correct error |
| Two quorums: one ROTATING, one ACTIVE; ptx_roll issued | Router skips ROTATING, routes to ACTIVE |
| Disband + pool ≥ 11 immediately after | Disband fires; survivors absorbed; new formation triggers |

**Gate:** every scenario produces the expected outcome at the expected chain height, measured
in regtest. Any outcome that must be reasoned about rather than observed is a gap — close it
with a concrete observable.

---

## §5 Build order

```
IMMEDIATE PARALLEL — no code dependency:
  W3.2 audit inquiry  ─── start today
  W3.1-Test1          ─── DONE — commit bcb4222 (same-stack subset exhaustion;
                          PTX_BLS_Init + PTX_BLS_PartialSign + PTX_BLS_Recover +
                          PTX_BLS_Verify; no chiabls oracle — IMP-D5)

W1.1 ──────────────────────────────────────────────────────────────────────────────
  Deliverable: coordinator-role spec (written doc, one page, peer of this plan)
  IMP-D1/D2/D3 already decided (recorded above) — no further blocking on those
  GATE: coordinator-role spec written + W3.1-Test1 PASS

W1.2 ──────────────────────────────────────────────────────────────────────────────
  Ceremony (blst, P2P, PTXDKG tx); single quorum; no rotation
  BLOCKED on: W1.1 gate
  GATE: 100 rolls pass + coordinator-replacement test + W3.1-Test2 PASS

W1.3 ──────────────────────────────────────────────────────────────────────────────
  Bridge hardening; abort handling; replay protection
  BLOCKED on: W1.2 gate (W3.1-Test2 must pass before proceeding)
  GATE: abort + bad-member scenarios measured in regtest

W3.2 audit handoff ────────────────────────────────────────────────────────────────
  Hand off W1.2 + W1.3 code when W1.3 gate passes
  Inquiry already running; handoff is the code-dependent step

W2.1 ──────────────────────────────────────────────────────────────────────────────
  Quorum registry; g_ptx_bls_state → per-quorum map; router
  BLOCKED on: W1.3

W2.2 ──────────────────────────────────────────────────────────────────────────────
  Formation trigger; pool; drift
  BLOCKED on: W2.1

W2.3 ──────────────────────────────────────────────────────────────────────────────
  Rotation trigger; same-set re-DKG; ODC-025 benchmark
  BLOCKED on: W2.2

W2.4 ──────────────────────────────────────────────────────────────────────────────
  Disband; ejection; ticket preservation
  BLOCKED on: W2.3

W3.3 ──────────────────────────────────────────────────────────────────────────────
  Lifecycle stress scenarios; full regtest suite
  BLOCKED on: W2.4

LAUNCH GATE: W3.3 all scenarios pass + W3.2 audit complete + findings addressed
```

Critical path: W3.1-Test1 + W1.1 → W1.2 → W3.1-Test2 → W1.3 → W2.1 → W2.2 → W2.3 →
W2.4 → W3.3 → audit complete. The audit inquiry is the only critical-path item that does
not depend on code — it starts today.

---

## §6 Highest-risk items

**Risk 1 (highest): ceremony-orchestration correctness on the blst substrate.**
Implementing Feldman VSS + GJKR commit-then-reveal hardening (KDD-051) in blst without
introducing a keying bug. The LLMQ reference is plain Feldman; PTX adds the GJKR
commit-then-reveal phase. The risk is not in the primitives (blst audited; `PTX_BLS_Recover`
in production). The risk is in the orchestration: the commit-then-reveal phase ordering,
contribution aggregation, complaint and justification handling, final commitment computation.
A bug here can produce a `group_pk` + `sk_share_i` set that signs correctly in isolation but
violates the VSS property — meaning `master_sk` is implicitly reconstructable by a party who
collected enough ceremony messages, with no observable signing failure.
*(Corrected from "Pedersen VSS from the LLMQ reference" per KDD-051, 2026-06-03.)*

Mitigation: W3.1-Test2 — same-stack subset-exhaustion differential on ceremony-produced
shares, no cross-stack oracle (IMP-D5; chiabls rejected as oracle). This is why W3.1-Test2
is a hard gate before W1.3 — the test must pass before any code builds on the ceremony
output.

**Risk 2: PTXDKG special transaction validation.**
Consensus-critical. Incorrect validation logic (wrong threshold check, wrong member set
validation, wrong `group_pk` aggregation verification) → chain split. Mitigation: in audit
scope; separate regtest coverage of the full PTXDKG validation path; reviewed with same
rigour as the ceremony.

Both risks are tractable. Neither involves unknown cryptography. The mitigations are
specific and measurable.

---

## §7 Open items and decisions

| ID | Item | Status | Where resolved |
|---|---|---|---|
| **IMP-D1** | Ceremony BLS substrate | **Decided: blst** | This plan §2 W1.1 — 2026-06-03 |
| **IMP-D2** | Ceremony transport | **Decided: P2P** | This plan §2 W1.1 — 2026-06-03 |
| **IMP-D3** | On-chain ceremony record | **Decided: PTXDKG special tx** | This plan §2 W1.1 — 2026-06-03 |
| **IMP-D4** | Drift collision strategy | **Decided at implementation** | Retry-with-increment; not security-critical |
| **IMP-D5** | W3.1 differential oracle: same-stack subset exhaustion | **Decided: no cross-stack oracle** | chiabls rejected — RELIC unreviewed + DST mismatch; 2026-06-03 |
| **KDD-049** | PTX_BLS_Verify explicit group_pk parameter | **Decided** | Pure function; caller extracts under cs_ptx_bls; commit 66251c8 |
| **KDD-050** | Test extraction interface — ENABLE_PTX_TEST_ACCESSORS compile gate | **Decided** | New configure option, default off, modelled on ENABLE_WALLET; 2026-06-03 |
| **KDD-052** | PTXDKG member set — committed node_id list, chain-determined score order; resolves OPEN-2 | **Decided** | Design doc §12; 2026-06-03 |
| ODC-025 | Rotation-N final value | **Open — measured at W2.3** — [KDD-051] benchmark must run GJKR-hardened ceremony | Design doc §9.2 |
| ODC-024 | Multi-quorum membership | Deferred (partially resolved per KDD-053: selection + failover decided) | Design doc §9.3 |
| **KDD-053** | Multi-quorum roll selection (Option D) + failover asymmetry; partially resolves ODC-024 | **Decided** | Design doc §13; 2026-06-03 |
| Coordinator role | Residual coordinator spec | **✅ COMPLETE 2026-06-03** — `doc/ptx/COORDINATOR_ROLE_SPEC_v1.md` | W1.1 gate closed; W1.2 unblocked |

**IMP- series note:** IMP-D1 through IMP-D4 are implementation-decisions — a new series,
distinct from KDD (design-decisions) and ODC (open-design-choices). The series was
introduced by this plan. See the standup register table for the cross-reference entry; the
IMP- series must be tracked there to prevent ID reuse (the KDD-033..036 overload is the
cautionary example).

---

## §8 Immediate parallel actions — start today

1. **W3.2 audit inquiry.** Draft a one-page scope brief from §4 W3.2. Contact candidate
   firms: Trail of Bits, NCC Group, Kudelski Security, Least Authority, Zellic. Get
   availability and lead-time estimates. Record engagement in `PTX_LE_STANDUP.md` once
   confirmed. This is the item most likely to extend the 12–18-month mainnet timeline if
   not started early.

2. **W3.1-Test1.** ✅ DONE — commit bcb4222. Same-stack subset exhaustion using
   `PTX_BLS_Init` + `PTX_BLS_PartialSign` + `PTX_BLS_Recover` + `PTX_BLS_Verify`.
   No chiabls oracle (IMP-D5). W1.2 gate cleared on this item.

3. **W1.1 coordinator-role spec.** ✅ DONE — `doc/ptx/COORDINATOR_ROLE_SPEC_v1.md`
   (2026-06-03). Two-invariant framing (I1: no SPOF; I2: no protocol-privileged party). I2
   resolved descriptive. Falsifiable cold-caller test written. W1.2 unblocked.

---

## Appendix: Register cross-reference

| Entry | Series | Title | Status |
|---|---|---|---|
| KDD-039 | KDD | DKG before public testnet | Decided — design doc §2 |
| KDD-044 | KDD | Quorum formation — batch-of-11 | Decided — design doc §7.1 |
| KDD-045 | KDD | Quorum rotation — same-set re-DKG | Decided — design doc §7.2 |
| KDD-046 | KDD | Ejection — 15-of-60 | Decided — design doc §8 |
| KDD-047 | KDD | Disband — n_disband=30, dissolve-to-pool | Decided — design doc §7.3 |
| KDD-048 | KDD | Quorum params: n=11, t=6 | Decided — design doc §3, §9.1 |
| ODC-024 | ODC | Multi-quorum membership | Deferred — design doc §9.3 (partially resolved per KDD-053) |
| KDD-053 | KDD | Multi-quorum roll selection + failover | Decided — 2026-06-03; design doc §13 |
| ODC-025 | ODC | Rotation-N final value | **Open** — measured at W2.3; [KDD-051] benchmark must run GJKR-hardened ceremony |
| KDD-051 | KDD | DKG construction — Feldman VSS + GJKR commit-then-reveal hardening | Decided — measured 2026-06-03; design doc §11 |
| KDD-052 | KDD | PTXDKG member set — committed node_id list, chain-determined score order | Decided — 2026-06-03; design doc §12 |
| IMP-D1 | IMP | Ceremony BLS substrate | **Decided: blst** — this plan W1.1 |
| IMP-D2 | IMP | Ceremony transport | **Decided: P2P** — this plan W1.1 |
| IMP-D3 | IMP | On-chain ceremony record | **Decided: PTXDKG tx** — this plan W1.1 |
| IMP-D4 | IMP | Drift collision strategy | **Decided at implementation** — this plan W2.2 |
| IMP-D5 | IMP | W3.1 differential oracle: same-stack subset exhaustion (chiabls rejected) | **Decided** — 2026-06-03 |
| KDD-049 | KDD | PTX_BLS_Verify explicit group_pk — pure function | Decided — commit 66251c8 |
| KDD-050 | KDD | Test extraction interface; ENABLE_PTX_TEST_ACCESSORS compile gate (default off) | Decided — 2026-06-03 |
