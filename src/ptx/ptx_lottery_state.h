// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef Hemis_PTX_LOTTERY_STATE_H
#define Hemis_PTX_LOTTERY_STATE_H

#include "amount.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "serialize.h"
#include "uint256.h"

/**
 * LotteryState — consensus chain state for the ODC-022 lottery accumulator.
 *
 * Stored in evodb as a per-block post-block snapshot (keyed by block hash).
 * Singleton g_lotteryState is the in-memory live view. Requires cs_main.
 *
 * Reorg: at DisconnectBlock, restore from the snapshot written for pprev.
 * Startup: load from snapshot at chain tip hash.
 */
struct LotteryState {
    // Outpoint of the current accumulator UTXO. IsNull() if no accumulator exists yet.
    COutPoint accumulator_outpoint;

    // Cached value of the accumulator UTXO (fast-access mirror; authoritative source is UTXO set).
    CAmount accumulator_value{0};

    // Metadata from the most recent successful PTXPAYOUT, for RPC display.
    struct LastSettlement {
        int height{0};
        uint256 winner_protx;
        CScript winner_script;
        CAmount amount{0};
        uint256 selection_entropy;
        uint256 payout_txid;
    } last_settle;

    SERIALIZE_METHODS(LotteryState, obj) {
        READWRITE(obj.accumulator_outpoint);
        READWRITE(obj.accumulator_value);
        READWRITE(obj.last_settle.height);
        READWRITE(obj.last_settle.winner_protx);
        READWRITE(obj.last_settle.winner_script);
        READWRITE(obj.last_settle.amount);
        READWRITE(obj.last_settle.selection_entropy);
        READWRITE(obj.last_settle.payout_txid);
    }

    void Reset() { *this = LotteryState{}; }
};

/**
 * Returns the live singleton LotteryState. cs_main must be held.
 *
 * Do not store a reference across lock releases; always re-read under cs_main.
 */
LotteryState& GetLotteryState();

/**
 * Load g_lotteryState from the evodb post-block snapshot at tipHash.
 * Called once at startup after the chain index is loaded.
 * Falls back to default-initialized (empty) state if no snapshot exists.
 */
void LoadLotteryStateFromDB(const uint256& tipHash);

/**
 * Write a post-block snapshot of state to evodb under blockHash.
 * Called inside ConnectBlock (within an open evoDb transaction) before
 * the transaction commits.
 */
void WriteLotteryStateSnapshotForBlock(const uint256& blockHash, const LotteryState& state);

/**
 * Read the post-block snapshot for blockHash from evodb into stateOut.
 * Returns true if found. Called at DisconnectBlock to restore to pprev state.
 */
bool ReadLotteryStateSnapshotForBlock(const uint256& blockHash, LotteryState& stateOut);

#endif // Hemis_PTX_LOTTERY_STATE_H
