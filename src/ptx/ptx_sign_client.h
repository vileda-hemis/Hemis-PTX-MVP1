// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_SIGN_CLIENT_H
#define HEMIS_PTX_SIGN_CLIENT_H

#include "ptx/ptx_sign_net.h"
#include "sync.h"
#include "uint256.h"

#include <map>
#include <string>
#include <vector>

class CConnman;

// ---------------------------------------------------------------------------
// Coordinator-side fail-mode map — MOVED here from ptx_fanout.h (component 4)
// ---------------------------------------------------------------------------
// ★ This is NOT dead weight carried along with the deletion: `PTX_FanOutSign`
// applied these live (its lines 683-694), and `ptx_debug_setnodefailmode` is how
// `validate_fleet`'s abandon-gate drives a round BELOW threshold on purpose, to
// exercise the fund-then-sign FORFEITURE path that the h510-class halt was first
// found by. Losing it would silently retire a tested failure case.
// ★ "abstain" and "withhold" both mean the same thing here as they did there:
// the caller never collects this member's partial. Applied at SEND time -- the
// member is simply never asked -- which is the honest analogue now that the
// member answers for itself rather than being dialled.
extern RecursiveMutex cs_ptx_failmodes;
extern std::map<std::string, std::string> g_ptx_node_failmodes;

// ===========================================================================
// KDD-085 component 3 — the CALLER side of sign-over-P2P.
// ===========================================================================
//
// ★★ THIS IS A SEPARATE TRANSLATION UNIT ON PURPOSE, AND IT IS NOT TIDINESS.
// `ptx_sign_net.cpp` carries component 1's structural guarantee: it includes
// neither `txmempool.h` nor `validation.h`, so the RESPONDER's cheap path
// cannot reach `mempool.cs`, and a test asserts that against the source. The
// caller needs the quorum store and the connection manager. Putting it in the
// same file would either weaken that guarantee or force the assertion to grow
// exceptions until it stopped meaning anything. Two files, one guarantee each.
//
// ★★ AND THE CALLER IS NOT A DIALER ANY MORE, WHICH IS THE SHAPE CHANGE.
// `ptx_fanout.cpp`'s 711 lines are mostly HTTP connection management: an
// `event_base` dispatched on the calling thread, a `PTXSignDialCtx` per dial
// held in a deque because "libevent callbacks hold a raw pointer ... never a
// vector, whose reallocation would move live callback targets", `raii_evhttp_
// connection` objects that must be destroyed before the base, and completion
// callbacks that classify replies inside the loop. **None of that has an
// analogue here.** Sending is `PushMessage`; replies arrive later, on the
// message-processing thread, as ordinary P2P messages.
//
// So the caller collapses to two things: **a round registry that the net thread
// writes and the RPC thread reads, and a wait.** The synchronous request/reply
// architecture does not transfer, and nothing needs to replace it.

// Per-member state within one signing round.
//
// ★ TERMINAL IS ABSORBING, AND THAT IS THE WHOLE POINT OF THE DISTINCTION.
// The responder answers TERMINAL exactly when re-sending identical bytes cannot
// change its verdict (component 2). So a TERMINAL member is removed from the
// round permanently: not re-sent, not re-framed. There is no alternative
// framing to try — the request is fully determined by (round_seed, quorum_hash,
// commitment), all three fixed for the round — so "try something else" is not
// an option the caller has. It stops asking, and recomputes whether the round
// can still be won.
enum class PTXMemberSignState {
    UNSENT = 0,
    INFLIGHT,     // request sent, nothing back yet
    RETRYABLE,    // refused, but the refusal was about propagation — re-send
    TERMINAL,     // refused finally — never re-send this member
    COLLECTED,    // partial in hand
    UNREACHABLE,  // gave us nothing within its budget — like TERMINAL, absorbing
    _COUNT        // ★ sentinel — see the static_assert in ptx_sign_client.cpp
};

