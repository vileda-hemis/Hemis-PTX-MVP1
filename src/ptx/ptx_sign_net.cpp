// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_sign_net.h"

#include "activegamemaster.h"   // component 2: this node's own proTxHash for the response
#include "logging.h"
#include "net.h"
#include "netmessagemaker.h"
#include "primitives/transaction.h"
#include "protocol.h"
#include "ptx/ptx_bls.h"      // component 2: PTX_SIG_BYTES
#include "ptx/ptx_mempool.h"  // component 2: the acceptance tail + the BUG-032 gate
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

PTXSignReqVetted PTX_SignReq_CheapCheck(const PTXSignReq& req,
                                        PTXSignReqReject& why_out)
{
    PTXSignReqVetted vetted;         // empty; only the success path fills it
#define PTX_SIGNREQ_REFUSE(r) do { why_out = (r); return vetted; } while (0)
    // 1. MANDATORY ATTACHMENT.  Cheapest possible discriminator — a size
    //    compare on a vector we already hold — and it is also the one that
    //    carries the model (§9.2).  There is no fallback to a mempool lookup;
    //    that path is what this design removes.
    if (req.commit_raw.empty()) PTX_SIGNREQ_REFUSE(PTXSignReqReject::MISSING_COMMITMENT);

    // 2. SIZE, before anything that allocates or parses.
    if (req.commit_raw.size() > PTX_SIGNREQ_MAX_COMMIT_BYTES) PTX_SIGNREQ_REFUSE(PTXSignReqReject::TOO_LARGE);

    // 3. DESERIALIZE.  ~1-2 us for a real commitment and the first step that
    //    costs more than a compare, which is why two cheaper checks precede it.
    CMutableTransaction mtx;
    try {
        CDataStream ss(req.commit_raw, SER_NETWORK, PROTOCOL_VERSION);
        ss >> mtx;
        // Trailing bytes are not a transaction.  Refusing them keeps the
        // encoding canonical, so the same commitment cannot arrive under two
        // different byte strings.
        if (!ss.empty()) PTX_SIGNREQ_REFUSE(PTXSignReqReject::DECODE_FAILED);
    } catch (const std::exception&) {
        PTX_SIGNREQ_REFUSE(PTXSignReqReject::DECODE_FAILED);
    }

    // 4. TYPE.  Two field compares (`primitives/transaction.h`).
    const auto tx = MakeTransactionRef(std::move(mtx));
    if (!tx->IsPTXRollCommitTx()) PTX_SIGNREQ_REFUSE(PTXSignReqReject::NOT_ROLLCOMMIT);

    // 5. ROUND BINDING.  A commitment for somebody else's round is not our
    //    business, and accepting it would let a caller push unrelated traffic
    //    through us.  This is the last check that can be made from the bytes
    //    alone — everything after it needs chain state.
    CPTXRollCommitPayload p;
    if (!GetTxPayload(*tx, p)) PTX_SIGNREQ_REFUSE(PTXSignReqReject::DECODE_FAILED);
    if (p.round_seed != req.round_seed || p.quorum_hash != req.quorum_hash) {
        PTX_SIGNREQ_REFUSE(PTXSignReqReject::ROUND_MISMATCH);
    }

    vetted.m_commit = tx;
    why_out = PTXSignReqReject::NONE;
    return vetted;
}
#undef PTX_SIGNREQ_REFUSE

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

