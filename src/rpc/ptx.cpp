// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_bls.h"
#include "ptx/ptx_commit_reveal.h"
#include "ptx/ptx_fanout.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_mempool.h"
#include "ptx/ptx_output_mapping.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_quorum.h"
#include "ptx/ptx_seed.h"
#include "crypto/sha256.h"
#include "key_io.h"
#include "logging.h"
#include "primitives/transaction.h"
#include "random.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "sync.h"
#include "uint256.h"
#include "util/system.h"
#include "utilstrencodings.h"
#include "validation.h"

#include "ptx/ptx_wallet.h"

#ifdef ENABLE_WALLET
#include "wallet/rpcwallet.h"
#include "wallet/wallet.h"
#endif // ENABLE_WALLET

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

    LogPrintf("PTX gm_bls_sign: node=%s sig[0..3]=%02x%02x%02x%02x\n",
              g_ptx_my_node_id, sig_buf[0],sig_buf[1],sig_buf[2],sig_buf[3]);

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

// Forward declaration: defined after ptx_pose_status (Step 13).
static UniValue PTX_BuildPoseJson(const PTXNodeRecord& rec);

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
        pose_arr.push_back(PTX_BuildPoseJson(kv.second));
    }
    ret.pushKV("pose_records", pose_arr);

    return ret;
}

// ---------------------------------------------------------------------------
// Shared pose-record JSON builder (Step 13)
// Used by ptx_pose_status, ptx_lottery_status.eligible_nodes,
// ptx_gm_pose, and ptx_wallet_operated_gms.
// Field order: node_id → pose_score → eligible → tickets → penalized_this_window
// (penalized_this_window appended last to preserve stable ordering for existing consumers).
// ---------------------------------------------------------------------------

static UniValue PTX_BuildPoseJson(const PTXNodeRecord& rec)
{
    UniValue po(UniValue::VOBJ);
    po.pushKV("node_id",               rec.node_id);
    po.pushKV("pose_score",            rec.pose_score);
    po.pushKV("eligible",              rec.quorum_eligible);
    po.pushKV("tickets",               rec.lottery_tickets);
    po.pushKV("penalized_this_window", rec.window_zeroed);
    return po;
}

// ---------------------------------------------------------------------------
// RPC: ptx_pose_status
// ---------------------------------------------------------------------------

UniValue ptx_pose_status(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_pose_status\n"
            "\nReturn PoSe scores, lottery tickets, and eligibility for all known GMs.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"node_id\"               : \"str\",  compound label:suffix from ProRegPL v3\n"
            "    \"pose_score\"            : n,        cumulative penalty score (0 = healthy)\n"
            "    \"eligible\"              : bool,     false when pose_score >= 100\n"
            "    \"tickets\"               : n,        honest-participation count this window\n"
            "    \"penalized_this_window\" : bool      true if GM was penalized this window (tickets were reset)\n"
            "  }, ...\n"
            "]\n"
            + HelpExampleCli("ptx_pose_status", "")
            + HelpExampleRpc("ptx_pose_status", "")
        );
    }

    UniValue arr(UniValue::VARR);
    for (const auto& kv : g_ptx_pose_tracker.GetAllRecords()) {
        arr.push_back(PTX_BuildPoseJson(kv.second));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// RPC: ptx_gm_pose
// ---------------------------------------------------------------------------

UniValue ptx_gm_pose(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "ptx_gm_pose \"node_id\"\n"
            "\nReturn pose-tracker detail for a single registered GM.\n"
            "Errors with RPC_INVALID_PARAMETER if node_id is not found in the DGM list.\n"
            "For GMs participating in rolls but not yet registered via protx_register*,\n"
            "use ptx_pose_status instead.\n"
            "\nArguments:\n"
            "1. \"node_id\"  (string, required) compound label:suffix from protx_register* response\n"
            "\nResult:\n"
            "{\n"
            "  \"node_id\"               : \"str\",\n"
            "  \"pose_score\"            : n,        cumulative penalty score (0 = healthy)\n"
            "  \"eligible\"              : bool,     false when pose_score >= 100\n"
            "  \"tickets\"               : n,        honest-participation count this window\n"
            "  \"penalized_this_window\" : bool,     true if GM was penalized this window\n"
            "  \"payment_configured\"    : bool      true if scriptPTXPayment is set (GM can win payouts)\n"
            "}\n"
            + HelpExampleCli("ptx_gm_pose", "\"gm01:aabbccdd\"")
            + HelpExampleRpc("ptx_gm_pose", "\"gm01:aabbccdd\"")
        );
    }

    const std::string node_id = request.params[0].get_str();
    CDeterministicGMList gmList = deterministicGMManager->GetListAtChainTip();

    GMPoseDetail detail;
    if (!PTX_GetGMPoseDetail(node_id, gmList, g_ptx_pose_tracker, detail)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "GM not found: " + node_id);
    }

    UniValue result = PTX_BuildPoseJson(detail.pose);
    result.pushKV("payment_configured", detail.payment_configured);
    return result;
}

