// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ptx/ptx_pose.h>
#include <logging.h>
#include <util/system.h>
#include <univalue.h>

#include <cstdio>
#include <string>

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
    Save();
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
    // Decay: -1 per honest round, floor 0. Restore eligibility if score drops below threshold.
    if (r.pose_score > 0) {
        r.pose_score -= 1;
        if (r.pose_score < POSE_THRESHOLD) r.quorum_eligible = true;
    }
    Save();
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
    Save();
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

void PTXPoSeTracker::Save() const
{
    // Caller must hold cs_pose; RecursiveMutex makes re-lock safe.
    LOCK(cs_pose);

    UniValue arr(UniValue::VARR);
    for (const auto& kv : records_) {
        const auto& r = kv.second;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("node_id",         r.node_id);
        obj.pushKV("pose_score",      r.pose_score);
        obj.pushKV("quorum_eligible", r.quorum_eligible);
        obj.pushKV("lottery_tickets", r.lottery_tickets);
        obj.pushKV("window_zeroed",   r.window_zeroed);
        arr.push_back(obj);
    }
    UniValue root(UniValue::VOBJ);
    root.pushKV("records", arr);
    std::string json = root.write(2);

    fs::path tmp_path = GetDataDir() / "ptx_pose.dat.tmp";
    fs::path dat_path = GetDataDir() / "ptx_pose.dat";

    FILE* f = fopen(tmp_path.string().c_str(), "w");
    if (!f) {
        LogPrintf("PTX PoSe: WARNING: cannot write %s\n", tmp_path.string());
        return;
    }
    if (fwrite(json.data(), 1, json.size(), f) != json.size()) {
        fclose(f);
        LogPrintf("PTX PoSe: WARNING: short write to %s\n", tmp_path.string());
        return;
    }
    fclose(f);
    RenameOver(tmp_path, dat_path);
}

void PTXPoSeTracker::Load()
{
    LOCK(cs_pose);

    fs::path dat_path = GetDataDir() / "ptx_pose.dat";
    FILE* f = fopen(dat_path.string().c_str(), "r");
    if (!f) {
        LogPrintf("PTX PoSe: no state file found, starting fresh\n");
        return;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string json(sz > 0 ? (size_t)sz : 0, '\0');
    if (sz > 0) {
        if ((long)fread(&json[0], 1, sz, f) != sz) {
            fclose(f);
            LogPrintf("PTX PoSe: WARNING: read error on %s — starting fresh\n", dat_path.string());
            return;
        }
    }
    fclose(f);

    UniValue root;
    if (!root.read(json)) {
        LogPrintf("PTX PoSe: WARNING: failed to parse %s — starting fresh\n", dat_path.string());
        return;
    }
    if (!root.isObject() || !root["records"].isArray()) {
        LogPrintf("PTX PoSe: WARNING: malformed %s — starting fresh\n", dat_path.string());
        return;
    }

    const UniValue& arr = root["records"];
    for (size_t i = 0; i < arr.size(); i++) {
        const UniValue& obj = arr[i];
        if (!obj.isObject() || !obj["node_id"].isStr()) continue;
        PTXNodeRecord r;
        r.node_id         = obj["node_id"].get_str();
        r.pose_score      = obj["pose_score"].isNum()  ? obj["pose_score"].get_int()  : 0;
        r.quorum_eligible = obj["quorum_eligible"].isBool() ? obj["quorum_eligible"].getBool() : true;
        r.lottery_tickets = obj["lottery_tickets"].isNum() ? obj["lottery_tickets"].get_int() : 0;
        r.window_zeroed   = obj["window_zeroed"].isBool()  ? obj["window_zeroed"].getBool()   : false;
        records_[r.node_id] = r;
    }
    LogPrintf("PTX PoSe: loaded %d records from %s\n", (int)records_.size(), dat_path.string());
}
