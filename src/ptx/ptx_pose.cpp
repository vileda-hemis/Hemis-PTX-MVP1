// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ptx/ptx_pose.h>
#include <logging.h>

PTXPoSeTracker g_ptx_pose_tracker;

PTXNodeRecord& PTXPoSeTracker::GetOrCreate(const std::string& nid)
{
    auto it = records_.find(nid);
    if (it == records_.end()) {
        PTXNodeRecord r;
        r.node_id = nid;
        records_[nid] = r;
    }
    return records_[nid];
}

void PTXPoSeTracker::ApplyPenalty(const std::string& nid, int delta, const char* reason)
{
    auto& r = GetOrCreate(nid);
    r.pose_score += delta;
    r.window_zeroed = true;
    r.lottery_tickets = 0;
    if (r.pose_score >= POSE_THRESHOLD) r.quorum_eligible = false;
    LogPrintf("PTX PoSe: %s %s. score=%d eligible=%s\n",
              nid, reason, r.pose_score, r.quorum_eligible ? "true" : "false");
}

void PTXPoSeTracker::RecordWithhold(const std::string& node_id)
{
    LOCK(cs_pose);
    ApplyPenalty(node_id, 5, "withheld reveal");
}

void PTXPoSeTracker::RecordAbstain(const std::string& node_id)
{
    LOCK(cs_pose);
    ApplyPenalty(node_id, 2, "abstained");
}

void PTXPoSeTracker::RecordInvalidCommit(const std::string& node_id)
{
    LOCK(cs_pose);
    ApplyPenalty(node_id, 10, "invalid commit/reveal");
}

void PTXPoSeTracker::RecordHonestParticipation(const std::string& node_id)
{
    LOCK(cs_pose);
    auto& r = GetOrCreate(node_id);
    if (!r.window_zeroed) {
        r.lottery_tickets += 1;
    }
}

bool PTXPoSeTracker::IsEligible(const std::string& node_id) const
{
    LOCK(cs_pose);
    auto it = records_.find(node_id);
    if (it == records_.end()) return true;
    return it->second.quorum_eligible;
}

void PTXPoSeTracker::AdvanceLotteryWindow()
{
    LOCK(cs_pose);

    // Log the node with the most tickets this window.
    std::string top_node;
    int top_tickets = 0;
    for (const auto& kv : records_) {
        if (kv.second.lottery_tickets > top_tickets) {
            top_tickets = kv.second.lottery_tickets;
            top_node = kv.first;
        }
    }
    if (!top_node.empty()) {
        LogPrintf("PTX PoSe: lottery window closed. top_node=%s tickets=%d\n",
                  top_node, top_tickets);
    } else {
        LogPrintf("PTX PoSe: lottery window closed. no participants.\n");
    }

    // Reset per-window state; pose_score is intentionally preserved (KDD-023).
    for (auto& kv : records_) {
        kv.second.lottery_tickets = 0;
        kv.second.window_zeroed = false;
    }
}

PTXNodeRecord PTXPoSeTracker::GetRecord(const std::string& node_id) const
{
    LOCK(cs_pose);
    auto it = records_.find(node_id);
    if (it == records_.end()) {
        PTXNodeRecord r;
        r.node_id = node_id;
        return r;
    }
    return it->second;
}

std::map<std::string, PTXNodeRecord> PTXPoSeTracker::GetAllRecords() const
{
    LOCK(cs_pose);
    return records_;
}
