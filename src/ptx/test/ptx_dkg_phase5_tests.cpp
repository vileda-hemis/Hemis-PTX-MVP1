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
#include "ptx/ptx_quorum_store.h"   // KDD-070 P2: records + PTX_WarnMissingSharesForNode
#include "evo/evodb.h"              // KDD-070 P2: the in-memory evoDb from BasicTestingSetup
#include "evo/specialtx_validation.h"
#include "consensus/validation.h"
#include "primitives/transaction.h"

#include "bls/bls_wrapper.h"
#include "fs.h"                     // KDD-070 P5: iterate src/rpc for the structural check
#include "random.h"
#include "sync.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <fstream>                  // KDD-070 P5: read source files for the structural check
#include <map>
#include <sstream>
#include <string>
#include <vector>

// KDD-070 P5: absolute source-tree root, injected at build time
// (-DPTX_SRCDIR=\"$(abs_top_srcdir)\" in Makefile.test.include). If a build ever
// omits it, the fallback is an EMPTY SENTINEL — the test detects the empty path
// and HARD-FAILS (BOOST_REQUIRE) with a "could not run" message, so a missing
// define surfaces as a red test, NEVER as a silent skip or a vacuous pass.
#ifndef PTX_SRCDIR
#define PTX_SRCDIR ""
#endif

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

// §C1 (KDD-057; keyed KDD-070 P1): the sk-share store is a process-global map
// guarded by per-key refuse-unless-empty (PTX_BLS_SetSkShare). Tests that store
// must start from an EMPTY store — a prior test in the shared binary may have
// left a key set, which the guard would (correctly) refuse. Clear before each.
static void PTX_TEST_ClearSkShareSlot()
{
    LOCK(cs_ptx_my_bls_sk);
    g_ptx_my_shares.clear();
    g_ptx_memory_only_shares.clear();
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
    BOOST_REQUIRE(PTX_DKG_StoreSkShare(sessions[0], 1000));

    // Read back bytes from the keyed store under the lock (KDD-070 P1).
    uint8_t stored_bytes[32];
    {
        LOCK(cs_ptx_my_bls_sk);
        auto it = g_ptx_my_shares.find(sessions[0].quorum_hash);
        BOOST_REQUIRE(it != g_ptx_my_shares.end());
        BOOST_REQUIRE(it->second.role == PTXShareRole::CURRENT);
        std::memcpy(stored_bytes, it->second.bytes, 32);
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
// P5_IndexSpace_AlphabeticalFailsScoreOrderVerifies   — SG-3 FIRST SUB-GATE
//
// THE INDEX-SPACE DISCRIMINATION TEST (the RED twin of the case above).
//
// SEAM-CLOSED REGRESSION (post-KDD-069). The alphabetical index basis was the
// retired trusted dealer's: it assigned Lagrange x by ALPHABETICAL node_id
// order (the removed PTX_BLS_Init). The DKG evaluates f(share_index) with
// share_index in CalculateQuorum SCORE order (KDD-052; PTX_DKG_InitSession
// ptx_dkg.cpp:261 -> GenerateLocalContrib :318). Interpolating DKG shares at
// alphabetical x's produces wrong Lagrange coefficients and a signature that
// MUST NOT verify. With the dealer retired (KDD-069) the alphabetical basis no
// longer exists in production — the index-space seam is STRUCTURALLY IMPOSSIBLE,
// not merely reconciled; this test reconstructs the dead basis in-test purely to
// document why it does not verify.
//
// GREEN: score-order share_index                 -> verifies against the DKG group_pk.
// RED:   alphabetical (retired dealer's basis)    -> does NOT verify.
//
// ★★ STRUCTURAL ASSERT (non-negotiable): the two orderings must actually
// DIFFER for this member set, else the RED leg is vacuous and the test would
// silently prove nothing if a future member set happened to coincide.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_IndexSpace_AlphabeticalFailsScoreOrderVerifies)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    uint8_t group_pk_bytes[48];
    blst_p1_affine_compress(group_pk_bytes, &sessions[0].group_pk);

    uint256 test_msg;
    {
        std::vector<unsigned char> buf(32, 0xCD);
        test_msg = uint256(buf);
    }

    // The retired dealer's convention (KDD-069), reconstructed in-test over the
    // SAME member set: node_ids sorted alphabetically, 1-indexed position.
    std::vector<std::string> sorted_ids;
    for (const auto& m : sessions[0].members) sorted_ids.push_back(m.node_id);
    std::sort(sorted_ids.begin(), sorted_ids.end());
    std::map<std::string, int> alpha_index;
    for (size_t i = 0; i < sorted_ids.size(); i++)
        alpha_index[sorted_ids[i]] = (int)i + 1;

    const int t = 6;
    std::vector<int> score_indices;
    std::vector<int> alpha_indices;
    std::vector<std::vector<uint8_t>> partial_sigs;

    for (int i = 0; i < t; i++) {
        const PTXDKGMember& me = sessions[i].members[sessions[i].my_idx];
        score_indices.push_back(me.share_index);
        alpha_indices.push_back(alpha_index.at(me.node_id));

        uint8_t sk_bytes[32];
        blst_bendian_from_scalar(sk_bytes, &sessions[i].sk_share_i);

        uint8_t sig_buf[PTX_SIG_BYTES];
        BOOST_REQUIRE(PTX_BLS_PartialSign(sk_bytes, test_msg, sig_buf));
        partial_sigs.push_back(std::vector<uint8_t>(sig_buf, sig_buf + PTX_SIG_BYTES));
    }

    // ★★ THE STRUCTURAL ASSERT — the test can never go vacuous.
    BOOST_REQUIRE_MESSAGE(
        score_indices != alpha_indices,
        "orderings coincide for this quorum — test cannot discriminate, pick another "
        "member set (the alphabetical RED leg would be vacuous)");

    // GREEN — the x the shares were generated at.
    uint8_t sig_score[PTX_SIG_BYTES];
    BOOST_REQUIRE(PTX_BLS_Recover(score_indices, partial_sigs, sig_score));
    BOOST_CHECK_MESSAGE(PTX_BLS_Verify(group_pk_bytes, test_msg, sig_score),
                        "score-order share_index MUST verify against the DKG group_pk");

    // RED — identical partial sigs, dealer's alphabetical x's.
    uint8_t sig_alpha[PTX_SIG_BYTES];
    BOOST_REQUIRE(PTX_BLS_Recover(alpha_indices, partial_sigs, sig_alpha));
    BOOST_CHECK_MESSAGE(!PTX_BLS_Verify(group_pk_bytes, test_msg, sig_alpha),
                        "alphabetical ordering MUST NOT verify — the index spaces are "
                        "not interchangeable (SG-3 seam)");

    // The two recoveries must also differ byte-wise (wrong lambdas -> wrong point).
    BOOST_CHECK(memcmp(sig_score, sig_alpha, PTX_SIG_BYTES) != 0);
}

// ---------------------------------------------------------------------------
// P5_ReSelection_SecondStoreRefusedAborts   — the 2320 MECHANISM
//
// §C1 (KDD-057) makes the sk-share slot REFUSE-UNLESS-EMPTY
// (PTX_BLS_SetSkShare, ptx_bls.h:74-79).  PTX_DKG_StoreSkShare routes through
// it (ptx_dkg.cpp:1338) and ClosePhase5 turns a refusal into phase = ABORTED
// (ptx_dkg.h:729-32).  So a member that ALREADY holds a share and is
// re-selected into a new quorum aborts at FINALIZE — it cannot complete.
//
// This is the predicted mechanism of the h2320 warm-mesh sub-threshold: h2240
// converged (shares stored) but never LANDED, so it never became ACTIVE, so
// KDD-040 exclusion did not apply, so the SAME 11 were re-selected at 2320 and
// every one of them refused.  (Post-KDD-058-A quorums land -> ACTIVE ->
// excluded -> fresh membership, which is why no refusal has been seen since.)
//
// Deterministic here; on a live fleet it is currently unforceable — with two
// ACTIVE quorums the pool is empty and formation correctly deterministic-skips.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P5_ReSelection_SecondStoreRefusedAborts)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    // First close on an EMPTY store: stores under sessions[0].quorum_hash, DONE.
    PTX_TEST_ClearSkShareSlot();
    CMutableTransaction tx_first;
    BOOST_REQUIRE(PTX_DKG_ClosePhase5(sessions[0], 1000, tx_first));
    BOOST_REQUIRE(sessions[0].phase == PTXDKGPhase::DONE);
    BOOST_REQUIRE(g_ptx_my_shares.count(sessions[0].quorum_hash) == 1);

    // Second close for the SAME quorum_hash (sessions[1] is another member of the
    // same ceremony → same key) WITHOUT clearing: §C1 per-key refuse-unless-empty
    // refuses the overwrite, so the ceremony ABORTS at FINALIZE. KDD-070 P1 note:
    // the refusal is now SCOPED TO THE SAME quorum_hash (a same-quorum replay /
    // double-store); a re-selection into a DIFFERENT quorum now SUCCEEDS (distinct
    // key) — see Slot_TwoQuorumsCoexist_NoCrosstalk. This narrows the KDD-067
    // burnout from process-lifetime to per-quorum.
    BOOST_REQUIRE(sessions[1].quorum_hash == sessions[0].quorum_hash); // same ceremony
    CMutableTransaction tx_second;
    BOOST_CHECK_MESSAGE(!PTX_DKG_ClosePhase5(sessions[1], 2000, tx_second),
                        "a second store for the SAME quorum_hash MUST fail to close "
                        "(sk_share is per-key refuse-unless-empty, §C1)");
    BOOST_CHECK_MESSAGE(sessions[1].phase == PTXDKGPhase::ABORTED,
                        "refused StoreSkShare MUST drive phase = ABORTED (the 2320 mechanism)");

    PTX_TEST_ClearSkShareSlot();
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

    uint256 qh;
    std::memset(qh.begin(), 0xA1, 32);   // a fixed quorum_hash key

    // First set into an empty key: accepted, bytes stored (role CURRENT).
    uint8_t first[32];
    std::memset(first, 0x11, 32);
    std::string err1;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qh, 100, first, err1));
    BOOST_CHECK(err1.empty());
    {
        LOCK(cs_ptx_my_bls_sk);
        auto it = g_ptx_my_shares.find(qh);
        BOOST_REQUIRE(it != g_ptx_my_shares.end());
        BOOST_CHECK(it->second.role == PTXShareRole::CURRENT);
        BOOST_CHECK_EQUAL_COLLECTIONS(it->second.bytes, it->second.bytes + 32,
                                      first, first + 32);
    }

    // Second set into the now-SET key with DIFFERENT bytes: REFUSED, err set,
    // and the live share is UNCHANGED. (Guard stubbed → this set succeeds and
    // clobbers → the collection assertion below flips RED.)
    uint8_t second[32];
    std::memset(second, 0x22, 32);
    std::string err2;
    BOOST_CHECK(!PTX_BLS_SetSkShare(qh, 200, second, err2));
    BOOST_CHECK(!err2.empty());
    {
        LOCK(cs_ptx_my_bls_sk);
        auto it = g_ptx_my_shares.find(qh);
        BOOST_REQUIRE(it != g_ptx_my_shares.end());
        // Load-bearing: the original share survived the refused overwrite.
        BOOST_CHECK_EQUAL_COLLECTIONS(it->second.bytes, it->second.bytes + 32,
                                      first, first + 32);
    }

    PTX_TEST_ClearSkShareSlot(); // leave the process-global store clean for later tests
}

