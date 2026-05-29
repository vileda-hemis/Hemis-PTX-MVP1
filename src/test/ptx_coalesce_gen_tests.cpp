// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "chain.h"
#include "chainparams.h"
#include "chainparamsbase.h"
#include "coins.h"
#include "evo/specialtx_validation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "ptx/ptx_accum_script.h"
#include "ptx/ptx_coalesce.h"
#include "ptx/ptx_lottery_state.h"
#include "script/script.h"
#include "serialize.h"
#include "sync.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

BOOST_FIXTURE_TEST_SUITE(ptx_coalesce_gen_tests, PTXBeaTestingSetup)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static void InsertAccumCoin(CCoinsViewCache& view, const COutPoint& op, CAmount val)
{
    Coin c;
    c.out.scriptPubKey = GetLotteryAccumScript();
    c.out.nValue       = val;
    c.nHeight          = 1;
    view.AddCoin(op, std::move(c), false);
}

// Minimal PTXSESS that satisfies CheckSpecialTx (payload + ptx-bad-accum-output rule).
static CMutableTransaction MakePTXSESS()
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTX;
    mtx.vin.push_back(CTxIn(COutPoint(
        uint256S("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"), 0)));
    CScript opret;
    opret << OP_RETURN << std::vector<uint8_t>{0x01, 0x02};
    mtx.vout.push_back(CTxOut(0, opret));
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));
    CProbabilisticTxPayload payload;
    payload.nSeedHeight      = 1;
    payload.count            = 1;
    payload.low              = 1;
    payload.high             = 100;
    payload.results          = {42};
    payload.quorum_sig_hash  = uint256S("abcdef0000000000000000000000000000000000000000000000000000000000");
    SetTxPayload(mtx, payload);
    return mtx;
}

static std::vector<uint8_t> SerializeState(const LotteryState& s)
{
    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << s;
    return std::vector<uint8_t>(ds.begin(), ds.end());
}

// ---------------------------------------------------------------------------
// Test 1 — Coalesce_FirstPTXSESSCreatesAccumulator
//
// No prior accumulator.  One PTXSESS.  Generator must produce a PTXCOALESCE
// with exactly one input (the PTXSESS fee output) and output = nPTXServiceFee.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Coalesce_FirstPTXSESSCreatesAccumulator)
{
    CMutableTransaction ptxsess = MakePTXSESS();
    const CTransaction  ptxsessTx(ptxsess);
    COutPoint feeOut(ptxsessTx.GetHash(), 1);
    AccumInput inp{feeOut, Params().PTXServiceFee()};

    CTransactionRef coalesce = PTX_BuildCoalesceTx(COutPoint(), 0, {inp});

    BOOST_REQUIRE(coalesce != nullptr);
    BOOST_CHECK(coalesce->IsPTXCoalesceTx());

    BOOST_REQUIRE_EQUAL(coalesce->vin.size(), 1u);
    BOOST_CHECK(coalesce->vin[0].prevout == feeOut);
    BOOST_CHECK(coalesce->vin[0].scriptSig.empty());

    BOOST_REQUIRE_EQUAL(coalesce->vout.size(), 1u);
    BOOST_CHECK(coalesce->vout[0].scriptPubKey == GetLotteryAccumScript());
    BOOST_CHECK_EQUAL(coalesce->vout[0].nValue, Params().PTXServiceFee());

    BOOST_REQUIRE(coalesce->extraPayload.has_value());
    BOOST_CHECK(coalesce->extraPayload->empty());
}

// ---------------------------------------------------------------------------
// Test 2 — Coalesce_ExtendsExistingAccumulator
//
// Prior accumulator (5 HMS) + one new PTXSESS fee.  Generator must put the
// prior accumulator outpoint first in vin and sum both values in vout.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Coalesce_ExtendsExistingAccumulator)
{
    const CAmount priorValue = 5 * COIN;
    COutPoint priorOut(
        uint256S("1111111111111111111111111111111111111111111111111111111111111111"), 0);

    CMutableTransaction ptxsess = MakePTXSESS();
    const CTransaction  ptxsessTx(ptxsess);
    COutPoint feeOut(ptxsessTx.GetHash(), 1);
    AccumInput inp{feeOut, Params().PTXServiceFee()};

    CTransactionRef coalesce = PTX_BuildCoalesceTx(priorOut, priorValue, {inp});

    BOOST_REQUIRE_EQUAL(coalesce->vin.size(), 2u);
    BOOST_CHECK(coalesce->vin[0].prevout == priorOut);
    BOOST_CHECK(coalesce->vin[1].prevout == feeOut);

    BOOST_REQUIRE_EQUAL(coalesce->vout.size(), 1u);
    BOOST_CHECK_EQUAL(coalesce->vout[0].nValue, priorValue + Params().PTXServiceFee());
    BOOST_CHECK(coalesce->vout[0].scriptPubKey == GetLotteryAccumScript());
}

