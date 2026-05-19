// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_POSE_H
#define HEMIS_PTX_POSE_H

#include <map>
#include <string>
#include <sync.h>

struct PTXNodeRecord {
    std::string node_id;
    int  pose_score{0};
    bool quorum_eligible{true};
    int  lottery_tickets{0};
    bool window_zeroed{false};
};

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
