// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_formation.h"

#include "activegamemaster.h"
#include "chain.h"
#include "chainparams.h"
#include "consensus/params.h"
#include "logging.h"
#include "ptx/ptx_dkg_net.h"
#include "threadinterrupt.h"
#include "util/system.h"
#include "validation.h"

#include <functional>
#include <memory>
#include <set>
#include <thread>

CDeterministicGMList PTX_Formation_BuildPool(
        const CDeterministicGMList& listAtAnchor,
        const std::vector<CPTXQuorumRecord>& activeAtAnchor)
{
    // D-SG1a-1: the exclusion set is the FULL formed-11 of every ACTIVE
    // record (QUAL or not), keyed by proTxHash.
    std::set<uint256> active_members;
    for (const auto& rec : activeAtAnchor) {
        for (const auto& m : rec.members) {
            active_members.insert(m.proTxHash);
        }
    }

    // KDD-060 filter-then-score, extended by the KDD-040 exclusion: build
    // the pool BEFORE any scoring so excluded GMs never enter the
    // competition (a post-score drop would change selection and split the
    // chain). The eligibility predicate is the shared PTX_DKG_IsGMPTXEligible
    // — never re-inlined.
    CDeterministicGMList pool;
    listAtAnchor.ForEachGM(true, [&](const CDeterministicGMCPtr& dgm) {
        if (!PTX_DKG_IsGMPTXEligible(dgm))
            return;
        if (active_members.count(dgm->proTxHash))
            return;
        pool.AddGM(dgm);
    });
    return pool;
}

bool PTX_Formation_SelectAtAnchor(const CBlockIndex* pindexAnchor,
                                  std::vector<PTXDKGMember>& membersOut)
{
    membersOut.clear();
    if (pindexAnchor == nullptr)
        return false;

    // SNAPSHOT-NOT-LIVE: both reads are AT THE ANCHOR. GetListForBlock is
    // the same idiom as the three existing selection sites
    // (specialtx_validation.cpp V5, ptx_quorum_store connect-time
    // materialization, the anchored debug populate probe). Never
    // GetListAtChainTip.
    const CDeterministicGMList listAtAnchor =
            deterministicGMManager->GetListForBlock(pindexAnchor);

    // ptxQuorumStore is initialized before validation in production; the
    // null tolerance is unit-test-environment-only and means "no active
    // quorums", which is exactly the zero-ACTIVE state those tests model.
    std::vector<CPTXQuorumRecord> activeAtAnchor;
    if (ptxQuorumStore) {
        activeAtAnchor =
                ptxQuorumStore->GetActiveQuorumsAtHeight(pindexAnchor->nHeight);
    }

    const CDeterministicGMList pool =
            PTX_Formation_BuildPool(listAtAnchor, activeAtAnchor);

    // The pool >= 11 gate: under-threshold is a DETERMINISTIC SKIP (this
    // cycle simply does not form), not an error.
    if (pool.GetValidGMsCount() < 11)
        return false;

    membersOut = PTX_DKG_BuildMemberVectorFromList(
            pool, pindexAnchor->GetBlockHash());
    return membersOut.size() == 11;
}

namespace {

// W2.2 SG-1c-i — the ceremony runner (Model B: the thread OWNS the session).
// ONE slot by design (Step-0 source verification: KDD-040 §4 at-most-one-
// quorum lifecycle-exclusive; KDD-045 §7.2 one ceremony per N per quorum;
// KDD-063 §23 rotation overlap = serving+ceremony, never two ceremonies).
class PTXFormationCeremonyRunner
{
public:
    // Interrupt any running ceremony thread and JOIN it.  LIVENESS CONTRACT
    // (plan-gate decision 2, release-before-join): the caller must NOT hold
    // cs_main or the transport's cs_session — the exit path takes cs_session
    // (SetActiveSession(nullptr)) and the store lock (ClearForming), so a
    // join under either deadlocks (k+1 waits for k; k waits for k+1's lock).
    void AbortAndJoin()
    {
        AssertLockNotHeld(cs_main);
        if (thread.joinable()) {
            LogPrintf("PTX formation: interrupting prior ceremony thread (collision/shutdown)\n");
            interrupt();
            thread.join();
            interrupt.reset();
        }
    }

