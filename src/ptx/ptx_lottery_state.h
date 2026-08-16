// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef Hemis_PTX_LOTTERY_STATE_H
#define Hemis_PTX_LOTTERY_STATE_H

#include "amount.h"
#include "consensus/consensus.h"
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
 * Write a post-block snapshot of state to evodb under blockHash, maintain the
 * height-keyed snapshot index, and trim the snapshot exactly PTX_SNAPSHOT_KEEP
 * below.  Called inside ConnectBlock (within an open evoDb transaction) before
 * the transaction commits — so trims reach disk only with the chainstate
 * flush, which is what makes the crash-replay base structurally un-purgeable.
 * Idempotent per (blockHash, nHeight): a second write in the same block (the
 * coalesce arm and the payout arm both snapshot) overwrites the snapshot and
 * leaves the index unchanged — the old ls_H whole-vector bookkeeping was
 * rewritten IN FULL on every block (O(n^2) cumulative churn) and
 * double-appended per block, so any purge depth counted entries, not blocks.
 */
void WriteLotteryStateSnapshotForBlock(const uint256& blockHash, int nHeight, const LotteryState& state);

/**
 * Read the post-block snapshot for blockHash from evodb into stateOut.
 * Returns true if found. Called at DisconnectBlock to restore to pprev state.
 */
bool ReadLotteryStateSnapshotForBlock(const uint256& blockHash, LotteryState& stateOut);

// Snapshot retention for the lottery AND pose per-block snapshots (the two are
// a deliberate mirror; pose uses this constant too).  Serves the DISCONNECT
// consumer only — UndoSpecialTxsInBlock restores from the pprev snapshot of
// every disconnecting block and refuses on a miss — so it must exceed reorg
// depth, mirrored on KDD-070's DEFAULT_MAX_REORG_DEPTH + margin posture.  The
// BUG-037 startup restore and the ReplayBlocks base seed need NO depth at
// all: snapshot erasures commit only with the chainstate flush, so the newest
// committed snapshot is always the reload tip's, at any replay depth.
// (Known limit, same as KDD-070/BUG-028 RED-3: -maxreorg is a runtime arg;
// a node run with -maxreorg greater than this constant narrows its own
// disconnect coverage.)
static const int PTX_SNAPSHOT_KEEP = DEFAULT_MAX_REORG_DEPTH + 120;

#endif // Hemis_PTX_LOTTERY_STATE_H
