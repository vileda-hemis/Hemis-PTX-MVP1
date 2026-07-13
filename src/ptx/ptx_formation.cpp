// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_formation.h"

#include "chain.h"

#include <set>

CDeterministicGMList PTX_Formation_BuildPool(
        const CDeterministicGMList& listAtAnchor,
        const std::vector<CPTXQuorumRecord>& activeAtAnchor)
{
    // D-SG1a-1: the exclusion set is the FULL formed-11 of every ACTIVE
    // record (QUAL or not), keyed by proTxHash.
    std::set<uint256> active_members;
    for (const auto& rec : activeAtAnchor) {
        for (const auto& m : rec.members) {
            active_members.insert(m.proTxHash);
        }
    }

    // KDD-060 filter-then-score, extended by the KDD-040 exclusion: build
    // the pool BEFORE any scoring so excluded GMs never enter the
    // competition (a post-score drop would change selection and split the
    // chain). The eligibility predicate is the shared PTX_DKG_IsGMPTXEligible
    // — never re-inlined.
    CDeterministicGMList pool;
    listAtAnchor.ForEachGM(true, [&](const CDeterministicGMCPtr& dgm) {
        if (!PTX_DKG_IsGMPTXEligible(dgm))
            return;
        if (active_members.count(dgm->proTxHash))
            return;
        pool.AddGM(dgm);
    });
    return pool;
}

bool PTX_Formation_SelectAtAnchor(const CBlockIndex* pindexAnchor,
                                  std::vector<PTXDKGMember>& membersOut)
{
    membersOut.clear();
    if (pindexAnchor == nullptr)
        return false;

    // SNAPSHOT-NOT-LIVE: both reads are AT THE ANCHOR. GetListForBlock is
    // the same idiom as the three existing selection sites
    // (specialtx_validation.cpp V5, ptx_quorum_store connect-time
    // materialization, the anchored debug populate probe). Never
    // GetListAtChainTip.
    const CDeterministicGMList listAtAnchor =
            deterministicGMManager->GetListForBlock(pindexAnchor);

    // ptxQuorumStore is initialized before validation in production; the
    // null tolerance is unit-test-environment-only and means "no active
    // quorums", which is exactly the zero-ACTIVE state those tests model.
    std::vector<CPTXQuorumRecord> activeAtAnchor;
    if (ptxQuorumStore) {
        activeAtAnchor =
                ptxQuorumStore->GetActiveQuorumsAtHeight(pindexAnchor->nHeight);
    }

    const CDeterministicGMList pool =
            PTX_Formation_BuildPool(listAtAnchor, activeAtAnchor);

    // The pool >= 11 gate: under-threshold is a DETERMINISTIC SKIP (this
    // cycle simply does not form), not an error.
    if (pool.GetValidGMsCount() < 11)
        return false;

    membersOut = PTX_DKG_BuildMemberVectorFromList(
            pool, pindexAnchor->GetBlockHash());
    return membersOut.size() == 11;
}