// ===========================================================================
// KDD-070 P1 — keyed share store (map<quorum_hash, HeldShare>), CURRENT-only,
// keyed selection via PTX_BLS_GetCurrentShare. Each test RED-provable by
// inversion (noted per case).
// ===========================================================================

namespace {
uint256 QHk(uint8_t fill) { uint256 h; std::memset(h.begin(), fill, 32); return h; }
}

// set then get-by-key round-trips, and PartialSign with the retrieved bytes
// equals PartialSign with the original — i.e. selection returns the RIGHT share.
// RED (inversion): had GetCurrentShare returned different bytes, the two sigs
// would differ and the equality check fails.
BOOST_AUTO_TEST_CASE(Slot_SetThenGetPerKey_Signs)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0x5A);
    uint8_t share[32]; std::memset(share, 0x3C, 32);
    std::string err;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qh, 100, share, err));

    uint8_t got[32];
    BOOST_REQUIRE(PTX_BLS_GetCurrentShare(qh, got));
    BOOST_CHECK_EQUAL_COLLECTIONS(got, got + 32, share, share + 32);

    uint256 msg = QHk(0xEE);
    uint8_t sig_got[PTX_SIG_BYTES], sig_ref[PTX_SIG_BYTES];
    BOOST_REQUIRE(PTX_BLS_PartialSign(got, msg, sig_got));
    BOOST_REQUIRE(PTX_BLS_PartialSign(share, msg, sig_ref));
    BOOST_CHECK_EQUAL_COLLECTIONS(sig_got, sig_got + PTX_SIG_BYTES,
                                  sig_ref, sig_ref + PTX_SIG_BYTES);
    PTX_TEST_ClearSkShareSlot();
}

// signing an UNHELD quorum_hash returns false — never a wrong-key signature.
// RED (inversion): a fallback-to-any-share implementation returns true here.
BOOST_AUTO_TEST_CASE(Slot_SignUnheldQuorum_ReturnsFalse)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 held = QHk(0x01), unheld = QHk(0x02);
    uint8_t share[32]; std::memset(share, 0x7F, 32);
    std::string err;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(held, 100, share, err));

    uint8_t out[32]; std::memset(out, 0xCC, 32);
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(unheld, out));           // not held → false
    // out must be untouched (no wrong-key share leaked into it).
    for (int i = 0; i < 32; ++i) BOOST_CHECK_EQUAL(out[i], 0xCC);
    PTX_TEST_ClearSkShareSlot();
}

