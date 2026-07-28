// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_quorum_store.h"

#include "chain.h"
#include "chainparams.h"           // W2.4 W4-f: the params gate at the producer hook
#include "compat/endian.h"
#include "consensus/consensus.h"   // KDD-072 P-b6b: DEFAULT_MAX_REORG_DEPTH (residue depth)
#include "consensus/validation.h"
#include "evo/deterministicgms.h"
#include "evo/evodb.h"
#include "logging.h"
#include "primitives/block.h"
#include "primitives/transaction.h" // GetTxPayload
#include "ptx/ptx_bls.h"     // KDD-070 P2: share store load/reconcile/held-set
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_formation.h"
#include "ptx/ptx_quorum.h"  // g_ptx_my_node_id
#include "util/system.h" // error()
#include "validation.h"  // LookupBlockIndex

#include <algorithm>
#include <limits>
#include <set>

std::unique_ptr<CPTXQuorumStore> ptxQuorumStore;

// evodb key prefixes (LLMQ mined-commitment scheme, PTX flavor):
//   ("pq_r", quorum_hash)                          -> CPTXQuorumRecord
//   ("pq_h", htobe32(UINT32_MAX - mined_height))   -> quorum_hash
static const std::string DB_PTXDKG_QUORUM   = "pq_r";
static const std::string DB_PTXDKG_BY_INV_H = "pq_h";
//   ("pq_p", predecessor_qh)                        -> successor_qh (KDD-072 P-b5)
static const std::string DB_PTXDKG_PRED     = "pq_p";

// mined_height inversed + big-endian so evodb iterates most-recent-first
// (llmq/quorums_blockprocessor BuildInversedHeightKey precedent).
static std::pair<std::string, uint32_t> BuildPTXInversedHeightKey(int nMinedHeight)
{
    return std::make_pair(DB_PTXDKG_BY_INV_H,
                          htobe32(std::numeric_limits<uint32_t>::max() - nMinedHeight));
}

const std::string& PTX_QuorumRecordDBPrefix() { return DB_PTXDKG_QUORUM; }

// KDD-072 P-b4 (ODC-042) — the as-of-height predicate.  See the header contract
// (semantics, the STRICT-> boundary, and why >= is a chain split).
bool PTX_QuorumRecordActiveAt(const CPTXQuorumRecord& rec, int nHeight)
{
    if (rec.mined_height > nHeight)
        return false;
    switch (static_cast<PTXQuorumState>(rec.state)) {
        case PTXQuorumState::ACTIVE:
            return true;
        case PTXQuorumState::SUPERSEDED:
            return rec.superseded_height > nHeight;  // STRICT: superseded AT h is NOT active at h
        case PTXQuorumState::DISBANDED:
            return rec.disbanded_height > nHeight;   // vacuous at HEAD (no producer until W2.4)
        case PTXQuorumState::REFORMED:
            return rec.reformed_height > nHeight;    // W4-c: same STRICT contract; vacuous until W4-f
        default:
            return false; // FORMING is never persisted; unknown state fail-safe
    }
}

// Find the block's PTXDKG (<= 1 guaranteed by CheckPTXDKGBlockRules).
static CTransactionRef FindPTXDKGInBlock(const CBlock& block)
{
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsPTXDKGTx()) return tx;
    }
    return nullptr;
}

