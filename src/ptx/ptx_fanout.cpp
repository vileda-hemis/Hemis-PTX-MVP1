// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_fanout.h"

#include "ptx/ptx_bls.h"
#include "ptx/ptx_commit_reveal.h"
#include "crypto/sha256.h"
#include "logging.h"
#include "rpc/server.h"
#include "support/events.h"
#include "sync.h"
#include "utilstrencodings.h"
#include "util/system.h"

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>

#include <univalue.h>

#include <map>
#include <set>
#include <string>
#include <vector>

RecursiveMutex cs_ptx_failmodes;
std::map<std::string, std::string> g_ptx_node_failmodes;

// ---------------------------------------------------------------------------
// Internal HTTP plumbing
// ---------------------------------------------------------------------------

namespace {

struct PTXHTTPReply {
    int status{0};
    int error{-1};
    std::string body;
};

void ptx_http_done(struct evhttp_request* req, void* ctx)
{
    PTXHTTPReply* reply = static_cast<PTXHTTPReply*>(ctx);
    if (!req) { reply->status = 0; return; }
    reply->status = evhttp_request_get_response_code(req);
    struct evbuffer* buf = evhttp_request_get_input_buffer(req);
    if (buf) {
        size_t sz = evbuffer_get_length(buf);
        const char* data = (const char*)evbuffer_pullup(buf, sz);
        if (data) reply->body = std::string(data, sz);
        evbuffer_drain(buf, sz);
    }
}

#if LIBEVENT_VERSION_NUMBER >= 0x02010300
void ptx_http_error_cb(enum evhttp_request_error err, void* ctx)
{
    PTXHTTPReply* reply = static_cast<PTXHTTPReply*>(ctx);
    reply->error = (int)err;
}
#endif

const PTXNodeInfo* PTX_FindNode(const std::string& node_id)
{
    for (const auto& n : g_ptx_nodes)
        if (n.node_id == node_id) return &n;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// PTX_CallNodeRpc
// ---------------------------------------------------------------------------

PTXRpcResponse PTX_CallNodeRpc(const PTXNodeInfo& node,
                               const std::string& method,
                               const UniValue& params)
{
    PTXRpcResponse result;

    raii_event_base base = obtain_event_base();
    raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), node.host, node.port);
    evhttp_connection_set_timeout(evcon.get(), 3);

    PTXHTTPReply response;
    raii_evhttp_request req = obtain_evhttp_request(ptx_http_done, &response);
    if (!req) return result;
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    evhttp_request_set_error_cb(req.get(), ptx_http_error_cb);
#endif

    struct evkeyvalq* hdrs = evhttp_request_get_output_headers(req.get());
    evhttp_add_header(hdrs, "Host", node.host.c_str());
    evhttp_add_header(hdrs, "Connection", "close");
    evhttp_add_header(hdrs, "Content-Type", "application/json");

    std::string rpcpass = gArgs.GetArg("-rpcpassword", "");
    if (!rpcpass.empty()) {
        std::string creds = gArgs.GetArg("-rpcuser", "") + ":" + rpcpass;
        evhttp_add_header(hdrs, "Authorization",
                          (std::string("Basic ") + EncodeBase64(creds)).c_str());
    }

    std::string body = JSONRPCRequestObj(method, params, 1).write() + "\n";
    struct evbuffer* out = evhttp_request_get_output_buffer(req.get());
    if (!out) return result;
    evbuffer_add(out, body.data(), body.size());

    int r = evhttp_make_request(evcon.get(), req.get(), EVHTTP_REQ_POST, "/");
    req.release(); // ownership transferred to evcon
    if (r != 0) return result;
    event_base_dispatch(base.get());

    result.body = response.body;
    if (response.status != 200) return result;

    try {
        UniValue res;
        if (!res.read(response.body)) return result;
        UniValue rval = find_value(res, "result");
        if (!rval.isObject()) return result;
        UniValue accepted = find_value(rval, "accepted");
        result.success = accepted.isBool() && accepted.get_bool();
    } catch (...) {}

    return result;
}

// ---------------------------------------------------------------------------
// PTX_FanOutCommit
// ---------------------------------------------------------------------------

