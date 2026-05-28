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
#include "script/script.h"
#include "sync.h"

#include <boost/test/unit_test.hpp>

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

BOOST_FIXTURE_TEST_SUITE(ptx_coalesce_tests, PTXBeaTestingSetup)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build a minimal PTXCOALESCE transaction.  All structural parameters can be
// overridden per-test via the optional flags.
static CMutableTransaction MakePTXCoalesce(
    const std::vector<COutPoint>& inputs,
    CAmount                       total_value,
    bool                          wrong_output_script   = false,
    bool                          nonempty_extrapayload = false,
    bool                          nonempty_scriptsig    = false,
    bool                          extra_output          = false,
    bool                          wrong_value           = false)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXCOALESCE;

    for (const COutPoint& out : inputs) {
        CTxIn txin(out);
        if (nonempty_scriptsig)
            txin.scriptSig << OP_1;  // C6 violation
        mtx.vin.push_back(txin);
    }

    // Single accumulator output.
    CScript outScript = wrong_output_script
        ? []() { CScript s; s << OP_DUP << OP_HASH160
                               << std::vector<uint8_t>(20, 0xAB)
                               << OP_EQUALVERIFY << OP_CHECKSIG; return s; }()
        : GetLotteryAccumScript();
    CAmount outValue = wrong_value ? total_value + 1 : total_value;
    mtx.vout.push_back(CTxOut(outValue, outScript));

    if (extra_output) {
        // C2 violation: second output
        mtx.vout.push_back(CTxOut(0, GetLotteryAccumScript()));
    }

    if (nonempty_extrapayload) {
        // C5 violation: non-empty payload
        mtx.extraPayload.emplace(std::vector<uint8_t>{0x01, 0x02, 0x03});
    } else {
        // Correct: present-but-empty
        mtx.extraPayload.emplace();
    }

    return mtx;
}

// Plant a synthetic LOTTERY_ACCUM_SCRIPT coin into a view so that C1/C4
// checks can be exercised without a live chain.
static void InsertAccumCoin(CCoinsViewCache& view, const COutPoint& outpoint, CAmount value)
{
    Coin coin;
    coin.out.scriptPubKey = GetLotteryAccumScript();
    coin.out.nValue       = value;
    coin.nHeight          = 1;
    view.AddCoin(outpoint, std::move(coin), false);
}

// Plant a coin with an arbitrary (non-accum) script — used to exercise C1.
static void InsertNonAccumCoin(CCoinsViewCache& view, const COutPoint& outpoint, CAmount value)
{
    Coin coin;
    CScript s;
    s << OP_DUP << OP_HASH160 << std::vector<uint8_t>(20, 0xCC) << OP_EQUALVERIFY << OP_CHECKSIG;
    coin.out.scriptPubKey = s;
    coin.out.nValue       = value;
    coin.nHeight          = 1;
    view.AddCoin(outpoint, std::move(coin), false);
}

// Convenience: run CheckSpecialTx with no context (pindexPrev=nullptr, view=nullptr).
// Sufficient for rules that don't depend on the coin view (C2, C3, C5, C6).
static std::string RunCheckNoCtx(const CMutableTransaction& mtx)
{
    LOCK(cs_main);
    CValidationState state;
    const CTransaction tx(mtx);
    if (CheckSpecialTxNoContext(tx, state))
        return "";
    return state.GetRejectReason();
}

// Run CheckSpecialTx with a real coin view (needed for C1, C4).
static std::string RunCheckWithView(const CMutableTransaction& mtx, const CCoinsViewCache& view)
{
    LOCK(cs_main);
    CValidationState state;
    const CTransaction tx(mtx);
    if (CheckSpecialTx(tx, nullptr, &view, state))
        return "";
    return state.GetRejectReason();
}

// Run ProcessSpecialTxsInBlock with a synthetic block and a coin view.
// The block's vtx is provided directly by the caller.
static std::string RunBlockCheck(const std::vector<CTransactionRef>& txs,
                                 const CCoinsViewCache&              view)
{
    LOCK(cs_main);
    CBlock block;
    block.vtx = txs;
    CValidationState state;
    // Dummy index: pprev=nullptr skips the V6_0 gate inside CheckSpecialTx,
    // which is the correct path for tests that don't require upgrade-height context.
    CBlockIndex dummyIndex;
    if (ProcessSpecialTxsInBlock(block, &dummyIndex, &view, state, true))
        return "";
    return state.GetRejectReason();
}