bool CPTXQuorumStore::ProcessBlock(const CBlock& block, const CBlockIndex* pindex,
                                   CValidationState& state, bool fJustCheck)
{
    AssertLockHeld(cs_main);

    // W2.4 W4-f — THE REFORM PRODUCER, ahead of the no-PTXDKG early-return
    // (reform is boundary-driven, not tx-driven; the early-return below must
    // not starve it — the P-b6b placement lesson, connect-side).  Connect
    // writes only (!fJustCheck), block-atomic via the surrounding evoDb
    // CurTransaction.  Dormant wherever the params gate is {0} (main/test).
    if (!fJustCheck) {
        const auto w4fReadBlock = [](const CBlockIndex* p2, CBlock& out) {
            return ReadBlockFromDisk(out, p2);
        };
        const auto w4fImpossibleAt = [](const CPTXQuorumRecord& r, const CBlockIndex* pb) {
            const CBlockIndex* pForm = LookupBlockIndex(r.quorum_hash);
            if (pForm == nullptr) return false;   // fail-safe: not impossible
            const CDeterministicGMList listRot  = deterministicGMManager->GetListForBlock(pb);
            const CDeterministicGMList listForm = deterministicGMManager->GetListForBlock(pForm);
            std::string why;
            return PTX_Formation_RotationImpossible(r, listRot, listForm, why);
        };
        MaybeReformAtBoundary(pindex, Params().GetConsensus().ptxFormation,
                              w4fReadBlock, w4fImpossibleAt);
    }

    const CTransactionRef tx = FindPTXDKGInBlock(block);
    if (tx == nullptr) {
        return true;
    }

    PTXDKGPayload payload;
    if (!GetTxPayload(*tx, payload)) {
        // CheckSpecialTx already deserialized this payload in the same
        // ProcessSpecialTxsInBlock call — failure here is local corruption.
        return state.DoS(100, error("%s: PTXDKG payload failed to deserialize post-check", __func__),
                         REJECT_INVALID, "ptxdkg-bad-payload");
    }

    // Persist-boundary guard 1 (ODC-030 cross-block uniqueness): one accepted
    // PTXDKG per formation — never overwrite an existing record.
    if (HasQuorumRecord(payload.quorum_hash)) {
        return state.DoS(100, error("%s: PTXDKG for formation %s already accepted", __func__,
                                    payload.quorum_hash.ToString()),
                         REJECT_INVALID, "ptxdkg-duplicate-formation");
    }

    // Re-run the canonical selection at the anchor — the exact V5 core
    // (KDD-060 one-function contract), which CheckPTXDKGTx just ran in this
    // same call.  V1-V3 proved the anchor exists on this chain.
    const CBlockIndex* pindexQuorum = LookupBlockIndex(payload.quorum_hash);
    if (pindexQuorum == nullptr) {
        return state.DoS(100, error("%s: PTXDKG anchor %s vanished post-check", __func__,
                                    payload.quorum_hash.ToString()),
                         REJECT_INVALID, "ptxdkg-quorum-hash-not-found");
    }
    // SG-1a / KDD-073: this guard reconstructs through the SAME functions as
    // V5/V12 and formation (the one-function contract). THE MISSED-SITE LESSON
    // (2026-07-13, battery_sg1 row v5(y)): with V5 pool-aware and this guard
    // raw, a valid PTXDKG passed populate + assembler and was REJECTED at
    // connect — a self-poisoning divergence. P-b3a extends the lesson to
    // rotation: this guard branches EXACTLY as V12 does, through the same
    // PTX_DKG_CheckRotationAndResolve, or a valid rotation dies here after
    // passing validation (the same battery_sg1 shape).
    std::vector<CDeterministicGMCPtr> quorum11;
    if (payload.nVersion >= PTXDKGPayload::ROTATION_VERSION &&
        !payload.predecessor_quorum_hash.IsNull()) {
        CPTXQuorumRecord predRec;
        if (!GetQuorumRecord(payload.predecessor_quorum_hash, predRec)) {
            return state.DoS(100, error("%s: rotation predecessor %s vanished post-check", __func__,
                                        payload.predecessor_quorum_hash.ToString()),
                             REJECT_INVALID, "ptxdkg-rotation-predecessor-unknown");
        }
        const CBlockIndex* pindexPred = LookupBlockIndex(predRec.quorum_hash);
        if (pindexPred == nullptr) {
            return state.DoS(100, error("%s: rotation predecessor anchor %s vanished post-check",
                                        __func__, predRec.quorum_hash.ToString()),
                             REJECT_INVALID, "ptxdkg-rotation-predecessor-anchor-unknown");
        }
        const CDeterministicGMList listRot  = deterministicGMManager->GetListForBlock(pindexQuorum);
        const CDeterministicGMList listForm = deterministicGMManager->GetListForBlock(pindexPred);
        if (!PTX_DKG_CheckRotationAndResolve(predRec, pindex->nHeight - 1,
                                             listRot, listForm, quorum11, state)) {
            return false;
        }
        // V12d twin (KDD-072 P-b5): persist-boundary defense, the V9 pattern.
        if (!CheckPredecessorUnrotated(payload.predecessor_quorum_hash, state)) {
            return false;
        }
    } else {
        const CDeterministicGMList dgmList = deterministicGMManager->GetListForBlock(pindexQuorum);
        const CDeterministicGMList formationPool =
            PTX_Formation_BuildPool(dgmList,
                                    GetActiveQuorumsAtHeight(pindexQuorum->nHeight));
        quorum11 =
            PTX_DKG_SelectQuorumFromList(formationPool, payload.quorum_hash);
    }

    // Materialize per-member share_index (KDD-061: recovery-x = committed
    // formation share_index, score-order, gaps preserved).  The full
    // selected-11 is recorded; in_qual marks the committed survivors.
    std::set<std::string> committed(payload.member_node_ids.begin(),
                                    payload.member_node_ids.end());
    std::vector<PTXQuorumMemberRecord> members;
    size_t in_qual_count = 0;
    for (size_t i = 0; i < quorum11.size(); i++) {
        PTXQuorumMemberRecord m;
        m.node_id     = quorum11[i]->pdgmState->node_id;
        m.proTxHash   = quorum11[i]->proTxHash;
        m.share_index = (uint8_t)(i + 1); // KDD-052/060 score-order rank
        m.in_qual     = committed.count(m.node_id) > 0;
        if (m.in_qual) in_qual_count++;
        members.push_back(m);
    }

    // Persist-boundary guard 2 (member containment): every committed member
    // must have a rank in the canonical selection, with no duplicates —
    // otherwise a committed member has NO derivable share_index and KDD-061
    // recovery is unreconstructable from this record.
    if (in_qual_count != payload.member_node_ids.size() ||
        committed.size() != payload.member_node_ids.size()) {
        return state.DoS(100, error("%s: PTXDKG committed members not contained in the "
                                    "canonical selection (%d committed, %d matched)", __func__,
                                    (int)payload.member_node_ids.size(), (int)in_qual_count),
                         REJECT_INVALID, "ptxdkg-member-not-in-quorum");
    }

    if (fJustCheck) {
        // TestBlockValidity passes a dummy CBlockIndex with NULL phashBlock —
        // pindex->GetBlockHash() must never run before this gate (only checks
        // above; the DGM manager holds the same contract).
        return true;
    }

    CPTXQuorumRecord rec;
    rec.quorum_hash      = payload.quorum_hash;
    rec.formation_height = payload.formation_height;
    rec.group_pk_bytes.assign(payload.group_pk_bytes, payload.group_pk_bytes + 48);
    rec.vvec_hash        = payload.vvec_hash;
    rec.members          = std::move(members);
    rec.formed_size      = (uint8_t)quorum11.size();
    rec.completed_size   = (uint8_t)in_qual_count;
    rec.state            = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    rec.provenance       = static_cast<uint8_t>(PTXQuorumProvenance::UNSET);
    rec.accepted_txid    = tx->GetHash();
    rec.mined_block_hash = pindex->GetBlockHash();
    rec.mined_height     = pindex->nHeight;
    // W2.4 LINEAGE CLOCK - fresh formation: the seat's silence starts at its
    // birth (own mined height; the young-quorum grace intact).  The rotation
    // branch below OVERWRITES this with the predecessor's inherited clock.
    rec.idle_since_height = pindex->nHeight;
    // KDD-072 P-b6b: stamp the SUCCESSOR's last_rotation_height — "this quorum
    // came into being by rotating its predecessor, at this height". A fresh
    // formation keeps the -1 sentinel. Pre-write field assignment: no second
    // evodb round-trip, block-atomic with the record, pindex-derived (the
    // reindex-determinism rule). Surfaced by ptx_quorum_info since ODC-043.
    if (payload.nVersion >= PTXDKGPayload::ROTATION_VERSION &&
        !payload.predecessor_quorum_hash.IsNull()) {
        rec.last_rotation_height = pindex->nHeight;
        // W2.4 LINEAGE CLOCK - rotation successor: INHERIT the predecessor's
        // idle_since_height (COPY, never mutate - the predecessor keeps its
        // own field, so undo needs nothing new: the successor's record is
        // erased whole on disconnect).  The seat's clock survives rotation;
        // without this the per-record clock resets every rotation and an
        // idle lineage rotates forever instead of reforming (Hazard A via
        // the age anchor - the pre-drill finding).  Pre-v4 predecessor
        // (sentinel -1): fall back to its mined_height, the same fallback
        // the anchor itself applies.  The predecessor record is guaranteed
        // present (the rotation guard above validated it this same call).
        CPTXQuorumRecord lineagePred;
        if (GetQuorumRecord(payload.predecessor_quorum_hash, lineagePred)) {
            rec.idle_since_height = lineagePred.idle_since_height >= 0
                                        ? lineagePred.idle_since_height
                                        : lineagePred.mined_height;
        }
    }

    evoDb.Write(std::make_pair(DB_PTXDKG_QUORUM, rec.quorum_hash), rec);
    evoDb.Write(BuildPTXInversedHeightKey(rec.mined_height), rec.quorum_hash);
    {
        LOCK(cs);
        recordCache[rec.quorum_hash] = rec;
    }
    // T-E consumption hook: no-op until W2.2 produces FORMING entries.
    ConsumeFormingOnConnect(rec.quorum_hash);

    // KDD-072 P-b4 — ROTATION CONNECT (inspection-only until a v2 rotation can
    // land, which is post-P-b3; ODC-032: block events are not unit-simulable).
    // Record flip is block-atomic via CurTransaction; the share-slot promote is
    // DELIBERATELY not atomic with it — KDD-070 covers the gap with idempotence
    // + key-isolation + startup reconciliation (do not force cross-store
    // atomicity; it fights that design).
    if (payload.nVersion >= PTXDKGPayload::ROTATION_VERSION &&
        !payload.predecessor_quorum_hash.IsNull()) {
        if (!MarkSuperseded(payload.predecessor_quorum_hash, pindex->nHeight)) {
            LogPrintf("%s: WARNING: rotation connected but predecessor %s was not "
                      "ACTIVE to supersede (V12 should have rejected this)\n",
                      __func__, payload.predecessor_quorum_hash.ToString());
        }
        // KDD-072 P-b5: set the predecessor-uniqueness index (refuse-unless-
        // absent; V12d rejected any second successor before this point).
        if (!WriteSuccessorOf(payload.predecessor_quorum_hash, payload.quorum_hash)) {
            LogPrintf("%s: WARNING: pq_p already set for predecessor %s "
                      "(V12d should have rejected this)\n",
                      __func__, payload.predecessor_quorum_hash.ToString());
        }
        PTX_BLS_Promote(payload.quorum_hash, payload.predecessor_quorum_hash,
                        pindex->nHeight, &evoDb);
    }

    LogPrintf("%s: persisted PTXDKG quorum record. quorum_hash=%s anchor_height=%d "
              "mined_height=%d formed=%d completed=%d txid=%s\n", __func__,
              rec.quorum_hash.ToString(), rec.formation_height, rec.mined_height,
              (int)rec.formed_size, (int)rec.completed_size, rec.accepted_txid.ToString());
    return true;
}