void PTX_FanOutCommit(const std::string& round_id,
                      const std::map<std::string, uint256>& secrets,
                      const uint256& round_seed,
                      const std::vector<std::string>& member_ids)
{
    for (const auto& node_id : member_ids) {
        std::string fmode;
        {
            LOCK(cs_ptx_failmodes);
            auto fit = g_ptx_node_failmodes.find(node_id);
            if (fit != g_ptx_node_failmodes.end()) fmode = fit->second;
        }

        if (fmode == "abstain") {
            LogPrintf("PTX: FanOutCommit: %s abstain (failmode)\n", node_id);
            continue;
        }

        auto sit = secrets.find(node_id);
        if (sit == secrets.end()) continue;
        const uint256& secret = sit->second;

        // commitment = SHA256(secret || round_seed)
        CSHA256 h;
        h.Write(secret.begin(), 32);
        h.Write(round_seed.begin(), 32);
        uint256 commitment;
        h.Finalize(commitment.begin());

        if (fmode == "invalid_commit") {
            commitment.SetNull();
        }

        // Record in coordinator's own round before HTTP (brief lock).
        {
            LOCK(cs_ptx_rounds);
            auto rit = g_ptx_rounds.find(round_id);
            if (rit != g_ptx_rounds.end())
                PTX_SubmitCommit(rit->second, node_id, commitment);
        }

        // Fan-out via HTTP — cs_ptx_rounds must NOT be held here.
        const PTXNodeInfo* ni = PTX_FindNode(node_id);
        if (!ni) {
            LogPrintf("PTX: FanOutCommit: no node info for %s\n", node_id);
            continue;
        }

        UniValue params(UniValue::VARR);
        params.push_back(round_id);
        params.push_back(round_seed.GetHex());
        UniValue marr(UniValue::VARR);
        for (const auto& m : member_ids) marr.push_back(m);
        params.push_back(marr);
        params.push_back(commitment.GetHex());

        auto resp = PTX_CallNodeRpc(*ni, "gm_commit", params);
        LogPrintf("PTX: FanOutCommit: %s %s\n", node_id,
                  resp.success ? "accepted" : "rejected/unreachable");
    }
}

// ---------------------------------------------------------------------------
// PTX_FanOutReveal
// ---------------------------------------------------------------------------

void PTX_FanOutReveal(const std::string& round_id,
                      const std::map<std::string, uint256>& secrets)
{
    // Snapshot committed nodes before any HTTP (do not hold lock during calls).
    std::vector<std::string> committed;
    {
        LOCK(cs_ptx_rounds);
        auto rit = g_ptx_rounds.find(round_id);
        if (rit == g_ptx_rounds.end()) return;
        for (const auto& n : rit->second.committed_nodes)
            committed.push_back(n);
    }

    for (const auto& node_id : committed) {
        std::string fmode;
        {
            LOCK(cs_ptx_failmodes);
            auto fit = g_ptx_node_failmodes.find(node_id);
            if (fit != g_ptx_node_failmodes.end()) fmode = fit->second;
        }

        if (fmode == "withhold") {
            LogPrintf("PTX: FanOutReveal: %s withhold (failmode)\n", node_id);
            continue;
        }

        auto sit = secrets.find(node_id);
        if (sit == secrets.end()) continue;
        const uint256& secret = sit->second;

        // Record reveal in coordinator's round before HTTP (brief lock).
        {
            LOCK(cs_ptx_rounds);
            auto rit = g_ptx_rounds.find(round_id);
            if (rit != g_ptx_rounds.end())
                PTX_SubmitReveal(rit->second, node_id, secret);
        }

        // Fan-out via HTTP — cs_ptx_rounds must NOT be held here.
        const PTXNodeInfo* ni = PTX_FindNode(node_id);
        if (!ni) {
            LogPrintf("PTX: FanOutReveal: no node info for %s\n", node_id);
            continue;
        }

        UniValue params(UniValue::VARR);
        params.push_back(round_id);
        params.push_back(secret.GetHex());

        auto resp = PTX_CallNodeRpc(*ni, "gm_reveal", params);
        LogPrintf("PTX: FanOutReveal: %s %s\n", node_id,
                  resp.success ? "accepted" : "rejected/unreachable");
    }
}

// ---------------------------------------------------------------------------
// PTX_FanOutKeySet  (Phase 2 BLS)
// ---------------------------------------------------------------------------

