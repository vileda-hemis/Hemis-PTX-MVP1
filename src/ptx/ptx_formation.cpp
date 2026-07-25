// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_formation.h"

#include "activegamemaster.h"
#include "chain.h"
#include "chainparams.h"
#include "consensus/params.h"
#include "logging.h"
#include "net.h" // g_connman (KDD-065 member-connection hooks)
#include "ptx/ptx_ceremony_driver.h"
#include "ptx/ptx_dkg_net.h"
#include "streams.h"
#include "threadinterrupt.h"
#include "tiertwo/net_gamemasters.h" // TierTwoConnMan (KDD-065 member-connection hooks)
#include "util/system.h"
#include "utilstrencodings.h" // HexStr (CP-5 group_pk-in-DONE observability)
#include "validation.h"
#include "version.h"

#include "span.h"

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
        ClearLive(); // idempotent — the exit path already cleared it
    }

    // Spawn the ceremony thread owning the session.  Single slot: any prior
    // thread is aborted first (the collision policy — abort IS thread-exit,
    // the same teardown path, not a special case).
    void Start(std::shared_ptr<PTXDKGSession> session, int formation_height)
    {
        AbortAndJoin();
        {
            LOCK(cs_live);
            liveQuorumHash = session->quorum_hash;
            liveHeight = formation_height;
            fLive = true;
        }
        std::function<void()> body =
            std::bind(&PTXFormationCeremonyRunner::ThreadBody, this,
                      std::move(session), formation_height);
        thread = std::thread(&TraceThread<std::function<void()>>,
                             std::string("ptx-ceremony"), std::move(body));
    }

    // SG-1c-ii — live-session IDENTITY for the wrapper's per-tip re-arm.
    // LEAF LOCK, read-only: the wrapper reads runner identity only, never
    // session state ("no global session pointer escapes" holds).
    bool GetLiveAnchor(uint256& quorum_hash_out, int& height_out) const
    {
        LOCK(cs_live);
        if (!fLive) return false;
        quorum_hash_out = liveQuorumHash;
        height_out = liveHeight;
        return true;
    }