bool CPTXQuorumStore::UndoBlock(const CBlock& block, const CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

    // W2.4 W4-f — reform disconnect, ahead of the early-return (a reform
    // block carries no PTXDKG).  The stamp is the undo journal; a non-reform
    // height matches nothing (idempotent no-op).  No ordering constraint
    // with the P-b4/P-b5 composition below (disjoint records/keys).
    // Null-guarded: this hook runs BEFORE the early-return that used to
    // shield null-pindex disconnect calls (unit harnesses exercise them).
    if (pindex != nullptr) {
        RestoreReformedAtHeight(pindex->nHeight);
    }

    const CTransactionRef tx = FindPTXDKGInBlock(block);
    if (tx == nullptr) {
        return true;
    }

    PTXDKGPayload payload;
    if (!GetTxPayload(*tx, payload)) {
        LogPrintf("%s: PTXDKG payload failed to deserialize on disconnect of %s — "
                  "evodb integrity failure\n", __func__, pindex->GetBlockHash().ToString());
        return false;
    }

    // Explicit-erase of BOTH keys (LLMQ UndoBlock precedent — see the header
    // note on why DGM's cache-only undo would be wrong for this store).
    evoDb.Erase(std::make_pair(DB_PTXDKG_QUORUM, payload.quorum_hash));
    evoDb.Erase(BuildPTXInversedHeightKey(pindex->nHeight));
    {
        LOCK(cs);
        recordCache.erase(payload.quorum_hash);
    }

    // KDD-072 P-b4 — ROTATION DISCONNECT (inspection-only until post-P-b3; the
    // bf-fleet invalidateblock cycle is the verification, ODC-032).  Both
    // reverts are pure functions of the disconnecting payload's predecessor;
    // both are idempotent no-ops for a non-rotation block (guard false / the
    // UndoPromote key-isolation contract).
    if (payload.nVersion >= PTXDKGPayload::ROTATION_VERSION &&
        !payload.predecessor_quorum_hash.IsNull()) {
        RestoreActiveOnUndo(payload.predecessor_quorum_hash);
        PTX_BLS_UndoPromote(payload.quorum_hash, payload.predecessor_quorum_hash,
                            &evoDb);
        // KDD-072 P-b5: the reorg RE-ALLOW — erasing pq_p lets a different
        // successor rotate this predecessor on the new branch (the V9
        // erase-on-disconnect pattern). Idempotent; no ordering constraint
        // with the two reverts above.
        EraseSuccessorOf(payload.predecessor_quorum_hash);
    }

    // E-5 boundary: NO re-pend of the disconnected tx.  Re-submission policy
    // is W2.2 formation's (the pending slot may hold a newer in-flight
    // formation by then) — PRODUCER-PENDING, register-marked.
    LogPrintf("%s: erased PTXDKG quorum record on disconnect. quorum_hash=%s height=%d\n",
              __func__, payload.quorum_hash.ToString(), pindex->nHeight);
    return true;
}

