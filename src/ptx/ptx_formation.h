// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PTX_FORMATION_H
#define PTX_FORMATION_H

// W2.2 SG-1a — the pure formation caller (selection at anchor).
//
// SCOPE (W2.2_SG1A_PREIMPL_APPROVED): anchor in -> member vector out. NO
// session start, NO network coupling, NO boundary/schedule (SG-1b), NO
// MarkForming/SetActiveSession (SG-1c). The anchor pindex is an INPUT —
// SG-1b owns computing it (height % N == 0 boundary,
// pindexNew->GetAncestor(height - stage)).
//
// CONSENSUS DISCIPLINE (the two trap classes this unit is falsified against):
//  - SNAPSHOT-NOT-LIVE: every chain/DGM read is at the ANCHOR
//    (GetListForBlock(pindexAnchor), GetActiveQuorumsAtHeight(anchor
//    height)); never the live tip. Two nodes at different tips MUST derive
//    the identical selection for the same anchor.
//  - DETERMINISTIC-ACROSS-NODES: inputs are chain-derived only; no
//    wall-clock, no node-local state (FORMING is bookkeeping, never a
//    selection input).

#include "evo/deterministicgms.h"
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_quorum_store.h"

#include <functional>
#include <vector>

class CBlockIndex;
namespace Consensus { struct PTXFormationParams; }

// The KDD-040 pool join — PURE list-in/list-out (zero globals; fully
// unit-testable). pool = listAtAnchor filtered by PTX_DKG_IsGMPTXEligible
// (KDD-060; the never-re-inlined predicate) MINUS every member of every
// record in activeAtAnchor, joined by proTxHash.
//
// D-SG1a-1 (decided 2026-07-12): the exclusion set is the FULL formed-11 of
// each ACTIVE record — QUAL or not. Conservative (never double-books a GM),
// simple (the exclusion set is exactly the record's members list — one
// deterministic consensus-visible set), KDD-040-literal. The in_qual-only
// utilization optimisation is a W2.4 revisit (top-up/disband era).
//
// Exclusion runs BEFORE CalculateQuorum (KDD-060 filter-then-score: a
// post-score drop would change the scoring competition and split the chain).
//
// SHARED WITH CONSENSUS: the V5 validator (specialtx_validation.cpp) builds
// the SAME pool through this function — formation and validation must
// reconstruct byte-identical membership (the one-function contract, extended
// to the pool). Forward-bind D-SG1a-2: once W2.3/W2.4 add state mutations,
// GetActiveQuorumsAtHeight must answer state-AS-OF-HEIGHT for this join to
// stay reindex-deterministic — a consensus obligation carried on the
// W2.3/W2.4 owed-lists.
CDeterministicGMList PTX_Formation_BuildPool(
        const CDeterministicGMList& listAtAnchor,
        const std::vector<CPTXQuorumRecord>& activeAtAnchor);

// The anchored formation caller. Reads chain state AT THE GIVEN ANCHOR only:
// GetListForBlock(pindexAnchor) + GetActiveQuorumsAtHeight(anchor height),
// builds the pool, and:
//  - pool  < 11 -> returns false, membersOut empty. Deterministic skip — the
//    formation simply does not fire this cycle (no error, no log spam).
//  - pool >= 11 -> membersOut = PTX_DKG_BuildMemberVectorFromList(pool,
//    pindexAnchor->GetBlockHash()); share_index 1..11 in CalculateQuorum
//    (score) order. The modifier is the anchor block hash — the same
//    quorum_hash identity the store and V5 use (ptx_quorum_store.h:105).
// PURE in effect: no writes, no session, no network. Requires cs_main held
// by the caller (chain/DGM/store reads).
bool PTX_Formation_SelectAtAnchor(const CBlockIndex* pindexAnchor,
                                  std::vector<PTXDKGMember>& membersOut);

// KDD-072 P-b3a — the DRIVER-side rotation member sourcing (present-but-unfed:
// the P-b6 rotation trigger is the only future caller; nothing invokes this at
// HEAD). Same-set members from the predecessor record via the SHARED
// PTX_DKG_ResolveRotationQuorum + PTX_DKG_MembersFromQuorum — the identical
// resolution V12 and the store guard run, so a ceremony this starts produces
// exactly the membership the chain will validate. Returns false (rotation must
// NOT start) on any unresolvable member or key change — the reject-not-exclude
// policy, one decision for all sites.
bool PTX_Formation_SelectRotationMembers(
        const CPTXQuorumRecord& predecessor,
        const CDeterministicGMList& listAtRotationAnchor,
        const CDeterministicGMList& listAtFormationAnchor,
        std::vector<PTXDKGMember>& membersOut);

