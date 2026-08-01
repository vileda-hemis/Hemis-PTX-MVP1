// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "bls/bls_wrapper.h"
#include "chain.h"
#include "chainparams.h"
#include "chainparamsbase.h"
#include "evo/deterministicgms.h"
#include "evo/specialtx_validation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "ptx/ptx_accum_script.h"
#include "ptx/ptx_coalesce.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_payout.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_winner_selection.h"
#include "script/script.h"
#include "sync.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

BOOST_FIXTURE_TEST_SUITE(ptx_payout_gen_tests, PTXBeaTestingSetup)

// ---------------------------------------------------------------------------
// Shared helpers (duplicated from ptx_payout_tests.cpp to keep tests self-contained)
// ---------------------------------------------------------------------------

static CScript MakeWinnerScript(uint8_t byte)
{
    CScript s;
    s << OP_DUP << OP_HASH160
      << std::vector<uint8_t>(20, byte)
      << OP_EQUALVERIFY << OP_CHECKSIG;
    return s;
}

static CDeterministicGMCPtr MakeDGM(const std::string& nodeId,
                                      const CScript&     payScript,
                                      const uint256&     proTxHash,
                                      uint64_t           internalId)
{
    auto dgm   = std::make_shared<CDeterministicGM>(internalId);
    dgm->proTxHash          = proTxHash;
    dgm->collateralOutpoint = COutPoint(proTxHash, 0);
    CBLSSecretKey sk; sk.MakeNewKey();
    auto state = std::make_shared<CDeterministicGMState>();
    state->node_id          = nodeId;
    state->scriptPTXPayment = payScript;
    uint160 keyBytes; memcpy(keyBytes.begin(), proTxHash.begin(), 20);
    state->keyIDOwner  = CKeyID(keyBytes);
    state->keyIDVoting = state->keyIDOwner;
    state->pubKeyOperator.Set(sk.GetPublicKey());
    dgm->pdgmState = state;
    return dgm;
}

static CDeterministicGMList MakeGMList(const std::vector<CDeterministicGMCPtr>& gms)
{
    CDeterministicGMList list;
    for (const auto& gm : gms) list.AddGM(gm);
    return list;
}

static void PopulateTracker(const std::vector<std::tuple<std::string, int, bool>>& entries)
{
    g_ptx_pose_tracker.AdvanceLotteryWindow();
    for (const auto& e : entries) {
        const std::string& nid      = std::get<0>(e);
        int                tickets  = std::get<1>(e);
        bool               eligible = std::get<2>(e);
        for (int i = 0; i < tickets; ++i) g_ptx_pose_tracker.RecordHonestParticipation(nid);
        if (!eligible) {
            for (int i = 0; i < 15; ++i) g_ptx_pose_tracker.RecordWithhold(nid);
        }
    }
}