// per-key refuse-unless-empty: a second write to the SAME key is refused while a
// DIFFERENT key is accepted. RED (inversion): a global (unkeyed) guard would
// refuse the second key too; a missing guard would accept the same-key rewrite.
BOOST_AUTO_TEST_CASE(Slot_RefuseUnlessEmptyPerKey)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 qa = QHk(0xA0), qb = QHk(0xB0);
    uint8_t s1[32]; std::memset(s1, 0x11, 32);
    uint8_t s2[32]; std::memset(s2, 0x22, 32);
    std::string e1, e2, e3;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qa, 100, s1, e1));           // first set QA: ok
    BOOST_CHECK(!PTX_BLS_SetSkShare(qa, 100, s2, e2));            // second set QA: refused
    BOOST_CHECK(!e2.empty());
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qb, 100, s2, e3));           // set QB (distinct key): ok
    BOOST_CHECK(e3.empty());
    PTX_TEST_ClearSkShareSlot();
}

// two quorums coexist with NO cross-talk: GetCurrentShare(A) returns A's share,
// (B) returns B's. RED (inversion): a single-slot or last-write-wins store
// returns B for both.
BOOST_AUTO_TEST_CASE(Slot_TwoQuorumsCoexist_NoCrosstalk)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 qa = QHk(0xAA), qb = QHk(0xBB);
    uint8_t sa[32]; std::memset(sa, 0xA5, 32);
    uint8_t sb[32]; std::memset(sb, 0x5B, 32);
    std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qa, 100, sa, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qb, 200, sb, e));

    uint8_t ga[32], gb[32];
    BOOST_REQUIRE(PTX_BLS_GetCurrentShare(qa, ga));
    BOOST_REQUIRE(PTX_BLS_GetCurrentShare(qb, gb));
    BOOST_CHECK_EQUAL_COLLECTIONS(ga, ga + 32, sa, sa + 32);      // A → A's share
    BOOST_CHECK_EQUAL_COLLECTIONS(gb, gb + 32, sb, sb + 32);      // B → B's share
    BOOST_CHECK(std::memcmp(ga, gb, 32) != 0);                    // and they differ
    PTX_TEST_ClearSkShareSlot();
}

// selection is by key regardless of insertion order (A-then-B vs B-then-A).
// RED (inversion): an order-dependent (e.g. index-based) store returns the
// wrong share under one of the two orderings.
BOOST_AUTO_TEST_CASE(Slot_SelectionByKey_BothOrderings)
{
    uint256 qa = QHk(0x1A), qb = QHk(0x2B);
    uint8_t sa[32]; std::memset(sa, 0x1A, 32);
    uint8_t sb[32]; std::memset(sb, 0x2B, 32);
    std::string e;
    uint8_t ga[32], gb[32];

    // Ordering 1: A then B.
    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qa, 1, sa, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qb, 2, sb, e));
    BOOST_REQUIRE(PTX_BLS_GetCurrentShare(qa, ga));
    BOOST_REQUIRE(PTX_BLS_GetCurrentShare(qb, gb));
    BOOST_CHECK_EQUAL_COLLECTIONS(ga, ga + 32, sa, sa + 32);
    BOOST_CHECK_EQUAL_COLLECTIONS(gb, gb + 32, sb, sb + 32);

    // Ordering 2: B then A (fresh store).
    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qb, 2, sb, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qa, 1, sa, e));
    BOOST_REQUIRE(PTX_BLS_GetCurrentShare(qa, ga));
    BOOST_REQUIRE(PTX_BLS_GetCurrentShare(qb, gb));
    BOOST_CHECK_EQUAL_COLLECTIONS(ga, ga + 32, sa, sa + 32);
    BOOST_CHECK_EQUAL_COLLECTIONS(gb, gb + 32, sb, sb + 32);
    PTX_TEST_ClearSkShareSlot();
}

// ===========================================================================
// KDD-070 P2 — persistence serializer, reconciliation, in_qual warn, wipe.
// Pure functions (no evoDb) — the evoDb read/write wiring and the on-start
// trigger are inspection-only (P2 CANNOT cover; bound to Package 3, ODC-032).
// ===========================================================================

// serializer round-trips all four fields byte-identically.
// RED (inversion): drop/relocate any field in Serialize/Deserialize -> a field
// mismatches after the round-trip.
BOOST_AUTO_TEST_CASE(P2_SerializeHeldShare_RoundTrip)
{
    HeldShare in;
    for (int i = 0; i < 32; ++i) in.bytes[i] = (uint8_t)(0x40 + i);
    in.formation_height = 123456;
    in.role             = PTXShareRole::SUPERSEDED_RETAINED;   // exercise a non-zero role
    in.promotion_height = -7;

    std::vector<uint8_t> blob = PTX_BLS_SerializeHeldShare(in);
    BOOST_REQUIRE_EQUAL(blob.size(), 41u);
    HeldShare out;
    BOOST_REQUIRE(PTX_BLS_DeserializeHeldShare(blob, out));
    BOOST_CHECK_EQUAL_COLLECTIONS(out.bytes, out.bytes + 32, in.bytes, in.bytes + 32);
    BOOST_CHECK_EQUAL(out.formation_height, in.formation_height);
    BOOST_CHECK(out.role == in.role);
    BOOST_CHECK_EQUAL(out.promotion_height, in.promotion_height);
    // wrong-size blob rejected.
    BOOST_CHECK(!PTX_BLS_DeserializeHeldShare(std::vector<uint8_t>(40, 0), out));
}

// reconcile DROPS an orphan (held quorum_hash absent from the known set).
// RED (inversion): a reconcile that keeps everything leaves the orphan -> the
// post-count assertion fails.
BOOST_AUTO_TEST_CASE(P2_Reconcile_DropsOrphan)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 live = QHk(0x11), orphan = QHk(0x22);
    uint8_t s[32]; std::memset(s, 0x33, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(live, 1, s, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(orphan, 2, s, e));

    std::set<uint256> known{live};                      // orphan NOT known
    BOOST_CHECK_EQUAL(PTX_BLS_ReconcileShares(known), 1u);  // exactly one dropped
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(live, out));     // live kept
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(orphan, out));  // orphan gone
    PTX_TEST_ClearSkShareSlot();
}

// reconcile KEEPS a live share (quorum_hash present in the known set).
// RED (inversion): a reconcile that drops present keys removes it -> Get fails.
BOOST_AUTO_TEST_CASE(P2_Reconcile_KeepsLive)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 live = QHk(0x44);
    uint8_t s[32]; std::memset(s, 0x55, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(live, 1, s, e));
    std::set<uint256> known{live};
    BOOST_CHECK_EQUAL(PTX_BLS_ReconcileShares(known), 0u);  // nothing dropped
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(live, out));
    PTX_TEST_ClearSkShareSlot();
}

// reconcile with an EMPTY known set discards ALL — the init-order failure mode
// in miniature (an unpopulated store would orphan every member, §3). This is
// the CORRECT behaviour of the pure function; the ordering fix (running only
// after the store is populated) is what prevents it in production.
// RED (inversion): a reconcile that special-cases the empty set to keep-all
// would leave shares held -> the count/Get assertions fail.
BOOST_AUTO_TEST_CASE(P2_Reconcile_EmptySetDiscardsAll)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 a = QHk(0x66), b = QHk(0x77);
    uint8_t s[32]; std::memset(s, 0x88, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(a, 1, s, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(b, 2, s, e));
    std::set<uint256> empty;
    BOOST_CHECK_EQUAL(PTX_BLS_ReconcileShares(empty), 2u);  // ALL discarded
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(a, out));
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(b, out));
    PTX_TEST_ClearSkShareSlot();
}