std::vector<CPTXQuorumRecord> CPTXQuorumStore::GetActiveQuorumsAtHeight(int nHeight)
{
    // LLMQ GetMinedCommitmentsUntilBlock pattern: seek the inversed-height key
    // for nHeight, walk forward (= decreasing mined height) while the prefix
    // holds, map each quorum_hash to its record, filter ACTIVE.
    std::vector<uint256> hashes;
    {
        LOCK(evoDb.cs);
        auto dbIt = evoDb.GetCurTransaction().NewIteratorUniquePtr();
        const auto firstKey = BuildPTXInversedHeightKey(nHeight);
        dbIt->Seek(firstKey);
        while (dbIt->Valid()) {
            std::pair<std::string, uint32_t> curKey;
            if (!dbIt->GetKey(curKey) || std::get<0>(curKey) != DB_PTXDKG_BY_INV_H) {
                break;
            }
            const uint32_t minedHeight =
                std::numeric_limits<uint32_t>::max() - be32toh(std::get<1>(curKey));
            if (minedHeight > (uint32_t)nHeight) {
                break; // defensive: Seek should already start at <= nHeight
            }
            uint256 qh;
            if (!dbIt->GetValue(qh)) {
                break;
            }
            hashes.push_back(qh);
            dbIt->Next();
        }
    }
    std::vector<CPTXQuorumRecord> ret;
    for (const uint256& qh : hashes) {
        CPTXQuorumRecord rec;
        // KDD-072 P-b4 (ODC-042): the AS-OF-HEIGHT predicate replaces the raw
        // current-state filter.  All six reconstruction consumers (V5, the
        // connect guard, the formation pool, signing-ctx, debug builder,
        // eligibility RPC) read through this one method, so the fix repairs
        // every site uniformly — the KDD-073 no-divergence property for the
        // record-read path.  On a zero-rotation chain every record is ACTIVE
        // and this reduces bit-identically to the old filter.
        if (GetQuorumRecord(qh, rec) && PTX_QuorumRecordActiveAt(rec, nHeight)) {
            ret.push_back(rec);
        }
    }
    return ret;
}

// ---------------------------------------------------------------------------
// W2.1 C2 — PRODUCER-PENDING transition functions (see header contract; no
// production caller except the documented no-op ConsumeFormingOnConnect hook).
// Register-marked: falsification bound to W2.2 (forming) / W2.4 (disband).
// ---------------------------------------------------------------------------

void CPTXQuorumStore::MarkForming(const uint256& quorum_hash, int formation_height)
{
    LOCK(cs);
    formingEntries[quorum_hash] = formation_height;
    LogPrintf("%s: FORMING entry registered (node-local). quorum_hash=%s height=%d\n",
              __func__, quorum_hash.ToString(), formation_height);
}

void CPTXQuorumStore::ClearForming(const uint256& quorum_hash)
{
    LOCK(cs);
    if (formingEntries.erase(quorum_hash)) {
        // §C1: this erase is the state-side half of abort-clears-slot; the
        // sk-share clear it authorizes is built at W2.2, not here.
        LogPrintf("%s: FORMING entry cleared (abort/authorized-replacement path). "
                  "quorum_hash=%s\n", __func__, quorum_hash.ToString());
    }
}

void CPTXQuorumStore::ConsumeFormingOnConnect(const uint256& quorum_hash)
{
    LOCK(cs);
    if (formingEntries.erase(quorum_hash)) {
        LogPrintf("%s: FORMING entry consumed by accepted PTXDKG. quorum_hash=%s\n",
                  __func__, quorum_hash.ToString());
    }
    // Empty map until W2.2 produces FORMING entries — no-op today by design.
}

bool CPTXQuorumStore::MarkDisbanded(const uint256& quorum_hash, int disband_height)
{
    // PRODUCER-PENDING (W2.4).  No block event drives this at W2.1 and its
    // disconnect-undo is deliberately not designed yet — W2.4 wires both.
    LOCK(cs);
    CPTXQuorumRecord rec;
    if (!GetQuorumRecord(quorum_hash, rec)) {
        return false;
    }
    rec.state = static_cast<uint8_t>(PTXQuorumState::DISBANDED);
    rec.consecutive_inquorate_blocks = 0;
    evoDb.Write(std::make_pair(DB_PTXDKG_QUORUM, quorum_hash), rec);
    recordCache[quorum_hash] = rec;
    LogPrintf("%s: quorum DISBANDED at height %d. quorum_hash=%s\n",
              __func__, disband_height, quorum_hash.ToString());
    return true;
}