// ---------------------------------------------------------------------------
// KDD-072 P-b6a — the ROTATION DECISION seam (STUBBED DISABLED).
//
// "Is a rotation due at this anchor, and of which quorum?" P-b6a wires the
// ceremony-start path that CONSUMES this answer; the answer itself is P-b6b's
// trigger POLICY. The stub returns {due=false} unconditionally, so NO rotation
// fires autonomously at HEAD — the whole rotation path is dormant behind this
// one function, exactly as the predecessor field was dormant behind its setter.
//
// ★ The signature is the REAL one so P-b6b fills the BODY, not the call site:
// (anchor, store, params) is everything any of the candidate policies needs —
// age from the record (formation_height/mined_height, both fed), the ACTIVE set
// from the store, and N from params. Option B (deadline/stagger) would
// additionally need a drift_offset producer; Option A (boundary lockstep) and
// the tie-break middle need nothing further.
//
// ★ NOT CONSENSUS: V12 validates a rotation's CORRECTNESS (predecessor exists,
// ACTIVE as-of, same-set resolve, uniqueness) and NEVER its timing — verified
// from source at the P-b6 recon. A node that decides "due" early merely
// produces a valid rotation early; it cannot split the chain. That is what
// lets this policy stay node-local and simple.
// ---------------------------------------------------------------------------
struct PTXRotationDecision {
    bool    due{false};
    uint256 predecessor_quorum_hash;  // meaningful only when due
};

// KDD-072 P-b6b — THE TIP SWEEPS, one entry point, three passes.
//
// ★ SIGNATURE IS THE PLACEMENT DECISION: this takes a TIP HEIGHT and NOTHING
// ELSE — no block, no tx. The conditions it enforces are DEPTH conditions that
// advance with every block (a PENDING aging past its TTL, a residual CURRENT
// share aging past maxreorg+margin), so it MUST run on every tip advance.
// It is called from PTX_Formation_NotifyUpdatedBlockTip (every block, has the
// tip, has the IBD guard) and deliberately NOT from
// CPTXQuorumStore::ProcessBlock, which early-returns on any block carrying no
// PTXDKG — sweeps there would fire only on rotation blocks, which is
// self-defeating for exactly the case that needs them most: a STRANDED PENDING
// whose successor never mined, so no PTXDKG block will ever arrive to trigger
// its expiry. Taking no block argument makes that mis-placement unbuildable.
//
// Three passes (see each function's contract): PENDING TTL expiry (KDD-070 §7),
// SUPERSEDED_RETAINED depth discard (§6), and the CURRENT-residue retirement
// (§5's two-live-keys bound — CPTXQuorumStore::RetireSupersededResidues).
// All are cheap linear walks over a map holding a handful of entries.
void PTX_Formation_RunTipSweeps(int tip_height);

// W2.4 W4-e (KDD-075): RotationDueAt now carries THE YIELD — a quorum that is
// terminal-eligible (idle, or rotation-impossible past grace) yields its
// rotation AT CEREMONY-START and is skipped by the due-selection.  ★ KEYED ON
// ELIGIBILITY, never on "the transition fired": a rate-deferred eligible
// quorum still yields (stays ACTIVE-queued for its limiter turn) — keying on
// "fired" would let it fall through to rotating, mint a successor with reset
// idleness, and reopen Hazard A through the limiter (KDD-075's load-bearing
// clause).  The two injectables (NO defaults — every caller states its
// sources, the P-b2 posture) feed the W4-d predicates; with the params gate
// at its 0 defaults nothing is ever eligible and behaviour is byte-identical
// to P-b6b.  impossible_at is rec-aware (production composes the resolver
// with GetListForBlock at the boundary; tests inject).
// ---------------------------------------------------------------------------
// ★ W2.5a GUARD 1 (KDD-079 §3) — the R/B >= L invariant, ENFORCED not assumed.
//
// Selection-staggering (P-b6b's tie-break at the boundary cadence) provides
// R/B rotation slots per rotation interval.  If that capacity does not cover
// the quorum count, quorums starve past nRotationInterval and KDD-045's
// key-compromise bound breaks.  KDD-079 promotes this from a tuning
// assumption to a checked bound — "an unchecked invariant is how the
// conflation went unnoticed in the first place."
//
// ★ TWO TIERS, deliberately, and the margin is NOT in the hard check:
//   REJECT  when capacity < L            — the true starvation bound.
//   WARN    when capacity < L * MARGIN   — thin, but workable.
// ★ Why the margin is advisory: it exists to absorb CONTENTION between
// competing quorums, and contention requires L > 1.  At L = 1 there is no
// competition — the lone quorum wins every tie-break — so a margin demand
// there would reject or spam a perfectly correct single-quorum config.
//
// ★ DEPLOY-SAFETY, and this is the point: at today's defaults (B = R,
// L = 1) capacity = 1 and L = 1, so 1 >= 1 PASSES with no special case, and
// the warn tier is skipped for L = 1.  The current banked chain, the live
// fleet and every drill config validate unchanged.
// ---------------------------------------------------------------------------
static const int PTX_GUARD1_MARGIN = 2;