// Run CheckAndApplyPTXPayout with a properly wired pprev (entropy fix).
static std::string RunApplyPayout(const std::vector<CTransactionRef>& txs,
                                   const CDeterministicGMList&          gmList,
                                   int                                  height,
                                   bool                                 fJustCheck = true)
{
    LOCK(cs_main);
    CBlock block;
    block.vtx = txs;
    CValidationState state;

    uint256 prevBlockHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    CBlockIndex dummyPrev;
    dummyPrev.phashBlock = &prevBlockHash;

    uint256 blockHash = uint256S("3333333333333333333333333333333333333333333333333333333333333333");
    CBlockIndex dummyIndex;
    dummyIndex.nHeight    = height;
    dummyIndex.phashBlock = &blockHash;
    dummyIndex.pprev      = &dummyPrev;

    // BUG-024: no coalesce in these harness blocks, so the effective accumulator
    // is the current LotteryState (mirrors ProcessSpecialTxsInBlock's no-coalesce fill).
    if (CheckAndApplyPTXPayout(block, &dummyIndex, gmList, g_ptx_pose_tracker,
                               GetLotteryState().accumulator_outpoint,
                               GetLotteryState().accumulator_value,
                               state, fJustCheck)) return "";
    return state.GetRejectReason();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// PTX_BuildPayoutTx returns a structurally valid PTXPAYOUT when an eligible winner exists.
// This is the structural validity test: checks 1-input / 1-output / correct value / nType=10.
// It does NOT verify the output script against PTX_SelectWinner — that role belongs to
// PayoutGen_GeneratedTxPassesStep8Rules (disagreement detection).
BOOST_AUTO_TEST_CASE(PayoutGen_BuildsValidPayoutWithEligibleWinner)
{
    const CAmount accumValue = 500000;
    const CAmount minerFee   = Params().PTXPayoutMinerFee();
    COutPoint     accumOp(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);

    CScript script01 = MakeWinnerScript(0x01);
    std::string suf  = "dddd4444";
    auto gm01 = MakeDGM("gm01:" + suf, script01,
                         uint256S("0101010101010101010101010101010101010101010101010101010101010101"), 1);
    CDeterministicGMList gmList = MakeGMList({gm01});
    PopulateTracker({{"gm01:" + suf, 5, true}});

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    LotteryState ls;
    {
        LOCK(cs_main);
        ls = GetLotteryState();
    }

    uint256 prevHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    const Optional<CTransactionRef> result = PTX_BuildPayoutTx(
        ls, gmList, g_ptx_pose_tracker, /*blockHeight=*/0, prevHash);

    BOOST_REQUIRE_MESSAGE(result, "PTX_BuildPayoutTx returned nullopt with eligible winner");
    const CTransaction& tx = **result;

    // Structural checks: 1 input, 1 output, correct nType, correct value, empty extraPayload, empty scriptSig.
    BOOST_CHECK(tx.IsPTXPayoutTx());
    BOOST_CHECK_EQUAL(tx.vin.size(), 1u);
    BOOST_CHECK_EQUAL(tx.vout.size(), 1u);
    BOOST_CHECK(tx.vin[0].prevout == accumOp);
    BOOST_CHECK(tx.vin[0].scriptSig.empty());
    BOOST_CHECK_EQUAL(tx.vout[0].nValue, accumValue - minerFee);
    BOOST_REQUIRE(tx.extraPayload);
    BOOST_CHECK(tx.extraPayload->empty());
    // Output must NOT be the accumulator script (P4 structural rule).
    BOOST_CHECK(tx.vout[0].scriptPubKey != GetLotteryAccumScript());
}

// PTX_BuildPayoutTx returns nullopt when no eligible GMs exist (§5.4 rollover).
BOOST_AUTO_TEST_CASE(PayoutGen_ReturnsNulloptForRollover)
{
    COutPoint accumOp(uint256S("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), 0);
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = 500000;
    }

    CDeterministicGMList emptyList;
    PopulateTracker({});

    LotteryState ls;
    {
        LOCK(cs_main);
        ls = GetLotteryState();
    }

    uint256 prevHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    const Optional<CTransactionRef> result = PTX_BuildPayoutTx(
        ls, emptyList, g_ptx_pose_tracker, 0, prevHash);

    BOOST_CHECK_MESSAGE(!result, "PTX_BuildPayoutTx should return nullopt for rollover (no eligible GMs)");
}

// PTX_BuildPayoutTx returns nullopt when accumulator_value < miner fee.
BOOST_AUTO_TEST_CASE(PayoutGen_ReturnsNulloptForTinyAccumulator)
{
    const CAmount minerFee = Params().PTXPayoutMinerFee();
    COutPoint accumOp(uint256S("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"), 0);
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = minerFee - 1;  // too small
    }

    CScript script = MakeWinnerScript(0xAA);
    std::string suf = "eeee5555";
    auto gm = MakeDGM("gm01:" + suf, script,
                       uint256S("0101010101010101010101010101010101010101010101010101010101010101"), 1);
    CDeterministicGMList gmList = MakeGMList({gm});
    PopulateTracker({{"gm01:" + suf, 5, true}});

    LotteryState ls;
    {
        LOCK(cs_main);
        ls = GetLotteryState();
    }

    uint256 prevHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    const Optional<CTransactionRef> result = PTX_BuildPayoutTx(
        ls, gmList, g_ptx_pose_tracker, 0, prevHash);

    BOOST_CHECK_MESSAGE(!result, "PTX_BuildPayoutTx should return nullopt when accumulator < miner fee");
}

// Integration test: generator output passes all Step 8 validator rules (P1–P10).
// Both generator (PTX_BuildPayoutTx) and validator (CheckAndApplyPTXPayout) call
// PTX_SelectWinner — agreement is enforced by shared code path.
// This test catches DISAGREEMENT between generator and validator.
BOOST_AUTO_TEST_CASE(PayoutGen_GeneratedTxPassesStep8Rules)
{
    const CAmount accumValue = 200000;
    COutPoint     accumOp(uint256S("5555555555555555555555555555555555555555555555555555555555555555"), 0);

    CScript script01 = MakeWinnerScript(0x01);
    CScript script02 = MakeWinnerScript(0x02);
    std::string suf  = "ffff6666";
    auto gm01 = MakeDGM("gm01:" + suf, script01,
                         uint256S("0101010101010101010101010101010101010101010101010101010101010101"), 1);
    auto gm02 = MakeDGM("gm02:" + suf, script02,
                         uint256S("0202020202020202020202020202020202020202020202020202020202020202"), 2);
    CDeterministicGMList gmList = MakeGMList({gm01, gm02});
    PopulateTracker({
        {"gm01:" + suf, 10, true},
        {"gm02:" + suf,  5, true},
    });

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    LotteryState ls;
    {
        LOCK(cs_main);
        ls = GetLotteryState();
    }

    // Generator uses prevBlockHash = 222...2 (same as RunApplyPayout's dummyPrev).
    uint256 prevHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    const Optional<CTransactionRef> result = PTX_BuildPayoutTx(
        ls, gmList, g_ptx_pose_tracker, /*blockHeight=*/0, prevHash);
    BOOST_REQUIRE_MESSAGE(result, "PTX_BuildPayoutTx returned nullopt with eligible winners");

    // Validator: RunApplyPayout uses the same prevHash via dummyPrev.
    // P2 (input = accumulator), P5 (output value), P10 (winner script) must all pass.
    // height=0 is % window==0 on ptx-bea (window=5) — a valid settlement boundary.
    BOOST_CHECK_EQUAL(RunApplyPayout({*result}, gmList, 0), "");
}

// Wrong-agreement test: fixture with known DGM/tracker where the §5.3 winner
// is determinable by hand calculation.  Generator output must match.
// This test catches the case where generator AND validator both compute the SAME
// wrong winner — the hardcoded expected winner differs from the shared wrong result.
//
// Fixture: single GM with 1 ticket.  Any entropy → that GM wins (only candidate).
// Expected winner script: MakeWinnerScript(0x77).
BOOST_AUTO_TEST_CASE(PayoutGen_HardcodedWinnerAgreesWithGenerator)
{
    const CAmount accumValue = 300000;
    const CAmount minerFee   = Params().PTXPayoutMinerFee();
    COutPoint     accumOp(uint256S("7777777777777777777777777777777777777777777777777777777777777777"), 0);

    // Single GM — wins by elimination regardless of entropy.
    const CScript expectedScript = MakeWinnerScript(0x77);
    std::string suf = "7777aaaa";
    auto gm = MakeDGM("gm01:" + suf, expectedScript,
                       uint256S("0707070707070707070707070707070707070707070707070707070707070707"), 7);
    CDeterministicGMList gmList = MakeGMList({gm});
    PopulateTracker({{"gm01:" + suf, 1, true}});

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    LotteryState ls;
    {
        LOCK(cs_main);
        ls = GetLotteryState();
    }

    uint256 prevHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    const Optional<CTransactionRef> result = PTX_BuildPayoutTx(
        ls, gmList, g_ptx_pose_tracker, 0, prevHash);
    BOOST_REQUIRE_MESSAGE(result, "PTX_BuildPayoutTx returned nullopt with single eligible GM");

    // Hard-check: output script must be the known expected winner (0x77 script).
    // If generator and validator both compute the same wrong answer, this assertion catches it.
    BOOST_CHECK_MESSAGE((*result)->vout[0].scriptPubKey == expectedScript,
        "Generator output script does not match hardcoded expected winner");
    BOOST_CHECK_EQUAL((*result)->vout[0].nValue, accumValue - minerFee);
}

// P11: settlement-boundary block with eligible winner and no PTXPAYOUT → rejected.
BOOST_AUTO_TEST_CASE(Payout_RolloverGapEnforced)
{
    const CAmount accumValue = 500000;
    COutPoint     accumOp(uint256S("8888888888888888888888888888888888888888888888888888888888888888"), 0);

    CScript script = MakeWinnerScript(0x88);
    std::string suf = "8888bbbb";
    auto gm = MakeDGM("gm01:" + suf, script,
                       uint256S("0808080808080808080808080808080808080808080808080808080808080808"), 8);
    CDeterministicGMList gmList = MakeGMList({gm});
    PopulateTracker({{"gm01:" + suf, 3, true}});

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    // Block at settlement boundary (height=0, window=5 on ptx-bea), no PTXPAYOUT.
    // P11 must reject because PTX_SelectWinner finds an eligible winner.
    BOOST_CHECK_EQUAL(RunApplyPayout({}, gmList, 0), "ptxpayout-missing-at-boundary");
}

// P11 rollover: settlement-boundary block, no eligible GMs → accepted (§5.4 rollover).
BOOST_AUTO_TEST_CASE(Payout_LegitimateRolloverAccepted)
{
    const CAmount accumValue = 500000;
    COutPoint     accumOp(uint256S("9999999999999999999999999999999999999999999999999999999999999999"), 0);

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    // No eligible GMs — rollover is legitimate.
    CDeterministicGMList emptyList;
    PopulateTracker({});

    // Block at settlement boundary, no PTXPAYOUT, no eligible winners → must accept.
    BOOST_CHECK_EQUAL(RunApplyPayout({}, emptyList, 0), "");
}

// ---------------------------------------------------------------------------
// BUG-024: the fJustCheck coalesce→payout ordering asymmetry.
//
// A block carrying BOTH a PTXCOALESCE and a PTXPAYOUT spending the coalesce's
// output (the assembler's product at any settlement boundary with pending
// PTXSESS rolls) must validate IDENTICALLY under fJustCheck=true (TestBlockValidity)
// and fJustCheck=false (connect).  Pre-fix, the coalesce check honoured
// fJustCheck (no apply) while P2 read the raw global — so the producer's own
// TestBlockValidity rejected every such block with ptxpayout-wrong-input while
// connect would have accepted it: valid-on-connect, unbuildable-by-producer,
// a structural liveness wedge at the first boundary × demand coincidence
// (the h720 fleet wedge, 2026-07-29).
// ---------------------------------------------------------------------------

// Mirrors ProcessSpecialTxsInBlock's coalesce→payout EFFECTIVE-ACCUMULATOR
// wiring — the coalesce check fills it, the payout check reads it — which is
// the surface under test.  Like RunApplyPayout above, this harness does not
// call the block-level rule helpers (P8/P9, CheckPTXCoalesceBlockRules); those
// are covered by their own cases and are orthogonal to the fJustCheck
// asymmetry being pinned here.
static std::string RunCoalescePlusPayout(const std::vector<CTransactionRef>& txs,
                                          const CDeterministicGMList&          gmList,
                                          int                                  height,
                                          bool                                 fJustCheck)
{
    LOCK(cs_main);
    CBlock block;
    block.vtx = txs;
    CValidationState state;

    uint256 prevBlockHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    CBlockIndex dummyPrev;
    dummyPrev.phashBlock = &prevBlockHash;

    uint256 blockHash = uint256S("3333333333333333333333333333333333333333333333333333333333333333");
    CBlockIndex dummyIndex;
    dummyIndex.nHeight    = height;
    dummyIndex.phashBlock = &blockHash;
    dummyIndex.pprev      = &dummyPrev;

    COutPoint effAccumOutpoint;
    CAmount   effAccumValue{0};
    if (!CheckAndApplyPTXCoalesce(block, &dummyIndex, state, fJustCheck,
                                  &effAccumOutpoint, &effAccumValue))
        return state.GetRejectReason();
    if (!CheckAndApplyPTXPayout(block, &dummyIndex, gmList, g_ptx_pose_tracker,
                                effAccumOutpoint, effAccumValue, state, fJustCheck))
        return state.GetRejectReason();
    return "";
}

// Build the boundary-with-demand block: coalesce rolling accumulator A forward,
// payout spending the coalesce's own output.  Exactly what CreateNewBlock emits.
static void SetupBug024State(const COutPoint& accumOp, CAmount accumValue,
                              const std::string& suf,
                              CDeterministicGMList& gmListOut,
                              CTransactionRef& coalesceOut,
                              CTransactionRef& payoutOut,
                              int height)
{
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }
    CScript script01 = MakeWinnerScript(0x24);
    auto gm01 = MakeDGM("gm01:" + suf, script01,
                         uint256S("2424242424242424242424242424242424242424242424242424242424242424"), 1);
    gmListOut = MakeGMList({gm01});
    PopulateTracker({{"gm01:" + suf, 5, true}});

    coalesceOut = PTX_BuildCoalesceTx(accumOp, accumValue, {});

    LotteryState tempLs;
    {
        LOCK(cs_main);
        tempLs = GetLotteryState();
    }
    tempLs.accumulator_outpoint = COutPoint(coalesceOut->GetHash(), 0);
    tempLs.accumulator_value    = coalesceOut->vout[0].nValue;

    uint256 prevHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    const Optional<CTransactionRef> payout = PTX_BuildPayoutTx(
        tempLs, gmListOut, g_ptx_pose_tracker, height, prevHash);
    BOOST_REQUIRE_MESSAGE(payout, "PTX_BuildPayoutTx returned nullopt in BUG-024 setup");
    payoutOut = *payout;
}