bool CPTXQuorumStore::MarkSuperseded(const uint256& quorum_hash, int superseded_at_height)
{
    // KDD-072 P-b4 (KDD-063 swap, connect half).  Refuse-unless-ACTIVE: a
    // double-flip / missing record / disbanded record is a clean no-op false.
    LOCK(cs);
    CPTXQuorumRecord rec;
    if (!GetQuorumRecord(quorum_hash, rec)) {
        return false;
    }
    if (rec.state != static_cast<uint8_t>(PTXQuorumState::ACTIVE)) {
        return false;
    }
    rec.nVersion          = CPTXQuorumRecord::CURRENT_VERSION; // v2 on rewrite
    rec.state             = static_cast<uint8_t>(PTXQuorumState::SUPERSEDED);
    rec.superseded_height = superseded_at_height; // pindex-derived ONLY
    evoDb.Write(std::make_pair(DB_PTXDKG_QUORUM, quorum_hash), rec);
    recordCache[quorum_hash] = rec;
    LogPrintf("%s: quorum SUPERSEDED at height %d. quorum_hash=%s\n",
              __func__, superseded_at_height, quorum_hash.ToString());
    return true;
}

bool CPTXQuorumStore::RestoreActiveOnUndo(const uint256& quorum_hash)
{
    // KDD-072 P-b4: the disconnect half.  Refuse-unless-SUPERSEDED (idempotent
    // no-op otherwise).  Restore-to-ACTIVE is unconditionally correct — V12
    // admits a successor only over an ACTIVE predecessor, so the pre-flip
    // state needs no undo journal.
    LOCK(cs);
    CPTXQuorumRecord rec;
    if (!GetQuorumRecord(quorum_hash, rec)) {
        return false;
    }
    if (rec.state != static_cast<uint8_t>(PTXQuorumState::SUPERSEDED)) {
        return false;
    }
    rec.nVersion          = CPTXQuorumRecord::CURRENT_VERSION;
    rec.state             = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    rec.superseded_height = -1;
    evoDb.Write(std::make_pair(DB_PTXDKG_QUORUM, quorum_hash), rec);
    recordCache[quorum_hash] = rec;
    LogPrintf("%s: quorum supersede REVERTED (SUPERSEDED->ACTIVE). quorum_hash=%s\n",
              __func__, quorum_hash.ToString());
    return true;
}

bool CPTXQuorumStore::MarkReformed(const uint256& quorum_hash, int reform_height)
{
    // W2.4 W4-c (KDD-074/075/076).  Refuse-unless-ACTIVE: the MarkSuperseded
    // posture — first-transition-wins (KDD-075 mechanics note), and a missing
    // record / double-flip is a clean no-op false.
    LOCK(cs);
    CPTXQuorumRecord rec;
    if (!GetQuorumRecord(quorum_hash, rec)) {
        return false;
    }
    if (rec.state != static_cast<uint8_t>(PTXQuorumState::ACTIVE)) {
        return false;
    }
    rec.nVersion        = CPTXQuorumRecord::CURRENT_VERSION; // v3 on rewrite
    rec.state           = static_cast<uint8_t>(PTXQuorumState::REFORMED);
    // THE STAMP, in the SAME write as the state flip — the ODC-044 lesson.
    // Without it the as-of arm reads the -1 sentinel and the record answers
    // inactive-at-every-height (MarkDisbanded's live bug, deliberately NOT
    // reproduced here and deliberately NOT fixed there — it is disband's,
    // owed with its producer).
    rec.reformed_height = reform_height; // pindex-derived ONLY
    evoDb.Write(std::make_pair(DB_PTXDKG_QUORUM, quorum_hash), rec);
    recordCache[quorum_hash] = rec;
    LogPrintf("%s: quorum REFORMED at height %d. quorum_hash=%s\n",
              __func__, reform_height, quorum_hash.ToString());
    return true;
}

bool CPTXQuorumStore::RestoreReformedOnUndo(const uint256& quorum_hash)
{
    // W2.4 W4-c: the disconnect twin.  Refuse-unless-REFORMED (idempotent
    // no-op otherwise).  Restore-to-ACTIVE is unconditionally correct:
    // MarkReformed refuses unless ACTIVE, so the pre-flip state is known
    // without an undo journal (the P-b4 argument).  The whole undo surface:
    // idleness is derived, never stored — nothing else to revert.
    LOCK(cs);
    CPTXQuorumRecord rec;
    if (!GetQuorumRecord(quorum_hash, rec)) {
        return false;
    }
    if (rec.state != static_cast<uint8_t>(PTXQuorumState::REFORMED)) {
        return false;
    }
    rec.nVersion        = CPTXQuorumRecord::CURRENT_VERSION;
    rec.state           = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    rec.reformed_height = -1;
    evoDb.Write(std::make_pair(DB_PTXDKG_QUORUM, quorum_hash), rec);
    recordCache[quorum_hash] = rec;
    LogPrintf("%s: quorum reform REVERTED (REFORMED->ACTIVE). quorum_hash=%s\n",
              __func__, quorum_hash.ToString());
    return true;
}

// W2.4 W4-f — the DURABLE record-hash snapshot at height (the pq_h walk of
// GetActiveQuorumsAtHeight, unfiltered).  NEVER the recordCache: the cache is
// read-through and empty after restart — a cache scan would silently miss
// pre-restart stamps and break reindex-determinism.
static std::vector<uint256> W4f_SnapshotHashes(CEvoDB& evoDb, int nHeight)
{
    std::vector<uint256> hashes;
    LOCK(evoDb.cs);
    auto dbIt = evoDb.GetCurTransaction().NewIteratorUniquePtr();
    if (!dbIt) return hashes;   // defensive: no iterator, empty snapshot
    const auto firstKey = BuildPTXInversedHeightKey(nHeight);
    dbIt->Seek(firstKey);
    while (dbIt->Valid()) {
        std::pair<std::string, uint32_t> curKey;
        if (!dbIt->GetKey(curKey) || std::get<0>(curKey) != DB_PTXDKG_BY_INV_H) break;
        uint256 qh;
        if (!dbIt->GetValue(qh)) break;
        hashes.push_back(qh);
        dbIt->Next();
    }
    return hashes;
}

