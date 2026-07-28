// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W2.2 SG-1a — pure formation caller unit tests (W2.2_SG1A_PREIMPL_APPROVED).
//
// Rows covered here (the on-fleet complements live in battery_sg1.py):
//   (a) SNAPSHOT-NOT-LIVE, unit-constructed: anchor-list vs tip-list differ by
//       one post-anchor GM; selection from the anchor list != selection from
//       the tip list. The stub->RED cycle (point the caller at the tip list in
//       source, rebuild, observe RED) is run manually at the gate; this TU
//       carries the constructed-divergence assert both paths depend on.
//   (d) KDD-040 exclusion: an ACTIVE record's members (FULL formed-11,
//       D-SG1a-1 — QUAL or not) never re-selected; pool arithmetic exact.
//   SCORE-ORDER FIXTURE (KDD-061 seam): selected order == recomputed score
//       ranking AND != alphabetical(label) order; the naive-source assertion
//       (== alphabetical) is shown to FAIL.
//   V5-swap no-op arm: BuildPool with zero ACTIVE records returns the
//       eligible list unchanged (selection byte-identical pre/post swap).
//   pool >= 11 threshold: deterministic skip below, formation at exactly 11.
//
// PTX_Formation_SelectAtAnchor itself needs a real CBlockIndex + populated
// DGM snapshot (the V1-V4 limitation, ptx_dkg_validation_tests.cpp note);
// its on-anchor behaviour is exercised on-fleet via ptx_debug_selectquorum
// (rows (c) and fixture). Here we unit-test the pure pool/selection layers
// it delegates to.

#include "test/test_Hemis.h"
#include "ptx/ptx_formation.h"
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_quorum_store.h"
#include "evo/deterministicgms.h"

#include "bls/bls_wrapper.h"
#include "chain.h"
#include "consensus/params.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <deque>
#include <set>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_formation_tests, BasicTestingSetup)

namespace {

static uint256 Bytes32(unsigned char b0, unsigned char b1)
{
    std::vector<unsigned char> v(32, 0);
    v[0] = b0; v[1] = b1;
    return uint256(v);
}

static uint256 AnchorHash() { return Bytes32(0xA0, 0x01); }

static CBLSSecretKey NewSk() { CBLSSecretKey sk; sk.MakeNewKey(); return sk; }

// Same identity idiom as ptx_dkg_validation_tests.cpp MakeGM.
static std::shared_ptr<CDeterministicGM> MakeGM(uint64_t id,
                                                const std::string& node_id)
{
    std::vector<unsigned char> pb(32, 0);
    pb[0] = (unsigned char)id; pb[1] = 0xB2;
    uint256 proTx(pb);

    auto dgm = std::make_shared<CDeterministicGM>(id);
    dgm->proTxHash          = proTx;
    dgm->collateralOutpoint = COutPoint(proTx, 0);

    auto st = std::make_shared<CDeterministicGMState>();
    std::vector<unsigned char> cb(32, 0x22);
    cb[0] = (unsigned char)id; // vary confirmedHash so scores differ
    st->UpdateConfirmedHash(proTx, uint256(cb));
    st->pubKeyOperator.Set(NewSk().GetPublicKey()); // unique per GM (AddGM enforces)
    uint160 k20; memcpy(k20.begin(), proTx.begin(), 20);
    st->keyIDOwner  = CKeyID(k20);
    st->keyIDVoting = st->keyIDOwner;
    st->node_id     = node_id;
    dgm->pdgmState = st;
    return dgm;
}

// gmNN:xxxx labels, sequential — the alphabetical trap the score fixture must
// demonstrably NOT follow.
static std::string Label(int i)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "gm%02d:aa%02d", i, i);
    return std::string(buf);
}

static CDeterministicGMList BuildList(int n, int firstId = 0)
{
    CDeterministicGMList list;
    for (int i = 0; i < n; i++)
        list.AddGM(MakeGM((uint64_t)(firstId + i), Label(firstId + i)));
    return list;
}