// ★★ EVERY NON-ABSORBING STATE MUST HAVE A BOUNDED EXIT, AND THE COMPILER
// CHECKS THAT A NEW STATE DECLARES WHICH IT IS.
// This rule exists because the same defect shipped THREE times in this one
// state machine:
//   UNSENT     — entered on "connection opening", no exit at all. Found by the
//                fleet: 0 partials, every counter zero, full 30s wall burned.
//   INFLIGHT   — entered on a successful send, exits only via OnResponse. A
//                member that accepts the message and goes silent (an old binary
//                ignoring an unknown command -- §9.13(h)) sits here forever.
//   RETRYABLE  — entered on a retryable refusal, exits only back to INFLIGHT,
//                with NO CAP on the cycle. A member that answers "commitment
//                not seen" indefinitely never retires. ★ The RPC fan-out this
//                replaced DID bound exactly this (FANOUT_MAX_ATTEMPTS, derived
//                as wall/tick); dropping the dialer dropped the cap with it, so
//                the P2P caller was STRICTLY WEAKER than the path it replaced,
//                in precisely the case that path was designed around.
// ★ All three share one shape: a state whose only exit depends on the far side
// cooperating. The fix is not three bounds bolted on -- it is that a member has
// a BUDGET, and anything still non-absorbing when the budget runs out retires.
bool PTX_SignReq_IsAbsorbing(PTXMemberSignState s);

// Overall per-member budget. A member that has produced no partial within this
// retires to UNREACHABLE regardless of which non-absorbing state it is in.
// ★ Strictly inside the wall, or the whole mechanism is decorative: the point
// is that winnability can fire EARLY, and it cannot if the budget outlives the
// round. Asserted by test.
static const int PTX_SIGNREQ_MEMBER_MS = 15000;

// Should this member be retired now? `elapsed_ms` is since its first attempt.
PTXMemberSignState PTX_SignReq_RetireExpired(PTXMemberSignState s, int64_t elapsed_ms);

// ---------------------------------------------------------------------------
// The authoritative member binding
// ---------------------------------------------------------------------------
// ★★ THE LAGRANGE x MUST NEVER COME FROM THE WIRE, AND THE COMPILER ENFORCES IT.
//
// A partial signature is meaningless without the x it was evaluated at (the
// score-order `share_index`, KDD-052). Getting that wrong yields wrong lambdas
// and a recovered signature that simply does not verify.
//
// The response carries `signer_protx`, which is a HINT the responder supplies.
// If the caller resolved x from that field, a hostile peer could claim to be
// another member and have its partial inserted at someone else's index —
// poisoning recovery, with the round failing later and elsewhere.
//
// So: **the authoritative binding is "I sent this request to peer P, and I chose
// P because it is member M"** — caller-side state, never attacker-supplied.
// `PTXSignRoundMember` can only be produced by resolving a NodeId against the
// round's own send table; `RecordPartial` takes one. A partial therefore CANNOT
// be recorded under an identity that came off the wire.
//
// ★ `signer_protx` is used, but only as a CROSS-CHECK: a mismatch against M is
// a hard reject, which catches a misrouted reply and a lying peer cheaply.
// ★★ AND THE LIMIT, STATED BECAUSE OVERSTATING IT WOULD BE THE KDD-105 ERROR:
// this catches MISROUTING AND MISLABELLING. It does NOT catch a member sending
// a validly-labelled BAD PARTIAL. Per-partial cryptographic verification would
// need each member's public share, and the quorum record persists only
// `group_pk` and `vvec_hash` (`ptx_quorum_store.h:137-138`) — the vvec itself is
// not stored, so pk_i is not derivable. A bad partial therefore still fails the
// round at recovery with no attribution, exactly as it does over RPC today.
// Unchanged, not fixed, and registered rather than glossed.
class PTXSignRound;

class PTXSignRoundMember
{
public:
    const std::string& NodeId() const { return m_node_id; }
    bool IsResolved() const { return !m_node_id.empty(); }

private:
    PTXSignRoundMember() = default;
    std::string m_node_id;
    friend class PTXSignRound;
};