// "holds nothing, in_qual": warns and does NOT throw. Returns one warning for a
// quorum where this node is in_qual but no share is held.
// RED (inversion): a warn that keys on membership-without-in_qual, or that skips
// the held-check, returns the wrong count.
BOOST_AUTO_TEST_CASE(P2_WarnMissing_InQualNoShare_LogsNoThrow)
{
    PTX_TEST_ClearSkShareSlot();
    const std::string me = "node-me:aa";

    CPTXQuorumRecord rec;
    rec.quorum_hash = QHk(0x99);
    PTXQuorumMemberRecord m; m.node_id = me; m.in_qual = true; m.share_index = 3;
    rec.members.push_back(m);
    std::vector<CPTXQuorumRecord> active{rec};

    int warned = 0;
    BOOST_REQUIRE_NO_THROW(warned = PTX_WarnMissingSharesForNode(active, me));
    BOOST_CHECK_EQUAL(warned, 1);   // in_qual + no share held -> one warning

    // holding the share suppresses the warning.
    uint8_t s[32]; std::memset(s, 0xAB, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(rec.quorum_hash, 1, s, e));
    BOOST_CHECK_EQUAL(PTX_WarnMissingSharesForNode(active, me), 0);
    PTX_TEST_ClearSkShareSlot();
}

// wipe clears ALL held shares (memory-only path, evoDb == nullptr).
// RED (inversion): a wipe that clears nothing leaves shares held -> Get succeeds.
BOOST_AUTO_TEST_CASE(P2_Wipe_ClearsAll)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 a = QHk(0xC0), b = QHk(0xC1);
    uint8_t s[32]; std::memset(s, 0xCE, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(a, 1, s, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(b, 2, s, e));
    BOOST_CHECK_EQUAL(PTX_BLS_WipeShares(nullptr), 2u);   // both cleared
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(a, out));
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(b, out));
    PTX_TEST_ClearSkShareSlot();
}

// ===========================================================================
// KDD-070 P2 — REAL persistence tests over the in-memory CEvoDB that
// BasicTestingSetup provides (evoDb = CEvoDB(1<<20, fMemory, fWipe),
// test_Hemis.cpp). These exercise the actual disk read/write/erase path.
// ===========================================================================

// Persist -> clear map -> LoadShares -> the share returns from disk, all fields.
// RED (inversion): a PersistShare that no-ops, or a LoadShares that doesn't
// repopulate, leaves the map empty -> GetCurrentShare fails.
BOOST_AUTO_TEST_CASE(P2_Persist_Load_RoundTrip)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0xD0);
    HeldShare hs;
    std::memset(hs.bytes, 0xD1, 32);
    hs.formation_height = 4242;
    hs.role             = PTXShareRole::CURRENT;
    hs.promotion_height = -1;
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, qh, hs));   // Write returned true

    PTX_TEST_ClearSkShareSlot();                            // wipe the MAP only
    BOOST_CHECK_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);       // 0 corrupt
    uint8_t out[32];
    BOOST_REQUIRE(PTX_BLS_GetCurrentShare(qh, out));        // reloaded from disk
    BOOST_CHECK_EQUAL_COLLECTIONS(out, out + 32, hs.bytes, hs.bytes + 32);
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// Persist -> WipeShares(evoDb) -> LoadShares -> nothing (disk cleared).
// RED (inversion): a wipe that clears memory but not disk -> LoadShares reloads
// the share -> GetCurrentShare succeeds.
BOOST_AUTO_TEST_CASE(P2_Persist_Wipe_Load_Nothing)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0xD2);
    HeldShare hs; std::memset(hs.bytes, 0xD3, 32); hs.role = PTXShareRole::CURRENT;
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, qh, hs));

    PTX_BLS_WipeShares(evoDb.get());          // erase memory + disk
    PTX_TEST_ClearSkShareSlot();              // ensure map empty
    BOOST_CHECK_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(qh, out));   // nothing reloaded
    PTX_TEST_ClearSkShareSlot();
}

// Persist two -> reconcile(known={live}, evoDb) -> clear map -> LoadShares ->
// only the live one; the ORPHAN IS GONE FROM DISK (would otherwise reload).
// RED (inversion): a reconcile that erases only the map (not disk) -> the orphan
// reloads on LoadShares -> GetCurrentShare(orphan) succeeds.
BOOST_AUTO_TEST_CASE(P2_Persist_Reconcile_OrphanGoneFromDisk)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 live = QHk(0xE0), orphan = QHk(0xE1);
    HeldShare a; std::memset(a.bytes, 0xEA, 32); a.role = PTXShareRole::CURRENT;
    HeldShare b; std::memset(b.bytes, 0xEB, 32); b.role = PTXShareRole::CURRENT;
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, live, a));
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, orphan, b));
    // populate the map to match a real start (LoadShares would have done this).
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);

    std::set<uint256> known{live};
    BOOST_CHECK_EQUAL(PTX_BLS_ReconcileShares(known, evoDb.get()), 1u);  // orphan dropped

    PTX_TEST_ClearSkShareSlot();                        // wipe map; disk is the truth now
    BOOST_CHECK_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(live, out));    // live still on disk
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(orphan, out)); // orphan ERASED from disk
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// (e) LoadShares REPORTS a corrupt/malformed on-disk blob (does not swallow it):
// nonzero corrupt count, and the share is NOT loaded.
// RED (inversion): a LoadShares that returns 0 / loads garbage -> the count
// assertion (or the not-loaded assertion) fails.
BOOST_AUTO_TEST_CASE(P2_LoadShares_CorruptReported)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0xF0);
    // inject a malformed blob (wrong size) directly under the share prefix.
    std::vector<uint8_t> bad(10, 0xFF);
    BOOST_REQUIRE(evoDb->GetRawDB().Write(std::make_pair(PTX_BLS_ShareDBPrefix(), qh), bad));

    BOOST_CHECK_EQUAL(PTX_BLS_LoadShares(*evoDb), 1);     // reported, not swallowed
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(qh, out));       // corrupt entry NOT loaded
    PTX_BLS_WipeShares(evoDb.get());                      // cleans the corrupt entry too
    PTX_TEST_ClearSkShareSlot();
}

// (item 1) memory-only tracking: when a persist fails and the share is kept
// (the (b) degraded path), the quorum is reported as memory-only — so a node
// can report its degraded state without anyone having seen the ceremony-time
// ERROR line. A durably-persisted share is NOT reported.
// RED (inversion): a MarkMemoryOnly that no-ops, or a report that returns empty,
// leaves the degraded quorum unreported.
BOOST_AUTO_TEST_CASE(P2_MemoryOnly_Tracked_And_Reported)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 degraded = QHk(0xB1), durable = QHk(0xB2);
    uint8_t s[32]; std::memset(s, 0x7A, 32); std::string e;

    // Both shares are held; only `degraded` had its persist fail (the (b) path,
    // simulated here by marking it — StoreSkShare calls MarkMemoryOnly on a
    // PersistShare failure).
    BOOST_REQUIRE(PTX_BLS_SetSkShare(degraded, 1, s, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(durable,  2, s, e));
    PTX_BLS_MarkMemoryOnly(degraded);

    std::set<uint256> mo = PTX_BLS_MemoryOnlyShares();
    BOOST_CHECK_EQUAL(mo.size(), 1u);
    BOOST_CHECK(mo.count(degraded) == 1);   // degraded reported
    BOOST_CHECK(mo.count(durable)  == 0);   // durable NOT reported

    // reconcile-discarding the degraded quorum clears its memory-only mark.
    std::set<uint256> known{durable};
    BOOST_CHECK_EQUAL(PTX_BLS_ReconcileShares(known), 1u);
    BOOST_CHECK(PTX_BLS_MemoryOnlyShares().count(degraded) == 0);
    PTX_TEST_ClearSkShareSlot();
}