// Rotation slots available per rotation interval (integer, floor).
int PTX_Formation_RotationCapacity(const Consensus::PTXFormationParams& params);

// Startup validation.  Returns false + a reason on a config that cannot serve
// its declared quorum count.  PURE (params in, verdict out) so it is
// unit-testable; called from InitSanityCheck.
bool PTX_Formation_CheckParams(const Consensus::PTXFormationParams& params,
                               std::string& err_out);

// Runtime tier: is the LIVE active count beyond what the configured cadence
// comfortably serves?  ★ WARN ONLY — never refuse: refusing to rotate would
// CAUSE the starvation this warns about.
bool PTX_Formation_OverCapacity(const Consensus::PTXFormationParams& params,
                                size_t active_count);

// ---------------------------------------------------------------------------
// ★ W2.5a GUARD 2 (KDD-079 §4) — THE FAIRNESS FLOOR, inside RotationDueAt.
//
// P-b6b's lowest-hash tie-break SPREADS contested rotations but is not FAIR:
// quorum_hash is fixed at formation, so a persistently high-hash quorum can
// lose every contested boundary and starve past nRotationInterval — ageing
// its key beyond KDD-045's compromise bound.  Guard 2 is the floor under the
// tie-break: a quorum whose age (anchor - formation_height, the SAME quantity
// the due test reads — no new record field) reaches
//     nRotationInterval + 2 * nBoundaryInterval
// is OVERDUE and wins its slot REGARDLESS of hash.  The slack scales with the
// cadence ("lost two contested boundaries, wins the third").  Ordering:
// most-overdue-first, lowest hash between equals, non-overdue fall through to
// the tie-break unchanged.  This is what lets KDD-079's Option B preserve
// KDD-045's rotation-windowing bound at scale.
// ★ NODE-LOCAL POLICY like the tie-break it floors — V12 never checks timing,
// so a policy-divergent node produces a valid-but-early rotation, not a split.
// ★ Observationally a NO-OP at the shipped L=1 defaults: a lone quorum wins
// every tie-break, so the winner is identical with or without the override.
// ★ HONEST SCOPE: unit-proven by forcing the tie-break loss ARTIFICIALLY at
// L=2; the guard is LATENT at low L (capacity generous, nothing starves) and
// load-bearing only as L approaches R/B.  Real validation is env-gated to
// W2.5b at L=6-8 under genuine competition.
// ★ GATE COUPLING (ODC-054): Guard 2 is SAFE only alongside the KDD-076
// yield — with nReformGrace == 0 a rotation-impossible quorum ages to
// most-overdue and the override hands it every boundary (fleet-wide rotation
// starvation).  CheckParams therefore HARD-REJECTS nSupportedQuorums > 1
// with nReformGrace == 0; the yield runs before the override by construction
// (pinned by IH_YieldBeatsOverride_GateCouplingMechanism).
// ---------------------------------------------------------------------------
PTXRotationDecision PTX_Formation_RotationDueAt(
        const CBlockIndex* pindexAnchor,
        CPTXQuorumStore& store,
        const Consensus::PTXFormationParams& params,
        const std::function<bool(const CBlockIndex*, CBlock&)>& read_block,
        const std::function<bool(const CPTXQuorumRecord&, const CBlockIndex*)>& impossible_at);