// ─────────────────────────────────────────────────────────────────────────
// BUG-026 (A) — the POSE-axis fJustCheck asymmetry (the h420 fleet wedge).
//
// ProcessSpecialTxsInBlock used to credit RecordHonestParticipation for THIS
// block's PTXSESS members BEFORE CheckAndApplyPTXPayout.  Under fJustCheck the
// credit is skipped, so the assembler selected on pre-block pose while connect
// selected on post-block pose: different winners, and every settlement boundary
// carrying a roll self-rejected with ptxpayout-wrong-recipient.  The fix is
// ordering — selection strictly precedes the mutation — so both paths read the
// same state by construction.  These cases pin the SELECTION BASIS, which is
// where the divergence actually lives.
// ─────────────────────────────────────────────────────────────────────────

// Build a multi-GM fixture whose winner is ticket-sensitive.
static void SetupBug026Pose(const std::string& suf,
                             CDeterministicGMList& gmListOut,
                             std::vector<std::string>& memberIdsOut)
{
    std::vector<CDeterministicGMCPtr> gms;
    memberIdsOut.clear();
    for (int i = 0; i < 6; ++i) {
        const std::string nid = strprintf("gm%02d:%s", i + 1, suf);
        memberIdsOut.push_back(nid);
        uint256 h; h.SetHex(strprintf("%064x", 0x26000 + i));
        gms.push_back(MakeDGM(nid, MakeWinnerScript((uint8_t)(0x60 + i)), h, i + 1));
    }
    gmListOut = MakeGMList(gms);
    std::vector<std::tuple<std::string, int, bool>> entries;
    for (size_t i = 0; i < memberIdsOut.size(); ++i)
        entries.emplace_back(memberIdsOut[i], (int)i + 1, true);   // 1..6 tickets
    PopulateTracker(entries);
}