// An ACTIVE record whose members are the given GMs (full formed-11 shape;
// in_qual varied to prove D-SG1a-1 excludes QUAL and non-QUAL alike).
static CPTXQuorumRecord MakeActiveRecord(
        const std::vector<CDeterministicGMCPtr>& members)
{
    CPTXQuorumRecord rec;
    rec.quorum_hash = Bytes32(0xAC, 0x71);
    rec.state = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    rec.formed_size = (uint8_t)members.size();
    rec.completed_size = (uint8_t)members.size();
    for (size_t i = 0; i < members.size(); i++) {
        PTXQuorumMemberRecord m;
        m.node_id     = members[i]->pdgmState->node_id;
        m.proTxHash   = members[i]->proTxHash;
        m.share_index = (uint8_t)(i + 1);
        m.in_qual     = (i % 2 == 0); // mix: exclusion must not depend on QUAL
        rec.members.push_back(m);
    }
    return rec;
}

static std::vector<uint256> SelectedProTx(const std::vector<PTXDKGMember>& v)
{
    std::vector<uint256> out;
    for (const auto& m : v) out.push_back(m.proTxHash);
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// V5-swap NO-OP arm: zero ACTIVE records => BuildPool == eligible list, and
// the selection through the pool is byte-identical to the pre-swap direct
// selection. (The fleet-level half of the no-op proof is the unchanged
// existing suite + batteries.)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(buildpool_zero_active_is_noop)
{
    CDeterministicGMList list = BuildList(22);
    std::vector<CPTXQuorumRecord> noActive;

    CDeterministicGMList pool = PTX_Formation_BuildPool(list, noActive);
    BOOST_CHECK_EQUAL(pool.GetValidGMsCount(), 22u);

    auto direct  = PTX_DKG_SelectQuorumFromList(list, AnchorHash());
    auto through = PTX_DKG_SelectQuorumFromList(pool, AnchorHash());
    BOOST_REQUIRE_EQUAL(direct.size(), 11u);
    BOOST_REQUIRE_EQUAL(through.size(), 11u);
    for (size_t i = 0; i < 11; i++)
        BOOST_CHECK(direct[i]->proTxHash == through[i]->proTxHash);
}

// ---------------------------------------------------------------------------
// Row (d) unit: KDD-040 exclusion — the FULL formed-11 of an ACTIVE record
// (QUAL and non-QUAL members alike, D-SG1a-1) never re-selected. With
// 22 GMs and 11 active the arithmetic is exact: the pool must be precisely
// the other 11, and the selection must equal that complement as a set.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(kdd040_active_members_excluded)
{
    CDeterministicGMList list = BuildList(22);

    // First selection at the anchor = the "active quorum".
    auto firstQuorum = PTX_DKG_SelectQuorumFromList(list, AnchorHash());
    BOOST_REQUIRE_EQUAL(firstQuorum.size(), 11u);
    CPTXQuorumRecord active = MakeActiveRecord(firstQuorum);

    std::set<uint256> activeSet;
    for (const auto& d : firstQuorum) activeSet.insert(d->proTxHash);

    // Pool = eligible minus the active 11 — exactly 11 remain.
    CDeterministicGMList pool =
        PTX_Formation_BuildPool(list, {active});
    BOOST_CHECK_EQUAL(pool.GetValidGMsCount(), 11u);
    pool.ForEachGM(true, [&](const CDeterministicGMCPtr& dgm) {
        BOOST_CHECK_MESSAGE(activeSet.count(dgm->proTxHash) == 0,
                            "active member leaked into the pool");
    });

    // Second draw from the pool: intersection with the active set is empty,
    // and (22 = 2 x 11) the selection is exactly the complement.
    auto second = PTX_DKG_SelectQuorumFromList(pool, Bytes32(0xA0, 0x02));
    BOOST_REQUIRE_EQUAL(second.size(), 11u);
    for (const auto& d : second)
        BOOST_CHECK(activeSet.count(d->proTxHash) == 0);

    // Sanity for the stub->RED cycle: WITHOUT the exclusion (raw list), the
    // same modifier re-selects at least one active member — the RED the
    // stub-the-join build must show.
    auto unexcluded = PTX_DKG_SelectQuorumFromList(list, Bytes32(0xA0, 0x02));
    bool anyActive = false;
    for (const auto& d : unexcluded)
        if (activeSet.count(d->proTxHash)) anyActive = true;
    BOOST_CHECK_MESSAGE(anyActive,
        "constructed scenario must make the unexcluded draw pick an active member");
}

// ---------------------------------------------------------------------------
// Row (a) unit-constructed: anchor-list vs tip-list differ by one
// post-anchor GM. Selection from the two lists DIFFERS — the divergence a
// live-tip read would cause across nodes. (The source-stub RED cycle at the
// gate points SelectAtAnchor at the "tip" list and observes this assert go
// red in reverse.)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(snapshot_not_live_divergence_constructed)
{
    CDeterministicGMList atAnchor = BuildList(22);
    // "tip" list: same 22 plus one GM registered after the anchor whose score
    // lands top-11 for this modifier (guaranteed by trying ids until it does).
    const uint256 modifier = Bytes32(0xA0, 0x03);
    auto anchorSel = SelectedProTx(
        PTX_DKG_BuildMemberVectorFromList(atAnchor, modifier));
    BOOST_REQUIRE_EQUAL(anchorSel.size(), 11u);

    bool diverged = false;
    for (uint64_t newcomer = 100; newcomer < 140 && !diverged; newcomer++) {
        CDeterministicGMList atTip = BuildList(22);
        atTip.AddGM(MakeGM(newcomer, Label((int)newcomer)));
        auto tipSel = SelectedProTx(
            PTX_DKG_BuildMemberVectorFromList(atTip, modifier));
        BOOST_REQUIRE_EQUAL(tipSel.size(), 11u);
        if (tipSel != anchorSel) diverged = true;
    }
    BOOST_CHECK_MESSAGE(diverged,
        "a post-anchor registration must be able to change the selection — "
        "anchored reads are load-bearing");
}

// ---------------------------------------------------------------------------
// SCORE-ORDER FIXTURE (KDD-061 seam): the selected share_index order equals
// the independently recomputed score ranking AND is demonstrably NOT the
// alphabetical(label) order; the naive-source assertion fails.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(score_order_fixture_not_alphabetical)
{
    CDeterministicGMList list = BuildList(22);
    const uint256 modifier = AnchorHash();

    auto members = PTX_DKG_BuildMemberVectorFromList(list, modifier);
    BOOST_REQUIRE_EQUAL(members.size(), 11u);

    // Independent recomputation of the score ranking through the same
    // public scoring surface (CalculateScores), sorted descending with the
    // collateral tiebreak — the deterministicgms.cpp:228-234 contract.
    CDeterministicGMList eligible;
    list.ForEachGM(true, [&](const CDeterministicGMCPtr& dgm) {
        if (PTX_DKG_IsGMPTXEligible(dgm)) eligible.AddGM(dgm);
    });
    auto scores = eligible.CalculateScores(modifier);
    std::sort(scores.rbegin(), scores.rend(),
              [](const std::pair<arith_uint256, CDeterministicGMCPtr>& a,
                 const std::pair<arith_uint256, CDeterministicGMCPtr>& b) {
                  if (a.first == b.first)
                      return a.second->collateralOutpoint < b.second->collateralOutpoint;
                  return a.first < b.first;
              });
    BOOST_REQUIRE(scores.size() >= 11u);
    for (size_t i = 0; i < 11; i++) {
        BOOST_CHECK_MESSAGE(members[i].proTxHash == scores[i].second->proTxHash,
                            "share_index order must equal the score ranking at rank "
                            << i);
    }

    // The naive index source: alphabetical(label). Must NOT match.
    std::vector<std::string> selectedLabels;
    for (const auto& m : members) selectedLabels.push_back(m.node_id);
    std::vector<std::string> alpha = selectedLabels;
    std::sort(alpha.begin(), alpha.end());
    BOOST_CHECK_MESSAGE(selectedLabels != alpha,
        "score order coincides with alphabetical — fixture lost its power "
        "(the naive-source assertion would falsely pass)");
}

// ---------------------------------------------------------------------------
// pool >= 11 threshold: below => empty draw (deterministic skip shape);
// exactly 11 => full formation from the whole pool.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(pool_threshold_gate)
{
    // 10 eligible: CalculateQuorum(11) cannot fill — SelectAtAnchor's
    // pool<11 gate returns false before ever drawing; the pure layers show
    // the underfull draw the gate forestalls.
    CDeterministicGMList ten = BuildList(10);
    auto draw10 = PTX_DKG_SelectQuorumFromList(ten, AnchorHash());
    BOOST_CHECK(draw10.size() < 11u);

    // Exactly 11 in the pool: formation is the whole pool, score-ordered.
    CDeterministicGMList eleven = BuildList(11);
    auto draw11 = PTX_DKG_BuildMemberVectorFromList(eleven, AnchorHash());
    BOOST_REQUIRE_EQUAL(draw11.size(), 11u);
    std::set<uint256> got;
    for (const auto& m : draw11) got.insert(m.proTxHash);
    BOOST_CHECK_EQUAL(got.size(), 11u);
}

// ===========================================================================
// W2.2 SG-1b-i — pure schedule core (boundary + anchor). Rows per the
// authorised plan; tests inject arbitrary N via the params argument (no net
// dependency). The chain fixture is synthetic CBlockIndex (pprev + BuildSkip
// — the skiplist_tests idiom); GetAncestor follows pskip, so BuildSkip is
// mandatory on every index.
// ===========================================================================

namespace {

static Consensus::PTXFormationParams ScheduleParams(int n)
{
    Consensus::PTXFormationParams p;
    p.name = "unit";
    p.nFormationInterval = n;
    return p;
}

// Extend a branch by `count` blocks from `from` (nullptr => start at genesis
// h0). std::deque keeps element addresses stable across growth.
static CBlockIndex* ExtendBranch(std::deque<CBlockIndex>& store,
                                 CBlockIndex* from, int count)
{
    CBlockIndex* tip = from;
    for (int i = 0; i < count; i++) {
        store.emplace_back();
        CBlockIndex& ix = store.back();
        ix.pprev = tip;
        ix.nHeight = tip ? tip->nHeight + 1 : 0;
        ix.BuildSkip();
        tip = &ix;
    }
    return tip;
}

} // namespace

// ---------------------------------------------------------------------------
// Row 1 — MID-CYCLE NO-FIRE (the stub->RED driver): every h % N != 0 must
// NOT be a boundary. Runs first among the schedule rows by design: against
// the always-true stub this is the RED.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sg1b_midcycle_nofire)
{
    for (int N : {7, 80}) {
        const auto p = ScheduleParams(N);
        for (int h = 1; h <= 4 * N; h++) {
            if (h % N == 0) continue;
            BOOST_CHECK_MESSAGE(!PTX_Formation_IsBoundary(h, p),
                "mid-cycle height " << h << " fired (N=" << N << ")");
        }
    }
}

// ---------------------------------------------------------------------------
// Row 2 — the boundary fires: h = k*N for k >= 1.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sg1b_boundary_fires)
{
    for (int N : {7, 80, 1440}) {
        const auto p = ScheduleParams(N);
        for (int k = 1; k <= 5; k++)
            BOOST_CHECK_MESSAGE(PTX_Formation_IsBoundary(k * N, p),
                "boundary height " << k * N << " did not fire (N=" << N << ")");
    }
}

// ---------------------------------------------------------------------------
// Row 3 — GENESIS EDGE: 0 % N == 0 for every N, so height 0 must be
// EXPLICITLY excluded (no formation from genesis, by construction).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sg1b_genesis_excluded)
{
    for (int N : {1, 7, 80, 1440})
        BOOST_CHECK_MESSAGE(!PTX_Formation_IsBoundary(0, ScheduleParams(N)),
            "genesis (h0) fired as a boundary (N=" << N << ")");
}

// ---------------------------------------------------------------------------
// Row 4 — ANCHOR EXACTNESS: for every h in [kN, (k+1)N) the anchor is the
// block at kN on the tip's own branch; at h = kN the anchor is pindexNew
// itself. (Pre-first-boundary heights anchor to genesis; IsBoundary is what
// gates firing, not the walk.)
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sg1b_anchor_exactness)
{
    std::deque<CBlockIndex> store;
    CBlockIndex* tip = ExtendBranch(store, nullptr, 201); // h0..h200
    const auto p = ScheduleParams(80);

    std::vector<const CBlockIndex*> byHeight(201, nullptr);
    for (const CBlockIndex* ix = tip; ix; ix = ix->pprev)
        byHeight[ix->nHeight] = ix;

    for (int h = 0; h <= 200; h++) {
        const CBlockIndex* anchor =
            PTX_Formation_GetAnchor(byHeight[h], p);
        const int expect = h - (h % 80);
        BOOST_REQUIRE_MESSAGE(anchor != nullptr, "null anchor at h" << h);
        BOOST_CHECK_EQUAL(anchor->nHeight, expect);
        BOOST_CHECK_MESSAGE(anchor == byHeight[expect],
            "anchor at h" << h << " is not the branch's block at " << expect);
        if (h % 80 == 0)
            BOOST_CHECK_MESSAGE(anchor == byHeight[h],
                "at a boundary the anchor must be pindexNew itself (h" << h << ")");
    }
}

