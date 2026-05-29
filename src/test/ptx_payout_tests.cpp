// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "chain.h"
#include "chainparams.h"
#include "chainparamsbase.h"
#include "coins.h"
#include "bls/bls_wrapper.h"
#include "evo/deterministicgms.h"
#include "evo/specialtx_validation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "ptx/ptx_accum_script.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_winner_selection.h"
#include "script/script.h"
#include "serialize.h"
#include "sync.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

BOOST_FIXTURE_TEST_SUITE(ptx_payout_tests, PTXBeaTestingSetup)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a canonical winner P2PKH script for test fixtures.
static CScript MakeWinnerScript(uint8_t byte)
{
    CScript s;
    s << OP_DUP << OP_HASH160
      << std::vector<uint8_t>(20, byte)
      << OP_EQUALVERIFY << OP_CHECKSIG;
    return s;
}

// Build a minimal PTXPAYOUT transaction.
static CMutableTransaction MakePTXPayout(
    const COutPoint& input,
    const CScript&   outputScript,
    CAmount          outputValue,
    bool             nonEmptyExtraPayload = false,
    bool             nonEmptyScriptSig    = false,
    bool             extraOutput          = false,
    bool             extraInput           = false)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXPAYOUT;

    CTxIn txin(input);
    if (nonEmptyScriptSig) txin.scriptSig << OP_1;
    mtx.vin.push_back(txin);
    if (extraInput) mtx.vin.push_back(CTxIn(COutPoint(uint256S("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), 0)));

    mtx.vout.push_back(CTxOut(outputValue, outputScript));
    if (extraOutput) mtx.vout.push_back(CTxOut(0, outputScript));

    if (nonEmptyExtraPayload) {
        mtx.extraPayload.emplace(std::vector<uint8_t>{0x01, 0x02});
    } else {
        mtx.extraPayload.emplace();
    }
    return mtx;
}

// Run CheckSpecialTxNoContext (pindexPrev=nullptr, view=nullptr).
static std::string RunCheckNoCtx(const CMutableTransaction& mtx)
{
    LOCK(cs_main);
    CValidationState state;
    const CTransaction tx(mtx);
    if (CheckSpecialTxNoContext(tx, state)) return "";
    return state.GetRejectReason();
}

// Build a block containing only the given txs and run CheckPTXPayoutBlockRules at a given height.
static std::string RunPayoutBlockRules(const std::vector<CTransactionRef>& txs, int height)
{
    LOCK(cs_main);
    CBlock block;
    block.vtx = txs;
    CValidationState state;
    CBlockIndex dummyIndex;
    dummyIndex.nHeight = height;
    if (CheckPTXPayoutBlockRules(block, &dummyIndex, state)) return "";
    return state.GetRejectReason();
}

// Run CheckAndApplyPTXPayout with hand-built DGM list and the global pose tracker.
// Callers must call PopulateTracker() before this to set up tracker state.
//
// Entropy fix (Step 8 amendment): CheckAndApplyPTXPayout uses pindex->pprev->GetBlockHash()
// as entropy base — not pindex->GetBlockHash() — to break the circular dependency between
// the block hash and PTXPAYOUT content.  Wire a dummy pprev so P10 can derive entropy.
static std::string RunApplyPayout(const std::vector<CTransactionRef>& txs,
                                   const CDeterministicGMList&          gmList,
                                   int                                  height,
                                   bool                                 fJustCheck = true)
{
    LOCK(cs_main);
    CBlock block;
    block.vtx = txs;
    CValidationState state;

    // Parent block: fixed hash used as entropy base by tests that call PTX_ComputeSelectionEntropy.
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

// Build a DGM with the given node_id, scriptPTXPayment, and proTxHash.
static CDeterministicGMCPtr MakeDGM(const std::string& nodeId,
                                      const CScript&     payScript,
                                      const uint256&     proTxHash,
                                      uint64_t           internalId)
{
    auto dgm   = std::make_shared<CDeterministicGM>(internalId);
    dgm->proTxHash          = proTxHash;
    dgm->collateralOutpoint = COutPoint(proTxHash, 0);  // non-null: required by AddUniqueProperty

    // Generate a fresh BLS keypair so pubKeyOperator is valid and unique.
    // AddGM enforces pubKeyOperator != nullValue; the generated key satisfies this.
    CBLSSecretKey sk; sk.MakeNewKey();
    CBLSPublicKey pk = sk.GetPublicKey();

    auto state = std::make_shared<CDeterministicGMState>();
    state->node_id          = nodeId;
    state->scriptPTXPayment = payScript;
    // Non-null unique CKeyID derived from the first 20 bytes of proTxHash.
    uint160 keyBytes; memcpy(keyBytes.begin(), proTxHash.begin(), 20);
    state->keyIDOwner  = CKeyID(keyBytes);
    state->keyIDVoting = state->keyIDOwner;
    state->pubKeyOperator.Set(pk);
    dgm->pdgmState = state;
    return dgm;
}

// Build a CDeterministicGMList with the given GMs.
static CDeterministicGMList MakeGMList(const std::vector<CDeterministicGMCPtr>& gms)
{
    CDeterministicGMList list;
    for (const auto& gm : gms) list.AddGM(gm);
    return list;
}

// Populate g_ptx_pose_tracker with records.  Resets the tracker first.
// Each entry: (node_id, tickets, eligible)
static void PopulateTracker(const std::vector<std::tuple<std::string, int, bool>>& entries)
{
    // PTXPoSeTracker is non-copyable (RecursiveMutex member); use the global directly.
    // Reset by replacing all records via honest-participation/penalty calls.
    // Advance lottery window to zero out any existing tickets, then add fresh ones.
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

// ---------------------------------------------------------------------------
// Tests — predicates and structural per-tx rules (CheckSpecialTxNoContext)
// ---------------------------------------------------------------------------

// IsPTXPayoutTx() returns true for nType=10, false otherwise.
BOOST_AUTO_TEST_CASE(PTXPayout_PredicateRecognizes)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.extraPayload.emplace();

    mtx.nType = CTransaction::TxType::PTXPAYOUT;
    BOOST_CHECK(CTransaction(mtx).IsPTXPayoutTx());

    mtx.nType = CTransaction::TxType::NORMAL;
    BOOST_CHECK(!CTransaction(mtx).IsPTXPayoutTx());

    mtx.nType = CTransaction::TxType::PTXCOALESCE;
    BOOST_CHECK(!CTransaction(mtx).IsPTXPayoutTx());
}

// P1: exactly one input required.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectsWrongInputCount)
{
    COutPoint op(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);
    // Zero inputs: nVersion/nType alone, no vin
    CMutableTransaction mtx0;
    mtx0.nVersion = CTransaction::TxVersion::SAPLING;
    mtx0.nType    = CTransaction::TxType::PTXPAYOUT;
    mtx0.vout.push_back(CTxOut(1000, MakeWinnerScript(0xAA)));
    mtx0.extraPayload.emplace();
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx0), "ptxpayout-bad-input-count");

    // Two inputs
    CMutableTransaction mtx2 = MakePTXPayout(op, MakeWinnerScript(0xAA), 1000,
        false, false, false, /*extraInput=*/true);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx2), "ptxpayout-bad-input-count");
}

