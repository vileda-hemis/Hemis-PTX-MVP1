// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.2-P5: Phase 5 (FINALIZE) unit tests.
// Direct-call tests — no daemon, no P2P.
//
// Test inventory (8 cases):
//   P5_StoreSkShare_WritesCorrectBytes          Green  sk_share_i → g_ptx_my_bls_sk_bytes round-trips
//   P5_BuildPTXDKGTx_StructuralValidity         Green  nType=11, nVersion=3, payload parses
//   P5_BuildPTXDKGTx_PremitCountAtLeastT        Green  premit_commitments.size() >= 6
//   P5_ClosePhase5_PhaseBecomesDone             Green  full ClosePhase5 → phase == DONE
//   P5_EndToEnd_SigningPathWorks                 Green  REQUIRED MILESTONE GATE (C4):
//                                                       DKG sk_share + group_pk → PartialSign
//                                                       → Recover → Verify passes
//   P5_CheckPTXDKGTx_AcceptValidPayload         Green  valid payload → CheckPTXDKGTx true
//   P5_CheckPTXDKGTx_RejectBadGroupPk           Red falsification  corrupt group_pk → rejected
//   P5_CheckPTXDKGTx_RejectInsufficientPremits  Red falsification  5 premit entries → rejected

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_bls.h"
#include "evo/specialtx_validation.h"
#include "consensus/validation.h"
#include "primitives/transaction.h"

#include "bls/bls_wrapper.h"
#include "random.h"
#include "sync.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_phase5_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers — mirrors phase4 pattern; static to this translation unit
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

// §C1 (KDD-057): the sk-share slot is a process-global guarded by
// refuse-unless-empty (PTX_BLS_SetSkShare). Tests that store a share must start
// from an EMPTY slot — a prior test in the shared test binary may have left it
// set, which the guard would now (correctly) refuse. Reset before each store.
static void PTX_TEST_ClearSkShareSlot()
{
    LOCK(cs_ptx_my_bls_sk);
    g_ptx_my_bls_sk_set = false;
}

// ---------------------------------------------------------------------------
// P5_StoreSkShare_WritesCorrectBytes
//
// StoreSkShare → g_ptx_my_bls_sk_bytes matches blst_bendian_from_scalar(sk_share_i).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_StoreSkShare_WritesCorrectBytes)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE(PTX_DKG_StoreSkShare(sessions[0]));

    // Read back bytes under the lock.
    uint8_t stored_bytes[32];
    {
        LOCK(cs_ptx_my_bls_sk);
        BOOST_REQUIRE(g_ptx_my_bls_sk_set);
        std::memcpy(stored_bytes, g_ptx_my_bls_sk_bytes, 32);
    }

    // Expected bytes: blst_bendian_from_scalar(sk_share_i).
    uint8_t expected_be[32];
    blst_bendian_from_scalar(expected_be, &sessions[0].sk_share_i);

    BOOST_CHECK_EQUAL_COLLECTIONS(stored_bytes, stored_bytes + 32,
                                  expected_be,  expected_be  + 32);
}

// ---------------------------------------------------------------------------
// P5_BuildPTXDKGTx_StructuralValidity
//
// BuildPTXDKGTx produces nType=11 (PTXDKG), nVersion=3 (SAPLING), parseable
// payload with decompressable group_pk and correct member count.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_BuildPTXDKGTx_StructuralValidity)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    CMutableTransaction tx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);

    BOOST_CHECK_EQUAL(tx.nType,    (int16_t)CTransaction::TxType::PTXDKG);
    BOOST_CHECK_EQUAL(tx.nVersion, (int16_t)CTransaction::TxVersion::SAPLING);
    BOOST_REQUIRE(tx.hasExtraPayload());

    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(tx, payload));

    // group_pk_bytes must decompress.
    blst_p1_affine gp;
    BOOST_CHECK(blst_p1_uncompress(&gp, payload.group_pk_bytes) == BLST_SUCCESS);

    // Effective-QUAL = 11 (all honest, no bad members).
    BOOST_CHECK_EQUAL((int)payload.member_node_ids.size(), 11);
    BOOST_CHECK_EQUAL(payload.quorum_hash, sessions[0].quorum_hash);
    BOOST_CHECK_EQUAL(payload.formation_height, 1000);
}