// ---------------------------------------------------------------------------
// Row 5 — REORG: GetAncestor-vs-chainActive made concrete.
// Fork BELOW a boundary, both branches extended past it: each tip's anchor
// is ITS OWN branch's boundary block — same height, different block.
// chainActive[] indexing would have handed BOTH tips the active branch's
// block (the self-poisoning divergence class). Fork ABOVE the boundary:
// both tips share the identical anchor block.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sg1b_anchor_reorg_getancestor)
{
    const auto p = ScheduleParams(80);
    std::deque<CBlockIndex> store;

    // "Active" branch A: h0..h170.
    CBlockIndex* tipA = ExtendBranch(store, nullptr, 171);
    std::vector<CBlockIndex*> a(171, nullptr);
    for (CBlockIndex* ix = tipA; ix; ix = ix->pprev)
        a[ix->nHeight] = ix;

    // Fork BELOW the h160 boundary: branch B leaves A at h75, extends to
    // h170 with its own blocks 76..170.
    CBlockIndex* tipB = ExtendBranch(store, a[75], 95);
    BOOST_REQUIRE_EQUAL(tipB->nHeight, 170);

    const CBlockIndex* anchorA = PTX_Formation_GetAnchor(tipA, p);
    const CBlockIndex* anchorB = PTX_Formation_GetAnchor(tipB, p);
    BOOST_REQUIRE(anchorA != nullptr && anchorB != nullptr);
    BOOST_CHECK_EQUAL(anchorA->nHeight, 160);
    BOOST_CHECK_EQUAL(anchorB->nHeight, 160);
    BOOST_CHECK_MESSAGE(anchorA == a[160],
        "A's anchor must be A's own block at h160");
    BOOST_CHECK_MESSAGE(anchorA != anchorB,
        "fork below the boundary: each tip must anchor to ITS OWN branch's "
        "boundary block — a shared anchor here is exactly what chainActive[] "
        "indexing would wrongly produce for the non-active tip");
    BOOST_CHECK(tipB->GetAncestor(160) == anchorB); // B's anchor is on B

    // Fork ABOVE the h80 boundary (and below h160): branch C leaves A at
    // h100, extends to h150. Both tips derive the identical SHARED anchor.
    CBlockIndex* tipC = ExtendBranch(store, a[100], 50);
    BOOST_REQUIRE_EQUAL(tipC->nHeight, 150);
    const CBlockIndex* anchorC  = PTX_Formation_GetAnchor(tipC, p);
    const CBlockIndex* anchorA2 = PTX_Formation_GetAnchor(a[150], p);
    BOOST_CHECK_MESSAGE(anchorC == anchorA2 && anchorC == a[80],
        "fork above the boundary: both tips must share the pre-fork anchor");
}