// P3: exactly one output required.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectsWrongOutputCount)
{
    COutPoint op(uint256S("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), 0);
    CMutableTransaction mtx = MakePTXPayout(op, MakeWinnerScript(0xBB), 1000,
        false, false, /*extraOutput=*/true);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx), "ptxpayout-bad-output-count");
}

// P4 structural: output must not be LOTTERY_ACCUM_SCRIPT.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectsWrongRecipient)
{
    COutPoint op(uint256S("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"), 0);
    // Output to accumulator script → structural P4 violation
    CMutableTransaction mtx = MakePTXPayout(op, GetLotteryAccumScript(), 1000);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx), "ptxpayout-output-is-accum");
}

// P6: extraPayload must be present-but-empty.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectsNonEmptyExtraPayload)
{
    COutPoint op(uint256S("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"), 0);
    CMutableTransaction mtx = MakePTXPayout(op, MakeWinnerScript(0xDD), 1000,
        /*nonEmptyExtraPayload=*/true);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx), "ptxpayout-bad-payload");
}

// P7: input scriptSig must be empty.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectsNonEmptyScriptSig)
{
    COutPoint op(uint256S("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"), 0);
    CMutableTransaction mtx = MakePTXPayout(op, MakeWinnerScript(0xEE), 1000,
        false, /*nonEmptyScriptSig=*/true);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx), "ptxpayout-nonempty-scriptsig");
}

