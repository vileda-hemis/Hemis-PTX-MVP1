// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_dkg_commitments.h"

#include "chain.h"
#include "evo/specialtx_validation.h" // CheckPTXDKGTx
#include "logging.h"
#include "net.h"                      // g_connman, CInv, RelayInv
#include "primitives/block.h"
#include "protocol.h"                 // MSG_PTX_DKG_COMMITMENT
#include "ptx/ptx_dkg.h"              // PTXDKGPayload
#include "streams.h"
#include "sync.h"
#include "tiertwo/tiertwo_sync_state.h"
#include "validation.h"               // cs_main, chainActive

#include <map>

namespace {
// Leaf mutex (minableCommitmentsCs precedent); lock order cs_main → store —
// the same order the superseded pending slot used.
Mutex g_ptx_dkg_commitments_cs;
// txHash → finished nType=11 tx.
std::map<uint256, CTransactionRef> g_commitments GUARDED_BY(g_ptx_dkg_commitments_cs);
// quorum_hash → txHash (the per-quorum index; replacement policy lives here).
std::map<uint256, uint256> g_by_quorum GUARDED_BY(g_ptx_dkg_commitments_cs);

// Effective-QUAL size from the payload — the "signers" count for the
// better-completed replacement policy (HasBetterMinableCommitment mirror).
bool GetQuorumAndCount(const CTransaction& tx, uint256& quorumHashOut, size_t& countOut)
{
    PTXDKGPayload pl;
    if (!GetTxPayload(tx, pl)) return false;
    quorumHashOut = pl.quorum_hash;
    countOut = pl.member_node_ids.size();
    return true;
}

// Insert under the per-quorum replacement policy.  Caller holds the store
// lock.  Returns false (with reason) when refused as not-better.
bool InsertLocked(const CTransactionRef& tx, const uint256& quorumHash, size_t count,
                  std::string& refuseReasonOut) EXCLUSIVE_LOCKS_REQUIRED(g_ptx_dkg_commitments_cs)
{
    const uint256 txHash = tx->GetHash();
    if (g_commitments.count(txHash)) {
        refuseReasonOut = "duplicate";
        return false;
    }
    auto it = g_by_quorum.find(quorumHash);
    if (it != g_by_quorum.end()) {
        auto jt = g_commitments.find(it->second);
        if (jt != g_commitments.end()) {
            uint256 q;
            size_t existingCount = 0;
            if (GetQuorumAndCount(*jt->second, q, existingCount) && existingCount >= count) {
                refuseReasonOut = "not-better";
                return false;
            }
            g_commitments.erase(jt);
        }
        it->second = txHash;
    } else {
        g_by_quorum.emplace(quorumHash, txHash);
    }
    g_commitments.emplace(txHash, tx);
    return true;
}

void RelayCommitment(const uint256& txHash)
{
    if (g_connman == nullptr) return;
    CInv inv(MSG_PTX_DKG_COMMITMENT, txHash); // RelayInv takes CInv& (lineage signature)
    g_connman->RelayInv(inv);
}
} // namespace

bool PTX_DKG_Commitments_AddAndRelay(const CTransactionRef& tx, CValidationState& state)
{
    LOCK(cs_main);

    // VALIDATE-BEFORE-STORE (the slot's populate-half, kept): null tip (unit
    // tests) runs the structural body only.
    if (!CheckPTXDKGTx(*tx, chainActive.Tip(), state)) {
        LogPrintf("PTX DKG: %s: refusing commitment %s (%s)\n", __func__,
                  tx->GetHash().ToString(), state.GetRejectReason());
        return false;
    }

    uint256 quorumHash;
    size_t count = 0;
    if (!GetQuorumAndCount(*tx, quorumHash, count)) {
        return state.Invalid(false, REJECT_INVALID, "ptxdkg-commitment-bad-payload");
    }

    {
        LOCK(g_ptx_dkg_commitments_cs);
        std::string reason;
        if (!InsertLocked(tx, quorumHash, count, reason)) {
            if (reason == "duplicate") return true; // already held — benign no-op
            return state.Invalid(false, REJECT_DUPLICATE, "ptxdkg-commitment-not-better");
        }
    }
    LogPrintf("PTX: minable commitment stored+relayed tx=%s quorum_hash=%s completed=%d\n",
              tx->GetHash().ToString(), quorumHash.ToString(), (int)count);
    RelayCommitment(tx->GetHash());
    return true;
}

