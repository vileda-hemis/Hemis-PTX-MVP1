// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_fanout.h"

#include "ptx/ptx_bls.h"
#include "ptx/ptx_commit_reveal.h"
#include "crypto/sha256.h"
#include "logging.h"
#include "rpc/protocol.h"
#include "rpc/server.h"

#include <chrono>
#include <set>
#include <thread>
#include "support/events.h"
#include "sync.h"
#include "utilstrencodings.h"
#include "util/system.h"
#include "evo/deterministicgms.h"  // A: live-DGM-derived fan-out address resolution
#include "chainparamsbase.h"       // A: BaseParams().RPCPort() for the port-convention

#include <event2/buffer.h>
#include <event2/event.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <event2/util.h>

#include <univalue.h>

#include <cassert>
#include <deque>
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

// A (2026-08-12) — LIVE-DGM-DERIVED HOST RESOLUTION (the real address-book fix).
// Member addresses are read from CURRENT on-chain ground truth (the DGM list, by
// proTxHash) EVERY fan-out, so they track permissionless membership churn — the
// static -ptxnode snapshot (g_ptx_nodes) went stale on any growth (the 4th
// declared-vs-ground-truth divergence: ~39% of a grown quorum unaddressable).
// The DGM-HOST half below is PERMANENT and correct.
//
// PORT is a different matter: the DGM address advertises only the P2P port; the
// PTX-RPC endpoint that serves gm_bls_sign is NOT on-chain.  We combine DGM-host
// with the chain default RPC port (overridable via -ptxfanoutport).  ★★ THIS
// PORT-CONVENTION IS A FLEET EXPEDIENT, NOT THE MAINNET DELIVERY ★★ — it embeds
// "GMs expose PTX-RPC at DGM-IP:standard-port", which a permissionless operator
// violates (non-standard port, firewalled, unexposed), re-introducing the very
// address-resolution-assumption class this fix removes.  The MAINNET-CORRECT
// delivery is SIGN-OVER-P2P (reach the member at its on-chain-advertised address
// via the protocol it definitely speaks) — the OTHER HALF of this fix, same tier,
// still OWED (KDD-085).  Do not read "A" as done until P2P delivery lands.
//
// Precedence: prefer DGM-derived (real fix); fall back to static -ptxnode (legacy
// trusted-dealer path / tests / a member not yet in the list).  Returns the
// source in *via for logging.
// ★ THE FAN-OUT PORT CONVENTION, in ONE named place (was an inline GetArg spread
// at the call site — the implicit-convention defect this makes explicit).
// The DGM record advertises only the P2P endpoint; the PTX-RPC port that serves
// gm_bls_sign is NOT on-chain, so the fan-out assumes a member exposes its RPC at
// the DGM-advertised HOST on THIS port. Default = the chain RPC port
// (BaseParams().RPCPort() — 29995 on ptxbea); override with -ptxfanoutport for a
// non-standard deployment. ★ OPERATOR REQUIREMENT (documented in
// ptxbea-known-limitations.md §13): a GM MUST expose PTX-RPC on this port at the
// same address it registers, or it looks healthy on-chain and silently fails
// every signing request (the IPv6-incident class). KDD-085 (sign-over-P2P) would
// remove this requirement entirely by reaching the member at its on-chain
// address; this convention is the interim.
uint16_t PTX_FanoutRpcPort()
{
    return (uint16_t)gArgs.GetArg("-ptxfanoutport", BaseParams().RPCPort());
}

bool PTX_ResolveMemberAddr(const std::string& node_id, const uint256& proTxHash,
                           std::string& host_out, uint16_t& port_out, const char** via)
{
    if (!proTxHash.IsNull() && deterministicGMManager) {
        const auto dgm = deterministicGMManager->GetListAtChainTip().GetValidGM(proTxHash);
        if (dgm && dgm->pdgmState) {
            const CService& addr = dgm->pdgmState->addr;
            if (addr.IsValid()) {
                host_out = addr.ToStringIP();
                port_out = PTX_FanoutRpcPort();
                if (via) *via = "dgm";
                return true;
            }
        }
    }
    const PTXNodeInfo* ni = PTX_FindNode(node_id);
    if (ni) { host_out = ni->host; port_out = ni->port; if (via) *via = "static"; return true; }
    return false;
}