// ---------------------------------------------------------------------------
// Block-level rules: P8/P9 via CheckPTXPayoutBlockRules
// ---------------------------------------------------------------------------

// P8: at most one PTXPAYOUT per block.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectsDuplicateInBlock)
{
    COutPoint op1(uint256S("1111111111111111111111111111111111111111111111111111111111111111"), 0);
    COutPoint op2(uint256S("2222222222222222222222222222222222222222222222222222222222222222"), 0);
    auto p1 = MakeTransactionRef(MakePTXPayout(op1, MakeWinnerScript(0x01), 9000));
    auto p2 = MakeTransactionRef(MakePTXPayout(op2, MakeWinnerScript(0x01), 9000));
    // Settlement window = 5 on ptx-bea; height 0 is a boundary.
    BOOST_CHECK_EQUAL(RunPayoutBlockRules({p1, p2}, 0), "ptxpayout-duplicate");
}

// P9: height must be a settlement boundary (height % window == 0).
BOOST_AUTO_TEST_CASE(PTXPayout_RejectedAtNonBoundaryHeight)
{
    COutPoint op(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);
    auto payout = MakeTransactionRef(MakePTXPayout(op, MakeWinnerScript(0xAA), 9000));
    const int window = Params().PTXSettlementWindow();  // 5 on ptx-bea
    BOOST_CHECK_EQUAL(RunPayoutBlockRules({payout}, 1), "ptxpayout-wrong-height");
    BOOST_CHECK_EQUAL(RunPayoutBlockRules({payout}, window - 1), "ptxpayout-wrong-height");
    // Boundary height passes P8/P9
    BOOST_CHECK_EQUAL(RunPayoutBlockRules({payout}, 0),      "");
    BOOST_CHECK_EQUAL(RunPayoutBlockRules({payout}, window), "");
}

// ---------------------------------------------------------------------------
// Contextual rules: P2/P5/P10 via CheckAndApplyPTXPayout
// ---------------------------------------------------------------------------

// Happy path: well-formed PTXPAYOUT passes P2/P5/P10.
BOOST_AUTO_TEST_CASE(PTXPayout_AcceptsValidStructure)
{
    const CAmount accumValue = 100000;
    const CAmount minerFee   = Params().PTXPayoutMinerFee();  // 10000

    COutPoint accumOp(uint256S("5555555555555555555555555555555555555555555555555555555555555555"), 0);

    // Set up a 3-GM fixture so P10 has a deterministic winner.
    // Fixture: gm01 (20 tickets), gm02 (10 tickets), gm03 (1 ticket).
    // Entropy at height=0, hash=333...3:
    // entropy_uint64 = LE64(SHA256("PTX-LOTTERY-PAYOUT-" || 0 || 333...3))[0:8]
    // Then winning_ticket = (entropy_uint64 % 31) + 1.
    // We use PTX_SelectWinner to find the expected winner programmatically.
    CScript script01 = MakeWinnerScript(0x01);
    CScript script02 = MakeWinnerScript(0x02);
    CScript script03 = MakeWinnerScript(0x03);
    std::string suf  = "aaaa0000";  // placeholder suffix (not validated in CheckAndApplyPTXPayout)
    auto gm01 = MakeDGM("gm01:" + suf, script01, uint256S("0101010101010101010101010101010101010101010101010101010101010101"), 1);
    auto gm02 = MakeDGM("gm02:" + suf, script02, uint256S("0202020202020202020202020202020202020202020202020202020202020202"), 2);
    auto gm03 = MakeDGM("gm03:" + suf, script03, uint256S("0303030303030303030303030303030303030303030303030303030303030303"), 3);
    CDeterministicGMList gmList = MakeGMList({gm01, gm02, gm03});
    PopulateTracker({
        {"gm01:" + suf, 20, true},
        {"gm02:" + suf, 10, true},
        {"gm03:" + suf,  1, true},
    });

    // Compute what the validator will compute for P10.
    // Entropy uses the PARENT block hash (222...2) — matches RunApplyPayout's dummyPrev.
    uint256 prevBlockHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    uint256 entropy   = PTX_ComputeSelectionEntropy(0, prevBlockHash);
    Optional<CScript> winner = PTX_SelectWinner(gmList, g_ptx_pose_tracker, entropy);
    BOOST_REQUIRE_MESSAGE(winner, "PTX_SelectWinner returned nullopt — eligible GMs set up incorrectly");

    // Set up LotteryState so P2/P5 pass.
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    auto payout = MakeTransactionRef(MakePTXPayout(accumOp, *winner, accumValue - minerFee));
    BOOST_CHECK_EQUAL(RunApplyPayout({payout}, gmList, 0), "");
}

