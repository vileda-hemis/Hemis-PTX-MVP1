// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "chainparams.h"
#include "chainparamsbase.h"
#include "evo/specialtx_validation.h"
#include "primitives/transaction.h"
#include "ptx/ptx_accum_script.h"
#include "rpc/protocol.h"
#include "script/script.h"
#include "streams.h"
#include "sync.h"
#include "version.h"

#include <boost/test/unit_test.hpp>

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

BOOST_FIXTURE_TEST_SUITE(ptx_sess_tests, PTXBeaTestingSetup)

// Construct a PTXSESS (nType=PTX) transaction that satisfies all pre-existing
// payload checks.  The accum output at vout[1] is supplied by the caller so
// each test can vary it independently.
static CMutableTransaction MakePTXSESSBase()
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTX;
    // Minimal non-coinbase vin so CheckSpecialTxBasic doesn't reject it as coinbase.
    mtx.vin.push_back(CTxIn(COutPoint(
        uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0)));

    // vout[0]: OP_RETURN (round seed placeholder)
    CScript opret;
    opret << OP_RETURN << std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04};
    mtx.vout.push_back(CTxOut(0, opret));

    // Payload: minimal values that satisfy all existing checks.
    CProbabilisticTxPayload payload;
    payload.nSeedHeight      = 1;
    payload.count            = 1;
    payload.low              = 1;
    payload.high             = 100;
    payload.results          = {50};
    payload.quorum_sig_hash  = uint256S("abcdef0000000000000000000000000000000000000000000000000000000000");
    SetTxPayload(mtx, payload);

    return mtx;
}

// Run CheckSpecialTxNoContext under cs_main and return the rejection reason on
// failure, or "" on success.
static std::string RunCheck(const CMutableTransaction& mtx)
{
    LOCK(cs_main);
    CValidationState state;
    const CTransaction tx(mtx);
    if (CheckSpecialTxNoContext(tx, state))
        return "";
    return state.GetRejectReason();
}

// -------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(RpcErrorCodeDefined)
{
    BOOST_CHECK_EQUAL(RPC_PTX_SETTLEMENT_FAILED, -32050);
}

BOOST_AUTO_TEST_CASE(PTXSESSValidOutput_AcceptsExactlyOne)
{
    CMutableTransaction mtx = MakePTXSESSBase();
    // vout[1]: correct accum output
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));
    BOOST_CHECK_EQUAL(RunCheck(mtx), "");
}

BOOST_AUTO_TEST_CASE(PTXSESSValidOutput_RejectsZeroAccumOutputs)
{
    CMutableTransaction mtx = MakePTXSESSBase();
    // No accum output added — only the OP_RETURN at vout[0].
    BOOST_CHECK_EQUAL(RunCheck(mtx), "ptx-bad-accum-output");
}

BOOST_AUTO_TEST_CASE(PTXSESSValidOutput_RejectsWrongValue)
{
    CMutableTransaction mtx = MakePTXSESSBase();
    // Accum output present but value is off by one satoshi.
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee() - 1, GetLotteryAccumScript()));
    BOOST_CHECK_EQUAL(RunCheck(mtx), "ptx-bad-accum-output");
}

BOOST_AUTO_TEST_CASE(PTXSESSValidOutput_RejectsTwoAccumOutputs)
{
    CMutableTransaction mtx = MakePTXSESSBase();
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));
    BOOST_CHECK_EQUAL(RunCheck(mtx), "ptx-bad-accum-output");
}

BOOST_AUTO_TEST_CASE(PTXSESSValidOutput_IgnoresNonAccumExtraOutputs)
{
    CMutableTransaction mtx = MakePTXSESSBase();
    // vout[1]: correct accum output
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));
    // vout[2]: change output to an unrelated P2PKH script — must not affect the check
    CScript change;
    change << OP_DUP << OP_HASH160 << std::vector<uint8_t>(20, 0xCC)
           << OP_EQUALVERIFY << OP_CHECKSIG;
    mtx.vout.push_back(CTxOut(50 * COIN, change));
    BOOST_CHECK_EQUAL(RunCheck(mtx), "");
}

// ---------------------------------------------------------------------------
// W2.4 W4-a — quorum_hash attribution field (KDD-074/076).

// The attribution survives the PRODUCTION serialization path (SetTxPayload ->
// GetTxPayload), not just the in-memory struct.
BOOST_AUTO_TEST_CASE(W4a_AttributionRoundTrip)
{
    CMutableTransaction mtx = MakePTXSESSBase();
    CProbabilisticTxPayload payload;
    BOOST_REQUIRE(GetTxPayload(CTransaction(mtx), payload));
    payload.quorum_hash = uint256S("ac5c28ed9d744ff80dfcf582b69c12b85fc4ce345d84243c6293f7c78d3c54b1");
    SetTxPayload(mtx, payload);

    CProbabilisticTxPayload out;
    BOOST_REQUIRE(GetTxPayload(CTransaction(mtx), out));
    BOOST_CHECK(out.quorum_hash ==
                uint256S("ac5c28ed9d744ff80dfcf582b69c12b85fc4ce345d84243c6293f7c78d3c54b1"));
    BOOST_CHECK(!out.quorum_hash.IsNull());
}

// The serialization boundary: a legacy-layout stream (everything up to
// quorum_sig, no quorum_hash) must FAIL to deserialize — never be silently
// read with garbage attribution. Wire-gate context: the banked bf chain holds
// zero type-6 txs, so no real payload has the legacy layout.
BOOST_AUTO_TEST_CASE(W4a_LegacyStreamBoundary)
{
    CProbabilisticTxPayload payload;
    payload.quorum_hash = uint256S("ac5c28ed9d744ff80dfcf582b69c12b85fc4ce345d84243c6293f7c78d3c54b1");
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << payload;

    // The legacy layout is exactly the new one minus the trailing 32 bytes.
    CDataStream legacy(std::vector<char>(ss.begin(), ss.end() - 32),
                       SER_NETWORK, PROTOCOL_VERSION);
    CProbabilisticTxPayload out;
    BOOST_CHECK_THROW(legacy >> out, std::ios_base::failure);
}

BOOST_AUTO_TEST_SUITE_END()