// W2.4 W4-e — THE ELIGIBILITY COMPOSITION (KDD-075/076): terminal-eligible =
//   (nRetireWindow > 0  AND  idle over that window)            [KDD-074]
//   OR (nReformGrace > 0 AND due-and-impossible for that many
//       consecutive boundaries)                                 [KDD-076]
// One set, two routes in; W4-e's yield reads it, W4-f's producer drains it
// through the limiter.  Pure composition of the W4-d predicates — no state.
// why_out is a DIAGNOSTIC SINK (set to idle/forced-reform on true) - the
// nullptr default is deliberate and does not breach the no-default-sources
// posture: P-b2's rule binds INPUTS a caller must consciously state; this is
// an optional output for the yield's log line.
// ★ W4-f amendment - THE AGE ANCHOR: the idle arm additionally requires
// mined_height + nRetireWindow <= anchor height (the quorum lived through the
// whole silent window).  Without it a young quorum on a quiet chain is
// instantly idle-eligible and the reformed successor churns.
bool PTX_Formation_TerminalEligible(
        const CPTXQuorumRecord& rec,
        const CBlockIndex* pindexAnchor,
        const Consensus::PTXFormationParams& params,
        const std::function<bool(const CBlockIndex*, CBlock&)>& read_block,
        const std::function<bool(const CPTXQuorumRecord&, const CBlockIndex*)>& impossible_at,
        std::string* why_out = nullptr);

// W2.4 W4-e — THE RATE LIMITER (KDD-074): among the terminal-eligible set, at
// most ONE transition per rate_window blocks, LEAST-RECENTLY-ACTIVE first
// (smallest last_activity_height; ties broken lowest-hash — the P-b6b
// tie-break shape, deterministic on every node).  candidates carry
// (quorum_hash, last_activity_height) — the caller (W4-f) derives activity
// from the same authenticated attributions the idle scan reads.
// ★ BUG-036: the rate limit is STATELESS — fires only at heights divisible by
// rate_window (a pure height predicate). The old last_reform_height input read
// the store's max stamp, so one clobbered stamp phase-shifted the node's whole
// schedule (the h5487 partition). Derive-don't-store: a schedule that reads no
// stored state cannot be shifted by losing any.
// rate_window <= 0 selects NOTHING (the gate posture).  PURE — W4-f wires
// the real derivation; the selection itself is this one function.
bool PTX_Formation_SelectReformCandidate(
        const std::vector<std::pair<uint256, int>>& candidates,
        int tip_height,
        int rate_window,
        uint256& selected_out);

// ---------------------------------------------------------------------------
// W2.4 W4-d — THE THREE TERMINAL-ELIGIBILITY PREDICATES (KDD-074/075/076).
// Pure, stateless, DORMANT: nothing calls them until W4-e composes them into
// the RotationDueAt yield (idle -> reform; impossible AND grace -> forced
// reform) and W4-f drains the eligible set through the rate limiter.
// ---------------------------------------------------------------------------

// Does this block carry a roll (type-6) ATTRIBUTED to quorum_hash?  The
// attribution read here is TRUSTWORTHY only because W4-b consensus-verifies
// quorum_sig against the named record's group_pk — a forged quorum_hash never
// connects, so the idle signal below reads authenticated attributions only.
bool PTX_Formation_BlockHasAttributedRoll(const CBlock& block,
                                          const uint256& quorum_hash);

// LINEAGE-SCOPED (the demanded-case half of seat-vs-record): idle means no
// roll attributed to ANY hash in the quorum's LINEAGE within the window -
// in-window rotation links (the PTXDKGs the walk already reads) resolve the
// lineage post-pass, so a demanded lineage that rotated is NOT false-idle.
// On a zero-roll chain (bf) this is byte-identical to hash-scoped.
// KDD-074 idle-eligibility, DERIVE-AT-EVAL (never a stored counter — the
// stored form has an unclosable disconnect-undo hole; deriving leaves ZERO
// undo surface).  True iff NO attributed roll for quorum_hash exists in the
// window (tip_height - n_retire, tip_height] — the last n_retire blocks,
// truncated at genesis on a young chain.  n_retire is a PARAMETER (W4-e owns
// the gate; default-disabled there, not here).  read_block has NO default —
// every caller states its source (the P-b2 no-default posture); production
// passes a ReadBlockFromDisk wrapper, tests inject.  FAIL-SAFE: unreadable
// block or degenerate params answer NOT idle (never retire on missing data).
bool PTX_Formation_QuorumIdleAt(
        const uint256& quorum_hash,
        const CBlockIndex* pindexTip,
        int n_retire,
        const std::function<bool(const CBlockIndex*, CBlock&)>& read_block);