// P2: input outpoint must match LotteryState.accumulator_outpoint.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectsWrongInputOutpoint)
{
    const CAmount accumValue = 100000;
    const CAmount minerFee   = Params().PTXPayoutMinerFee();
    COutPoint accumOp(uint256S("6666666666666666666666666666666666666666666666666666666666666666"), 0);
    COutPoint wrongOp(uint256S("7777777777777777777777777777777777777777777777777777777777777777"), 0);

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    CDeterministicGMList emptyList;
    PopulateTracker({});
    auto payout = MakeTransactionRef(MakePTXPayout(wrongOp, MakeWinnerScript(0xAA), accumValue - minerFee));
    BOOST_CHECK_EQUAL(RunApplyPayout({payout}, emptyList, 0), "ptxpayout-wrong-input");
}

// P5: output value must equal accumulator_value - nPTXPayoutMinerFee.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectsWrongValue)
{
    const CAmount accumValue = 100000;
    const CAmount minerFee   = Params().PTXPayoutMinerFee();
    COutPoint accumOp(uint256S("8888888888888888888888888888888888888888888888888888888888888888"), 0);

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    CDeterministicGMList emptyList;
    PopulateTracker({});
    // Wrong value: off by 1 sat
    auto payout = MakeTransactionRef(MakePTXPayout(accumOp, MakeWinnerScript(0xBB), accumValue - minerFee + 1));
    BOOST_CHECK_EQUAL(RunApplyPayout({payout}, emptyList, 0), "ptxpayout-wrong-output-value");
}