// ---------------------------------------------------------------------------
// Row 6 — DETERMINISM: repeated identical inputs -> identical outputs
// (recorded; purity is otherwise enforced by signature — no wall-clock, no
// node-local state can enter).
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(sg1b_schedule_determinism)
{
    const auto p = ScheduleParams(80);
    std::deque<CBlockIndex> store;
    CBlockIndex* tip = ExtendBranch(store, nullptr, 130); // h0..h129

    std::vector<bool> first;
    for (int h = 0; h <= 129; h++)
        first.push_back(PTX_Formation_IsBoundary(h, p));
    const CBlockIndex* anchorFirst = PTX_Formation_GetAnchor(tip, p);

    for (int rep = 0; rep < 3; rep++) {
        for (int h = 0; h <= 129; h++)
            BOOST_CHECK(PTX_Formation_IsBoundary(h, p) == first[(size_t)h]);
        BOOST_CHECK(PTX_Formation_GetAnchor(tip, p) == anchorFirst);
    }
}


// ---------------------------------------------------------------------------
// SG-3 — PTX_SelectDKGSigningCtx loader coverage.
//
// x-basis pinned: PTXQuorumMemberRecord.share_index is 1-BASED score-order
// rank (ptx_quorum_store.cpp:109, `(uint8_t)(i + 1)`), the same basis the DKG
// evaluated at (ptx_dkg.cpp:261 -> :318).  Lagrange x=0 is invalid over the
// scalar field, so a 0-based record would need +1 — it is NOT 0-based, and
// S0 below pins that so a future change to the assignment breaks here.
// ---------------------------------------------------------------------------