// Null-dnsbase discipline: obtain_evhttp_connection_base passes a NULL dnsbase,
// so a NON-numeric host makes libevent do a BLOCKING getaddrinfo inside the
// event loop — which serializes the parallel sign pass on that one member.
// DGM-derived hosts are always numeric (CService::ToStringIP); a static
// -ptxnode entry may not be. Asserted in debug builds, logged in release
// (the dial still proceeds — it blocks only its own slot's setup).
bool PTX_IsNumericHost(const std::string& host)
{
    struct in_addr a4;
    struct in6_addr a6;
    return evutil_inet_pton(AF_INET, host.c_str(), &a4) == 1 ||
           evutil_inet_pton(AF_INET6, host.c_str(), &a6) == 1;
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
// PTX_FanOutSign  (Phase 2 BLS)
// ---------------------------------------------------------------------------

namespace {

// Budget constants — see doc/ptx/FANOUT_BUDGET_ANALYSIS.md. The attempt count
// is a re-dial-opportunity count (one per 150ms tick), NOT a time bound; the
// wall-clock ceiling is the time bound: 30s is >2x the worst legitimate ladder
// observation (14.33s, at an RTT beyond mainnet p99) — it rejects nothing real
// and converts the minutes-scale stall class into a bounded clean failure the
// forfeit/abandon machinery already handles.
static const int FANOUT_MAX_ATTEMPTS = 60;
static const int FANOUT_RETRY_MS     = 150;
static const int FANOUT_WALL_MS      = 30000;

struct PTXSignRoundCtx;

// One member's dial. STABLE STORAGE REQUIRED: libevent callbacks hold a raw
// pointer to this object for its whole lifetime, so the round keeps these in
// a std::deque — never a vector, whose reallocation would move live callback
// targets. A re-dialed member gets a FRESH ctx and a fresh connection
// (connection reuse is deliberately deferred to a follow-up).
struct PTXSignDialCtx {
    std::string node_id;
    PTXHTTPReply reply;                 // filled by the completion callback
    PTXSignRoundCtx* round{nullptr};
    raii_evhttp_connection evcon;       // per-dial connection. The ctx deque is
                                        // declared AFTER the round's event_base,
                                        // so every connection is destroyed
                                        // BEFORE the base — the order libevent
                                        // requires.
};

// Whole-round shared state. Single-threaded by design: everything here runs
// inside ONE event_base_dispatch on the calling thread — no locks, and none
// may be added (callbacks must not block the loop).
//
// There are NO retry-pass barriers. A persistent 150ms timer re-dials members
// whose only answer so far is the RETRYABLE "commitment not seen" (-32051)
// while everyone else's dial keeps running concurrently; each answer is
// classified the moment it lands. Nothing ever waits on the slowest member —
// the round returns at the threshold-th FASTEST partial (loopbreak from the
// completion callback) and a straggler's dial is simply abandoned at
// teardown. (The first parallel cut kept per-pass barriers; because every
// roll's commitment is freshly broadcast, the first pass ends below threshold
// on not-seens, and the barrier then taxed EVERY roll by a slow member's full
// dial time — 4.25s measured with one 2000ms-netem member. The single-loop
// shape is what "stop at the sixth fastest" actually requires.)
struct PTXSignRoundCtx {
    event_base* base{nullptr};
    size_t threshold{0};
    std::map<std::string, std::vector<uint8_t>>* collected{nullptr};
    std::deque<PTXSignDialCtx>* dials{nullptr};
    std::set<std::string> pending;      // answered -32051; re-dialed next tick
    std::set<std::string> inflight;     // dial outstanding
    int ticks{0};
    std::chrono::steady_clock::time_point wall_start;
    // Dial-factory inputs — owned by PTX_FanOutSign's frame, which outlives
    // every callback:
    const uint256* round_seed{nullptr};
    const uint256* quorum_hash{nullptr};
    const std::map<std::string, uint256>* member_protx{nullptr};
};

bool ptx_sign_dial(PTXSignRoundCtx& round, const std::string& node_id);

// Nothing left that can produce another answer -> stop the loop (the
// persistent timer would otherwise keep the base dispatching forever).
void ptx_sign_check_drained(PTXSignRoundCtx* round)
{
    if (round->pending.empty() && round->inflight.empty())
        event_base_loopbreak(round->base);
}

// The error cb receives the request's cb_arg, which is a PTXSignDialCtx —
// ptx_http_error_cb's PTXHTTPReply cast would scribble on it.
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
void ptx_sign_dial_error_cb(enum evhttp_request_error err, void* arg)
{
    PTXSignDialCtx* ctx = static_cast<PTXSignDialCtx*>(arg);
    ctx->reply.error = (int)err;
}
#endif

// Completion callback: parse and CLASSIFY here — the round reacts to each
// answer the moment it lands (collect / mark retryable / drop); nothing is
// deferred to a barrier.
void ptx_sign_dial_done(struct evhttp_request* req, void* arg)
{
    PTXSignDialCtx* ctx = static_cast<PTXSignDialCtx*>(arg);
    ptx_http_done(req, &ctx->reply);    // handles req == NULL (aborted/failed)
    PTXSignRoundCtx* round = ctx->round;
    const std::string& node_id = ctx->node_id;
    const PTXHTTPReply& response = ctx->reply;
    round->inflight.erase(node_id);
    if (response.status != 200) {
        // Only the RETRYABLE "commitment not seen yet" (-32051, relay lag) is
        // re-dialed; terminal refusals and transport errors drop — retrying
        // cannot help them.
        bool retryable = false;
        try {
            UniValue res;
            if (res.read(response.body)) {
                UniValue err = find_value(res, "error");
                if (err.isObject()) {
                    UniValue code = find_value(err, "code");
                    if (code.isNum() && code.get_int() == RPC_PTX_COMMITMENT_NOT_SEEN)
                        retryable = true;
                }
            }
        } catch (...) {}
        // ODC-064: the server's error TEXT names the exact condition ("no
        // CURRENT sk_share…", "no commitment seen yet…") — keep it, TRIMMED and
        // newline-flattened so a broken or hostile peer cannot flood the log.
        static const size_t ODC064_BODY_MAX = 200;
        std::string body = response.body.substr(0, ODC064_BODY_MAX);
        for (char& ch : body) { if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' '; }
        LogPrintf("PTX: FanOutSign: %s HTTP %d %s body=%s%s\n", node_id, response.status,
                  retryable ? "RETRY(not-seen)" : "FAILED", body,
                  response.body.size() > ODC064_BODY_MAX ? " …[truncated]" : "");
        if (retryable) round->pending.insert(node_id);
        else ptx_sign_check_drained(round);
        return;
    }
    try {
        UniValue res;
        if (res.read(response.body)) {
            UniValue rval = find_value(res, "result");
            if (rval.isObject()) {
                UniValue sig_val = find_value(rval, "sig_hex");
                if (sig_val.isStr() && IsHex(sig_val.get_str())) {
                    std::vector<uint8_t> sig_bytes = ParseHex(sig_val.get_str());
                    if ((int)sig_bytes.size() == PTX_SIG_BYTES) {
                        (*round->collected)[node_id] = std::move(sig_bytes);
                        LogPrintf("PTX: FanOutSign: %s got partial sig (%d/%zu)\n",
                                  node_id, (int)round->collected->size(), round->threshold);
                        // STOP-AT-THRESHOLD: a t-of-n threshold signature is
                        // fully recoverable at t. This is the correctness of
                        // the t-of-n contract, not just an optimization:
                        // n=11/t=6 must gate at 6, never at 11.
                        if (round->threshold > 0 &&
                            round->collected->size() >= round->threshold) {
                            LogPrintf("PTX: FanOutSign: threshold %zu reached — returning "
                                      "(%zu partials, %d attempt(s))\n",
                                      round->threshold, round->collected->size(),
                                      round->ticks + 1);
                            event_base_loopbreak(round->base);
                            return;
                        }
                    } else {
                        LogPrintf("PTX: FanOutSign: %s bad sig size %d\n",
                                  node_id, (int)sig_bytes.size());
                    }
                }
            }
        }
    } catch (...) {
        LogPrintf("PTX: FanOutSign: %s parse error\n", node_id);
    }
    ptx_sign_check_drained(round);
}

// 150ms persistent tick: enforce the wall clock and the attempt budget, and
// re-dial every member currently marked retryable.
void ptx_sign_tick(evutil_socket_t, short, void* arg)
{
    PTXSignRoundCtx* round = static_cast<PTXSignRoundCtx*>(arg);
    const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - round->wall_start).count();
    if (wall_ms >= FANOUT_WALL_MS) {
        LogPrintf("PTX: FanOutSign: wall-clock ceiling %dms hit after %d tick(s) "
                  "(%zu collected, %d pending, %d inflight) — returning\n",
                  FANOUT_WALL_MS, round->ticks, round->collected->size(),
                  (int)round->pending.size(), (int)round->inflight.size());
        event_base_loopbreak(round->base);
        return;
    }
    if (++round->ticks >= FANOUT_MAX_ATTEMPTS) {
        if (!round->pending.empty() || !round->inflight.empty())
            LogPrintf("PTX: FanOutSign: %d member(s) still 'commitment not seen' after %d attempts "
                      "(propagation budget exhausted)\n",
                      (int)(round->pending.size() + round->inflight.size()),
                      FANOUT_MAX_ATTEMPTS);
        event_base_loopbreak(round->base);
        return;
    }
    if (!round->pending.empty()) {
        std::set<std::string> todo;
        todo.swap(round->pending);
        for (const auto& node_id : todo)
            ptx_sign_dial(*round, node_id);
    }
    ptx_sign_check_drained(round);
}

// Dial one member: resolve, connect, send gm_bls_sign. Returns false (with a
// log line) when the member cannot be dialed — it then drops out of the round.
// Called both from PTX_FanOutSign's frame and from the timer callback, so it
// must not throw (a throw through a libevent C callback is fatal).
bool ptx_sign_dial(PTXSignRoundCtx& round, const std::string& node_id)
{
    // A: resolve member address from the LIVE DGM list (by proTxHash),
    // falling back to static -ptxnode config.  Fixes the stale-address-book
    // failure (unaddressable grown members); DGM-host is the real fix, the
    // RPC port-convention is the fleet expedient (see PTX_ResolveMemberAddr).
    std::string mhost; uint16_t mport = 0; const char* via = "none";
    {
        auto pit = round.member_protx->find(node_id);
        const uint256 mprotx = (pit != round.member_protx->end()) ? pit->second : uint256();
        if (!PTX_ResolveMemberAddr(node_id, mprotx, mhost, mport, &via)) {
            LogPrintf("PTX: FanOutSign: no address for %s (DGM miss + no static node info)\n", node_id);
            return false;
        }
    }

    // Null-dnsbase discipline (see PTX_IsNumericHost): a non-numeric host
    // does a BLOCKING resolve during its dial's setup. Assert in debug;
    // in release, log and proceed — only this member's setup stalls.
    if (!PTX_IsNumericHost(mhost)) {
        LogPrintf("PTX: FanOutSign: %s host '%s' not numeric — NULL-dnsbase "
                  "blocking resolve stalls this dial's setup\n", node_id, mhost);
        assert(false && "non-numeric fan-out host with NULL dnsbase");
    }

    // KDD-070 P1: gm_bls_sign takes (round_seed_hex, quorum_hash) — the
    // quorum_hash selects which CURRENT share the member signs with.
    UniValue params(UniValue::VARR);
    params.push_back(round.round_seed->GetHex());
    params.push_back(round.quorum_hash->GetHex());

    // Raw HTTP; sig_hex parsed by the completion callback (not "accepted").
    round.dials->emplace_back();
    PTXSignDialCtx& ctx = round.dials->back();
    ctx.node_id = node_id;
    ctx.round = &round;
    try {
        ctx.evcon = obtain_evhttp_connection_base(round.base, mhost, mport);
    } catch (const std::exception& e) {
        LogPrintf("PTX: FanOutSign: %s connection setup failed: %s\n", node_id, e.what());
        return false;
    }
    evhttp_connection_set_timeout(ctx.evcon.get(), 5);

    raii_evhttp_request req = obtain_evhttp_request(ptx_sign_dial_done, &ctx);
    if (!req) return false;
#if LIBEVENT_VERSION_NUMBER >= 0x02010300
    evhttp_request_set_error_cb(req.get(), ptx_sign_dial_error_cb);
#endif

    struct evkeyvalq* hdrs = evhttp_request_get_output_headers(req.get());
    evhttp_add_header(hdrs, "Host", mhost.c_str());
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
    if (!out) return false;
    evbuffer_add(out, body.data(), body.size());

    int r = evhttp_make_request(ctx.evcon.get(), req.get(), EVHTTP_REQ_POST, "/");
    req.release(); // ownership transferred to evcon — looks like a leak to a
                   // tidy-up; it is the libevent contract. DO NOT "fix".
    if (r != 0) return false;
    round.inflight.insert(node_id);
    return true;
}

} // namespace