// Build a minimal PTXSESS transaction (nType=PTX) — used for C8 block tests.
static CMutableTransaction MakePTXSESS()
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTX;
    mtx.vin.push_back(CTxIn(COutPoint(
        uint256S("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), 0)));

    CScript opret;
    opret << OP_RETURN << std::vector<uint8_t>{0xDE, 0xAD};
    mtx.vout.push_back(CTxOut(0, opret));
    // Accum output at vout[1]
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));

    CProbabilisticTxPayload payload;
    payload.nSeedHeight     = 1;
    payload.count           = 1;
    payload.low             = 1;
    payload.high            = 100;
    payload.results         = {42};
    payload.quorum_sig_hash = uint256S("abcdef0000000000000000000000000000000000000000000000000000000000");
    SetTxPayload(mtx, payload);
    return mtx;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// IsPTXCoalesceTx() recognises nType=9 and rejects everything else.
BOOST_AUTO_TEST_CASE(PredicateRecognizes)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.extraPayload.emplace();

    mtx.nType = CTransaction::TxType::PTXCOALESCE;
    BOOST_CHECK(CTransaction(mtx).IsPTXCoalesceTx());

    mtx.nType = CTransaction::TxType::NORMAL;
    BOOST_CHECK(!CTransaction(mtx).IsPTXCoalesceTx());

    mtx.nType = CTransaction::TxType::PTX;
    BOOST_CHECK(!CTransaction(mtx).IsPTXCoalesceTx());
}

// C1: all inputs must be LOTTERY_ACCUM_SCRIPT UTXOs.
BOOST_AUTO_TEST_CASE(RuleC1_RejectsNonAccumInputs)
{
    COutPoint op1(uint256S("1111111111111111111111111111111111111111111111111111111111111111"), 0);
    COutPoint op2(uint256S("2222222222222222222222222222222222222222222222222222222222222222"), 0);

    CCoinsView base;
    CCoinsViewCache view(&base);
    InsertAccumCoin(view, op1, 1 * COIN);
    InsertNonAccumCoin(view, op2, 1 * COIN);  // non-accum input

    CMutableTransaction mtx = MakePTXCoalesce({op1, op2}, 2 * COIN);
    BOOST_CHECK_EQUAL(RunCheckWithView(mtx, view), "ptxcoalesce-non-accum-input");
}

// C2: exactly one output; two outputs rejected.
BOOST_AUTO_TEST_CASE(RuleC2_RejectsMultipleOutputs)
{
    COutPoint op(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);
    CMutableTransaction mtx = MakePTXCoalesce({op}, 1 * COIN,
        /*wrong_output_script=*/false,
        /*nonempty_extrapayload=*/false,
        /*nonempty_scriptsig=*/false,
        /*extra_output=*/true);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx), "ptxcoalesce-bad-output-count");
}

// C3: output scriptPubKey must be LOTTERY_ACCUM_SCRIPT.
BOOST_AUTO_TEST_CASE(RuleC3_RejectsWrongOutputScript)
{
    COutPoint op(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);
    CMutableTransaction mtx = MakePTXCoalesce({op}, 1 * COIN,
        /*wrong_output_script=*/true);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx), "ptxcoalesce-bad-output-script");
}

// C4: output value must equal sum of input values.
BOOST_AUTO_TEST_CASE(RuleC4_RejectsValueMismatch)
{
    COutPoint op(uint256S("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"), 0);

    CCoinsView base;
    CCoinsViewCache view(&base);
    InsertAccumCoin(view, op, 1 * COIN);

    // wrong_value=true adds +1 satoshi to the output → mismatch
    CMutableTransaction mtx = MakePTXCoalesce({op}, 1 * COIN,
        /*wrong_output_script=*/false,
        /*nonempty_extrapayload=*/false,
        /*nonempty_scriptsig=*/false,
        /*extra_output=*/false,
        /*wrong_value=*/true);
    BOOST_CHECK_EQUAL(RunCheckWithView(mtx, view), "ptxcoalesce-value-mismatch");
}

// C5: extraPayload must be present-but-empty; non-empty payload rejected.
BOOST_AUTO_TEST_CASE(RuleC5_RejectsNonEmptyExtraPayload)
{
    COutPoint op(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);
    CMutableTransaction mtx = MakePTXCoalesce({op}, 1 * COIN,
        /*wrong_output_script=*/false,
        /*nonempty_extrapayload=*/true);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx), "ptxcoalesce-bad-payload");
}