size_t CPTXQuorumStore::MaybeReformAtBoundary(
        const CBlockIndex* pindex,
        const Consensus::PTXFormationParams& params,
        const std::function<bool(const CBlockIndex*, CBlock&)>& read_block,
        const std::function<bool(const CPTXQuorumRecord&, const CBlockIndex*)>& impossible_at)
{
    // The gate (all default 0): nothing eligible, nothing selected — dormant.
    if (pindex == nullptr) return 0;
    if (params.nRetireWindow <= 0 && params.nReformGrace <= 0) return 0;
    if (params.nReformRateWindow <= 0) return 0;   // limiter selects nothing
    if (params.nFormationInterval <= 0 ||
        pindex->nHeight <= 0 ||
        pindex->nHeight % params.nFormationInterval != 0) {
        return 0;                                   // boundaries only
    }

    const std::vector<CPTXQuorumRecord> active =
            GetActiveQuorumsAtHeight(pindex->nHeight);
    if (active.empty()) return 0;

    // One shared backward pass over the idle window: qh -> last attributed
    // roll height (the same authenticated attributions the idle scan reads).
    std::map<uint256, int> last_attributed;
    if (params.nRetireWindow > 0) {
        const CBlockIndex* p = pindex;
        for (int i = 0; i < params.nRetireWindow && p != nullptr; ++i, p = p->pprev) {
            CBlock block;
            if (!read_block(p, block)) break;      // fail-safe: partial map only
            for (const auto& tx : block.vtx) {
                if (!tx->IsProbabilisticTx()) continue;
                CProbabilisticTxPayload payload;
                if (!GetTxPayload(*tx, payload)) continue;
                auto it = last_attributed.find(payload.quorum_hash);
                if (it == last_attributed.end() || it->second < p->nHeight) {
                    last_attributed[payload.quorum_hash] = p->nHeight;
                }
            }
        }
    }

    // The eligible candidates, LRA-keyed: an idle-eligible has no in-window
    // activity by definition, so its LRA is record antiquity (mined_height);
    // a forced-reform eligible (may be busy) gets its real last-attributed.
    std::vector<std::pair<uint256, int>> candidates;
    for (const CPTXQuorumRecord& rec : active) {
        if (!PTX_Formation_TerminalEligible(rec, pindex, params,
                                            read_block, impossible_at)) {
            continue;
        }
        const auto it = last_attributed.find(rec.quorum_hash);
        candidates.emplace_back(rec.quorum_hash,
                                it != last_attributed.end() ? it->second
                                                            : rec.mined_height);
    }
    if (candidates.empty()) return 0;

    // The most recent reform fleet-wide (the one-per-window input): walk the
    // full record snapshot for the max reformed_height stamp.
    int last_reform_height = -1;
    for (const uint256& qh : W4f_SnapshotHashes(evoDb, pindex->nHeight)) {
        CPTXQuorumRecord r;
        if (!GetQuorumRecord(qh, r)) continue;
        if (r.reformed_height > last_reform_height) {
            last_reform_height = r.reformed_height;
        }
    }

    uint256 selected;
    if (!PTX_Formation_SelectReformCandidate(candidates, pindex->nHeight,
                                             params.nReformRateWindow,
                                             last_reform_height, selected)) {
        return 0;                                   // rate-limited out
    }
    return MarkReformed(selected, pindex->nHeight) ? 1 : 0;
}

size_t CPTXQuorumStore::RestoreReformedAtHeight(int height)
{
    // The stamp is the undo journal: collect matches under cs, then restore
    // through the guarded writer (idempotent; at most one by construction).
    std::vector<uint256> matches;
    for (const uint256& qh : W4f_SnapshotHashes(evoDb, height)) {
        CPTXQuorumRecord r;
        if (!GetQuorumRecord(qh, r)) continue;
        if (r.reformed_height == height &&
            r.state == static_cast<uint8_t>(PTXQuorumState::REFORMED)) {
            matches.push_back(qh);
        }
    }
    size_t n = 0;
    for (const uint256& qh : matches) {
        if (RestoreReformedOnUndo(qh)) ++n;
    }
    return n;
}

size_t CPTXQuorumStore::RetireSupersededResidues(int tip_height)
{
    // See the header contract (why this exists, why DELETED, why store-side).
    const int retire_depth = DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN;
    size_t retired = 0;
    for (const uint256& qh : PTX_BLS_HeldCurrentQuorumHashes()) {
        CPTXQuorumRecord rec;
        if (!GetQuorumRecord(qh, rec)) continue;   // orphan — reconciliation's job
        // ★ THE LIVE-SHARE PROTECTION: only a CURRENT share whose record is
        // SUPERSEDED is a residue. Without these guards the sweep would retire
        // a LIVE quorum's working share (RED-proven, P-b6b INV-B2).
        if (rec.state != static_cast<uint8_t>(PTXQuorumState::SUPERSEDED)) continue;
        // ★ AND the sentinel: an unstamped record is never retired. NOTE (proven
        // by inversion, P-b6b): these two checks are REDUNDANT across every
        // reachable record state — an ACTIVE record always carries -1 (and
        // RestoreActiveOnUndo explicitly clears the stamp), so either check alone
        // blocks the live-share catastrophe. Dropping BOTH retires a live
        // quorum's working share. Both are kept: the state test states the
        // intent, the sentinel is the fail-safe.
        if (rec.superseded_height < 0) continue;
        if (tip_height - rec.superseded_height < retire_depth) continue;
        if (PTX_BLS_RetireShare(qh, &evoDb)) {
            LogPrintf("%s: retired residual CURRENT share for SUPERSEDED quorum %s "
                      "(superseded at %d, buried %d >= %d blocks; KDD-070 section-5 "
                      "two-live-keys bound)\n", __func__, qh.ToString(),
                      rec.superseded_height, tip_height - rec.superseded_height,
                      retire_depth);
            ++retired;
        }
    }
    return retired;
}