// MECHANISM PIN: crediting this block's own participants moves the winner.
// total_tickets is PTX_SelectWinner's modulus, so the credit re-maps the
// entropy — this is why the asymmetry is deterministic, not incidental.
BOOST_AUTO_TEST_CASE(Bug026_PoseCreditMovesWinner_MechanismPin)
{
    CDeterministicGMList gmList;
    std::vector<std::string> members;
    SetupBug026Pose("b0260001", gmList, members);

    const uint256 entropy = uint256S(
        "5151515151515151515151515151515151515151515151515151515151515151");

    const Optional<CScript> before = PTX_SelectWinner(gmList, g_ptx_pose_tracker, entropy);
    BOOST_REQUIRE(before);

    // Simulate the block's own PTXSESS crediting its quorum members.
    for (const std::string& nid : members)
        g_ptx_pose_tracker.RecordHonestParticipation(nid);

    const Optional<CScript> after = PTX_SelectWinner(gmList, g_ptx_pose_tracker, entropy);
    BOOST_REQUIRE(after);

    // Same gmList, same entropy — only pose changed.  If this ever stops
    // differing the scenario no longer discriminates and the two cases below
    // prove nothing; pick different tickets/entropy rather than delete it.
    BOOST_CHECK_MESSAGE(*before != *after,
        "pose credit did not move the winner — fixture no longer discriminates");
}

