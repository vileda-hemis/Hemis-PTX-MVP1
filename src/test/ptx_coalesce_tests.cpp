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
    if (ProcessSpecialTxsInBlock(block, &dummyIndex, &view, {}, state, true))
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
    // BUG-032 2b-iii: the settle carries NO accum fee output (fee relocated to the
    // PTXROLLCOMMIT). These C7/C8 tests exercise the coalesce block rules, which
    // run before the 2c pairing rule; the settle just needs to be a valid PTXSESS.

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

// Build a PTXROLLCOMMIT (nType=12). withFee=true gives it the relocated service
// fee as an accum-script output — the roll-fee SOURCE that PTX_CollectRollFeeOutputs
// recognises (BUG-032 2b-iii: the fee lives in the commitment now, not the settle).
static CMutableTransaction MakeCommit(bool withFee = true)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXROLLCOMMIT;
    mtx.vin.push_back(CTxIn(COutPoint(
        uint256S("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"), 0)));
    if (withFee)
        mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));
    else
        mtx.vout.push_back(CTxOut(0, CScript() << OP_RETURN << std::vector<uint8_t>{0x00}));
    CPTXRollCommitPayload p;
    p.nSeedHeight   = 1;
    p.nExpiryHeight = 1;
    p.count = 1; p.low = 1; p.high = 100;
    p.round_seed  = uint256S("1111111111111111111111111111111111111111111111111111111111111111");
    p.quorum_hash = uint256S("abcdef0000000000000000000000000000000000000000000000000000000000");
    SetTxPayload(mtx, p);
    return mtx;
}

// Run CheckPTXCoalesceBlockRules directly (C7/C8, in isolation from the coalesce
// value-derivation). Returns "" on accept, else the reject reason.
static std::string RunC78(const std::vector<CTransactionRef>& txs)
{
    LOCK(cs_main);
    CBlock block;
    block.vtx = txs;
    CValidationState state;
    if (CheckPTXCoalesceBlockRules(block, state))
        return "";
    return state.GetRejectReason();
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

// ── C8 mandatory-iff-ROLL-FEE-SOURCE (BUG-032 reverse-direction reconciliation) ──
// The fee moved from the settle (PTXSESS) to the commitment (PTXROLLCOMMIT), so
// C8 now keys on roll-fee-output presence, NOT PTXSESS presence. The three cases
// below prove the fix separates two the old "no PTXSESS" rule conflated:
//   #2 orphan commit (fee source present, no settle)  → VALID
//   #3 coalesce with no fee source at all             → REJECTED (anti-vacuity)
// The dummy outpoint in the coalesce is irrelevant here — CheckPTXCoalesceBlockRules
// only COUNTS coalesces and scans PTX_CollectRollFeeOutputs; the input/value
// derivation is CheckAndApplyPTXCoalesce's job, exercised elsewhere.
static const COutPoint kDummyAccum(
    uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);

// C8 case #1 (happy path): commit(fee) + settle + coalesce → VALID.
BOOST_AUTO_TEST_CASE(RuleC8_CommitSettleCoalesce_Valid)
{
    auto commit = MakeTransactionRef(MakeCommit(/*withFee=*/true));
    auto sess   = MakeTransactionRef(MakePTXSESS());
    auto coal   = MakeTransactionRef(MakePTXCoalesce({kDummyAccum}, 1 * COIN));
    BOOST_CHECK_EQUAL(RunC78({commit, sess, coal}), "");
}

// C8 case #2 (THE FIX — forfeiture path): orphan commit(fee) + coalesce, NO
// settle → VALID. The fee SOURCE is present, so the coalesce is expected. Under
// the pre-fix rule this returned "ptxcoalesce-unexpected" (ptxSessCount==0) and
// halted the chain on the design's own abandoned-roll path.
BOOST_AUTO_TEST_CASE(RuleC8_OrphanCommitCoalesce_Valid)
{
    auto commit = MakeTransactionRef(MakeCommit(/*withFee=*/true));
    auto coal   = MakeTransactionRef(MakePTXCoalesce({kDummyAccum}, 1 * COIN));
    BOOST_CHECK_EQUAL(RunC78({commit, coal}), "");
}

// C8 case #3 (ANTI-VACUITY): coalesce with NO fee source (no commit, no settle)
// → STILL REJECTED. The fix must not over-correct into "accept any coalesce".
BOOST_AUTO_TEST_CASE(RuleC8_CoalesceWithoutFeeSource_Rejected)
{
    auto coal = MakeTransactionRef(MakePTXCoalesce({kDummyAccum}, 1 * COIN));
    BOOST_CHECK_EQUAL(RunC78({coal}), "ptxcoalesce-unexpected");
}

// C8 forward direction, re-keyed: a roll-fee source (commit) with NO coalesce
// → "ptxcoalesce-missing" (was keyed on PTXSESS; now on the fee source).
BOOST_AUTO_TEST_CASE(RuleC8_FeeSourceWithoutCoalesce_Missing)
{
    auto commit = MakeTransactionRef(MakeCommit(/*withFee=*/true));
    BOOST_CHECK_EQUAL(RunC78({commit}), "ptxcoalesce-missing");
}

// A commit WITHOUT its fee output is not a fee source: no coalesce needed → VALID.
BOOST_AUTO_TEST_CASE(RuleC8_CommitWithoutFee_NeedsNoCoalesce)
{
    auto commit = MakeTransactionRef(MakeCommit(/*withFee=*/false));
    BOOST_CHECK_EQUAL(RunC78({commit}), "");
}

BOOST_AUTO_TEST_SUITE_END()