bool CPTXQuorumStore::HasSuccessorOf(const uint256& predecessor_qh)
{
    uint256 successor;
    return evoDb.Read(std::make_pair(DB_PTXDKG_PRED, predecessor_qh), successor);
}

bool CPTXQuorumStore::CheckPredecessorUnrotated(const uint256& predecessor_qh,
                                                CValidationState& state)
{
    // KDD-072 P-b5 V12d — see the header contract (index primary, as-of
    // secondary). One implementation; the validator and the connect guard
    // both call this.
    if (HasSuccessorOf(predecessor_qh)) {
        return state.DoS(100, error("%s: predecessor %s already has a successor (pq_p set)",
                                    __func__, predecessor_qh.ToString()),
                         REJECT_INVALID, "ptxdkg-predecessor-already-rotated");
    }
    return true;
}

bool CPTXQuorumStore::WriteSuccessorOf(const uint256& predecessor_qh,
                                       const uint256& successor_qh)
{
    // Refuse-unless-absent: a second write to the same predecessor is a defect
    // (V12d should have rejected the payload before this point).
    if (HasSuccessorOf(predecessor_qh)) {
        return false;
    }
    evoDb.Write(std::make_pair(DB_PTXDKG_PRED, predecessor_qh), successor_qh);
    LogPrintf("%s: predecessor-uniqueness index set. predecessor=%s successor=%s\n",
              __func__, predecessor_qh.ToString(), successor_qh.ToString());
    return true;
}

void CPTXQuorumStore::EraseSuccessorOf(const uint256& predecessor_qh)
{
    evoDb.Erase(std::make_pair(DB_PTXDKG_PRED, predecessor_qh));
}

bool CPTXQuorumStore::IsForming(const uint256& quorum_hash) const
{
    LOCK(cs);
    return formingEntries.count(quorum_hash) > 0;
}

bool CPTXQuorumStore::HasQuorumRecord(const uint256& quorum_hash)
{
    {
        LOCK(cs);
        if (recordCache.count(quorum_hash)) return true;
    }
    return evoDb.Exists(std::make_pair(DB_PTXDKG_QUORUM, quorum_hash));
}

