// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PTX_DKG_NET_H
#define PTX_DKG_NET_H

// W2.0b: PTX-DKG ceremony transport (IMP-D2 — P2P gossip, LLMQ dispatch as
// the template; W2.0b_PREIMPL_APPROVED.md is the plan of record).
//
// C1 scope: the enqueue surface + the Option-2 concurrency seam.  The P2P
// message thread ONLY enqueues raw streams (the quorums_dkgsessionhandler.cpp
// :131-142 posture — no deserialization on the message thread); the drain
// pipeline (deserialize → resolve → PreVerify → sig verify → PTX_DKG_Receive*
// → relay) is C2/C3.  Messages sitting in capped queues with no drain is the
// designed C1 state: bounded (per-node cap + dedup), and at W2.0b runtime
// every message would drop at resolve() anyway — no production session exists.
//
// THE OPTION-2 SEAM (structural commitments, W2.0b_PREIMPL_APPROVED §1):
//   - Route by quorum_hash from the start (every ceremony message carries it).
//   - ALL session access through resolve(quorum_hash) → session* | nullptr
//     (nullptr ⇒ DROP, the LLMQ unknown-quorumHash no-ban semantics,
//     quorums_dkgsession.cpp:219-222).  W2.0b: one active-session slot.
//     W2.5: a quorum_hash→session map behind the same interface (register-
//     marked W2.5-BOUND: provisioned, not postponed).
//   - No global session pointer escapes this module.
//
// SESSION CREATION IS NOT TRANSPORT'S (approval Resolution 1): transport
// resolves-or-drops.  SetActiveSession's PRODUCER is W2.2 formation —
// register-marked producer-pending, ZERO production callers at W2.0b (the
// W2.1 MarkForming convention).  Unit tests construct sessions directly.

#include "protocol.h" // CInv
#include "ptx/ptx_dkg.h"
#include "streams.h"
#include "sync.h"
#include "uint256.h"

#include <atomic>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>

typedef int NodeId; // matches net.h:94; avoids pulling net.h into ceremony code

// Per-node enqueue cap: quorum size * 2 — the LLMQ pattern
// (params.size * 2, quorums_dkgsessionhandler.cpp:93-96).
static const size_t PTX_DKG_TRANSPORT_MAX_PER_NODE = 22;

// Drain batch size (C2 consumer) — the LLMQ pattern
// (ProcessPendingMessageBatch callers, quorums_dkgsessionhandler.cpp:547-574).
static const size_t PTX_DKG_TRANSPORT_BATCH = 8;

// Per-member per-phase ACCEPTED-message cap — the LLMQ per-member
// contribution cap (quorums_dkgsession.cpp:248-254): a member's messages
// past this count are dropped BEFORE the expensive sig verify (no ban —
// matching the template).  Bounds store growth and CPU under a
// valid-key-flood from a compromised member.
static const size_t PTX_DKG_TRANSPORT_MAX_PER_MEMBER = 2;

// ---------------------------------------------------------------------------
// CPTXPendingMessages — one instance per ceremony phase message type.
// Copied from CDKGPendingMessages (quorums_dkgsessionhandler.{h,cpp}):
// per-node count cap, raw-bytes-hash dedup, FIFO pop.  Deviations:
//   - PushPendingMessage returns bool (queued / dropped) so unit rows can
//     gate on the outcome; LLMQ's returns void.  Drop is SILENT (no peer
//     penalty) — flooding past the cap and duplicates are not Misbehaving
//     in the template either (quorums_dkgsessionhandler.cpp:34-53).
//   - g_connman->RemoveAskFor(hash, invType) is DEFERRED TO C3: it is
//     inv-fetch bookkeeping and there are no PTX invs until relay lands.
//     invType is carried now so C3 wires it without a signature change.
// ---------------------------------------------------------------------------
class CPTXPendingMessages
{
public:
    typedef std::pair<NodeId, std::shared_ptr<CDataStream>> BinaryMessage;

private:
    mutable RecursiveMutex cs;
    size_t maxMessagesPerNode;
    std::list<BinaryMessage> pendingMessages;
    std::map<NodeId, size_t> messagesPerNode;
    std::set<uint256> seenMessages;

public:
    explicit CPTXPendingMessages(size_t _maxMessagesPerNode) :
        maxMessagesPerNode(_maxMessagesPerNode) {}

