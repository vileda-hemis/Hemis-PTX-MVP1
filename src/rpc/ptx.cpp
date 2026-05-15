// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_commit_reveal.h"
#include "ptx/ptx_fanout.h"
#include "ptx/ptx_mempool.h"
#include "ptx/ptx_output_mapping.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_quorum.h"
#include "ptx/ptx_seed.h"
#include "crypto/sha256.h"
#include "logging.h"
#include "primitives/transaction.h"
#include "random.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "sync.h"
#include "uint256.h"
#include "util/system.h"
#include "utilstrencodings.h"
#include "validation.h"

#include <univalue.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

RecursiveMutex cs_ptx_secrets;
std::map<std::string, std::map<std::string, uint256>> g_ptx_local_secrets;

// ---------------------------------------------------------------------------
// Exclude-list helpers
// ---------------------------------------------------------------------------

static void PTX_BuildExcludeLists(const UniValue& arr,
                                   std::vector<int64_t>& exc_ints,
                                   std::vector<std::string>& exc_txids)
{
    if (arr.isNull() || !arr.isArray()) return;
    for (size_t i = 0; i < arr.size(); i++) {
        const UniValue& v = arr[i];
        if (v.isNum()) {
            exc_ints.push_back(v.get_int64());
        } else if (v.isStr()) {
            const std::string& s = v.get_str();
            // 64-char hex string is a tx_id (256-bit hash)
            if (s.size() == 64) {
                exc_txids.push_back(s);
            } else {
                exc_ints.push_back((int64_t)atoll(s.c_str()));
            }
        }
    }
}

static std::set<int64_t> PTX_ResolveExclude(const UniValue& arr)
{
    std::set<int64_t> result;
    std::vector<int64_t> exc_ints;
    std::vector<std::string> exc_txids;
    PTX_BuildExcludeLists(arr, exc_ints, exc_txids);
    for (int64_t v : exc_ints) result.insert(v);
    if (!exc_txids.empty())
        LogPrintf("PTX: tx_id exclude resolution deferred to Phase 2\n");
    return result;
}

// ---------------------------------------------------------------------------
// RPC: ptx_roll
// ---------------------------------------------------------------------------