// ===========================================================================
// KDD-070 P3 — PENDING role, promotion, TTL expiry, key isolation.
// ===========================================================================

namespace {
// held role of a quorum_hash (returns false if not held).
bool P3_held_role(const uint256& qh, PTXShareRole& out)
{
    LOCK(cs_ptx_my_bls_sk);
    auto it = g_ptx_my_shares.find(qh);
    if (it == g_ptx_my_shares.end()) return false;
    out = it->second.role;
    return true;
}
int P3_promo_height(const uint256& qh)
{
    LOCK(cs_ptx_my_bls_sk);
    auto it = g_ptx_my_shares.find(qh);
    return it == g_ptx_my_shares.end() ? -999 : it->second.promotion_height;
}
} // namespace

// store -> PENDING: held (in the map with role PENDING) but NOT signable.
// RED (inversion): a SetSkShare that stores CURRENT regardless of role -> the
// share becomes signable and role != PENDING.
BOOST_AUTO_TEST_CASE(P3_StorePending_HeldButNotSignable)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0x31);
    uint8_t s[32]; std::memset(s, 0x31, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qh, 100, s, PTXShareRole::PENDING, e));
    PTXShareRole r;
    BOOST_REQUIRE(P3_held_role(qh, r));
    BOOST_CHECK(r == PTXShareRole::PENDING);            // held as PENDING
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(qh, out));     // NOT signable
    PTX_TEST_ClearSkShareSlot();
}

// signing a PENDING quorum_hash is refused (GetCurrentShare false) — the gm_bls_sign
// handler turns that into a hard error naming the quorum.
// RED (inversion): a GetCurrentShare that ignores role returns the pending bytes.
BOOST_AUTO_TEST_CASE(P3_SignPending_Refused)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0x32);
    uint8_t s[32]; std::memset(s, 0x32, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qh, 100, s, PTXShareRole::PENDING, e));
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(qh, out));     // PENDING refuses to sign
    PTX_TEST_ClearSkShareSlot();
}

// promote(successor): PENDING(succ)->CURRENT and CURRENT(pred)->SUPERSEDED_RETAINED
// with promotion_height stamped = connect height. Function-level.
// RED (inversion): a promote that only flips the successor (leaves pred CURRENT)
// or stamps no height -> the SUPERSEDED/height assertions fail.
BOOST_AUTO_TEST_CASE(P3_Promote_FlipsBothRoles_StampsHeight)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x33), succ = QHk(0x34);
    uint8_t s[32]; std::memset(s, 0x33, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(pred, 100, s, PTXShareRole::CURRENT, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(succ, 140, s, PTXShareRole::PENDING, e));

    BOOST_CHECK_EQUAL(PTX_BLS_Promote(succ, pred, 150), 1u);
    PTXShareRole rs, rp;
    BOOST_REQUIRE(P3_held_role(succ, rs)); BOOST_CHECK(rs == PTXShareRole::CURRENT);
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_CHECK_EQUAL(P3_promo_height(pred), 150);      // stamped = connect height
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(succ, out));    // successor now signs
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(pred, out));   // old no longer signs
    PTX_TEST_ClearSkShareSlot();
}

// ★ key isolation: promote for a quorum Y with no PENDING is a NO-OP and does
// NOT touch PENDING(X). Not a best-effort match.
// RED (inversion): a promote that promotes the "first PENDING" instead of the
// keyed one flips X.
BOOST_AUTO_TEST_CASE(P3_Promote_KeyIsolation_NoOpOnMismatch)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 x = QHk(0x35), y = QHk(0x36), pred = QHk(0x37);
    uint8_t s[32]; std::memset(s, 0x35, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(pred, 100, s, PTXShareRole::CURRENT, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(x,    140, s, PTXShareRole::PENDING, e));

    BOOST_CHECK_EQUAL(PTX_BLS_Promote(y, pred, 150), 0u);   // no PENDING(y) -> no-op
    PTXShareRole rx, rp;
    BOOST_REQUIRE(P3_held_role(x, rx)); BOOST_CHECK(rx == PTXShareRole::PENDING);        // X untouched
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::CURRENT);     // pred untouched
    PTX_TEST_ClearSkShareSlot();
}

// second store-pending refused while a PENDING exists (item 3, KDD-040 rule).
// RED (inversion): a setter without the one-PENDING guard accepts the second.
BOOST_AUTO_TEST_CASE(P3_SecondPending_Refused)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 b = QHk(0x38), c = QHk(0x39);
    uint8_t s[32]; std::memset(s, 0x38, 32); std::string e1, e2;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(b, 100, s, PTXShareRole::PENDING, e1));   // first ok
    BOOST_CHECK(!PTX_BLS_SetSkShare(c, 100, s, PTXShareRole::PENDING, e2));    // second refused
    BOOST_CHECK(!e2.empty());
    PTXShareRole r;
    BOOST_CHECK(!P3_held_role(c, r));   // C not stored
    PTX_TEST_ClearSkShareSlot();
}

// TTL boundary: at exactly TTL kept, past TTL expired.
// RED (inversion): a `>=` instead of `>` expires at the boundary; a `+1` slip
// keeps it one block too long.
BOOST_AUTO_TEST_CASE(P3_TTL_Boundary)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0x3A);
    uint8_t s[32]; std::memset(s, 0x3A, 32); std::string e;
    const int fh = 1000;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qh, fh, s, PTXShareRole::PENDING, e));

    // age == TTL -> kept (not > TTL).
    BOOST_CHECK_EQUAL(PTX_BLS_ExpirePending(fh + PTX_PENDING_TTL_BLOCKS), 0u);
    PTXShareRole r; BOOST_CHECK(P3_held_role(qh, r));
    // age == TTL+1 -> expired.
    BOOST_CHECK_EQUAL(PTX_BLS_ExpirePending(fh + PTX_PENDING_TTL_BLOCKS + 1), 1u);
    BOOST_CHECK(!P3_held_role(qh, r));
    PTX_TEST_ClearSkShareSlot();
}

