// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx_lottery_state.h"

#include "evo/evodb.h"
#include "sync.h"
#include "util/system.h"
#include "validation.h"

#include <algorithm>

static const std::string DB_LOTTERY_STATE_SNAP = "ls_S";
static const std::string DB_LOTTERY_SNAP_HASHES = "ls_H";   // legacy whole-vector bookkeeping, migrated away
static const std::string DB_LOTTERY_SNAP_INDEX = "ls_I";    // ("ls_I", height) -> hashes snapshotted at that height

static LotteryState g_lotteryState;

LotteryState& GetLotteryState()
{
    AssertLockHeld(cs_main);
    return g_lotteryState;
}

void LoadLotteryStateFromDB(const uint256& tipHash)
{
    LOCK(cs_main);
    if (tipHash.IsNull() || !evoDb->Read(std::make_pair(DB_LOTTERY_STATE_SNAP, tipHash), g_lotteryState)) {
        g_lotteryState.Reset();
        LogPrintf("PTX LotteryState: no snapshot at tip, starting fresh\n");
    } else {
        LogPrintf("PTX LotteryState: loaded from tip %s, accumulator %s value=%lld\n",
            tipHash.GetHex(),
            g_lotteryState.accumulator_outpoint.IsNull() ? "null" : g_lotteryState.accumulator_outpoint.hash.GetHex(),
            g_lotteryState.accumulator_value);
    }
}

void WriteLotteryStateSnapshotForBlock(const uint256& blockHash, int nHeight, const LotteryState& state)
{
    AssertLockHeld(cs_main);
    evoDb->Write(std::make_pair(DB_LOTTERY_STATE_SNAP, blockHash), state);

    // Height-keyed index entry: one small vector per height, dedup on append.
    // Bookkeeping is O(1) per block regardless of chain length — the legacy
    // ls_H vector was read and rewritten IN FULL every block.
    std::vector<uint256> at;
    evoDb->Read(std::make_pair(DB_LOTTERY_SNAP_INDEX, (int32_t)nHeight), at);
    if (std::find(at.begin(), at.end(), blockHash) == at.end()) {
        at.push_back(blockHash);
        evoDb->Write(std::make_pair(DB_LOTTERY_SNAP_INDEX, (int32_t)nHeight), at);
    }

    // Bounded trim: retire the snapshot(s) exactly PTX_SNAPSHOT_KEEP below.
    // Erasures commit only with the chainstate flush, so the crash-replay
    // base (the last flush tip) can never lose its snapshot; the disconnect
    // consumer needs only reorg depth, which KEEP exceeds by margin.
    const int hTrim = nHeight - PTX_SNAPSHOT_KEEP;
    if (hTrim > 0) {
        std::vector<uint256> old;
        if (evoDb->Read(std::make_pair(DB_LOTTERY_SNAP_INDEX, (int32_t)hTrim), old)) {
            for (const uint256& h : old) {
                evoDb->Erase(std::make_pair(DB_LOTTERY_STATE_SNAP, h));
            }
            evoDb->Erase(std::make_pair(DB_LOTTERY_SNAP_INDEX, (int32_t)hTrim));
        }
    }

    // One-time migration off the legacy whole-vector key.  The list is
    // append-ordered (oldest first), so everything before a generous recent
    // tail is erased here and the key dropped; tail entries that never gain
    // an index record stay as a bounded orphan set rather than paying a
    // full-list scan on every block.
    std::vector<uint256> legacy;
    if (evoDb->Read(DB_LOTTERY_SNAP_HASHES, legacy)) {
        const size_t keepTail = (size_t)2 * PTX_SNAPSHOT_KEEP;
        const size_t eraseN = legacy.size() > keepTail ? legacy.size() - keepTail : 0;
        for (size_t i = 0; i < eraseN; ++i) {
            evoDb->Erase(std::make_pair(DB_LOTTERY_STATE_SNAP, legacy[i]));
        }
        evoDb->Erase(DB_LOTTERY_SNAP_HASHES);
        LogPrintf("PTX LotteryState: legacy snapshot list migrated (%u entries, %u pre-tail snapshots erased)\n",
                  (unsigned)legacy.size(), (unsigned)eraseN);
    }
}

bool ReadLotteryStateSnapshotForBlock(const uint256& blockHash, LotteryState& stateOut)
{
    return evoDb->Read(std::make_pair(DB_LOTTERY_STATE_SNAP, blockHash), stateOut);
}