UniValue ptx_roll(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 7 || request.params.size() > 7) {
        throw std::runtime_error(
            "ptx_roll count low high unique exclude game_id caller_salt\n"
            "\nRun a PTX commit-reveal round and return verifiable random results.\n"
            "\nArguments:\n"
            "1. count       (int)  Number of values to draw (>= 1)\n"
            "2. low         (int)  Minimum value, inclusive\n"
            "3. high        (int)  Maximum value, inclusive\n"
            "4. unique      (bool) Whether draws must be distinct\n"
            "5. exclude     (arr)  Integers or 64-char tx_id strings to skip\n"
            "6. game_id     (str)  Caller-defined game identifier\n"
            "7. caller_salt (str)  Caller entropy as hex\n"
            "\nResult:\n"
            "{\n"
            "  \"results\"        : [n, ...]\n"
            "  \"round_seed\"     : \"hex\"\n"
            "  \"quorum_sig\"     : \"hex\"\n"
            "  \"quorum_members\" : [\"id\", ...]\n"
            "  \"block_height\"   : n\n"
            "  \"tx_id\"          : \"hex\"\n"
            "}\n"
            + HelpExampleCli("ptx_roll", "1 1 100 false '[]' mygame 00aabbcc")
            + HelpExampleRpc("ptx_roll", "1, 1, 100, false, [], \"mygame\", \"00aabbcc\"")
        );
    }

    if (g_ptx_my_node_id.empty())
        throw JSONRPCError(RPC_MISC_ERROR, "PTX not enabled: set ptxnodeid= in config");

    int       count            = request.params[0].get_int();
    int64_t   low              = request.params[1].get_int64();
    int64_t   high             = request.params[2].get_int64();
    bool      unique           = request.params[3].get_bool();
    const UniValue& exc_arr    = request.params[4];
    std::string game_id        = request.params[5].get_str();
    std::string caller_salt_hex = request.params[6].get_str();

    if (count < 1)
        throw JSONRPCError(RPC_INVALID_PARAMS, "count must be >= 1");
    if (low > high)
        throw JSONRPCError(RPC_INVALID_PARAMS, "low must be <= high");
    if (!exc_arr.isArray())
        throw JSONRPCError(RPC_INVALID_PARAMS, "exclude must be a JSON array");
    for (size_t i = 0; i < exc_arr.size(); i++) {
        const UniValue& v = exc_arr[i];
        if (!v.isNum() && !v.isStr())
            throw JSONRPCError(RPC_INVALID_PARAMS, "exclude elements must be integers or 64-char hex tx_id strings");
        if (v.isStr() && v.get_str().size() != 64)
            throw JSONRPCError(RPC_INVALID_PARAMS, "exclude string elements must be 64-char hex tx_id");
    }
    if (!caller_salt_hex.empty() && !IsHex(caller_salt_hex))
        throw JSONRPCError(RPC_INVALID_PARAMS, "caller_salt must be a hex string");

    uint32_t block_height = (uint32_t)chainActive.Height();
    uint256  prev_beacon  = PTX_GetLastBeacon();

    std::vector<unsigned char> caller_salt_bytes = ParseHex(caller_salt_hex);
    uint256 nonce = PTX_BuildNonce(prev_beacon, caller_salt_bytes);

    std::vector<int64_t>    exc_ints;
    std::vector<std::string> exc_txids;
    PTX_BuildExcludeLists(exc_arr, exc_ints, exc_txids);

    uint256     params_hash = PTX_HashParams((uint32_t)count, low, high, unique, exc_ints, exc_txids);
    uint256     round_seed  = PTX_BuildRoundSeed(game_id, block_height, {}, nonce, params_hash);
    std::string round_id    = PTX_MakeRoundId(game_id, block_height, params_hash);

    PTXQuorumAssignment quorum = PTX_AssignQuorum(round_id, round_seed, 5, 3);
    if ((int)quorum.members.size() < quorum.threshold)
        throw JSONRPCError(RPC_MISC_ERROR, "PTX: insufficient eligible nodes");

    // Generate one secret per quorum member. Coordinator owns all secrets.
    std::map<std::string, uint256> secrets;
    for (const auto& nid : quorum.members) {
        CSHA256 h;
        h.Write(round_seed.begin(), 32);
        h.Write((const unsigned char*)nid.data(), nid.size());
        unsigned char rb[8];
        GetRandBytes(rb, 8);
        h.Write(rb, 8);
        uint256 s;
        h.Finalize(s.begin());
        secrets[nid] = s;
    }
    { LOCK(cs_ptx_secrets); g_ptx_local_secrets[round_id] = secrets; }

    // Sort members deterministically.
    std::vector<std::string> member_ids;
    for (const auto& nid : quorum.members) member_ids.push_back(nid);
    std::sort(member_ids.begin(), member_ids.end());

    // Initialise coordinator's round entry.
    {
        LOCK(cs_ptx_rounds);
        PTXCommitRevealRound round;
        round.round_id          = round_id;
        round.round_seed        = round_seed;
        round.threshold         = 3;
        round.quorum_members    = member_ids;
        round.count             = (uint32_t)count;
        round.low               = low;
        round.high              = high;
        round.unique            = unique;
        round.exclude_integers  = exc_ints;
        round.exclude_txids     = exc_txids;
        g_ptx_rounds[round_id] = round;
    }

    PTX_FanOutCommit(round_id, secrets, round_seed, member_ids);

    {
        LOCK(cs_ptx_rounds);
        if (!PTX_ForceRevealPhase(g_ptx_rounds[round_id]))
            throw JSONRPCError(RPC_MISC_ERROR, "PTX: threshold commits not reached");
    }

    PTX_FanOutReveal(round_id, secrets);

    uint256 beacon;
    std::vector<std::string> withheld;
    std::vector<std::string> abstained;
    {
        LOCK(cs_ptx_rounds);
        if (!PTX_TryResolve(g_ptx_rounds[round_id]))
            throw JSONRPCError(RPC_MISC_ERROR, "PTX: threshold reveals not reached");
        beacon   = g_ptx_rounds[round_id].beacon;
        withheld = g_ptx_rounds[round_id].withheld_nodes;
        abstained = g_ptx_rounds[round_id].abstained_nodes;
    }

    // PoSe scoring.
    for (const auto& nid : withheld)  g_ptx_pose_tracker.RecordWithhold(nid);
    for (const auto& nid : abstained) g_ptx_pose_tracker.RecordAbstain(nid);
    for (const auto& nid : member_ids) {
        if (std::find(withheld.begin(),  withheld.end(),  nid) == withheld.end() &&
            std::find(abstained.begin(), abstained.end(), nid) == abstained.end()) {
            g_ptx_pose_tracker.RecordHonestParticipation(nid);
        }
    }

    std::set<int64_t> exclude_set = PTX_ResolveExclude(exc_arr);
    std::vector<int64_t> results  = PTX_MapBeacon(beacon, (uint32_t)count, low, high, unique, exclude_set);

    PTX_SetLastBeacon(beacon);

    // Assemble payload.
    CProbabilisticTxPayload payload;
    payload.game_id          = game_id;
    payload.nSeedHeight      = block_height;
    payload.nExpiryHeight    = block_height; // KDD-027: no accept_window, no expires_at
    payload.nonce            = nonce;
    payload.ptx_params_hash  = params_hash;
    payload.count            = (uint32_t)count;
    payload.low              = low;
    payload.high             = high;
    payload.unique           = unique;
    payload.exclude_integers = exc_ints;
    payload.exclude_txids    = exc_txids;
    payload.round_seed       = round_seed;
    payload.beacon           = beacon;
    payload.results          = results;
    payload.quorum_members   = member_ids;
    {
        // Phase 1 quorum_sig_hash: deterministic hash of the round_id.
        CSHA256 qh;
        qh.Write((const unsigned char*)round_id.data(), round_id.size());
        qh.Finalize(payload.quorum_sig_hash.begin());
    }

    PTXCommitRevealRound round_copy;
    { LOCK(cs_ptx_rounds); round_copy = g_ptx_rounds[round_id]; }
    std::string txid = PTX_AutoCommit(round_copy, payload);

    UniValue ret(UniValue::VOBJ);
    UniValue res_arr(UniValue::VARR);
    for (int64_t v : results) res_arr.push_back(v);
    ret.pushKV("results",        res_arr);
    ret.pushKV("round_seed",     round_seed.GetHex());
    ret.pushKV("quorum_sig",     payload.quorum_sig_hash.GetHex());
    UniValue qm_arr(UniValue::VARR);
    for (const auto& nid : member_ids) qm_arr.push_back(nid);
    ret.pushKV("quorum_members", qm_arr);
    ret.pushKV("block_height",   (int64_t)block_height);
    ret.pushKV("tx_id",          txid);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: gm_commit  (coordinator → node)