// expired PENDING is gone from DISK, not just memory (P2 defect (a) not repeated).
// RED (inversion): an expiry that erases only the map -> the PENDING reloads.
BOOST_AUTO_TEST_CASE(P3_ExpiredPending_GoneFromDisk)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0x3B);
    HeldShare hs; std::memset(hs.bytes, 0x3B, 32); hs.role = PTXShareRole::PENDING;
    hs.formation_height = 1000;
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, qh, hs));
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);       // held as PENDING

    BOOST_CHECK_EQUAL(PTX_BLS_ExpirePending(1000 + PTX_PENDING_TTL_BLOCKS + 1, evoDb.get()), 1u);
    PTX_TEST_ClearSkShareSlot();                              // wipe map; disk is truth
    BOOST_CHECK_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole r;
    BOOST_CHECK(!P3_held_role(qh, r));                        // gone from disk
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// late connect after expiry -> promote finds no PENDING, promotes nothing, no crash.
// RED (inversion): a promote that resurrects/creates a CURRENT for an absent key.
BOOST_AUTO_TEST_CASE(P3_LateConnect_AfterExpiry_NoOp)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 succ = QHk(0x3C), pred = QHk(0x3D);
    uint8_t s[32]; std::memset(s, 0x3C, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(succ, 1000, s, PTXShareRole::PENDING, e));
    BOOST_CHECK_EQUAL(PTX_BLS_ExpirePending(1000 + PTX_PENDING_TTL_BLOCKS + 1), 1u);   // expired

    BOOST_CHECK_EQUAL(PTX_BLS_Promote(succ, pred, 2000), 0u);   // nothing to promote
    PTXShareRole r;
    BOOST_CHECK(!P3_held_role(succ, r));   // not resurrected
    PTX_TEST_ClearSkShareSlot();
}

// PENDING persists and reloads via LoadShares with its role intact.
// RED (inversion): a serializer that drops role -> reloads as CURRENT (default).
BOOST_AUTO_TEST_CASE(P3_Pending_Persists_RoleIntact)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0x3E);
    HeldShare hs; std::memset(hs.bytes, 0x3E, 32); hs.role = PTXShareRole::PENDING;
    hs.formation_height = 1234;
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, qh, hs));

    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole r;
    BOOST_REQUIRE(P3_held_role(qh, r));
    BOOST_CHECK(r == PTXShareRole::PENDING);   // role survived the round trip
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// promotion survives LoadShares — the flipped roles reload as CURRENT/SUPERSEDED.
// RED (inversion): a promote that doesn't RE-PERSIST -> LoadShares reloads the
// pre-promote roles (PENDING successor, CURRENT predecessor).
BOOST_AUTO_TEST_CASE(P3_Promotion_SurvivesLoadShares)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x40), succ = QHk(0x41);
    HeldShare a; std::memset(a.bytes, 0x40, 32); a.role = PTXShareRole::CURRENT; a.formation_height = 100;
    HeldShare b; std::memset(b.bytes, 0x41, 32); b.role = PTXShareRole::PENDING; b.formation_height = 140;
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, pred, a));
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, succ, b));
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);

    BOOST_CHECK_EQUAL(PTX_BLS_Promote(succ, pred, 150, evoDb.get()), 1u);   // re-persists
    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole rs, rp;
    BOOST_REQUIRE(P3_held_role(succ, rs)); BOOST_CHECK(rs == PTXShareRole::CURRENT);
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_CHECK_EQUAL(P3_promo_height(pred), 150);
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// ===========================================================================
// KDD-070 P4 — SUPERSEDED retention window: depth-discard + undo revert.
// ===========================================================================

namespace {
// Build a promoted lineage in memory: CURRENT(pred) + PENDING(succ) -> Promote
// -> SUPERSEDED_RETAINED(pred, promotion_height=connect_h) + CURRENT(succ).
// Returns after asserting the post-promote roles, so each P4 test starts from a
// known-good superseded state without repeating the setup.
void P4_make_promoted(const uint256& pred, const uint256& succ, int connect_h,
                      uint8_t pred_fill, uint8_t succ_fill)
{
    uint8_t sp[32]; std::memset(sp, pred_fill, 32);
    uint8_t ss[32]; std::memset(ss, succ_fill, 32);
    std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(pred, connect_h - 50, sp, e));                     // CURRENT
    BOOST_REQUIRE(PTX_BLS_SetSkShare(succ, connect_h - 10, ss, PTXShareRole::PENDING, e));
    BOOST_REQUIRE_EQUAL(PTX_BLS_Promote(succ, pred, connect_h), 1u);
    PTXShareRole r;
    BOOST_REQUIRE(P3_held_role(pred, r)); BOOST_REQUIRE(r == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_REQUIRE(P3_held_role(succ, r)); BOOST_REQUIRE(r == PTXShareRole::CURRENT);
}
} // namespace

// retention boundary: depth 119 kept, depth 120 discarded (DEFAULT_MAX_REORG_DEPTH
// + PTX_SUPERSEDED_REORG_MARGIN = 100 + 20). DEPTH-based on tip - promotion_height.
// RED (inversion): a discard using `>` instead of `>=` keeps depth 120 (returns 0).
BOOST_AUTO_TEST_CASE(P4_RetentionBoundary_119Kept_120Discarded)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x51), succ = QHk(0x52);
    P4_make_promoted(pred, succ, 150, 0x51, 0x52);   // pred SUPERSEDED, promotion_height=150
    const int depth = DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN;   // 120
    PTXShareRole r;

    // depth 119 (tip = 150 + 119): kept.
    BOOST_CHECK_EQUAL(PTX_BLS_DiscardSuperseded(150 + depth - 1), 0u);
    BOOST_REQUIRE(P3_held_role(pred, r)); BOOST_CHECK(r == PTXShareRole::SUPERSEDED_RETAINED);

    // depth 120 (tip = 150 + 120): discarded.
    BOOST_CHECK_EQUAL(PTX_BLS_DiscardSuperseded(150 + depth), 1u);
    BOOST_CHECK(!P3_held_role(pred, r));
    PTX_TEST_ClearSkShareSlot();
}

// a discarded SUPERSEDED share is gone from DISK, not just memory.
// RED (inversion): a discard that erases memory but not the RAW-DB entry -> the
// share reloads on the next LoadShares.
BOOST_AUTO_TEST_CASE(P4_DiscardedSuperseded_GoneFromDisk)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 qh = QHk(0x53);
    HeldShare hs; std::memset(hs.bytes, 0x53, 32); hs.role = PTXShareRole::SUPERSEDED_RETAINED;
    hs.formation_height = 100; hs.promotion_height = 150;
    BOOST_REQUIRE(PTX_BLS_PersistShare(*evoDb, qh, hs));
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);

    BOOST_CHECK_EQUAL(PTX_BLS_DiscardSuperseded(150 + 120, evoDb.get()), 1u);
    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole r;
    BOOST_CHECK(!P3_held_role(qh, r));   // gone from DISK, did not reload
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// undo revert: SUPERSEDED(pred) -> CURRENT, the reverted CURRENT(succ) discarded,
// promotion_height cleared.
// RED (inversion): an undo that does not clear promotion_height, or does not
// discard the successor, or does not restore the predecessor role.
BOOST_AUTO_TEST_CASE(P4_UndoRevert_RestoresPred_DiscardsSucc_ClearsHeight)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x54), succ = QHk(0x55);
    P4_make_promoted(pred, succ, 150, 0x54, 0x55);

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred), 1u);
    PTXShareRole rp;
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::CURRENT);   // restored
    BOOST_CHECK_EQUAL(P3_promo_height(pred), -1);                                       // height cleared
    PTXShareRole rs;
    BOOST_CHECK(!P3_held_role(succ, rs));                                               // successor discarded
    PTX_TEST_ClearSkShareSlot();
}