    // Spawn the ceremony thread owning the session.  Single slot: any prior
    // thread is aborted first (the collision policy — abort IS thread-exit,
    // the same teardown path, not a special case).
    void Start(std::shared_ptr<PTXDKGSession> session, int formation_height)
    {
        AbortAndJoin();
        std::function<void()> body =
            std::bind(&PTXFormationCeremonyRunner::ThreadBody, this,
                      std::move(session), formation_height);
        thread = std::thread(&TraceThread<std::function<void()>>,
                             std::string("ptx-ceremony"), std::move(body));
    }

private:
    void ThreadBody(std::shared_ptr<PTXDKGSession> session, int formation_height)
    {
        const uint256 quorum_hash = session->quorum_hash;
        const int my_idx = session->my_idx;
        g_ptx_ceremony_transport.SetActiveSession(session);
        LogPrintf("PTX formation: ceremony session STARTED (parked) quorum_hash=%s formation_height=%d my_idx=%d (SG-1c-i — SG-2 fills the phase loop)\n",
                  quorum_hash.ToString(), formation_height, my_idx);

        // PARKED (SG-1c-i): self-clocked interruptible wait; the SG-2 phase
        // walk replaces this loop body.  sleep_for returns false when
        // interrupted — abort/shutdown is the only exit today.
        while (interrupt.sleep_for(std::chrono::milliseconds(500))) {
        }

        // Teardown order: unpublish from the transport FIRST (no new
        // resolves; in-flight dispatches keep their ref — the shared_ptr
        // memory contract), then clear the node-local FORMING marker.
        // ClearForming is erase-if-present: an entry already consumed by
        // ConsumeFormingOnConnect stays silent (the "unless-consumed" arm).
        g_ptx_ceremony_transport.SetActiveSession(nullptr);
        if (ptxQuorumStore) ptxQuorumStore->ClearForming(quorum_hash);
        LogPrintf("PTX formation: ceremony session EXITED quorum_hash=%s\n",
                  quorum_hash.ToString());
        // The owning ref drops here; the session dies when the last
        // in-flight dispatch (if any) completes.
    }

    std::thread thread;
    CThreadInterrupt interrupt;
};

PTXFormationCeremonyRunner g_ptx_formation_runner;

} // namespace

void PTX_Formation_StopCeremonyRunner()
{
    g_ptx_formation_runner.AbortAndJoin();
}

bool PTX_Formation_IsBoundary(int nHeight,
                              const Consensus::PTXFormationParams& params)
{
    // nHeight > 0 is load-bearing: 0 % N == 0 would make genesis a boundary.
    return nHeight > 0 && (nHeight % params.nFormationInterval) == 0;
}

const CBlockIndex* PTX_Formation_GetAnchor(
        const CBlockIndex* pindexNew,
        const Consensus::PTXFormationParams& params)
{
    if (pindexNew == nullptr)
        return nullptr;
    // Walk pindexNew's OWN branch (V3's reorg-robust idiom) — never
    // chainActive[] (equivalent only while the tip is on the active chain;
    // divergent, self-poisoning, on a fork), never cached across tips.
    const int stage = pindexNew->nHeight % params.nFormationInterval;
    return pindexNew->GetAncestor(pindexNew->nHeight - stage);
}

