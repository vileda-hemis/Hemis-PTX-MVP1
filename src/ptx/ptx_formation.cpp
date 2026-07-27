// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_formation.h"

#include "activegamemaster.h"
#include "chain.h"
#include "chainparams.h"
#include "consensus/params.h"
#include "logging.h"
#include "primitives/block.h"        // W2.4 W4-d: CBlock walk (idle derive-at-eval)
#include "primitives/transaction.h"  // W2.4 W4-d: CProbabilisticTxPayload + GetTxPayload
#include "net.h" // g_connman (KDD-065 member-connection hooks)
#include "evo/evodb.h"                // KDD-072 P-b6b: evoDb for the tip sweeps
#include "ptx/ptx_bls.h"                // KDD-072 P-b6b: ExpirePending / DiscardSuperseded
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

PTXRotationDecision PTX_Formation_RotationDueAt(
        const CBlockIndex* pindexAnchor,
        CPTXQuorumStore& store,
        const Consensus::PTXFormationParams& params,
        const std::function<bool(const CBlockIndex*, CBlock&)>& read_block,
        const std::function<bool(const CPTXQuorumRecord&, const CBlockIndex*)>& impossible_at)
{
    // KDD-072 P-b6b — THE TRIGGER POLICY: age test + lowest-hash tie-break.
    //
    // ★ NODE-LOCAL POLICY, NOT CONSENSUS. V12 validates a rotation's
    // CORRECTNESS (predecessor exists, ACTIVE as-of pindexPrev, same-set
    // resolve, predecessor-uniqueness) and NEVER its timing — there is no
    // "was it due?" check anywhere in the validator. A node whose policy
    // differs, or that rotates early, merely produces a VALID rotation early;
    // it cannot split the chain. That is what lets this stay simple.
    //
    // ★ THE TIE-BREAK, and why it exists: one PTXDKG can ever be accepted per
    // ANCHOR (V9 keys uniqueness on quorum_hash, and the quorum_hash IS the
    // anchor block hash; V11 requires that anchor to be a boundary). So if two
    // due quorums both started at the same boundary, both ceremonies would
    // produce payloads with the same quorum_hash and only ONE could be
    // accepted — the loser's full 11-member ceremony wasted. Deterministic
    // lowest-quorum_hash-first makes the collision impossible, and needs NO
    // drift_offset producer (the deadline/stagger alternative would). The
    // deferred quorum simply becomes due again at the next boundary.
    if (pindexAnchor == nullptr) return PTXRotationDecision{};

    const std::vector<CPTXQuorumRecord> active =
            store.GetActiveQuorumsAtHeight(pindexAnchor->nHeight);

    // Of the ACTIVE quorums, which are DUE at this anchor? Age is measured from
    // the quorum's own formation anchor, so a quorum is due once a full
    // interval has elapsed since it formed.
    const uint256* lowest = nullptr;
    for (const CPTXQuorumRecord& rec : active) {
        if (pindexAnchor->nHeight - rec.formation_height < params.nFormationInterval)
            continue;                                     // not yet due
        // W2.4 W4-e — THE KDD-075 YIELD: terminal-eligibility yields rotation
        // AT CEREMONY-START.  ★ Keyed on ELIGIBILITY, never "fired": an
        // eligible quorum the rate limiter defers this window STILL yields —
        // it stays ACTIVE-queued for its turn.  Keying on "fired" would let
        // the deferred quorum fall through to rotating, mint a successor with
        // a fresh (reset) idleness view, and reopen Hazard A through the
        // limiter.  A rotation-impossible quorum yielding here also stops it
        // winning the tie-break with a ceremony that cannot start (the
        // ODC-045 starvation fix — the yield's double duty, KDD-076).
        // With the params gate at its 0 defaults this is never true.
        std::string why;
        if (PTX_Formation_TerminalEligible(rec, pindexAnchor, params,
                                           read_block, impossible_at, &why)) {
            // Diagnostic only (not consensus): the DIRECT observable for the
            // KDD-075 yield — without it the yield is a silent continue and a
            // drill can only observe it by proxy.
            LogPrintf("PTX formation: quorum %s TERMINAL-ELIGIBLE (%s) at height %d - "
                      "rotation YIELDED (KDD-075)\n",
                      rec.quorum_hash.ToString(), why, pindexAnchor->nHeight);
            continue;                                     // yields to reform
        }
        if (lowest == nullptr || rec.quorum_hash < *lowest)
            lowest = &rec.quorum_hash;                    // tie-break: lowest hash wins
    }
    if (lowest == nullptr) return PTXRotationDecision{};   // none due

    PTXRotationDecision d;
    d.due = true;
    d.predecessor_quorum_hash = *lowest;
    return d;
}