// RED (the h420 wedge, reproduced): a payout built from PRE-block pose is
// rejected when judged against POST-block pose — exactly ptxpayout-wrong-recipient.
BOOST_AUTO_TEST_CASE(Bug026_PayoutBuiltPreBlock_RejectedAgainstPostBlockPose)
{
    CDeterministicGMList gmList;
    std::vector<std::string> members;
    SetupBug026Pose("b0260002", gmList, members);

    const int height = Params().PTXSettlementWindow();
    const uint256 prevHash = uint256S(
        "2222222222222222222222222222222222222222222222222222222222222222");

    LotteryState ls;
    ls.Reset();
    ls.accumulator_outpoint = COutPoint(uint256S(
        "a026a026a026a026a026a026a026a026a026a026a026a026a026a026a026a026"), 0);
    ls.accumulator_value = 500000;

    // The ASSEMBLER builds against pre-block pose.
    const Optional<CTransactionRef> payout =
        PTX_BuildPayoutTx(ls, gmList, g_ptx_pose_tracker, height, prevHash);
    BOOST_REQUIRE_MESSAGE(payout, "PTX_BuildPayoutTx returned nullopt in BUG-026 setup");

    // Pre-fix connect ordering: credit this block's members FIRST...
    for (const std::string& nid : members)
        g_ptx_pose_tracker.RecordHonestParticipation(nid);

    // ...then judge the payout.  This is the defect.
    LOCK(cs_main);
    GetLotteryState() = ls;
    CBlock block; block.vtx = {*payout};
    CValidationState state;
    uint256 prevBlockHash = prevHash;
    CBlockIndex dummyPrev; dummyPrev.phashBlock = &prevBlockHash;
    uint256 blockHash = uint256S(
        "3333333333333333333333333333333333333333333333333333333333333333");
    CBlockIndex dummyIndex;
    dummyIndex.nHeight = height;
    dummyIndex.phashBlock = &blockHash;
    dummyIndex.pprev = &dummyPrev;

    const bool ok = CheckAndApplyPTXPayout(block, &dummyIndex, gmList, g_ptx_pose_tracker,
                                           ls.accumulator_outpoint, ls.accumulator_value,
                                           state, /*fJustCheck=*/true);
    BOOST_CHECK_MESSAGE(!ok, "post-block pose accepted a pre-block payout — "
                             "fixture no longer reproduces the h420 wedge");
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxpayout-wrong-recipient");
}

// GREEN (what the fix guarantees by ordering): the SAME payout is accepted when
// judged against the pre-block pose the assembler used.  Selection basis is the
// only variable between this case and the one above.
BOOST_AUTO_TEST_CASE(Bug026_PayoutBuiltPreBlock_AcceptsAgainstPreBlockPose)
{
    CDeterministicGMList gmList;
    std::vector<std::string> members;
    SetupBug026Pose("b0260003", gmList, members);

    const int height = Params().PTXSettlementWindow();
    const uint256 prevHash = uint256S(
        "2222222222222222222222222222222222222222222222222222222222222222");

    LotteryState ls;
    ls.Reset();
    ls.accumulator_outpoint = COutPoint(uint256S(
        "a026a026a026a026a026a026a026a026a026a026a026a026a026a026a026a026"), 0);
    ls.accumulator_value = 500000;

    const Optional<CTransactionRef> payout =
        PTX_BuildPayoutTx(ls, gmList, g_ptx_pose_tracker, height, prevHash);
    BOOST_REQUIRE(payout);

    // NO credit before the check — the post-fix ordering.
    LOCK(cs_main);
    GetLotteryState() = ls;
    CBlock block; block.vtx = {*payout};
    CValidationState state;
    uint256 prevBlockHash = prevHash;
    CBlockIndex dummyPrev; dummyPrev.phashBlock = &prevBlockHash;
    uint256 blockHash = uint256S(
        "3333333333333333333333333333333333333333333333333333333333333333");
    CBlockIndex dummyIndex;
    dummyIndex.nHeight = height;
    dummyIndex.phashBlock = &blockHash;
    dummyIndex.pprev = &dummyPrev;

    BOOST_CHECK_MESSAGE(CheckAndApplyPTXPayout(block, &dummyIndex, gmList, g_ptx_pose_tracker,
                                               ls.accumulator_outpoint, ls.accumulator_value,
                                               state, /*fJustCheck=*/true),
                        "pre-block pose rejected its own payout: " + state.GetRejectReason());
}

