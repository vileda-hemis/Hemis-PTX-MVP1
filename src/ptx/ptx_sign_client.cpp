// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_sign_client.h"

#include "evo/deterministicgms.h"
#include "logging.h"
#include "net.h"
#include "netmessagemaker.h"
#include "protocol.h"
#include "version.h"   // PTX_SIGNREQ_MIN_PROTO_VERSION
#include "chainparams.h"
#include "netbase.h"             // LookupNumeric
#include "utiltime.h"   // GetTimeMillis for the connect window

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>

RecursiveMutex cs_ptx_failmodes;
std::map<std::string, std::string> g_ptx_node_failmodes;

const char* PTXSignRoundOutcomeString(PTXSignRoundOutcome o)
{
    switch (o) {
        case PTXSignRoundOutcome::THRESHOLD_MET:     return "threshold met";
        case PTXSignRoundOutcome::UNWINNABLE:        return "unwinnable";
        case PTXSignRoundOutcome::DEADLINE:          return "deadline";
        case PTXSignRoundOutcome::NO_QUORUM_CONTACT: return "no quorum contact";
    }
    return "unknown";
}

// ★★ THE COMPLETENESS GUARD. Adding a state to PTXMemberSignState without
// classifying it here FAILS TO COMPILE -- the same standard component 1 set for
// the cheap-check ordering: a rule the compiler remembers, not one a reviewer
// has to. Both the static_assert and the default-less switch are load-bearing:
// the assert catches a state added at the end, the switch catches one added in
// the middle that shifts _COUNT back into agreement.
static_assert((int)PTXMemberSignState::_COUNT == 7,
              "PTXMemberSignState gained or lost a state: classify it in "
              "PTX_SignReq_IsAbsorbing and PTX_SignReq_RetireExpired, and say "
              "whether it is absorbing or has a bounded exit. Three separate "
              "defects in this machine were states with no exit.");

bool PTX_SignReq_IsAbsorbing(PTXMemberSignState s)
{
    switch (s) {                                   // no default, on purpose
        case PTXMemberSignState::COLLECTED:   return true;
        case PTXMemberSignState::TERMINAL:    return true;
        case PTXMemberSignState::UNREACHABLE: return true;
        // ★ Absorbing: a peer's protocol version does not change mid-round.
        case PTXMemberSignState::TOO_OLD:     return true;
        case PTXMemberSignState::UNSENT:      return false;
        case PTXMemberSignState::INFLIGHT:    return false;
        case PTXMemberSignState::RETRYABLE:   return false;
        case PTXMemberSignState::_COUNT:      break;
    }
    return false;   // unreachable for a classified enum
}

PTXMemberSignState PTX_SignReq_RetireExpired(PTXMemberSignState s, int64_t elapsed_ms)
{
    // Absorbing states are final -- never revisited, never re-timed.
    if (PTX_SignReq_IsAbsorbing(s)) return s;
    // ★ ONE budget for every non-absorbing state. Not three bounds bolted on:
    // the property is "a member that has produced no partial within its budget
    // stops holding the round open", and that is true regardless of WHICH way
    // it is failing to produce one.
    if (elapsed_ms >= PTX_SIGNREQ_MEMBER_MS) return PTXMemberSignState::UNREACHABLE;
    return s;
}

bool PTX_SignRound_StillWinnable(size_t collected, size_t inflight,
                                 size_t retryable, size_t unsent,
                                 size_t threshold)
{
    // Everything that could still become a partial. TERMINAL and UNREACHABLE
    // members are absent from this sum by construction — that is what makes
    // them absorbing.
    return collected + inflight + retryable + unsent >= threshold;
}

// ---------------------------------------------------------------------------
// The round
// ---------------------------------------------------------------------------

class PTXSignRound
{
public:
    PTXSignRound(const uint256& seed, const uint256& qh, size_t threshold)
        : m_seed(seed), m_qh(qh), m_threshold(threshold) {}

