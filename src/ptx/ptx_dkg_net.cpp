// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_dkg_net.h"

#include "hash.h"
#include "logging.h"
#include "net.h"            // g_connman, CNode::PushInventory (default relay hook)
#include "net_processing.h" // Misbehaving (default penalty hook)
#include "protocol.h"
#include "util/threadnames.h"
#include "validation.h"     // cs_main

#include <chrono>

CPTXCeremonyTransport g_ptx_ceremony_transport;

void InitPTXCeremonyTransport()
{
    // Daemon default hooks.  Unit rows construct their own transport with
    // recorder hooks instead — these defaults are the LLMQ mechanisms.
    g_ptx_ceremony_transport.penaltyHook = [](NodeId id, int score) {
        WITH_LOCK(cs_main, Misbehaving(id, score));
    };
    // Relay BORN-RESTRICTED: inv pushed only to GMAUTH-verified peers whose
    // verifiedProRegTxHash is a member of the RESOLVED session — the
    // RelayInvToParticipants shape (quorums_dkgsession.cpp:1314-1322) keyed
    // off the resolved session (the Option-2 seam commitment), never a
    // global.  On the GMAUTH-inert fleet this relays to zero peers until
    // the operator keys are wired (W2.2 boundary, register-marked).
    g_ptx_ceremony_transport.relayHook = [](const CInv& inv, const PTXDKGSession& session) {
        if (!g_connman) return;
        g_connman->ForEachNode([&](CNode* pnode) {
            if (pnode->verifiedProRegTxHash.IsNull()) return;
            for (const auto& m : session.members) {
                if (m.proTxHash == pnode->verifiedProRegTxHash) {
                    pnode->PushInventory(inv);
                    return;
                }
            }
        });
    };
    g_ptx_ceremony_transport.StartDrain();
    LogPrintf("PTX: ceremony transport initialized (W2.0b: dispatch + validate-before-relay live; sessions produced at W2.2)\n");
}