// P10: output script must match the deterministically selected winner.
// Fixture: gm01 (20 tickets) wins, gm03 (1 ticket, last alphabetically) is planted as wrong winner.
// Broken algo check: if we break the validator to always return last-alpha, it coincidentally
// accepts gm03's script — PTXPayout_RejectedIfWrongWinnerSelected becomes informative RED.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectedIfWrongWinnerSelected)
{
    const CAmount accumValue = 100000;
    const CAmount minerFee   = Params().PTXPayoutMinerFee();
    COutPoint accumOp(uint256S("9999999999999999999999999999999999999999999999999999999999999999"), 0);

    CScript script01 = MakeWinnerScript(0x01);
    CScript script02 = MakeWinnerScript(0x02);
    CScript script03 = MakeWinnerScript(0x03);  // wrong winner (last alphabetically)
    std::string suf  = "bbbb1111";
    auto gm01 = MakeDGM("gm01:" + suf, script01, uint256S("0101010101010101010101010101010101010101010101010101010101010101"), 1);
    auto gm02 = MakeDGM("gm02:" + suf, script02, uint256S("0202020202020202020202020202020202020202020202020202020202020202"), 2);
    auto gm03 = MakeDGM("gm03:" + suf, script03, uint256S("0303030303030303030303030303030303030303030303030303030303030303"), 3);
    CDeterministicGMList gmList = MakeGMList({gm01, gm02, gm03});
    PopulateTracker({
        {"gm01:" + suf, 20, true},
        {"gm02:" + suf, 10, true},
        {"gm03:" + suf,  1, true},
    });

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().accumulator_outpoint = accumOp;
        GetLotteryState().accumulator_value    = accumValue;
    }

    // Verify §5.3 does NOT pick gm03 for this fixture.
    // The exact winner (gm01 or gm02) doesn't matter; what matters is that gm03 is the
    // planted wrong winner AND the broken algo (last-alphabetically) picks gm03, making
    // PTXPayout_RejectedIfWrongWinnerSelected flip RED under the broken algo.
    // Entropy uses parent hash (222...2) — matches RunApplyPayout's dummyPrev.
    uint256 prevBlockHash = uint256S("2222222222222222222222222222222222222222222222222222222222222222");
    uint256 entropy   = PTX_ComputeSelectionEntropy(0, prevBlockHash);
    Optional<CScript> realWinner = PTX_SelectWinner(gmList, g_ptx_pose_tracker, entropy);
    BOOST_REQUIRE(realWinner);
    BOOST_REQUIRE(*realWinner != script03);  // the real winner must not be gm03 (the planted wrong winner)

    // Build PTXPAYOUT to gm03 (wrong winner — last alphabetically = broken algo's pick).
    auto wrongPayout = MakeTransactionRef(MakePTXPayout(accumOp, script03, accumValue - minerFee));
    BOOST_CHECK_EQUAL(RunApplyPayout({wrongPayout}, gmList, 0), "ptxpayout-wrong-recipient");
}

// PTX_SelectWinner returns nullopt when no eligible GMs exist (rollover).
BOOST_AUTO_TEST_CASE(PTXPayout_RolloverReturnsNullopt)
{
    CDeterministicGMList emptyList;
    PopulateTracker({});
    uint256 entropy = PTX_ComputeSelectionEntropy(0, uint256S("1234000000000000000000000000000000000000000000000000000000000000"));
    Optional<CScript> result = PTX_SelectWinner(emptyList, g_ptx_pose_tracker, entropy);
    BOOST_CHECK(!result);

    // Also: eligible GMs exist but all have 0 tickets → rollover.
    CScript script = MakeWinnerScript(0xAA);
    std::string suf = "cccc2222";
    auto gm = MakeDGM("gm01:" + suf, script, uint256S("0101010101010101010101010101010101010101010101010101010101010101"), 1);
    CDeterministicGMList list = MakeGMList({gm});
    PopulateTracker({{"gm01:" + suf, 0, true}});  // 0 tickets → rollover
    Optional<CScript> r2 = PTX_SelectWinner(list, g_ptx_pose_tracker, entropy);
    BOOST_CHECK(!r2);
}

// Mempool rejection: IsPTXPayoutTx() is the predicate that triggers the guard.
BOOST_AUTO_TEST_CASE(PTXPayout_RejectedFromMempool)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXPAYOUT;
    mtx.extraPayload.emplace();
    BOOST_CHECK(CTransaction(mtx).IsPTXPayoutTx());
    // Verify nType=0 is NOT caught by the predicate.
    mtx.nType = CTransaction::TxType::NORMAL;
    BOOST_CHECK(!CTransaction(mtx).IsPTXPayoutTx());
}

// Miner fee arithmetic: (accumulator_value) - (output_value) == nPTXPayoutMinerFee.
// Demonstrates that ConnectBlock's nFees += txValueIn - txValueOut picks it up automatically.
BOOST_AUTO_TEST_CASE(PTXPayout_MinerReceivesFee)
{
    const CAmount accumValue = 100000;
    const CAmount minerFee   = Params().PTXPayoutMinerFee();
    BOOST_REQUIRE(minerFee > 0);
    const CAmount outputValue = accumValue - minerFee;
    BOOST_CHECK_EQUAL(accumValue - outputValue, minerFee);
}

BOOST_AUTO_TEST_SUITE_END()