// ---------------------------------------------------------------------------
// P5_BuildPTXDKGTx_PremitCountAtLeastT
//
// With all 11 sessions broadcasting Phase 4 messages, premit_commitments >= t=6.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_BuildPTXDKGTx_PremitCountAtLeastT)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    CMutableTransaction tx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);

    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(tx, payload));

    BOOST_CHECK_GE((int)payload.premit_commitments.size(), 6);
}

// ---------------------------------------------------------------------------
// P5_ClosePhase5_PhaseBecomesDone
//
// Full ClosePhase5 → phase == DONE, transaction is returned.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_ClosePhase5_PhaseBecomesDone)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    PTX_TEST_ClearSkShareSlot();
    CMutableTransaction tx_out;
    BOOST_CHECK(PTX_DKG_ClosePhase5(sessions[0], 1000, tx_out));
    BOOST_CHECK(sessions[0].phase == PTXDKGPhase::DONE);
    BOOST_CHECK_EQUAL(tx_out.nType, (int16_t)CTransaction::TxType::PTXDKG);
}

// ---------------------------------------------------------------------------
// P5_EndToEnd_SigningPathWorks   — REQUIRED MILESTONE GATE (C4)
//
// DKG-produced sk_share_i + group_pk → PTX_BLS_PartialSign → PTX_BLS_Recover
// → PTX_BLS_Verify passes.
//
// This simultaneously exercises:
//   (a) own-vvec[0] inclusion invariant (GF2): wrong group_pk → Verify fails
//   (b) sk_share storage-format match (GF3): wrong bytes → PartialSign wrong sig
//   (c) end-to-end threshold key correctness
// W1.2 is not complete until this test is GREEN.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_EndToEnd_SigningPathWorks)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    // Close Phase 5 for session[0] (writes sk_share to global storage).
    PTX_TEST_ClearSkShareSlot();
    CMutableTransaction tx_out;
    BOOST_REQUIRE(PTX_DKG_ClosePhase5(sessions[0], 1000, tx_out));
    BOOST_REQUIRE(sessions[0].phase == PTXDKGPhase::DONE);

    // All sessions should agree on the same group_pk.
    uint8_t group_pk_bytes[48];
    blst_p1_affine_compress(group_pk_bytes, &sessions[0].group_pk);

    // Test message.
    uint256 test_msg;
    {
        std::vector<unsigned char> buf(32, 0xAB);
        test_msg = uint256(buf);
    }

    // Collect t=6 partial signatures using sessions[0..5].
    // Each session's sk_share_i is the correct share at its share_index.
    const int t = 6;
    std::vector<int> indices;
    std::vector<std::vector<uint8_t>> partial_sigs;

    for (int i = 0; i < t; i++) {
        int share_index = sessions[i].members[sessions[i].my_idx].share_index;
        indices.push_back(share_index);

        uint8_t sk_bytes[32];
        blst_bendian_from_scalar(sk_bytes, &sessions[i].sk_share_i);

        uint8_t sig_buf[PTX_SIG_BYTES];
        BOOST_REQUIRE(PTX_BLS_PartialSign(sk_bytes, test_msg, sig_buf));
        partial_sigs.push_back(std::vector<uint8_t>(sig_buf, sig_buf + PTX_SIG_BYTES));
    }

    // Recover threshold signature via Lagrange interpolation.
    uint8_t combined_sig[PTX_SIG_BYTES];
    BOOST_REQUIRE(PTX_BLS_Recover(indices, partial_sigs, combined_sig));

    // Verify against the DKG-produced group_pk — this is the milestone gate.
    BOOST_CHECK(PTX_BLS_Verify(group_pk_bytes, test_msg, combined_sig));
}

// ---------------------------------------------------------------------------
// P5_CheckPTXDKGTx_AcceptValidPayload
//
// A properly constructed PTXDKG transaction passes CheckPTXDKGTx.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_CheckPTXDKGTx_AcceptValidPayload)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    PTX_TEST_ClearSkShareSlot();
    CMutableTransaction mtx_out;
    BOOST_REQUIRE(PTX_DKG_ClosePhase5(sessions[0], 1000, mtx_out));

    CTransaction tx(mtx_out);
    CValidationState state;
    LOCK(cs_main); // CheckPTXDKGTx now requires cs_main (EXCLUSIVE_LOCKS_REQUIRED); null path is structural-only
    BOOST_CHECK(CheckPTXDKGTx(tx, nullptr, state));
}