// KDD-076 rotation-impossible: the P-b3a resolver REUSED as a predicate (one
// implementation — reject-not-exclude decides both rotation validity and this
// eligibility, so they can never disagree).  Stateless: re-evaluated fresh
// each boundary, which is what dissolves terminal-vs-transient (a ProUpReg
// self-heal simply makes this return false again).
bool PTX_Formation_RotationImpossible(
        const CPTXQuorumRecord& predecessor,
        const CDeterministicGMList& listAtRotationAnchor,
        const CDeterministicGMList& listAtFormationAnchor,
        std::string& why_out);

// KDD-076 grace-M: forced reform only after the quorum was DUE-AND-IMPOSSIBLE
// at each of the last grace_m boundaries (anchor, anchor-interval, ...).
// STATELESS — re-derived from chain history each call, so a boundary where
// rotation was possible (the pathological self-heal) breaks the run and the
// grace restarts by construction; no stored grace counter exists.
// impossible_at evaluates predicate #2 at a historical boundary (production
// composes it with GetListForBlock; tests inject).  FAIL-SAFE false on
// degenerate params / missing ancestors.
// ★ KDD-079: TWO intervals, deliberately.  boundary_interval steps back over
// boundaries; rotation_interval tests due-ness at each.  A single param for
// both is a DEFECT at divergent values (it fired forced-reform on a
// 30-block-old quorum at B=30/R=1440) — the fourth conflation, fixed.
bool PTX_Formation_ForcedReformGraceElapsed(
        const CPTXQuorumRecord& rec,
        const CBlockIndex* pindexAnchor,
        int boundary_interval,
        int rotation_interval,
        int grace_m,
        const std::function<bool(const CBlockIndex*)>& impossible_at);

// ---------------------------------------------------------------------------
// W2.2 SG-1b-i — the pure schedule core (boundary + anchor). PURITY IS
// ENFORCED BY SIGNATURE: (height/pindex, params) in, (fires?, anchor) out —
// no wall-clock, no FORMING reads, no fInitialDownload parameter (the IBD
// guard lives only in SG-1b-ii's notification wrapper and gates ACTION, never
// this computation). Two nodes with the same chain state MUST compute the
// identical answer.
// ---------------------------------------------------------------------------

// Formation boundary: nHeight > 0 && nHeight % N == 0. The nHeight > 0 term
// is load-bearing — 0 % N == 0, so genesis would otherwise be a boundary; no
// formation fires from genesis, by construction.
bool PTX_Formation_IsBoundary(int nHeight,
                              const Consensus::PTXFormationParams& params);

// The cycle-start anchor for pindexNew's height, walked down pindexNew's OWN
// branch: pindexNew->GetAncestor(nHeight - nHeight % N) — the reorg-robust
// V3 idiom (specialtx_validation.cpp CheckPTXDKGTx), NEVER chainActive[]
// indexing and NEVER cached across tips: after a reorg that crosses a
// boundary, each tip derives its own branch's boundary block. Heights before
// the first boundary anchor to genesis; PTX_Formation_IsBoundary is what
// gates firing, not this walk. Returns nullptr only for a null pindexNew.
const CBlockIndex* PTX_Formation_GetAnchor(
        const CBlockIndex* pindexNew,
        const Consensus::PTXFormationParams& params);