    // ★ THE ONLY WAY TO NAME A MEMBER. Resolves the peer that sent us something
    // against the round's OWN send table — caller-side state, written when we
    // chose to send. Nothing from the wire reaches this decision.
    PTXSignRoundMember ResolvePeer(int64_t node) const EXCLUSIVE_LOCKS_REQUIRED(m_cs)
    {
        PTXSignRoundMember m;
        auto it = m_peer_to_member.find(node);
        if (it != m_peer_to_member.end()) m.m_node_id = it->second;
        return m;
    }

    // Recording a partial REQUIRES a resolved member. An identity that came off
    // the wire cannot be turned into one of these, so it cannot select a
    // Lagrange x.
    void RecordPartial(const PTXSignRoundMember& who, std::vector<unsigned char> sig)
        EXCLUSIVE_LOCKS_REQUIRED(m_cs)
    {
        if (!who.IsResolved()) return;
        m_state[who.NodeId()] = PTXMemberSignState::COLLECTED;
        m_partials[who.NodeId()] = std::move(sig);
    }

    RecursiveMutex m_cs;
    const uint256 m_seed;
    const uint256 m_qh;
    const size_t  m_threshold;

    std::map<int64_t, std::string>                       m_peer_to_member;
    // ★★ ADDRESS, NOT PEER-ID. The defect this replaces: `m_member_to_peer` was
    // written ONLY on the already-connected send path, and the re-send tick
    // LOOKED IT UP -- so a member that needed OpenNetworkConnection was marked
    // UNSENT, never gained an entry, and the tick's lookup missed it forever.
    // The connection it had just opened was never used, the member never left
    // UNSENT, and the round burned the full wall for nothing (0 partials,
    // 0 terminal, 0 retryable, 0 unreachable -- observed on the fleet).
    // ★ The fix is not to also-write the table on the other path. It is to
    // DELETE THE DEPENDENCY: every send re-resolves address -> peer at send
    // time, so no path reads state that only another path writes.
    std::map<std::string, CService>                      m_member_addr;
    std::map<std::string, int64_t>                       m_first_attempt_ms;
    std::map<std::string, uint256>                       m_member_protx;
    std::map<std::string, PTXMemberSignState>            m_state;
    std::map<std::string, std::vector<unsigned char>>    m_partials;
    size_t m_protx_mismatch{0};
    std::condition_variable_any m_cv;

    size_t CountIn(PTXMemberSignState s) const EXCLUSIVE_LOCKS_REQUIRED(m_cs)
    {
        size_t n = 0;
        for (const auto& kv : m_state) if (kv.second == s) ++n;
        return n;
    }
};

namespace {
RecursiveMutex cs_ptx_sign_rounds;
// Keyed by (round_seed, quorum_hash) — the correlation key, already unique per
// round, so no new nonce and no new replay surface (§9.5).
std::map<std::pair<uint256, uint256>, std::shared_ptr<PTXSignRound>> g_rounds
    GUARDED_BY(cs_ptx_sign_rounds);
} // anonymous namespace