// ---------------------------------------------------------------------------
// How a round ends
// ---------------------------------------------------------------------------
enum class PTXSignRoundOutcome {
    THRESHOLD_MET = 0,  // enough partials; proceed
    UNWINNABLE,         // ★ provably cannot reach threshold — stop now, not at the wall
    DEADLINE,           // wall clock hit with the round still theoretically winnable
    NO_QUORUM_CONTACT,  // could not reach any member at all
};

const char* PTXSignRoundOutcomeString(PTXSignRoundOutcome o);

// ---------------------------------------------------------------------------
// Winnability — the caller-side consequence of the TERMINAL/RETRYABLE split
// ---------------------------------------------------------------------------
// ★★ THIS IS A CAPABILITY THE RPC ARM DOES NOT HAVE, and it falls straight out
// of component 2's typed refusals. Over RPC every failure looks alike, so the
// caller burns the full 30 s wall whatever the shape of the failures.
//
// Here the shape is legible. With t = 6 of 11:
//   5 TERMINAL + 6 RETRYABLE -> max reachable 6 -> STILL WINNABLE, keep going
//   6 TERMINAL + 5 RETRYABLE -> max reachable 5 -> UNWINNABLE, stop immediately
// Only one of those is worth waiting on, and only one is worth retrying the
// whole round for.
//
// ★ HONEST ABOUT WHAT THIS SAVES: it does NOT save the fee. The commitment is
// broadcast before any signing (BUG-032 fund-then-sign), so it is already
// forfeit when the round fails. What it saves is 28 seconds of a caller
// spinning against members that have given final answers, and — more useful —
// it makes the failure legible at the moment it becomes certain instead of at
// the wall, where every failure looks like a timeout.
//
// max_reachable = collected + inflight + retryable + unsent.
bool PTX_SignRound_StillWinnable(size_t collected, size_t inflight,
                                 size_t retryable, size_t unsent,
                                 size_t threshold);

// ---------------------------------------------------------------------------
// The round
// ---------------------------------------------------------------------------
struct PTXSignRoundResult {
    PTXSignRoundOutcome outcome{PTXSignRoundOutcome::NO_QUORUM_CONTACT};
    // node_id -> 96-byte partial, for the members that answered OK.
    std::map<std::string, std::vector<uint8_t>> partials;
    // Failure shape, for the operator-facing diagnostic. Counting these is what
    // turns "the roll failed" into "six members refused finally".
    size_t terminal{0};
    size_t retryable{0};
    size_t unreachable{0};
    size_t protx_mismatch{0};   // replies rejected by the cross-check
};

// Run one signing round over P2P. Blocks the calling (RPC) thread until the
// threshold is met, the round becomes unwinnable, or the wall clock expires.
//
// ★ TIMEOUTS: the 30 s wall (FANOUT_WALL_MS) is kept and remains the single
// authority, and the per-dial 3 s/5 s connect timeouts DISAPPEAR — they were
// `evhttp_connection` setup and there is no connection to set up.
// ★★ A BLOCK-DENOMINATED DEADLINE IS NOT AVAILABLE, and the plan's reason for
// saying so is now out of date: §9.5 assumed the consensus same-block mandate
// `nExpiryHeight == nSeedHeight` was the one height bound. **BUG-034 Phase 2
// retired that equality** — only the structural floor `nExpiryHeight >=
// nSeedHeight` survives (`ptxcommit-window-negative`,
// `specialtx_validation.cpp:950`), and a settle may now be mined at any depth.
// So there is no consensus height bound to denominate against at all, which
// makes the wall clock the only honest deadline rather than merely the
// convenient one.
PTXSignRoundResult PTX_SignRound_Run(const uint256& round_seed,
                                     const uint256& quorum_hash,
                                     const std::vector<std::string>& member_ids,
                                     const std::map<std::string, uint256>& member_protx,
                                     const std::vector<unsigned char>& commit_raw,
                                     size_t threshold,
                                     CConnman& connman);

// Called from net_processing when a `ptxsignresp` arrives. Never throws.
// Returns false if the response did not match any live round (stale, or for a
// round this node is not running) — not misbehaviour on its own.
bool PTX_SignClient_OnResponse(int64_t from_node, const PTXSignResp& resp);

#endif // HEMIS_PTX_SIGN_CLIENT_H