// ---------------------------------------------------------------------------
// Test 3 — Coalesce_LotteryStateUpdatedCorrectly
//
// After CheckAndApplyPTXCoalesce with fJustCheck=false, LotteryState must
// reflect the new accumulator outpoint (coalesce txid:0) and value.
// Uses CheckAndApplyPTXCoalesce directly to avoid the deterministicGMManager
// and llmq::quorumBlockProcessor pprev contracts.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Coalesce_LotteryStateUpdatedCorrectly)
{
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
    }

    CMutableTransaction ptxsess = MakePTXSESS();
    const CTransaction  ptxsessTx(ptxsess);
    COutPoint feeOut(ptxsessTx.GetHash(), 1);
    AccumInput inp{feeOut, Params().PTXServiceFee()};

    CTransactionRef coalesce = PTX_BuildCoalesceTx(COutPoint(), 0, {inp});

    // Plant fee output coin so C1/C4 in CheckSpecialTx see unspent accum coins.
    CCoinsView base;
    CCoinsViewCache view(&base);
    InsertAccumCoin(view, feeOut, Params().PTXServiceFee());

    CBlock block;
    block.vtx.push_back(MakeTransactionRef([]() {
        CMutableTransaction cb; cb.nVersion = CTransaction::TxVersion::SAPLING;
        cb.nType = CTransaction::TxType::NORMAL;
        CTxIn in; in.prevout.SetNull(); in.scriptSig << CScriptNum(1); cb.vin.push_back(in);
        cb.vout.push_back(CTxOut(0, CScript())); return cb; }()));
    block.vtx.push_back(MakeTransactionRef(ptxsess));
    block.vtx.push_back(coalesce);

    uint256 blockHash =
        uint256S("3333333333333333333333333333333333333333333333333333333333333333");
    CBlockIndex idx;
    idx.phashBlock = &blockHash;
    idx.nHeight    = 5;
    idx.pprev      = nullptr;  // CheckAndApplyPTXCoalesce does not dereference pprev

    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_REQUIRE_MESSAGE(
            CheckAndApplyPTXCoalesce(block, &idx, state, /*fJustCheck=*/false),
            "CheckAndApplyPTXCoalesce failed: " + state.GetRejectReason());
    }

    {
        LOCK(cs_main);
        const LotteryState& ls = GetLotteryState();
        BOOST_CHECK(ls.HasAccumulator());
        BOOST_CHECK(ls.accumulator_outpoint == COutPoint(coalesce->GetHash(), 0));
        BOOST_CHECK_EQUAL(ls.accumulator_value, Params().PTXServiceFee());
    }
}

// ---------------------------------------------------------------------------
// Test 4 — Coalesce_ReorgReversedCorrectly
//
// Connect (CheckAndApplyPTXCoalesce, fJustCheck=false) then disconnect
// (UndoSpecialTxsInBlock).  LotteryState must be restored byte-exactly.
// UndoSpecialTxsInBlock is the production reorg path; CheckAndApplyPTXCoalesce
// is the exact lottery-state portion that ConnectBlock calls.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Coalesce_ReorgReversedCorrectly)
{
    uint256 prevHash =
        uint256S("4444444444444444444444444444444444444444444444444444444444444444");
    uint256 currHash =
        uint256S("5555555555555555555555555555555555555555555555555555555555555555");

    CBlockIndex prevIdx;
    prevIdx.phashBlock = &prevHash;
    prevIdx.nHeight    = 10;
    prevIdx.pprev      = nullptr;

    CBlockIndex currIdx;
    currIdx.phashBlock = &currHash;
    currIdx.nHeight    = 11;
    currIdx.pprev      = &prevIdx;  // required by assert in UndoSpecialTxsInBlock

    // Establish known pre-block state and write the pprev snapshot so that
    // UndoSpecialTxsInBlock can read it when restoring after the disconnect.
    // This write MUST precede the CheckAndApplyPTXCoalesce call.
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        WriteLotteryStateSnapshotForBlock(prevHash, GetLotteryState());
    }
    const std::vector<uint8_t> preBlockBytes = [&]() {
        LOCK(cs_main);
        return SerializeState(GetLotteryState());
    }();

    CMutableTransaction ptxsess = MakePTXSESS();
    const CTransaction  ptxsessTx(ptxsess);
    COutPoint feeOut(ptxsessTx.GetHash(), 1);
    AccumInput inp{feeOut, Params().PTXServiceFee()};

    CTransactionRef coalesce = PTX_BuildCoalesceTx(COutPoint(), 0, {inp});

    CCoinsView base;
    CCoinsViewCache view(&base);
    InsertAccumCoin(view, feeOut, Params().PTXServiceFee());

    CBlock block;
    block.vtx.push_back(MakeTransactionRef([]() {
        CMutableTransaction cb; cb.nVersion = CTransaction::TxVersion::SAPLING;
        cb.nType = CTransaction::TxType::NORMAL;
        CTxIn in; in.prevout.SetNull(); in.scriptSig << CScriptNum(1); cb.vin.push_back(in);
        cb.vout.push_back(CTxOut(0, CScript())); return cb; }()));
    block.vtx.push_back(MakeTransactionRef(ptxsess));
    block.vtx.push_back(coalesce);

    // Connect: updates LotteryState and writes currHash snapshot.
    {
        LOCK(cs_main);
        CValidationState state;
        BOOST_REQUIRE_MESSAGE(
            CheckAndApplyPTXCoalesce(block, &currIdx, state, /*fJustCheck=*/false),
            "connect failed: " + state.GetRejectReason());
    }

    // Confirm the state IS different after connect.
    {
        LOCK(cs_main);
        BOOST_REQUIRE(SerializeState(GetLotteryState()) != preBlockBytes);
        BOOST_CHECK(GetLotteryState().HasAccumulator());
    }

    // Disconnect: UndoSpecialTxsInBlock reads from prevHash snapshot and restores.
    {
        LOCK(cs_main);
        BOOST_REQUIRE(UndoSpecialTxsInBlock(block, &currIdx));
    }

    // Post-restore: must byte-exactly match the pre-block state.
    {
        LOCK(cs_main);
        BOOST_CHECK(SerializeState(GetLotteryState()) == preBlockBytes);
        BOOST_CHECK(!GetLotteryState().HasAccumulator());
        BOOST_CHECK_EQUAL(GetLotteryState().accumulator_value, 0);
    }
}