// ---------------------------------------------------------------------------
// RPC: ptx_lottery_status
// ---------------------------------------------------------------------------

// Build one settlement JSON entry for last_settle or settlement_history.
// "gm" is omitted when winner_script is empty or yields no standard destination —
// this cannot happen in normal operation (PTX_SelectWinner skips empty scripts) but
// the omit-not-empty-string contract makes any anomaly clearly visible to consumers.
// include_amount_sat: true for last_settle (flat monitoring field), false for history entries.
static UniValue PTX_MakeSettlementJson(const LastSettlement& s, bool include_amount_sat)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("height",       (int64_t)s.height);
    obj.pushKV("winner_protx", s.winner_protx.GetHex());
    if (include_amount_sat) obj.pushKV("amount_sat", s.amount);
    obj.pushKV("amount",       strprintf("%.8f", (double)s.amount / COIN));
    obj.pushKV("txid",         s.payout_txid.GetHex());
    if (!s.winner_script.empty()) {
        CTxDestination dest;
        if (ExtractDestination(s.winner_script, dest)) {
            obj.pushKV("gm", EncodeDestination(dest));
        }
    }
    return obj;
}

UniValue ptx_lottery_status(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_lottery_status\n"
            "\nReturn current PTX lottery state for explorer and monitoring consumption.\n"
            "\nResult:\n"
            "{\n"
            "  \"pool_balance_sat\"   : n,        (numeric) accumulator UTXO value in satoshis\n"
            "  \"settlement_window\"  : n,        (numeric) blocks per settlement window\n"
            "  \"current_height\"     : n,        (numeric) current chain tip height\n"
            "  \"next_settlement_at\" : n,        (numeric) height of next settlement boundary\n"
            "  \"total_rolls\"        : n,        (numeric) cumulative PTX sessions since genesis\n"
            "  \"eligible_nodes\"     : [         (array) all pose-tracker nodes\n"
            "    {\n"
            "      \"node_id\"               : \"str\",\n"
            "      \"pose_score\"            : n,\n"
            "      \"eligible\"              : bool,\n"
            "      \"tickets\"               : n,\n"
            "      \"penalized_this_window\" : bool\n"
            "    }, ...\n"
            "  ],\n"
            "  \"last_settle\"        : {         (object) most recent settlement;\n"
            "                                     height=0 and winner_protx all-zero indicate no settlement yet\n"
            "    \"height\"       : n,\n"
            "    \"gm\"           : \"str\",   Base58Check payment address (field absent if script empty)\n"
            "    \"winner_protx\" : \"hex\",\n"
            "    \"amount_sat\"   : n,        (extra field on last_settle only)\n"
            "    \"amount\"       : \"str\",   HMS, 8 decimal places\n"
            "    \"txid\"         : \"hex\"\n"
            "  },\n"
            "  \"settlement_history\" : [         (array) recent settlements, newest first, cap=20\n"
            "    {\n"
            "      \"height\"       : n,\n"
            "      \"gm\"           : \"str\",   Base58Check payment address (field absent if script empty)\n"
            "      \"winner_protx\" : \"hex\",\n"
            "      \"amount\"       : \"str\",   HMS, 8 decimal places\n"
            "      \"txid\"         : \"hex\"\n"
            "    }, ...\n"
            "  ]\n"
            "}\n"
            + HelpExampleCli("ptx_lottery_status", "")
            + HelpExampleRpc("ptx_lottery_status", "")
        );
    }

    const int window  = Params().PTXSettlementWindow();
    const int height  = chainActive.Height();
    const int next_at = height + (window - (height % window));

    // Snapshot the full LotteryState under a single cs_main acquisition so all
    // fields are consistent with each other.
    LotteryState snapshot;
    {
        LOCK(cs_main);
        snapshot = GetLotteryState();
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("pool_balance_sat",   snapshot.accumulator_value);
    ret.pushKV("settlement_window",  window);
    ret.pushKV("current_height",     (int64_t)height);
    ret.pushKV("next_settlement_at", (int64_t)next_at);
    ret.pushKV("total_rolls",        (int64_t)snapshot.total_rolls);

    UniValue nodes_arr(UniValue::VARR);
    for (const auto& kv : g_ptx_pose_tracker.GetAllRecords()) {
        nodes_arr.push_back(PTX_BuildPoseJson(kv.second));
    }
    ret.pushKV("eligible_nodes", nodes_arr);

    // last_settle: sentinel when height==0 and winner_protx all-zero (no settlement yet).
    ret.pushKV("last_settle", PTX_MakeSettlementJson(snapshot.last_settle, /*include_amount_sat=*/true));

    // settlement_history: newest first (reverse-iterate the ring buffer).
    UniValue hist_arr(UniValue::VARR);
    const std::vector<LastSettlement>& hist = snapshot.settlement_history;
    for (int i = (int)hist.size() - 1; i >= 0; --i) {
        hist_arr.push_back(PTX_MakeSettlementJson(hist[i], /*include_amount_sat=*/false));
    }
    ret.pushKV("settlement_history", hist_arr);

    return ret;
}