    bool PushPendingMessage(NodeId from, CDataStream& vRecv, int invType);
    std::list<BinaryMessage> PopPendingMessages(size_t maxCount);
    bool HasSeen(const uint256& hash) const;
    size_t Size() const;
    void Clear();
};

// ---------------------------------------------------------------------------
// CPTXCeremonyTransport — the transport singleton.
// C1: command→queue routing + the resolve() seam.  C2 adds the drain thread
// and the validate pipeline; C3 adds relay/getdata.
// ---------------------------------------------------------------------------
class CPTXCeremonyTransport
{
private:
    mutable RecursiveMutex cs_session;
    // W2.0b: the single-session slot behind resolve().  W2.2 SG-1c-i: the
    // slot holds a shared_ptr (the LLMQ curSession idiom,
    // quorums_dkgsessionhandler.cpp:165) — the ceremony THREAD owns the
    // session; resolve() hands out a ref-holding copy so an in-flight
    // dispatch keeps the session alive across a producer-side teardown
    // (the SG-1c plan-gate use-after-free catch).  Session lifetime =
    // thread scope + a bounded one-message dispatch tail.
    std::shared_ptr<PTXDKGSession> activeSession;

    CPTXPendingMessages pendingHashCommits{PTX_DKG_TRANSPORT_MAX_PER_NODE};
    CPTXPendingMessages pendingContributions{PTX_DKG_TRANSPORT_MAX_PER_NODE};
    CPTXPendingMessages pendingComplaints{PTX_DKG_TRANSPORT_MAX_PER_NODE};
    CPTXPendingMessages pendingJustifications{PTX_DKG_TRANSPORT_MAX_PER_NODE};
    CPTXPendingMessages pendingPremits{PTX_DKG_TRANSPORT_MAX_PER_NODE};

    // Accepted (envelope-valid) messages, per phase, keyed by RAW-received-
    // bytes hash — the relay/getdata identity.  Byte-stability (C0) makes
    // the re-serialized getdata reply byte-identical to what was received.
    mutable RecursiveMutex cs_store;
    std::map<uint256, PTXDKGPhase0Msg> storeP0;
    std::map<uint256, PTXDKGPhase1Msg> storeP1;
    std::map<uint256, PTXDKGPhase2Msg> storeP2;
    std::map<uint256, PTXDKGPhase3Msg> storeP3;
    std::map<uint256, PTXDKGPhase4Msg> storeP4;
    std::map<uint256, size_t> acceptedPerMember[5]; // sender proTxHash → count

    // Drain thread (daemon path; unit rows call ProcessBatch directly).
    std::thread drainThread;
    std::atomic<bool> stopDrain{false};
    void DrainLoop();

    template <typename Msg>
    void ProcessBatchT(int phase, int invType, std::map<uint256, Msg>& store, size_t maxCount);

public:
    // THE SEAM.  Returns a REF-HOLDING copy of the active session iff
    // quorum_hash matches, else nullptr (caller drops, no ban).  The copy
    // keeps the session alive through the caller's dispatch even if the
    // owning ceremony thread exits meanwhile (teardown-memory safety,
    // SG-1c-i).  W2.5 swaps the slot for a map behind this same signature;
    // no caller changes.
    std::shared_ptr<PTXDKGSession> resolve(const uint256& quorum_hash);

    // PRODUCER: W2.2 formation's ceremony thread (SG-1c-i; Resolution 1
    // unchanged — the transport routes, the producer owns the ceremony).
    // nullptr clears.  Swapping/clearing the session clears queues + stores
    // (single-slot hygiene; the W2.5 map revisits this).
    void SetActiveSession(std::shared_ptr<PTXDKGSession> session);

    // Injectable side-effect hooks.  Daemon defaults are installed by
    // InitPTXCeremonyTransport: penalty = Misbehaving(score) under cs_main
    // (the tiertwo_networksync.cpp:66-74 contract), relay = inv push to
    // GMAUTH-verified peers that are members of the RESOLVED session
    // (verifiedProRegTxHash ∈ session.members — quorums_dkgsession.cpp
    // :1314-1322, born-restricted).  Unset hooks are no-ops.  Unit rows
    // install recorders to OBSERVE relayed-vs-dropped / punished-vs-not
    // per reject arm.
    std::function<void(NodeId, int)> penaltyHook;
    std::function<void(const CInv&, const PTXDKGSession&)> relayHook;