void PTX_Formation_RunTipSweeps(int tip_height)
{
    // See the header contract for WHY this takes only a height (the placement
    // decision is enforced by the signature). Null store/evoDb = unit-test
    // environment; the pure share sweeps still run.
    if (evoDb != nullptr) {
        PTX_BLS_ExpirePending(tip_height, evoDb.get());      // KDD-070 §7 TTL
        PTX_BLS_DiscardSuperseded(tip_height, evoDb.get());  // KDD-070 §6 depth
    }
    if (ptxQuorumStore != nullptr) {
        ptxQuorumStore->RetireSupersededResidues(tip_height); // KDD-070 §5 bound
    }
}

bool PTX_Formation_SelectRotationMembers(
        const CPTXQuorumRecord& predecessor,
        const CDeterministicGMList& listAtRotationAnchor,
        const CDeterministicGMList& listAtFormationAnchor,
        std::vector<PTXDKGMember>& membersOut)
{
    // KDD-072 P-b3a (dormant until the P-b6 trigger): resolve through the SAME
    // shared core as V12/the store guard, then materialize through the SAME
    // mapper as the fresh path. All-or-nothing: any unresolvable member or
    // key change means the rotation does not start.
    membersOut.clear();
    std::vector<CDeterministicGMCPtr> quorum;
    std::string err;
    if (!PTX_DKG_ResolveRotationQuorum(predecessor, listAtRotationAnchor,
                                       listAtFormationAnchor, quorum, err)) {
        LogPrintf("PTX formation: rotation of %s NOT started: %s\n",
                  predecessor.quorum_hash.ToString(), err);
        return false;
    }
    membersOut = PTX_DKG_MembersFromQuorum(quorum);
    return membersOut.size() == predecessor.members.size();
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
    // KDD-072 P-b6a: null on the fresh path (the overwhelming majority, and the
    // ONLY path reachable while RotationDueAt is stubbed false); set to the
    // predecessor when a rotation is due. Bound into the session below — that
    // one field then feeds the Phase-4 sign-hash (P-b2), the v2 payload +
    // predecessor (P-b2), and the KDD-070 PENDING share role (ClosePhase5).
    uint256 rotationPredecessor;
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

        // KDD-072 P-b6a — THE ROTATION BRANCH. The decision is P-b6b's policy;
        // the stub returns due=false today, so the else-arm below is the only
        // reachable path and the fresh behaviour is byte-identical to pre-P-b6a.
        // W2.4 W4-e — the production eligibility sources: blocks from disk
        // (the authenticated attributions W4-b verified at connect), and the
        // resolver composed with the boundary-time DGM lists.  Fail-safe
        // NOT-impossible when the formation anchor is unknown.
        const auto readBlockFromDisk = [](const CBlockIndex* p, CBlock& out) {
            return ReadBlockFromDisk(out, p);
        };
        const auto impossibleAt = [](const CPTXQuorumRecord& r, const CBlockIndex* pb) {
            const CBlockIndex* pForm = LookupBlockIndex(r.quorum_hash);
            if (pForm == nullptr) return false;   // fail-safe: not impossible
            const CDeterministicGMList listRot  = deterministicGMManager->GetListForBlock(pb);
            const CDeterministicGMList listForm = deterministicGMManager->GetListForBlock(pForm);
            std::string why;
            return PTX_Formation_RotationImpossible(r, listRot, listForm, why);
        };
        const PTXRotationDecision decision =
                PTX_Formation_RotationDueAt(pindexAnchor, *ptxQuorumStore, params,
                                            readBlockFromDisk, impossibleAt);
        if (decision.due) {
            // ROTATION: members come from the predecessor record through the
            // SHARED resolver (P-b3a) — the identical resolution V12 and the
            // store connect guard run, so this ceremony produces exactly the
            // membership the chain will validate. Any unresolvable member or
            // ProUpReg'd key means the rotation must NOT start (reject-not-
            // exclude, one policy for all sites).
            CPTXQuorumRecord predRec;
            if (!ptxQuorumStore->GetQuorumRecord(decision.predecessor_quorum_hash, predRec)) {
                LogPrintf("PTX formation: rotation due for %s but no record — not starting\n",
                          decision.predecessor_quorum_hash.ToString());
                return;
            }
            const CBlockIndex* pindexPred = LookupBlockIndex(predRec.quorum_hash);
            if (pindexPred == nullptr) {
                LogPrintf("PTX formation: rotation predecessor anchor %s not found — not starting\n",
                          predRec.quorum_hash.ToString());
                return;
            }
            const CDeterministicGMList listRot =
                    deterministicGMManager->GetListForBlock(pindexAnchor);
            const CDeterministicGMList listForm =
                    deterministicGMManager->GetListForBlock(pindexPred);
            if (!PTX_Formation_SelectRotationMembers(predRec, listRot, listForm, members)) {
                // SelectRotationMembers logs the specific member/key reason.
                return;
            }
            rotationPredecessor = decision.predecessor_quorum_hash;
            LogPrintf("PTX formation: ROTATION of %s at anchor %s — %d same-set members\n",
                      rotationPredecessor.ToString(), anchorHash.ToString(), (int)members.size());
        } else {
            // FRESH FORMATION (unchanged): selection at the anchor (SG-1a).
            // pool < 11 => deterministic skip.
            if (!PTX_Formation_SelectAtAnchor(pindexAnchor, members)) {
                LogPrintf("PTX formation: pool below threshold at anchor %s — deterministic skip\n",
                          anchorHash.ToString());
                return;
            }
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
    // KDD-072 P-b2 setter site, FED by P-b6a's rotation branch above: null on
    // the fresh path (and always, while RotationDueAt is stubbed false), the
    // predecessor when a rotation is due. This single field is the whole
    // rotation binding — everything downstream reads it and needed NO change:
    // the Phase-4 sign-hash (P-b2), the v2 payload + predecessor (P-b2), and
    // the KDD-070 PENDING share role at ClosePhase5.
    session->predecessor_quorum_hash = rotationPredecessor;
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

    // KDD-072 P-b6b — THE TIP SWEEPS, on EVERY block (boundary or not), past
    // the IBD guard above. Placed here rather than in ProcessBlock precisely
    // because a stranded PENDING's successor never mines: no PTXDKG block will
    // ever arrive to trigger its expiry, so a connect-path sweep would never
    // fire for the case that needs it most. See PTX_Formation_RunTipSweeps.
    PTX_Formation_RunTipSweeps(pindexNew->nHeight);

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

// ---------------------------------------------------------------------------
// W2.4 W4-d — the three terminal-eligibility predicates (KDD-074/075/076).
// Pure and stateless; see the header for the full contracts.  DORMANT: no
// production caller until W4-e composes them into the RotationDueAt yield.
// ---------------------------------------------------------------------------

bool PTX_Formation_BlockHasAttributedRoll(const CBlock& block,
                                          const uint256& quorum_hash)
{
    for (const auto& tx : block.vtx) {
        if (!tx->IsProbabilisticTx()) continue;
        CProbabilisticTxPayload payload;
        if (!GetTxPayload(*tx, payload)) continue;
        // The attribution compared here is AUTHENTICATED: W4-b rejects any
        // roll whose quorum_sig does not verify against the named record's
        // group_pk, so a connected block cannot carry a forged quorum_hash.
        if (payload.quorum_hash == quorum_hash) return true;
    }
    return false;
}

bool PTX_Formation_QuorumIdleAt(
        const uint256& quorum_hash,
        const CBlockIndex* pindexTip,
        int n_retire,
        const std::function<bool(const CBlockIndex*, CBlock&)>& read_block)
{
    // FAIL-SAFE: degenerate inputs answer NOT idle — never retire on missing
    // data (the same posture as the as-of predicate's unknown-state arm).
    if (pindexTip == nullptr || n_retire <= 0 || !read_block) return false;

    // The window (tip - n_retire, tip]: exactly n_retire blocks, walked
    // tip-first; truncated at genesis on a young chain.
    const CBlockIndex* p = pindexTip;
    for (int i = 0; i < n_retire && p != nullptr; ++i, p = p->pprev) {
        CBlock block;
        if (!read_block(p, block)) return false;  // unreadable: NOT idle
        if (PTX_Formation_BlockHasAttributedRoll(block, quorum_hash)) {
            return false;                          // attributed output in window
        }
    }
    return true;
}

bool PTX_Formation_RotationImpossible(
        const CPTXQuorumRecord& predecessor,
        const CDeterministicGMList& listAtRotationAnchor,
        const CDeterministicGMList& listAtFormationAnchor,
        std::string& why_out)
{
    // The P-b3a resolver IS the predicate (one implementation, KDD-073):
    // impossible == the same-set resolve refuses.  Never reimplemented — the
    // eligibility and V12's validity can therefore never disagree.
    std::vector<CDeterministicGMCPtr> unused;
    return !PTX_DKG_ResolveRotationQuorum(predecessor, listAtRotationAnchor,
                                          listAtFormationAnchor, unused, why_out);
}

bool PTX_Formation_ForcedReformGraceElapsed(
        const CPTXQuorumRecord& rec,
        const CBlockIndex* pindexAnchor,
        int formation_interval,
        int grace_m,
        const std::function<bool(const CBlockIndex*)>& impossible_at)
{
    // FAIL-SAFE false: a grace that cannot be evaluated has not elapsed.
    if (pindexAnchor == nullptr || formation_interval <= 0 || grace_m <= 0 ||
        !impossible_at) {
        return false;
    }
    // Due-AND-impossible at each of the last grace_m boundaries, newest
    // first.  Any boundary that was NOT due (quorum too young there) or NOT
    // impossible (the pathological ProUpReg self-heal) breaks the run — the
    // stateless re-derivation IS the grace reset.
    for (int i = 0; i < grace_m; ++i) {
        const int bh = pindexAnchor->nHeight - i * formation_interval;
        if (bh < 0) return false;
        // Plain pprev walk, NOT GetAncestor: this tree's GetAncestor follows
        // pskip unguarded (chain.cpp — upstream guards it, this fork doesn't),
        // so it faults on any index without a built skiplist.  The walk is
        // bounded by grace_m * interval and assumption-free.
        const CBlockIndex* pb = pindexAnchor;
        while (pb != nullptr && pb->nHeight > bh) pb = pb->pprev;
        if (pb == nullptr || pb->nHeight != bh) return false;
        if (pb->nHeight - rec.formation_height < formation_interval) {
            return false;  // not yet due at this boundary
        }
        if (!impossible_at(pb)) {
            return false;  // rotation was possible here: grace restarts
        }
    }
    return true;
}

bool PTX_Formation_TerminalEligible(
        const CPTXQuorumRecord& rec,
        const CBlockIndex* pindexAnchor,
        const Consensus::PTXFormationParams& params,
        const std::function<bool(const CBlockIndex*, CBlock&)>& read_block,
        const std::function<bool(const CPTXQuorumRecord&, const CBlockIndex*)>& impossible_at,
        std::string* why_out)
{
    // KDD-074 idle arm — gated by nRetireWindow (0 = disabled).
    // ★ THE AGE ANCHOR (W4-f amendment, pre-drill finding): "N blocks of
    // silence" semantically requires N blocks of the QUORUM'S opportunity to
    // be silent — the scan below is chain-anchored, so without this clause a
    // young quorum on a quiet chain is instantly idle-eligible (youth misread
    // as idleness) and the reformed successor churns: reformed away at its
    // first boundary before it could be anything.  The forced arm already
    // excludes young quorums (grace-M's per-boundary due-ness); this brings
    // idle into line.  ONE implementation — the yield and the producer both
    // inherit it here.
    if (params.nRetireWindow > 0 &&
        rec.mined_height + params.nRetireWindow <= pindexAnchor->nHeight &&
        PTX_Formation_QuorumIdleAt(rec.quorum_hash, pindexAnchor,
                                   params.nRetireWindow, read_block)) {
        if (why_out != nullptr) *why_out = "idle";
        return true;
    }
    // KDD-076 forced-reform arm — gated by nReformGrace (0 = disabled; the
    // grace function itself fail-safes on m <= 0).  Due-AND-impossible at
    // each of the last nReformGrace boundaries, rec-bound.
    if (params.nReformGrace > 0 &&
        PTX_Formation_ForcedReformGraceElapsed(
                rec, pindexAnchor, params.nFormationInterval, params.nReformGrace,
                [&](const CBlockIndex* pb) { return impossible_at(rec, pb); })) {
        if (why_out != nullptr) *why_out = "forced-reform";
        return true;
    }
    return false;
}

bool PTX_Formation_SelectReformCandidate(
        const std::vector<std::pair<uint256, int>>& candidates,
        int tip_height,
        int rate_window,
        int last_reform_height,
        uint256& selected_out)
{
    // Gate posture: a disabled limiter selects NOTHING (the transition stays
    // dormant even with eligibility enabled).
    if (rate_window <= 0 || candidates.empty()) return false;
    // One per window: a reform inside the window rate-limits everyone out.
    if (last_reform_height >= 0 && tip_height - last_reform_height < rate_window) {
        return false;
    }
    // Least-recently-active first (smallest last_activity_height), ties to
    // the lowest hash — deterministic on every node (the P-b6b shape).
    const std::pair<uint256, int>* best = nullptr;
    for (const auto& c : candidates) {
        if (best == nullptr || c.second < best->second ||
            (c.second == best->second && c.first < best->first)) {
            best = &c;
        }
    }
    selected_out = best->first;
    return true;
}
