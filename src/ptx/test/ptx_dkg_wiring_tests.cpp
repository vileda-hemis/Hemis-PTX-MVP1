// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.3 Package 3: block-inject wiring unit tests (spec §4, KDD-058).
// Direct-call tests — no daemon, no P2P, no chain fixture.
//
// Test inventory:
//   W1_CheckTransaction_AcceptsRealPTXDKG        Green  BuildPTXDKGTx output passes
//                                                       CheckTransaction (tx_verify
//                                                       empty-vin/vout exemption)
//   W2_CheckTransaction_RejectsDegenerateNType11 Green  nType=11 with no payload is NOT
//                                                       exempted (IsPTXDKGTx requires
//                                                       IsSpecialTx) → bad-txns-vin-empty
//   W3_CheckTransaction_RejectsOrdinaryEmptyVin  Green  guard not weakened for normal txs

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"
#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "primitives/transaction.h"

#include "bls/bls_wrapper.h"
#include "sync.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_wiring_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers — copied verbatim from ptx_dkg_phase5_tests.cpp (the established
// copy-per-TU pattern; phase0/1/2/4/5 each carry this block). If the phase5
// helpers change, re-sync this block.
// ---------------------------------------------------------------------------

static std::vector<PTXDKGMember> MakeTestMembers(std::map<uint256, CBLSSecretKey>& key_map)
{
    key_map.clear();
    std::vector<PTXDKGMember> members;
    for (int i = 0; i < 11; i++) {
        PTXDKGMember m;

        std::vector<unsigned char> buf(32, 0);
        buf[0] = (unsigned char)i;
        buf[1] = 0xAA;
        m.proTxHash = uint256(buf);

        buf[1] = 0xBB;
        m.confirmedHash = uint256(buf);

        m.confirmedHashWithProRegTxHash =
            PTX_DKG_ComputeInnerHash(m.proTxHash, m.confirmedHash);

        m.node_id = "gm" + std::to_string(i) + ":8080";

        CBLSSecretKey sk;
        sk.MakeNewKey();
        m.pubKeyOperator = sk.GetPublicKey();
        key_map[m.proTxHash] = sk;

        members.push_back(m);
    }
    return members;
}

static uint256 TestFormationHash()
{
    std::vector<unsigned char> buf(32, 0);
    buf[0] = 0xFB;
    buf[31] = 0x01;
    return uint256(buf);
}

static uint256 PtxOf(const std::vector<PTXDKGSession>& sessions, int i)
{
    return sessions[i].members[sessions[i].my_idx].proTxHash;
}

static void SetupFullPhase0Sessions(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    uint256 fbh = TestFormationHash();
    auto members = MakeTestMembers(key_map);

    sessions.resize(11);
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_InitSession(sessions[i], members, fbh, members[i].proTxHash));

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(sessions[i].my_idx >= 0);
        BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(sessions[i]));
    }

    std::vector<PTXDKGPhase0Msg> p0msgs(11);
    for (int i = 0; i < 11; i++) {
        const uint256& my_ptx = sessions[i].members[sessions[i].my_idx].proTxHash;
        p0msgs[i] = PTX_DKG_BuildPhase0Msg(sessions[i], key_map.at(my_ptx));
    }

    for (int recv = 0; recv < 11; recv++)
        for (int sender = 0; sender < 11; sender++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase0Msg(sessions[recv], p0msgs[sender]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase0(sessions[i]));
}

static void AdvanceToComplaint(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    SetupFullPhase0Sessions(key_map, sessions);

    std::vector<PTXDKGPhase1Msg> p1msgs(11);
    for (int i = 0; i < 11; i++)
        p1msgs[i] = PTX_DKG_BuildPhase1Msg(sessions[i], key_map.at(PtxOf(sessions, i)));

    for (int r = 0; r < 11; r++)
        for (int s = 0; s < 11; s++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(sessions[r], p1msgs[s]));

    for (int r = 0; r < 11; r++) {
        const uint256& rptx = PtxOf(sessions, r);
        for (int s = 0; s < 11; s++)
            PTX_DKG_DecryptMyShare(sessions[r], PtxOf(sessions, s), key_map.at(rptx));
    }

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase1(sessions[i]));
}

static void AdvanceToPremit(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    AdvanceToComplaint(key_map, sessions);

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase2(sessions[i]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase3(sessions[i]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(sessions[i].phase == PTXDKGPhase::PREMIT);
}

// Drive all 11 sessions through P4 (compute + broadcast + close) → FINALIZE.
static void AdvanceToFinalize(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    AdvanceToPremit(key_map, sessions);

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }

    // Build and broadcast all 11 Phase 4 messages to all sessions.
    std::vector<PTXDKGPhase4Msg> p4msgs(11);
    for (int s = 0; s < 11; s++)
        p4msgs[s] = PTX_DKG_BuildPhase4Msg(sessions[s], key_map.at(PtxOf(sessions, s)));

    for (int r = 0; r < 11; r++)
        for (int s = 0; s < 11; s++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[r], p4msgs[s]));

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ClosePhase4(sessions[i]));
        BOOST_REQUIRE(sessions[i].phase == PTXDKGPhase::FINALIZE);
    }
}

// ---------------------------------------------------------------------------
// W1_CheckTransaction_AcceptsRealPTXDKG
//
// A real BuildPTXDKGTx output (empty vin, empty vout, populated payload)
// passes CheckTransaction: the tx_verify empty-vin/vout guards exempt
// IsPTXDKGTx() (spec §4.1, KDD-058 mechanism a).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(W1_CheckTransaction_AcceptsRealPTXDKG)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    CTransaction tx(mtx);

    BOOST_REQUIRE(tx.vin.empty());
    BOOST_REQUIRE(tx.vout.empty());
    BOOST_REQUIRE(tx.IsPTXDKGTx());

    CValidationState state;
    BOOST_CHECK(CheckTransaction(tx, state, true /* fColdStakingActive */));
    BOOST_CHECK(state.IsValid());
}

// ---------------------------------------------------------------------------
// W2_CheckTransaction_RejectsDegenerateNType11
//
// nType=11 with absent extraPayload: IsPTXDKGTx() is false (requires
// IsSpecialTx() → populated payload), so the exemption does NOT apply and the
// vin-empty guard still rejects. Proves the IsSpecialTx keying.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(W2_CheckTransaction_RejectsDegenerateNType11)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXDKG;
    // no SetTxPayload — extraPayload absent
    CTransaction tx(mtx);

    BOOST_REQUIRE(!tx.IsPTXDKGTx());

    CValidationState state;
    BOOST_CHECK(!CheckTransaction(tx, state, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-vin-empty");
}

// ---------------------------------------------------------------------------
// W3_CheckTransaction_RejectsOrdinaryEmptyVin
//
// An ordinary (nType=NORMAL) sapling-version tx with empty vin still rejects:
// the guard is not weakened for non-PTXDKG transactions.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(W3_CheckTransaction_RejectsOrdinaryEmptyVin)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
    CTransaction tx(mtx);

    BOOST_REQUIRE(tx.vin.empty());
    BOOST_REQUIRE(!tx.IsPTXDKGTx());

    CValidationState state;
    BOOST_CHECK(!CheckTransaction(tx, state, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-vin-empty");
}

BOOST_AUTO_TEST_SUITE_END()
