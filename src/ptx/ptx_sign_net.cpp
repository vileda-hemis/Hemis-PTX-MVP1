// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_sign_net.h"

#include "logging.h"
#include "net.h"
#include "netmessagemaker.h"
#include "primitives/transaction.h"
#include "protocol.h"
#include "streams.h"
#include "utiltime.h"

#include <algorithm>

// ★★ THIS TRANSLATION UNIT DELIBERATELY DOES NOT INCLUDE txmempool.h OR
// validation.h.  The §9.4 claim is that a request is rejected before this node
// takes `mempool.cs`, and the cheapest way to make that claim TRUE rather than
// MAINTAINED is to put the guard somewhere the lock is not reachable.  A future
// edit that needs the mempool here has to add the include, which is visible in
// review and asserted against by
// `Kdd085_CheapCheck_TakesNoLocks_Structural`.

const char* PTX_SignReqRejectString(PTXSignReqReject r)
{
    switch (r) {
        case PTXSignReqReject::NONE:               return "ok";
        case PTXSignReqReject::MISSING_COMMITMENT: return "no commitment attached";
        case PTXSignReqReject::TOO_LARGE:          return "attached commitment too large";
        case PTXSignReqReject::DECODE_FAILED:      return "attached commitment decode failed";
        case PTXSignReqReject::NOT_ROLLCOMMIT:     return "attached tx is not a PTXROLLCOMMIT";
        case PTXSignReqReject::ROUND_MISMATCH:     return "attached commitment does not match this round";
    }
    return "unknown";
}

bool PTX_SignReq_IsMisbehaviour(PTXSignReqReject r)
{
    // Every reason is a pure byte property of the request: an honest caller
    // cannot produce any of them by losing a race, being behind on blocks, or
    // disagreeing with us about the mempool.  So all of them score.  See the
    // header for why this is a predicate rather than an `r != NONE` at the
    // call site.
    return r != PTXSignReqReject::NONE;
}

PTXSignReqReject PTX_SignReq_CheapCheck(const PTXSignReq& req,
                                        CTransactionRef& commit_out)
{
    // 1. MANDATORY ATTACHMENT.  Cheapest possible discriminator — a size
    //    compare on a vector we already hold — and it is also the one that
    //    carries the model (§9.2).  There is no fallback to a mempool lookup;
    //    that path is what this design removes.
    if (req.commit_raw.empty()) return PTXSignReqReject::MISSING_COMMITMENT;

    // 2. SIZE, before anything that allocates or parses.
    if (req.commit_raw.size() > PTX_SIGNREQ_MAX_COMMIT_BYTES) return PTXSignReqReject::TOO_LARGE;

    // 3. DESERIALIZE.  ~1-2 us for a real commitment and the first step that
    //    costs more than a compare, which is why two cheaper checks precede it.
    CMutableTransaction mtx;
    try {
        CDataStream ss(req.commit_raw, SER_NETWORK, PROTOCOL_VERSION);
        ss >> mtx;
        // Trailing bytes are not a transaction.  Refusing them keeps the
        // encoding canonical, so the same commitment cannot arrive under two
        // different byte strings.
        if (!ss.empty()) return PTXSignReqReject::DECODE_FAILED;
    } catch (const std::exception&) {
        return PTXSignReqReject::DECODE_FAILED;
    }

    // 4. TYPE.  Two field compares (`primitives/transaction.h`).
    const auto tx = MakeTransactionRef(std::move(mtx));
    if (!tx->IsPTXRollCommitTx()) return PTXSignReqReject::NOT_ROLLCOMMIT;

    // 5. ROUND BINDING.  A commitment for somebody else's round is not our
    //    business, and accepting it would let a caller push unrelated traffic
    //    through us.  This is the last check that can be made from the bytes
    //    alone — everything after it needs chain state.
    CPTXRollCommitPayload p;
    if (!GetTxPayload(*tx, p)) return PTXSignReqReject::DECODE_FAILED;
    if (p.round_seed != req.round_seed || p.quorum_hash != req.quorum_hash) {
        return PTXSignReqReject::ROUND_MISMATCH;
    }

    commit_out = tx;
    return PTXSignReqReject::NONE;
}

// Refill and spend, copied from the ADDR limiter
// (`net_processing.cpp:1645-1666`).
double PTX_SignReq_RefillBucket(double bucket, std::chrono::microseconds since_last)
{
    // Don't increment a full bucket -- and clamp a negative interval to zero, so
    // a clock that steps backwards cannot DRAIN the bucket and lock a peer out.
    if (bucket >= PTX_SIGNREQ_TOKEN_BUCKET_MAX) return PTX_SIGNREQ_TOKEN_BUCKET_MAX;
    const auto diff = std::max(since_last, std::chrono::microseconds{0});
    const double increment =
        std::chrono::duration_cast<std::chrono::duration<double>>(diff).count() *
        PTX_SIGNREQ_RATE_PER_SECOND;
    return std::min<double>(bucket + increment, PTX_SIGNREQ_TOKEN_BUCKET_MAX);
}