bool PTX_SignClient_OnResponse(int64_t from_node, const PTXSignResp& resp)
{
    std::shared_ptr<PTXSignRound> round;
    {
        LOCK(cs_ptx_sign_rounds);
        auto it = g_rounds.find(std::make_pair(resp.round_seed, resp.quorum_hash));
        if (it == g_rounds.end()) return false;   // stale or not ours; not misbehaviour
        round = it->second;
    }

    LOCK(round->m_cs);

    // ★ AUTHORITATIVE BINDING FIRST: who did we SEND to on this connection?
    const PTXSignRoundMember who = round->ResolvePeer(from_node);
    if (!who.IsResolved()) {
        // A response from a peer we never asked. Nothing to do with it — we have
        // no index to place it at, and inventing one is the exact mistake this
        // design exists to prevent.
        LogPrint(BCLog::NET, "PTX signresp: unsolicited from peer=%d\n", (int)from_node);
        return false;
    }

    // ★ CROSS-CHECK, NOT A TRUST DECISION. The responder tells us who it thinks
    // it is; we already know who we asked. A mismatch means a misrouted reply or
    // a peer claiming someone else's seat — reject either way, and count it, so
    // the condition is visible rather than silently absorbed.
    // ★ What this does NOT catch: a validly-labelled BAD partial. See the header
    // — per-member public shares are not persisted, so the partial cannot be
    // verified individually here.
    if (!resp.signer_protx.IsNull()) {
        auto pit = round->m_member_protx.find(who.NodeId());
        if (pit != round->m_member_protx.end() && pit->second != resp.signer_protx) {
            round->m_protx_mismatch++;
            round->m_state[who.NodeId()] = PTXMemberSignState::TERMINAL;
            LogPrintf("PTX signresp: REJECTED peer=%d claims proTx %s but we asked %s (%s)\n",
                      (int)from_node, resp.signer_protx.ToString(),
                      pit->second.ToString(), who.NodeId());
            round->m_cv.notify_all();
            return false;
        }
    }

    switch ((PTXSignStatus)resp.status) {
        case PTXSignStatus::OK:
            if (resp.sig.size() != 96) {
                // A malformed "success" is a terminal answer, not a retry.
                round->m_state[who.NodeId()] = PTXMemberSignState::TERMINAL;
                LogPrintf("PTX signresp: %s returned OK with a %u-byte signature\n",
                          who.NodeId(), (unsigned)resp.sig.size());
                break;
            }
            round->RecordPartial(who, resp.sig);
            break;

        case PTXSignStatus::RETRYABLE:
            // The member's own verdict that its refusal is about propagation.
            // Carried through unchanged from the gate — this is the one case
            // where waiting is correct, and treating it as failure would charge
            // ordinary network delay to the caller as a forfeited fee.
            round->m_state[who.NodeId()] = PTXMemberSignState::RETRYABLE;
            break;

        case PTXSignStatus::TERMINAL:
        default:
            // ★ ABSORBING. Identical bytes cannot change this answer, and the
            // request is fully determined by the round, so there is no other
            // framing to try. Stop asking this member.
            round->m_state[who.NodeId()] = PTXMemberSignState::TERMINAL;
            // ★ ODC-064 flood bound. The responder caps `reason` when it
            // PRODUCES one, but a hostile peer sends whatever it likes, so the
            // cap has to be applied again where the bytes are CONSUMED. A
            // producer-side limit is not a limit on anything received.
            LogPrint(BCLog::NET, "PTX signresp: %s TERMINAL (%s)\n",
                     who.NodeId(), resp.reason.substr(0, PTX_SIGNRESP_MAX_REASON));
            break;
    }

    round->m_cv.notify_all();
    return true;
}

