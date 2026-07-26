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

PTXRotationDecision PTX_Formation_RotationDueAt(
        const CBlockIndex* pindexAnchor,
        CPTXQuorumStore& store,
        const Consensus::PTXFormationParams& params);

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