// ---------------------------------------------------------------------------
// W2.2 SG-1b-ii — the notification wrapper (LOG-ONLY). Modeled on
// CDKGSessionManager::UpdatedBlockTip (llmq/quorums_dkgsessionmgr.cpp):
// fInitialDownload early-return + IsDIP3Enforced activation guard (the DGM
// list is formation's substrate; no PTX spork/upgrade gate exists — these
// two are the guard set), LOCK(cs_main) (the notification arrives on the
// background scheduler thread without it, like the LLMQ manager), then the
// pure SG-1b-i core: IsBoundary -> GetAnchor -> LogPrintf.
//
// THE GUARDS GATE ACTION ONLY — the pure schedule functions above cannot
// receive them (purity by signature; two nodes at the same chain state
// compute the identical boundary/anchor regardless of either guard).
// NO session start, NO MarkForming (SG-1c's — see the scope banner), NO
// SelectAtAnchor: this unit only OBSERVES the boundary firing; acting on it
// is SG-1c. Called from EvoNotificationInterface::UpdatedBlockTip.
// ---------------------------------------------------------------------------
void PTX_Formation_NotifyUpdatedBlockTip(const CBlockIndex* pindexNew,
                                         bool fInitialDownload);

// ---------------------------------------------------------------------------
// W2.2 SG-1c-i — the ACTING trigger (Model B foundation: thread-driven,
// self-clocked, pure worker). On a boundary the wrapper above now also:
// re-verifies the anchor at action time (chainActive.Contains — the h400
// do-not-drift input, decision 3a) -> IsForming idempotence skip ->
// SelectAtAnchor -> InitSession(my proTxHash) -> member? spawn the ceremony
// thread OWNING the session (shared_ptr; the transport's resolve() hands out
// ref-holding copies — teardown-memory safety) + MarkForming +
// SetActiveSession : fully passive discard (no FORMING, no thread, no
// transport touch; a null activeGamemasterManager — non-GM node — is
// passive-by-identity).
//
// THE THREAD IS A PURE WORKER: it drives only THIS node's participation
// (zero cross-node authority; the form-decision is the chain-deterministic
// SG-1b boundary; the ceremony is GJKR peer-symmetric). It is SELF-CLOCKED:
// at SG-1c-i it is PARKED on an interruptible wait — SG-2 fills the phase
// loop body with own-schedule deadlines; inbound messages are data, never
// control flow.
//
// COLLISION (boundary k+1 vs a live thread k): abort-old = interrupt +
// RELEASE-LOCKS + join, then start k+1 (the LLMQ release-before-join
// pattern; joining under cs_main/cs_session would deadlock against the
// exit path — SG-1c plan-gate liveness decision 2).
//
// W2.2 SG-1c-ii — PER-TIP RE-ARM: THE REORG-CLASS HANDLER (the h400
// do-not-drift closure, NARROW). On every non-boundary tip, iff a live
// session exists, the wrapper compares the session's anchor against the
// recomputed current-cycle anchor (GetAnchor(tip)); inequality
// (reorg-swapped boundary block or cycle-staleness — subsumes a Contains
// check) => abort (release-before-join) + restart at the recomputed anchor
// through the same action path.
//
// THE TWO-PATH DIVISION OF LABOUR (source-decided 2026-07-17):
// UpdatedBlockTip fires ONCE PER WORK-IMPROVEMENT PASS, not per height
// (validation.cpp: starting_tip captured :2342, inner loop :2374, the
// single notification :2377-84; the Step's disconnect loop is silent
// :2230-42 and its connect loop early-breaks only when new chainwork
// EXCEEDS the old tip's :2282-86).
//  - FORWARD arrival: each block is its own pass -> per-height
//    notifications -> the BOUNDARY path in the wrapper owns forward
//    crossings (proven on real catch-ups at 14/48/309-block scales).
//  - REORG: one pass disconnects to the fork and connects PAST the old
//    tip's work before the break can fire -> ONE notification at the final
//    tip -> a boundary inside the skip window (fork+1 .. overtake height)
//    gets NO notification -> the boundary path never runs -> THIS ARM IS
//    THE SOLE RESCUER.  h400 is exactly this class: gm11's single pass
//    (disconnect its own h400, connect the majority's h400+h401 — equal
//    work at 400, overtake at 401) notified only at h401; without this arm
//    its stale session would have parked forever.
//  - Nuance: if new work exceeds the old tip exactly AT the boundary, the
//    pass notifies at B and the boundary path handles it; the skip case is
//    the same-length-race class.
//
// No late-join: starting with NO live session on a non-boundary tip is an
// SG-2 design input (participation model / phase window — recorded
// 2026-07-15).
//
// Shutdown: called from tiertwo/init.cpp BEFORE StopPTXCeremonyTransport
// (join the producer before tearing down the router).
void PTX_Formation_StopCeremonyRunner();

#endif // PTX_FORMATION_H