namespace {

// ★★ THE P2P CALLER DOES NOT NEED `PTX_ResolveMemberAddr`, AND §9.6 IS WRONG
// ABOUT THIS. The plan says `ptx_fanout.cpp` goes entirely "EXCEPT
// PTX_ResolveMemberAddr, which MOVES (DGM host resolution is the permanent half
// of fix A)". Tracing it for this call site shows otherwise: that function's
// whole job is to graft an RPC PORT onto a DGM HOST, because the PTX-RPC
// endpoint is not on chain. Its own comment calls the port half "A FLEET
// EXPEDIENT, NOT THE MAINNET DELIVERY".
//
// Over P2P there is no port to graft. The DGM address IS the address -- host
// and port together, exactly as advertised on chain -- so the resolver is two
// lines with no convention in it. ★ The permanent half of fix A was never the
// FUNCTION; it was the IDEA of reading the address from the live DGM list every
// round, and that idea survives here in its simplest possible form.
// **So nothing is salvaged from ptx_fanout.cpp: the whole file dies in
// component 4, along with PTX_FanoutRpcPort and -ptxfanoutport.**
//
// ★ AND THERE IS DELIBERATELY NO STATIC `-ptxnode` FALLBACK. The fan-out kept
// one for the trusted-dealer path; reinstating it here would re-introduce a
// config-derived address book -- the exact stale-snapshot class fix A removed
// (~39% of a grown quorum unaddressable). A member absent from the live DGM
// list is not a registered gamemaster, so it holds no share worth asking for.
bool PTX_ResolveMemberP2PAddr(const uint256& proTxHash, CService& addr_out)
{
    if (proTxHash.IsNull() || !deterministicGMManager) return false;
    const auto dgm = deterministicGMManager->GetListAtChainTip().GetValidGM(proTxHash);
    if (!dgm || !dgm->pdgmState) return false;
    if (!dgm->pdgmState->addr.IsValid()) return false;
    addr_out = dgm->pdgmState->addr;
    return true;
}

// ★★ THE ONE SEND PATH. Called by the initial pass AND by every re-send tick,
// with no difference between them. Resolves address -> connected peer EVERY
// time rather than consulting a table some other path was supposed to have
// filled in. Returns the NodeId sent to, or -1 if not connected yet (in which
// case a connection is opened and the next tick tries again).
int64_t TrySendSignReq(CConnman& connman, const CService& addr,
                       const PTXSignReq& req, bool& too_old_out)
{
    int64_t used = -1;
    too_old_out = false;
    connman.ForNode(addr,
        [](const CNode*) { return true; },
        [&](CNode* pnode) {
            // ★★ CAPABILITY CHECK (§9.13(h)). An older node has no handler for
            // ptxsignreq and IGNORES it silently, so sending would buy nothing
            // and cost a member slot until its budget expired. Ask the version
            // instead -- the peer already told us during the handshake.
            if (pnode->nVersion > 0 && pnode->nVersion < PTX_SIGNREQ_MIN_PROTO_VERSION) {
                too_old_out = true;
                return true;
            }
            CNetMsgMaker msgMaker(pnode->GetSendVersion());
            connman.PushMessage(pnode, msgMaker.Make(NetMsgType::PTXSIGNREQ, req));
            used = pnode->GetId();
            return true;
        });
    if (used < 0 && !too_old_out) {
        // Not connected. Open one; the next tick re-resolves and finds it.
        // ★ Nothing is recorded here that a later read depends on -- that
        // asymmetry was the defect.
        CAddress caddr(addr, NODE_NETWORK);
        connman.OpenNetworkConnection(caddr, false, nullptr, nullptr,
                                      false, false, false, /*gamemaster_connection=*/true);
    }
    return used;
}

} // anonymous namespace