void PTX_FanOutKeySet(const std::vector<std::string>& member_ids)
{
    {
        LOCK(cs_ptx_bls);
        uint8_t gpk_check[48];
        blst_p1_affine_compress(gpk_check, &g_ptx_bls_state.group_pk);
        LogPrintf("PTX DKG: group_pk[0..7]=%02x%02x%02x%02x%02x%02x%02x%02x n=%d t=%d\n",
                  gpk_check[0],gpk_check[1],gpk_check[2],gpk_check[3],
                  gpk_check[4],gpk_check[5],gpk_check[6],gpk_check[7],
                  g_ptx_bls_state.n, g_ptx_bls_state.t);
    }

    for (const auto& node_id : member_ids) {
        // Track keyset_sent in g_ptx_bls_state node_index presence
        // (re-send if not yet acknowledged this session).
        {
            LOCK(cs_ptx_bls);
            if (g_ptx_bls_state.node_index.count(node_id) == 0) {
                LogPrintf("PTX: FanOutKeySet: no BLS state for %s\n", node_id);
                continue;
            }
        }

        uint8_t sk_bytes[32];
        if (!PTX_BLS_GetShareBytes(node_id, sk_bytes)) {
            LogPrintf("PTX: FanOutKeySet: no share for %s\n", node_id);
            continue;
        }
        std::string sk_hex = HexStr(Span<const uint8_t>(sk_bytes, 32));

        const PTXNodeInfo* ni = PTX_FindNode(node_id);
        if (!ni) {
            LogPrintf("PTX: FanOutKeySet: no node info for %s\n", node_id);
            continue;
        }

        UniValue params(UniValue::VARR);
        params.push_back(sk_hex);

        auto resp = PTX_CallNodeRpc(*ni, "gm_bls_keyset", params);
        LogPrintf("PTX: FanOutKeySet: %s %s\n", node_id,
                  resp.success ? "accepted" : "rejected/unreachable");
    }
}

// ---------------------------------------------------------------------------
// PTX_FanOutSign  (Phase 2 BLS)
// ---------------------------------------------------------------------------

std::map<std::string, std::vector<uint8_t>> PTX_FanOutSign(
    const std::string& round_id,
    const uint256& round_seed,
    const std::vector<std::string>& member_ids)
{
    std::map<std::string, std::vector<uint8_t>> collected;

    for (const auto& node_id : member_ids) {
        const PTXNodeInfo* ni = PTX_FindNode(node_id);
        if (!ni) {
            LogPrintf("PTX: FanOutSign: no node info for %s\n", node_id);
            continue;
        }

        UniValue params(UniValue::VARR);
        params.push_back(round_seed.GetHex());

        // Use raw HTTP call; parse sig_hex from body directly (not "accepted" pattern).
        raii_event_base base = obtain_event_base();
        raii_evhttp_connection evcon = obtain_evhttp_connection_base(base.get(), ni->host, ni->port);
        evhttp_connection_set_timeout(evcon.get(), 5);

        PTXHTTPReply response;
        raii_evhttp_request req = obtain_evhttp_request(ptx_http_done, &response);
        if (!req) continue;
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
        evhttp_request_set_error_cb(req.get(), ptx_http_error_cb);
#endif

        struct evkeyvalq* hdrs = evhttp_request_get_output_headers(req.get());
        evhttp_add_header(hdrs, "Host", ni->host.c_str());
        evhttp_add_header(hdrs, "Connection", "close");
        evhttp_add_header(hdrs, "Content-Type", "application/json");

        std::string rpcpass = gArgs.GetArg("-rpcpassword", "");
        if (!rpcpass.empty()) {
            std::string creds = gArgs.GetArg("-rpcuser", "") + ":" + rpcpass;
            evhttp_add_header(hdrs, "Authorization",
                              (std::string("Basic ") + EncodeBase64(creds)).c_str());
        }

        std::string body = JSONRPCRequestObj("gm_bls_sign", params, 1).write() + "\n";
        struct evbuffer* out = evhttp_request_get_output_buffer(req.get());
        if (!out) continue;
        evbuffer_add(out, body.data(), body.size());

        int r = evhttp_make_request(evcon.get(), req.get(), EVHTTP_REQ_POST, "/");
        req.release();
        if (r != 0) continue;
        event_base_dispatch(base.get());

        if (response.status != 200) {
            LogPrintf("PTX: FanOutSign: %s HTTP %d\n", node_id, response.status);
            continue;
        }

        try {
            UniValue res;
            if (!res.read(response.body)) continue;
            UniValue rval = find_value(res, "result");
            if (!rval.isObject()) continue;
            UniValue sig_val = find_value(rval, "sig_hex");
            if (!sig_val.isStr()) continue;
            std::string sig_hex = sig_val.get_str();
            if (!IsHex(sig_hex)) continue;
            std::vector<uint8_t> sig_bytes = ParseHex(sig_hex);
            if ((int)sig_bytes.size() != PTX_SIG_BYTES) {
                LogPrintf("PTX: FanOutSign: %s bad sig size %d\n", node_id, (int)sig_bytes.size());
                continue;
            }
            collected[node_id] = std::move(sig_bytes);
            LogPrintf("PTX: FanOutSign: %s got partial sig\n", node_id);
        } catch (...) {
            LogPrintf("PTX: FanOutSign: %s parse error\n", node_id);
        }
    }

    return collected;
}
