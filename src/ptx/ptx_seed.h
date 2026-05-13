// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_SEED_H
#define HEMIS_PTX_SEED_H

#include "uint256.h"

#include <cstdint>
#include <string>
#include <vector>

// KDD-015: nonce = SHA256(prev_beacon || caller_salt). All-zeros prev_beacon for round 0.
uint256 PTX_BuildNonce(const uint256& prev_beacon, const std::vector<uint8_t>& caller_salt);

// KDD-002: round_seed = SHA256(game_id || block_height_LE4 || caller_pubkey || nonce || params_hash)
// KDD-003: same block = same seed = same result (rate limiter)
uint256 PTX_BuildRoundSeed(const std::string& game_id, uint32_t block_height,
                            const std::vector<uint8_t>& caller_pubkey,
                            const uint256& nonce, const uint256& params_hash);

// Deterministic hash of draw parameters for inclusion in seed and round_id.
uint256 PTX_HashParams(uint32_t count, int64_t low, int64_t high, bool unique,
                        const std::vector<int64_t>& exc_ints,
                        const std::vector<std::string>& exc_txids);

// Unique round identifier: hex-prefix of SHA256(game_id || block_height_LE4 || params_hash || rand8).
// The 8 random bytes ensure uniqueness for concurrent same-block same-game_id calls.
std::string PTX_MakeRoundId(const std::string& game_id, uint32_t block_height,
                              const uint256& params_hash);

#endif // HEMIS_PTX_SEED_H