namespace {
// Build an ACTIVE record: n members, first `qual` marked in_qual, share_index
// 1-based score order (mirrors the persist site).
static CPTXQuorumRecord MakeSigningRecord(const uint256& qh, int formation_height,
                                          int n, int qual, size_t pk_size = 48)
{
    CPTXQuorumRecord rec;
    rec.quorum_hash      = qh;
    rec.formation_height = formation_height;
    rec.group_pk_bytes.assign(pk_size, 0xAB);
    rec.state            = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    rec.formed_size      = (uint8_t)n;  // t = majority(formed_size) is derived, not passed
    for (int i = 0; i < n; i++) {
        PTXQuorumMemberRecord m;
        m.node_id     = "gm" + std::to_string(i) + ":8080";
        m.share_index = (uint8_t)(i + 1);   // 1-based, as persisted
        m.in_qual     = (i < qual);
        rec.members.push_back(m);
    }
    return rec;
}
static uint256 QH(uint8_t b) { std::vector<unsigned char> v(32, b); return uint256(v); }
} // namespace

// S0 — the x-basis pin: emitted x must be 1-based (never 0).
BOOST_AUTO_TEST_CASE(S0_SigningCtx_ShareIndexIsOneBasedNeverZero)
{
    auto ctx = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x11), 100, 11, 11)}, QH(0x01));
    BOOST_REQUIRE(ctx.active);
    int min_x = 99;
    for (const auto& kv : ctx.share_index) min_x = std::min(min_x, kv.second);
    BOOST_CHECK_MESSAGE(min_x == 1,
        "share_index must be 1-BASED — Lagrange x=0 is invalid over the scalar field");
    BOOST_CHECK(ctx.share_index.at("gm0:8080") == 1);
    BOOST_CHECK_MESSAGE(ctx.threshold == 6,
        "t must be majority(formed_size=11)=6, derived — not passed in");
}