// post-revert CURRENT signs; pre-revert SUPERSEDED refuses.
// RED (inversion): an undo that leaves the predecessor non-CURRENT -> the
// post-revert signing read fails.
BOOST_AUTO_TEST_CASE(P4_PostRevert_Signs_PreRevert_Refuses)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x56), succ = QHk(0x57);
    P4_make_promoted(pred, succ, 150, 0x56, 0x57);

    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(pred, out));   // pre-revert: SUPERSEDED refuses

    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(succ, pred), 1u);
    BOOST_CHECK(PTX_BLS_GetCurrentShare(pred, out));    // post-revert: CURRENT signs
    uint8_t sp[32]; std::memset(sp, 0x56, 32);
    BOOST_CHECK(std::memcmp(out, sp, 32) == 0);         // and it is the predecessor's own bytes
    PTX_TEST_ClearSkShareSlot();
}

// undo is idempotent: the second call is a no-op, not a double-revert.
// RED (inversion): an undo that returns 1 without discarding the successor / not
// requiring successor==CURRENT -> a second call reverts again (returns 1).
BOOST_AUTO_TEST_CASE(P4_Undo_Idempotent_SecondCallNoOps)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x58), succ = QHk(0x59);
    P4_make_promoted(pred, succ, 150, 0x58, 0x59);

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred), 1u);   // first: reverts
    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred), 0u);   // second: clean no-op
    PTXShareRole rp;
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::CURRENT);
    BOOST_CHECK_EQUAL(P3_promo_height(pred), -1);
    PTX_TEST_ClearSkShareSlot();
}

// undo for a quorum_hash with no promotion -> clean no-op (returns 0, no error,
// predecessor untouched).
// RED (inversion): an undo that restores whenever the predecessor is present,
// regardless of the successor -> returns 1 for a never-promoted lineage.
BOOST_AUTO_TEST_CASE(P4_Undo_NoPromotion_CleanNoOp)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x5A), succ = QHk(0x5B);
    uint8_t sp[32]; std::memset(sp, 0x5A, 32); std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(pred, 100, sp, e));   // CURRENT, never superseded; succ never held

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred), 0u);   // successor not held -> no-op
    PTXShareRole rp;
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::CURRENT);   // untouched
    PTX_TEST_ClearSkShareSlot();
}

// key isolation on the revert: undo(Y) leaves a promoted lineage X untouched.
// RED (inversion): an undo that reverts the first CURRENT/first SUPERSEDED it
// finds (ignores the key) -> undo(Y) reverts X.
BOOST_AUTO_TEST_CASE(P4_Undo_KeyIsolation_LeavesOtherLineageUntouched)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 predX = QHk(0x5C), succX = QHk(0x5D);
    P4_make_promoted(predX, succX, 150, 0x5C, 0x5D);   // promoted lineage X

    uint256 succY = QHk(0x6A), predY = QHk(0x6B);      // unrelated Y, not held
    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succY, predY), 0u);   // no-op

    PTXShareRole r;
    BOOST_REQUIRE(P3_held_role(succX, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);              // X intact
    BOOST_REQUIRE(P3_held_role(predX, r)); BOOST_CHECK(r == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_CHECK_EQUAL(P3_promo_height(predX), 150);
    PTX_TEST_ClearSkShareSlot();
}

// the revert survives LoadShares — roles reload correctly after a revert.
// RED (inversion): an undo that mutates memory but does not re-persist the
// restored predecessor / does not erase the discarded successor from disk ->
// after reload the predecessor is stale SUPERSEDED or the successor reappears.
BOOST_AUTO_TEST_CASE(P4_Revert_SurvivesLoadShares)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x5E), succ = QHk(0x5F);
    uint8_t sp[32]; std::memset(sp, 0x5E, 32);
    uint8_t ss[32]; std::memset(ss, 0x5F, 32);
    std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(pred, 100, sp, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(succ, 140, ss, PTXShareRole::PENDING, e));
    BOOST_REQUIRE_EQUAL(PTX_BLS_Promote(succ, pred, 150, evoDb.get()), 1u);   // persisted

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred, evoDb.get()), 1u);      // persisted revert
    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole rp;
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::CURRENT);   // reloaded restored
    BOOST_CHECK_EQUAL(P3_promo_height(pred), -1);
    PTXShareRole rs;
    BOOST_CHECK(!P3_held_role(succ, rs));                                              // successor gone from disk
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// multi-block disconnect (item 1): the one-PENDING rule does NOT bound completed
// reorgeable promotions — a lineage can stack SUPERSEDED(A) + SUPERSEDED(B) +
// CURRENT(C). A reorg disconnects tip-first, reverting each promotion keyed to its
// block in LIFO order; a disconnect of a block that promoted nothing is a clean
// no-op. RED (inversion): a key-ignoring revert (first-match) or a non-zero no-op.
BOOST_AUTO_TEST_CASE(P4_MultiBlockDisconnect_ReversesEachKeyedPromotion)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 A = QHk(0x60), B = QHk(0x61), C = QHk(0x62);
    uint8_t sa[32]; std::memset(sa, 0x60, 32);
    uint8_t sb[32]; std::memset(sb, 0x61, 32);
    uint8_t sc[32]; std::memset(sc, 0x62, 32);
    std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(A, 100, sa, e));                        // A CURRENT
    BOOST_REQUIRE(PTX_BLS_SetSkShare(B, 140, sb, PTXShareRole::PENDING, e));
    BOOST_REQUIRE_EQUAL(PTX_BLS_Promote(B, A, 150), 1u);                     // A SUPERSEDED(150), B CURRENT
    // B is now CURRENT (not PENDING), so a SECOND PENDING is permitted — the
    // one-PENDING rule did NOT prevent a second in-flight promotion.
    BOOST_REQUIRE(PTX_BLS_SetSkShare(C, 190, sc, PTXShareRole::PENDING, e));
    BOOST_REQUIRE_EQUAL(PTX_BLS_Promote(C, B, 200), 1u);                     // B SUPERSEDED(200), C CURRENT

    PTXShareRole r;
    BOOST_REQUIRE(P3_held_role(A, r)); BOOST_CHECK(r == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_REQUIRE(P3_held_role(B, r)); BOOST_CHECK(r == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_REQUIRE(P3_held_role(C, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);

    // a disconnect of a block that promoted nothing -> clean no-op.
    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(QHk(0x6F), QHk(0x6E)), 0u);

    // disconnect tip-first: reverse promote2 (C,B), then promote1 (B,A).
    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(C, B), 1u);
    BOOST_REQUIRE(P3_held_role(B, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);
    BOOST_CHECK_EQUAL(P3_promo_height(B), -1);
    BOOST_CHECK(!P3_held_role(C, r));

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(B, A), 1u);
    BOOST_REQUIRE(P3_held_role(A, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);
    BOOST_CHECK_EQUAL(P3_promo_height(A), -1);
    BOOST_CHECK(!P3_held_role(B, r));
    PTX_TEST_ClearSkShareSlot();
}

// SUPERSEDED is unsignable at EVERY point across promote -> revert -> re-promote,
// not only at the single moment P4_PostRevert asserts. The signing selection
// (PTX_BLS_GetCurrentShare — the sole read path; rpc/ptx.cpp:539 is its only
// caller, feeding PartialSign) gates on role == CURRENT, so a SUPERSEDED share's
// bytes never reach the signer. RED (inversion): a GetCurrentShare that drops the
// role check -> the SUPERSEDED predecessor becomes signable at stage 1 (and again
// after re-promotion).
BOOST_AUTO_TEST_CASE(P4_SupersededNeverSignable_AcrossCycle)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 P = QHk(0x70), S = QHk(0x71), S2 = QHk(0x72);
    uint8_t sp[32];  std::memset(sp,  0x70, 32);
    uint8_t ss[32];  std::memset(ss,  0x71, 32);
    uint8_t ss2[32]; std::memset(ss2, 0x72, 32);
    std::string e; uint8_t out[32];

    // stage 0: P CURRENT, S PENDING. PENDING is not signable; P is.
    BOOST_REQUIRE(PTX_BLS_SetSkShare(P, 100, sp, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(S, 140, ss, PTXShareRole::PENDING, e));
    BOOST_CHECK(PTX_BLS_GetCurrentShare(P, out));      // CURRENT signs
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(S, out));     // PENDING refused

    // stage 1: promote. P -> SUPERSEDED (must NOT sign), S -> CURRENT (signs).
    BOOST_REQUIRE_EQUAL(PTX_BLS_Promote(S, P, 150), 1u);
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(P, out));     // SUPERSEDED refused (window 1)
    BOOST_CHECK(PTX_BLS_GetCurrentShare(S, out));

    // stage 2: revert. P -> CURRENT (signs again), S discarded (not signable).
    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(S, P), 1u);
    BOOST_CHECK(PTX_BLS_GetCurrentShare(P, out));
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(S, out));     // gone -> refused

    // stage 3: re-promote onto a fresh successor. P -> SUPERSEDED again (must NOT
    // sign), S2 -> CURRENT. Confirms the refusal holds through a second window.
    BOOST_REQUIRE(PTX_BLS_SetSkShare(S2, 290, ss2, PTXShareRole::PENDING, e));
    BOOST_REQUIRE_EQUAL(PTX_BLS_Promote(S2, P, 300), 1u);
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(P, out));     // SUPERSEDED refused (window 2)
    BOOST_CHECK(PTX_BLS_GetCurrentShare(S2, out));
    PTX_TEST_ClearSkShareSlot();
}