// C6: all scriptSigs must be empty.
BOOST_AUTO_TEST_CASE(RuleC6_RejectsNonEmptyScriptSig)
{
    COutPoint op(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);
    CMutableTransaction mtx = MakePTXCoalesce({op}, 1 * COIN,
        /*wrong_output_script=*/false,
        /*nonempty_extrapayload=*/false,
        /*nonempty_scriptsig=*/true);
    BOOST_CHECK_EQUAL(RunCheckNoCtx(mtx), "ptxcoalesce-nonempty-scriptsig");
}

// Happy path: a well-formed PTXCOALESCE passes all per-tx checks.
BOOST_AUTO_TEST_CASE(AcceptsValidStructure)
{
    COutPoint op1(uint256S("1111111111111111111111111111111111111111111111111111111111111111"), 0);
    COutPoint op2(uint256S("2222222222222222222222222222222222222222222222222222222222222222"), 0);

    CCoinsView base;
    CCoinsViewCache view(&base);
    InsertAccumCoin(view, op1, 1 * COIN);
    InsertAccumCoin(view, op2, 2 * COIN);

    CMutableTransaction mtx = MakePTXCoalesce({op1, op2}, 3 * COIN);
    BOOST_CHECK_EQUAL(RunCheckWithView(mtx, view), "");
}

// Mempool exclusion: IsPTXCoalesceTx() should be detected early (tested
// structurally here — the full AcceptToMemoryPool path requires a wallet and
// funded chain, so we validate via the predicate that triggers the guard).
BOOST_AUTO_TEST_CASE(RejectedFromMempool)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXCOALESCE;
    mtx.extraPayload.emplace();
    // A PTXCOALESCE tx must be detected by IsPTXCoalesceTx() before
    // AcceptToMemoryPoolWorker reaches script validation.
    BOOST_CHECK(CTransaction(mtx).IsPTXCoalesceTx());
    // Verify the predicate returns false for nType=0 (NORMAL) so the guard is
    // not accidentally broad.
    mtx.nType = CTransaction::TxType::NORMAL;
    BOOST_CHECK(!CTransaction(mtx).IsPTXCoalesceTx());
}

// C7 (block-level): at most one PTXCOALESCE per block.
BOOST_AUTO_TEST_CASE(RuleC7_AtMostOneCoalescePerBlock)
{
    COutPoint op1(uint256S("1111111111111111111111111111111111111111111111111111111111111111"), 0);
    COutPoint op2(uint256S("2222222222222222222222222222222222222222222222222222222222222222"), 0);

    CCoinsView base;
    CCoinsViewCache view(&base);
    InsertAccumCoin(view, op1, 1 * COIN);
    InsertAccumCoin(view, op2, 1 * COIN);

    // Block with one PTXSESS and two PTXCOALESCE — C7 violation.
    auto sess  = MakeTransactionRef(MakePTXSESS());
    auto coal1 = MakeTransactionRef(MakePTXCoalesce({op1}, 1 * COIN));
    auto coal2 = MakeTransactionRef(MakePTXCoalesce({op2}, 1 * COIN));

    BOOST_CHECK_EQUAL(RunBlockCheck({sess, coal1, coal2}, view), "ptxcoalesce-duplicate");
}

// C8a (block-level): PTXCOALESCE is mandatory when PTXSESS is present.
BOOST_AUTO_TEST_CASE(RuleC8_MissingCoalesceWhenSessPresent)
{
    CCoinsView base;
    CCoinsViewCache view(&base);

    auto sess = MakeTransactionRef(MakePTXSESS());
    // Block has a PTXSESS but no PTXCOALESCE.
    BOOST_CHECK_EQUAL(RunBlockCheck({sess}, view), "ptxcoalesce-missing");
}

// C8b (block-level): PTXCOALESCE is rejected when no PTXSESS is present.
BOOST_AUTO_TEST_CASE(RuleC8_UnexpectedCoalesceWithoutSess)
{
    COutPoint op(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);

    CCoinsView base;
    CCoinsViewCache view(&base);
    InsertAccumCoin(view, op, 1 * COIN);

    auto coal = MakeTransactionRef(MakePTXCoalesce({op}, 1 * COIN));
    // Block has a PTXCOALESCE but no PTXSESS.
    BOOST_CHECK_EQUAL(RunBlockCheck({coal}, view), "ptxcoalesce-unexpected");
}

BOOST_AUTO_TEST_SUITE_END()