bool PTX_SignReq_SpendToken(double& bucket)
{
    if (bucket < 1.0) return false;
    bucket -= 1.0;
    return true;
}

namespace {

bool ConsumeSignReqToken(CNode* pfrom)
{
    const auto now = GetTime<std::chrono::microseconds>();
    pfrom->m_ptx_signreq_token_bucket =
        PTX_SignReq_RefillBucket(pfrom->m_ptx_signreq_token_bucket,
                                 now - pfrom->m_ptx_signreq_token_timestamp);
    pfrom->m_ptx_signreq_token_timestamp = now;
    return PTX_SignReq_SpendToken(pfrom->m_ptx_signreq_token_bucket);
}

void PushRefusal(CNode* pfrom, CConnman& connman, const PTXSignReq& req,
                 PTXSignStatus status, const std::string& reason)
{
    PTXSignResp resp;
    resp.round_seed  = req.round_seed;
    resp.quorum_hash = req.quorum_hash;
    resp.status      = (uint8_t)status;
    resp.reason      = reason.substr(0, PTX_SIGNRESP_MAX_REASON);
    CNetMsgMaker msgMaker(pfrom->GetSendVersion());   // Make() is non-const in this fork
    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::PTXSIGNRESP, resp));
}

} // anonymous namespace

bool PTX_SignNet_ProcessMessage(CNode* pfrom,
                                const std::string& strCommand,
                                CDataStream& vRecv,
                                CConnman& connman,
                                int& score_out)
{
    if (strCommand != NetMsgType::PTXSIGNREQ) return false;

    // ── RATE LIMIT, BEFORE DESERIALIZATION ──────────────────────────────────
    // ★ First, because it is the only check cheaper than parsing, and because
    // an exhausted bucket is NOT misbehaviour: it is a peer asking faster than
    // we serve.  Drop silently — no score, and no reply, since a reply to a
    // flood is amplification.
    if (!ConsumeSignReqToken(pfrom)) {
        LogPrint(BCLog::NET, "PTX signreq: rate-limited peer=%d\n", pfrom->GetId());
        return true;
    }

    PTXSignReq req;
    try {
        vRecv >> req;
    } catch (const std::exception&) {
        // Undeserializable at the message level — the peer is not speaking the
        // protocol.  Same score as a failed cheap check; see the header for why
        // it is 10 and not 100.
        score_out = PTX_SIGNREQ_MISBEHAVIOUR_SCORE;
        LogPrint(BCLog::NET, "PTX signreq: malformed message peer=%d\n", pfrom->GetId());
        return true;
    }

    // ── THE CHEAP CHECK ─────────────────────────────────────────────────────
    // Everything above and inside this call is pure byte inspection.  No lock
    // of any kind has been taken on this path.
    CTransactionRef commit;
    const PTXSignReqReject why = PTX_SignReq_CheapCheck(req, commit);
    if (why != PTXSignReqReject::NONE) {
        if (PTX_SignReq_IsMisbehaviour(why)) score_out = PTX_SIGNREQ_MISBEHAVIOUR_SCORE;
        // ★ Refuse TERMINAL, not retryable: every one of these is a property of
        // the bytes the peer sent, so re-sending the same request cannot help
        // and telling it to retry would invite exactly that.
        PushRefusal(pfrom, connman, req, PTXSignStatus::TERMINAL, PTX_SignReqRejectString(why));
        LogPrint(BCLog::NET, "PTX signreq: REFUSED peer=%d reason=%s\n",
                 pfrom->GetId(), PTX_SignReqRejectString(why));
        return true;
    }

    // ═══════════════════════════════════════════════════════════════════════
    // ★★ THE EXPENSIVE PATH GOES HERE, AND NOWHERE ELSE — COMPONENT 2.
    // ═══════════════════════════════════════════════════════════════════════
    // A request that reaches this line is syntactically a plausible paid round
    // for THIS node's round_seed/quorum_hash, and only such a request earns a
    // lock.  Component 2 replaces the refusal below with: accept the attached
    // commitment through the normal path, then the UNCHANGED BUG-032 gate
    // (`PTX_SignRoundIfCommitted`) decides, mapping its `retryable` out-param
    // onto PTXSignStatus.
    //
    // ★ Until then this is a deliberate dormant stub, not an oversight: the
    // guard is built first so the work is added behind it rather than guarded
    // afterwards (§9, build order 1).  No production caller sends PTXSIGNREQ
    // yet — the fan-out is still HTTP — so this refusal is unreachable in
    // practice and answering it honestly beats staying silent.
    LogPrint(BCLog::NET, "PTX signreq: accepted-by-guard, signing not yet armed peer=%d round_seed=%s\n",
             pfrom->GetId(), req.round_seed.ToString());
    PushRefusal(pfrom, connman, req, PTXSignStatus::TERMINAL,
                "sign-over-P2P not yet armed on this node");
    return true;
}