bool CPTXQuorumStore::GetQuorumRecord(const uint256& quorum_hash, CPTXQuorumRecord& ret)
{
    {
        LOCK(cs);
        auto it = recordCache.find(quorum_hash);
        if (it != recordCache.end()) {
            ret = it->second;
            return true;
        }
    }
    CPTXQuorumRecord rec;
    if (!evoDb.Read(std::make_pair(DB_PTXDKG_QUORUM, quorum_hash), rec)) {
        return false;
    }
    {
        LOCK(cs);
        recordCache[quorum_hash] = rec;
    }
    ret = rec;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_SelectDKGSigningCtx (SG-3) — pure selection of DKG signing material.
// See the header for the x-basis note and the PROVISIONAL selection rule.
// ---------------------------------------------------------------------------

PTXDKGSigningCtx PTX_SelectDKGSigningCtx(const std::vector<CPTXQuorumRecord>& active,
                                         const uint256& tip_hash)
{
    PTXDKGSigningCtx ctx;
    if (active.empty())
        return ctx;                 // no quorum at all -> legitimate dealer fallback

    ctx.quorum_present = true;      // from here, a load failure is FAIL-CLOSED

    // ------------------------------------------------------------------
    // §7.4 ROUTING DISTRIBUTION (W2.5a, KDD-079 §7(ii); replaces the
    // PROVISIONAL KDD-066 newest-wins rule).  §7.4 requires the router to
    // "select among available quorums", caller-agnostic — newest-wins sent
    // EVERY roll to one quorum, which is the structural half of ODC-052:
    // at L>1 the other L-1 are idle BY CONSTRUCTION, so reform drains them
    // and the fleet churns.  Distributing breaks that: a quorum receiving
    // its share is not idle, not eligible, not reformed.
    //
    // ★ SELECTION INPUT = THE TIP BLOCK HASH (decision A'), never the
    // caller's round_seed.  round_seed is built from caller_salt, which is
    // FREE-FORM HEX: keying on it would let a caller grind salts locally,
    // at ~zero cost, until the selector named a quorum of their choosing —
    // a targeting oracle.  The tip hash is unforgeable by the caller.
    //
    // ★ A' IS A DISTRIBUTION FIX WITH A DOCUMENTED RESIDUAL, NOT A SECURITY
    // FIX.  The caller can still TIMING-grind (wait ~L blocks for the tip to
    // reshuffle onto a target).  That residual sits inside §9.1's accepted
    // bound — targeting only helps a caller who has ALREADY compromised a
    // quorum, which §9.1 accepts can bias its own outputs; targeting
    // amplifies that, it grants no new capability (cannot make an honest
    // quorum lie, cannot forge).  The sharper residual is LIFECYCLE
    // manipulation (avoid a quorum -> force its reform; pin it -> prevent
    // reform).  The structural fix is commit-reveal (selection resolved by
    // the inclusion block, which does NOT exist at this point in the flow —
    // selection precedes the tx), recorded as the escape hatch.
    //
    // ★ SORTED BY quorum_hash before indexing: GetActiveQuorumsAtHeight's
    // iteration order is a storage detail and MUST NOT leak into the
    // selection, or two nodes could route the same roll differently.
    // ------------------------------------------------------------------
    std::vector<const CPTXQuorumRecord*> ordered;
    ordered.reserve(active.size());
    for (const auto& rec : active) ordered.push_back(&rec);
    std::sort(ordered.begin(), ordered.end(),
              [](const CPTXQuorumRecord* a, const CPTXQuorumRecord* b) {
                  return a->quorum_hash < b->quorum_hash;
              });

    // Index from the tip hash's low 8 bytes (uint256 is little-endian
    // internally; any fixed slice is fine — it must only be deterministic).
    uint64_t sel = 0;
    for (int i = 0; i < 8; i++)
        sel = (sel << 8) | (uint64_t)(*(tip_hash.begin() + i));
    const CPTXQuorumRecord* best = ordered[sel % ordered.size()];

    if (best == nullptr)
        return ctx;

    ctx.quorum_hash = best->quorum_hash;

    // QUORUM-SCOPED threshold: t = majority(formed_size).  Mirrors
    // PTX_BLS_Threshold (rpc/ptx.cpp:57, n/2+1) — the DKG ceremony bakes t=6 for
    // its 11-member quorums (KDD-048).  ODC-036 addendum: this equivalence
    // (t == n/2+1) is a STOPGAP; KDD-048 pre-documents a t=7-at-n=11 upgrade
    // where the derivation would silently return 6 for a ceremony that baked 7
    // — the durable fix persists t in the record.
    ctx.threshold = (int)best->formed_size / 2 + 1;

    if (best->group_pk_bytes.size() != 48)
        return ctx;                 // present but unusable -> caller hard-errors

    for (const auto& m : best->members) {
        if (!m.in_qual) continue;   // only committed effective-QUAL members hold usable shares
        if (m.share_index == 0) continue; // 1-based by construction; 0 means unset
        ctx.member_ids.push_back(m.node_id);
        ctx.share_index[m.node_id] = (int)m.share_index;
    }
    if ((int)ctx.member_ids.size() < ctx.threshold) {
        ctx.member_ids.clear();
        ctx.share_index.clear();
        return ctx;                 // present but sub-threshold -> caller hard-errors
    }

    ctx.group_pk = best->group_pk_bytes;
    ctx.active   = true;
    return ctx;
}

// ---------------------------------------------------------------------------
// KDD-070 P2 — startup reconciliation orchestration (§3 ordering).
// ---------------------------------------------------------------------------

int PTX_WarnMissingSharesForNode(const std::vector<CPTXQuorumRecord>& active,
                                 const std::string& node_id)
{
    if (node_id.empty()) return 0;
    const std::set<uint256> held = PTX_BLS_HeldQuorumHashes();
    int warned = 0;
    for (const CPTXQuorumRecord& rec : active) {
        for (const PTXQuorumMemberRecord& m : rec.members) {
            if (m.node_id == node_id && m.in_qual && held.count(rec.quorum_hash) == 0) {
                LogPrintf("PTX P2: WARNING: chain lists this node (%s) in_qual for ACTIVE "
                          "quorum %s but NO share is held (degraded, ODC-035) — the quorum "
                          "may fall below threshold; awaiting re-selection / W2.4\n",
                          node_id, rec.quorum_hash.ToString());
                ++warned;
                break; // one warning per quorum
            }
        }
    }
    return warned;
}

void PTX_ReconcileHeldSharesOnStart()
{
    // ★ §3 ORDERING (load-bearing): this runs from LoadTierTwo (init.cpp), AFTER
    // InitTierTwoPreChainLoad constructed ptxQuorumStore/evoDb (tiertwo/init.cpp:76)
    // AND AFTER LoadChainTip (init.cpp) validated evoDb against the active tip.
    // Running it before the store is populated would make EVERY held quorum_hash
    // look orphaned (HasQuorumRecord=false) and wipe every member — the
    // empty-known-set failure mode (Slot_Reconcile_EmptySetDiscardsAll).
    if (ptxQuorumStore == nullptr || evoDb == nullptr) return;

    // 1. Load persisted shares from disk into the in-memory store.
    const int corrupt = PTX_BLS_LoadShares(*evoDb);
    if (corrupt > 0)
        LogPrintf("PTX P2: reconcile on start: %d CORRUPT persisted share(s) found (see ERROR "
                  "lines above for the affected quorum_hashes)\n", corrupt);

    // 1b. WIPE (KDD-070 P2): the -ptxwipeshares startup flag clears ALL held
    // shares (memory + disk) and stops — for restore_fleet.sh after a bank-restore
    // to a pre-formation snapshot. Load-then-wipe so the on-disk entries (now in
    // memory) are erased. NOT RPC-reachable: a restart-gated flag, not a live call.
    if (gArgs.GetBoolArg("-ptxwipeshares", false)) {
        const size_t n = PTX_BLS_WipeShares(evoDb.get());
        LogPrintf("PTX P2: -ptxwipeshares set — wiped %u held share(s); skipping reconcile\n",
                  (unsigned)n);
        return;
    }

    // 2. Reconcile: keep held shares whose quorum_hash is a record on the active
    //    chain (any role); discard orphans. Build the known set from the held
    //    keys checked against the store — deliberately NOT an empty set.
    const std::set<uint256> held = PTX_BLS_HeldQuorumHashes();
    std::set<uint256> known;
    {
        LOCK(cs_main); // HasQuorumRecord reads evoDb; keep chain access consistent
        for (const uint256& qh : held)
            if (ptxQuorumStore->HasQuorumRecord(qh)) known.insert(qh);
    }
    const size_t dropped = PTX_BLS_ReconcileShares(known, evoDb.get());
    LogPrintf("PTX P2: reconcile on start: %u held, %u known-on-chain, %u orphan(s) discarded\n",
              (unsigned)held.size(), (unsigned)known.size(), (unsigned)dropped);

    // 3. "Holds nothing, in_qual" warning over ACTIVE quorums at the tip.
    std::vector<CPTXQuorumRecord> active;
    int tipHeight = 0;
    {
        LOCK(cs_main);
        tipHeight = chainActive.Height();
        if (tipHeight > 0) active = ptxQuorumStore->GetActiveQuorumsAtHeight(tipHeight);
    }
    PTX_WarnMissingSharesForNode(active, g_ptx_my_node_id);
}