private:
    void ThreadBody(std::shared_ptr<PTXDKGSession> session, int formation_height)
    {
        const uint256 quorum_hash = session->quorum_hash;
        const int my_idx = session->my_idx;
        g_ptx_ceremony_transport.SetActiveSession(session);
        LogPrintf("PTX formation: ceremony session STARTED quorum_hash=%s formation_height=%d my_idx=%d (SG-2a driver)\n",
                  quorum_hash.ToString(), formation_height, my_idx);

        // KDD-065 member-connections: hand the member set to the tiertwo
        // connman — it opens GM connections (one per cycle), the GMAUTH
        // handshake stamps verifiedProRegTxHash, and the transport relayHook
        // gains its targets. Full mesh minus self (11-member scale; LLMQ's
        // relay-subset is a 400-member optimisation). Keyed by quorum_hash so
        // the epilogue removes exactly this session's entry (thrash-safe).
        if (g_connman != nullptr && my_idx >= 0) {
            std::set<uint256> memberProTxs;
            const uint256& myProTx = session->members[my_idx].proTxHash;
            for (const auto& m : session->members) {
                if (m.proTxHash != myProTx)
                    memberProTxs.insert(m.proTxHash);
            }
            g_connman->GetTierTwoConnMan()->setQuorumNodes(
                    Consensus::LLMQ_TYPE_PTX_CEREMONY, quorum_hash, memberProTxs);
            LogPrintf("PTX: member-connections SET quorum_hash=%s n=%d\n",
                      quorum_hash.ToString(), (int)memberProTxs.size());
        }

        // Operator key — signs this node's own phase messages; NEVER stored in
        // the session (key-separation invariant).  A member ceremony thread is
        // only started with a live activeGamemasterManager (my_idx >= 0 path).
        CBLSSecretKey operator_sk;
        if (activeGamemasterManager != nullptr)
            operator_sk = *activeGamemasterManager->OperatorKey();

        PTXCeremonyDriverState  dstate;
        PTXCeremonyDeadlines    deadlines;   // production defaults (tip-height widths)
        std::vector<PTXCeremonyOutbound> outbounds;
        CMutableTransaction     dkgtx;

        // SG-2a — the self-clocked phase walk (replaces the SG-1c-i park).
        // Each tick: read the tip-height clock under cs_main and RELEASE it
        // (invariant 2 — never held with session.cs); step the ceremony by at
        // most one transition; then TRANSMIT outbounds (invariant 1 — sending
        // takes the pending-queue lock, never held under session.cs).
        // interrupt.sleep_for returns false on interrupt (abort/shutdown).
        do {
            int height = -1;
            { LOCK(cs_main); height = chainActive.Height(); }

            PTXStepResult r = PTX_Ceremony_Step(*session, dstate, height, deadlines,
                                                operator_sk, formation_height,
                                                outbounds, dkgtx);

            // Invariant 1: transmit only after the step returned (session.cs
            // released).  Own message is routed through the transport as if
            // received from self — the drain stores it and relays the inv to
            // member peers; the redundant self-dispatch is a dedup no-op (the
            // step already self-delivered it).
            AssertLockNotHeld(cs_main);
            for (const auto& ob : outbounds) {
                CDataStream ss(ob.raw, SER_NETWORK, PROTOCOL_VERSION);
                g_ptx_ceremony_transport.ProcessMessage(PTX_CEREMONY_SELF_NODE_ID,
                                                        ob.command, ss);
            }

            if (r == PTXStepResult::DONE) {
                // CP-5 observability (SG-2b-0): emit the COMPRESSED group_pk so a
                // fleet-grep can compare all members for identity — the detector
                // for SILENT PARTIAL-DIVERGENCE (N nodes on key X, one on X', all
                // reporting DONE).  Pure read of the already-computed
                // session->group_pk (ComputeGroupPk ran at PREMIT); no recompute,
                // no control-flow change.  Single-owner at DONE (drain quiesced by
                // the FINALIZE phase gate), same as the existing teardown reads.
                uint8_t gpk[48];
                blst_p1_affine_compress(gpk, &session->group_pk);
                LogPrintf("PTX formation: ceremony DONE quorum_hash=%s group_pk=%s sk_share stored, nType=11 tx built (formation_height=%d)\n",
                          quorum_hash.ToString(), HexStr(Span<const uint8_t>(gpk, 48)),
                          formation_height);
                break;
            }
            if (r == PTXStepResult::ABORTED) {
                LogPrintf("PTX formation: ceremony ABORTED quorum_hash=%s (sub-threshold close)\n",
                          quorum_hash.ToString());
                break;
            }
        } while (interrupt.sleep_for(std::chrono::milliseconds(1000)));

        // Teardown order: unpublish from the transport FIRST (no new
        // resolves; in-flight dispatches keep their ref — the shared_ptr
        // memory contract), then clear the node-local FORMING marker.
        // ClearForming is erase-if-present: an entry already consumed by
        // ConsumeFormingOnConnect stays silent (the "unless-consumed" arm).
        g_ptx_ceremony_transport.SetActiveSession(nullptr);
        // KDD-065 mirror of the SET above — same single epilogue every exit
        // funnels through (DONE/ABORT/interrupt); erase is idempotent and
        // keyed by this session's quorum_hash, so re-arm thrash removes
        // exactly its own entry.
        if (g_connman != nullptr) {
            g_connman->GetTierTwoConnMan()->removeQuorumNodes(
                    Consensus::LLMQ_TYPE_PTX_CEREMONY, quorum_hash);
            LogPrintf("PTX: member-connections REMOVED quorum_hash=%s\n",
                      quorum_hash.ToString());
        }
        if (ptxQuorumStore) ptxQuorumStore->ClearForming(quorum_hash);
        ClearLive();
        LogPrintf("PTX formation: ceremony session EXITED quorum_hash=%s\n",
                  quorum_hash.ToString());
        // The owning ref drops here; the session dies when the last
        // in-flight dispatch (if any) completes.
    }

    void ClearLive()
    {
        LOCK(cs_live);
        fLive = false;
    }

    std::thread thread;
    CThreadInterrupt interrupt;
    mutable RecursiveMutex cs_live;
    bool fLive GUARDED_BY(cs_live){false};
    uint256 liveQuorumHash GUARDED_BY(cs_live);
    int liveHeight GUARDED_BY(cs_live){0};
};

PTXFormationCeremonyRunner g_ptx_formation_runner;

