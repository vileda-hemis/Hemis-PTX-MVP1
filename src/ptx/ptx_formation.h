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

#endif // PTX_FORMATION_H