// BUG-026 (B): the rollback primitive the failure sentry relies on.  A block
// that fails to connect must leave the tracker exactly as it found it.
BOOST_AUTO_TEST_CASE(Bug026_RestoreRecords_RoundTrips)
{
    CDeterministicGMList gmList;
    std::vector<std::string> members;
    SetupBug026Pose("b0260004", gmList, members);

    // PTXNodeRecord has no operator==, so compare the fields that matter to
    // winner selection (tickets, score, eligibility) node by node.
    auto sameRecords = [](const std::map<std::string, PTXNodeRecord>& a,
                          const std::map<std::string, PTXNodeRecord>& b) {
        if (a.size() != b.size()) return false;
        for (const auto& kv : a) {
            auto it = b.find(kv.first);
            if (it == b.end()) return false;
            if (it->second.lottery_tickets != kv.second.lottery_tickets) return false;
            if (it->second.pose_score      != kv.second.pose_score) return false;
            if (it->second.quorum_eligible != kv.second.quorum_eligible) return false;
        }
        return true;
    };

    const auto saved = g_ptx_pose_tracker.GetAllRecords();
    for (const std::string& nid : members)
        g_ptx_pose_tracker.RecordHonestParticipation(nid);
    BOOST_REQUIRE_MESSAGE(!sameRecords(g_ptx_pose_tracker.GetAllRecords(), saved),
                          "credit did not mutate the tracker — fixture is inert");

    g_ptx_pose_tracker.RestoreRecords(saved);
    BOOST_CHECK_MESSAGE(sameRecords(g_ptx_pose_tracker.GetAllRecords(), saved),
                        "RestoreRecords did not restore the pre-mutation record set");

    // ★ PERSISTENCE half (the h480 partition's lesson): RecordHonestParticipation
    // Save()s every credit, so a rejected block has already written its credits to
    // ptx_pose.dat.  If the rollback does not reach the FILE, the leak returns at
    // the next restart and the node diverges from its peers forever.  Reload from
    // disk and require the restored values — an in-memory-only rollback fails here.
    PTXPoSeTracker reloaded;
    reloaded.Load();
    BOOST_CHECK_MESSAGE(sameRecords(reloaded.GetAllRecords(), saved),
                        "rollback did not reach ptx_pose.dat — the flat file still "
                        "carries the rejected block's credits (h480 partition shape)");
}

// ---------------------------------------------------------------------------
// BUG-027 / ODC-056(c) — pose must be REVERSED on disconnect, not just on a
// failed connect.
//
// BUG-026 [B] covered the block that FAILS to connect.  This covers the block
// that connects SUCCESSFULLY and is later DISCONNECTED by a reorg — a path
// [B]'s sentry never sees, because the sentry committed.  Before the fix,
// UndoSpecialTxsInBlock restored LotteryState and touched pose nowhere, so
// credits accumulated once per connect and were never removed: pose became
// MONOTONIC across reorgs while the accumulator stayed transactional.
//
// MEASURED on the Phase-2 fleet (this is a regression test for an observed
// failure, not a hypothetical): at total_rolls=2, expected tickets 11*2 = 22,
// nodes read 22 / 33 / 44 / 88 strictly in proportion to their chain-switch
// count — gm01 had 48 tip-height regressions and 88 tickets; gm03 had 14 and
// 22.  Divergent pose picked a different settlement winner, rejecting the h300
// boundary; and because wallet-less GMs cannot stake, the stranded nodes WERE
// the producer set, fragmenting stake until the majority chain held 0.4% of
// weight and the chain HALTED.
//
// THE ASSERTION IS THE INVARIANT, NOT THE SYMPTOM: after connect-then-
// disconnect, pose must equal its pre-block value — NOT a value scaled by how
// many times the block was connected.  The loop below connects and disconnects
// the same credits repeatedly; a monotonic tracker grows every iteration.
BOOST_AUTO_TEST_CASE(Bug027_PoseReversedOnDisconnect_NotMonotonicAcrossReorgs)
{
    CDeterministicGMList gmList;
    std::vector<std::string> members;
    SetupBug026Pose("b0270001", gmList, members);

    auto ticketSum = [](const std::map<std::string, PTXNodeRecord>& m) {
        int64_t t = 0;
        for (const auto& kv : m) t += kv.second.lottery_tickets;
        return t;
    };

    uint256 prevHash = uint256S("2727272727272727272727272727272727272727"
                                "272727272727272727272727");
    LOCK(cs_main);

    // Pre-block state + the pprev snapshot the undo path reads.
    const auto preRecords = g_ptx_pose_tracker.GetAllRecords();
    const int64_t preTickets = ticketSum(preRecords);
    WritePoseSnapshotForBlock(prevHash, preRecords);

    // Round-trip the same block's credits several times. Each iteration is one
    // connect + one disconnect — exactly what a chain switch does.
    for (int round = 0; round < 4; ++round) {
        for (const std::string& nid : members)
            g_ptx_pose_tracker.RecordHonestParticipation(nid);

        BOOST_REQUIRE_MESSAGE(
            ticketSum(g_ptx_pose_tracker.GetAllRecords()) > preTickets,
            "credit did not mutate pose — fixture is inert, the test would pass "
            "vacuously");

        // The undo arm under test: restore from the pprev snapshot.
        std::map<std::string, PTXNodeRecord> restored;
        BOOST_REQUIRE_MESSAGE(ReadPoseSnapshotForBlock(prevHash, restored),
                              "pose snapshot missing for pprev — the connect path "
                              "must write one for EVERY block or disconnect cannot "
                              "restore");
        g_ptx_pose_tracker.RestoreRecords(std::move(restored));

        // ★ THE BUG-027 ASSERTION. Pre-fix this grows by |members| per round
        // (22 -> 33 -> 44 -> ... the measured 88); post-fix it is invariant.
        BOOST_CHECK_MESSAGE(
            ticketSum(g_ptx_pose_tracker.GetAllRecords()) == preTickets,
            "pose is MONOTONIC across reorg round " + std::to_string(round) +
            ": tickets " +
            std::to_string(ticketSum(g_ptx_pose_tracker.GetAllRecords())) +
            " != pre-block " + std::to_string(preTickets) +
            " — disconnect did not reverse the credits (ODC-056 leg 2)");
    }

    // Durability: the reversal must reach ptx_pose.dat, or it returns at restart
    // — the same half BUG-026 [B] had to learn.
    PTXPoSeTracker reloaded;
    reloaded.Load();
    BOOST_CHECK_MESSAGE(ticketSum(reloaded.GetAllRecords()) == preTickets,
                        "reorg reversal did not reach ptx_pose.dat");
}