bool PTX_SignReq_ConnectWindowExpired(int64_t elapsed_ms)
{
    return elapsed_ms >= PTX_SIGNREQ_CONNECT_MS;
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

// ===========================================================================
// ★★ THE EXPENSIVE PATH — THE SIX CHECKS, IN ORDER, STATED NOT MERELY ACHIEVED.
// ===========================================================================
// §9.2's table, and where each check actually happens.  The order below is
// cheapest-first and every step is a precondition of the next, which is why
// this comment exists rather than a reader having to reconstruct it:
//
//   1-5. (component 1, ALREADY DONE before this function can be called)
//        attachment present -> size -> decode -> is a PTXROLLCOMMIT -> names
//        THIS round.  Pure byte inspection, no lock.  Enforced by the token.
//
//   6.   ACCEPT THE ATTACHED COMMITMENT (PTX_AcceptVettedCommitment).
//        ★ This single call discharges FOUR of the six §9.2 checks, because
//        TryATMP already runs them and re-implementing them here would be two
//        code paths asked to agree about validity:
//          - check 2: well-formed, exactly one service-fee output to the
//            accumulator          -> CheckPTXRollCommitTx (:953-962)
//          - check 3: quorum CANONICAL, i.e. a real record ACTIVE at
//            nSeedHeight (BUG-033) -> :966-981
//          - check 4: anchor lag <= nPTXSeedHeightWindow (ODC-073) -> :984-995
//          - check 6: inputs exist and are unspent -> CheckInputs, inside ATMP
//        ★ AND IT IS WHAT TURNS "ASSERTED" INTO "PROVEN": up to here the caller
//        has merely CLAIMED payment; this checks the claim against OUR UTXO set
//        and OUR chainparams, reading nothing about the requester.
//
//   7.   THE UNCHANGED BUG-032 GATE (PTX_SignRoundIfCommitted): check 1
//        (a funded commitment for this exact pair is present -- now true because
//        WE accepted it) and check 5 (we hold a CURRENT share for quorum_hash),
//        then sign.
//
// ★ NOTE THE GATE IS CALLED, NOT REPRODUCED.  The P2P arm must be incapable of
// signing under conditions the RPC arm would refuse; the only way to guarantee
// that is to call the same function.
void PTX_SignReq_ServeVetted(const PTXSignReq& req,
                             const PTXSignReqVetted& vetted,
                             PTXSignResp& resp)
{
    resp.round_seed  = req.round_seed;
    resp.quorum_hash = req.quorum_hash;
    resp.status      = (uint8_t)PTXSignStatus::TERMINAL;
    // Advisory and self-checking (see the field's comment): we say who we are,
    // the caller verifies the partial against that member's public share.
    if (activeGamemasterManager != nullptr) {
        resp.signer_protx = activeGamemasterManager->GetProTx();
    }

    // ★ The behavioural half of the token guarantee. The compiler stops anyone
    // reaching this function without running the cheap check; it cannot stop
    // them ignoring its return value. An ignored rejection leaves an EMPTY
    // token, and an empty token refuses here rather than falling through to the
    // mempool with a null commitment.
    if (!vetted.IsVetted()) {
        resp.reason = "internal: unvetted request reached the signing path";
        LogPrintf("PTX signreq: REFUSED (unvetted token reached ServeVetted) round_seed=%s\n",
                  req.round_seed.ToString());
        return;
    }

    // ── 6. Accept the attached commitment: assertion becomes proof ───────────
    std::string accept_err;
    if (!PTX_AcceptVettedCommitment(vetted.Commitment(), req.round_seed,
                                    req.quorum_hash, accept_err)) {
        // ★ TERMINAL, and this is the one place the P2P arm is deliberately
        // STRICTER than the RPC arm. Over RPC a bad attachment falls through to
        // "maybe my mempool has it anyway" and answers RETRYABLE. Here there is
        // no mempool fallback by design (§9.2), so a commitment WE could not
        // accept is a commitment we will never sign for: re-sending identical
        // bytes cannot change our UTXO set, and telling the caller to retry
        // would invite exactly that.
        resp.reason = accept_err.substr(0, PTX_SIGNRESP_MAX_REASON);
        LogPrintf("PTX signreq: REFUSED (commitment not acceptable: %s) round_seed=%s\n",
                  accept_err, req.round_seed.ToString());
        return;
    }

    // ── 7. The unchanged BUG-032 gate: commitment present + share held → sign ─
    uint8_t   sig_buf[PTX_SIG_BYTES];
    std::string sign_err;
    bool        retryable = false;
    if (!PTX_SignRoundIfCommitted(req.round_seed, req.quorum_hash, sig_buf,
                                  sign_err, &retryable)) {
        // The retryable/terminal split is the gate's own verdict, carried
        // through unchanged — it is what stops ordinary propagation delay being
        // charged to the caller as a forfeited fee.
        resp.status = (uint8_t)(retryable ? PTXSignStatus::RETRYABLE
                                          : PTXSignStatus::TERMINAL);
        resp.reason = sign_err.substr(0, PTX_SIGNRESP_MAX_REASON);
        LogPrintf("PTX signreq: %s quorum=%s reason=%s\n",
                  retryable ? "RETRY" : "FAILED",
                  req.quorum_hash.ToString(), sign_err);
        return;
    }

    resp.status = (uint8_t)PTXSignStatus::OK;
    resp.sig.assign(sig_buf, sig_buf + PTX_SIG_BYTES);
    LogPrintf("PTX signreq: SIGNED quorum=%s round_seed=%s sig[0..3]=%02x%02x%02x%02x\n",
              req.quorum_hash.ToString(), req.round_seed.ToString(),
              sig_buf[0], sig_buf[1], sig_buf[2], sig_buf[3]);
}

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
    PTXSignReqReject why = PTXSignReqReject::NONE;
    const PTXSignReqVetted vetted = PTX_SignReq_CheapCheck(req, why);
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

    // ── THE EXPENSIVE PATH ──────────────────────────────────────────────────
    // A request reaching this line is syntactically a plausible paid round for
    // a round this node was asked about, and only such a request earns a lock.
    // Unreachable without `vetted`, which only the cheap check can produce.
    PTXSignResp resp;
    PTX_SignReq_ServeVetted(req, vetted, resp);
    CNetMsgMaker msgMaker(pfrom->GetSendVersion());
    connman.PushMessage(pfrom, msgMaker.Make(NetMsgType::PTXSIGNRESP, resp));
    return true;
}
