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
 * Metadata from a single PTXPAYOUT settlement event.
 * Defined as a standalone serializable struct so it can be stored in the
 * settlement_history ring buffer inside LotteryState.
 */
struct LastSettlement {
    int height{0};
    uint256 winner_protx;
    CScript winner_script;
    CAmount amount{0};
    uint256 selection_entropy;
    uint256 payout_txid;

    SERIALIZE_METHODS(LastSettlement, obj) {
        READWRITE(obj.height);
        READWRITE(obj.winner_protx);
        READWRITE(obj.winner_script);
        READWRITE(obj.amount);
        READWRITE(obj.selection_entropy);
        READWRITE(obj.payout_txid);
    }
};

// Number of recent settlements retained in LotteryState::settlement_history.
static constexpr size_t kSettlementHistoryDepth = 20;

/**
 * LotteryState — consensus chain state for the ODC-022 lottery accumulator.
 *
 * Stored in evodb as a per-block post-block snapshot (keyed by block hash).
 * Singleton g_lotteryState is the in-memory live view. Requires cs_main.
 *
 * Reorg: at DisconnectBlock, restore from the snapshot written for pprev.
 * Startup: load from snapshot at chain tip hash.
 *
 * Serialization versions:
 *   v1 — accumulator_outpoint, accumulator_value, last_settle fields.
 *   v2 — adds total_rolls and settlement_history.  v1 snapshots decode cleanly
 *        via try/catch; new fields keep their in-struct defaults (0 / empty).
 */
struct LotteryState {
    // Outpoint of the current accumulator UTXO. IsNull() if no accumulator exists yet.
    COutPoint accumulator_outpoint;

    // Cached value of the accumulator UTXO (fast-access mirror; authoritative source is UTXO set).
    CAmount accumulator_value{0};

    // Metadata from the most recent successful PTXPAYOUT, for RPC display.
    LastSettlement last_settle;

    // Cumulative count of PTX rolls (PTXSESS transactions) since chain genesis.
    uint64_t total_rolls{0};

    // Ring buffer of recent settlements, newest at back, capped at kSettlementHistoryDepth.
    std::vector<LastSettlement> settlement_history;

    SERIALIZE_METHODS(LotteryState, obj) {
        // v1 fields — written individually to preserve the v1 wire format for
        // backward-compat with existing evodb snapshots.
        READWRITE(obj.accumulator_outpoint);
        READWRITE(obj.accumulator_value);
        READWRITE(obj.last_settle.height);
        READWRITE(obj.last_settle.winner_protx);
        READWRITE(obj.last_settle.winner_script);
        READWRITE(obj.last_settle.amount);
        READWRITE(obj.last_settle.selection_entropy);
        READWRITE(obj.last_settle.payout_txid);
        // v2 fields — try/catch: if stream ends here (v1 snapshot), the catch is empty.
        // Both fields have in-struct defaults (0 / empty vector) set before deserialization
        // runs, so no explicit assignment is needed on failure — same pattern as
        // CDeterministicGMState's scriptPTXPayment / node_id catch blocks.
        try {
            READWRITE(obj.total_rolls);
            READWRITE(obj.settlement_history);
        } catch (const std::ios_base::failure&) {
            // v1 snapshot: total_rolls stays 0, settlement_history stays empty.
        }
    }

    void Reset() { *this = LotteryState{}; }

    bool HasAccumulator() const { return !accumulator_outpoint.IsNull(); }
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
 * Also appends blockHash to the persistent snapshot hash list used by PurgeStaleSnapshots.
 * Called inside ConnectBlock (within an open evoDb transaction) before the transaction commits.
 */
void WriteLotteryStateSnapshotForBlock(const uint256& blockHash, const LotteryState& state);

/**
 * Read the post-block snapshot for blockHash from evodb into stateOut.
 * Returns true if found. Called at DisconnectBlock to restore to pprev state.
 */
bool ReadLotteryStateSnapshotForBlock(const uint256& blockHash, LotteryState& stateOut);

// Snapshots to retain for reorg rollback. Covers the 100-block coinbase-maturity
// finality horizon used by Bitcoin-lineage chains; any reorg deeper than this
// would already violate other consensus rules before reaching LotteryState.
static const int PTX_LOTTERY_SNAPSHOT_DEPTH = 100;

/**
 * Erase evodb snapshots older than keepCount blocks.
 * Reads the snapshot hash list, erases the oldest (size - keepCount) entries
 * from evodb, then rewrites the trimmed list.
 * TODO: wire at Step 7 — call from ConnectBlock every PTX_LOTTERY_SNAPSHOT_DEPTH blocks.
 */
void PurgeStaleSnapshots(int keepCount = PTX_LOTTERY_SNAPSHOT_DEPTH);

#endif // Hemis_PTX_LOTTERY_STATE_H