    // The drain pipeline for one phase queue.  ORDER (cheap → expensive,
    // the LLMQ ordering, quorums_dkgsessionhandler.cpp:430-499 — enqueue
    // already applied per-node cap + seen-hash dedup BEFORE this):
    //   deserialize (R1 malformed → ban) → resolve (R2 unknown-quorum →
    //   silent drop) → membership of the RESOLVED session (R3 → ban) →
    //   per-member accept cap (drop, no ban) → operator-key sig verify
    //   (R4 → ban; the EXPENSIVE check, deliberately LAST) → store +
    //   relay (envelope-valid relays REGARDLESS of the deeper semantic
    //   outcome — quorums_dkgsession.cpp:270-272) → PTX_DKG_Receive*.
    void ProcessBatch(int phase, size_t maxCount = PTX_DKG_TRANSPORT_BATCH);

    // ★ W2.5b DELIVERY FIX (ODC-055) — THE CATCH-UP HALF of the born-restricted
    // relay.  The store-time relayHook is ONE-SHOT and fires the instant a
    // message is stored — but a ceremony's member-connections OPEN at ceremony
    // start and GMAUTH-verify seconds LATER, so at realistic mesh density the
    // one-shot relay finds zero verified member peers and every ceremony
    // starves (qual=0 fleet abort, first caught live at N98; masked at N22 by
    // the dense-mesh accident — members were already-verified peers).  This
    // method is the pure core: if proTxHash is a member of the ACTIVE session,
    // collect EVERY stored inv (all five phases — the stores are phase-split
    // but the catch-up is family-wide by construction).  Returns false when
    // no live session or not a member.  The CNode-facing wrapper
    // (PTX_Ceremony_CatchupRelayOnVerify) is called from the GMAUTH verify
    // point — the same on-verify-push shape as the QSENDRECSIGS precedent.
    // ★ ADDITIVE: the store-time relay stays (correct when peers are already
    // verified); this covers the not-yet-verified-at-store case.
    bool CatchupInvsForMember(const uint256& proTxHash, std::vector<CInv>& invs_out);

    // Relay/getdata surface (net_processing AlreadyHave / ProcessGetData).
    bool AlreadyHaveMsg(const CInv& inv);
    bool GetStoredPhase0(const uint256& hash, PTXDKGPhase0Msg& ret);
    bool GetStoredPhase1(const uint256& hash, PTXDKGPhase1Msg& ret);
    bool GetStoredPhase2(const uint256& hash, PTXDKGPhase2Msg& ret);
    bool GetStoredPhase3(const uint256& hash, PTXDKGPhase3Msg& ret);
    bool GetStoredPhase4(const uint256& hash, PTXDKGPhase4Msg& ret);

    void StartDrain();
    void StopDrain();
    void ClearAll(); // queues + stores + member counts

    // Enqueue-only entry from the tier-two dispatcher (P2P message thread).
    // Returns false ONLY on an unroutable command (dispatcher misroute →
    // Misbehaving(100) at the caller, the tiertwo_networksync.cpp:66-74
    // contract).  Queue drops (dup/cap) return true — not peer misbehavior
    // in the template.
    bool ProcessMessage(NodeId from, const std::string& strCommand, CDataStream& vRecv);

    // Queue access for the C2 drain (and unit rows).  phase: 0..4.
    CPTXPendingMessages& PendingForPhase(int phase);
};

// The transport singleton (plain global, the g_ptx_bls_state PTX convention).
// No threads at C1, so static construction is safe.
extern CPTXCeremonyTransport g_ptx_ceremony_transport;

// Lifecycle hooks, called from tiertwo/init.cpp (the InitLLMQSystem /
// ptxQuorumStore pattern).  C1: log-only — and load-bearing for LINKING:
// tiertwo_networksync.cpp (libbitcoin_common) references the singleton, and
// SERVER precedes COMMON on the daemon link line, so a server-side referencer
// (tiertwo/init.o) must pull ptx_dkg_net.o from the archive — exactly how
// the LLMQ managers link.  C2 turns these into drain-thread start/stop.
void InitPTXCeremonyTransport();
void StopPTXCeremonyTransport();

// ★ W2.5b DELIVERY FIX (ODC-055) — the GMAUTH-verify hook's entry: if the
// now-verified peer is a member of the live ceremony session, push it every
// stored session inv (CatchupInvsForMember + PushInventory).  Called from
// CGMAuth::ProcessMessage immediately after verifiedProRegTxHash is set.
// No-op for non-members, no live session, or a null verified hash.
class CNode;
void PTX_Ceremony_CatchupRelayOnVerify(CNode* pnode);

#endif // PTX_DKG_NET_H