// ---------------------------------------------------------------------------
// Wallet-scoped RPCs (Step 12)
// ---------------------------------------------------------------------------

#ifdef ENABLE_WALLET

UniValue ptx_wallet_lottery_status(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_wallet_lottery_status\n"
            "\nReturn wallet-scoped PTX lottery state: GMs whose scriptPTXPayment is spendable\n"
            "by keys in this wallet, and their current lottery participation status.\n"
            "\nNote: my_gms lists GMs where this wallet controls the payout address. It does NOT\n"
            "necessarily represent GMs operated by this wallet — in the cold/hot operator pattern\n"
            "the payout key can be on a different wallet from the operator/collateral keys.\n"
            "For operational ownership, see protx_list wallet_only=true.\n"
            "(KDD-035)\n"
            "\nResult:\n"
            "{\n"
            "  \"my_gms\": [               (array) GMs whose scriptPTXPayment this wallet controls\n"
            "    {\n"
            "      \"node_id\"    : \"str\",  compound label:suffix from ProRegPL v3\n"
            "      \"address\"    : \"str\",  Base58Check of scriptPTXPayment\n"
            "      \"tickets\"    : n,       current lottery_tickets in pose tracker\n"
            "      \"eligible\"   : bool,    quorum_eligible from pose tracker\n"
            "      \"pose_score\" : n        pose_score from pose tracker\n"
            "    }, ...\n"
            "  ],\n"
            "  \"my_eligible_count\" : n    count of my_gms where eligible==true and tickets>0\n"
            "}\n"
            + HelpExampleCli("ptx_wallet_lottery_status", "")
            + HelpExampleRpc("ptx_wallet_lottery_status", "")
        );
    }

    CDeterministicGMList gmList = deterministicGMManager->GetListAtChainTip();

    std::vector<WalletGMInfo> myGMs;
    {
        LOCK(pwallet->cs_wallet);
        myGMs = PTX_FilterWalletGMs(*pwallet, gmList, g_ptx_pose_tracker);
    }

    UniValue gmsArr(UniValue::VARR);
    int eligibleCount = 0;
    for (const auto& info : myGMs) {
        UniValue gobj(UniValue::VOBJ);
        gobj.pushKV("node_id",    info.node_id);
        CTxDestination dest;
        if (ExtractDestination(info.payment_script, dest)) {
            gobj.pushKV("address", EncodeDestination(dest));
        }
        gobj.pushKV("tickets",    info.tickets);
        gobj.pushKV("eligible",   info.eligible);
        gobj.pushKV("pose_score", info.pose_score);
        if (info.eligible && info.tickets > 0) ++eligibleCount;
        gmsArr.push_back(gobj);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("my_gms",           gmsArr);
    ret.pushKV("my_eligible_count", eligibleCount);
    return ret;
}