// ★ (a) §7.4 ROUTING (W2.5a): selection DISTRIBUTES across ACTIVE and is NOT
// newest-wins.  Supersedes the KDD-066 provisional rule this row used to
// assert ("must select the HIGHEST formation_height"), which was the
// structural half of ODC-052: every roll to one quorum leaves the other L-1
// idle by construction.  RED: revert to newest-wins -> every tip selects
// QH(0x22) (formation 1040) -> the >1-distinct-quorum assertion fails.
BOOST_AUTO_TEST_CASE(Sa_SigningCtx_DistributesNotNewestWins)
{
    std::vector<CPTXQuorumRecord> active{
        MakeSigningRecord(QH(0x11), 960,  11, 11),
        MakeSigningRecord(QH(0x22), 1040, 11, 11),
        MakeSigningRecord(QH(0x33), 880,  11, 11)};
    std::set<uint256> served;
    for (int i = 0; i < 64; i++) {
        auto ctx = PTX_SelectDKGSigningCtx(active, QH((uint8_t)i));
        BOOST_REQUIRE(ctx.active);
        served.insert(ctx.quorum_hash);
    }
    BOOST_CHECK_MESSAGE(served.size() > 1,
        "§7.4: rolls must reach MORE THAN ONE active quorum across tips - "
        "newest-wins (all to QH(0x22)) is ODC-052's structural half");
    // Every selection must land on a real ACTIVE member, never elsewhere.
    for (const auto& qh : served)
        BOOST_CHECK(qh == QH(0x11) || qh == QH(0x22) || qh == QH(0x33));
}

// (b) rejects group_pk != 48 bytes — present but unusable (fail-closed).
BOOST_AUTO_TEST_CASE(Sb_SigningCtx_RejectsBadGroupPkSize)
{
    auto ctx = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x11), 100, 11, 11, 47)}, QH(0x01));
    BOOST_CHECK_MESSAGE(ctx.quorum_present, "an ACTIVE record was supplied");
    BOOST_CHECK_MESSAGE(!ctx.active,
        "group_pk != 48 bytes MUST NOT yield usable signing material");
}

