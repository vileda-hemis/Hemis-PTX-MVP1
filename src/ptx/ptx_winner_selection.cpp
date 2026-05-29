// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_winner_selection.h"

#include "compat/endian.h"
#include "crypto/sha256.h"
#include "serialize.h"
#include "utilstrencodings.h"

#include <algorithm>
#include <cstring>
#include <vector>

uint256 PTX_ComputeSelectionEntropy(int height, const uint256& blockHash)
{
    // ODC-022 §5.3: SHA256("PTX-LOTTERY-PAYOUT-" || height_uint32_le || block_hash)
    // Single SHA256 (CSHA256), domain-separated for safety.
    static const char* domain = "PTX-LOTTERY-PAYOUT-";
    const uint32_t heightLE = htole32((uint32_t)height);

    uint256 result;
    CSHA256 h;
    h.Write((const unsigned char*)domain, strlen(domain));
    h.Write((const unsigned char*)&heightLE, 4);
    h.Write(blockHash.begin(), 32);
    h.Finalize(result.begin());
    return result;
}

Optional<CScript> PTX_SelectWinner(const CDeterministicGMList& gmList,
                                    const PTXPoSeTracker& poseTracker,
                                    const uint256& selectionEntropy)
{
    // §5.2 eligibility filter + §5.3 ticket-weighted selection.
    struct Candidate {
        std::string node_id;
        int         tickets;
        CScript     script;
    };
    std::vector<Candidate> eligible;

    gmList.ForEachGM(/*onlyValid=*/false, [&](const CDeterministicGMCPtr& dgm) {
        // Amendment 2: skip v1/v2 records (empty node_id) — ineligible by construction.
        // Must be checked before touching the pose tracker to avoid polluting it.
        if (dgm->pdgmState->node_id.empty()) return;
        if (dgm->pdgmState->scriptPTXPayment.empty()) return;

        const PTXNodeRecord& rec = poseTracker.GetRecord(dgm->pdgmState->node_id);
        if (!rec.quorum_eligible) return;
        if (rec.lottery_tickets <= 0) return;

        eligible.push_back({dgm->pdgmState->node_id, rec.lottery_tickets,
                            dgm->pdgmState->scriptPTXPayment});
    });

    if (eligible.empty()) return nullopt;   // rollover: no eligible GMs

    // §5.3: sort deterministically by node_id (KDD-030 convention)

    // Build cumulative ticket distribution
    std::vector<std::pair<int, size_t>> cumulative;  // (running_sum, candidate_idx)
    int running = 0;
    for (size_t i = 0; i < eligible.size(); ++i) {
        running += eligible[i].tickets;
        cumulative.emplace_back(running, i);
    }
    const int total_tickets = running;

    // Derive winning ticket from the first 8 bytes of entropy (little-endian uint64)
    uint64_t entropyLE;
    memcpy(&entropyLE, selectionEntropy.begin(), sizeof(entropyLE));
    entropyLE = le64toh(entropyLE);
    const int winning_ticket = (int)(entropyLE % (uint64_t)total_tickets) + 1;  // 1..total_tickets

    // Lower-bound binary search: first entry where cumulative sum >= winning_ticket
    size_t idx = 0;
    {
        size_t lo = 0, hi = cumulative.size();
        while (lo < hi) {
            size_t mid = lo + (hi - lo) / 2;
            if (cumulative[mid].first < winning_ticket) lo = mid + 1;
            else                                        hi = mid;
        }
        idx = lo;
    }
    return eligible[cumulative[idx].second].script;
}