PTXSignRoundResult PTX_SignRound_Run(const uint256& round_seed,
                                     const uint256& quorum_hash,
                                     const std::vector<std::string>& member_ids,
                                     const std::map<std::string, uint256>& member_protx,
                                     const std::vector<unsigned char>& commit_raw,
                                     size_t threshold,
                                     CConnman& connman)
{
    PTXSignRoundResult out;

    PTXSignReq req;
    req.round_seed  = round_seed;
    req.quorum_hash = quorum_hash;
    req.commit_raw  = commit_raw;   // MANDATORY on this arm (§9.2) — never empty

    auto round = std::make_shared<PTXSignRound>(round_seed, quorum_hash, threshold);
    round->m_member_protx = member_protx;
    for (const auto& id : member_ids) round->m_state[id] = PTXMemberSignState::UNSENT;

    {
        LOCK(cs_ptx_sign_rounds);
        g_rounds[std::make_pair(round_seed, quorum_hash)] = round;
    }

    // ── Send ────────────────────────────────────────────────────────────────
    // ★ The one genuinely new thing the caller must do. §9.5 said "most members
    // are already connected for block relay", and "most" is load-bearing: block
    // relay gives ~8 outbound peers, not necessarily these 11. A member we have
    // no connection to has to be connected to, and one we cannot connect to is
    // UNREACHABLE — absorbing, like TERMINAL, because there is nothing to retry
    // against.
    for (const auto& node_id : member_ids) {
        // Test hook, carried over from PTX_FanOutSign verbatim in meaning: a
        // 'withhold' or 'abstain' fail-mode makes the caller never collect this
        // member's partial. Never set in production. UNREACHABLE rather than a
        // new state, so the winnability arithmetic treats it as absorbing --
        // which is what driving a round below threshold on purpose requires.
        {
            LOCK(cs_ptx_failmodes);
            auto fit = g_ptx_node_failmodes.find(node_id);
            if (fit != g_ptx_node_failmodes.end() &&
                (fit->second == "withhold" || fit->second == "abstain")) {
                LOCK(round->m_cs);
                round->m_state[node_id] = PTXMemberSignState::UNREACHABLE;
                LogPrintf("PTX signreq: %s %s (failmode) — not collecting\n",
                          node_id, fit->second);
                continue;
            }
        }
        auto pit = member_protx.find(node_id);
        const uint256 mprotx = (pit != member_protx.end()) ? pit->second : uint256();
        CService addr;
        if (!PTX_ResolveMemberP2PAddr(mprotx, addr)) {
            LOCK(round->m_cs);
            round->m_state[node_id] = PTXMemberSignState::UNREACHABLE;
            LogPrintf("PTX signreq: %s not in the live DGM list — dropping from round\n", node_id);
            continue;
        }

        {
            LOCK(round->m_cs);
            round->m_member_addr[node_id]      = addr;
            round->m_first_attempt_ms[node_id] = GetTimeMillis();
        }
        bool too_old = false;
        const int64_t peer = TrySendSignReq(connman, addr, req, too_old);
        LOCK(round->m_cs);
        if (too_old) {
            round->m_state[node_id] = PTXMemberSignState::TOO_OLD;
            LogPrintf("PTX signreq: %s speaks protocol < %d -- cannot serve P2P signing; "
                      "OPERATOR ACTION: upgrade that gamemaster's binary (this is NOT a "
                      "network-reachability problem)\n", node_id, PTX_SIGNREQ_MIN_PROTO_VERSION);
        } else if (peer < 0) {
            // Connection opening. UNSENT is a WAITING state now, not a dead end:
            // the tick re-resolves and it has a bounded exit (below).
            round->m_state[node_id] = PTXMemberSignState::UNSENT;
        } else {
            round->m_peer_to_member[peer] = node_id;
            round->m_state[node_id]       = PTXMemberSignState::INFLIGHT;
        }
    }

    // ── Wait ────────────────────────────────────────────────────────────────
    // No event loop, no dial contexts, no connection lifetimes: a condition
    // variable and a re-send tick. This is the whole of what replaced
    // ptx_fanout.cpp's libevent machinery.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(PTX_SIGNREQ_WALL_MS);
    PTXSignRoundOutcome outcome = PTXSignRoundOutcome::DEADLINE;

    while (true) {
        LOCK(round->m_cs);
        const size_t collected  = round->CountIn(PTXMemberSignState::COLLECTED);
        const size_t inflight   = round->CountIn(PTXMemberSignState::INFLIGHT);
        const size_t retryable  = round->CountIn(PTXMemberSignState::RETRYABLE);
        const size_t unsent     = round->CountIn(PTXMemberSignState::UNSENT);

        if (collected >= threshold) { outcome = PTXSignRoundOutcome::THRESHOLD_MET; break; }

        // ★ Fail at the moment failure becomes CERTAIN, not at the wall.
        if (!PTX_SignRound_StillWinnable(collected, inflight, retryable, unsent, threshold)) {
            outcome = PTXSignRoundOutcome::UNWINNABLE;
            LogPrintf("PTX sign round: UNWINNABLE — %zu collected, %zu terminal, %zu unreachable, "
                      "threshold %zu (stopping now rather than at the %d ms wall)\n",
                      collected, round->CountIn(PTXMemberSignState::TERMINAL),
                      round->CountIn(PTXMemberSignState::UNREACHABLE),
                      threshold, PTX_SIGNREQ_WALL_MS);
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;

        round->m_cv.wait_for(round->m_cs, std::chrono::milliseconds(PTX_SIGNREQ_TICK_MS));

        // Re-send RETRYABLE and UNSENT members through the SAME send path the
        // initial pass used. TERMINAL and UNREACHABLE are never revisited --
        // that is what "absorbing" means.
        for (auto& kv : round->m_state) {
            if (kv.second != PTXMemberSignState::RETRYABLE &&
                kv.second != PTXMemberSignState::UNSENT) continue;
            auto ait = round->m_member_addr.find(kv.first);
            if (ait == round->m_member_addr.end()) {
                // No address at all -- cannot ever be sent to. Absorbing.
                kv.second = PTXMemberSignState::UNREACHABLE;
                continue;
            }
            // ★★ THE BOUNDED EXIT. Without this the fix above only helps the
            // member that WOULD have connected; a member that never connects
            // would still sit UNSENT, still count toward max_reachable, and
            // still hold a doomed round open to the full wall -- the winnability
            // inversion, unfixed. Past the window it becomes UNREACHABLE, which
            // is absorbing, so the round can go UNWINNABLE promptly instead.
            const int64_t first = round->m_first_attempt_ms.count(kv.first)
                                      ? round->m_first_attempt_ms[kv.first] : GetTimeMillis();
            const int64_t elapsed = GetTimeMillis() - first;
            // Tighter, connect-specific bound: a member we could not even reach
            // retires sooner than one that is answering but unhelpfully.
            if (kv.second == PTXMemberSignState::UNSENT &&
                PTX_SignReq_ConnectWindowExpired(elapsed)) {
                kv.second = PTXMemberSignState::UNREACHABLE;
                LogPrintf("PTX signreq: %s never connected within %dms -- UNREACHABLE\n",
                          kv.first, PTX_SIGNREQ_CONNECT_MS);
                continue;
            }
            // ★★ THE GENERAL BUDGET -- covers INFLIGHT (accepted the message,
            // went silent: an old binary ignoring an unknown command) and
            // RETRYABLE (answering "commitment not seen" forever). Both used to
            // hold the round to the wall AND keep winnability from ever firing.
            const PTXMemberSignState retired = PTX_SignReq_RetireExpired(kv.second, elapsed);
            if (retired != kv.second) {
                LogPrintf("PTX signreq: %s no partial within %dms (was %s) -- UNREACHABLE\n",
                          kv.first, PTX_SIGNREQ_MEMBER_MS,
                          kv.second == PTXMemberSignState::INFLIGHT ? "INFLIGHT" : "RETRYABLE");
                kv.second = retired;
                continue;
            }
            bool too_old = false;
            const int64_t peer = TrySendSignReq(connman, ait->second, req, too_old);
            if (too_old) {
                kv.second = PTXMemberSignState::TOO_OLD;
                LogPrintf("PTX signreq: %s speaks protocol < %d -- cannot serve P2P signing; "
                          "OPERATOR ACTION: upgrade that gamemaster's binary\n",
                          kv.first, PTX_SIGNREQ_MIN_PROTO_VERSION);
                continue;
            }
            if (peer >= 0) {
                round->m_peer_to_member[peer] = kv.first;
                kv.second = PTXMemberSignState::INFLIGHT;
            }
        }
    }

    {
        LOCK(round->m_cs);
        out.outcome     = outcome;
        out.partials    = round->m_partials;
        out.terminal    = round->CountIn(PTXMemberSignState::TERMINAL);
        out.retryable   = round->CountIn(PTXMemberSignState::RETRYABLE);
        out.unreachable = round->CountIn(PTXMemberSignState::UNREACHABLE);
        out.too_old     = round->CountIn(PTXMemberSignState::TOO_OLD);
        out.protx_mismatch = round->m_protx_mismatch;
        if (out.partials.empty() && out.unreachable == member_ids.size())
            out.outcome = PTXSignRoundOutcome::NO_QUORUM_CONTACT;
    }
    {
        LOCK(cs_ptx_sign_rounds);
        g_rounds.erase(std::make_pair(round_seed, quorum_hash));
    }

    // ★ The failure SHAPE, logged, because "the roll failed" and "six members
    // refused finally" are different operator problems and only one of them is
    // worth retrying the whole round for.
    LogPrintf("PTX sign round: %s — %zu partial(s), %zu terminal, %zu retryable, "
              "%zu unreachable, %zu TOO-OLD, %zu protx-mismatch (threshold %zu)\n",
              PTXSignRoundOutcomeString(out.outcome), out.partials.size(),
              out.terminal, out.retryable, out.unreachable, out.too_old,
              out.protx_mismatch, threshold);
    return out;
}
