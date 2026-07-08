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

#include "ptx/ptx_dkg.h"
#include "streams.h"
#include "sync.h"
#include "uint256.h"

#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>

typedef int NodeId; // matches net.h:94; avoids pulling net.h into ceremony code

// Per-node enqueue cap: quorum size * 2 — the LLMQ pattern
// (params.size * 2, quorums_dkgsessionhandler.cpp:93-96).
static const size_t PTX_DKG_TRANSPORT_MAX_PER_NODE = 22;

// Drain batch size (C2 consumer) — the LLMQ pattern
// (ProcessPendingMessageBatch callers, quorums_dkgsessionhandler.cpp:547-574).
static const size_t PTX_DKG_TRANSPORT_BATCH = 8;

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
    // W2.0b: the single-session slot behind resolve().  NON-OWNING: the
    // producer (W2.2 formation) owns the session and must clear the slot
    // before destroying it (producer-side contract, register-marked).
    PTXDKGSession* activeSession{nullptr};

    CPTXPendingMessages pendingHashCommits{PTX_DKG_TRANSPORT_MAX_PER_NODE};
    CPTXPendingMessages pendingContributions{PTX_DKG_TRANSPORT_MAX_PER_NODE};
    CPTXPendingMessages pendingComplaints{PTX_DKG_TRANSPORT_MAX_PER_NODE};
    CPTXPendingMessages pendingJustifications{PTX_DKG_TRANSPORT_MAX_PER_NODE};
    CPTXPendingMessages pendingPremits{PTX_DKG_TRANSPORT_MAX_PER_NODE};

public:
    // THE SEAM.  Returns the active session iff quorum_hash matches, else
    // nullptr (caller drops, no ban).  W2.5 swaps the slot for a map behind
    // this same signature; no caller changes.
    PTXDKGSession* resolve(const uint256& quorum_hash);

    // PRODUCER: W2.2 formation (register-marked producer-pending; zero
    // production callers at W2.0b — Resolution 1).  nullptr clears.
    void SetActiveSession(PTXDKGSession* session);

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

#endif // PTX_DKG_NET_H