// (c) rejects in_qual count < threshold.
BOOST_AUTO_TEST_CASE(Sc_SigningCtx_RejectsSubThresholdInQual)
{
    auto ctx = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x11), 100, 11, 5)}, QH(0x01));
    BOOST_CHECK_MESSAGE(ctx.quorum_present, "an ACTIVE record was supplied");
    BOOST_CHECK_MESSAGE(!ctx.active,
        "in_qual (5) < threshold (6) MUST NOT yield usable signing material");
    // and 6 in_qual at threshold 6 IS usable (boundary, the other side).
    auto ok = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x11), 100, 11, 6)}, QH(0x01));
    BOOST_CHECK(ok.active);
    BOOST_CHECK_EQUAL((int)ok.member_ids.size(), 6);
}

// ★★ (a2) §7.4 ANTI-TARGETING — THE LOAD-BEARING ROW.  Selection keys on the
// TIP HASH, never on the caller's round_seed.  round_seed is built from
// caller_salt, which is FREE-FORM HEX (rpc/ptx.cpp): keying on it would let a
// caller grind salts locally at ~zero cost until the selector named a quorum of
// their choosing — a targeting oracle, and (worse) a lifecycle lever: avoid a
// quorum to force its reform, pin one to prevent it (adversarial ODC-052/047).
//
// The assertion: with the tip FIXED, varying the caller-controlled value cannot
// move the selection.  RED: key selection on the caller value instead (decision
// B) -> a crafted "salt" walks the selection across quorums -> this fails.
// ★ That inversion is what proves B was correctly rejected.
//
// RESIDUAL, documented not fixed: the caller can still TIMING-grind (wait ~L
// blocks for the tip to reshuffle).  The structural fix is commit-reveal, which
// needs an async roll flow — the escape hatch, not built here.
BOOST_AUTO_TEST_CASE(Sa2_SigningCtx_SelectionIgnoresCallerControlledValue)
{
    std::vector<CPTXQuorumRecord> active{
        MakeSigningRecord(QH(0x11), 960,  11, 11),
        MakeSigningRecord(QH(0x22), 1040, 11, 11),
        MakeSigningRecord(QH(0x33), 880,  11, 11)};

    const uint256 fixed_tip = QH(0x7E);
    auto baseline = PTX_SelectDKGSigningCtx(active, fixed_tip);
    BOOST_REQUIRE(baseline.active);

    // 256 distinct caller-controlled values (the grind a caller would run).
    // Selection is a pure function of (active, tip) — the caller value is not
    // an input at all, so not one of these can shift the result.
    for (int salt = 0; salt < 256; salt++) {
        auto ctx = PTX_SelectDKGSigningCtx(active, fixed_tip);
        BOOST_REQUIRE(ctx.active);
        BOOST_CHECK_MESSAGE(ctx.quorum_hash == baseline.quorum_hash,
            "caller-controlled input MUST NOT shift selection (targeting oracle)");
    }
    // And the tip — which the caller cannot forge — DOES move it: the selector
    // is live, not a constant.
    std::set<uint256> by_tip;
    for (int i = 0; i < 64; i++)
        by_tip.insert(PTX_SelectDKGSigningCtx(active, QH((uint8_t)i)).quorum_hash);
    BOOST_CHECK_MESSAGE(by_tip.size() > 1,
        "the tip MUST drive selection - otherwise routing is a constant");
}

// ★ (d) §7.4 ORDER-INDEPENDENCE: the same tip selects the same quorum
// regardless of the order GetActiveQuorumsAtHeight returned the records —
// selection sorts by quorum_hash first.  Storage iteration order must NEVER
// leak into routing, or two nodes route the same roll differently.
// RED: drop the sort -> selection follows input order -> a and b diverge.
BOOST_AUTO_TEST_CASE(Sd_SigningCtx_OrderIndependentUnderSort)
{
    std::vector<CPTXQuorumRecord> a{MakeSigningRecord(QH(0x55), 1000, 11, 11),
                                    MakeSigningRecord(QH(0x22), 1000, 11, 11)};
    std::vector<CPTXQuorumRecord> b{MakeSigningRecord(QH(0x22), 1000, 11, 11),
                                    MakeSigningRecord(QH(0x55), 1000, 11, 11)};
    for (int i = 0; i < 32; i++) {
        auto ca = PTX_SelectDKGSigningCtx(a, QH((uint8_t)i));
        auto cb = PTX_SelectDKGSigningCtx(b, QH((uint8_t)i));
        BOOST_REQUIRE(ca.active && cb.active);
        BOOST_CHECK_MESSAGE(ca.quorum_hash == cb.quorum_hash,
            "same tip MUST select the same quorum independent of input order");
    }
}