UniValue ptx_lottery_history(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_lottery_history\n"
            "\nReturn this wallet's recent PTX lottery winnings — the subset of the chain-side\n"
            "settlement_history where the payout went to a key controlled by this wallet.\n"
            "Results are newest first. Bounded by the chain-side history cap (max 20 entries).\n"
            "For an audit-grade all-time history, scan wallet transaction history for PTXPAYOUT\n"
            "receipts.\n"
            "\nResult:\n"
            "[                            (array) recent settlements won by this wallet, newest first\n"
            "  {\n"
            "    \"height\"       : n,\n"
            "    \"gm\"           : \"str\",  Base58Check payment address (field absent if non-standard)\n"
            "    \"winner_protx\" : \"hex\",\n"
            "    \"amount\"       : \"str\",  HMS, 8 decimal places\n"
            "    \"amount_sat\"   : n,\n"
            "    \"txid\"         : \"hex\"\n"
            "  }, ...\n"
            "]\n"
            + HelpExampleCli("ptx_lottery_history", "")
            + HelpExampleRpc("ptx_lottery_history", "")
        );
    }

    std::vector<LastSettlement> history;
    {
        LOCK(cs_main);
        history = GetLotteryState().settlement_history;
    }

    std::vector<LastSettlement> mine;
    {
        LOCK(pwallet->cs_wallet);
        mine = PTX_FilterWalletSettlements(*pwallet, history);
    }

    // mine is in ring-buffer order (oldest first); reverse to newest-first for output.
    UniValue ret(UniValue::VARR);
    for (int i = (int)mine.size() - 1; i >= 0; --i) {
        ret.push_back(PTX_MakeSettlementJson(mine[i], /*include_amount_sat=*/true));
    }
    return ret;
}

UniValue ptx_wallet_operated_gms(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_wallet_operated_gms\n"
            "\nReturn GMs where this wallet holds the owner or voting key, annotated with\n"
            "current pose-tracker state.\n"
            "\nNote: predicate is ks.HaveKey(keyIDOwner) || ks.HaveKey(keyIDVoting). This covers\n"
            "EC keys only. The BLS operator key (used for quorum signing on hot nodes) is NOT\n"
            "checked — a hot node holding only the BLS operator key will see empty results.\n"
            "For full GM association including collateral and BLS keys, use protx_list\n"
            "wallet_only=true. For GMs paying to keys in this wallet, see\n"
            "ptx_wallet_lottery_status. (KDD-036)\n"
            "\nResult:\n"
            "[                              (array) GMs where this wallet holds owner or voting key\n"
            "  {\n"
            "    \"node_id\"               : \"str\",  compound label:suffix from ProRegPL v3\n"
            "    \"proTxHash\"             : \"hex\",\n"
            "    \"payment_address\"       : \"str\",  Base58Check of scriptPTXPayment (field absent if not set)\n"
            "    \"has_payment_address\"   : bool,    true if scriptPTXPayment is configured\n"
            "    \"pose_score\"            : n,\n"
            "    \"eligible\"              : bool,\n"
            "    \"tickets\"               : n,\n"
            "    \"penalized_this_window\" : bool\n"
            "  }, ...\n"
            "]\n"
            + HelpExampleCli("ptx_wallet_operated_gms", "")
            + HelpExampleRpc("ptx_wallet_operated_gms", "")
        );
    }

    CDeterministicGMList gmList = deterministicGMManager->GetListAtChainTip();

    std::vector<OperatedGMInfo> myGMs;
    {
        LOCK(pwallet->cs_wallet);
        myGMs = PTX_FilterOperatedGMs(*pwallet, gmList, g_ptx_pose_tracker);
    }

    UniValue ret(UniValue::VARR);
    for (const auto& info : myGMs) {
        UniValue gobj(UniValue::VOBJ);
        gobj.pushKV("node_id",    info.node_id);
        gobj.pushKV("proTxHash",  info.proTxHash.GetHex());
        if (!info.payment_script.empty()) {
            CTxDestination dest;
            if (ExtractDestination(info.payment_script, dest)) {
                gobj.pushKV("payment_address", EncodeDestination(dest));
            }
        }
        gobj.pushKV("has_payment_address",   info.has_payment_address);
        gobj.pushKV("pose_score",            info.pose_score);
        gobj.pushKV("eligible",              info.eligible);
        gobj.pushKV("tickets",               info.tickets);
        gobj.pushKV("penalized_this_window", info.penalized_this_window);
        ret.push_back(gobj);
    }
    return ret;
}
#endif // ENABLE_WALLET

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
    { "ptx",  "ptx_pose_status",           &ptx_pose_status,            true,   {} },
    { "ptx",  "ptx_gm_pose",               &ptx_gm_pose,                true,   {"node_id"} },
    { "ptx",  "ptx_lottery_status",        &ptx_lottery_status,         true,   {} },
#ifdef ENABLE_WALLET
    { "ptx",  "ptx_wallet_lottery_status", &ptx_wallet_lottery_status,  true,   {} },
    { "ptx",  "ptx_lottery_history",       &ptx_lottery_history,        true,   {} },
    { "ptx",  "ptx_wallet_operated_gms",   &ptx_wallet_operated_gms,    true,   {} },
#endif // ENABLE_WALLET
};
// clang-format on

void RegisterPTXRPCCommands(CRPCTable& tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
