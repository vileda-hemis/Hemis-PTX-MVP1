// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_COMMIT_REVEAL_H
#define HEMIS_PTX_COMMIT_REVEAL_H

#include "sync.h"
#include "uint256.h"

#include <map>
#include <set>
#include <string>
#include <vector>

enum class PTXRoundState {
    COMMIT_PHASE,
    REVEAL_PHASE,
    RESOLVED,
    FAILED
};

struct PTXCommitRevealRound {
    std::string round_id;
    uint256 round_seed;
    PTXRoundState state{PTXRoundState::COMMIT_PHASE};
    std::map<std::string, uint256> commits;
    std::set<std::string> committed_nodes;
    std::map<std::string, uint256> reveals;
    std::vector<std::string> quorum_members;
    int threshold{3};
    uint256 beacon;
    std::vector<std::string> withheld_nodes;
    std::vector<std::string> abstained_nodes;
    std::vector<std::string> invalid_reveal_nodes;
    // payload parameters — set by coordinator at round creation
    uint32_t count{0};
    int64_t low{0};
    int64_t high{0};
    bool unique{false};
    std::vector<int64_t> exclude_integers;
    std::vector<std::string> exclude_txids;

    // BLS threshold signature fields (Phase 2)
    // node_id -> 96-byte partial BLS signature (G2 element serialized)
    std::map<std::string, std::vector<uint8_t>> bls_partial_sigs;
    // 96-byte recovered threshold signature (set when round resolves via BLS)
    std::vector<uint8_t> threshold_sig;
};

extern std::map<std::string, PTXCommitRevealRound> g_ptx_rounds;
extern RecursiveMutex cs_ptx_rounds;

// commitment = SHA256(secret || seed)
bool PTX_VerifyCommitment(const uint256& commit, const uint256& secret, const uint256& seed);

// Record a commit from node_id. Node must be in quorum_members; round must not be terminal.
bool PTX_SubmitCommit(PTXCommitRevealRound& round, const std::string& node_id, const uint256& commit);

// Transition to REVEAL_PHASE if committed_nodes >= threshold, else FAILED.
bool PTX_ForceRevealPhase(PTXCommitRevealRound& round);

// Record a reveal from node_id. Verifies against stored commitment and round_seed.
bool PTX_SubmitReveal(PTXCommitRevealRound& round, const std::string& node_id, const uint256& secret);

// Compute beacon from reveals if >= threshold valid reveals. Classifies non-revealers.
bool PTX_TryResolve(PTXCommitRevealRound& round);

#endif // HEMIS_PTX_COMMIT_REVEAL_H