void PTX_Formation_NotifyUpdatedBlockTip(const CBlockIndex* pindexNew,
                                         bool fInitialDownload)
{
    if (pindexNew == nullptr)
        return;

    // ACTION guards (the CDKGSessionManager::UpdatedBlockTip pair). Neither
    // is an input to the boundary computation — the pure core below cannot
    // receive them by signature.
    if (fInitialDownload)
        return;
    if (!deterministicGMManager->IsDIP3Enforced(pindexNew->nHeight))
        return;

    const Consensus::PTXFormationParams& params =
            Params().GetConsensus().ptxFormation;
    if (!PTX_Formation_IsBoundary(pindexNew->nHeight, params))
        return;

    // Chain-derived work under cs_main; the ACTION (thread spawn/join)
    // happens after the lock releases (release-before-join, liveness).
    std::vector<PTXDKGMember> members;
    uint256 anchorHash;
    int anchorHeight = 0;
    {
        LOCK(cs_main);
        const CBlockIndex* pindexAnchor =
                PTX_Formation_GetAnchor(pindexNew, params);
        if (pindexAnchor == nullptr)
            return;

        // The SG-1b-ii observability line stays — the boundary fires
        // identically on every node regardless of what this node then does.
        LogPrintf("PTX formation boundary: height=%d anchor=%s anchor_height=%d N=%d\n",
                  pindexNew->nHeight, pindexAnchor->GetBlockHash().ToString(),
                  pindexAnchor->nHeight, params.nFormationInterval);

        // (a) AT-START ANCHOR RE-VERIFY (decision 3a — the h400 stake-race
        // do-not-drift input): the anchor must be on the ACTIVE chain at
        // action time; the notified tip can itself be stale by now.  The
        // per-tip re-arm half (3b) is SG-1c-ii.
        if (!chainActive.Contains(pindexAnchor)) {
            LogPrintf("PTX formation: anchor %s no longer on the active chain at action time — not starting (SG-1c-ii re-arms)\n",
                      pindexAnchor->GetBlockHash().ToString());
            return;
        }

        if (ptxQuorumStore == nullptr)
            return; // unit-test environment: observe only, never act

        anchorHash = pindexAnchor->GetBlockHash();
        anchorHeight = pindexAnchor->nHeight;

        // IDEMPOTENCE: one action per formation per node.  FORMING is
        // NODE-LOCAL bookkeeping gating THIS ACTION only — it never enters
        // the boundary decision above or any validation path.
        if (ptxQuorumStore->IsForming(anchorHash)) {
            LogPrintf("PTX formation: already FORMING for %s — idempotent skip\n",
                      anchorHash.ToString());
            return;
        }

        // Selection at the anchor (SG-1a).  pool < 11 => deterministic skip.
        if (!PTX_Formation_SelectAtAnchor(pindexAnchor, members)) {
            LogPrintf("PTX formation: pool below threshold at anchor %s — deterministic skip\n",
                      anchorHash.ToString());
            return;
        }
    }

    // Membership: a non-GM node (null activeGamemasterManager — the caller)
    // is passive-by-identity; a GM not selected gets my_idx == -1.
    const uint256 myProTx = (activeGamemasterManager != nullptr)
            ? activeGamemasterManager->GetProTx() : UINT256_ZERO;
    auto session = std::make_shared<PTXDKGSession>();
    if (!PTX_DKG_InitSession(*session, members, anchorHash, myProTx)) {
        LogPrintf("PTX formation: InitSession failed at anchor %s — not starting\n",
                  anchorHash.ToString());
        return;
    }
    if (session->my_idx < 0) {
        // FULLY PASSIVE: no FORMING, no thread, no transport touch —
        // resolve() on this node keeps returning nullptr (R2 drop).
        LogPrintf("PTX formation: not a member at anchor %s — passive\n",
                  anchorHash.ToString());
        return;
    }

    // MEMBER path.  Collision policy first (abort-old = interrupt + join),
    // OUTSIDE cs_main/cs_session — the release-before-join liveness
    // contract (plan-gate decision 2).
    g_ptx_formation_runner.AbortAndJoin();
    ptxQuorumStore->MarkForming(anchorHash, anchorHeight);
    g_ptx_formation_runner.Start(std::move(session), anchorHeight);
}