void StopPTXCeremonyTransport()
{
    g_ptx_ceremony_transport.StopDrain();
    g_ptx_ceremony_transport.SetActiveSession(nullptr); // also clears queues + stores
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

std::shared_ptr<PTXDKGSession> CPTXCeremonyTransport::resolve(const uint256& quorum_hash)
{
    LOCK(cs_session);
    if (activeSession && activeSession->quorum_hash == quorum_hash) {
        return activeSession; // ref-holding copy — survives producer teardown
    }
    return nullptr; // unknown formation → caller drops, no ban
}

void CPTXCeremonyTransport::SetActiveSession(std::shared_ptr<PTXDKGSession> session)
{
    {
        LOCK(cs_session);
        activeSession = std::move(session);
    }
    ClearAll(); // single-slot hygiene: a session swap invalidates queued/stored msgs
}

void CPTXCeremonyTransport::ClearAll()
{
    for (int phase = 0; phase < 5; phase++) {
        PendingForPhase(phase).Clear();
    }
    LOCK(cs_store);
    storeP0.clear();
    storeP1.clear();
    storeP2.clear();
    storeP3.clear();
    storeP4.clear();
    for (auto& m : acceptedPerMember) m.clear();
}

// ---------------------------------------------------------------------------
// The drain pipeline
// ---------------------------------------------------------------------------

namespace {

// The wire "sender" identity per phase — mirrors the receive functions'
// own sender resolution (complaint is signed by the complainant,
// justification by the dealer).
const uint256& SenderOf(const PTXDKGPhase0Msg& m) { return m.proTxHash; }
const uint256& SenderOf(const PTXDKGPhase1Msg& m) { return m.proTxHash; }
const uint256& SenderOf(const PTXDKGPhase2Msg& m) { return m.complainant_proTxHash; }
const uint256& SenderOf(const PTXDKGPhase3Msg& m) { return m.dealer_proTxHash; }
const uint256& SenderOf(const PTXDKGPhase4Msg& m) { return m.proTxHash; }

bool ReceiveDispatch(PTXDKGSession& s, const PTXDKGPhase0Msg& m) { return PTX_DKG_ReceivePhase0Msg(s, m); }
bool ReceiveDispatch(PTXDKGSession& s, const PTXDKGPhase1Msg& m) { return PTX_DKG_ReceivePhase1Msg(s, m); }
bool ReceiveDispatch(PTXDKGSession& s, const PTXDKGPhase2Msg& m) { return PTX_DKG_ReceivePhase2Msg(s, m); }
bool ReceiveDispatch(PTXDKGSession& s, const PTXDKGPhase3Msg& m) { return PTX_DKG_ReceivePhase3Msg(s, m); }
bool ReceiveDispatch(PTXDKGSession& s, const PTXDKGPhase4Msg& m) { return PTX_DKG_ReceivePhase4Msg(s, m); }

const PTXDKGMember* FindMember(const PTXDKGSession& session, const uint256& proTxHash)
{
    for (const auto& m : session.members) {
        if (m.proTxHash == proTxHash) return &m;
    }
    return nullptr;
}

// KDD-072 P-b2: per-type sign-hash for the R4 envelope verify. Phase 0-3
// preimages are session-independent; Phase 4 binds the RESOLVED session's
// predecessor (the receiver's own view) — a premit signed over a different
// rotation view fails R4 exactly as it fails Receive check 7. The overload set
// exists because ProcessBatchT's call is templated over Msg and cannot pass an
// argument for only one type; a new phase msg type without an overload here is
// a COMPILE error, not a silent v1 fallback.
uint256 SignHashOf(const PTXDKGPhase0Msg& m, const PTXDKGSession&)   { return m.GetSignHash(); }
uint256 SignHashOf(const PTXDKGPhase1Msg& m, const PTXDKGSession&)   { return m.GetSignHash(); }
uint256 SignHashOf(const PTXDKGPhase2Msg& m, const PTXDKGSession&)   { return m.GetSignHash(); }
uint256 SignHashOf(const PTXDKGPhase3Msg& m, const PTXDKGSession&)   { return m.GetSignHash(); }
uint256 SignHashOf(const PTXDKGPhase4Msg& m, const PTXDKGSession& s) { return m.GetSignHash(s.predecessor_quorum_hash); }

} // anonymous namespace

template <typename Msg>
void CPTXCeremonyTransport::ProcessBatchT(int phase, int invType, std::map<uint256, Msg>& store, size_t maxCount)
{
    auto batch = PendingForPhase(phase).PopPendingMessages(maxCount);
    for (auto& bm : batch) {
        const NodeId from = bm.first;
        CDataStream& stream = *bm.second;

        // Relay/getdata identity = the RAW received bytes (the enqueue-time
        // dedup hash, quorums_dkgsessionhandler.cpp:42-44).
        CHashWriter hw(SER_GETHASH, 0);
        hw.write(stream.data(), stream.size());
        const uint256 hash = hw.GetHash();

        // R1 — malformed: deserialize failure (incl. the C0 decode gates:
        // bad point / non-canonical scalar / oversize / truncation)
        // → drop + ban(100), never relayed (handler :444-450).
        Msg msg;
        try {
            stream >> msg;
        } catch (const std::exception& e) {
            LogPrintf("PTX: transport reject phase=%d malformed (%s) peer=%d\n", phase, e.what(), from);
            if (penaltyHook) penaltyHook(from, 100);
            continue;
        }

        // R2 — unknown formation: resolve() nullptr → SILENT drop, no ban
        // (a node may legitimately not know a formation — session :219-222),
        // never relayed.
        std::shared_ptr<PTXDKGSession> session = resolve(msg.quorum_hash);
        if (session == nullptr) {
            LogPrintf("PTX: transport drop phase=%d unknown-quorum %s peer=%d\n",
                      phase, msg.quorum_hash.ToString(), from);
            continue;
        }

        // R3 — sender is not a member of the RESOLVED session → drop +
        // ban(100), never relayed (session :224-229).
        const PTXDKGMember* member = FindMember(*session, SenderOf(msg));
        if (member == nullptr) {
            LogPrintf("PTX: transport reject phase=%d non-member %s peer=%d\n",
                      phase, SenderOf(msg).ToString(), from);
            if (penaltyHook) penaltyHook(from, 100);
            continue;
        }

        // Per-member accept cap — CHEAP, before the expensive sig verify
        // (the LLMQ per-member contribution cap, session :248-254): drop,
        // no ban, never relayed.
        {
            LOCK(cs_store);
            auto it = acceptedPerMember[phase].find(member->proTxHash);
            if (it != acceptedPerMember[phase].end() && it->second >= PTX_DKG_TRANSPORT_MAX_PER_MEMBER) {
                LogPrintf("PTX: transport drop phase=%d member-cap %s peer=%d\n",
                          phase, member->proTxHash.ToString(), from);
                continue;
            }
        }

        // R4 — operator-key sig fails → drop + ban(100), never relayed
        // (handler :473-480).  The EXPENSIVE check, deliberately LAST.
        // KDD-072 P-b2: per-type SignHashOf — Phase 4 verifies against the
        // resolved session's predecessor view.
        if (!msg.sig.VerifyInsecure(member->pubKeyOperator, SignHashOf(msg, *session))) {
            LogPrintf("PTX: transport reject phase=%d bad-sig %s peer=%d\n",
                      phase, member->proTxHash.ToString(), from);
            if (penaltyHook) penaltyHook(from, 100);
            continue;
        }

        // Envelope-valid → store (getdata serving) + relay.  Relay happens
        // REGARDLESS of the deeper semantic outcome below — the LLMQ posture
        // ("so the whole quorum sees the bad behavior", session :270-272).
        {
            LOCK(cs_store);
            store.emplace(hash, msg);
            acceptedPerMember[phase][member->proTxHash]++;
        }
        if (relayHook) relayHook(CInv(invType, hash), *session);

        // SG-2a Resolution A: the drain thread is one of two session writers
        // (the other is the ceremony STEP thread, PTX_Ceremony_Step).  Take
        // the session-state lock around the ONLY session mutation here —
        // ReceiveDispatch — and nothing else (invariant: `cs` never nests with
        // the pending-queue lock, already popped above, nor with cs_main).
        bool ok;
        {
            LOCK(*session->cs);
            ok = ReceiveDispatch(*session, msg);
        }
        LogPrintf("PTX: transport %s phase=%d from=%s peer=%d\n",
                  ok ? "accepted" : "semantic-reject", phase,
                  member->proTxHash.ToString(), from);
    }
}

void CPTXCeremonyTransport::ProcessBatch(int phase, size_t maxCount)
{
    switch (phase) {
        case 0: ProcessBatchT(0, MSG_PTX_QUORUM_HASH_COMMIT, storeP0, maxCount); break;
        case 1: ProcessBatchT(1, MSG_PTX_QUORUM_CONTRIB, storeP1, maxCount); break;
        case 2: ProcessBatchT(2, MSG_PTX_QUORUM_COMPLAINT, storeP2, maxCount); break;
        case 3: ProcessBatchT(3, MSG_PTX_QUORUM_JUSTIFICATION, storeP3, maxCount); break;
        case 4: ProcessBatchT(4, MSG_PTX_QUORUM_PREMATURE_COMMITMENT, storeP4, maxCount); break;
        default: assert(false);
    }
}

bool CPTXCeremonyTransport::AlreadyHaveMsg(const CInv& inv)
{
    const int phase = inv.type - MSG_PTX_QUORUM_HASH_COMMIT;
    if (phase < 0 || phase > 4) return false;
    if (PendingForPhase(phase).HasSeen(inv.hash)) return true;
    LOCK(cs_store);
    switch (phase) {
        case 0: return storeP0.count(inv.hash) != 0;
        case 1: return storeP1.count(inv.hash) != 0;
        case 2: return storeP2.count(inv.hash) != 0;
        case 3: return storeP3.count(inv.hash) != 0;
        default: return storeP4.count(inv.hash) != 0;
    }
}

namespace {
template <typename Msg>
bool GetStoredT(RecursiveMutex& cs, const std::map<uint256, Msg>& store, const uint256& hash, Msg& ret)
{
    LOCK(cs);
    auto it = store.find(hash);
    if (it == store.end()) return false;
    ret = it->second;
    return true;
}
} // anonymous namespace

bool CPTXCeremonyTransport::GetStoredPhase0(const uint256& hash, PTXDKGPhase0Msg& ret) { return GetStoredT(cs_store, storeP0, hash, ret); }
bool CPTXCeremonyTransport::GetStoredPhase1(const uint256& hash, PTXDKGPhase1Msg& ret) { return GetStoredT(cs_store, storeP1, hash, ret); }
bool CPTXCeremonyTransport::GetStoredPhase2(const uint256& hash, PTXDKGPhase2Msg& ret) { return GetStoredT(cs_store, storeP2, hash, ret); }
bool CPTXCeremonyTransport::GetStoredPhase3(const uint256& hash, PTXDKGPhase3Msg& ret) { return GetStoredT(cs_store, storeP3, hash, ret); }
bool CPTXCeremonyTransport::GetStoredPhase4(const uint256& hash, PTXDKGPhase4Msg& ret) { return GetStoredT(cs_store, storeP4, hash, ret); }

void CPTXCeremonyTransport::DrainLoop()
{
    util::ThreadRename("ptx-ceremony");
    while (!stopDrain) {
        for (int phase = 0; phase < 5; phase++) {
            ProcessBatch(phase, PTX_DKG_TRANSPORT_BATCH);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void CPTXCeremonyTransport::StartDrain()
{
    stopDrain = false;
    drainThread = std::thread(&CPTXCeremonyTransport::DrainLoop, this);
}

void CPTXCeremonyTransport::StopDrain()
{
    stopDrain = true;
    if (drainThread.joinable()) drainThread.join();
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