// The fix: coalesce+payout validates under fJustCheck=true (TestBlockValidity path).
BOOST_AUTO_TEST_CASE(Bug024_CoalescePlusPayout_AcceptsUnderJustCheck)
{
    const CAmount accumValue = 500000;
    COutPoint accumOp(uint256S("a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024"), 0);
    CDeterministicGMList gmList;
    CTransactionRef coalesceTx, payoutTx;
    const int height = Params().PTXSettlementWindow();
    SetupBug024State(accumOp, accumValue, "b0240001", gmList, coalesceTx, payoutTx, height);

    BOOST_CHECK_EQUAL(RunCoalescePlusPayout({coalesceTx, payoutTx}, gmList, height,
                                            /*fJustCheck=*/true), "");
}

// Connect parity control: the same block under fJustCheck=false also accepts,
// and the apply advances the global to the coalesce's output.
BOOST_AUTO_TEST_CASE(Bug024_CoalescePlusPayout_ConnectParity)
{
    const CAmount accumValue = 500000;
    COutPoint accumOp(uint256S("a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024"), 0);
    CDeterministicGMList gmList;
    CTransactionRef coalesceTx, payoutTx;
    const int height = Params().PTXSettlementWindow();
    SetupBug024State(accumOp, accumValue, "b0240002", gmList, coalesceTx, payoutTx, height);

    BOOST_CHECK_EQUAL(RunCoalescePlusPayout({coalesceTx, payoutTx}, gmList, height,
                                            /*fJustCheck=*/false), "");
    LOCK(cs_main);
    // Payout apply resets the accumulator (winner took it) — connect semantics.
    BOOST_CHECK(GetLotteryState().accumulator_outpoint.IsNull());
}