void PTX_DKG_Commitments_ForceAdd(const CTransactionRef& tx)
{
    uint256 quorumHash;
    size_t count = 0;
    const bool havePayload = GetQuorumAndCount(*tx, quorumHash, count);
    {
        LOCK(g_ptx_dkg_commitments_cs);
        if (havePayload) {
            auto it = g_by_quorum.find(quorumHash);
            if (it != g_by_quorum.end()) g_commitments.erase(it->second);
            g_by_quorum[quorumHash] = tx->GetHash();
        }
        g_commitments[tx->GetHash()] = tx;
    }
    LogPrintf("PTX DKG: %s: FORCE-stored commitment %s (guards bypassed)\n", __func__,
              tx->GetHash().ToString());
    RelayCommitment(tx->GetHash());
}

bool PTX_DKG_Commitments_ProcessMessage(NodeId from, CDataStream& vRecv, int& retMisbehavingScore)
{
    retMisbehavingScore = 0;

    CTransactionRef tx;
    try {
        vRecv >> tx;
    } catch (const std::exception&) {
        retMisbehavingScore = 100;
        return false;
    }
    if (tx == nullptr || !tx->IsPTXDKGTx()) {
        retMisbehavingScore = 100;
        return false;
    }

    {
        LOCK(cs_main);
        if (g_connman) g_connman->RemoveAskFor(tx->GetHash(), MSG_PTX_DKG_COMMITMENT);
    }

    // Sync soft-gate (the GMAUTH posture, gmauth.cpp:62): we cannot judge a
    // commitment against a chain we do not have — ignore without DoS.
    if (!g_tiertwo_sync_state.IsBlockchainSynced()) {
        return true;
    }

    CValidationState state;
    if (!PTX_DKG_Commitments_AddAndRelay(tx, state)) {
        // Contextual reject: possibly a stale view on either side — small
        // score, mirroring the lineage's not-up-to-date leniency.  A
        // not-better refusal is score-free (normal gossip overlap).
        if (state.GetRejectReason() != "ptxdkg-commitment-not-better") {
            retMisbehavingScore = 10;
        }
        LogPrint(BCLog::NET, "PTX: commitment from peer=%d refused (%s)\n",
                 from, state.GetRejectReason());
    }
    return true;
}

bool PTX_DKG_Commitments_Has(const uint256& txHash)
{
    LOCK(g_ptx_dkg_commitments_cs);
    return g_commitments.count(txHash) != 0;
}

bool PTX_DKG_Commitments_GetByHash(const uint256& txHash, CTransactionRef& ret)
{
    LOCK(g_ptx_dkg_commitments_cs);
    auto it = g_commitments.find(txHash);
    if (it == g_commitments.end()) return false;
    ret = it->second;
    return true;
}

bool PTX_DKG_Commitments_GetMinableTx(const CBlockIndex* pindexPrev, CTransactionRef& ret)
{
    AssertLockHeld(cs_main);
    LOCK(g_ptx_dkg_commitments_cs);

    // Generate-time RE-VALIDATION against the assembler's captured anchor —
    // the reorg guard, kept verbatim from the superseded slot.  KEEP-BUT-SKIP:
    // a failing entry survives a transient reorg-and-back; erase-on-mined and
    // the debug clear are the removal paths.
    for (const auto& it : g_commitments) {
        CValidationState state;
        if (!CheckPTXDKGTx(*it.second, pindexPrev, state)) {
            LogPrintf("PTX DKG: %s: commitment %s skipped at generate time (%s) — kept\n",
                      __func__, it.first.ToString(), state.GetRejectReason());
            continue;
        }
        ret = it.second;
        return true;
    }
    return false;
}

void PTX_DKG_Commitments_EraseMined(const CBlock& block)
{
    LOCK(g_ptx_dkg_commitments_cs);
    for (const auto& tx : block.vtx) {
        if (!tx->IsPTXDKGTx()) continue;
        uint256 quorumHash;
        size_t count = 0;
        if (GetQuorumAndCount(*tx, quorumHash, count)) {
            auto it = g_by_quorum.find(quorumHash);
            // Erase the whole quorum entry: once ANY commitment for this
            // quorum is mined, competing minables for it are moot.
            if (it != g_by_quorum.end()) {
                g_commitments.erase(it->second);
                g_by_quorum.erase(it);
            }
        }
        if (g_commitments.erase(tx->GetHash())) {
            LogPrintf("PTX: minable commitment %s mined — erased from store\n",
                      tx->GetHash().ToString());
        }
    }
}

void PTX_DKG_Commitments_Clear()
{
    LOCK(g_ptx_dkg_commitments_cs);
    if (!g_commitments.empty()) {
        LogPrintf("PTX DKG: %s: minable-commitments store cleared (%d entries)\n",
                  __func__, (int)g_commitments.size());
    }
    g_commitments.clear();
    g_by_quorum.clear();
}

bool PTX_DKG_Commitments_Empty()
{
    LOCK(g_ptx_dkg_commitments_cs);
    return g_commitments.empty();
}
