// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_bls.h"
#include "ptx/ptx_commit_reveal.h"
#include "ptx/ptx_fanout.h"
#include "ptx/ptx_lottery.h"
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

// GM-side BLS key share (received via gm_bls_keyset, used in gm_bls_sign).
// Stored as 32-byte big-endian blst scalar.
static uint8_t g_ptx_my_bls_sk_bytes[32] = {};
static bool    g_ptx_my_bls_sk_set = false;
static RecursiveMutex cs_ptx_my_bls_sk;

// BLS threshold: t-of-n. KDD-TBD; using simple majority floor(n/2)+1.
static int PTX_BLS_Threshold(int n) { return n / 2 + 1; }

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

    // BLS threshold: select all eligible GMs; t = majority.
    int n_nodes = (int)g_ptx_nodes.size();
    if (n_nodes < 1)
        throw JSONRPCError(RPC_MISC_ERROR, "PTX: no registered nodes");
    int bls_threshold = PTX_BLS_Threshold(n_nodes);

    // Lazy-init BLS state: generate master polynomial and per-GM key shares once per session.
    {
        bool need_init = false;
        {
            LOCK(cs_ptx_bls);
            need_init = !g_ptx_bls_state.initialized || g_ptx_bls_state.t != bls_threshold;
        }
        if (need_init) {
            std::vector<std::string> all_ids;
            for (const auto& ni : g_ptx_nodes) all_ids.push_back(ni.node_id);
            if (!PTX_BLS_Init(all_ids, bls_threshold))
                throw JSONRPCError(RPC_MISC_ERROR, "PTX: BLS init failed");
        }
    }

    PTXQuorumAssignment quorum = PTX_AssignQuorum(round_id, round_seed, n_nodes, bls_threshold);
    if ((int)quorum.members.size() < bls_threshold)
        throw JSONRPCError(RPC_MISC_ERROR, "PTX: insufficient eligible nodes for BLS threshold");

    // Sort members deterministically.
    std::vector<std::string> member_ids = quorum.members;
    std::sort(member_ids.begin(), member_ids.end());

    // Initialise coordinator's round entry.
    {
        LOCK(cs_ptx_rounds);
        PTXCommitRevealRound round;
        round.round_id       = round_id;
        round.round_seed     = round_seed;
        round.threshold      = bls_threshold;
        round.quorum_members = member_ids;
        round.count          = (uint32_t)count;
        round.low            = low;
        round.high           = high;
        round.unique         = unique;
        round.exclude_integers = exc_ints;
        round.exclude_txids    = exc_txids;
        round.state          = PTXRoundState::COMMIT_PHASE;
        g_ptx_rounds[round_id] = round;
    }

    // Distribute BLS key shares to any GMs that haven't received one this session.
    PTX_FanOutKeySet(member_ids);

    // Collect partial BLS signatures from each quorum member.
    auto partial_sigs_raw = PTX_FanOutSign(round_id, round_seed, member_ids);

    // Collect blst partial signatures and 1-indexed polynomial positions.
    std::vector<std::vector<uint8_t>> bls_sigs;
    std::vector<int>                  bls_indices;
    std::vector<std::string>          signed_nodes;
    std::vector<std::string>          withheld;

    for (const auto& nid : member_ids) {
        auto it = partial_sigs_raw.find(nid);
        int idx = PTX_BLS_GetNodeIndex(nid);
        if (it != partial_sigs_raw.end() && idx > 0 &&
            (int)it->second.size() == PTX_SIG_BYTES) {
            bls_sigs.push_back(it->second);
            bls_indices.push_back(idx);
            signed_nodes.push_back(nid);
        } else {
            withheld.push_back(nid);
        }
    }

    if ((int)bls_sigs.size() < bls_threshold)
        throw JSONRPCError(RPC_MISC_ERROR,
            strprintf("PTX: BLS threshold not met: got %d/%d",
                      (int)bls_sigs.size(), bls_threshold));

    // Lagrange recovery from the first t partial sigs.
    std::vector<std::vector<uint8_t>> thresh_sigs(bls_sigs.begin(),
                                                   bls_sigs.begin() + bls_threshold);
    std::vector<int> thresh_indices(bls_indices.begin(),
                                    bls_indices.begin() + bls_threshold);

    uint8_t combined_sig[PTX_SIG_BYTES];
    if (!PTX_BLS_Recover(thresh_indices, thresh_sigs, combined_sig))
        throw JSONRPCError(RPC_MISC_ERROR, "PTX: BLS threshold signature recovery failed");

    if (!PTX_BLS_Verify(round_seed, combined_sig))
        throw JSONRPCError(RPC_MISC_ERROR, "PTX: BLS threshold signature verification failed");

    std::vector<uint8_t> threshold_sig_bytes(combined_sig, combined_sig + PTX_SIG_BYTES);
    uint256 beacon = PTX_BLS_SigToBeacon(combined_sig);

    // Update round state in coordinator's record.
    {
        LOCK(cs_ptx_rounds);
        auto& round = g_ptx_rounds[round_id];
        for (const auto& nid : signed_nodes)
            round.bls_partial_sigs[nid] = partial_sigs_raw[nid];
        round.threshold_sig = threshold_sig_bytes;
        round.beacon        = beacon;
        round.state         = PTXRoundState::RESOLVED;
    }

    // PoSe scoring.
    for (const auto& nid : withheld)     g_ptx_pose_tracker.RecordWithhold(nid);
    for (const auto& nid : signed_nodes) g_ptx_pose_tracker.RecordHonestParticipation(nid);

    std::set<int64_t> exclude_set = PTX_ResolveExclude(exc_arr);
    std::vector<int64_t> results  = PTX_MapBeacon(beacon, (uint32_t)count, low, high, unique, exclude_set);

    PTX_SetLastBeacon(beacon);

    // Assemble payload.
    CProbabilisticTxPayload payload;
    payload.game_id          = game_id;
    payload.nSeedHeight      = block_height;
    payload.nExpiryHeight    = block_height;
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
    payload.quorum_sig       = threshold_sig_bytes;
    // quorum_sig_hash = SHA256(threshold_sig); non-null satisfies existing validation.
    {
        CSHA256 qh;
        qh.Write(threshold_sig_bytes.data(), threshold_sig_bytes.size());
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
    ret.pushKV("quorum_sig",     HexStr(threshold_sig_bytes));
    ret.pushKV("quorum_sig_hash", payload.quorum_sig_hash.GetHex());
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
// RPC: gm_bls_keyset  (coordinator → GM)
// ---------------------------------------------------------------------------

UniValue gm_bls_keyset(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1) {
        throw std::runtime_error(
            "gm_bls_keyset sk_share_hex\n"
            "\nStore this node's BLS key share (32-byte hex). Called by the coordinator.\n"
            "\nArguments:\n"
            "1. sk_share_hex (str) BLS private key share as hex\n"
            + HelpExampleRpc("gm_bls_keyset", "\"aabb...\"")
        );
    }

    std::string sk_hex = request.params[0].get_str();
    if (!IsHex(sk_hex))
        throw JSONRPCError(RPC_INVALID_PARAMS, "sk_share_hex must be a hex string");

    std::vector<uint8_t> sk_bytes = ParseHex(sk_hex);
    if ((int)sk_bytes.size() != 32)
        throw JSONRPCError(RPC_INVALID_PARAMS, "sk_share_hex must be 32 bytes");

    {
        LOCK(cs_ptx_my_bls_sk);
        memcpy(g_ptx_my_bls_sk_bytes, sk_bytes.data(), 32);
        g_ptx_my_bls_sk_set = true;
    }
    LogPrintf("PTX: gm_bls_keyset: key share stored for node=%s\n", g_ptx_my_node_id);

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("accepted", true);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: gm_bls_sign  (coordinator → GM)
// ---------------------------------------------------------------------------

UniValue gm_bls_sign(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1) {
        throw std::runtime_error(
            "gm_bls_sign round_seed_hex\n"
            "\nSign round_seed with this node's BLS key share and return the partial signature.\n"
            "\nArguments:\n"
            "1. round_seed_hex (str) 64-char hex round seed\n"
            "\nResult:\n"
            "{\n"
            "  \"sig_hex\" : \"hex\"  96-byte BLS partial signature\n"
            "}\n"
            + HelpExampleRpc("gm_bls_sign", "\"aabb...\"")
        );
    }

    std::string seed_hex = request.params[0].get_str();
    if (!IsHex(seed_hex))
        throw JSONRPCError(RPC_INVALID_PARAMS, "round_seed_hex must be a hex string");

    uint256 round_seed = uint256S(seed_hex);

    uint8_t sk_bytes[32];
    bool    have_key = false;
    {
        LOCK(cs_ptx_my_bls_sk);
        have_key = g_ptx_my_bls_sk_set;
        if (have_key) memcpy(sk_bytes, g_ptx_my_bls_sk_bytes, 32);
    }
    if (!have_key)
        throw JSONRPCError(RPC_MISC_ERROR, "BLS key not set: coordinator must call gm_bls_keyset first");

    uint8_t sig_buf[PTX_SIG_BYTES];
    if (!PTX_BLS_PartialSign(sk_bytes, round_seed, sig_buf))
        throw JSONRPCError(RPC_MISC_ERROR, "BLS signing failed");

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("sig_hex", HexStr(Span<const uint8_t>(sig_buf, PTX_SIG_BYTES)));
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
// RPC: ptx_lottery_status
// ---------------------------------------------------------------------------

UniValue ptx_lottery_status(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_lottery_status\n"
            "\nReturn current PTX lottery state: pool balance, settlement window, eligible nodes.\n"
            "\nResult:\n"
            "{\n"
            "  \"pool_balance_sat\"    : n\n"
            "  \"settlement_window\"   : n\n"
            "  \"current_height\"      : n\n"
            "  \"next_settlement_at\"  : n\n"
            "  \"eligible_nodes\"      : [{\"node_id\", \"tickets\", \"pose_score\", \"eligible\"}, ...]\n"
            "}\n"
            + HelpExampleCli("ptx_lottery_status", "")
            + HelpExampleRpc("ptx_lottery_status", "")
        );
    }

    const int window  = Params().PTXSettlementWindow();
    const int height  = chainActive.Height();
    const int next_at = height + (window - (height % window));

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("pool_balance_sat",  PTX_GetPoolBalance());
    ret.pushKV("settlement_window", window);
    ret.pushKV("current_height",    (int64_t)height);
    ret.pushKV("next_settlement_at",(int64_t)next_at);

    UniValue nodes_arr(UniValue::VARR);
    for (const auto& kv : g_ptx_pose_tracker.GetAllRecords()) {
        const auto& rec = kv.second;
        UniValue no(UniValue::VOBJ);
        no.pushKV("node_id",    rec.node_id);
        no.pushKV("tickets",    rec.lottery_tickets);
        no.pushKV("pose_score", rec.pose_score);
        no.pushKV("eligible",   rec.quorum_eligible);
        nodes_arr.push_back(no);
    }
    ret.pushKV("eligible_nodes", nodes_arr);

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
    { "ptx",  "gm_bls_keyset",             &gm_bls_keyset,              true,   {"sk_share_hex"} },
    { "ptx",  "gm_bls_sign",               &gm_bls_sign,                true,   {"round_seed_hex"} },
    { "ptx",  "ptx_debug_setnodefailmode", &ptx_debug_setnodefailmode,  true,   {"target_node_id","mode"} },
    { "ptx",  "ptx_getroundstatus",        &ptx_getroundstatus,         true,   {"round_id"} },
    { "ptx",  "ptx_lottery_status",        &ptx_lottery_status,         true,   {} },
};
// clang-format on

void RegisterPTXRPCCommands(CRPCTable& tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
