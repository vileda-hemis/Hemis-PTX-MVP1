// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <ptx/ptx_pose.h>
#include <evo/evodb.h>
#include <logging.h>
#include <ptx/ptx_lottery_state.h>   // PTX_SNAPSHOT_KEEP (shared retention)
#include <sync.h>
#include <util/system.h>
#include <univalue.h>
#include <validation.h>

#include <algorithm>
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

void PTXPoSeTracker::RestoreRecords(std::map<std::string, PTXNodeRecord> records)
{
    LOCK(cs_pose);
    records_ = std::move(records);
    // BUG-026 (B): the rollback MUST reach the file, not just memory.
    // RecordHonestParticipation() calls Save() on every credit, so a block that
    // is later rejected has ALREADY written its credits to ptx_pose.dat.
    // Restoring only the in-memory map would leave the flat file contaminated
    // and the leak would return at the next restart — which is exactly what the
    // h480 partition showed: gm01's restart reloaded a clean ACCUMULATOR (evoDb,
    // per-block snapshot) but pose came back from the file, so the two
    // populations stayed divergent across restarts.  Save() re-locks a
    // RecursiveMutex, which its own contract permits.
    //
    // ★ SCOPE, stated honestly: this makes the rollback DURABLE, not
    // TRANSACTIONAL.  A crash between the contaminating Save() and this one
    // still leaves the file dirty, and pose still has no per-block snapshot or
    // disconnect undo.  Full transactionality means moving pose to evoDb with
    // snapshot+undo exactly as LotteryState has (ODC-056 option (c)) — owed,
    // and NOT closed here.
    Save();
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

void PTXPoSeTracker::ResetForReindex()
{
    LOCK(cs_pose);

    const size_t discarded = records_.size();
    records_.clear();

    fs::path dat_path = GetDataDir() / "ptx_pose.dat";
    fs::path tmp_path = GetDataDir() / "ptx_pose.dat.tmp";
    // Mirrors Load()'s contract: warn, never throw — a failure to remove must
    // not abort init.  Note the in-memory clear above already guarantees the
    // replay starts fresh even if the unlink fails.
    try {
        if (fs::exists(dat_path)) fs::remove(dat_path);
        if (fs::exists(tmp_path)) fs::remove(tmp_path);
    } catch (const std::exception& e) {
        LogPrintf("PTX PoSe: WARNING: could not remove %s: %s\n", dat_path.string(), e.what());
    }

    LogPrintf("PTX PoSe: reindex in progress — discarded %u in-memory record(s) and removed %s "
              "(BUG-025: PoSe is derived state; a replay must not inherit records from heights "
              "it has not yet reached)\n",
              (unsigned)discarded, dat_path.string());
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

// ── BUG-027 / ODC-056(c): per-block pose snapshots in evoDb ────────────────
// Deliberate mirror of ptx_lottery_state.cpp. Key names follow the same
// convention ("ls_S"/"ls_H" -> "ps_S"/"ps_H") so the two are recognisably one
// pattern rather than two inventions.
static const std::string DB_POSE_STATE_SNAP  = "ps_S";
static const std::string DB_POSE_SNAP_HASHES = "ps_H";   // legacy whole-vector bookkeeping, migrated away
static const std::string DB_POSE_SNAP_INDEX  = "ps_I";   // ("ps_I", height) -> hashes snapshotted at that height

void WritePoseSnapshotForBlock(const uint256& blockHash, int nHeight,
                               const std::map<std::string, PTXNodeRecord>& records)
{
    AssertLockHeld(cs_main);
    evoDb->Write(std::make_pair(DB_POSE_STATE_SNAP, blockHash), records);

    // Height-keyed index + bounded trim — deliberate mirror of
    // WriteLotteryStateSnapshotForBlock; see the retention rationale at
    // PTX_SNAPSHOT_KEEP.  The legacy ps_H vector was read and rewritten IN
    // FULL every block (O(n^2) cumulative churn) and never purged — the
    // purge function existed but had no caller.
    std::vector<uint256> at;
    evoDb->Read(std::make_pair(DB_POSE_SNAP_INDEX, (int32_t)nHeight), at);
    if (std::find(at.begin(), at.end(), blockHash) == at.end()) {
        at.push_back(blockHash);
        evoDb->Write(std::make_pair(DB_POSE_SNAP_INDEX, (int32_t)nHeight), at);
    }

    const int hTrim = nHeight - PTX_SNAPSHOT_KEEP;
    if (hTrim > 0) {
        std::vector<uint256> old;
        if (evoDb->Read(std::make_pair(DB_POSE_SNAP_INDEX, (int32_t)hTrim), old)) {
            for (const uint256& h : old) {
                evoDb->Erase(std::make_pair(DB_POSE_STATE_SNAP, h));
            }
            evoDb->Erase(std::make_pair(DB_POSE_SNAP_INDEX, (int32_t)hTrim));
        }
    }

    std::vector<uint256> legacy;
    if (evoDb->Read(DB_POSE_SNAP_HASHES, legacy)) {
        const size_t keepTail = (size_t)2 * PTX_SNAPSHOT_KEEP;
        const size_t eraseN = legacy.size() > keepTail ? legacy.size() - keepTail : 0;
        for (size_t i = 0; i < eraseN; ++i) {
            evoDb->Erase(std::make_pair(DB_POSE_STATE_SNAP, legacy[i]));
        }
        evoDb->Erase(DB_POSE_SNAP_HASHES);
        LogPrintf("PTX PoSe: legacy snapshot list migrated (%u entries, %u pre-tail snapshots erased)\n",
                  (unsigned)legacy.size(), (unsigned)eraseN);
    }
}

bool ReadPoseSnapshotForBlock(const uint256& blockHash,
                              std::map<std::string, PTXNodeRecord>& recordsOut)
{
    return evoDb->Read(std::make_pair(DB_POSE_STATE_SNAP, blockHash), recordsOut);
}

bool LoadPoseFromDB(const uint256& tipHash)
{
    std::map<std::string, PTXNodeRecord> snap;
    if (tipHash.IsNull() || !ReadPoseSnapshotForBlock(tipHash, snap)) {
        return false;
    }

    // Log the file-vs-snapshot delta before overwriting: this line is the
    // observable proof that the restore DID something (or was a no-op) — the
    // anti-vacuity signal for the BUG-037 green run.
    const auto fileRecs = g_ptx_pose_tracker.GetAllRecords();
    int64_t fileTickets = 0, snapTickets = 0;
    for (const auto& kv : fileRecs) fileTickets += kv.second.lottery_tickets;
    for (const auto& kv : snap)     snapTickets += kv.second.lottery_tickets;
    LogPrintf("PTX PoSe: restored %u records (%lld tickets) from tip snapshot %s; "
              "flat file held %u records (%lld tickets)%s\n",
              (unsigned)snap.size(), (long long)snapTickets, tipHash.ToString(),
              (unsigned)fileRecs.size(), (long long)fileTickets,
              (fileRecs.size() != snap.size() || fileTickets != snapTickets)
                  ? " — file was NOT at-tip, corrected" : "");

    g_ptx_pose_tracker.RestoreRecords(std::move(snap));
    return true;
}

