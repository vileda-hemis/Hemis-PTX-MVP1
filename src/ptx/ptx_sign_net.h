// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_SIGN_NET_H
#define HEMIS_PTX_SIGN_NET_H

#include "primitives/transaction.h"
#include "uint256.h"

#include <chrono>
#include <string>
#include <vector>

class CDataStream;
class CNode;
class CConnman;

// ===========================================================================
// KDD-085 — sign-over-P2P.  The REJECTION PATH.
// ===========================================================================
// Plan: doc/ptx/W4B_COST_AND_KDD085_SCOPE.md §9 (§9.4 is this file's contract).
// Register: KDD-105 (a credential used where the question is not about identity
// is a design error), KDD-106, ODC-083.
//
// ★★ THE MODEL THIS REPLACES, AND WHY THE GUARD COMES FIRST.
// `gm_bls_sign` today is an authenticated RPC, and its authentication answers a
// question that was never about identity: the actual authorisation is the
// BUG-032 payment gate — "is there a funded commitment for this exact
// (round_seed, quorum_hash)?" — which every GM answers independently, from data
// it already holds, about a caller it has never heard of.  KDD-085 deletes the
// credential.  The caller does not authenticate; the GM trusts neither the
// caller, the requester, nor the other ten members.
//
// ★★ WHICH MEANS THE BOUND ON WORK-BEFORE-REJECTION IS THE ENTIRE DEFENCE.
// With no credential there is nothing else standing between a stranger's bytes
// and this node's locks.  Measured (§9.4): before the `bfea163` index, at the
// DEFAULT maxmempool=300, ~47 kbit/s of 88-byte requests held `mempool.cs`
// continuously — a liveness attack on block production, because that lock is
// contended by ATMP and block assembly, not a CPU burn.
//
// ★ SO THIS HEADER IS DELIBERATELY THE FIRST THING BUILT, BEFORE ANY HAPPY
// PATH.  The expensive path is added BEHIND an existing guard rather than
// guarded afterwards.  BUG-052 is the live worked example of the opposite order
// on this codebase — a pairing running before `CheckInputs`, handing an
// attacker expensive rejections for free.  The same shape one file over would
// be the same defect.

// ---------------------------------------------------------------------------
// Wire messages
// ---------------------------------------------------------------------------

// ★★ commit_raw IS MANDATORY, AND THAT IS A DESIGN DECISION LOAD-BEARING TWICE.
//
// (1) CORRECTNESS (§9.2).  Of the six checks a GM makes, five read chain state
//     or self-validating bytes.  The sixth — "is a funded commitment present?"
//     — reads the MEMPOOL, which is node-local: it varies by propagation,
//     policy and eviction, and GMs legitimately disagree about it.  That is why
//     the retryable "commitment not seen" path exists at all.  So "eleven
//     strangers reach the same verdict from public data" is FALSE of the one
//     check that decides.  Making the request carry its own evidence is what
//     makes the claim true: the GM then derives its verdict from caller-supplied
//     self-validating bytes checked against its own chain state, and the mempool
//     becomes a local cache rather than a dependency.
//
// (2) SURVIVABILITY (§9.4).  A mandatory attachment is what lets every cheap
//     check run before any lock: an attacker must produce a well-formed
//     PTXROLLCOMMIT naming their own round before this node takes `mempool.cs`.
//
// ★ THERE IS NO "LOOK IN MY MEMPOOL AND HOPE" PATH.  A request without its
// commitment attached is rejected — not softened to a mempool lookup, because
// anything softer reintroduces exactly the node-local disagreement the model
// cannot tolerate, and re-opens the pre-attachment DoS surface at the same time.
struct PTXSignReq {
    uint256                    round_seed;
    uint256                    quorum_hash;
    std::vector<unsigned char> commit_raw;   // MANDATORY — see above

    SERIALIZE_METHODS(PTXSignReq, obj)
    {
        READWRITE(obj.round_seed, obj.quorum_hash, obj.commit_raw);
    }
};

// The refusal taxonomy survives the transport change.  RPC had exactly this
// distinction (RPC_PTX_COMMITMENT_NOT_SEEN vs RPC_MISC_ERROR) and the caller's
// retry budget depends on it: a not-yet-propagated commitment is a WAIT, not a
// failure, and burning a round on ordinary network delay is a fee loss.
enum class PTXSignStatus : uint8_t {
    OK        = 0,   // sig carries a 96-byte partial
    RETRYABLE = 1,   // ask again — nothing about this request is wrong
    TERMINAL  = 2,   // do not ask again
};

struct PTXSignResp {
    uint256                    round_seed;
    uint256                    quorum_hash;
    uint8_t                    status{(uint8_t)PTXSignStatus::TERMINAL};
    std::vector<unsigned char> sig;      // 96 bytes iff status == OK
    std::string                reason;   // diagnostic only; never parsed

    SERIALIZE_METHODS(PTXSignResp, obj)
    {
        READWRITE(obj.round_seed, obj.quorum_hash, obj.status, obj.sig, obj.reason);
    }
};

// Correlation is (round_seed, quorum_hash) — already unique per round, so no
// new nonce is introduced and no new replay surface with it.

// ---------------------------------------------------------------------------
// Deserialize-time bounds
// ---------------------------------------------------------------------------