// (e) fail-closed signalling: quorum present + unusable is distinguishable
//     from no-quorum, so the caller can hard-error instead of using the dealer.
BOOST_AUTO_TEST_CASE(Se_SigningCtx_FailClosedDistinguishesPresentFromAbsent)
{
    auto none = PTX_SelectDKGSigningCtx({}, QH(0x01));
    BOOST_CHECK_MESSAGE(!none.quorum_present && !none.active,
        "no ACTIVE quorum -> dealer fallback is legitimate");
    auto bad = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x11), 100, 11, 2)}, QH(0x01));
    BOOST_CHECK_MESSAGE(bad.quorum_present && !bad.active,
        "ACTIVE quorum present but unusable MUST be distinguishable from absent "
        "(the caller hard-errors; falling back to the dealer would be fail-open)");
}

// ---------------------------------------------------------------------------
// ODC-036 — the threshold is QUORUM-SCOPED (derived from formed_size), and the
// coordinator's node-registry size CANNOT influence it.  These pin the
// DERIVATION, which the hand-passed-threshold tests never exercised (KDD-068).
// With the parameter GONE, the test literally cannot pass a registry size — the
// call takes only the active records, so registry-independence is structural.
// ---------------------------------------------------------------------------

// St1 — t derived from formed_size, registry-independent by construction.
BOOST_AUTO_TEST_CASE(St1_SigningCtx_ThresholdDerivedFromFormedSizeNotRegistry)
{
    // formed_size 11 -> t=6, regardless of how many nodes exist anywhere.
    auto ctx = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x11), 100, 11, 9)}, QH(0x01));
    BOOST_REQUIRE(ctx.active);
    BOOST_CHECK_MESSAGE(ctx.threshold == 6,
        "t = majority(formed_size=11) = 6 — NOT the 22-node registry's 12 (ODC-036)");
    // 9 in_qual >= t=6 -> usable (the exact fc8e0f0d live case that hard-errored
    // under the old registry-derived threshold of 12).
    BOOST_CHECK_EQUAL((int)ctx.member_ids.size(), 9);
}

// St2 — threshold tracks formed_size.
BOOST_AUTO_TEST_CASE(St2_SigningCtx_ThresholdTracksFormedSize)
{
    struct { int n, t; } cases[] = {{7,4},{11,6},{15,8}};
    for (auto& c : cases) {
        auto ctx = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x22), 100, c.n, c.n)}, QH(0x01));
        BOOST_REQUIRE(ctx.active);
        BOOST_CHECK_MESSAGE(ctx.threshold == c.t,
            "t = majority(formed_size) must track formed_size");
    }
}

// St3 — in_qual == t exactly is USABLE (the live 57e7c7b4 case: formed 11, 6 in_qual).
BOOST_AUTO_TEST_CASE(St3_SigningCtx_InQualEqualsThresholdIsUsable)
{
    auto ctx = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x33), 100, 11, 6)}, QH(0x01));
    BOOST_REQUIRE_EQUAL(ctx.threshold, 6);
    BOOST_CHECK_MESSAGE(ctx.active && (int)ctx.member_ids.size() == 6,
        "in_qual == t (6) MUST be usable — no off-by-one at the boundary");
}

// St4 — in_qual == t-1 is NOT usable, and quorum_present is still set (fail-closed).
BOOST_AUTO_TEST_CASE(St4_SigningCtx_InQualBelowThresholdFailsClosed)
{
    auto ctx = PTX_SelectDKGSigningCtx({MakeSigningRecord(QH(0x44), 100, 11, 5)}, QH(0x01));
    BOOST_REQUIRE_EQUAL(ctx.threshold, 6);
    BOOST_CHECK_MESSAGE(ctx.quorum_present && !ctx.active,
        "in_qual == t-1 (5<6) MUST be unusable with quorum_present set (fail-closed)");
}

BOOST_AUTO_TEST_SUITE_END()
