// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_OUTPUT_MAPPING_H
#define HEMIS_PTX_OUTPUT_MAPPING_H

#include "uint256.h"

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

// KDD-024: Expand beacon into a stream of pseudorandom bytes via chained SHA256.
// Each 32-byte block = SHA256(prev_block || idx_LE32). Produces at least needed_bytes.
std::vector<uint8_t> PTX_ExpandBeacon(const uint256& beacon, size_t needed_bytes);

// KDD-009: Rejection sampling — consume 8 bytes at a time from exp[offset..].
// Returns false if exp is exhausted before a valid sample is found.
bool PTX_SampleOne(const std::vector<uint8_t>& exp, size_t& offset,
                   int64_t low, int64_t high, int64_t& result);

// KDD-009/KDD-024: Map beacon to count integers in [low, high] (inclusive, KDD-026).
// unique=true: Fisher-Yates shuffle over the eligible pool.
// unique=false: independent rejection-sampled draws.
// exclude_set: values to skip (KDD-025).
std::vector<int64_t> PTX_MapBeacon(const uint256& beacon, uint32_t count,
                                   int64_t low, int64_t high, bool unique,
                                   const std::set<int64_t>& exclude_set);

#endif // HEMIS_PTX_OUTPUT_MAPPING_H