std::map<std::string, std::vector<uint8_t>> PTX_FanOutSign(
    const std::string& round_id,
    const uint256& round_seed,
    const uint256& quorum_hash,
    const std::vector<std::string>& member_ids,
    size_t threshold,
    const std::map<std::string, uint256>& member_protx)
{
    std::map<std::string, std::vector<uint8_t>> collected;

    // BUG-032 2b-iii — coordinator-side wait-not-reject. The commit-before-sign
    // flow broadcasts the PTXROLLCOMMIT immediately before this fan-out, so on the
    // first pass a member may not have RECEIVED it yet (relay lag) and returns the
    // RETRYABLE RPC_PTX_COMMITMENT_NOT_SEEN (-32051). That is "wait", not "fail":
    // retry ONLY those members, with a short backoff, bounded by a budget (~the
    // 2000ms real-network target). Terminal refusals (no share, sign failure) and
    // transport errors drop immediately — retrying cannot help them.
    // RESIDUAL-DECOMPOSITION EXPERIMENT (2026-08-14): the threshold-miss residual
    // decomposed into two not-seen populations — a 1-2-member near-miss tail
    // (propagation margin) and a 10-11-member TOTAL BLACKOUT (the commitment's
    // own INV trickle leaving the caller later than the whole 1.8s budget, so no
    // member has it). Both are timing, not inability (zero no-share / transport
    // in the data). 60 × 150ms ≈ 9s covers source-trickle (~2-5s) + mesh
    // propagation; stop-at-threshold means success still returns at the 6th
    // partial, so the raised ceiling costs nothing on the happy path — it only
    // spends time that previously became a forfeited stake.
    // Single-loop orchestration — the retry machinery (timer, classification,
    // re-dial) lives in the callbacks above. Declaration order is load-bearing
    // and must not be "tidied": base first, round second, ctx deque third,
    // timer last — destruction then runs timer → connections → round → base,
    // the order libevent requires (callbacks fired during connection teardown
    // still see a live round and base).
    raii_event_base base = obtain_event_base();
    PTXSignRoundCtx round;
    std::deque<PTXSignDialCtx> dials;
    round.base = base.get();
    round.threshold = threshold;
    round.collected = &collected;
    round.dials = &dials;
    round.wall_start = std::chrono::steady_clock::now();
    round.round_seed = &round_seed;
    round.quorum_hash = &quorum_hash;
    round.member_protx = &member_protx;

    size_t dialed = 0;
    for (const auto& node_id : member_ids) {
        // Test hook (mirrors FanOutReveal's fail-mode handling): a 'withhold' or
        // 'abstain' fail-mode set via ptx_debug_setnodefailmode makes the
        // coordinator NEVER collect this member's partial. This is the deliberate
        // way to drive FanOutSign below threshold and exercise the fund-then-sign
        // FORFEITURE / orphan-commit path on purpose (validate_fleet abandon_gate) —
        // the h510-class halt found by a NATURAL FanOutSign miss, now a tested case.
        // Never dialed → never collected, never retried. Never set in prod.
        {
            LOCK(cs_ptx_failmodes);
            auto fit = g_ptx_node_failmodes.find(node_id);
            if (fit != g_ptx_node_failmodes.end() &&
                (fit->second == "withhold" || fit->second == "abstain")) {
                LogPrintf("PTX: FanOutSign: %s %s (failmode) — not collecting\n",
                          node_id, fit->second);
                continue;
            }
        }
        if (ptx_sign_dial(round, node_id)) ++dialed;
    }
    if (dialed == 0) return collected;

    raii_event tick = obtain_event(base.get(), -1, EV_PERSIST, ptx_sign_tick, &round);
    struct timeval tv;
    tv.tv_sec = 0;
    tv.tv_usec = FANOUT_RETRY_MS * 1000;
    event_add(tick.get(), &tv);

    event_base_dispatch(base.get());
    return collected;
}
