// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx_lottery_state.h"

#include "evo/evodb.h"
#include "sync.h"
#include "util/system.h"
#include "validation.h"

static const std::string DB_LOTTERY_STATE_SNAP = "ls_S";

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

void WriteLotteryStateSnapshotForBlock(const uint256& blockHash, const LotteryState& state)
{
    AssertLockHeld(cs_main);
    evoDb->Write(std::make_pair(DB_LOTTERY_STATE_SNAP, blockHash), state);
}

bool ReadLotteryStateSnapshotForBlock(const uint256& blockHash, LotteryState& stateOut)
{
    return evoDb->Read(std::make_pair(DB_LOTTERY_STATE_SNAP, blockHash), stateOut);
}