// The formation ACTION at pindexNew's current-cycle anchor — shared by the
// boundary branch and the SG-1c-ii re-arm restart (NARROW policy: the re-arm
// path only reaches here after a live session was reorg-aborted).  Caller
// must NOT hold cs_main (AbortAndJoin runs inside — release-before-join).
void StartFormationAtAnchor(const CBlockIndex* pindexNew,
                            const Consensus::PTXFormationParams& params,
                            bool fBoundary)
{
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

        if (fBoundary) {
            // The SG-1b-ii observability line stays — the boundary fires
            // identically on every node regardless of what this node then
            // does.  The re-arm path logs its own RE-ARM line instead.
            LogPrintf("PTX formation boundary: height=%d anchor=%s anchor_height=%d N=%d\n",
                      pindexNew->nHeight, pindexAnchor->GetBlockHash().ToString(),
                      pindexAnchor->nHeight, params.nFormationInterval);
        }

        // (a) AT-START ANCHOR RE-VERIFY (decision 3a — the h400 stake-race
        // do-not-drift input): the anchor must be on the ACTIVE chain at
        // action time; the notified tip can itself be stale by now.
        // Behavioural coverage is (b)'s — the per-tip re-arm below is the
        // live safety net for this assert's failure mode (recorded
        // subsumption, 2026-07-15).
        if (!chainActive.Contains(pindexAnchor)) {
            LogPrintf("PTX formation: anchor %s no longer on the active chain at action time — not starting (the per-tip re-arm covers it)\n",
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
    // KDD-072 P-b2 — THE predecessor setter site (present-but-unfed): this
    // fresh-formation path always sets zero. The P-b6 rotation trigger is the
    // only future feeder of a non-zero value, and it must set it HERE, before
    // the runner starts (the field feeds the Phase 4 sign-hash, the payload
    // version, and the KDD-070 PENDING role).
    session->predecessor_quorum_hash.SetNull();
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

    if (!PTX_Formation_IsBoundary(pindexNew->nHeight, params)) {
        // ------------------------------------------------------------------
        // SG-1c-ii — PER-TIP RE-ARM: THE REORG-CLASS HANDLER (NARROW: acts
        // only when a live session exists; no late-join — a recorded SG-2
        // design input).  ZERO cost when no session runs (one leaf-lock
        // read).
        //
        // WHY THIS ARM EXISTS (not defense-in-depth — the sole rescuer for
        // its class): a REORG delivers ONE UpdatedBlockTip at the final tip
        // (one notification per work-improvement pass; the disconnect loop
        // is silent and the connect loop only breaks past the old tip's
        // work — validation.cpp:2230-42, :2282-86, :2377-84).  A boundary
        // inside the reorged range gets NO notification, so the boundary
        // branch above never runs for it — the stale session's ONLY exit is
        // this non-boundary check (the gm11/h400 case).  Forward crossings,
        // by contrast, notify per height and are owned by the boundary
        // branch.
        //
        // The staleness test is ANCHOR-IDENTITY INEQUALITY against the
        // recomputed current-cycle anchor: session anchor != GetAnchor(tip).
        // This subsumes a Contains check (an anchor that left the active
        // chain can never equal the active chain's cycle anchor) AND catches
        // cycle-staleness — a session left anchored to a previous cycle.
        // Pure chain-state decision; only the action is node-local.
        // ------------------------------------------------------------------
        uint256 liveHash;
        int liveHeight = 0;
        if (!g_ptx_formation_runner.GetLiveAnchor(liveHash, liveHeight))
            return;
        uint256 currentAnchorHash;
        {
            LOCK(cs_main);
            const CBlockIndex* pindexAnchor =
                    PTX_Formation_GetAnchor(pindexNew, params);
            if (pindexAnchor == nullptr)
                return;
            currentAnchorHash = pindexAnchor->GetBlockHash();
        }
        if (currentAnchorHash == liveHash)
            return; // anchor still the live cycle's anchor — nothing to do

        LogPrintf("PTX formation: RE-ARM — live session anchor %s (h%d) != current-cycle anchor %s at tip h%d; aborting and restarting\n",
                  liveHash.ToString(), liveHeight,
                  currentAnchorHash.ToString(), pindexNew->nHeight);
        // Abort OUTSIDE all locks (release-before-join, the h800-proven
        // shape), then restart at the recomputed anchor via the shared
        // action path.
        g_ptx_formation_runner.AbortAndJoin();
        StartFormationAtAnchor(pindexNew, params, /*fBoundary=*/false);
        return;
    }

    StartFormationAtAnchor(pindexNew, params, /*fBoundary=*/true);
}
