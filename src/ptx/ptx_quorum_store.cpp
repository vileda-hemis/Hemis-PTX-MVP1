// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_quorum_store.h"

#include "chain.h"
#include "compat/endian.h"
#include "consensus/validation.h"
#include "evo/deterministicgms.h"
#include "evo/evodb.h"
#include "logging.h"
#include "primitives/block.h"
#include "primitives/transaction.h" // GetTxPayload
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_formation.h"
#include "util/system.h" // error()
#include "validation.h"  // LookupBlockIndex

#include <limits>
#include <set>

std::unique_ptr<CPTXQuorumStore> ptxQuorumStore;

// evodb key prefixes (LLMQ mined-commitment scheme, PTX flavor):
//   ("pq_r", quorum_hash)                          -> CPTXQuorumRecord
//   ("pq_h", htobe32(UINT32_MAX - mined_height))   -> quorum_hash
static const std::string DB_PTXDKG_QUORUM   = "pq_r";
static const std::string DB_PTXDKG_BY_INV_H = "pq_h";

// mined_height inversed + big-endian so evodb iterates most-recent-first
// (llmq/quorums_blockprocessor BuildInversedHeightKey precedent).
static std::pair<std::string, uint32_t> BuildPTXInversedHeightKey(int nMinedHeight)
{
    return std::make_pair(DB_PTXDKG_BY_INV_H,
                          htobe32(std::numeric_limits<uint32_t>::max() - nMinedHeight));
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
    const CDeterministicGMList dgmList = deterministicGMManager->GetListForBlock(pindexQuorum);
    // SG-1a: the connect-time materialization/containment guard reconstructs
    // through the SAME KDD-040 pool as V5 and formation (the one-function
    // contract now includes the pool). THE MISSED-SITE LESSON (2026-07-13,
    // caught live by battery_sg1 row v5(y)): with V5 pool-aware and this
    // guard raw, a valid PTXDKG passed populate + assembler and was REJECTED
    // at connect ("11 committed, 4 matched") — a self-poisoning divergence.
    // All THREE reconstruction sites (V5, debug builder, this guard) must
    // share PTX_Formation_BuildPool.
    const CDeterministicGMList formationPool =
        PTX_Formation_BuildPool(dgmList,
                                GetActiveQuorumsAtHeight(pindexQuorum->nHeight));
    const std::vector<CDeterministicGMCPtr> quorum11 =
        PTX_DKG_SelectQuorumFromList(formationPool, payload.quorum_hash);

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

    evoDb.Write(std::make_pair(DB_PTXDKG_QUORUM, rec.quorum_hash), rec);
    evoDb.Write(BuildPTXInversedHeightKey(rec.mined_height), rec.quorum_hash);
    {
        LOCK(cs);
        recordCache[rec.quorum_hash] = rec;
    }
    // T-E consumption hook: no-op until W2.2 produces FORMING entries.
    ConsumeFormingOnConnect(rec.quorum_hash);

    LogPrintf("%s: persisted PTXDKG quorum record. quorum_hash=%s anchor_height=%d "
              "mined_height=%d formed=%d completed=%d txid=%s\n", __func__,
              rec.quorum_hash.ToString(), rec.formation_height, rec.mined_height,
              (int)rec.formed_size, (int)rec.completed_size, rec.accepted_txid.ToString());
    return true;
}

bool CPTXQuorumStore::UndoBlock(const CBlock& block, const CBlockIndex* pindex)
{
    AssertLockHeld(cs_main);

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
        if (GetQuorumRecord(qh, rec) &&
            rec.state == static_cast<uint8_t>(PTXQuorumState::ACTIVE)) {
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

PTXDKGSigningCtx PTX_SelectDKGSigningCtx(const std::vector<CPTXQuorumRecord>& active)
{
    PTXDKGSigningCtx ctx;
    if (active.empty())
        return ctx;                 // no quorum at all -> legitimate dealer fallback

    ctx.quorum_present = true;      // from here, a load failure is FAIL-CLOSED

    // PROVISIONAL selection rule (KDD-066, owed to W2.1 — see header): highest
    // formation_height; ties broken by LOWEST quorum_hash lexicographically so
    // the choice is deterministic across nodes rather than iteration-ordered.
    const CPTXQuorumRecord* best = nullptr;
    for (const auto& rec : active) {
        if (best == nullptr ||
            rec.formation_height > best->formation_height ||
            (rec.formation_height == best->formation_height &&
             rec.quorum_hash < best->quorum_hash)) {
            best = &rec;
        }
    }
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