// ---------------------------------------------------------------------------
// Test 5 — Coalesce_GeneratedTxPassesStep6Rules
//
// A PTXCOALESCE from PTX_BuildCoalesceTx must pass CheckSpecialTx (C1–C6)
// and CheckAndApplyPTXCoalesce (C7–C8 + Step 7 structural check).
// Writer/validator agreement is the invariant.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(Coalesce_GeneratedTxPassesStep6Rules)
{
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
    }

    CMutableTransaction ptxsess = MakePTXSESS();
    const CTransaction  ptxsessTx(ptxsess);
    COutPoint feeOut(ptxsessTx.GetHash(), 1);
    AccumInput inp{feeOut, Params().PTXServiceFee()};

    CTransactionRef coalesce = PTX_BuildCoalesceTx(COutPoint(), 0, {inp});

    CCoinsView base;
    CCoinsViewCache view(&base);
    InsertAccumCoin(view, feeOut, Params().PTXServiceFee());

    // --- Per-tx check (C1–C6) ---
    {
        LOCK(cs_main);
        CValidationState state;
        const CTransaction tx(*coalesce);
        BOOST_CHECK_MESSAGE(
            CheckSpecialTx(tx, nullptr, &view, state),
            "CheckSpecialTx rejected: " + state.GetRejectReason());
    }

    // --- Block-level count rules (C7: ≤1 coalesce, C8: mandatory iff PTXSESS) ---
    // --- plus Step 7 structural check (input/value correctness) ---
    // Both helpers are called directly so the test exercises C7/C8 against
    // the generator's output without invoking deterministicGMManager or llmq.
    // fJustCheck=true in CheckAndApplyPTXCoalesce: validates without mutating state.
    // pprev=nullptr is safe: neither helper dereferences pprev.
    {
        LOCK(cs_main);
        CBlock block;
        block.vtx.push_back(MakeTransactionRef([]() {
            CMutableTransaction cb; cb.nVersion = CTransaction::TxVersion::SAPLING;
            cb.nType = CTransaction::TxType::NORMAL;
            CTxIn in; in.prevout.SetNull(); in.scriptSig << CScriptNum(1); cb.vin.push_back(in);
            cb.vout.push_back(CTxOut(0, CScript())); return cb; }()));
        block.vtx.push_back(MakeTransactionRef(ptxsess));
        block.vtx.push_back(coalesce);

        uint256 blockHash =
            uint256S("6666666666666666666666666666666666666666666666666666666666666666");
        CBlockIndex idx;
        idx.phashBlock = &blockHash;
        idx.nHeight    = 1;
        idx.pprev      = nullptr;

        CValidationState stateC78;
        BOOST_CHECK_MESSAGE(
            CheckPTXCoalesceBlockRules(block, stateC78),
            "CheckPTXCoalesceBlockRules rejected: " + stateC78.GetRejectReason());

        CValidationState stateStep7;
        BOOST_CHECK_MESSAGE(
            CheckAndApplyPTXCoalesce(block, &idx, stateStep7, /*fJustCheck=*/true),
            "CheckAndApplyPTXCoalesce rejected: " + stateStep7.GetRejectReason());
    }
}

BOOST_AUTO_TEST_SUITE_END()
