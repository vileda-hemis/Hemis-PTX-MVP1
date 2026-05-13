// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_output_mapping.h"

#include "crypto/common.h"
#include "crypto/sha256.h"

#include <stdexcept>

std::vector<uint8_t> PTX_ExpandBeacon(const uint256& beacon, size_t needed_bytes)
{
    std::vector<uint8_t> result;
    result.reserve(needed_bytes + 32);
    uint256 prev = beacon;
    uint32_t idx = 0;
    while (result.size() < needed_bytes) {
        CSHA256 h;
        h.Write(prev.begin(), 32);
        uint8_t ib[4];
        WriteLE32(ib, idx++);
        h.Write(ib, 4);
        uint256 block;
        h.Finalize(block.begin());
        result.insert(result.end(), block.begin(), block.end());
        prev = block;
    }
    result.resize(needed_bytes);
    return result;
}

bool PTX_SampleOne(const std::vector<uint8_t>& exp, size_t& offset,
                   int64_t low, int64_t high, int64_t& result)
{
    uint64_t pool = (uint64_t)(high - low + 1);
    // Largest multiple of pool that fits in uint64 — reject values >= threshold to avoid bias.
    uint64_t threshold = (UINT64_MAX / pool) * pool;
    while (offset + 8 <= exp.size()) {
        uint64_t chunk = ReadLE64(exp.data() + offset);
        offset += 8;
        if (chunk < threshold) {
            result = low + (int64_t)(chunk % pool);
            return true;
        }
    }
    return false;
}

std::vector<int64_t> PTX_MapBeacon(const uint256& beacon, uint32_t count,
                                   int64_t low, int64_t high, bool unique,
                                   const std::set<int64_t>& exclude_set)
{
    if (!unique) {
        // KDD-009: independent draws with rejection sampling.
        // Re-expand beacon each time material is exhausted.
        size_t expand_mult = 128;
        auto exp = PTX_ExpandBeacon(beacon, (size_t)count * expand_mult);
        size_t off = 0;
        std::vector<int64_t> res;
        uint32_t stall = 0;
        while (res.size() < count) {
            int64_t v = 0;
            if (!PTX_SampleOne(exp, off, low, high, v)) {
                expand_mult *= 2;
                exp = PTX_ExpandBeacon(beacon, (size_t)count * expand_mult);
                off = 0;
                continue;
            }
            if (exclude_set.count(v) == 0) {
                res.push_back(v);
                stall = 0;
            } else {
                if (++stall > 100000)
                    throw std::runtime_error("PTX: exclude set covers entire range");
            }
        }
        return res;
    } else {
        // KDD-009: Fisher-Yates shuffle over the eligible pool (KDD-024).
        std::vector<int64_t> pool;
        for (int64_t v = low; v <= high; v++)
            if (!exclude_set.count(v)) pool.push_back(v);
        if ((int64_t)pool.size() < (int64_t)count)
            throw std::runtime_error("PTX: pool too small for unique draw");
        // 32 bytes per pool slot is sufficient for unbiased index sampling.
        auto exp = PTX_ExpandBeacon(beacon, pool.size() * 32);
        size_t off = 0;
        for (uint32_t i = 0; i < count; i++) {
            int64_t j = i;
            PTX_SampleOne(exp, off, (int64_t)i, (int64_t)pool.size() - 1, j);
            std::swap(pool[i], pool[j]);
        }
        return std::vector<int64_t>(pool.begin(), pool.begin() + count);
    }
}
