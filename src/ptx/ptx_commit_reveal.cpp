// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_commit_reveal.h"

#include "crypto/sha256.h"
#include "logging.h"

#include <algorithm>

std::map<std::string, PTXCommitRevealRound> g_ptx_rounds;
RecursiveMutex cs_ptx_rounds;

bool PTX_VerifyCommitment(const uint256& commit, const uint256& secret, const uint256& seed)
{
    CSHA256 h;
    h.Write(secret.begin(), 32);
    h.Write(seed.begin(), 32);
    uint256 expected;
    h.Finalize(expected.begin());
    return expected == commit;
}

bool PTX_SubmitCommit(PTXCommitRevealRound& round, const std::string& node_id, const uint256& commit)
{
    if (round.state == PTXRoundState::RESOLVED || round.state == PTXRoundState::FAILED) {
        return false;
    }
    if (std::find(round.quorum_members.begin(), round.quorum_members.end(), node_id)
            == round.quorum_members.end()) {
        return false;
    }
    round.commits[node_id] = commit;
    round.committed_nodes.insert(node_id);
    LogPrintf("PTX: commit recorded round=%s node=%s\n", round.round_id, node_id);
    return true;
}

bool PTX_ForceRevealPhase(PTXCommitRevealRound& round)
{
    if ((int)round.committed_nodes.size() < round.threshold) {
        LogPrintf("PTX: ForceRevealPhase failed — commits=%d threshold=%d round=%s\n",
                  (int)round.committed_nodes.size(), round.threshold, round.round_id);
        round.state = PTXRoundState::FAILED;
        return false;
    }
    round.state = PTXRoundState::REVEAL_PHASE;
    LogPrintf("PTX: reveal phase started round=%s commits=%d\n",
              round.round_id, (int)round.committed_nodes.size());
    return true;
}

bool PTX_SubmitReveal(PTXCommitRevealRound& round, const std::string& node_id, const uint256& secret)
{
    if (round.committed_nodes.count(node_id) == 0) {
        return false;
    }
    if (round.reveals.count(node_id)) {
        return false;
    }
    if (!PTX_VerifyCommitment(round.commits[node_id], secret, round.round_seed)) {
        round.invalid_reveal_nodes.push_back(node_id);
        LogPrintf("PTX: invalid reveal round=%s node=%s\n", round.round_id, node_id);
        return false;
    }
    round.reveals[node_id] = secret;
    LogPrintf("PTX: reveal accepted round=%s node=%s\n", round.round_id, node_id);
    return true;
}

bool PTX_TryResolve(PTXCommitRevealRound& round)
{
    if ((int)round.reveals.size() < round.threshold) {
        return false;
    }

    for (const auto& node_id : round.quorum_members) {
        if (round.reveals.count(node_id)) continue;
        if (round.committed_nodes.count(node_id)) {
            round.withheld_nodes.push_back(node_id);
        } else {
            round.abstained_nodes.push_back(node_id);
        }
    }

    // Beacon = SHA256(reveals[quorum_members in order] || round_seed)
    CSHA256 hasher;
    for (const auto& node_id : round.quorum_members) {
        if (round.reveals.count(node_id)) {
            hasher.Write(round.reveals[node_id].begin(), 32);
        }
    }
    hasher.Write(round.round_seed.begin(), 32);
    hasher.Finalize(round.beacon.begin());

    round.state = PTXRoundState::RESOLVED;
    LogPrintf("PTX: resolved round=%s beacon=%s withheld=%d abstained=%d\n",
              round.round_id, round.beacon.GetHex(),
              (int)round.withheld_nodes.size(), (int)round.abstained_nodes.size());
    return true;
}
