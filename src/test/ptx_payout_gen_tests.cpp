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

    if (CheckAndApplyPTXPayout(block, &dummyIndex, gmList, g_ptx_pose_tracker, state, fJustCheck)) return "";
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

BOOST_AUTO_TEST_SUITE_END()