// The hole, reproduced: feed the payout check the RAW GLOBAL (the pre-fix read)
// instead of the effective accumulator → ptxpayout-wrong-input under fJustCheck.
// If this limb ever stops failing-the-old-way, the scenario no longer
// discriminates and the two limbs above prove nothing — keep all three.
BOOST_AUTO_TEST_CASE(Bug024_RawGlobalRead_IsTheBug)
{
    const CAmount accumValue = 500000;
    COutPoint accumOp(uint256S("a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024"), 0);
    CDeterministicGMList gmList;
    CTransactionRef coalesceTx, payoutTx;
    const int height = Params().PTXSettlementWindow();
    SetupBug024State(accumOp, accumValue, "b0240003", gmList, coalesceTx, payoutTx, height);

    LOCK(cs_main);
    CBlock block;
    block.vtx = {coalesceTx, payoutTx};
    CValidationState state;
    uint256 prevBlockHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    CBlockIndex dummyPrev;  dummyPrev.phashBlock = &prevBlockHash;
    uint256 blockHash = uint256S("3333333333333333333333333333333333333333333333333333333333333333");
    CBlockIndex dummyIndex; dummyIndex.nHeight = height;
    dummyIndex.phashBlock = &blockHash;  dummyIndex.pprev = &dummyPrev;

    COutPoint effOp; CAmount effVal{0};
    BOOST_REQUIRE(CheckAndApplyPTXCoalesce(block, &dummyIndex, state, /*fJustCheck=*/true,
                                           &effOp, &effVal));
    // Pre-fix behaviour: P2 against the un-advanced global.
    BOOST_CHECK(!CheckAndApplyPTXPayout(block, &dummyIndex, gmList, g_ptx_pose_tracker,
                                        GetLotteryState().accumulator_outpoint,
                                        GetLotteryState().accumulator_value,
                                        state, /*fJustCheck=*/true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxpayout-wrong-input");
}

// Discrimination pin: the fix must NOT weaken P2 — a payout spending the STALE
// pre-coalesce accumulator in a coalesce-carrying block still rejects, under
// both fJustCheck values.
BOOST_AUTO_TEST_CASE(Bug024_StalePayoutStillRejects)
{
    const CAmount accumValue = 500000;
    COutPoint accumOp(uint256S("a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024"), 0);
    CDeterministicGMList gmList;
    CTransactionRef coalesceTx, payoutTx;
    const int height = Params().PTXSettlementWindow();
    SetupBug024State(accumOp, accumValue, "b0240004", gmList, coalesceTx, payoutTx, height);

    // Stale payout: built against the PRE-coalesce accumulator.
    LotteryState staleLs;
    {
        LOCK(cs_main);
        staleLs = GetLotteryState();
    }
    uint256 prevHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    const Optional<CTransactionRef> stale = PTX_BuildPayoutTx(
        staleLs, gmList, g_ptx_pose_tracker, height, prevHash);
    BOOST_REQUIRE(stale);

    BOOST_CHECK_EQUAL(RunCoalescePlusPayout({coalesceTx, *stale}, gmList, height,
                                            /*fJustCheck=*/true), "ptxpayout-wrong-input");
    BOOST_CHECK_EQUAL(RunCoalescePlusPayout({coalesceTx, *stale}, gmList, height,
                                            /*fJustCheck=*/false), "ptxpayout-wrong-input");
}

// P11 reads the effective accumulator too: a boundary block that coalesces but
// omits the owed payout rejects with missing-at-boundary under fJustCheck.
BOOST_AUTO_TEST_CASE(Bug024_CoalesceWithoutPayout_StillOwesPayout)
{
    const CAmount accumValue = 500000;
    COutPoint accumOp(uint256S("a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024"), 0);
    CDeterministicGMList gmList;
    CTransactionRef coalesceTx, payoutTx;
    const int height = Params().PTXSettlementWindow();
    SetupBug024State(accumOp, accumValue, "b0240005", gmList, coalesceTx, payoutTx, height);

    BOOST_CHECK_EQUAL(RunCoalescePlusPayout({coalesceTx}, gmList, height,
                                            /*fJustCheck=*/true), "ptxpayout-missing-at-boundary");
}

// No-coalesce path: the effective accumulator degenerates to the current global
// (the fill every payout-only block rides).
BOOST_AUTO_TEST_CASE(Bug024_NoCoalesce_EffectiveIsGlobal)
{
    COutPoint accumOp(uint256S("a024a024a024a024a024a024a024a024a024a024a024a024a024a024a024a029"), 0);
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = 123456;
    }
    LOCK(cs_main);
    CBlock block;  // empty — no coalesce
    CValidationState state;
    uint256 blockHash = uint256S("3333333333333333333333333333333333333333333333333333333333333333");
    CBlockIndex idx; idx.nHeight = 5; idx.phashBlock = &blockHash; idx.pprev = nullptr;

    COutPoint effOp; CAmount effVal{0};
    BOOST_REQUIRE(CheckAndApplyPTXCoalesce(block, &idx, state, /*fJustCheck=*/true,
                                           &effOp, &effVal));
    BOOST_CHECK(effOp == accumOp);
    BOOST_CHECK_EQUAL(effVal, 123456);
}

// ---------------------------------------------------------------------------
// BUG-023 × BUG-024 COMPOSITION: the two defects sit at DIFFERENT layers, so
// BUG-024's fix alone cannot rescue a node whose lottery state was clobbered.
//
// Sequence on the wedged fleet: h717 carried a PTXCOALESCE spending accumulator
// A and producing B.  A restart then ran CVerifyDB's level-3 walk over the last
// 6 blocks (717 ∈ window) and regressed the live global from B back to A —
// BUG-023.  Every node then held A while the chain held B.
//
// This case pins what that regressed node does with a block a HEALTHY producer
// builds (coalesce spending B, payout spending the coalesce's output): it is
// rejected at the COALESCE check — before the payout checks BUG-024 repairs are
// ever reached.  So the effective-accumulator fix is necessary but not
// sufficient; the two fixes compose rather than overlap, and a fleet carrying
// only BUG-024 would still have been wedged.  ★ This is also the reason the
// 3b5d27b revert alone did not unwedge the fleet.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Bug023x024_ClobberedGlobalRejectsAtCoalesceNotPayout)
{
    // B — the accumulator the healthy chain actually holds after h717.
    const CAmount valueB = 500000;
    COutPoint opB(uint256S("b023b023b023b023b023b023b023b023b023b023b023b023b023b023b023b023"), 0);
    // A — the pre-coalesce accumulator a clobbered node regresses to.
    COutPoint opA(uint256S("a023a023a023a023a023a023a023a023a023a023a023a023a023a023a023a023"), 0);

    CDeterministicGMList gmList;
    CTransactionRef coalesceTx, payoutTx;
    const int height = Params().PTXSettlementWindow();

    // Build the healthy producer's block against B.
    SetupBug024State(opB, valueB, "b0230001", gmList, coalesceTx, payoutTx, height);

    // Sanity: on a HEALTHY node (global == B) the block validates under
    // fJustCheck — i.e. the only thing wrong below is the clobber.
    BOOST_REQUIRE_EQUAL(RunCoalescePlusPayout({coalesceTx, payoutTx}, gmList, height,
                                              /*fJustCheck=*/true), "");

    // Now clobber: regress the live global to A, exactly as the VerifyDB walk did.
    {
        LOCK(cs_main);
        GetLotteryState().accumulator_outpoint = opA;
        GetLotteryState().accumulator_value    = valueB;
    }

    // The same, valid, block now fails — and it fails at the COALESCE input
    // check, NOT at a payout check.  BUG-024's effective-accumulator threading
    // is downstream of this and cannot help.
    BOOST_CHECK_EQUAL(RunCoalescePlusPayout({coalesceTx, payoutTx}, gmList, height,
                                            /*fJustCheck=*/true),
                      "ptxcoalesce-wrong-input");
}

BOOST_AUTO_TEST_SUITE_END()