// A roll commitment is a few hundred bytes.  100 kB is the same cap the
// KDD-088 RPC attach path already applies (`ptx_mempool.cpp`), kept identical
// so the two entry points cannot drift into disagreeing about what is abusive.
static const uint64_t PTX_SIGNREQ_MAX_COMMIT_BYTES = 100000;

// Diagnostic-only, never parsed by the caller, so it is bounded hard: a peer
// cannot make us allocate a megabyte of "reason".
static const uint64_t PTX_SIGNRESP_MAX_REASON = 256;

// ---------------------------------------------------------------------------
// Per-peer rate limit — copied, not designed
// ---------------------------------------------------------------------------
// ★ These mirror `m_addr_token_bucket` (`net.h`, used at
// `net_processing.cpp:1645-1666`).  Deliberately NOT a new mechanism: an
// in-tree limiter that has run on every ADDR message for years is worth more
// than a better one written this week.
//
// Sizing: a legitimate caller sends ONE request per member per round, and
// retries only a refusal it was told is retryable.  A burst of 10 with a 1/s
// refill is generous for that and ruinous for a flood.
static const double PTX_SIGNREQ_TOKEN_BUCKET_MAX = 10.0;
static const double PTX_SIGNREQ_RATE_PER_SECOND  = 1.0;

// The limiter's arithmetic, factored out of the CNode path so it is testable
// WITHOUT a CNode and a live CConnman.  ★ Not cosmetic: this is a defence, and
// a defence that can only be exercised through a full network fixture is a
// defence that will be tested once and then trusted forever.
double PTX_SignReq_RefillBucket(double bucket, std::chrono::microseconds since_last);
// Spends one token if any remain.  false => the peer is over its rate.
bool   PTX_SignReq_SpendToken(double& bucket);

// ---------------------------------------------------------------------------
// The cheap check
// ---------------------------------------------------------------------------

// Why an enum and not a bool: the DISPOSITION differs per reason (see
// PTX_SignReq_IsMisbehaviour) and a bool cannot carry that.
enum class PTXSignReqReject {
    NONE = 0,
    MISSING_COMMITMENT,   // mandatory attachment absent — §9.2/§9.4
    TOO_LARGE,            // over PTX_SIGNREQ_MAX_COMMIT_BYTES
    DECODE_FAILED,        // not a deserializable transaction
    NOT_ROLLCOMMIT,       // deserialized, but is not a PTXROLLCOMMIT
    ROUND_MISMATCH,       // a PTXROLLCOMMIT, but not for the round asked about
};

const char* PTX_SignReqRejectString(PTXSignReqReject r);

// ★★ THE WHOLE POINT OF THIS FUNCTION IS WHAT IT CANNOT DO.
// It takes no mempool, no chain, no lock, and no node.  Its inputs are the
// request's own bytes and nothing else, so it is INCAPABLE of reaching a lock —
// the guarantee is structural rather than a promise about call order, and
// `Kdd085_CheapCheck_TakesNoLocks_Structural` asserts it against the source.
//
// Order is strictly cheapest-first: presence, then size, then deserialize, then
// type, then round-binding.  Every step is pure byte inspection.
//
// On NONE, commit_out holds the decoded commitment so the caller need not
// deserialize twice.
PTXSignReqReject PTX_SignReq_CheapCheck(const PTXSignReq& req,
                                        CTransactionRef& commit_out);

// Is this rejection a peer's fault?
// ★ Every current reason is a pure byte property of the request — none of them
// can be produced by an honest caller that merely lost a race — so all of them
// score.  The predicate exists so that when a reason IS added that an honest
// caller can hit (a reorg, a policy difference), the answer is a one-line change
// here instead of a forgotten `Misbehaving` call at the dispatcher.
bool PTX_SignReq_IsMisbehaviour(PTXSignReqReject r);

// The score for a rejected request.
// ★ NOT 100.  100 is this codebase's "unroutable command" score — an instant
// ban.  A caller is an unauthenticated stranger BY DESIGN under KDD-085, and
// banning strangers on their first malformed byte is how honest callers behind
// NAT or a version skew get partitioned off.  10 is the graduated-contextual
// precedent already used by the KDD-058-A landing relay, and it still bans at
// ten strikes.
static const int PTX_SIGNREQ_MISBEHAVIOUR_SCORE = 10;

// ---------------------------------------------------------------------------
// Dispatcher entry
// ---------------------------------------------------------------------------
// Returns false only for an unroutable command.  Everything else — rate limit,
// rejection, refusal — is handled inside and reported through `score_out`.
//
// ★ ACCEPTED FROM UNVERIFIED PEERS BY DESIGN.  GMAUTH does not transfer and
// could not be used: `ptx_dkg_net.cpp` keys the ceremony relay on
// `verifiedProRegTxHash` matched against session members, which is GM-to-GM by
// construction, and a caller is not a gamemaster.  The identity layer is what
// this change removes, not something it reuses.
bool PTX_SignNet_ProcessMessage(CNode* pfrom,
                                const std::string& strCommand,
                                CDataStream& vRecv,
                                CConnman& connman,
                                int& score_out);

#endif // HEMIS_PTX_SIGN_NET_H
