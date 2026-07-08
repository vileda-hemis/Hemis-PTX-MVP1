// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_dkg_net.h"

#include "hash.h"
#include "logging.h"
#include "protocol.h"

CPTXCeremonyTransport g_ptx_ceremony_transport;

void InitPTXCeremonyTransport()
{
    // C1: enqueue-only transport; the drain pipeline (C2) starts its thread
    // here.  This call also anchors ptx_dkg_net.o into the daemon link (see
    // header note).
    LogPrintf("PTX: ceremony transport initialized (W2.0b: enqueue-only, no drain yet)\n");
}

void StopPTXCeremonyTransport()
{
    // C2: drain-thread stop lands here.  Clearing queues on shutdown keeps
    // teardown deterministic.
    for (int phase = 0; phase < 5; phase++) {
        g_ptx_ceremony_transport.PendingForPhase(phase).Clear();
    }
    g_ptx_ceremony_transport.SetActiveSession(nullptr);
}

// ---------------------------------------------------------------------------
// CPTXPendingMessages — the CDKGPendingMessages body
// (quorums_dkgsessionhandler.cpp:26-90), RemoveAskFor deferred to C3.
// ---------------------------------------------------------------------------

bool CPTXPendingMessages::PushPendingMessage(NodeId from, CDataStream& vRecv, int invType)
{
    // this will also consume the data, even if we bail out early
    auto pm = std::make_shared<CDataStream>(std::move(vRecv));

    {
        LOCK(cs);
        if (messagesPerNode[from] >= maxMessagesPerNode) {
            LogPrint(BCLog::NET, "CPTXPendingMessages::%s -- too many messages, peer=%d\n", __func__, from);
            return false;
        }
        messagesPerNode[from]++;
    }

    CHashWriter hw(SER_GETHASH, 0);
    hw.write(pm->data(), pm->size());
    uint256 hash = hw.GetHash();

    // C3 wires: g_connman->RemoveAskFor(hash, invType) — inv-fetch
    // bookkeeping; no PTX invs exist until relay lands.
    (void)invType;

    LOCK(cs);
    if (!seenMessages.emplace(hash).second) {
        LogPrint(BCLog::NET, "CPTXPendingMessages::%s -- already seen %s, peer=%d\n", __func__, hash.ToString(), from);
        return false;
    }

    pendingMessages.emplace_back(std::make_pair(from, std::move(pm)));
    return true;
}

std::list<CPTXPendingMessages::BinaryMessage> CPTXPendingMessages::PopPendingMessages(size_t maxCount)
{
    LOCK(cs);
    std::list<BinaryMessage> ret;
    while (!pendingMessages.empty() && ret.size() < maxCount) {
        ret.emplace_back(std::move(pendingMessages.front()));
        pendingMessages.pop_front();
    }
    return ret;
}

bool CPTXPendingMessages::HasSeen(const uint256& hash) const
{
    LOCK(cs);
    return seenMessages.count(hash) != 0;
}

size_t CPTXPendingMessages::Size() const
{
    LOCK(cs);
    return pendingMessages.size();
}

void CPTXPendingMessages::Clear()
{
    LOCK(cs);
    pendingMessages.clear();
    messagesPerNode.clear();
    seenMessages.clear();
}

// ---------------------------------------------------------------------------
// CPTXCeremonyTransport
// ---------------------------------------------------------------------------

PTXDKGSession* CPTXCeremonyTransport::resolve(const uint256& quorum_hash)
{
    LOCK(cs_session);
    if (activeSession && activeSession->quorum_hash == quorum_hash) {
        return activeSession;
    }
    return nullptr; // unknown formation → caller drops, no ban
}

void CPTXCeremonyTransport::SetActiveSession(PTXDKGSession* session)
{
    LOCK(cs_session);
    activeSession = session;
}

bool CPTXCeremonyTransport::ProcessMessage(NodeId from, const std::string& strCommand, CDataStream& vRecv)
{
    if (strCommand == NetMsgType::PTXQHASHCOMMIT) {
        pendingHashCommits.PushPendingMessage(from, vRecv, MSG_PTX_QUORUM_HASH_COMMIT);
    } else if (strCommand == NetMsgType::PTXQCONTRIB) {
        pendingContributions.PushPendingMessage(from, vRecv, MSG_PTX_QUORUM_CONTRIB);
    } else if (strCommand == NetMsgType::PTXQCOMPLAINT) {
        pendingComplaints.PushPendingMessage(from, vRecv, MSG_PTX_QUORUM_COMPLAINT);
    } else if (strCommand == NetMsgType::PTXQJUSTIFICATION) {
        pendingJustifications.PushPendingMessage(from, vRecv, MSG_PTX_QUORUM_JUSTIFICATION);
    } else if (strCommand == NetMsgType::PTXQPCOMMITMENT) {
        pendingPremits.PushPendingMessage(from, vRecv, MSG_PTX_QUORUM_PREMATURE_COMMITMENT);
    } else {
        return false; // unroutable — Misbehaving(100) at the dispatcher
    }
    return true;
}

CPTXPendingMessages& CPTXCeremonyTransport::PendingForPhase(int phase)
{
    switch (phase) {
        case 0: return pendingHashCommits;
        case 1: return pendingContributions;
        case 2: return pendingComplaints;
        case 3: return pendingJustifications;
        case 4: return pendingPremits;
        default: assert(false);
    }
}