// ---------------------------------------------------------------------------

UniValue gm_commit(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 4) {
        throw std::runtime_error(
            "gm_commit round_id round_seed_hex members_json commitment_hex\n"
            "\nRecord a quorum commitment for this node (called by the coordinator).\n"
            "\nArguments:\n"
            "1. round_id       (str)   Round identifier\n"
            "2. round_seed_hex (str)   Hex round seed\n"
            "3. members_json   (array) Sorted quorum member node_id list\n"
            "4. commitment_hex (str)   Hex commitment for this node\n"
            + HelpExampleRpc("gm_commit", "\"rid\", \"aabb...\", [\"n1\"], \"ccdd...\"")
        );
    }

    std::string        round_id    = request.params[0].get_str();
    uint256            round_seed  = uint256S(request.params[1].get_str());
    const UniValue&    marr        = request.params[2].get_array();
    uint256            commitment  = uint256S(request.params[3].get_str());
    const std::string& committer   = g_ptx_my_node_id;

    bool ok = false;
    {
        LOCK(cs_ptx_rounds);
        if (g_ptx_rounds.count(round_id) == 0) {
            PTXCommitRevealRound r;
            r.round_id   = round_id;
            r.round_seed = round_seed;
            r.threshold  = 3;
            for (size_t i = 0; i < marr.size(); i++)
                r.quorum_members.push_back(marr[i].get_str());
            std::sort(r.quorum_members.begin(), r.quorum_members.end());
            g_ptx_rounds[round_id] = r;
        }
        ok = PTX_SubmitCommit(g_ptx_rounds[round_id], committer, commitment);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("accepted", ok);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: gm_reveal  (coordinator → node)
// ---------------------------------------------------------------------------

UniValue gm_reveal(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2) {
        throw std::runtime_error(
            "gm_reveal round_id secret_hex\n"
            "\nRecord a quorum reveal for this node (called by the coordinator).\n"
            "\nArguments:\n"
            "1. round_id   (str) Round identifier\n"
            "2. secret_hex (str) Hex secret\n"
            + HelpExampleRpc("gm_reveal", "\"rid\", \"aabb...\"")
        );
    }

    std::string round_id = request.params[0].get_str();
    uint256     secret   = uint256S(request.params[1].get_str());

    bool ok = false;
    {
        LOCK(cs_ptx_rounds);
        if (g_ptx_rounds.count(round_id) == 0) {
            UniValue ret(UniValue::VOBJ);
            ret.pushKV("accepted", false);
            ret.pushKV("resolved", false);
            return ret;
        }
        ok = PTX_SubmitReveal(g_ptx_rounds[round_id], g_ptx_my_node_id, secret);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("accepted", ok);
    ret.pushKV("resolved", false);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: ptx_debug_setnodefailmode
// ---------------------------------------------------------------------------

UniValue ptx_debug_setnodefailmode(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2) {
        throw std::runtime_error(
            "ptx_debug_setnodefailmode target_node_id mode\n"
            "\nSimulate a node fail mode for testing (coordinator side only).\n"
            "\nArguments:\n"
            "1. target_node_id (str) Node to target\n"
            "2. mode           (str) abstain | withhold | invalid_commit | clear\n"
            + HelpExampleRpc("ptx_debug_setnodefailmode", "\"node1\", \"withhold\"")
        );
    }

    std::string target = request.params[0].get_str();
    std::string mode   = request.params[1].get_str();

    {
        LOCK(cs_ptx_failmodes);
        if (mode == "clear")
            g_ptx_node_failmodes.erase(target);
        else
            g_ptx_node_failmodes[target] = mode;
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("target", target);
    ret.pushKV("mode",   mode);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: ptx_getroundstatus
// ---------------------------------------------------------------------------

UniValue ptx_getroundstatus(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_getroundstatus ( round_id )\n"
            "\nReturn PTX round status and PoSe records.\n"
            "\nArguments:\n"
            "1. round_id (str, optional) Return status for a specific round only\n"
            + HelpExampleRpc("ptx_getroundstatus", "")
        );
    }

    UniValue ret(UniValue::VOBJ);

    // Helper lambda to serialise one round.
    auto round_to_uv = [](const PTXCommitRevealRound& r) {
        UniValue ro(UniValue::VOBJ);
        ro.pushKV("round_id",   r.round_id);
        ro.pushKV("round_seed", r.round_seed.GetHex());
        ro.pushKV("beacon",     r.beacon.GetHex());
        ro.pushKV("threshold",  r.threshold);
        ro.pushKV("state",      (int)r.state);
        UniValue cm(UniValue::VARR);
        for (const auto& n : r.committed_nodes) cm.push_back(n);
        ro.pushKV("committed",  cm);
        UniValue wh(UniValue::VARR);
        for (const auto& n : r.withheld_nodes) wh.push_back(n);
        ro.pushKV("withheld",   wh);
        UniValue ab(UniValue::VARR);
        for (const auto& n : r.abstained_nodes) ab.push_back(n);
        ro.pushKV("abstained",  ab);
        ro.pushKV("count",      (int64_t)r.count);
        ro.pushKV("low",        r.low);
        ro.pushKV("high",       r.high);
        ro.pushKV("unique",     r.unique);
        UniValue exc(UniValue::VARR);
        for (int64_t v : r.exclude_integers) exc.push_back(v);
        for (const auto& s : r.exclude_txids) exc.push_back(s);
        ro.pushKV("exclude",    exc);
        if (r.state == PTXRoundState::RESOLVED) {
            std::set<int64_t> exc_set(r.exclude_integers.begin(), r.exclude_integers.end());
            std::vector<int64_t> derived = PTX_MapBeacon(r.beacon, r.count, r.low, r.high, r.unique, exc_set);
            UniValue res(UniValue::VARR);
            for (int64_t v : derived) res.push_back(v);
            ro.pushKV("results", res);
        }
        return ro;
    };

    UniValue rounds_arr(UniValue::VARR);
    {
        LOCK(cs_ptx_rounds);
        if (!request.params.empty() && !request.params[0].isNull()) {
            std::string rid = request.params[0].get_str();
            auto it = g_ptx_rounds.find(rid);
            if (it != g_ptx_rounds.end())
                rounds_arr.push_back(round_to_uv(it->second));
        } else {
            // Return up to the last 10 rounds.
            std::vector<const PTXCommitRevealRound*> all;
            for (const auto& kv : g_ptx_rounds) all.push_back(&kv.second);
            size_t start = all.size() > 10 ? all.size() - 10 : 0;
            for (size_t i = start; i < all.size(); i++)
                rounds_arr.push_back(round_to_uv(*all[i]));
        }
    }
    ret.pushKV("rounds", rounds_arr);

    // PoSe records.
    UniValue pose_arr(UniValue::VARR);
    for (const auto& kv : g_ptx_pose_tracker.GetAllRecords()) {
        const auto& rec = kv.second;
        UniValue po(UniValue::VOBJ);
        po.pushKV("node_id",    rec.node_id);
        po.pushKV("pose_score", rec.pose_score);
        po.pushKV("eligible",   rec.quorum_eligible);
        po.pushKV("tickets",    rec.lottery_tickets);
        pose_arr.push_back(po);
    }
    ret.pushKV("pose_records", pose_arr);

    return ret;
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// clang-format off
static const CRPCCommand commands[] = {
//  category  name                         handler                       okSafe  argNames
    { "ptx",  "ptx_roll",                  &ptx_roll,                   true,   {"count","low","high","unique","exclude","game_id","caller_salt"} },
    { "ptx",  "gm_commit",                 &gm_commit,                  true,   {"round_id","round_seed_hex","members_json","commitment_hex"} },
    { "ptx",  "gm_reveal",                 &gm_reveal,                  true,   {"round_id","secret_hex"} },
    { "ptx",  "ptx_debug_setnodefailmode", &ptx_debug_setnodefailmode,  true,   {"target_node_id","mode"} },
    { "ptx",  "ptx_getroundstatus",        &ptx_getroundstatus,         true,   {"round_id"} },
};
// clang-format on

void RegisterPTXRPCCommands(CRPCTable& tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