// ---------------------------------------------------------------------------
// P5_CheckPTXDKGTx_RejectBadGroupPk
//
// Corrupted group_pk_bytes in the payload → CheckPTXDKGTx rejects.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_CheckPTXDKGTx_RejectBadGroupPk)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);

    // Deserialize payload, corrupt group_pk_bytes, re-serialize.
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));
    std::memset(payload.group_pk_bytes, 0xFF, 48);  // invalid compressed G1 point
    SetTxPayload(mtx, payload);

    CTransaction tx(mtx);
    CValidationState state;
    LOCK(cs_main); // CheckPTXDKGTx now requires cs_main (EXCLUSIVE_LOCKS_REQUIRED); null path is structural-only
    BOOST_CHECK(!CheckPTXDKGTx(tx, nullptr, state));
}

// ---------------------------------------------------------------------------
// P5_CheckPTXDKGTx_RejectInsufficientPremits
//
// Only 5 premit entries in the payload → CheckPTXDKGTx rejects.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_CheckPTXDKGTx_RejectInsufficientPremits)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);

    // Deserialize payload, remove entries until only 5 remain, re-serialize.
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));
    while ((int)payload.premit_commitments.size() > 5)
        payload.premit_commitments.erase(payload.premit_commitments.begin());
    BOOST_REQUIRE_EQUAL((int)payload.premit_commitments.size(), 5);
    SetTxPayload(mtx, payload);

    CTransaction tx(mtx);
    CValidationState state;
    LOCK(cs_main); // CheckPTXDKGTx now requires cs_main (EXCLUSIVE_LOCKS_REQUIRED); null path is structural-only
    BOOST_CHECK(!CheckPTXDKGTx(tx, nullptr, state));
}

// ---------------------------------------------------------------------------
// C1_ReplayGuard_RefusesOverwriteOfSetShare  — §C1 replay guard (KDD-057)
//
// The load-bearing property: PTX_BLS_SetSkShare stores a first-set (empty slot)
// but REFUSES to overwrite an already-set share (silent replay / second-
// coordinator takeover defense). Falsification: with the guard removed (setter
// made unconditional), the second set clobbers the live share — the
// "bytes unchanged after refusal" assertion goes RED.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(C1_ReplayGuard_RefusesOverwriteOfSetShare)
{
    PTX_TEST_ClearSkShareSlot();

    // First set into an empty slot: accepted, bytes stored.
    uint8_t first[32];
    std::memset(first, 0x11, 32);
    std::string err1;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(first, err1));
    BOOST_CHECK(err1.empty());
    {
        LOCK(cs_ptx_my_bls_sk);
        BOOST_REQUIRE(g_ptx_my_bls_sk_set);
        BOOST_CHECK_EQUAL_COLLECTIONS(g_ptx_my_bls_sk_bytes, g_ptx_my_bls_sk_bytes + 32,
                                      first, first + 32);
    }

    // Second set into the now-SET slot with DIFFERENT bytes: REFUSED, err set,
    // and the live share is UNCHANGED. (Guard stubbed → this set succeeds and
    // clobbers → the collection assertion below flips RED.)
    uint8_t second[32];
    std::memset(second, 0x22, 32);
    std::string err2;
    BOOST_CHECK(!PTX_BLS_SetSkShare(second, err2));
    BOOST_CHECK(!err2.empty());
    {
        LOCK(cs_ptx_my_bls_sk);
        BOOST_CHECK(g_ptx_my_bls_sk_set);
        // Load-bearing: the original share survived the refused overwrite.
        BOOST_CHECK_EQUAL_COLLECTIONS(g_ptx_my_bls_sk_bytes, g_ptx_my_bls_sk_bytes + 32,
                                      first, first + 32);
    }

    PTX_TEST_ClearSkShareSlot(); // leave the process-global clean for later tests
}

BOOST_AUTO_TEST_SUITE_END()
