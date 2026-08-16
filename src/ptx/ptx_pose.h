// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_POSE_H
#define HEMIS_PTX_POSE_H

#include <map>
#include <string>
#include <sync.h>

#include "serialize.h"
#include "uint256.h"

struct PTXNodeRecord {
    std::string node_id;
    int  pose_score{0};
    bool quorum_eligible{true};
    int  lottery_tickets{0};
    bool window_zeroed{false};

    // BUG-027 / ODC-056(c): binary serialization for the evoDb per-block
    // snapshot.  The flat file uses UniValue JSON and stays that way — this is
    // the evoDb representation, mirroring LastSettlement/LotteryState.
    SERIALIZE_METHODS(PTXNodeRecord, obj) {
        READWRITE(obj.node_id);
        READWRITE(obj.pose_score);
        READWRITE(obj.quorum_eligible);
        READWRITE(obj.lottery_tickets);
        READWRITE(obj.window_zeroed);
    }
};

// ── BUG-027 / ODC-056(c): per-block pose snapshots in evoDb ────────────────
//
// WHY THIS EXISTS.  UndoSpecialTxsInBlock restored LotteryState from its pprev
// snapshot but touched pose nowhere, so a disconnected block's
// RecordHonestParticipation credits were never removed and pose grew monotonic
// across reorgs.  Measured on the Phase-2 rebuild: with total_rolls=2 (expected
// 11*2 = 22 tickets) nodes read 22 / 33 / 44 / 88 in proportion to how many
// chain switches they had seen (gm01: 48 tip regressions -> 88 tickets; gm03:
// 14 -> 22).  Divergent pose selects a different settlement winner, which
// rejected the h300 boundary and — because wallet-less GMs cannot stake, so the
// callers ARE the producer set — stranded the producers on 7 tips until the
// majority chain held 0.4% of stake and HALTED.
//
// The accumulator never diverged, because it already had exactly this
// mechanism.  These functions are a deliberate mirror of
// ptx_lottery_state.{h,cpp}: same key shape, same height-keyed index and
// PTX_SNAPSHOT_KEEP trim, same refuse-on-missing posture at disconnect.  Kept
// as free functions (not members) for the same reason LotteryState does: the
// evoDb dependency stays out of the tracker class.
void WritePoseSnapshotForBlock(const uint256& blockHash, int nHeight,
                               const std::map<std::string, PTXNodeRecord>& records);

// false when absent — the caller MUST treat that as an integrity failure and
// refuse the disconnect rather than restoring a default-constructed map, which
// would silently zero every node's pose.
bool ReadPoseSnapshotForBlock(const uint256& blockHash,
                              std::map<std::string, PTXNodeRecord>& recordsOut);

// BUG-037: restore the tracker from the ps_S snapshot at the given block hash,
// replacing whatever the flat file loaded.  The file is written on every credit
// and therefore holds CRASH-TIME state, not at-tip state; after a hard-reset
// rollback the chainstate reloads at an earlier flush while the file stays at
// the future, and every replayed settlement boundary is judged against records
// it should not have yet — h360 of the 2026-08-16 incident, where all 161
// nodes rejected their own valid history with ptxpayout-wrong-recipient and
// re-staked a fork.  The snapshot at the LOADED TIP is the only pose source
// keyed by block hash (the standing rule for consensus inputs), and evoDb
// commits atomically with the chainstate flush, so that snapshot is present by
// construction after any crash, at any replay depth.
// Returns false when no snapshot exists at tipHash (pre-snapshot datadir or
// genesis-only chain) — the caller owns that policy; the tracker is untouched.
bool LoadPoseFromDB(const uint256& tipHash);

class PTXPoSeTracker {
public:
    // Committed but no valid reveal. pose_score += 5.
    void RecordWithhold(const std::string& node_id);

    // Never committed. pose_score += 2.
    void RecordAbstain(const std::string& node_id);

    // Reveal failed verification. pose_score += 10.
    void RecordInvalidCommit(const std::string& node_id);

    // Valid partial sig: lottery_tickets += 1, pose_score -= 1 (decay, floor 0).
    void RecordHonestParticipation(const std::string& node_id);

    // Unknown nodes return true. Known nodes return quorum_eligible.
    bool IsEligible(const std::string& node_id) const;

    // KDD-017: every 1440 blocks. Log top node. Reset tickets + window_zeroed.
    // Does NOT reset pose_score. No reward distribution in Phase 1.
    void AdvanceLotteryWindow();

    PTXNodeRecord GetRecord(const std::string& node_id) const;
    std::map<std::string, PTXNodeRecord> GetAllRecords() const;

    // Load state from <datadir>/ptx_pose.dat. Call once at startup.
    // Missing or corrupt file: log WARNING, start fresh (never crashes).
    void Load();

    // BUG-025: discard all in-memory records AND remove the on-disk file.
    // Called at startup when a reindex is under way.  PoSe feeds PTXPAYOUT
    // winner selection (P10/P11), so it is consensus-affecting DERIVED state:
    // a replay that inherits records accumulated at heights it has not reached
    // yet judges committed blocks against the future and forks.  -reindex wipes
    // evoDb but never touched this file, which is what forked a node at h480.
    // Clears memory too — the load at startup happens before the reindex flags
    // are known, so the file alone is not enough.  Never throws.
    void ResetForReindex();

    // BUG-026 (B): wholesale restore of the in-memory record set.  A block that
    // FAILS to connect must leave in-memory PTX state exactly as it found it —
    // the same invariant BUG-023 established for CVerifyDB, on the other
    // untransacted path.  Pose is mutated part-way through
    // ProcessSpecialTxsInBlock; when a later check rejects the block, those
    // credits survived and permanently skewed the live tracker (observed on the
    // h420 wedge alongside the accumulator leak).  Used only by the failure
    // sentry there; not a general-purpose setter.
    void RestoreRecords(std::map<std::string, PTXNodeRecord> records);

private:
    mutable RecursiveMutex cs_pose;
    std::map<std::string, PTXNodeRecord> records_;
    static constexpr int POSE_THRESHOLD = 100;

    // Applies a penalty: increments score, zeros lottery, logs, updates eligibility.
    void ApplyPenalty(const std::string& nid, int delta, const char* reason);

    PTXNodeRecord& GetOrCreate(const std::string& nid);

    // Atomically persist records_ to <datadir>/ptx_pose.dat.
    // Must be called with cs_pose already held.
    void Save() const;
};

extern PTXPoSeTracker g_ptx_pose_tracker;

#endif // HEMIS_PTX_POSE_H