// ===========================================================================
// KDD-070 P5 — structural §1 check: NO rpc-reachable mutator of g_ptx_my_shares.
// ===========================================================================

namespace {
// slurp a whole file; empty string if it cannot be opened (the caller REQUIREs
// non-empty so a bad path fails loud, never skips).
std::string P5_slurp(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::string();
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
// count non-overlapping occurrences of needle in hay.
size_t P5_count(const std::string& hay, const std::string& needle)
{
    if (needle.empty()) return 0;
    size_t n = 0, p = 0;
    while ((p = hay.find(needle, p)) != std::string::npos) { ++n; p += needle.size(); }
    return n;
}
} // namespace

// §1 (widened by P4): g_ptx_my_shares has EIGHT mutators — one guarded
// (PTX_BLS_SetSkShare) and seven not, including the undo revert (which must
// bypass the §C1 setter). §1's guarantee is NO RPC-REACHABLE write path. This
// asserts, over the COMPLETE mutator set, that none is referenced under src/rpc
// (negative limb) AND that each is defined in ptx_bls.cpp (positive limb) — so a
// rename fails LOUDLY instead of the negative check passing vacuously.
// RED (inversion A, negative): a reference to any mutator added under src/rpc.
// RED (inversion B, positive): a mutator renamed -> its name absent from
// ptx_bls.cpp -> positive limb fails (the anti-vacuous guard).
BOOST_AUTO_TEST_CASE(P5_ShareStore_NoRpcReachableMutator)
{
    const std::string src = PTX_SRCDIR;

    // ★ ABSENT-DEFINE GUARD: if the build did not inject PTX_SRCDIR, `src` is the
    // empty sentinel. HARD-FAIL here — the check COULD NOT RUN and MUST NOT be
    // treated as passing. This is the vacuous-pass-via-build-system that P5
    // exists to prevent; it surfaces as a red test, never a skip.
    BOOST_REQUIRE_MESSAGE(!src.empty(),
        "P5: PTX_SRCDIR was NOT injected by the build (-DPTX_SRCDIR absent) — the "
        "structural §1 check COULD NOT RUN and MUST NOT be treated as passing; "
        "fix Makefile.test.include (test_test_ptx_CPPFLAGS) before trusting a green run");

    // the COMPLETE mutator set (P5 report item 1): every function that inserts,
    // erases, role-changes, or clears g_ptx_my_shares.
    const std::vector<std::string> mutators = {
        "PTX_BLS_SetSkShare",       // :84  insert-material (GUARDED, §C1)
        "PTX_BLS_Promote",          // :171 in-place role
        "PTX_BLS_ExpirePending",    // :213 erase
        "PTX_BLS_DiscardSuperseded",// :244 erase
        "PTX_BLS_UndoPromote",      // :275 role restore + :279 erase (UNGUARDED revert)
        "PTX_BLS_LoadShares",       // :310 insert (disk reload)
        "PTX_BLS_ReconcileShares",  // :338 erase
        "PTX_BLS_WipeShares",       // :377 clear
    };

    // read the defining TU — FAIL LOUD if the injected path is wrong (never skip).
    const std::string bls = P5_slurp(src + "/src/ptx/ptx_bls.cpp");
    BOOST_REQUIRE_MESSAGE(!bls.empty(),
        "P5: cannot read src/ptx/ptx_bls.cpp under PTX_SRCDIR='" << src <<
        "' — the structural check would be vacuous; is -DPTX_SRCDIR injected?");

    // concatenate every source under src/rpc.
    const fs::path rpcdir = fs::path(src) / "src" / "rpc";
    BOOST_REQUIRE_MESSAGE(fs::is_directory(rpcdir),
        "P5: src/rpc not found at '" << rpcdir.string() << "' — check would be vacuous");
    std::string rpc_all;
    size_t rpc_files = 0;
    for (fs::recursive_directory_iterator it(rpcdir), end; it != end; ++it) {
        if (!fs::is_regular_file(it->path())) continue;
        const std::string ext = it->path().extension().string();
        if (ext != ".cpp" && ext != ".h") continue;
        rpc_all += P5_slurp(it->path().string());
        ++rpc_files;
    }
    BOOST_REQUIRE_MESSAGE(rpc_files > 0 && !rpc_all.empty(),
        "P5: read zero source under src/rpc — the negative check would be vacuous");

    for (const std::string& m : mutators) {
        // POSITIVE limb: the mutator is DEFINED in ptx_bls.cpp. A rename makes
        // this fail loudly (anti-vacuous) rather than the negative limb passing.
        BOOST_CHECK_MESSAGE(P5_count(bls, m + "(") > 0,
            "P5 positive limb: mutator '" << m << "' not found in ptx_bls.cpp — "
            "renamed? update the mutator list; do not let the rpc check go vacuous");
        // NEGATIVE limb: the mutator name appears NOWHERE under src/rpc.
        BOOST_CHECK_MESSAGE(P5_count(rpc_all, m) == 0,
            "P5 negative limb: mutator '" << m << "' is referenced under src/rpc — "
            "§1 forbids an rpc-reachable write path to g_ptx_my_shares");
    }
}

BOOST_AUTO_TEST_SUITE_END()
