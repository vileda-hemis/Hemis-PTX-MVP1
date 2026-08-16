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
#include "evo/deterministicgms.h"   // KDD-072 P-b2: GM ptrs for the VerifyPremits harness
#include "ptx/ptx_formation.h"      // KDD-072 P-b3a: driver rotation wrapper
#include "chainparams.h"          // KDD-072 P-b6a: ptxFormation params for the due-rule stub
#include "evo/evodb.h"              // KDD-070 P2: the in-memory evoDb from BasicTestingSetup
#include "evo/specialtx_validation.h"
#include "ptx/ptx_accum_script.h"  // W2.4 W4-b: accum output for the roll-tx shape
#include "ptx/ptx_mempool.h"       // BUG-032: the fund-then-sign gate under test
#include "validation.h"            // BUG-032 2b: the global mempool the signing scan reads
#include "txmempool.h"             // BUG-032 2b: TestMemPoolEntryHelper / addUnchecked
#include "consensus/validation.h"
#include "primitives/block.h"       // W2.4 W4-d: CBlock for the idle-scan fakes
#include "primitives/transaction.h"

#include "bls/bls_wrapper.h"
#include "fs.h"                     // KDD-070 P5: iterate src/rpc for the structural check
#include "random.h"
#include "streams.h"                // KDD-072 P-a: CDataStream for payload wire round-trip / break tests
#include "sync.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <set>
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


// ODC-070 margin-erosion rows: the pure health core classifies each active
// quorum by (membership, CURRENT share, memory-only) from injected sets.
// RED (inversion): swapping the membership test to held-any, or dropping the
// in_qual condition, misclassifies rows B/C below.
BOOST_AUTO_TEST_CASE(ShareHealth_ClassifiesMembershipAndCapability)
{
    const std::string me = "node-me:hh";

    auto mkRec = [&](uint8_t k, bool meMember, bool meQual) {
        CPTXQuorumRecord rec;
        rec.quorum_hash = QHk(k);
        rec.mined_height = 100 + k;
        rec.formed_size = 11; rec.completed_size = 11;
        PTXQuorumMemberRecord m;
        m.node_id = meMember ? me : "node-other:xx";
        m.in_qual = meQual; m.share_index = 1;
        rec.members.push_back(m);
        return rec;
    };
    // A: member with share.  B: member, NO share.  C: not a member.
    // D: member but NOT in_qual (selected, not committed) — must not count.
    std::vector<CPTXQuorumRecord> active{
        mkRec(0xA1, true, true), mkRec(0xA2, true, true),
        mkRec(0xA3, false, true), mkRec(0xA4, true, false)};

    const std::set<uint256> current{QHk(0xA1)};
    const std::set<uint256> memOnly{QHk(0xA1)};

    const auto rows = PTX_ShareHealthFromRecords(active, me, current, memOnly);
    BOOST_REQUIRE_EQUAL(rows.size(), 4u);

    BOOST_CHECK(rows[0].am_member  && rows[0].share_current && rows[0].memory_only);
    BOOST_CHECK(rows[1].am_member  && !rows[1].share_current);   // the erosion row
    BOOST_CHECK(!rows[2].am_member);
    BOOST_CHECK(!rows[3].am_member);                             // in_qual=false
    BOOST_CHECK_EQUAL(rows[1].mined_height, 100 + 0xA2);
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
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), qh, hs));   // Write returned true

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
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), qh, hs));

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
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), live, a));
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), orphan, b));
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
// A live CURRENT share is held alongside so the post-expiry write is non-empty —
// under the BUG-039 floor an expiry that would EMPTY the file defers its disk
// erase instead (see Bug039_EmptyOverwriteRefused_LastShareRetainedOnDisk).
// RED (inversion): an expiry that erases only the map -> the PENDING reloads.
BOOST_AUTO_TEST_CASE(P3_ExpiredPending_GoneFromDisk)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    PTX_BLS_WipeShares(evoDb.get());
    uint256 qh = QHk(0x3B), live = QHk(0x3F);
    HeldShare hs; std::memset(hs.bytes, 0x3B, 32); hs.role = PTXShareRole::PENDING;
    hs.formation_height = 1000;
    HeldShare hl; std::memset(hl.bytes, 0x3F, 32); hl.role = PTXShareRole::CURRENT;
    hl.formation_height = 900; hl.promotion_height = -1;
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), qh, hs));
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), live, hl));
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);       // held as PENDING

    BOOST_CHECK_EQUAL(PTX_BLS_ExpirePending(1000 + PTX_PENDING_TTL_BLOCKS + 1, evoDb.get()), 1u);
    PTX_TEST_ClearSkShareSlot();                              // wipe map; disk is truth
    BOOST_CHECK_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole r;
    BOOST_CHECK(!P3_held_role(qh, r));                        // gone from disk
    BOOST_CHECK(P3_held_role(live, r));                       // companion intact
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
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), qh, hs));

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
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), pred, a));
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), succ, b));
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

// a discarded SUPERSEDED share is gone from DISK, not just memory. A live
// CURRENT share is held alongside so the post-discard write is non-empty —
// under the BUG-039 floor a discard that would EMPTY the file defers its disk
// erase instead (see Bug039_EmptyOverwriteRefused_LastShareRetainedOnDisk).
// RED (inversion): a discard that erases memory but not the RAW-DB entry -> the
// share reloads on the next LoadShares.
BOOST_AUTO_TEST_CASE(P4_DiscardedSuperseded_GoneFromDisk)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    PTX_BLS_WipeShares(evoDb.get());
    uint256 qh = QHk(0x53), live = QHk(0x54);
    HeldShare hs; std::memset(hs.bytes, 0x53, 32); hs.role = PTXShareRole::SUPERSEDED_RETAINED;
    hs.formation_height = 100; hs.promotion_height = 150;
    HeldShare hl; std::memset(hl.bytes, 0x54, 32); hl.role = PTXShareRole::CURRENT;
    hl.formation_height = 100; hl.promotion_height = -1;
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), qh, hs));
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), live, hl));
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);

    BOOST_CHECK_EQUAL(PTX_BLS_DiscardSuperseded(150 + 120, evoDb.get()), 1u);
    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole r;
    BOOST_CHECK(!P3_held_role(qh, r));   // gone from DISK, did not reload
    BOOST_CHECK(P3_held_role(live, r));  // companion intact
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// undo revert: SUPERSEDED(pred) -> CURRENT with promotion_height cleared, and
// CURRENT(succ) -> UNDONE_RETAINED stamped with the undo height.
//
// ★ AMENDED BY BUG-028. This case previously asserted `!P3_held_role(succ, rs)`
// — that the successor was DISCARDED — and so encoded the defect as the expected
// behaviour. The erase is what made the undo a one-way door: the promotion never
// created the successor's share (it pre-existed as PENDING from the ceremony), so
// erasing it destroyed material no redo could rebuild. The predecessor half was
// always retained-and-restorable; the successor half now matches it.
//
// RED (inversion): an undo that does not clear promotion_height, does not retain
// the successor on its undo clock, or does not restore the predecessor role.
BOOST_AUTO_TEST_CASE(P4_UndoRevert_RestoresPred_RetainsSucc_ClearsHeight)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x54), succ = QHk(0x55);
    P4_make_promoted(pred, succ, 150, 0x54, 0x55);

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 1u);
    PTXShareRole rp;
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::CURRENT);   // restored
    BOOST_CHECK_EQUAL(P3_promo_height(pred), -1);                                       // height cleared
    PTXShareRole rs;
    BOOST_REQUIRE(P3_held_role(succ, rs));                                              // RETAINED, not discarded
    BOOST_CHECK(rs == PTXShareRole::UNDONE_RETAINED);
    BOOST_CHECK_EQUAL(P3_promo_height(succ), 200);                                      // undo height stamped
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

    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 1u);
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

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 1u);   // first: reverts
    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 0u);   // second: clean no-op
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

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 0u);   // successor not held -> no-op
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
    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succY, predY, 200), 0u);   // no-op

    PTXShareRole r;
    BOOST_REQUIRE(P3_held_role(succX, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);              // X intact
    BOOST_REQUIRE(P3_held_role(predX, r)); BOOST_CHECK(r == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_CHECK_EQUAL(P3_promo_height(predX), 150);
    PTX_TEST_ClearSkShareSlot();
}

// ---------------------------------------------------------------------------
// BUG-028 — the undo/redo asymmetry.
//
// A promotion has two halves, and only one of them was reversible:
//
//   predecessor  on undo: SUPERSEDED_RETAINED -> CURRENT   retained 120 blocks
//                         on a reorg-DEPTH clock (promotion_height)   RESTORABLE
//   successor    on undo: ERASED                            no retention, no
//                         clock                                  NOT RESTORABLE
//
// That asymmetry IS BUG-028. A reorg that disconnects a PTXDKG block and then
// re-applies it left the node holding NO share for the quorum: the redo's
// Promote requires PENDING(successor_qh), but the undo had erased it, so the
// redo no-opped and the node could not sign the rolls it was selected for.
//
// The successor now demotes to UNDONE_RETAINED and is retained on the SAME
// basis as the predecessor. PENDING's TTL is deliberately not the mechanism:
// PTX_PENDING_TTL_BLOCKS expires on tip_height - formation_height, and reorg
// survival has to be measured from the UNDO, so no value of that constant can
// be correct here — the clock is wrong, not the number.
// ---------------------------------------------------------------------------

// RED-1 (the defect itself): disconnect a PTXDKG block, then RE-CONNECT the same
// block — the node must hold a signable CURRENT share again.
// RED (inversion): an undo that ERASES the successor -> the redo's Promote finds
// no share for successor_qh, no-ops (returns 0), and the node cannot sign.
BOOST_AUTO_TEST_CASE(BUG028_UndoThenRedo_SuccessorSignableAgain)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x70), succ = QHk(0x71);
    P4_make_promoted(pred, succ, 150, 0x70, 0x71);

    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 1u);   // block DISCONNECTED

    // the reorg RE-APPLIES the same block at the same height
    BOOST_CHECK_EQUAL(PTX_BLS_Promote(succ, pred, 150), 1u);    // RED: returns 0 today

    PTXShareRole rs;
    BOOST_REQUIRE(P3_held_role(succ, rs));                      // RED: not held at all today
    BOOST_CHECK(rs == PTXShareRole::CURRENT);
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(succ, out));            // RED: cannot sign today
    uint8_t ss[32]; std::memset(ss, 0x71, 32);
    BOOST_CHECK(std::memcmp(out, ss, 32) == 0);                 // and it is the successor's own bytes

    PTXShareRole rp;                                            // predecessor re-superseded by the redo
    BOOST_REQUIRE(P3_held_role(pred, rp));
    BOOST_CHECK(rp == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_CHECK_EQUAL(P3_promo_height(pred), 150);
    PTX_TEST_ClearSkShareSlot();
}

// RED-1b: the undone successor is HELD but NOT SIGNABLE while retained — the
// same guarantee SUPERSEDED_RETAINED already carries. Retention must not become
// a second signing path for a quorum whose activating block is off the chain.
// RED (inversion): a demote that leaves the successor CURRENT -> it signs for a
// quorum that the active chain no longer activates.
BOOST_AUTO_TEST_CASE(BUG028_UndoneSuccessor_HeldButNotSignable)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x72), succ = QHk(0x73);
    P4_make_promoted(pred, succ, 150, 0x72, 0x73);

    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 1u);

    PTXShareRole rs;
    BOOST_REQUIRE(P3_held_role(succ, rs));                      // RED: erased today
    BOOST_CHECK(rs == PTXShareRole::UNDONE_RETAINED);
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(succ, out));           // retained != signable
    PTX_TEST_ClearSkShareSlot();
}

// RED-3 — the retention BOUNDARY for the undone successor, and the limit of the
// basis. Depth 119 kept, depth 120 discarded, measured from the UNDO height:
// DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN, the same constant the
// predecessor half uses.
//
// ★ KNOWN LIMIT — pinned here rather than left implicit. DEFAULT_MAX_REORG_DEPTH
// is the DEFAULT of a runtime option, not a hard constant: validation.cpp reads
// `gArgs.GetArg("-maxreorg", DEFAULT_MAX_REORG_DEPTH)` and rejects forks at or
// beyond that depth. A node started with -maxreorg ABOVE 120 therefore PERMITS
// reorgs deeper than either retained role is kept for. Such a node could accept a
// reorg that re-applies a PTXDKG block whose UNDONE_RETAINED share this sweep had
// already discarded — reproducing BUG-028 exactly, from configuration rather than
// from the erase. This test asserts the boundary the DEFAULT gives; it does NOT
// assert safety under a widened window, and no share-slot change can provide that
// (the bound is a consensus-layer parameter). Raising -maxreorg above 120 without
// raising PTX_SUPERSEDED_REORG_MARGIN with it is an unsupported configuration.
BOOST_AUTO_TEST_CASE(BUG028_UndoneRetentionBoundary_119Kept_120Discarded)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x74), succ = QHk(0x75);
    P4_make_promoted(pred, succ, 150, 0x74, 0x75);
    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 1u);   // undo height = 200
    const int depth = DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN;   // 120
    PTXShareRole r;

    // depth 119 (tip = 200 + 119): kept, still redoable.
    BOOST_CHECK_EQUAL(PTX_BLS_DiscardUndone(200 + depth - 1), 0u);
    BOOST_REQUIRE(P3_held_role(succ, r)); BOOST_CHECK(r == PTXShareRole::UNDONE_RETAINED);

    // depth 120 (tip = 200 + 120): discarded — no permitted reorg can re-apply it.
    BOOST_CHECK_EQUAL(PTX_BLS_DiscardUndone(200 + depth), 1u);
    BOOST_CHECK(!P3_held_role(succ, r));

    // the sweep is role-scoped: the restored predecessor (CURRENT) is untouched.
    BOOST_REQUIRE(P3_held_role(pred, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);
    PTX_TEST_ClearSkShareSlot();
}

// ---------------------------------------------------------------------------
// BUG-029 — the VerifyDB dry-run walk mutates the share store.
//
// VerifyDB at nCheckLevel>=3 runs the REAL DisconnectBlock over the last
// -checkblocks (default 6) blocks:
//
//   VerifyDB -> DisconnectBlock (validation.cpp:1346)
//            -> UndoSpecialTxsInBlock (specialtx_validation.cpp:1571)
//            -> ptxQuorumStore->UndoBlock (:1625)
//            -> PTX_BLS_UndoPromote  ==> mutates g_ptx_my_shares
//
// Two sandboxes protect that walk — a throwaway CCoinsViewCache and an evoDb
// rollback transaction — and the share store is outside BOTH. The memory half is
// a bare global; the DISK half escapes because PTX_BLS_PersistShare writes through
// GetRawDB(), which bypasses CDBTransaction by design (ODC-035). Level 3 never
// reconnects, so nothing promotes the share back.
//
// This is BUG-023's mechanism on a different global — there LotteryState, here the
// share store. BUG-023's sentry saves only LotteryState.
//
// ★ BUG-028 INTERACTION, pinned rather than assumed: before 56c938a the same path
// ERASED the share (and the erase was a raw write too), so a startup verify could
// destroy key material irrecoverably. Retention converted that into a recoverable
// wrong-role. These tests assert the mid-walk role explicitly so the seam between
// the two fixes stays visible.
// ---------------------------------------------------------------------------

// RED (memory half): snapshot, run the walk's mutation, restore — the share must
// come back CURRENT and signable.
// RED (inversion): no snapshot/restore around the walk -> the share is left
// UNDONE_RETAINED and the node cannot sign for a quorum the chain still activates.
BOOST_AUTO_TEST_CASE(BUG029_WalkMutationRestoredByShareSnapshot)
{
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x78), succ = QHk(0x79);
    P4_make_promoted(pred, succ, 150, 0x78, 0x79);

    auto snap = PTX_BLS_SnapshotShares();          // what VerifyDB must leave behind
    BOOST_CHECK_EQUAL(snap.size(), 2u);

    // the walk: a disconnect of the rotation block, exactly as UndoBlock issues it
    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 1u);

    // ★ BUG-028 cross-check — the mid-walk state, observed not assumed.
    PTXShareRole mid;
    BOOST_REQUIRE(P3_held_role(succ, mid));
    BOOST_CHECK(mid == PTXShareRole::UNDONE_RETAINED);   // retained (was: erased)
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(succ, out));    // and unsignable mid-walk

    PTX_BLS_RestoreShares(snap);                    // the sentry's restore

    PTXShareRole after;
    BOOST_REQUIRE(P3_held_role(succ, after));
    BOOST_CHECK(after == PTXShareRole::CURRENT);         // restored
    BOOST_CHECK(PTX_BLS_GetCurrentShare(succ, out));     // signable again
    PTXShareRole rp;
    BOOST_REQUIRE(P3_held_role(pred, rp));
    BOOST_CHECK(rp == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_CHECK_EQUAL(P3_promo_height(pred), 150);       // and its clock survived
    PTX_TEST_ClearSkShareSlot();
}

// RED (disk half): the walk's raw writes ALREADY reached disk, so a memory-only
// restore leaves the regression persisted — it would survive the restart, and
// every restart re-runs the walk, so it could never self-heal.
// RED (inversion): a restore that does not re-persist -> after reload the share
// comes back UNDONE_RETAINED.
BOOST_AUTO_TEST_CASE(BUG029_RestoreReachesDisk)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    uint256 pred = QHk(0x7A), succ = QHk(0x7B);
    uint8_t sp[32]; std::memset(sp, 0x7A, 32);
    uint8_t ss[32]; std::memset(ss, 0x7B, 32);
    std::string e;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(pred, 100, sp, e));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(succ, 140, ss, PTXShareRole::PENDING, e));
    BOOST_REQUIRE_EQUAL(PTX_BLS_Promote(succ, pred, 150, evoDb.get()), 1u);

    auto snap = PTX_BLS_SnapshotShares();
    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200, evoDb.get()), 1u);  // raw write hits disk
    PTX_BLS_RestoreShares(snap, evoDb.get());

    // simulate the restart the operator would do next
    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole r;
    BOOST_REQUIRE(P3_held_role(succ, r));
    BOOST_CHECK(r == PTXShareRole::CURRENT);        // disk was restored, not just memory
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(succ, out));
    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// BUG-028 SAFETY NET (explicitly NOT the fix — the fix is the UNDONE_RETAINED
// retention in PTX_BLS_UndoPromote). The missing-share warning is about SIGNING
// capacity, so it must key on the CURRENT subset, not on held-any: a share in a
// retained role is HELD yet cannot sign, so a held-any test reports healthy while
// the quorum is short a signer.
//
// This is not hypothetical bookkeeping — the retention itself created the hole.
// Before the fix a node stranded by a reorg held NOTHING and so did warn; with
// retention it holds an UNDONE_RETAINED share, and a held-any test would have
// gone SILENT on exactly the state BUG-028 leaves behind.
// RED (inversion): a warn keyed on PTX_BLS_HeldQuorumHashes() returns 0 here.
BOOST_AUTO_TEST_CASE(BUG028_WarnMissing_RetainedRoleIsNotSignable)
{
    PTX_TEST_ClearSkShareSlot();
    const std::string me = "node-me:bb";

    uint256 pred = QHk(0x76), succ = QHk(0x77);
    P4_make_promoted(pred, succ, 150, 0x76, 0x77);       // succ CURRENT, pred SUPERSEDED

    CPTXQuorumRecord rec;
    rec.quorum_hash = succ;
    PTXQuorumMemberRecord m; m.node_id = me; m.in_qual = true; m.share_index = 3;
    rec.members.push_back(m);
    std::vector<CPTXQuorumRecord> active{rec};

    // CURRENT: signable, no warning.
    BOOST_CHECK_EQUAL(PTX_WarnMissingSharesForNode(active, me), 0);

    // the reorg strands it: still HELD, but UNDONE_RETAINED and unsignable.
    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200), 1u);
    PTXShareRole rr;
    BOOST_REQUIRE(P3_held_role(succ, rr)); BOOST_CHECK(rr == PTXShareRole::UNDONE_RETAINED);
    uint8_t out[32];
    BOOST_REQUIRE(!PTX_BLS_GetCurrentShare(succ, out));               // cannot sign
    BOOST_CHECK_EQUAL(PTX_WarnMissingSharesForNode(active, me), 1);   // and it SAYS so

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

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(succ, pred, 200, evoDb.get()), 1u);      // persisted revert
    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    PTXShareRole rp;
    BOOST_REQUIRE(P3_held_role(pred, rp)); BOOST_CHECK(rp == PTXShareRole::CURRENT);   // reloaded restored
    BOOST_CHECK_EQUAL(P3_promo_height(pred), -1);
    // BUG-028 / RED-2 (durability): this ASSERTED the successor was gone from disk.
    // It is now RETAINED, so it must reload as UNDONE_RETAINED carrying its undo
    // height — otherwise the redo path survives a reorg but not a restart, and the
    // node comes back up unable to sign for a quorum the chain still activates.
    PTXShareRole rs;
    BOOST_REQUIRE(P3_held_role(succ, rs));                                             // reloaded, not erased
    BOOST_CHECK(rs == PTXShareRole::UNDONE_RETAINED);
    BOOST_CHECK_EQUAL(P3_promo_height(succ), 200);                                     // undo height persisted
    uint8_t out[32];
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(succ, out));                                  // retained != signable
    // and the redo still works AFTER the restart
    BOOST_CHECK_EQUAL(PTX_BLS_Promote(succ, pred, 210, evoDb.get()), 1u);
    BOOST_CHECK(PTX_BLS_GetCurrentShare(succ, out));                                   // signable again
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
    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(QHk(0x6F), QHk(0x6E), 200), 0u);

    // disconnect tip-first: reverse promote2 (C,B), then promote1 (B,A).
    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(C, B, 200), 1u);
    BOOST_REQUIRE(P3_held_role(B, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);
    BOOST_CHECK_EQUAL(P3_promo_height(B), -1);
    // BUG-028: C is RETAINED (was: discarded), so the re-connect below can redo it.
    BOOST_REQUIRE(P3_held_role(C, r)); BOOST_CHECK(r == PTXShareRole::UNDONE_RETAINED);

    BOOST_CHECK_EQUAL(PTX_BLS_UndoPromote(B, A, 200), 1u);
    BOOST_REQUIRE(P3_held_role(A, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);
    BOOST_CHECK_EQUAL(P3_promo_height(A), -1);
    BOOST_REQUIRE(P3_held_role(B, r)); BOOST_CHECK(r == PTXShareRole::UNDONE_RETAINED);

    // BUG-028 — the multi-block REDO, which is the shape a real reorg takes: the
    // new branch re-applies both blocks in order, and the lineage must return to
    // exactly the state it was in before the disconnect. Under the old erase this
    // was unreachable — both promotions no-opped and the node held no CURRENT
    // share for its own quorum at all.
    BOOST_CHECK_EQUAL(PTX_BLS_Promote(B, A, 150), 1u);   // re-connect block 1
    BOOST_CHECK_EQUAL(PTX_BLS_Promote(C, B, 200), 1u);   // re-connect block 2
    BOOST_REQUIRE(P3_held_role(A, r)); BOOST_CHECK(r == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_REQUIRE(P3_held_role(B, r)); BOOST_CHECK(r == PTXShareRole::SUPERSEDED_RETAINED);
    BOOST_REQUIRE(P3_held_role(C, r)); BOOST_CHECK(r == PTXShareRole::CURRENT);
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(C, out));        // and the tip quorum signs again
    BOOST_CHECK(std::memcmp(out, sc, 32) == 0);
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
    BOOST_REQUIRE_EQUAL(PTX_BLS_UndoPromote(S, P, 200), 1u);
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
// Return the first line of `hay` containing `needle` (empty if none).
std::string P5_line_containing(const std::string& hay, const std::string& needle)
{
    size_t pos = hay.find(needle);
    if (pos == std::string::npos) return std::string();
    size_t b = hay.rfind('\n', pos); b = (b == std::string::npos) ? 0 : b + 1;
    size_t e = hay.find('\n', pos);  if (e == std::string::npos) e = hay.size();
    return hay.substr(b, e - b);
}

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

// ---------------------------------------------------------------------------
// ODC-064 — THE SILENT FAILURE BRANCHES.
//
// BUG-028's cause could not be recovered from four days of fleet logs. Two
// branches produced no output at all:
//
//   ptx_fanout.cpp   the non-200 arm logged only the HTTP status and DISCARDED
//                    response.body — the server's error text, which names the
//                    exact condition, was never written anywhere.
//   rpc/ptx.cpp      gm_bls_sign's failure arms threw without logging; only the
//                    SUCCESS path logged. A node that could not sign was
//                    indistinguishable from one that was never asked.
//
// Diagnosis therefore required a live read-only probe of a running node. Had the
// fleet been rebuilt first, the cause would have been unrecoverable.
//
// ★ WHAT THIS PROVES, STATED HONESTLY: that the failure branches CONTAIN the
// logging, with the fields that would have closed the diagnosis. It does NOT
// prove the line fires at runtime — this codebase has no log-capture harness and
// building one is a larger change than the logging it would verify. A deliberate
// trade, not an oversight. ODC-065's argument (observability changes get verified
// like behaviour changes) is honoured as far as the harness allows, and the
// remaining gap is named rather than papered over.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(ODC064_FailureBranchesLogTheirCause)
{
    // --- site 1: the fan-out non-200 arm must log the BODY, not just the status
    const std::string fanout = P5_slurp(std::string(PTX_SRCDIR) + "/src/ptx/ptx_fanout.cpp");
    BOOST_REQUIRE(!fanout.empty());
    // ★ Target the FanOutSign arm specifically — the braced one. There is a second
    // non-200 arm at the generic RPC helper (`... != 200) return result;`) which is
    // BENIGN: it sets `result.body = response.body` one line earlier, so the body is
    // PROPAGATED to the caller rather than discarded. Matching the first occurrence
    // pointed the check at that helper and made it unfixable-by-construction.
    const size_t nz = fanout.find("response.status != 200) {");
    BOOST_REQUIRE_MESSAGE(nz != std::string::npos, "FanOutSign non-200 branch not found");
    // ★ ANTI-VACUITY: bound the window to the ARM ITSELF (up to its `continue;`).
    // A loose window passes on the success path's own `res.read(response.body)`
    // further down — i.e. it would go green without the fix. Caught by observing
    // which sub-checks passed in the RED run.
    const size_t armEnd = fanout.find("continue;", nz);
    BOOST_REQUIRE_MESSAGE(armEnd != std::string::npos, "non-200 arm has no continue");
    const std::string arm = fanout.substr(nz, armEnd - nz);
    BOOST_CHECK_MESSAGE(arm.find("LogPrintf") != std::string::npos,
                        "ODC-064: non-200 arm does not log at all");
    BOOST_CHECK_MESSAGE(arm.find("response.body") != std::string::npos,
                        "ODC-064: non-200 arm still discards response.body");
    BOOST_CHECK_MESSAGE(arm.find("ODC-064") != std::string::npos,
                        "ODC-064: the branch is not marked with its rationale");
    // bounded — a broken or hostile peer must not be able to flood the log
    BOOST_CHECK_MESSAGE(arm.find("substr") != std::string::npos,
                        "ODC-064: response.body is logged UNTRIMMED (flood risk)");

    // --- site 2: gm_bls_sign's failure arms must log quorum_hash + reason
    const std::string rpc = P5_slurp(std::string(PTX_SRCDIR) + "/src/rpc/ptx.cpp");
    BOOST_REQUIRE(!rpc.empty());
    // BUG-032 2b consolidated gm_bls_sign's per-branch failure arms into ONE
    // gated call (PTX_SignRoundIfCommitted); the ODC-064 "every failure branch
    // has a voice" property is preserved — the unified arm logs quorum_hash +
    // reason before throwing, and the underlying no-share reason is now named by
    // the gate itself.
    const size_t gs = rpc.find("if (!PTX_SignRoundIfCommitted(");
    BOOST_REQUIRE_MESSAGE(gs != std::string::npos, "gm_bls_sign gated-sign failure arm not found");
    const size_t th = rpc.find("throw JSONRPCError", gs);
    BOOST_REQUIRE_MESSAGE(th != std::string::npos, "gm_bls_sign failure arm has no throw");
    const std::string gsArm = rpc.substr(gs, th - gs);   // the arm up to (not incl.) the throw
    BOOST_CHECK_MESSAGE(gsArm.find("LogPrintf") != std::string::npos,
                        "ODC-064: gm_bls_sign's failure arm throws without logging");
    BOOST_CHECK_MESSAGE(gsArm.find("quorum_hash.ToString()") != std::string::npos,
                        "ODC-064: the failure log does not name the quorum_hash");
    BOOST_CHECK_MESSAGE(gsArm.find("reason=") != std::string::npos,
                        "ODC-064: the failure log does not name the reason");
    // 2b-ii: the arm must distinguish a RETRYABLE refusal (commitment not seen)
    // from a terminal one — the property the coordinator's wait-and-retry needs.
    BOOST_CHECK_MESSAGE(rpc.substr(gs, (th - gs) + 200).find("RPC_PTX_COMMITMENT_NOT_SEEN") != std::string::npos,
                        "2b-ii: gm_bls_sign does not signal the retryable (commitment-not-seen) case distinctly");
    // The no-share reason moved into the gate — verify it still names its cause.
    const std::string mp = P5_slurp(std::string(PTX_SRCDIR) + "/src/ptx/ptx_mempool.cpp");
    BOOST_REQUIRE(!mp.empty());
    BOOST_CHECK_MESSAGE(mp.find("no CURRENT sk_share held for quorum") != std::string::npos,
                        "ODC-064: the gate's no-share branch no longer names its cause");
}

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

// ===========================================================================
// KDD-072 P-a — versioned PTXDKGPayload (nVersion FIRST field + version gate).
// ===========================================================================

namespace {
// Faithful reconstruction of the PRE-P-a (unversioned) PTXDKGPayload wire form:
// the SAME fields in the SAME order MINUS nVersion. Byte-identical to what the
// old serializer emitted. Used by Pa_OldLayoutMisparses to feed genuine old-
// format bytes to the NEW deserializer, independent of the current serializer.
struct PaOldLayoutPayload {
    uint256                            quorum_hash;
    uint8_t                            group_pk_bytes[48] = {};
    uint256                            vvec_hash;
    std::vector<std::string>           member_node_ids;
    int                                formation_height{0};
    std::map<uint256, PTXDKGPhase4Msg> premit_commitments;
    SERIALIZE_METHODS(PaOldLayoutPayload, obj)
    {
        READWRITE(obj.quorum_hash);
        READWRITE(Using<PTXFixedBytesFormatter<48>>(obj.group_pk_bytes));
        READWRITE(obj.vvec_hash, obj.member_node_ids,
                  obj.formation_height, obj.premit_commitments);
    }
};
} // namespace

// (1) round-trip — the version field is genuinely ON THE WIRE and survives
// serialize->deserialize. A distinguishable value (7) proves the field is
// serialized, not merely defaulted. RED (inversion): drop READWRITE(obj.nVersion)
// -> the value is not written and deserializes to the default (1) != 7.
BOOST_AUTO_TEST_CASE(Pa_RoundTrip_VersionIntact)
{
    PTXDKGPayload in;
    in.nVersion = 7;                                   // distinguishable non-default
    std::memset(in.quorum_hash.begin(), 0xAB, 32);
    std::memset(in.group_pk_bytes, 0, 48);
    in.vvec_hash = QHk(0x22);
    in.formation_height = 4242;
    in.member_node_ids = {"gm01:aa", "gm02:bb"};

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << in;
    PTXDKGPayload out;
    ss >> out;
    BOOST_CHECK_EQUAL(out.nVersion, (uint16_t)7);       // version survived the wire
    BOOST_CHECK(out.quorum_hash == in.quorum_hash);     // fields aligned, not shifted
    BOOST_CHECK_EQUAL(out.formation_height, 4242);
    BOOST_CHECK(out.member_node_ids == in.member_node_ids);
}

// (2) nVersion == 0 -> bad-ptxdkg-version. RED (inversion): remove the gate ->
// a v0 payload passes the structural section.
BOOST_AUTO_TEST_CASE(Pa_Version0_Rejected)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);
    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));
    payload.nVersion = 0;
    SetTxPayload(mtx, payload);
    CTransaction tx(mtx);
    CValidationState state;
    LOCK(cs_main);
    BOOST_CHECK(!CheckPTXDKGTx(tx, nullptr, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-ptxdkg-version");
}

// (3) nVersion > CURRENT_VERSION -> bad-ptxdkg-version. RED (inversion): a gate
// using >= or a wrong bound accepts CURRENT+1.
BOOST_AUTO_TEST_CASE(Pa_VersionAboveCurrent_Rejected)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);
    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));
    payload.nVersion = PTXDKGPayload::ROTATION_VERSION + 1; // P-b3b: v2 accepted now; v3 is the reject bound
    SetTxPayload(mtx, payload);
    CTransaction tx(mtx);
    CValidationState state;
    LOCK(cs_main);
    BOOST_CHECK(!CheckPTXDKGTx(tx, nullptr, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-ptxdkg-version");
}

// (4) a valid v1 payload passes the (structural) sequence unchanged, AND
// BuildPTXDKGTx sets nVersion = CURRENT_VERSION EXPLICITLY (not default-reliant,
// item 7). RED (inversion): remove the explicit assignment + change the struct
// default -> BuildPTXDKGTx yields the wrong version; or a gate that rejects v1.
BOOST_AUTO_TEST_CASE(Pa_ValidV1_PassesAndBuildSetsVersion)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);
    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    PTXDKGPayload built;
    BOOST_REQUIRE(GetTxPayload(mtx, built));
    const uint16_t cur = PTXDKGPayload::CURRENT_VERSION; // local avoids ODR-use (tree never defines it out-of-line)
    BOOST_CHECK_EQUAL(built.nVersion, cur);                            // item 7: explicit
    CTransaction tx(mtx);
    CValidationState state;
    LOCK(cs_main);
    BOOST_CHECK(CheckPTXDKGTx(tx, nullptr, state));                     // valid v1 still passes
}

// (5) the version field is FIRST: genuine OLD-format bytes (PaOldLayoutPayload,
// no nVersion) MISPARSE under the new deserializer — proving the wire break is
// ENFORCED, not silently tolerated. The new reader consumes the first 2 bytes of
// the old quorum_hash as nVersion and shifts every field, so it either throws
// (short read) or yields a payload that does not round-trip. RED (inversion):
// drop READWRITE(obj.nVersion) -> the new reader IS the old reader -> old-format
// bytes parse cleanly and the misparse assertion fails.
BOOST_AUTO_TEST_CASE(Pa_OldLayoutMisparses_BreakEnforced)
{
    PaOldLayoutPayload oldp;
    std::memset(oldp.quorum_hash.begin(), 0xAB, 32);   // first 2 bytes 0xABAB -> misparsed version
    std::memset(oldp.group_pk_bytes, 0, 48);
    oldp.vvec_hash = QHk(0x33);
    oldp.formation_height = 100;
    oldp.member_node_ids = {"gm01:aa"};

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << oldp;                                          // genuine pre-P-a wire bytes

    PTXDKGPayload out;
    bool threw = false;
    try { ss >> out; } catch (const std::exception&) { threw = true; }

    // Break enforced: either deserialization throws (short read from the 2-byte
    // shift), OR the fields shifted so the payload does not round-trip.
    const bool misparsed = threw
        || out.nVersion != PTXDKGPayload::CURRENT_VERSION
        || !(out.quorum_hash == oldp.quorum_hash);
    BOOST_CHECK_MESSAGE(misparsed,
        "old-format (unversioned) bytes silently round-tripped under the new "
        "deserializer — the KDD-072 P-a wire break is NOT enforced");
}

// ---------------------------------------------------------------------------
// KDD-072 P-b1 — payload v2 predecessor field (format only; gate deferred P-b3)
// ---------------------------------------------------------------------------

// (P-b1 row 2) v1 byte-exactness golden. The hex below was CAPTURED FROM HEAD
// BEFORE the P-b1 edit by executing this row against the pre-edit serializer
// (build discipline: this row lands green at pre-edit HEAD first, then the edit
// must keep it green). Layout: nVersion(2 LE) | quorum_hash(32) | group_pk(48) |
// vvec_hash(32) | member_node_ids(compact=0) | formation_height(4 LE) |
// premit_commitments(compact=0) = 120 bytes. RED (inversion): serialize the
// predecessor unconditionally -> the v1 stream grows 32 bytes -> mismatch.
BOOST_AUTO_TEST_CASE(Pb1_V1Stream_ByteExact_Golden)
{
    PTXDKGPayload in;
    in.nVersion = 1;
    std::memset(in.quorum_hash.begin(), 0x22, 32);
    std::memset(in.group_pk_bytes, 0x33, 48);
    in.vvec_hash = QHk(0x44);
    in.formation_height = 880;
    // member_node_ids + premit_commitments deliberately EMPTY: every byte of the
    // golden is fixed-width or a compactsize(0), so the vector is fully
    // determined by the v1 layout and nothing else.

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << in;

    const std::string golden =
        std::string("0100") +
        std::string(64, '2') +                 // quorum_hash: 32 x 0x22
        [] { std::string s; for (int i = 0; i < 48; i++) s += "33"; return s; }() +
        std::string(64, '4') +                 // vvec_hash: 32 x 0x44
        "00" + "70030000" + "00";
    BOOST_CHECK_EQUAL(HexStr(ss), golden);
    BOOST_CHECK_EQUAL(ss.size(), (size_t)120);
}

// (P-b1 row 1) v2 round-trip: predecessor_quorum_hash is genuinely on the wire
// at ROTATION_VERSION and survives serialize->deserialize; the stream is exactly
// 32 bytes longer than the v1 golden (152 = 120 + 32). RED (inversion B): drop
// the conditional READWRITE -> predecessor deserializes to zero != 0x77-fill.
BOOST_AUTO_TEST_CASE(Pb1_V2RoundTrip_PredecessorIntact)
{
    PTXDKGPayload in;
    in.nVersion = PTXDKGPayload::ROTATION_VERSION;
    std::memset(in.quorum_hash.begin(), 0x22, 32);
    std::memset(in.group_pk_bytes, 0x33, 48);
    in.vvec_hash = QHk(0x44);
    in.formation_height = 880;
    in.predecessor_quorum_hash = QHk(0x77);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << in;
    BOOST_CHECK_EQUAL(ss.size(), (size_t)152);           // v1 golden + 32

    PTXDKGPayload out;
    ss >> out;
    const uint16_t rot = PTXDKGPayload::ROTATION_VERSION; // local avoids ODR-use (tree never defines it out-of-line)
    BOOST_CHECK_EQUAL(out.nVersion, rot);
    BOOST_CHECK(out.quorum_hash == in.quorum_hash);
    BOOST_CHECK(out.predecessor_quorum_hash == QHk(0x77)); // survived the wire
    BOOST_CHECK_EQUAL(out.formation_height, 880);
}

// (P-b1 row 3) a v1 stream deserialized into a FRESH payload (the production
// shape — GetTxPayload default-constructs) leaves predecessor_quorum_hash ZERO:
// the rotation signal is vacuously off for every v1 payload. RED (inversion A):
// make the predecessor read unconditional -> the 120-byte v1 stream exhausts
// mid-read and this row throws instead of passing.
BOOST_AUTO_TEST_CASE(Pb1_V1Deser_PredecessorDefaultsZero)
{
    PTXDKGPayload in;
    in.nVersion = 1;
    std::memset(in.quorum_hash.begin(), 0x22, 32);
    std::memset(in.group_pk_bytes, 0x33, 48);
    in.vvec_hash = QHk(0x44);
    in.formation_height = 880;
    in.predecessor_quorum_hash = QHk(0x77);  // set on the OBJECT — must NOT serialize at v1

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << in;
    BOOST_CHECK_EQUAL(ss.size(), (size_t)120);           // predecessor NOT written at v1

    PTXDKGPayload out;                                    // fresh, default-zero
    ss >> out;
    BOOST_CHECK(out.predecessor_quorum_hash.IsNull());    // rotation signal off
    BOOST_CHECK(out.quorum_hash == in.quorum_hash);
}

// (P-b1 row 4) a v2 header on a v1-length stream FAILS deserialization — a
// malformed v2 cannot silently parse as "no predecessor". Right-reason check
// (build item 7, resolved from source): CBaseDataStream::read (streams.h:262-269)
// THROWS std::ios_base::failure("CDataStream::read(): end of data") on
// exhaustion — the bounds check precedes the memcpy, so a short buffer can NEVER
// yield a default-zero uint256. Exception TYPE and MESSAGE are both asserted so
// this row fails if that contract ever changes. RED (inversion B): drop the
// conditional read -> the truncated stream parses clean -> no exception -> fail.
BOOST_AUTO_TEST_CASE(Pb1_V2Stream_TruncatedAtV1Length_Throws)
{
    PTXDKGPayload in;
    in.nVersion = PTXDKGPayload::ROTATION_VERSION;
    std::memset(in.quorum_hash.begin(), 0x22, 32);
    std::memset(in.group_pk_bytes, 0x33, 48);
    in.vvec_hash = QHk(0x44);
    in.formation_height = 880;
    in.predecessor_quorum_hash = QHk(0x77);

    CDataStream full(SER_NETWORK, PROTOCOL_VERSION);
    full << in;                                           // 152 bytes
    std::vector<unsigned char> raw(full.begin(), full.begin() + 120); // v1 length
    CDataStream truncated(raw, SER_NETWORK, PROTOCOL_VERSION);

    PTXDKGPayload out;
    BOOST_CHECK_EXCEPTION(truncated >> out, std::ios_base::failure,
        [](const std::ios_base::failure& e) {
            return std::string(e.what()).find("end of data") != std::string::npos;
        });
}

// (P-b3b — REPLACES Pb1_GateStillRejectsV2, as its comment promised since
// P-b1) v2 is now ACCEPTED through the structural section: the gate bound is
// ROTATION_VERSION, and a rotation-shaped v2 payload (predecessor set) passes.
// RED (inversion A): revert the bound to CURRENT_VERSION -> v2 rejected -> fail.
BOOST_AUTO_TEST_CASE(Pb3b_GateAcceptsV2_RotationShaped)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);
    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));
    payload.nVersion = PTXDKGPayload::ROTATION_VERSION;
    payload.predecessor_quorum_hash = QHk(0x77);
    SetTxPayload(mtx, payload);
    CTransaction tx(mtx);
    CValidationState state;
    LOCK(cs_main);
    BOOST_CHECK_MESSAGE(CheckPTXDKGTx(tx, nullptr, state), state.GetRejectReason());
}

// (P-b3b) v2-WITHOUT-predecessor is REJECTED at the STRUCTURAL depth (the
// null-pindexPrev path — proving CheckBlock catches it before any contextual
// machinery; the contextual twin inside the V12 branch head is defense-in-depth
// against a structural refactor, unreachable through the public sequence).
// RED (inversion B): remove the structural check -> this malformed payload
// passes the whole structural section (contextual skipped at nullptr) -> fail.
BOOST_AUTO_TEST_CASE(Pb3b_V2WithoutPredecessor_Rejected)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);
    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));
    payload.nVersion = PTXDKGPayload::ROTATION_VERSION;
    payload.predecessor_quorum_hash.SetNull();   // the malformed shape
    SetTxPayload(mtx, payload);
    CTransaction tx(mtx);
    CValidationState state;
    LOCK(cs_main);
    BOOST_CHECK(!CheckPTXDKGTx(tx, nullptr, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-v2-without-predecessor");
}

// (P-b3b) ★ THE BREAKER-CHECK: a fresh formation still emits LITERAL v1 and
// still validates. This is the row that would have caught the literal
// CURRENT_VERSION-bump bug the recon found (fresh would emit
// v2-with-zero-predecessor and die on the reject above). The LITERAL 1 matters:
// asserting ==CURRENT_VERSION would silently follow a bump.
// RED (inversion C): bump CURRENT_VERSION to 2 -> fresh emits v2 -> both checks fail.
BOOST_AUTO_TEST_CASE(Pb3b_FreshStillEmitsV1AndValidates)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    PTX_TEST_ClearSkShareSlot();
    AdvanceToFinalize(key_map, sessions);
    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));
    BOOST_CHECK_EQUAL(payload.nVersion, (uint16_t)1);           // LITERAL, not ==CURRENT_VERSION
    BOOST_CHECK(payload.predecessor_quorum_hash.IsNull());
    CTransaction tx(mtx);
    CValidationState state;
    LOCK(cs_main);
    BOOST_CHECK_MESSAGE(CheckPTXDKGTx(tx, nullptr, state), state.GetRejectReason());
}

// (P-b1 row 6) nVersion occupies the LEADING bytes of the stream — the invariant
// the whole conditional-parse scheme depends on, pinned by a NAMED test
// independent of the golden vector (a reorder fails HERE by name, not via a hex
// mismatch someone might "fix" by recapturing the golden). 0x1234 is
// distinguishable from every fill pattern; LE on the wire -> leading bytes 3412.
// RED (inversion C): move READWRITE(obj.nVersion) after quorum_hash -> the
// leading bytes are 0x22-fill -> fail.
// ---------------------------------------------------------------------------
// KDD-072 P-b2 — Phase 4 sign-hash predecessor binding (dormant; trigger P-b6)
// ---------------------------------------------------------------------------

// Wrap REAL ceremony members (genuine operator keys, genuine premits) as the
// canonical quorum vector and run PTX_DKG_VerifyPremits over them — the
// validation_tests MakeGM shape, but sourced from the fixture ceremony so the
// §3 vectors are real signatures, not synthetic ones.
static bool PTX_TEST_VerifyPremitsOverMembers(const std::vector<PTXDKGMember>& members,
                                              const PTXDKGPayload& payload,
                                              CValidationState& state)
{
    std::vector<CDeterministicGMCPtr> gms;
    uint64_t id = 0;
    for (const auto& m : members) {
        auto dgm = std::make_shared<CDeterministicGM>(id++);
        dgm->proTxHash = m.proTxHash;
        auto st = std::make_shared<CDeterministicGMState>();
        st->pubKeyOperator.Set(m.pubKeyOperator);
        st->node_id = m.node_id;
        dgm->pdgmState = st;
        gms.push_back(dgm);
    }
    return PTX_DKG_VerifyPremits(gms, payload, state);
}

// (P-b2) fresh sign-hash golden. The hex was CAPTURED BY EXECUTION at pre-edit
// HEAD (this row lands green against the parameterless pre-P-b2 GetSignHash
// first; the P-b2 edit must reproduce it byte-for-byte for a zero predecessor).
// Preimage: SHA256( quorum_hash[32] || proTxHash[32] || group_pk[48] ||
// vvec_hash[32] ) = 144 bytes — a v1 sign-hash must not move by one bit.
// RED (inversion): append the predecessor unconditionally (even when zero) ->
// the fresh preimage becomes 176 bytes -> mismatch. Note this golden is the
// ONLY row that catches a uniform preimage drift — sign and verify move
// together everywhere else.
BOOST_AUTO_TEST_CASE(Pb2_FreshSignHash_Golden)
{
    PTXDKGPhase4Msg m;
    std::memset(m.quorum_hash.begin(), 0x22, 32);
    std::memset(m.proTxHash.begin(), 0x55, 32);
    std::memset(m.group_pk_bytes, 0x33, 48);
    m.vvec_hash = QHk(0x44);
    BOOST_CHECK_EQUAL(m.GetSignHash(uint256()).GetHex(),
        "761090b80c3c5421e12e902929a2990aafb5bddddb96a16a44e22e8250b3aa3d");
}

// (P-b2) rotation sign-hash vector: SHA256 over the 176-byte preimage with the
// predecessor appended LAST. Independently derived (sha256 of the fixed byte
// pattern); no pre-edit truth exists for the v2 preimage — it is new.
// RED (inversion): strip the predecessor from the preimage -> equals the fresh
// hash instead.
BOOST_AUTO_TEST_CASE(Pb2_RotationSignHash_Vector)
{
    PTXDKGPhase4Msg m;
    std::memset(m.quorum_hash.begin(), 0x22, 32);
    std::memset(m.proTxHash.begin(), 0x55, 32);
    std::memset(m.group_pk_bytes, 0x33, 48);
    m.vvec_hash = QHk(0x44);
    BOOST_CHECK_EQUAL(m.GetSignHash(QHk(0x77)).GetHex(),
        "d75ea8cfc618d0c66df57dcb44e40b7170fc7fbf110fee31377d6a88a25de506");
    // and the two preimages are distinct (predecessor genuinely bound):
    BOOST_CHECK(m.GetSignHash(QHk(0x77)) != m.GetSignHash(uint256()));
}

// (P-b2) ★ THE §3 HOLE TEST — a legitimately-formed, legitimately-premitted
// FORMATION cannot be re-cast as a rotation. CONTROL first (the same payload
// passes VerifyPremits before the re-cast — everything else is valid), then the
// ATTACK (two-field re-cast only: nVersion + predecessor). VerifyPremits is
// called DIRECTLY (pure fn) so the version gate cannot mask the property, and
// the reject reason must be EXACTLY ptxdkg-bad-premit-sig — V7a-V7f all pass by
// construction (same premits, same quorum, same keys; quorum_hash/group_pk/vvec
// untouched by the re-cast), so V7g is the isolated cause.
// RED (inversion A): strip the predecessor from V7g's recompute -> the attack
// payload VERIFIES (signed v1, checked v1) -> "attack succeeded" -> fail.
BOOST_AUTO_TEST_CASE(Pb2_HoleTest_RecastFormationFails)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    PTX_TEST_ClearSkShareSlot();
    AdvanceToFinalize(key_map, sessions);
    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));

    // The canonical quorum11 as VerifyPremits wants it: the session's members
    // materialized as DGM ptrs is a validation-side concern (P-b3); here the
    // pure function only needs proTxHash->operator-key agreement, which the
    // test harness provides via the session member set.
    std::vector<PTXDKGMember> members = sessions[0].members;

    // CONTROL: the genuine v1 payload verifies (proves the vectors are valid
    // before the re-cast — the isolation baseline).
    {
        CValidationState st;
        BOOST_REQUIRE_MESSAGE(PTX_TEST_VerifyPremitsOverMembers(members, payload, st),
            "control failed: genuine formation premits did not verify — vectors invalid");
    }

    // ATTACK: two-field re-cast. Nothing else changes.
    payload.nVersion = PTXDKGPayload::ROTATION_VERSION;
    payload.predecessor_quorum_hash = QHk(0xAA);
    {
        CValidationState st;
        const bool verified = PTX_TEST_VerifyPremitsOverMembers(members, payload, st);
        BOOST_CHECK_MESSAGE(!verified,
            "attack succeeded: formation premits verified as a rotation — the "
            "KDD-072 §3 unsigned-predecessor hole is OPEN");
        BOOST_CHECK_EQUAL(st.GetRejectReason(), "ptxdkg-bad-premit-sig");
    }
}

// (P-b2) genuine rotation verifies + marker-strip fails (KDD-072 §7 second
// disposition). Premits SIGNED over a predecessor (all 11 sessions share the
// view — the honest-rotation shape) -> the matching v2 payload PASSES; the
// marker-stripped variant (predecessor zeroed, nVersion=1 — an attacker hiding
// the rotation) FAILS V7g (signed 176-byte preimage, checked 144).
// RED (inversion A strips V7g -> stripped variant passes; inversion B strips
// the Build side -> the genuine-pass limb fails).
BOOST_AUTO_TEST_CASE(Pb2_GenuineRotation_VerifiesAndMarkerStripFails)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    PTX_TEST_ClearSkShareSlot();
    AdvanceToPremit(key_map, sessions);

    const uint256 pred = QHk(0x66);
    for (int i = 0; i < 11; i++)
        sessions[i].predecessor_quorum_hash = pred;   // shared honest view

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }
    std::vector<PTXDKGPhase4Msg> p4msgs(11);
    for (int s = 0; s < 11; s++)
        p4msgs[s] = PTX_DKG_BuildPhase4Msg(sessions[s], key_map.at(PtxOf(sessions, s)));
    for (int r = 0; r < 11; r++)
        for (int s = 0; s < 11; s++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[r], p4msgs[s]));
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase4(sessions[i]));

    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    PTXDKGPayload payload;
    BOOST_REQUIRE(GetTxPayload(mtx, payload));
    const uint16_t rot = PTXDKGPayload::ROTATION_VERSION; // local avoids ODR-use
    BOOST_CHECK_EQUAL(payload.nVersion, rot);             // Phase-5 consumer emitted v2
    BOOST_CHECK(payload.predecessor_quorum_hash == pred);

    std::vector<PTXDKGMember> members = sessions[0].members;
    {   // genuine rotation: premits attest this predecessor -> verifies
        CValidationState st;
        BOOST_CHECK(PTX_TEST_VerifyPremitsOverMembers(members, payload, st));
    }
    {   // marker-strip: hide the rotation -> V7g must fail
        PTXDKGPayload stripped = payload;
        stripped.nVersion = PTXDKGPayload::CURRENT_VERSION;
        stripped.predecessor_quorum_hash.SetNull();
        CValidationState st;
        BOOST_CHECK_MESSAGE(!PTX_TEST_VerifyPremitsOverMembers(members, stripped, st),
            "marker-strip succeeded: rotation premits verified as a plain formation");
        BOOST_CHECK_EQUAL(st.GetRejectReason(), "ptxdkg-bad-premit-sig");
    }
}

// (P-b2) Receive check-7 divergence: a peer signing over a DIFFERENT predecessor
// than my session's view is rejected at the ceremony hot path (the transport R4
// twin verifies the same preimage via SignHashOf). Control: a matching-view
// peer is accepted. RED (inversion C): strip the predecessor from Receive
// check 7 -> the divergent premit is accepted -> fail.
BOOST_AUTO_TEST_CASE(Pb2_Receive_DivergentPredecessorRejected)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    PTX_TEST_ClearSkShareSlot();
    AdvanceToPremit(key_map, sessions);

    // My view: rotation of 0x66. Peers: one matching, one divergent (0x99).
    sessions[0].predecessor_quorum_hash = QHk(0x66);
    sessions[1].predecessor_quorum_hash = QHk(0x66);   // matching peer
    sessions[2].predecessor_quorum_hash = QHk(0x99);   // divergent peer

    for (int i = 0; i < 3; i++) {
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }
    PTXDKGPhase4Msg match = PTX_DKG_BuildPhase4Msg(sessions[1], key_map.at(PtxOf(sessions, 1)));
    PTXDKGPhase4Msg diverge = PTX_DKG_BuildPhase4Msg(sessions[2], key_map.at(PtxOf(sessions, 2)));

    BOOST_CHECK(PTX_DKG_ReceivePhase4Msg(sessions[0], match));     // same view: accepted
    BOOST_CHECK(!PTX_DKG_ReceivePhase4Msg(sessions[0], diverge));  // divergent: rejected
    BOOST_CHECK_EQUAL(sessions[0].phase4_premit_msgs.count(PtxOf(sessions, 2)), 0u);
}

// (P-b2) Phase-5 dormant consumers, fresh path UNCHANGED + rotation path feeds
// KDD-070: ClosePhase5 on a fresh session stores CURRENT and emits v1 (the
// pre-P-b2 behaviour, byte-guarded elsewhere); on a rotation session it stores
// role=PENDING and emits v2+predecessor. RED (inversion D): remove the role/
// version derivation -> the rotation limb stores CURRENT / emits v1 -> fail.
BOOST_AUTO_TEST_CASE(Pb2_ClosePhase5_RotationStoresPendingEmitsV2)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    PTX_TEST_ClearSkShareSlot();
    AdvanceToPremit(key_map, sessions);

    const uint256 pred = QHk(0x66);
    for (int i = 0; i < 11; i++)
        sessions[i].predecessor_quorum_hash = pred;
    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }
    std::vector<PTXDKGPhase4Msg> p4msgs(11);
    for (int s = 0; s < 11; s++)
        p4msgs[s] = PTX_DKG_BuildPhase4Msg(sessions[s], key_map.at(PtxOf(sessions, s)));
    for (int r = 0; r < 11; r++)
        for (int s = 0; s < 11; s++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[r], p4msgs[s]));
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase4(sessions[i]));

    CMutableTransaction tx_out;
    // AddAndRelay inside ClosePhase5 will REFUSE the v2 commitment (the gate
    // still rejects v2 until P-b3) — logged, non-fatal by design; the store
    // and the returned tx are what this row asserts.
    BOOST_REQUIRE(PTX_DKG_ClosePhase5(sessions[0], 1000, tx_out));

    {   // role: PENDING, not CURRENT (KDD-070 §8 store-pending at FINALIZE)
        LOCK(cs_ptx_my_bls_sk);
        auto it = g_ptx_my_shares.find(sessions[0].quorum_hash);
        BOOST_REQUIRE(it != g_ptx_my_shares.end());
        BOOST_CHECK(it->second.role == PTXShareRole::PENDING);
    }
    PTXDKGPayload out;
    BOOST_REQUIRE(GetTxPayload(CMutableTransaction(tx_out), out));
    const uint16_t rot = PTXDKGPayload::ROTATION_VERSION; // local avoids ODR-use
    BOOST_CHECK_EQUAL(out.nVersion, rot);
    BOOST_CHECK(out.predecessor_quorum_hash == pred);
}

BOOST_AUTO_TEST_CASE(Pb1_VersionFieldSerializesFirst)
{
    PTXDKGPayload in;
    in.nVersion = 0x1234;
    std::memset(in.quorum_hash.begin(), 0x22, 32);
    std::memset(in.group_pk_bytes, 0x33, 48);
    in.vvec_hash = QHk(0x44);
    in.formation_height = 880;
    in.predecessor_quorum_hash = QHk(0x77);   // 0x1234 >= 2: serialized, harmless here

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << in;
    BOOST_CHECK_EQUAL(HexStr(ss).substr(0, 4), "3412");  // uint16 LE, first on the wire
}

// ---------------------------------------------------------------------------
// KDD-072 P-b4 — ODC-042 as-of-height predicate + record v2 + supersede/revert
// ---------------------------------------------------------------------------

static CPTXQuorumRecord Pb4Rec(PTXQuorumState st, int mined, int sh = -1, int dh = -1)
{
    CPTXQuorumRecord r;
    r.quorum_hash       = QHk(0xC1);
    r.mined_height      = mined;
    r.state             = static_cast<uint8_t>(st);
    r.superseded_height = sh;
    r.disbanded_height  = dh;
    return r;
}

// (P-b4) ★ THE BOUNDARY — strict >. A record superseded AT h is NOT active at
// h (the driver at anchor h already saw the flip; >= keeps it active for the
// validator only → pool divergence → chain split). RED (inversion): flip > to
// >= in the SUPERSEDED arm → the ==h case flips to active → this row fails.
BOOST_AUTO_TEST_CASE(Pb4_Boundary_StrictGreater)
{
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(Pb4Rec(PTXQuorumState::SUPERSEDED, 500, 900), 900)); // ==h: INACTIVE
    BOOST_CHECK( PTX_QuorumRecordActiveAt(Pb4Rec(PTXQuorumState::SUPERSEDED, 500, 901), 900)); // ==h+1: active
}

// (P-b4) as-of sweep + mined gate: superseded at 900 → active at 899, inactive
// at 900 and 901; not yet mined (h < mined_height) → inactive regardless.
BOOST_AUTO_TEST_CASE(Pb4_AsOf_Sweep)
{
    const CPTXQuorumRecord r = Pb4Rec(PTXQuorumState::SUPERSEDED, 500, 900);
    BOOST_CHECK( PTX_QuorumRecordActiveAt(r, 899));
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(r, 900));
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(r, 901));
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(Pb4Rec(PTXQuorumState::ACTIVE, 500), 499)); // mined gate
    // stamped-state with corrupt sentinel: fail-safe inactive at every h
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(Pb4Rec(PTXQuorumState::SUPERSEDED, 500, -1), 1000));
}

// (P-b4) a v1 record (both stamp fields never on the wire) deserializes with
// sentinels and answers through the ACTIVE arm — no migration. Also pins that
// v1 streams are 8 bytes shorter (the two int32s absent).
BOOST_AUTO_TEST_CASE(Pb4_V1Record_SentinelsActive)
{
    CPTXQuorumRecord in = Pb4Rec(PTXQuorumState::ACTIVE, 500, 777, 888);
    in.nVersion = 2;                               // EXPLICIT v2 (W4-c: default is
                                                   // now v3; this test pins the
                                                   // v2-vs-v1 delta, so it must
                                                   // serialize a v2 stream)
    CDataStream ssv2(SER_DISK, 0);
    ssv2 << in;                                    // v2: stamps on the wire
    in.nVersion = 1;
    CDataStream ssv1(SER_DISK, 0);
    ssv1 << in;                                    // v1: stamps skipped
    BOOST_CHECK_EQUAL(ssv2.size() - ssv1.size(), (size_t)8);

    CPTXQuorumRecord out;
    ssv1 >> out;                                   // fresh object, v1 stream
    BOOST_CHECK_EQUAL(out.nVersion, (uint8_t)1);
    BOOST_CHECK_EQUAL(out.superseded_height, -1);  // sentinel, not 777
    BOOST_CHECK_EQUAL(out.disbanded_height, -1);   // sentinel, not 888
    BOOST_CHECK(PTX_QuorumRecordActiveAt(out, 500)); // ACTIVE arm answers v1
}

// (P-b4) record-v2 round-trip: both stamps serialize and reload.
// RED (inversion): drop the v2 conditional → both reload as sentinels.
BOOST_AUTO_TEST_CASE(Pb4_V2RoundTrip_BothStamps)
{
    CPTXQuorumRecord in = Pb4Rec(PTXQuorumState::SUPERSEDED, 500, 123, 456);
    in.nVersion = 2;                               // EXPLICIT v2 (W4-c: see above)
    CDataStream ss(SER_DISK, 0);
    ss << in;
    CPTXQuorumRecord out;
    ss >> out;
    BOOST_CHECK_EQUAL(out.superseded_height, 123);
    BOOST_CHECK_EQUAL(out.disbanded_height, 456);
    BOOST_CHECK_EQUAL(out.nVersion, (uint8_t)2);
}

// (P-b4) MarkSuperseded flips + stamps + bumps to v2; refuse-unless-ACTIVE
// rejects a double-flip. Store-level via the in-memory evoDb (P2 pattern;
// seeded through the same key the store reads — PTX_QuorumRecordDBPrefix).
// RED (inversion): remove the ACTIVE check → the double-flip returns true.
BOOST_AUTO_TEST_CASE(Pb4_MarkSuperseded_FlipsStampsRefuses)
{
    BOOST_REQUIRE(evoDb);
    CPTXQuorumStore store(*evoDb);
    const uint256 qh = QHk(0xC4);
    CPTXQuorumRecord seed = Pb4Rec(PTXQuorumState::ACTIVE, 880);
    seed.quorum_hash = qh;
    evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), qh), seed); // CEvoDB::Write returns void

    BOOST_CHECK(store.MarkSuperseded(qh, 950));
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK(rec.state == static_cast<uint8_t>(PTXQuorumState::SUPERSEDED));
    BOOST_CHECK_EQUAL(rec.superseded_height, 950);
    const uint8_t curv = CPTXQuorumRecord::CURRENT_VERSION; // local copy: BOOST_CHECK_EQUAL
                                                             // ODR-uses its args (the P-b1 lesson)
    BOOST_CHECK_EQUAL(rec.nVersion, curv);                   // vN on rewrite (W4-c: was literal 2)

    BOOST_CHECK(!store.MarkSuperseded(qh, 951));            // double-flip refused
    BOOST_CHECK(!store.MarkSuperseded(QHk(0xC5), 950));     // missing record refused
}

// (P-b4) the revert: SUPERSEDED→ACTIVE, stamp cleared; refuse-unless-SUPERSEDED
// makes a second revert (or a revert of a never-flipped record) a no-op.
// RED (inversion): remove the SUPERSEDED check → the second revert returns true.
BOOST_AUTO_TEST_CASE(Pb4_Restore_RevertsClearsIdempotent)
{
    BOOST_REQUIRE(evoDb);
    CPTXQuorumStore store(*evoDb);
    const uint256 qh = QHk(0xC6);
    CPTXQuorumRecord seed = Pb4Rec(PTXQuorumState::ACTIVE, 880);
    seed.quorum_hash = qh;
    evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), qh), seed); // CEvoDB::Write returns void

    BOOST_REQUIRE(store.MarkSuperseded(qh, 950));
    BOOST_CHECK(store.RestoreActiveOnUndo(qh));
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK(rec.state == static_cast<uint8_t>(PTXQuorumState::ACTIVE));
    BOOST_CHECK_EQUAL(rec.superseded_height, -1);
    // replay determinism: flip again after revert → same stamp shape
    BOOST_CHECK(store.MarkSuperseded(qh, 950));
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL(rec.superseded_height, 950);
    BOOST_CHECK(store.RestoreActiveOnUndo(qh));
    BOOST_CHECK(!store.RestoreActiveOnUndo(qh));            // idempotent no-op
}

// (P-b4) happy path bit-identical: for ACTIVE records (every record on every
// zero-rotation chain) the predicate reduces exactly to the pre-P-b4 filter
// (state==ACTIVE, mined_height<=h) at every height.
BOOST_AUTO_TEST_CASE(Pb4_HappyPath_BitIdentical)
{
    for (int mined : {100, 500, 900}) {
        const CPTXQuorumRecord r = Pb4Rec(PTXQuorumState::ACTIVE, mined);
        for (int h : {99, 100, 101, 499, 500, 501, 899, 900, 10000}) {
            const bool old_filter = (mined <= h); // pre-P-b4: ACTIVE + mined gate
            BOOST_CHECK_EQUAL(PTX_QuorumRecordActiveAt(r, h), old_filter);
        }
    }
}

// (P-b4) the DISBANDED arm — vacuous at HEAD (no producer feeds
// disbanded_height; W2.4 T-H owes the stamp), but the schema + predicate are
// in place so W2.4 adds a producer, not a format/predicate change. Same strict
// boundary as SUPERSEDED; -1 sentinel fail-safe inactive.
BOOST_AUTO_TEST_CASE(Pb4_DisbandedArm_VacuousButCorrect)
{
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(Pb4Rec(PTXQuorumState::DISBANDED, 500, -1, -1), 1000)); // unfed: inactive
    BOOST_CHECK( PTX_QuorumRecordActiveAt(Pb4Rec(PTXQuorumState::DISBANDED, 500, -1, 900), 899)); // as-of active
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(Pb4Rec(PTXQuorumState::DISBANDED, 500, -1, 900), 900)); // strict boundary
}

// ---------------------------------------------------------------------------
// KDD-072 P-b3a — the KDD-073 atomic core: shared materialization + V12 checks
// ---------------------------------------------------------------------------

// MakeGM-shape fixture (validation_tests precedent) — real BLS operator keys,
// distinct score identities.
static std::shared_ptr<CDeterministicGM> Pb3aGM(uint64_t id, const CBLSSecretKey& opSk)
{
    std::vector<unsigned char> pb(32, 0);
    pb[0] = (unsigned char)id; pb[1] = 0xB3;
    uint256 proTx(pb);
    auto dgm = std::make_shared<CDeterministicGM>(id);
    dgm->proTxHash          = proTx;
    dgm->collateralOutpoint = COutPoint(proTx, 0);
    auto st = std::make_shared<CDeterministicGMState>();
    std::vector<unsigned char> cb(32, 0x11);
    cb[0] = (unsigned char)id;
    st->UpdateConfirmedHash(proTx, uint256(cb));
    st->pubKeyOperator.Set(opSk.GetPublicKey());
    uint160 k20; memcpy(k20.begin(), proTx.begin(), 20);
    st->keyIDOwner  = CKeyID(k20);
    st->keyIDVoting = st->keyIDOwner;
    st->node_id     = "gm" + std::to_string(id) + ":8080";
    dgm->pdgmState = st;
    return dgm;
}

struct Pb3aWorld {
    std::vector<std::shared_ptr<CDeterministicGM>> dgms;   // all 14 (non-const for rekey rebuild)
    CDeterministicGMList list;                   // the anchor list
    std::map<uint256, CBLSSecretKey> sks;        // proTxHash -> operator sk
    std::vector<PTXDKGMember> fresh;             // the fresh 11 (score order)
    CPTXQuorumRecord rec;                        // predecessor record of the fresh 11
    uint256 fbh;                                 // formation anchor hash
};

static Pb3aWorld Pb3aMakeWorld()
{
    Pb3aWorld w;
    w.fbh = QHk(0xB0);
    for (uint64_t id = 1; id <= 14; id++) {
        CBLSSecretKey sk; sk.MakeNewKey();
        auto dgm = Pb3aGM(id, sk);
        w.dgms.push_back(dgm);
        w.sks[dgm->proTxHash] = sk;
        w.list.AddGM(dgm);
    }
    w.fresh = PTX_DKG_BuildMemberVectorFromList(w.list, w.fbh);
    BOOST_REQUIRE_EQUAL(w.fresh.size(), (size_t)11);
    w.rec.quorum_hash     = w.fbh;               // record identity = formation anchor
    w.rec.mined_height    = 100;
    w.rec.state           = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    w.rec.formed_size     = 11;
    for (size_t i = 0; i < w.fresh.size(); i++) {
        PTXQuorumMemberRecord m;
        m.node_id     = w.fresh[i].node_id;
        m.proTxHash   = w.fresh[i].proTxHash;
        m.share_index = (uint8_t)(i + 1);        // the connect-time rank rule
        m.in_qual     = true;
        w.rec.members.push_back(m);
    }
    return w;
}

// (P-b3a) ★ MATERIALIZATION EQUALITY — the byte-identical guarantee. The
// rotation path (ResolveRotationQuorum → MembersFromQuorum) must reproduce the
// driver's fresh materialization (BuildMemberVectorFromList) field-by-field in
// share_index order; both flow through the SAME mapper, so this row pins the
// order+resolution half. RED (inversion 1): resolve in reverse record order →
// every field pairwise mismatches.
BOOST_AUTO_TEST_CASE(Pb3a_MaterializationEquality)
{
    Pb3aWorld w = Pb3aMakeWorld();
    std::vector<CDeterministicGMCPtr> quorum;
    std::string err;
    BOOST_REQUIRE_MESSAGE(
        PTX_DKG_ResolveRotationQuorum(w.rec, w.list, w.list, quorum, err), err);
    const std::vector<PTXDKGMember> rot = PTX_DKG_MembersFromQuorum(quorum);

    BOOST_REQUIRE_EQUAL(rot.size(), w.fresh.size());
    for (size_t i = 0; i < rot.size(); i++) {
        BOOST_CHECK(rot[i].proTxHash == w.fresh[i].proTxHash);
        BOOST_CHECK(rot[i].confirmedHash == w.fresh[i].confirmedHash);
        BOOST_CHECK(rot[i].confirmedHashWithProRegTxHash ==
                    w.fresh[i].confirmedHashWithProRegTxHash);
        BOOST_CHECK_EQUAL(rot[i].node_id, w.fresh[i].node_id);
        BOOST_CHECK(rot[i].pubKeyOperator == w.fresh[i].pubKeyOperator);
    }
}

// (P-b3a) V12 accepts a valid same-set rotation and the substituted quorum11
// flows through V6-V8 (VerifyPremits with the P-b2 predecessor sign-hash) and
// the V10 containment shape. RED (inversion 1 reverses order — VerifyPremits
// still passes by map lookup, the EQUALITY row is the order catcher; inversion
// 3 makes the as-of leg fail).
BOOST_AUTO_TEST_CASE(Pb3a_V12_AcceptsValidRotation)
{
    Pb3aWorld w = Pb3aMakeWorld();

    // The rotation payload: v2, predecessor = the record, premits signed over
    // the predecessor sign-hash by each member's ANCHOR-TIME operator key.
    PTXDKGPayload pl;
    pl.nVersion = PTXDKGPayload::ROTATION_VERSION;
    pl.quorum_hash = QHk(0xE1);                          // rotation anchor
    pl.predecessor_quorum_hash = w.rec.quorum_hash;
    std::memset(pl.group_pk_bytes, 0x33, 48);
    pl.vvec_hash = QHk(0x44);
    pl.formation_height = 500;
    for (const auto& m : w.rec.members) pl.member_node_ids.push_back(m.node_id);
    for (const auto& m : w.rec.members) {
        PTXDKGPhase4Msg p4;
        p4.quorum_hash = pl.quorum_hash;
        p4.proTxHash   = m.proTxHash;
        std::memcpy(p4.group_pk_bytes, pl.group_pk_bytes, 48);
        p4.vvec_hash   = pl.vvec_hash;
        p4.sig = w.sks.at(m.proTxHash).Sign(p4.GetSignHash(pl.predecessor_quorum_hash));
        pl.premit_commitments[m.proTxHash] = p4;
    }

    // V12b+V12c (the shared core both consensus sites call):
    std::vector<CDeterministicGMCPtr> quorum11;
    CValidationState st;
    BOOST_REQUIRE_MESSAGE(
        PTX_DKG_CheckRotationAndResolve(w.rec, 500, w.list, w.list, quorum11, st),
        st.GetRejectReason());
    BOOST_REQUIRE_EQUAL(quorum11.size(), (size_t)11);

    // V10 shape: committed ⊆ substituted selection, no dups.
    {
        std::set<std::string> selected;
        for (const auto& d : quorum11) selected.insert(d->pdgmState->node_id);
        std::set<std::string> seen;
        for (const auto& nid : pl.member_node_ids) {
            BOOST_CHECK(selected.count(nid) == 1);
            BOOST_CHECK(seen.insert(nid).second);
        }
    }
    // V6-V8 verbatim over the substituted quorum11 — the P-b2 sign-hash now
    // verified against a REAL non-zero predecessor for the first time.
    CValidationState st2;
    BOOST_CHECK_MESSAGE(PTX_DKG_VerifyPremits(quorum11, pl, st2), st2.GetRejectReason());
}

// (P-b3a) ★ MISSING-MEMBER REJECT — one policy, both sites. A predecessor
// member absent from the rotation-anchor list rejects the WHOLE rotation
// (never member-exclusion): the shared resolver refuses, so the V12-shaped
// call AND the driver wrapper both reject from the same decision.
// RED (inversion 4): skip-absent-instead-of-reject → resolve "succeeds" with
// 10 members and both limbs fail.
BOOST_AUTO_TEST_CASE(Pb3a_MissingMember_RejectsBothSites)
{
    Pb3aWorld w = Pb3aMakeWorld();
    // Rebuild the rotation-anchor list WITHOUT the first selected member.
    CDeterministicGMList listMissing;
    for (const auto& d : w.dgms) {
        if (d->proTxHash == w.rec.members[0].proTxHash) continue;
        listMissing.AddGM(d);
    }
    // V12-shaped: the shared core rejects with the named reason.
    std::vector<CDeterministicGMCPtr> q;
    CValidationState st;
    BOOST_CHECK(!PTX_DKG_CheckRotationAndResolve(w.rec, 500, listMissing, w.list, q, st));
    BOOST_CHECK_EQUAL(st.GetRejectReason(), "ptxdkg-rotation-member-unresolvable");
    // Driver-shaped: the rotation does not start — same helper, same verdict.
    std::vector<PTXDKGMember> members;
    BOOST_CHECK(!PTX_Formation_SelectRotationMembers(w.rec, listMissing, w.list, members));
    BOOST_CHECK(members.empty());
}

// (P-b3a) ★ ProUpReg KEY-ROTATION REJECT — the fork-hiding seam. A predecessor
// member whose operator key differs between the formation-anchor and
// rotation-anchor lists rejects the rotation at BOTH sites; requiring the two
// lists to AGREE is what pins "which block's key" for premit verification
// (driver and validator resolve through the same equality). RED (inversion 2):
// drop the key-equality check → both limbs fail.
BOOST_AUTO_TEST_CASE(Pb3a_KeyRotation_RejectsBothSites)
{
    Pb3aWorld w = Pb3aMakeWorld();
    // Rotation-anchor list where member[0] has a ProUpReg'd (new) operator key.
    CBLSSecretKey newSk; newSk.MakeNewKey();
    CDeterministicGMList listRekeyed;
    for (size_t di = 0; di < w.dgms.size(); di++) {
        const auto& d = w.dgms[di];
        if (d->proTxHash == w.rec.members[0].proTxHash) {
            auto re = Pb3aGM((uint64_t)di + 1, newSk);   // ids are creation-order 1..14
            // same identity, new key: keep proTxHash/confirmed identity
            re->proTxHash = d->proTxHash;
            auto st2 = std::make_shared<CDeterministicGMState>(*d->pdgmState);
            st2->pubKeyOperator.Set(newSk.GetPublicKey());
            re->pdgmState = st2;
            listRekeyed.AddGM(re);
        } else {
            listRekeyed.AddGM(d);
        }
    }
    std::vector<CDeterministicGMCPtr> q;
    CValidationState st;
    BOOST_CHECK(!PTX_DKG_CheckRotationAndResolve(w.rec, 500, listRekeyed, w.list, q, st));
    BOOST_CHECK_EQUAL(st.GetRejectReason(), "ptxdkg-rotation-member-unresolvable");
    std::vector<PTXDKGMember> members;
    BOOST_CHECK(!PTX_Formation_SelectRotationMembers(w.rec, listRekeyed, w.list, members));
}

// (P-b3a) predecessor NOT ACTIVE as-of → V12b rejects — and proves V12 reads
// P-b4's AS-OF predicate, not raw current state: the same SUPERSEDED record IS
// accepted at a height below its stamp. RED (inversion 3): swap the predicate
// for raw state==ACTIVE → the below-stamp leg wrongly rejects.
BOOST_AUTO_TEST_CASE(Pb3a_PredecessorNotActiveAsOf_Rejects)
{
    Pb3aWorld w = Pb3aMakeWorld();
    w.rec.state             = static_cast<uint8_t>(PTXQuorumState::SUPERSEDED);
    w.rec.superseded_height = 1000;

    std::vector<CDeterministicGMCPtr> q;
    {   // at/after the stamp: not active as-of → reject
        CValidationState st;
        BOOST_CHECK(!PTX_DKG_CheckRotationAndResolve(w.rec, 1000, w.list, w.list, q, st));
        BOOST_CHECK_EQUAL(st.GetRejectReason(), "ptxdkg-rotation-predecessor-not-active");
    }
    {   // below the stamp: ACTIVE as-of (P-b4 semantics) → V12b passes and the
        // resolve succeeds — raw current-state would wrongly reject here
        CValidationState st;
        BOOST_CHECK_MESSAGE(
            PTX_DKG_CheckRotationAndResolve(w.rec, 999, w.list, w.list, q, st),
            st.GetRejectReason());
        BOOST_CHECK_EQUAL(q.size(), (size_t)11);
    }
}

// ---------------------------------------------------------------------------
// KDD-072 P-b5 — the predecessor-uniqueness index (pq_p, the PRIMARY guard)
// ---------------------------------------------------------------------------

// (P-b5) index lifecycle: absent -> write -> present -> erase -> absent. The
// erase leg IS the reorg re-allow (UndoBlock erases pq_p so a different
// successor may rotate the predecessor on the new branch).
// RED (inversion B2): make EraseSuccessorOf a no-op -> the re-allow legs fail.
BOOST_AUTO_TEST_CASE(Pb5_Index_WriteEraseReallow)
{
    BOOST_REQUIRE(evoDb);
    CPTXQuorumStore store(*evoDb);
    const uint256 pred = QHk(0xD1), succ = QHk(0xD2);

    BOOST_CHECK(!store.HasSuccessorOf(pred));                 // absent
    BOOST_CHECK(store.WriteSuccessorOf(pred, succ));          // write
    BOOST_CHECK(store.HasSuccessorOf(pred));                  // present
    store.EraseSuccessorOf(pred);                             // the reorg re-allow
    BOOST_CHECK(!store.HasSuccessorOf(pred));                 // absent again
    store.EraseSuccessorOf(pred);                             // idempotent no-op
    BOOST_CHECK(!store.HasSuccessorOf(pred));
    BOOST_CHECK(store.WriteSuccessorOf(pred, QHk(0xD3)));     // a DIFFERENT successor may now rotate
}

// (P-b5) connect-write refuse-unless-absent: a second write to the same
// predecessor key is a DEFECT (V12d rejects the payload first), not an
// overwrite. RED (inversion B1): allow the overwrite -> this row fails.
BOOST_AUTO_TEST_CASE(Pb5_Write_RefusesUnlessAbsent)
{
    BOOST_REQUIRE(evoDb);
    CPTXQuorumStore store(*evoDb);
    const uint256 pred = QHk(0xD4);
    BOOST_REQUIRE(store.WriteSuccessorOf(pred, QHk(0xD5)));
    BOOST_CHECK(!store.WriteSuccessorOf(pred, QHk(0xD6)));    // refused
    BOOST_CHECK(store.HasSuccessorOf(pred));                  // first write intact
}

// (P-b5) V12d — ONE implementation, both consensus sites call it: a
// predecessor with an existing pq_p entry rejects with EXACTLY
// ptxdkg-predecessor-already-rotated; an unrotated predecessor passes.
// RED (inversion A): drop the HasSuccessorOf check -> the second-successor
// case passes the check chain -> this row fails (proving V12d is the guard,
// not incidental).
BOOST_AUTO_TEST_CASE(Pb5_V12d_RejectsSecondSuccessor)
{
    BOOST_REQUIRE(evoDb);
    CPTXQuorumStore store(*evoDb);
    const uint256 pred = QHk(0xD7);
    {   // unrotated: passes
        CValidationState st;
        BOOST_CHECK(store.CheckPredecessorUnrotated(pred, st));
    }
    BOOST_REQUIRE(store.WriteSuccessorOf(pred, QHk(0xD8)));   // first successor lands
    {   // second successor: the distinct V12d rejection
        CValidationState st;
        BOOST_CHECK(!store.CheckPredecessorUnrotated(pred, st));
        BOOST_CHECK_EQUAL(st.GetRejectReason(), "ptxdkg-predecessor-already-rotated");
    }
    // the reorg re-allow flows through the SAME check: erase -> passes again
    store.EraseSuccessorOf(pred);
    {
        CValidationState st;
        BOOST_CHECK(store.CheckPredecessorUnrotated(pred, st));
    }
}

// ---------------------------------------------------------------------------
// KDD-072 P-b6a — ceremony-start rotation wiring (dormant: due-rule stubbed)
// ---------------------------------------------------------------------------

// (P-b6a) ★ THE DORMANCY PIN: the due-rule stub returns due=false for EVERY
// anchor, so no rotation can start on any node. While this holds,
// StartFormationAtAnchor's else-arm is the only reachable path and fresh
// formation behaviour is byte-identical to pre-P-b6a.
// RED (inversion A): make the stub return due=true -> this row fails, and the
// whole rotation path becomes live — which is exactly what P-b6b will do
// deliberately, with a policy, not a constant.
// W2.4 W4-e: RotationDueAt takes the two eligibility sources.  The Pb6b rows
// run with the params gate at its 0 defaults (no chainparams enables it), so
// inert stubs preserve the pre-W4-e behaviour byte-identically.
static bool W4eStubReader(const CBlockIndex*, CBlock& out) { out = CBlock(); return true; }
static bool W4eStubImpossible(const CPTXQuorumRecord&, const CBlockIndex*) { return false; }

BOOST_AUTO_TEST_CASE(Pb6a_RotationDue_StubbedDisabled)
{
    BOOST_REQUIRE(evoDb);
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams& params = Params().GetConsensus().ptxFormation;
    // Every input shape the production call site can present — all disabled.
    const PTXRotationDecision d = PTX_Formation_RotationDueAt(nullptr, store, params, W4eStubReader, W4eStubImpossible);
    BOOST_CHECK(!d.due);
    BOOST_CHECK(d.predecessor_quorum_hash.IsNull());
}

// (P-b6a) GIVEN a rotation decision, the ceremony-start SEQUENCE the branch
// performs is correct end-to-end: resolver members -> InitSession -> the
// predecessor bound on the session -> ClosePhase5 derives role=PENDING and
// emits a v2 payload naming the predecessor. This is the branch's body proven
// as a composition (the `if` itself needs cs_main + a real chain — ODC-032,
// fleet-owed like every prior connect-path). The members are asserted EQUAL to
// the shared resolver's output, so the ceremony runs over exactly what V12 and
// the store guard will reconstruct.
// RED (inversion B): bind SetNull instead of the predecessor -> the session is
// fresh -> v1 payload, no predecessor, role CURRENT -> the wiring is inert.
BOOST_AUTO_TEST_CASE(Pb6a_GivenDue_WiringFeedsPendingAndV2)
{
    Pb3aWorld w = Pb3aMakeWorld();
    PTX_TEST_ClearSkShareSlot();

    // What the branch does, in order: resolve same-set members via the SHARED
    // resolver (P-b3a), then bind the predecessor onto the session.
    std::vector<PTXDKGMember> rotMembers;
    BOOST_REQUIRE(PTX_Formation_SelectRotationMembers(w.rec, w.list, w.list, rotMembers));
    {   // == the resolver's own output (the KDD-073 shared-code property)
        std::vector<CDeterministicGMCPtr> q;
        std::string err;
        BOOST_REQUIRE(PTX_DKG_ResolveRotationQuorum(w.rec, w.list, w.list, q, err));
        const std::vector<PTXDKGMember> direct = PTX_DKG_MembersFromQuorum(q);
        BOOST_REQUIRE_EQUAL(rotMembers.size(), direct.size());
        for (size_t i = 0; i < rotMembers.size(); i++) {
            BOOST_CHECK(rotMembers[i].proTxHash == direct[i].proTxHash);
            BOOST_CHECK(rotMembers[i].pubKeyOperator == direct[i].pubKeyOperator);
        }
    }

    // Drive the 11 same-set members through a REAL ceremony with the
    // predecessor bound — the rotation ceremony P-b6b's trigger will start.
    const uint256 rotAnchor = QHk(0xE7);
    std::vector<PTXDKGSession> sessions(11);
    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_InitSession(sessions[i], rotMembers, rotAnchor,
                                          rotMembers[i].proTxHash));
        sessions[i].predecessor_quorum_hash = w.rec.quorum_hash;  // the branch's binding
        BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(sessions[i]));
    }
    std::vector<PTXDKGPhase0Msg> p0(11);
    for (int i = 0; i < 11; i++)
        p0[i] = PTX_DKG_BuildPhase0Msg(sessions[i], w.sks.at(rotMembers[i].proTxHash));
    for (int r = 0; r < 11; r++)
        for (int t = 0; t < 11; t++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase0Msg(sessions[r], p0[t]));
    for (int i = 0; i < 11; i++) BOOST_REQUIRE(PTX_DKG_ClosePhase0(sessions[i]));
    std::vector<PTXDKGPhase1Msg> p1(11);
    for (int i = 0; i < 11; i++)
        p1[i] = PTX_DKG_BuildPhase1Msg(sessions[i], w.sks.at(rotMembers[i].proTxHash));
    for (int r = 0; r < 11; r++)
        for (int t = 0; t < 11; t++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(sessions[r], p1[t]));
    // Decrypt from EVERY sender incl. self, then close each phase in its own
    // pass across all sessions — the fixture's proven order (AdvanceToComplaint
    // / AdvanceToPremit); ComputeSkShare's C6 completeness check requires a
    // received share for every effective-QUAL member, self included.
    for (int r = 0; r < 11; r++)
        for (int t = 0; t < 11; t++)
            PTX_DKG_DecryptMyShare(sessions[r], rotMembers[t].proTxHash,
                                   w.sks.at(rotMembers[r].proTxHash));
    for (int i = 0; i < 11; i++) BOOST_REQUIRE(PTX_DKG_ClosePhase1(sessions[i]));
    for (int i = 0; i < 11; i++) BOOST_REQUIRE(PTX_DKG_ClosePhase2(sessions[i]));
    for (int i = 0; i < 11; i++) BOOST_REQUIRE(PTX_DKG_ClosePhase3(sessions[i]));
    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(sessions[i].phase == PTXDKGPhase::PREMIT);
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }
    std::vector<PTXDKGPhase4Msg> p4(11);
    for (int i = 0; i < 11; i++)
        p4[i] = PTX_DKG_BuildPhase4Msg(sessions[i], w.sks.at(rotMembers[i].proTxHash));
    for (int r = 0; r < 11; r++)
        for (int t = 0; t < 11; t++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[r], p4[t]));
    for (int i = 0; i < 11; i++) BOOST_REQUIRE(PTX_DKG_ClosePhase4(sessions[i]));

    CMutableTransaction tx_out;
    BOOST_REQUIRE(PTX_DKG_ClosePhase5(sessions[0], 1200, tx_out));

    // ★ The payoff: a REAL rotation ceremony stores PENDING (so the connect-time
    // Promote finally has something to promote — the drill's P3 boundary) and
    // emits v2 naming the predecessor.
    {
        LOCK(cs_ptx_my_bls_sk);
        auto it = g_ptx_my_shares.find(rotAnchor);
        BOOST_REQUIRE(it != g_ptx_my_shares.end());
        BOOST_CHECK(it->second.role == PTXShareRole::PENDING);
    }
    PTXDKGPayload out;
    BOOST_REQUIRE(GetTxPayload(CMutableTransaction(tx_out), out));
    const uint16_t rot = PTXDKGPayload::ROTATION_VERSION; // local avoids ODR-use
    BOOST_CHECK_EQUAL(out.nVersion, rot);
    BOOST_CHECK(out.predecessor_quorum_hash == w.rec.quorum_hash);
}

// ---------------------------------------------------------------------------
// KDD-072 P-b6b — trigger policy (age + tie-break) and the three tip sweeps
// ---------------------------------------------------------------------------

// Seed an ACTIVE record with a chosen identity/age into the store's evodb.
static void Pb6bSeedActive(const uint256& qh, int formation_height, int mined_height)
{
    BOOST_REQUIRE(evoDb);
    CPTXQuorumRecord r;
    r.quorum_hash      = qh;
    r.formation_height = formation_height;
    r.mined_height     = mined_height;
    r.state            = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), qh), r);
    evoDb->Write(std::make_pair(std::string("pq_h"),
                 htobe32(std::numeric_limits<uint32_t>::max() - mined_height)), qh);
}

// A CBlockIndex standing in for the anchor (only nHeight is read).
static CBlockIndex Pb6bAnchor(int height)
{
    CBlockIndex idx;
    idx.nHeight = height;
    return idx;
}

// (P-b6b) THE AGE TEST at its boundary: not due one block short of N, due at
// exactly N. RED (inversion A: drop the tie-break/age arm) — see the tie-break
// row for the collision proof.
BOOST_AUTO_TEST_CASE(Pb6b_Trigger_AgeTestBoundary)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams& params = Params().GetConsensus().ptxFormation;
    const int N = params.nBoundaryInterval;   // KDD-079: boundaries

    const uint256 qh = QHk(0xA1);
    Pb6bSeedActive(qh, /*formation*/ 1000, /*mined*/ 1005);

    CBlockIndex early = Pb6bAnchor(1000 + N - 1);           // one short
    BOOST_CHECK(!PTX_Formation_RotationDueAt(&early, store, params, W4eStubReader, W4eStubImpossible).due);

    CBlockIndex due = Pb6bAnchor(1000 + N);                  // exactly N
    const PTXRotationDecision d = PTX_Formation_RotationDueAt(&due, store, params, W4eStubReader, W4eStubImpossible);
    BOOST_CHECK(d.due);
    BOOST_CHECK(d.predecessor_quorum_hash == qh);
}

// (P-b6b) ★ THE TIE-BREAK: two ACTIVE quorums BOTH due at the same anchor —
// only the LOWEST quorum_hash starts; the other waits for the next boundary.
// One PTXDKG can ever be accepted per anchor (V9 keys on quorum_hash == the
// anchor hash), so without this both ceremonies would race for one slot and
// the loser's full 11-member ceremony would be wasted.
// RED (inversion A): return the first due quorum instead of the lowest -> the
// decision becomes seed/iteration-order dependent and this row fails.
BOOST_AUTO_TEST_CASE(Pb6b_Trigger_TieBreakLowestHashOnly)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams& params = Params().GetConsensus().ptxFormation;
    const int N = params.nBoundaryInterval;   // KDD-079: boundaries

    const uint256 lo = QHk(0x11), hi = QHk(0x99);
    Pb6bSeedActive(lo, 2000, 2005);
    Pb6bSeedActive(hi, 2000, 2006);                          // both same age

    CBlockIndex anchor = Pb6bAnchor(2000 + N);               // both due
    const PTXRotationDecision d = PTX_Formation_RotationDueAt(&anchor, store, params, W4eStubReader, W4eStubImpossible);
    BOOST_REQUIRE(d.due);
    BOOST_CHECK(d.predecessor_quorum_hash == lo);            // lowest starts
    BOOST_CHECK(!(d.predecessor_quorum_hash == hi));         // the other waits
}

// ---------------------------------------------------------------------------
// W2.5a P3 — GUARD 2 (KDD-079 §4): the fairness floor over the tie-break.
// ---------------------------------------------------------------------------

// Divergent-cadence params: Guard 2's home terrain is B < R (at the shipped
// B == R defaults the floor sits at 3R and a lone quorum wins anyway).
static Consensus::PTXFormationParams G2Params()
{
    Consensus::PTXFormationParams p{};
    p.name = "g2";
    p.nBoundaryInterval = 10;
    p.nRotationInterval = 40;    // floor = 40 + 2*10 = 60
    p.nCeremonyBudget   = 80;
    p.nSupportedQuorums = 2;
    return p;
}

// Quorum records leak across rows in the shared evoDb (each row seeds, none
// erases).  The earlier rows tolerate that by hash choice; the G2 rows CANNOT
// — the override favours the OLDEST formation, so any leaked old record would
// hijack the selection.  Retire everything visible before seeding, THROUGH the
// store's own producer: a direct evoDb write behind a warm store is invisible
// (recordCache faults in on read and never invalidates — mid-test retires hit
// exactly that), while MarkReformed writes record AND cache in one lock.
// reformed_height 0 is never active at any probed height (strict predicate).
static void G2RetireAllActive(CPTXQuorumStore& store)
{
    for (const CPTXQuorumRecord& rec : store.GetActiveQuorumsAtHeight(1 << 30)) {
        BOOST_REQUIRE(store.MarkReformed(rec.quorum_hash, 0));
    }
}

// (P3) ★ GUARD 2 — THE FAIRNESS FLOOR.  A quorum whose hash loses EVERY
// contested tie-break (highest hash by construction) starves while merely
// due; once its age reaches R + 2B it wins the slot REGARDLESS of hash.
// Asserted as the INVARIANT (the overdue quorum wins), never as hash/height
// literals — heights are computed from the params (the W4d/W4f
// pin-the-property lesson, third application).
// RED (inversion: drop the override -> winner is always the lowest hash): the
// high-hash quorum loses at the floor too, and at every boundary after —
// starving INDEFINITELY past nRotationInterval.  KDD-045's key-compromise
// bound violated, surfacing as this row's failure.
// ★ HONEST SCOPE (recorded here AND in the KDD-079 entry): this row forces
// the tie-break loss ARTIFICIALLY at L=2 — the guard is LATENT at low L
// (capacity generous, nothing starves) and load-bearing only as L approaches
// R/B.  Real validation is env-gated to W2.5b at L=6-8 under genuine
// competition.
BOOST_AUTO_TEST_CASE(G2a_OverdueQuorumBeatsTieBreak)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = G2Params();
    G2RetireAllActive(store);

    const int R = params.nRotationInterval;
    const int floorAge = R + 2 * params.nBoundaryInterval;

    // The target: HIGHEST hash (loses every tie-break), formed earliest.  The
    // rival: lower hash, formed one boundary later — due-but-never-overdue at
    // both probed anchors, so it wins exactly while the tie-break rules.
    const uint256 target = QHk(0xEE), rival = QHk(0x22);
    BOOST_REQUIRE(rival < target);                 // the construction's premise
    const int tform = 5000;
    Pb6bSeedActive(target, tform, tform + 2);
    Pb6bSeedActive(rival,  tform + params.nBoundaryInterval,
                           tform + params.nBoundaryInterval + 2);

    // One block SHORT of the floor: both merely due -> the tie-break rules
    // and the target keeps losing (the starvation this guard exists to stop).
    CBlockIndex shortOf = Pb6bAnchor(tform + floorAge - 1);
    {
        const PTXRotationDecision d = PTX_Formation_RotationDueAt(
                &shortOf, store, params, W4eStubReader, W4eStubImpossible);
        BOOST_REQUIRE(d.due);
        BOOST_CHECK(d.predecessor_quorum_hash == rival);
    }
    // AT the floor: the overdue target wins its slot regardless of hash.
    CBlockIndex atFloor = Pb6bAnchor(tform + floorAge);
    {
        const PTXRotationDecision d = PTX_Formation_RotationDueAt(
                &atFloor, store, params, W4eStubReader, W4eStubImpossible);
        BOOST_REQUIRE(d.due);
        BOOST_CHECK_MESSAGE(d.predecessor_quorum_hash == target,
            "the overdue quorum did not win its slot - the fairness floor is "
            "inert and a high-hash quorum starves past nRotationInterval "
            "(KDD-045's key-compromise bound broken)");
    }
}

// (P3) THE ORDERING among several overdue: MOST-overdue-first regardless of
// hash; lowest hash only between EQUAL ages (deterministic on every node, the
// P-b6b shape); non-overdue quorums fall through to the tie-break untouched.
// RED (inversion: order the overdue set by hash): the less-starved low-hash
// quorum jumps the queue and the most-aged key keeps ageing — the first probe
// fails.
BOOST_AUTO_TEST_CASE(G2b_MostOverdueFirstLowestHashAmongEquals)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = G2Params();
    G2RetireAllActive(store);

    const int B = params.nBoundaryInterval;
    const int floorAge = params.nRotationInterval + 2 * B;

    // Both overdue, one boundary apart in age; the MORE overdue has the
    // HIGHER hash — most-overdue-first must beat lowest-hash.
    const uint256 aged = QHk(0xDD), less = QHk(0x11);
    const int aform = 6000;
    Pb6bSeedActive(aged, aform,     aform + 2);
    Pb6bSeedActive(less, aform + B, aform + B + 2);
    CBlockIndex anchor = Pb6bAnchor(aform + floorAge + B);   // ages: floor+B / floor
    {
        const PTXRotationDecision d = PTX_Formation_RotationDueAt(
                &anchor, store, params, W4eStubReader, W4eStubImpossible);
        BOOST_REQUIRE(d.due);
        BOOST_CHECK(d.predecessor_quorum_hash == aged);      // most overdue first
    }

    // EQUAL overdue ages: lowest hash between them — the selection stays
    // deterministic when the age ordering cannot discriminate.
    G2RetireAllActive(store);
    const uint256 eqLo = QHk(0x33), eqHi = QHk(0x77);
    const int eform = 7000;
    Pb6bSeedActive(eqLo, eform, eform + 2);
    Pb6bSeedActive(eqHi, eform, eform + 3);
    CBlockIndex equalAnchor = Pb6bAnchor(eform + floorAge);
    {
        const PTXRotationDecision d = PTX_Formation_RotationDueAt(
                &equalAnchor, store, params, W4eStubReader, W4eStubImpossible);
        BOOST_REQUIRE(d.due);
        BOOST_CHECK(d.predecessor_quorum_hash == eqLo);
    }
}

// (P3) ★ THE DEPLOY-SAFETY PIN, Guard-2 edition: at the shipped defaults
// (B == R, L == 1) the floor sits at 3R, and a lone quorum wins every
// tie-break anyway — the winner is IDENTICAL with or without the override,
// both while merely due and even once its age passes the floor (reachable at
// L=1 only through repeated ceremony failure).  Guard 2 is observationally a
// NO-OP at the single-quorum defaults, like Guards 1 and 3.  Like G1a this
// row asserts ACCEPTANCE and stays GREEN under the G2a/G2b inversions —
// correctly: a dropped override cannot change a lone quorum's selection.
BOOST_AUTO_TEST_CASE(G2c_LoneQuorumDefaultsUnchanged)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams& params = Params().GetConsensus().ptxFormation;
    BOOST_REQUIRE_EQUAL(params.nBoundaryInterval, params.nRotationInterval); // shipped shape
    G2RetireAllActive(store);

    const uint256 lone = QHk(0xC4);
    const int lform = 8000;
    Pb6bSeedActive(lone, lform, lform + 2);

    CBlockIndex due = Pb6bAnchor(lform + params.nRotationInterval);   // merely due
    CBlockIndex far = Pb6bAnchor(lform + 3 * params.nRotationInterval); // past the floor
    for (CBlockIndex* a : {&due, &far}) {
        const PTXRotationDecision d = PTX_Formation_RotationDueAt(
                a, store, params, W4eStubReader, W4eStubImpossible);
        BOOST_REQUIRE(d.due);
        BOOST_CHECK(d.predecessor_quorum_hash == lone);
    }
}

// (P-b6b) (a) ExpirePending: a PENDING past TTL is erased; within TTL survives.
BOOST_AUTO_TEST_CASE(Pb6b_Sweep_ExpirePending)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    const uint256 qh = QHk(0xB1);
    uint8_t sk[32]; std::memset(sk, 0x5A, 32);
    std::string err;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qh, /*formation_height*/ 100, sk,
                                     PTXShareRole::PENDING, err));
    PTX_BLS_ExpirePending(100 + PTX_PENDING_TTL_BLOCKS, evoDb.get());   // within TTL
    BOOST_CHECK_EQUAL(PTX_BLS_HeldQuorumHashes().count(qh), 1u);
    PTX_BLS_ExpirePending(100 + PTX_PENDING_TTL_BLOCKS + 1, evoDb.get()); // past TTL
    BOOST_CHECK_EQUAL(PTX_BLS_HeldQuorumHashes().count(qh), 0u);
}

// (P-b6b) DiscardSuperseded: a SUPERSEDED_RETAINED past depth erased; within
// depth survives.
BOOST_AUTO_TEST_CASE(Pb6b_Sweep_DiscardSuperseded)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    const uint256 qh = QHk(0xB2);
    {
        LOCK(cs_ptx_my_bls_sk);
        HeldShare hs; std::memset(hs.bytes, 0x6B, 32);
        hs.formation_height = 500;
        hs.role             = PTXShareRole::SUPERSEDED_RETAINED;
        hs.promotion_height = 600;
        g_ptx_my_shares[qh] = hs;
    }
    const int depth = DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN;
    PTX_BLS_DiscardSuperseded(600 + depth - 1, evoDb.get());   // within
    BOOST_CHECK_EQUAL(PTX_BLS_HeldQuorumHashes().count(qh), 1u);
    PTX_BLS_DiscardSuperseded(600 + depth, evoDb.get());       // at depth
    BOOST_CHECK_EQUAL(PTX_BLS_HeldQuorumHashes().count(qh), 0u);
}

// (P-b6b) ★ (b) THE RESIDUE RETIRE — the KDD-070 §5 bound, enforced.
// Three shares, one sweep: a CURRENT share whose record is SUPERSEDED past 120
// is DELETED; one within 120 survives; one whose record is ACTIVE is UNTOUCHED.
// ★ RED (inversion B): drop the state==SUPERSEDED guard -> the ACTIVE quorum's
// live working share is retired too — the catastrophe the guard prevents, which
// is what makes it load-bearing rather than incidental.
BOOST_AUTO_TEST_CASE(Pb6b_Sweep_ResidueRetire)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const int depth = DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN;
    const int tip   = 5000;

    const uint256 deep = QHk(0xC1);   // SUPERSEDED, buried past depth -> retire
    const uint256 near = QHk(0xC2);   // SUPERSEDED, within depth      -> survive
    const uint256 live = QHk(0xC3);   // ACTIVE                        -> untouched
    {
        CPTXQuorumRecord r;
        r.quorum_hash = deep; r.mined_height = 100;
        r.state = static_cast<uint8_t>(PTXQuorumState::SUPERSEDED);
        r.superseded_height = tip - depth;            // exactly at the bound
        evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), deep), r);
        r.quorum_hash = near; r.superseded_height = tip - depth + 1;  // one short
        evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), near), r);
        CPTXQuorumRecord a;
        a.quorum_hash = live; a.mined_height = 100;
        a.state = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
        evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), live), a);
    }
    uint8_t sk[32]; std::memset(sk, 0x7C, 32);
    std::string e1, e2, e3;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(deep, 100, sk, PTXShareRole::CURRENT, e1));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(near, 100, sk, PTXShareRole::CURRENT, e2));
    BOOST_REQUIRE(PTX_BLS_SetSkShare(live, 100, sk, PTXShareRole::CURRENT, e3));

    const size_t retired = store.RetireSupersededResidues(tip);
    BOOST_CHECK_EQUAL(retired, 1u);
    BOOST_CHECK_EQUAL(PTX_BLS_HeldQuorumHashes().count(deep), 0u); // DELETED
    BOOST_CHECK_EQUAL(PTX_BLS_HeldQuorumHashes().count(near), 1u); // survives
    BOOST_CHECK_EQUAL(PTX_BLS_HeldQuorumHashes().count(live), 1u); // untouched
}

// (P-b6b) ★ THE SWEEP-PLACEMENT PIN. The sweep entry point takes a TIP HEIGHT
// and nothing else — no block, no tx — so it CANNOT be gated on block contents.
// Here a residue is retired by a tip advance with NO PTXDKG anywhere in play,
// which is exactly the stranded case a ProcessBlock-sited sweep could never
// serve (that path early-returns on any block carrying no PTXDKG).
// RED: no inversion is expressible in the test — the signature is the proof;
// re-siting the call behind the FindPTXDKGInBlock gate would leave this row
// green while breaking production, which is WHY the constraint is enforced by
// the type rather than by a test (stated plainly rather than claimed as
// coverage).
BOOST_AUTO_TEST_CASE(Pb6b_Sweep_FiresOnPlainTipAdvance)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    const int depth = DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN;
    const int tip   = 7000;
    const uint256 qh = QHk(0xD9);
    {
        CPTXQuorumRecord r;
        r.quorum_hash = qh; r.mined_height = 10;
        r.state = static_cast<uint8_t>(PTXQuorumState::SUPERSEDED);
        r.superseded_height = tip - depth;
        evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), qh), r);
    }
    uint8_t sk[32]; std::memset(sk, 0x8D, 32);
    std::string err;
    BOOST_REQUIRE(PTX_BLS_SetSkShare(qh, 10, sk, PTXShareRole::CURRENT, err));

    // No block. No PTXDKG. Only a height — the whole point.
    CPTXQuorumStore store(*evoDb);
    BOOST_CHECK_EQUAL(store.RetireSupersededResidues(tip), 1u);
    BOOST_CHECK_EQUAL(PTX_BLS_HeldQuorumHashes().count(qh), 0u);
}

// ===========================================================================
// W2.4 W4-b — consensus verification of the roll threshold signature
// (CheckSpecialTx, PTX case, contextual block).  RED discipline: the two
// reject tests FAIL at pre-W4-b HEAD (the forgery passes — the demonstrated
// unverified-quorum_sig hole); the verify block turns them GREEN.
// ===========================================================================

// Swap the global store for one bound to the fixture's in-memory evoDb for
// the duration of a test (CheckSpecialTx reads the global).
struct W4bStoreGuard {
    std::unique_ptr<CPTXQuorumStore> saved;
    W4bStoreGuard()
    {
        saved = std::move(ptxQuorumStore);
        ptxQuorumStore = std::make_unique<CPTXQuorumStore>(*evoDb);
    }
    ~W4bStoreGuard() { ptxQuorumStore = std::move(saved); }
};

// One real DKG walk -> ACTIVE record seeded under qh + a VALID combined
// threshold signature over msg (the exact signing path the coordinator uses).
static void W4bMakeQuorumAndSig(const uint256& qh, const uint256& msg,
                                std::vector<uint8_t>& sig_out)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    uint8_t group_pk_bytes[48];
    blst_p1_affine_compress(group_pk_bytes, &sessions[0].group_pk);

    const int t = 6;
    std::vector<int> indices;
    std::vector<std::vector<uint8_t>> partial_sigs;
    for (int i = 0; i < t; i++) {
        indices.push_back(sessions[i].members[sessions[i].my_idx].share_index);
        uint8_t sk_bytes[32];
        blst_bendian_from_scalar(sk_bytes, &sessions[i].sk_share_i);
        uint8_t sig_buf[PTX_SIG_BYTES];
        BOOST_REQUIRE(PTX_BLS_PartialSign(sk_bytes, msg, sig_buf));
        partial_sigs.push_back(std::vector<uint8_t>(sig_buf, sig_buf + PTX_SIG_BYTES));
    }
    uint8_t combined[PTX_SIG_BYTES];
    BOOST_REQUIRE(PTX_BLS_Recover(indices, partial_sigs, combined));
    BOOST_REQUIRE(PTX_BLS_Verify(group_pk_bytes, msg, combined));
    sig_out.assign(combined, combined + PTX_SIG_BYTES);

    CPTXQuorumRecord r;
    r.quorum_hash      = qh;
    r.formation_height = 1;
    r.group_pk_bytes.assign(group_pk_bytes, group_pk_bytes + 48);
    r.formed_size      = 11;
    r.completed_size   = 11;
    r.state            = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    r.mined_height     = 1;
    evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), qh), r);
}

// A structurally-valid roll tx carrying (round_seed=msg, quorum_sig=sig,
// quorum_hash=qh) — the sess-tests base shape (non-coinbase vin, one accum
// output at the service fee).
static CMutableTransaction W4bMakeRollTx(const uint256& qh, const uint256& msg,
                                         const std::vector<uint8_t>& sig)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTX;
    mtx.vin.push_back(CTxIn(COutPoint(QHk(0xAA), 0)));
    // BUG-032 2b-iii: the settle carries NO accum fee output (the fee relocated to
    // the commitment). A single non-accum output keeps the tx realistically shaped.
    CScript nonAccum; nonAccum << OP_TRUE;
    mtx.vout.push_back(CTxOut(1 * COIN, nonAccum));

    CProbabilisticTxPayload payload;
    payload.nSeedHeight     = 1;
    payload.count           = 1;
    payload.low             = 1;
    payload.high            = 100;
    payload.results         = {50};
    payload.round_seed      = msg;
    payload.quorum_sig      = sig;
    payload.quorum_hash     = qh;
    payload.quorum_sig_hash = QHk(0xBB);   // non-null satisfies the structural check
    SetTxPayload(mtx, payload);
    return mtx;
}

static std::string W4bRunContextual(const CMutableTransaction& mtx)
{
    static CBlockIndex dummyPrev;   // PTX case never dereferences pindexPrev
    LOCK(cs_main);
    CValidationState state;
    if (CheckSpecialTx(CTransaction(mtx), &dummyPrev, nullptr, state))
        return "";
    return state.GetRejectReason();
}

// (control) A genuine roll — real quorum, real threshold sig — passes the
// contextual check.  Holds at RED and GREEN.
BOOST_AUTO_TEST_CASE(W4b_ValidRollPasses)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xC1), msg = QHk(0x77);
    std::vector<uint8_t> sig;
    W4bMakeQuorumAndSig(qh, msg, sig);
    BOOST_CHECK_EQUAL(W4bRunContextual(W4bMakeRollTx(qh, msg, sig)), "");
}

// (RED discriminator) A FORGED sig — correct-looking bytes, correct
// quorum_hash, wrong signature — must be REJECTED.  At pre-W4-b HEAD this
// passes validation: the demonstrated spoofable-trigger hole.
BOOST_AUTO_TEST_CASE(W4b_ForgedSigRejected)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xC2), msg = QHk(0x77);
    std::vector<uint8_t> sig;
    W4bMakeQuorumAndSig(qh, msg, sig);
    sig[40] ^= 0x01;   // one bit: valid-looking, cryptographically wrong
    BOOST_CHECK_EQUAL(W4bRunContextual(W4bMakeRollTx(qh, msg, sig)),
                      "ptx-bad-quorum-sig");
}

// (RED discriminator) A quorum_hash naming NO quorum — the second forgery
// vector — must be REJECTED.  Also covers the null-hash case by construction.
BOOST_AUTO_TEST_CASE(W4b_UnknownQuorumRejected)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xC3), msg = QHk(0x77);
    std::vector<uint8_t> sig;
    W4bMakeQuorumAndSig(qh, msg, sig);
    BOOST_CHECK_EQUAL(W4bRunContextual(W4bMakeRollTx(QHk(0xDD), msg, sig)),
                      "ptx-unknown-quorum");
    CMutableTransaction nullAttr = W4bMakeRollTx(uint256(), msg, sig);
    BOOST_CHECK_EQUAL(W4bRunContextual(nullAttr), "ptx-unknown-quorum");
}

// A valid roll from a since-SUPERSEDED quorum still verifies: quorum_hash
// names an IMMUTABLE record (rotation mints a new record under a new hash),
// so the pk lookup is height-free — and an in-flight roll mined just after
// its signer rotated must not be rejected (the mempool-latency race).
BOOST_AUTO_TEST_CASE(W4b_SupersededQuorumStillVerifies)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xC4), msg = QHk(0x77);
    std::vector<uint8_t> sig;
    W4bMakeQuorumAndSig(qh, msg, sig);
    BOOST_REQUIRE(ptxQuorumStore->MarkSuperseded(qh, 500));
    BOOST_CHECK_EQUAL(W4bRunContextual(W4bMakeRollTx(qh, msg, sig)), "");
}

// The siting: the structural path (CheckSpecialTxNoContext) stays
// structural-only — the forged tx is caught contextually, not here.
BOOST_AUTO_TEST_CASE(W4b_NoContextStructuralOnly)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xC5), msg = QHk(0x77);
    std::vector<uint8_t> sig;
    W4bMakeQuorumAndSig(qh, msg, sig);
    sig[40] ^= 0x01;
    const CMutableTransaction forged = W4bMakeRollTx(qh, msg, sig);
    LOCK(cs_main);
    CValidationState state;
    BOOST_CHECK(CheckSpecialTxNoContext(CTransaction(forged), state));
}

// ===========================================================================
// W2.4 W4-c — REFORMED state machinery, FULLY DORMANT (producer = W4-f).
// The P-b1 pattern: zero behaviour change; every piece RED-proven by inversion.
// ===========================================================================

// Seed an ACTIVE record at nVersion=2 (an OLD record — also proves the
// writers upgrade v2 records cleanly on rewrite).
static uint256 W4cSeedActive(uint8_t fill)
{
    CPTXQuorumRecord r;
    r.nVersion     = 2;
    r.quorum_hash  = QHk(fill);
    r.state        = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    r.mined_height = 1;
    evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), r.quorum_hash), r);
    return r.quorum_hash;
}

// The as-of 4th arm: STRICT > — a record reformed AT h is NOT active at h
// (byte-same contract as P-b4's superseded arm).  RED: > flipped to >= makes
// ActiveAt(reformed_height) answer true.
BOOST_AUTO_TEST_CASE(W4c_AsOfBoundary)
{
    CPTXQuorumRecord r;
    r.state           = static_cast<uint8_t>(PTXQuorumState::REFORMED);
    r.mined_height    = 1;
    r.reformed_height = 100;
    BOOST_CHECK(PTX_QuorumRecordActiveAt(r, 99));     // before: active
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(r, 100));   // AT the stamp: NOT active (strict)
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(r, 101));   // after: not active
}

// ★ THE ODC-044 PIN — MarkReformed stamps IN THE SAME WRITE as the state
// flip.  RED (the load-bearing inversion): reproduce MarkDisbanded's bug
// (state set, stamp never written) → the arm reads the -1 sentinel → the
// record answers inactive-at-EVERY-height → the active-before-reform check
// fails.  That failing test is ODC-044's bug, caught this time.
BOOST_AUTO_TEST_CASE(W4c_MarkReformedStampsFromBirth)
{
    const uint256 qh = W4cSeedActive(0xD1);
    CPTXQuorumStore store(*evoDb);
    BOOST_REQUIRE(store.MarkReformed(qh, 100));

    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL((int)rec.state, (int)PTXQuorumState::REFORMED);
    BOOST_CHECK_EQUAL(rec.reformed_height, 100);      // THE STAMP
    const int curv2 = CPTXQuorumRecord::CURRENT_VERSION; // local copy (the P-b1 ODR lesson)
    BOOST_CHECK_EQUAL((int)rec.nVersion, curv2);      // upgraded to CURRENT on rewrite (was literal 3)
    BOOST_CHECK(PTX_QuorumRecordActiveAt(rec, 50));   // as-of: active BEFORE reform
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(rec, 100)); // not AT it

    // Refuse-unless-ACTIVE: the double-flip is a clean no-op false.
    BOOST_CHECK(!store.MarkReformed(qh, 200));
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL(rec.reformed_height, 100);      // first stamp untouched
}

// The undo twin: REFORMED->ACTIVE, stamp back to sentinel; idempotent.
// RED: an undo that restores state but not the stamp leaves stale as-of data.
BOOST_AUTO_TEST_CASE(W4c_UndoRestores)
{
    const uint256 qh = W4cSeedActive(0xD2);
    CPTXQuorumStore store(*evoDb);

    // Undo of a never-reformed (ACTIVE) record: refuse, no-op.
    BOOST_CHECK(!store.RestoreReformedOnUndo(qh));

    BOOST_REQUIRE(store.MarkReformed(qh, 100));
    BOOST_REQUIRE(store.RestoreReformedOnUndo(qh));

    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL((int)rec.state, (int)PTXQuorumState::ACTIVE);
    BOOST_CHECK_EQUAL(rec.reformed_height, -1);       // sentinel restored
    BOOST_CHECK(PTX_QuorumRecordActiveAt(rec, 500));  // fully active again

    // Idempotent: a second undo is a clean no-op false.
    BOOST_CHECK(!store.RestoreReformedOnUndo(qh));
}

// The additive-versioning pin: a v2 record round-trips BYTE-IDENTICAL (the
// new field is not written for v2 — old records unchanged on disk), and
// deserializes with the -1 sentinel; a v3 record carries the field.
// RED: serializing reformed_height unconditionally changes v2 bytes.
BOOST_AUTO_TEST_CASE(W4c_AdditiveRecordVersioning)
{
    CPTXQuorumRecord v2;
    v2.nVersion    = 2;
    v2.quorum_hash = QHk(0xD3);
    v2.state       = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    v2.reformed_height = 777;   // must NOT reach the wire at v2

    CDataStream s2(SER_DISK, PROTOCOL_VERSION);
    s2 << v2;
    const std::vector<char> v2bytes(s2.begin(), s2.end());

    CPTXQuorumRecord v2back;
    CDataStream s2r(v2bytes, SER_DISK, PROTOCOL_VERSION);
    s2r >> v2back;
    BOOST_CHECK_EQUAL(v2back.reformed_height, -1);    // sentinel, not 777

    CDataStream s2again(SER_DISK, PROTOCOL_VERSION);
    s2again << v2back;
    BOOST_CHECK(std::vector<char>(s2again.begin(), s2again.end()) == v2bytes);

    CPTXQuorumRecord v3;
    v3.nVersion        = 3;
    v3.quorum_hash     = QHk(0xD4);
    v3.reformed_height = 888;
    CDataStream s3(SER_DISK, PROTOCOL_VERSION);
    s3 << v3;
    const size_t v3len = s3.size();
    CPTXQuorumRecord v3back;
    s3 >> v3back;
    BOOST_CHECK_EQUAL(v3back.reformed_height, 888);   // v3 carries the field
    BOOST_CHECK_EQUAL(v3len, v2bytes.size() + sizeof(int32_t)); // exactly one field wider
}

// ★ DORMANCY + THE DEFERRED-BUG GUARD (the P5 structural idiom).
// Negative limbs: across the production tree, ZERO call-shaped references to
// MarkReformed / RestoreReformedOnUndo (dormant until W4-f), ZERO qualified
// uses of PTXQuorumState::REFORMED outside the store TU (never reached by a
// live path), and ZERO call-shaped references to MarkDisbanded — guarding the
// deferred ODC-044 stamp bug (deliberately NOT fixed in W2.4; it is disband's,
// owed with its producer) from silently gaining a caller.
// Positive limbs (anti-vacuous): the definitions and the enum value exist.
BOOST_AUTO_TEST_CASE(W4c_Dormancy_Structural)
{
    const std::string src = PTX_SRCDIR;
    BOOST_REQUIRE_MESSAGE(!src.empty(),
        "W4c: PTX_SRCDIR not injected — the structural check could not run");

    // Positive limbs.
    const std::string store_cpp = P5_slurp(src + "/src/ptx/ptx_quorum_store.cpp");
    const std::string store_h   = P5_slurp(src + "/src/ptx/ptx_quorum_store.h");
    BOOST_REQUIRE(!store_cpp.empty() && !store_h.empty());
    BOOST_CHECK(P5_count(store_cpp, "CPTXQuorumStore::MarkReformed") == 1);
    BOOST_CHECK(P5_count(store_cpp, "CPTXQuorumStore::RestoreReformedOnUndo") == 1);
    BOOST_CHECK(P5_count(store_cpp, "CPTXQuorumStore::MarkDisbanded") == 1);
    BOOST_CHECK(P5_count(store_h, "REFORMED = 4") == 1);

    // Negative limbs: scan the first-party production dirs where a caller
    // could live.  (Excludes tests; third-party subtrees have no PTX symbols.)
    const std::vector<std::string> dirs = {
        "/src/evo", "/src/rpc", "/src/consensus", "/src/primitives",
        "/src/wallet", "/src/ptx"
    };
    std::string prod;
    for (const std::string& d : dirs) {
        const fs::path p = fs::path(src + d);
        if (!fs::is_directory(p)) continue;
        for (fs::recursive_directory_iterator it(p), end; it != end; ++it) {
            if (!fs::is_regular_file(it->path())) continue;
            const std::string sp = it->path().string();
            if (sp.find("/ptx/test/") != std::string::npos) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".cpp" && ext != ".h") continue;
            prod += P5_slurp(sp);
        }
    }
    // Also the top-level production TUs a producer would plausibly touch.
    for (const char* f : {"/src/validation.cpp", "/src/init.cpp"})
        prod += P5_slurp(src + f);
    BOOST_REQUIRE(!prod.empty());

    for (const std::string& fn :
         {"MarkReformed", "RestoreReformedOnUndo", "MarkDisbanded"}) {
        BOOST_CHECK_MESSAGE(P5_count(prod, "->" + fn + "(") == 0 &&
                            P5_count(prod, "." + fn + "(") == 0,
                            "production call-shaped reference to " << fn);
    }
    // REFORMED unreached: qualified uses only inside the store TU itself.
    const size_t total   = P5_count(prod, "PTXQuorumState::REFORMED");
    const size_t in_store = P5_count(store_cpp, "PTXQuorumState::REFORMED") +
                            P5_count(store_h, "PTXQuorumState::REFORMED");
    BOOST_CHECK_MESSAGE(total == in_store,
        "PTXQuorumState::REFORMED referenced outside the store TU: " <<
        total << " vs " << in_store);
}

// ===========================================================================
// W2.4 W4-d — the three terminal-eligibility predicates (KDD-074/075/076).
// Pure, stateless, DORMANT (W4-e composes them).  Each RED-proven by
// inversion: idle window boundary + attribution-soundness; resolver-reject
// classification; grace-M boundary + heal-reset.
// ===========================================================================

// A pprev-linked fake chain 0..top (GetAncestor degenerates to a pprev walk
// with pskip null — the Pb6b anchor idiom, extended to a chain).
static std::vector<CBlockIndex> W4dChain(int top)
{
    std::vector<CBlockIndex> chain(top + 1);
    for (int h = 0; h <= top; ++h) {
        chain[h].nHeight = h;
        chain[h].pprev   = (h > 0) ? &chain[h - 1] : nullptr;
    }
    return chain;
}

// A block carrying one roll attributed to qh (sig content irrelevant here —
// the scan reads attributions the W4-b verify already authenticated at
// connect; it does not re-verify).
static CBlock W4dRollBlock(const uint256& qh)
{
    CBlock b;
    b.vtx.push_back(MakeTransactionRef(
        W4bMakeRollTx(qh, QHk(0x77), std::vector<uint8_t>(96, 0x01))));
    return b;
}

// Idle derive-at-eval: the window is (tip - n, tip] EXACTLY, the match is
// attribution-scoped, and missing data answers NOT idle.
// RED: window off-by-one flips the at-boundary case; an any-roll (rather than
// attributed-roll) match flips the other-quorum case.
BOOST_AUTO_TEST_CASE(W4d_IdleWindow)
{
    const uint256 qh = QHk(0xE1), other = QHk(0xE2);
    std::vector<CBlockIndex> chain = W4dChain(200);
    const CBlockIndex* tip = &chain[200];
    const int N = 50;   // window (150, 200]

    std::map<int, CBlock> blocks;   // heights carrying an attributed roll
    auto reader = [&blocks](const CBlockIndex* p, CBlock& out) {
        auto it = blocks.find(p->nHeight);
        out = (it != blocks.end()) ? it->second : CBlock();
        return true;
    };

    // (a) no rolls anywhere: idle.
    BOOST_CHECK(PTX_Formation_QuorumIdleAt(qh, tip, N, reader));

    // (b) roll at EXACTLY tip-N (150): OUTSIDE the window — still idle.
    blocks = {{150, W4dRollBlock(qh)}};
    BOOST_CHECK(PTX_Formation_QuorumIdleAt(qh, tip, N, reader));

    // (c) roll at tip-N+1 (151): first block INSIDE — not idle.
    blocks = {{151, W4dRollBlock(qh)}};
    BOOST_CHECK(!PTX_Formation_QuorumIdleAt(qh, tip, N, reader));

    // (d) roll at the tip itself: not idle.
    blocks = {{200, W4dRollBlock(qh)}};
    BOOST_CHECK(!PTX_Formation_QuorumIdleAt(qh, tip, N, reader));

    // (e) ATTRIBUTION-SCOPED: another quorum's roll inside the window does
    // NOT count as X's activity.
    blocks = {{180, W4dRollBlock(other)}};
    BOOST_CHECK(PTX_Formation_QuorumIdleAt(qh, tip, N, reader));

    // (f) FAIL-SAFE: an unreadable block answers NOT idle.
    blocks.clear();
    auto badReader = [](const CBlockIndex* p, CBlock& out) {
        return p->nHeight != 170;   // 170 unreadable
    };
    BOOST_CHECK(!PTX_Formation_QuorumIdleAt(qh, tip, N, badReader));

    // (g) degenerate params: never idle.
    BOOST_CHECK(!PTX_Formation_QuorumIdleAt(qh, nullptr, N, reader));
    BOOST_CHECK(!PTX_Formation_QuorumIdleAt(qh, tip, 0, reader));
}

// Rotation-impossible = the P-b3a resolver refuses (one implementation, never
// reimplemented).  RED: inverting the wrapper flips both classifications.
BOOST_AUTO_TEST_CASE(W4d_RotationImpossible)
{
    Pb3aWorld w = Pb3aMakeWorld();
    std::string why;

    // All members present: possible.
    BOOST_CHECK(!PTX_Formation_RotationImpossible(w.rec, w.list, w.list, why));

    // A member absent from the rotation-anchor list: impossible, with the
    // resolver's own reason (the Pb3a missing-member idiom).
    CDeterministicGMList listMissing;
    for (const auto& d : w.dgms) {
        if (d->proTxHash == w.rec.members[0].proTxHash) continue;
        listMissing.AddGM(d);
    }
    BOOST_CHECK(PTX_Formation_RotationImpossible(w.rec, listMissing, w.list, why));
    BOOST_CHECK(why.find("unresolvable") != std::string::npos);
}

// Grace-M: due-AND-impossible at each of the last M boundaries; a healed or
// not-yet-due boundary breaks the run (the stateless grace reset).
// RED: M off-by-one passes at M-1; heal-reset removal passes the healed case.
BOOST_AUTO_TEST_CASE(W4d_GraceElapsed)
{
    const int INTERVAL = 80;
    std::vector<CBlockIndex> chain = W4dChain(400);
    const CBlockIndex* anchor = &chain[400];   // boundaries at 400, 320 for M=2

    CPTXQuorumRecord rec;
    rec.formation_height = 80;                 // due at both boundaries

    std::set<int> impossibleAt;
    auto impossible = [&impossibleAt](const CBlockIndex* pb) {
        return impossibleAt.count(pb->nHeight) > 0;
    };

    // (a) impossible at both of the last two boundaries: grace elapsed.
    impossibleAt = {400, 320};
    BOOST_CHECK(PTX_Formation_ForcedReformGraceElapsed(rec, anchor, INTERVAL, INTERVAL, 2, impossible));

    // (b) HEALED at the older boundary (possible at 320): grace NOT elapsed —
    // the pathological ProUpReg self-heal resets the run by construction.
    impossibleAt = {400};
    BOOST_CHECK(!PTX_Formation_ForcedReformGraceElapsed(rec, anchor, INTERVAL, INTERVAL, 2, impossible));

    // (c) M=1: the current boundary alone decides.
    BOOST_CHECK(PTX_Formation_ForcedReformGraceElapsed(rec, anchor, INTERVAL, INTERVAL, 1, impossible));

    // (d) YOUNG quorum: not yet due at the older boundary (formed 300;
    // 320-300 < 80) — grace cannot elapse even though both marked impossible.
    impossibleAt = {400, 320};
    CPTXQuorumRecord young;
    young.formation_height = 300;
    BOOST_CHECK(!PTX_Formation_ForcedReformGraceElapsed(young, anchor, INTERVAL, INTERVAL, 2, impossible));

    // (e) degenerate: M=0 / null anchor / boundary below genesis.
    BOOST_CHECK(!PTX_Formation_ForcedReformGraceElapsed(rec, anchor, INTERVAL, INTERVAL, 0, impossible));
    BOOST_CHECK(!PTX_Formation_ForcedReformGraceElapsed(rec, nullptr, INTERVAL, INTERVAL, 2, impossible));
    impossibleAt = {40};
    CPTXQuorumRecord early;
    early.formation_height = 0;
    BOOST_CHECK(!PTX_Formation_ForcedReformGraceElapsed(early, &chain[40], INTERVAL, INTERVAL, 2, impossible));
}

// Dormancy (the P5 idiom): the three predicates + the block helper have ZERO
// production references outside their defining TU — W4-e is their first
// caller.  Positive limbs anti-vacuous.
BOOST_AUTO_TEST_CASE(W4d_Dormancy_Structural)
{
    const std::string src = PTX_SRCDIR;
    BOOST_REQUIRE_MESSAGE(!src.empty(),
        "W4d: PTX_SRCDIR not injected — the structural check could not run");

    const std::string form_cpp = P5_slurp(src + "/src/ptx/ptx_formation.cpp");
    const std::string form_h   = P5_slurp(src + "/src/ptx/ptx_formation.h");
    BOOST_REQUIRE(!form_cpp.empty() && !form_h.empty());

    const std::vector<std::string> fns = {
        "PTX_Formation_BlockHasAttributedRoll",
        "PTX_Formation_QuorumIdleAt",
        "PTX_Formation_RotationImpossible",
        "PTX_Formation_ForcedReformGraceElapsed",
    };
    for (const std::string& fn : fns)   // positive limb: defined in the TU
        BOOST_CHECK_MESSAGE(P5_count(form_cpp, "bool " + fn + "(") == 1, fn);

    const std::vector<std::string> dirs = {
        "/src/evo", "/src/rpc", "/src/consensus", "/src/primitives",
        "/src/wallet", "/src/ptx"
    };
    std::string prod;
    for (const std::string& d : dirs) {
        const fs::path p = fs::path(src + d);
        if (!fs::is_directory(p)) continue;
        for (fs::recursive_directory_iterator it(p), end; it != end; ++it) {
            if (!fs::is_regular_file(it->path())) continue;
            const std::string sp = it->path().string();
            if (sp.find("/ptx/test/") != std::string::npos) continue;
            if (sp.find("ptx_formation.") != std::string::npos) continue; // the defining TU
            const std::string ext = it->path().extension().string();
            if (ext != ".cpp" && ext != ".h") continue;
            prod += P5_slurp(sp);
        }
    }
    for (const char* f : {"/src/validation.cpp", "/src/init.cpp"})
        prod += P5_slurp(src + f);
    BOOST_REQUIRE(!prod.empty());

    // Negative limbs, W4-f-adjusted: RotationImpossible's dormancy EXPIRED at
    // W4-f — the store's producer lambda is its one sanctioned consumer
    // (pinned to exactly one ref, in ptx_quorum_store.cpp).  The other three
    // stay at zero production refs (their consumers live inside the
    // formation TU, which this scan excludes by design).
    const std::string store_cpp2 = P5_slurp(src + "/src/ptx/ptx_quorum_store.cpp");
    BOOST_REQUIRE(!store_cpp2.empty());
    for (const std::string& fn : fns) {
        const size_t in_prod  = P5_count(prod, fn);
        const size_t in_store = P5_count(store_cpp2, fn);
        if (fn == "PTX_Formation_RotationImpossible") {
            BOOST_CHECK_MESSAGE(in_store == 1 && in_prod == in_store,
                "RotationImpossible: expected exactly the one sanctioned "
                "store-TU consumer (W4-f producer), got " << in_prod);
        } else {
            // ★ CALL-SHAPED, not bare-name (the W4c idiom).  Bare-name matching
            // also flags COMMENTS that merely NAME a function: KDD-079's
            // params.h comment cites ForcedReformGraceElapsed to explain the
            // fourth conflation, which is documentation, not a caller.  A
            // dormancy pin must catch CALLS.
            const size_t calls = P5_count(prod, "->" + fn + "(") +
                                 P5_count(prod, " " + fn + "(");
            BOOST_CHECK_MESSAGE(calls == 0,
                "production CALL to " << fn << " outside the formation TU");
        }
    }
}

// ===========================================================================
// W2.4 W4-e — the KDD-075 yield + the KDD-074 rate limiter, PARAM-GATED OFF.
// The two ★ load-bearing pins: keying-on-ELIGIBILITY (Hazard A stays closed
// through the limiter) and the gate's 0 defaults (deploy safety on the
// all-idle bf fleet).
// ===========================================================================

// ★ THE GATE PIN: every knob defaults 0 == DISABLED, and NO chainparams
// enables any of them — with the gate off, an idle-in-fact due quorum is NOT
// eligible and RotationDueAt behaves byte-identically to P-b6b (still due).
// RED: flipping any in-struct default to non-zero fails this test — the
// deploy-safety pin (bf is all-idle; a default-on gate suppresses every
// rotation fleet-wide).
BOOST_AUTO_TEST_CASE(W4e_GateDefaultOff_Dormancy)
{
    const Consensus::PTXFormationParams& params = Params().GetConsensus().ptxFormation;
    BOOST_CHECK_EQUAL(params.nRetireWindow, 0);
    BOOST_CHECK_EQUAL(params.nReformGrace, 0);
    BOOST_CHECK_EQUAL(params.nReformRateWindow, 0);

    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const int N = params.nBoundaryInterval;   // KDD-079: boundaries

    const uint256 qh = QHk(0xF1);
    Pb6bSeedActive(qh, /*formation*/ 1000, /*mined*/ 1005);
    std::vector<CBlockIndex> chain = W4dChain(1000 + N);
    const CBlockIndex* due = &chain[1000 + N];

    // Idle by every measure (empty blocks), impossible by every measure —
    // and STILL not eligible, because the gate is off.
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto alwaysImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return true; };
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK(!PTX_Formation_TerminalEligible(rec, due, params, emptyReader, alwaysImpossible));

    // And the due-decision is byte-identical to pre-W4-e: the quorum rotates.
    const PTXRotationDecision d =
        PTX_Formation_RotationDueAt(due, store, params, emptyReader, alwaysImpossible);
    BOOST_CHECK(d.due);
    BOOST_CHECK(d.predecessor_quorum_hash == qh);
}

// ★ THE KEYING PIN (KDD-075): with the gate ON, ALL eligible quorums yield —
// including the one the rate limiter would defer this window.  RED
// (key-on-fired inversion): only the limiter-selected quorum yields, the
// deferred one falls through to rotating — minting a successor with a reset
// idleness view and REOPENING HAZARD A through the limiter.  This test must
// fail under that inversion.
BOOST_AUTO_TEST_CASE(W4e_YieldKeyedOnEligibility)
{
    Consensus::PTXFormationParams params = Params().GetConsensus().ptxFormation;
    params.nRetireWindow = 50;          // idle arm ON
    params.nReformRateWindow = 100;     // limiter would pick at most ONE

    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const int N = params.nBoundaryInterval;   // KDD-079: boundaries

    // TWO due quorums, both idle (no attributed rolls anywhere).
    const uint256 qhA = QHk(0x0A), qhB = QHk(0x0B);
    Pb6bSeedActive(qhA, 1000, 1005);
    Pb6bSeedActive(qhB, 1000, 1006);
    std::vector<CBlockIndex> chain = W4dChain(1000 + N);
    const CBlockIndex* due = &chain[1000 + N];
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    // BOTH yield: no rotation is due at this anchor, even though the limiter
    // could only ever transition one of them this window.  The deferred one
    // stays ACTIVE-queued — it must NOT fall through to rotating.
    const PTXRotationDecision d =
        PTX_Formation_RotationDueAt(due, store, params, emptyReader, neverImpossible);
    BOOST_CHECK_MESSAGE(!d.due,
        "an eligible-but-rate-deferred quorum fell through to rotating - Hazard A reopened");
}

// The composition: idle OR (impossible AND grace), each arm under its own
// knob.  RED: composition inversions flip the per-arm rows.
BOOST_AUTO_TEST_CASE(W4e_YieldComposition)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    Consensus::PTXFormationParams params = Params().GetConsensus().ptxFormation;
    const int N = params.nBoundaryInterval;   // KDD-079: boundaries

    const uint256 qh = QHk(0xF2);
    Pb6bSeedActive(qh, 1000, 1005);
    std::vector<CBlockIndex> chain = W4dChain(1000 + N);
    const CBlockIndex* due = &chain[1000 + N];
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));

    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };
    auto alwaysImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return true; };

    // Idle arm alone (window ON, grace OFF): idle -> eligible.
    params.nRetireWindow = 50; params.nReformGrace = 0;
    BOOST_CHECK(PTX_Formation_TerminalEligible(rec, due, params, emptyReader, neverImpossible));

    // Busy (an attributed roll at the anchor): NOT eligible via the idle arm.
    std::map<int, CBlock> blocks = {{1000 + N, W4dRollBlock(qh)}};
    auto busyReader = [&blocks](const CBlockIndex* p, CBlock& out) {
        auto it = blocks.find(p->nHeight);
        out = (it != blocks.end()) ? it->second : CBlock();
        return true;
    };
    BOOST_CHECK(!PTX_Formation_TerminalEligible(rec, due, params, busyReader, neverImpossible));

    // Forced arm alone (window OFF, grace ON): impossible-and-due -> eligible
    // even while BUSY (a dead-but-demanded quorum reforms on the impossible
    // route, not the idle route).
    params.nRetireWindow = 0; params.nReformGrace = 1;
    BOOST_CHECK(PTX_Formation_TerminalEligible(rec, due, params, busyReader, alwaysImpossible));

    // Neither arm fires: possible and busy -> not eligible.
    BOOST_CHECK(!PTX_Formation_TerminalEligible(rec, due, params, busyReader, neverImpossible));
}

// The limiter: one per window, least-recently-active first, lowest-hash ties,
// disabled selects nothing.  RED: most-recent selection / dropped rate check.
BOOST_AUTO_TEST_CASE(W4e_LimiterSelectsOneLRA)
{
    const uint256 hiHash = QHk(0xEE), loHash = QHk(0x11);
    uint256 sel;

    // LRA wins: B (last active 50) over A (last active 100).
    std::vector<std::pair<uint256, int>> cands = {{hiHash, 100}, {loHash, 50}};
    BOOST_REQUIRE(PTX_Formation_SelectReformCandidate(cands, 1000, 200, sel));
    BOOST_CHECK(sel == loHash);
    // ...independent of ordering.
    std::vector<std::pair<uint256, int>> rev = {{loHash, 50}, {hiHash, 100}};
    BOOST_REQUIRE(PTX_Formation_SelectReformCandidate(rev, 1000, 200, sel));
    BOOST_CHECK(sel == loHash);

    // Tie on activity: lowest hash wins (the P-b6b shape).
    std::vector<std::pair<uint256, int>> tie = {{hiHash, 70}, {loHash, 70}};
    BOOST_REQUIRE(PTX_Formation_SelectReformCandidate(tie, 1000, 200, sel));
    BOOST_CHECK(sel == loHash);

    // ★ BUG-036: SelectReformCandidate is now a PURE PICKER — pacing lives at
    // the caller (stateless stride post-activation / legacy pre-activation in
    // MaybeReformAtBoundary). The picker itself selects whenever enabled:
    BOOST_CHECK(PTX_Formation_SelectReformCandidate(cands, 1030, 200, sel));   // any height: picker picks
    BOOST_CHECK(PTX_Formation_SelectReformCandidate(cands, 1000, 200, sel));

    // Gate posture: disabled limiter or no candidates selects nothing.
    BOOST_CHECK(!PTX_Formation_SelectReformCandidate(cands, 1000, 0, sel));
    BOOST_CHECK(!PTX_Formation_SelectReformCandidate({}, 1000, 200, sel));
}

// ===========================================================================
// W2.4 W4-f — the block-driven reform producer + the un-stub.  The mechanism
// FIRES here: eligibility (W4-d) -> limiter (W4-e) -> MarkReformed (W4-c),
// connect-side, boundary-cadence; the stamp is the undo journal.
// ===========================================================================

static Consensus::PTXFormationParams W4fParams()
{
    Consensus::PTXFormationParams p{};
    p.name = "w4f";
    p.nBoundaryInterval = 80;
    p.nRotationInterval = 80;
    p.nCeremonyBudget   = 80;
    p.nRetireWindow      = 50;
    p.nReformGrace       = 0;
    p.nReformRateWindow  = 40;
    // ★ BUG-036: stateless from 100 — the drills fire at 160 (stateless,
    // stride=ceil(40/80)*80=80) while the heal stays DISARMED for prevB=80
    // (< activation), so sparse drill invocation isn't retro-judged.
    p.nReformStatelessHeight = 100;
    return p;
}

// The producer fires: an idle-eligible ACTIVE quorum at a boundary is
// MarkReformed'd, stamped with the boundary height.  Cadence and both gate
// postures pinned.  RED: limiter bypassed -> the rate-0 row fires anyway;
// boundary gate dropped -> the non-boundary row fires.
BOOST_AUTO_TEST_CASE(W4f_ProducerFiresAtBoundary)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = W4fParams();

    const uint256 qh = QHk(0xFA);
    Pb6bSeedActive(qh, /*formation*/ 1, /*mined*/ 5);
    std::vector<CBlockIndex> chain = W4dChain(160);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    // Non-boundary: nothing fires (159 % 80 != 0).
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[159], params, emptyReader, neverImpossible), 0u);

    // Limiter disabled (rate window 0): nothing fires even though eligible.
    Consensus::PTXFormationParams noRate = params;
    noRate.nReformRateWindow = 0;
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[160], noRate, emptyReader, neverImpossible), 0u);

    // Eligibility disabled (both arms 0): nothing fires.
    Consensus::PTXFormationParams noElig = params;
    noElig.nRetireWindow = 0; noElig.nReformGrace = 0;
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[160], noElig, emptyReader, neverImpossible), 0u);

    // The boundary, gate live: THE REFORM FIRES.
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[160], params, emptyReader, neverImpossible), 1u);
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL((int)rec.state, (int)PTXQuorumState::REFORMED);
    BOOST_CHECK_EQUAL(rec.reformed_height, 160);
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(rec, 500));   // gone from the as-of view
}

// LRA on real records: among idle-eligibles (no in-window activity by
// definition) the LRA collapses to record antiquity — the OLDER mined record
// reforms first.  RED: limiter-bypass picks iteration order instead.

// ★ BUG-036 RED — derive-don't-store, both levels. MECHANISM: a reform stamp
// clobbered by the exact VerifyDB shape (RestoreReformedAtHeight, the walk's
// entry point) SELF-HEALS at the next stateless boundary — re-stamped at its
// TRUE height, not re-selected at the wrong one. PROPERTY (what BUG-036
// violated): the record's canonicity verdicts re-converge to the uncorrupted
// expectation at every height. Anti-vacuity: the wrong state is asserted
// live before the heal (the node genuinely held the wrong value).
BOOST_AUTO_TEST_CASE(Bug036_ClobberedStampSelfHeals)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = W4fParams();  // stride 80, stateless>=100
    const uint256 qh = QHk(0x02);                  // lowest hash + oldest mined: LRA-dominant
    Pb6bSeedActive(qh, /*formation*/ 1, /*mined*/ 2);
    std::vector<CBlockIndex> chain = W4dChain(240);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    // Fire at 160 (stateless regime).
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[160], params, emptyReader, neverImpossible), 1u);
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL(rec.reformed_height, 160);

    // THE CLOBBER — the VerifyDB walk's own entry point.
    BOOST_CHECK_EQUAL(store.RestoreReformedAtHeight(160), 1u);
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL((int)rec.state, (int)PTXQuorumState::ACTIVE);   // genuinely wrong now
    BOOST_CHECK(PTX_QuorumRecordActiveAt(rec, 200));                  // wrong verdict, live

    // NEXT boundary: heal runs BEFORE selection → re-stamped at the TRUE 160
    // (the stateful design would have re-selected it at 240 = a divergent stamp).
    store.MaybeReformAtBoundary(&chain[240], params, emptyReader, neverImpossible);
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL((int)rec.state, (int)PTXQuorumState::REFORMED);
    BOOST_CHECK_EQUAL(rec.reformed_height, 160);

    // ★ THE PROPERTY: canonicity verdicts match the uncorrupted expectation.
    BOOST_CHECK( PTX_QuorumRecordActiveAt(rec, 159));
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(rec, 160));
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(rec, 200));
}

// ★ BUG-036 PROPERTY, peer form — the level that matters: a node whose stamp
// was clobbered must re-converge with an UNCORRUPTED PEER's canonicity
// verdicts within one boundary. Two independent stores over independent DBs
// model the two nodes; equality after the heal is simultaneously the
// DETERMINISM proof (the heal recomputed from the same chain inputs and
// arrived at the peer's answer, not merely at *an* answer).
BOOST_AUTO_TEST_CASE(Bug036_CorruptedNodeConvergesWithCleanPeer)
{
    const Consensus::PTXFormationParams params = W4fParams();  // stride 80, stateless>=100
    const uint256 qh = QHk(0x02);
    std::vector<CBlockIndex> chain = W4dChain(240);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    // Two nodes: same chain data, independent state.
    CEvoDB dbA(1 << 20, true, true), dbB(1 << 20, true, true);
    CPTXQuorumStore nodeA(dbA), nodeB(dbB);
    auto seed = [&qh](CEvoDB& db) {
        CPTXQuorumRecord r;
        r.quorum_hash      = qh;
        r.formation_height = 1;
        r.mined_height     = 2;
        r.state            = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
        db.Write(std::make_pair(PTX_QuorumRecordDBPrefix(), qh), r);
        db.Write(std::make_pair(std::string("pq_h"),
                 htobe32(std::numeric_limits<uint32_t>::max() - 2)), qh);
    };
    seed(dbA);
    seed(dbB);

    // Both fire at the stateless boundary 160.
    BOOST_CHECK_EQUAL(nodeA.MaybeReformAtBoundary(&chain[160], params, emptyReader, neverImpossible), 1u);
    BOOST_CHECK_EQUAL(nodeB.MaybeReformAtBoundary(&chain[160], params, emptyReader, neverImpossible), 1u);

    // Clobber node A only (the VerifyDB walk's entry point).
    BOOST_CHECK_EQUAL(nodeA.RestoreReformedAtHeight(160), 1u);

    // ANTI-VACUITY: the peers genuinely disagree before the heal — A holds
    // the wrong value live, B holds the truth.
    CPTXQuorumRecord ra, rb;
    BOOST_REQUIRE(nodeA.GetQuorumRecord(qh, ra));
    BOOST_REQUIRE(nodeB.GetQuorumRecord(qh, rb));
    BOOST_CHECK( PTX_QuorumRecordActiveAt(ra, 200));
    BOOST_CHECK(!PTX_QuorumRecordActiveAt(rb, 200));

    // Both connect through the next boundary.
    nodeA.MaybeReformAtBoundary(&chain[240], params, emptyReader, neverImpossible);
    nodeB.MaybeReformAtBoundary(&chain[240], params, emptyReader, neverImpossible);

    // THE PROPERTY: full record equality and identical canonicity verdicts at
    // EVERY height — the corrupted node reached the clean peer's answer.
    BOOST_REQUIRE(nodeA.GetQuorumRecord(qh, ra));
    BOOST_REQUIRE(nodeB.GetQuorumRecord(qh, rb));
    BOOST_CHECK_EQUAL((int)ra.state, (int)rb.state);
    BOOST_CHECK_EQUAL(ra.reformed_height, rb.reformed_height);
    for (int h = 0; h <= 240; ++h) {
        BOOST_CHECK_MESSAGE(
            PTX_QuorumRecordActiveAt(ra, h) == PTX_QuorumRecordActiveAt(rb, h),
            "canonicity verdicts diverge at height " << h);
    }
}

BOOST_AUTO_TEST_CASE(W4f_LRAPicksOldestRecord)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = W4fParams();

    const uint256 qhA = QHk(0xA5), qhB = QHk(0xB5);
    Pb6bSeedActive(qhA, 1, /*mined*/ 5);
    Pb6bSeedActive(qhB, 1, /*mined*/ 3);   // OLDER — least recently active
    std::vector<CBlockIndex> chain = W4dChain(160);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[160], params, emptyReader, neverImpossible), 1u);
    CPTXQuorumRecord a, b;
    BOOST_REQUIRE(store.GetQuorumRecord(qhA, a));
    BOOST_REQUIRE(store.GetQuorumRecord(qhB, b));
    BOOST_CHECK_EQUAL((int)b.state, (int)PTXQuorumState::REFORMED);  // B reformed
    BOOST_CHECK_EQUAL((int)a.state, (int)PTXQuorumState::ACTIVE);    // A queued
}

// The undo: the stamp is the journal — only the matching height reverts,
// idempotent.  RED: height-keying dropped -> the wrong-height row reverts.
BOOST_AUTO_TEST_CASE(W4f_UndoRevertsAtHeight)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = W4fParams();

    const uint256 qh = QHk(0xFC);
    Pb6bSeedActive(qh, 1, 5);
    std::vector<CBlockIndex> chain = W4dChain(160);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };
    BOOST_REQUIRE_EQUAL(store.MaybeReformAtBoundary(&chain[160], params, emptyReader, neverImpossible), 1u);

    BOOST_CHECK_EQUAL(store.RestoreReformedAtHeight(159), 0u);   // wrong height: no-op
    BOOST_CHECK_EQUAL(store.RestoreReformedAtHeight(160), 1u);   // THE revert
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL((int)rec.state, (int)PTXQuorumState::ACTIVE);
    BOOST_CHECK_EQUAL(rec.reformed_height, -1);
    BOOST_CHECK_EQUAL(store.RestoreReformedAtHeight(160), 0u);   // idempotent
}

// ★ The un-stub is drill-chain-only, and the wiring exists — both pinned
// structurally (the P5 idiom).  Main/test/ptxtest keep the 2-initializer
// (gate {0}) form; regtest/ptxbea carry the 5-initializer live form;
// ProcessBlock calls the producer, UndoBlock the revert.
BOOST_AUTO_TEST_CASE(W4f_UnstubAndWiring_Structural)
{
    const std::string src = PTX_SRCDIR;
    BOOST_REQUIRE_MESSAGE(!src.empty(), "W4f: PTX_SRCDIR not injected");

    const std::string cp = P5_slurp(src + "/src/chainparams.cpp");
    BOOST_REQUIRE(!cp.empty());
    // ★ PIN THE PROPERTY, NOT THE LITERAL.  This row previously pinned each
    // network's ENTIRE initializer, so it broke on every schema growth — it
    // fired on W2.5a P1 (the three-way split) and again on P2 (adding
    // nSupportedQuorums), both times for a change that did not touch what it
    // guards.  What it actually guards is the REFORM GATE: live on the drill
    // chains, dormant everywhere else.  Assert exactly that, and the row
    // survives future params while still catching a real un-stub leak.
    const std::string gate = "200, 1, 40";   // nRetireWindow, nReformGrace, nReformRateWindow
    for (const char* net : {"main", "test", "ptxtest"}) {
        const std::string line = P5_line_containing(cp, std::string("ptxFormation = {\"") + net + "\"");
        BOOST_CHECK_MESSAGE(!line.empty(), std::string("no ptxFormation line for ") + net);
        BOOST_CHECK_MESSAGE(line.find(gate) == std::string::npos,
            std::string(net) + " must NOT carry the reform gate (mainnet/testnet stay DORMANT): " + line);
    }
    for (const char* net : {"regtest", "ptxbea"}) {
        const std::string line = P5_line_containing(cp, std::string("ptxFormation = {\"") + net + "\"");
        BOOST_CHECK_MESSAGE(line.find(gate) != std::string::npos,
            std::string(net) + " must carry the reform gate (drill chains LIVE): " + line);
    }

    const std::string st = P5_slurp(src + "/src/ptx/ptx_quorum_store.cpp");
    BOOST_REQUIRE(!st.empty());
    BOOST_CHECK(P5_count(st, "MaybeReformAtBoundary(pindex, Params().GetConsensus().ptxFormation") == 1);
    BOOST_CHECK(P5_count(st, "RestoreReformedAtHeight(pindex->nHeight);") == 1);
}

// ★ W4-f AMENDMENT — THE AGE ANCHOR (the pre-drill finding): "N blocks of
// silence" requires N blocks of the QUORUM'S opportunity to be silent.  A
// quorum idle-IN-FACT but YOUNGER than the window is NOT eligible — youth is
// not idleness.  Without the anchor, a young quorum on a quiet chain reforms
// at its first boundary and the reformed successor churns forever.
// RED: drop the age clause -> both young rows flip eligible/reformed.
BOOST_AUTO_TEST_CASE(W4fA_AgeAnchor)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = W4fParams();  // window 50

    std::vector<CBlockIndex> chain = W4dChain(160);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    // PREDICATE level: idle-in-fact but young (mined 130; 130+50 > 160).
    const uint256 qhYoung = QHk(0xD7);
    Pb6bSeedActive(qhYoung, /*formation*/ 125, /*mined*/ 130);
    CPTXQuorumRecord young;
    BOOST_REQUIRE(store.GetQuorumRecord(qhYoung, young));
    BOOST_CHECK(!PTX_Formation_TerminalEligible(young, &chain[160], params,
                                                emptyReader, neverImpossible));

    // The EXACT boundary: mined + window == anchor -> the quorum has lived
    // the whole window -> eligible (110 + 50 == 160).
    const uint256 qhEdge = QHk(0xD8);
    Pb6bSeedActive(qhEdge, 105, /*mined*/ 110);
    CPTXQuorumRecord edge;
    BOOST_REQUIRE(store.GetQuorumRecord(qhEdge, edge));
    BOOST_CHECK(PTX_Formation_TerminalEligible(edge, &chain[160], params,
                                               emptyReader, neverImpossible));

    // PRODUCER level (the churn-loop guard where it matters): with ONLY the
    // young quorum active, the producer reforms NOTHING at the boundary.
    // (Seed a fresh store world: reuse qhYoung alone by marking edge reformed
    // out of the way is intrusive - instead assert the producer selects the
    // EDGE quorum, never the young one.)
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[160], params,
                                                  emptyReader, neverImpossible), 1u);
    CPTXQuorumRecord y2, e2;
    BOOST_REQUIRE(store.GetQuorumRecord(qhYoung, y2));
    BOOST_REQUIRE(store.GetQuorumRecord(qhEdge, e2));
    BOOST_CHECK_EQUAL((int)y2.state, (int)PTXQuorumState::ACTIVE);    // young: untouched
    BOOST_CHECK_EQUAL((int)e2.state, (int)PTXQuorumState::REFORMED);  // edge: reformed
}

// ===========================================================================
// W2.4 LINEAGE CLOCK - idle_since_height (v4), inherited across rotation.
// The fix for age-anchor-reopens-Hazard-A: silence is measured on the SEAT
// (lineage), not the key-generation (record).
// ===========================================================================

// Seed an ACTIVE record with an explicit lineage clock.
static uint256 W4LSeed(uint8_t fill, int mined, int idle_since)
{
    CPTXQuorumRecord r;
    r.quorum_hash       = QHk(fill);
    r.state             = static_cast<uint8_t>(PTXQuorumState::ACTIVE);
    r.mined_height      = mined;
    r.idle_since_height = idle_since;
    evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), r.quorum_hash), r);
    evoDb->Write(std::make_pair(std::string("pq_h"),
                 htobe32(std::numeric_limits<uint32_t>::max() - mined)), r.quorum_hash);
    return r.quorum_hash;
}

// ★ THE INHERITED-CLOCK ROW (load-bearing): a rotation successor of an IDLE
// LINEAGE - young record (own mined 130), OLD inherited clock (idle_since 10)
// - IS eligible at its first boundary: the lineage's silence already exceeds
// the window.  RED (the anchor reverted to mined_height = the don't-inherit
// world): the successor reads as too young -> NOT eligible -> it would rotate
// instead of reforming -> HAZARD A REPRODUCED as this row's failure.
// Fresh-grace + sentinel rows hold under that inversion (idle_since==mined
// and the -1 fallback both degrade to mined) - single-row attribution.
BOOST_AUTO_TEST_CASE(W4L_InheritedClockEligible)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = W4fParams();   // window 50
    std::vector<CBlockIndex> chain = W4dChain(160);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    // Successor-shaped: mined 130 (young record), inherited clock 10 (old seat).
    const uint256 qh = W4LSeed(0xE7, /*mined*/ 130, /*idle_since*/ 10);
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_MESSAGE(
        PTX_Formation_TerminalEligible(rec, &chain[160], params, emptyReader, neverImpossible),
        "successor of an idle lineage NOT eligible at its first boundary - "
        "the clock reset on rotation: Hazard A (the starvation loop) is back");

    // PRODUCER level: the same successor reforms at the boundary.
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[160], params,
                                                  emptyReader, neverImpossible), 1u);
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK_EQUAL((int)rec.state, (int)PTXQuorumState::REFORMED);
}

// Fresh-formation grace INTACT: a fresh record (idle_since == own mined) is
// NOT eligible before it lived the window - the W4-f amendment's churn fix
// survives the lineage clock unchanged.
BOOST_AUTO_TEST_CASE(W4L_FreshGraceIntact)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = W4fParams();
    std::vector<CBlockIndex> chain = W4dChain(160);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    const uint256 qh = W4LSeed(0xE8, /*mined*/ 130, /*idle_since*/ 130);  // fresh-shaped
    CPTXQuorumRecord rec;
    BOOST_REQUIRE(store.GetQuorumRecord(qh, rec));
    BOOST_CHECK(!PTX_Formation_TerminalEligible(rec, &chain[160], params,
                                                emptyReader, neverImpossible));
}

// Sentinel fallback: a pre-v4 record (idle_since -1) behaves exactly as
// before the lineage clock - the anchor reads mined_height.  (The W4fA
// boundary rows re-proven through the fallback path.)
BOOST_AUTO_TEST_CASE(W4L_SentinelFallback)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    const Consensus::PTXFormationParams params = W4fParams();
    std::vector<CBlockIndex> chain = W4dChain(160);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto neverImpossible = [](const CPTXQuorumRecord&, const CBlockIndex*) { return false; };

    const uint256 old_enough = W4LSeed(0xE9, /*mined*/ 110, /*idle_since*/ -1);  // 110+50==160
    const uint256 too_young  = W4LSeed(0xEA, /*mined*/ 130, /*idle_since*/ -1);
    CPTXQuorumRecord a, b;
    BOOST_REQUIRE(store.GetQuorumRecord(old_enough, a));
    BOOST_REQUIRE(store.GetQuorumRecord(too_young, b));
    BOOST_CHECK(PTX_Formation_TerminalEligible(a, &chain[160], params, emptyReader, neverImpossible));
    BOOST_CHECK(!PTX_Formation_TerminalEligible(b, &chain[160], params, emptyReader, neverImpossible));
}

// Additive v4: a v3 stream is byte-identical (field never written), loads
// with the sentinel; v4 carries the field and is exactly one int32 wider.
BOOST_AUTO_TEST_CASE(W4L_AdditiveV4)
{
    CPTXQuorumRecord v3;
    v3.nVersion          = 3;
    v3.quorum_hash       = QHk(0xEB);
    v3.idle_since_height = 777;          // must NOT reach the wire at v3
    CDataStream s3(SER_DISK, PROTOCOL_VERSION);
    s3 << v3;
    const std::vector<char> v3bytes(s3.begin(), s3.end());
    CPTXQuorumRecord v3back;
    CDataStream s3r(v3bytes, SER_DISK, PROTOCOL_VERSION);
    s3r >> v3back;
    BOOST_CHECK_EQUAL(v3back.idle_since_height, -1);       // sentinel, not 777
    CDataStream s3again(SER_DISK, PROTOCOL_VERSION);
    s3again << v3back;
    BOOST_CHECK(std::vector<char>(s3again.begin(), s3again.end()) == v3bytes);

    CPTXQuorumRecord v4;
    v4.nVersion          = 4;
    v4.quorum_hash       = QHk(0xEC);
    v4.idle_since_height = 888;
    CDataStream s4(SER_DISK, PROTOCOL_VERSION);
    s4 << v4;
    const size_t v4len = s4.size();
    CPTXQuorumRecord v4back;
    s4 >> v4back;
    BOOST_CHECK_EQUAL(v4back.idle_since_height, 888);
    BOOST_CHECK_EQUAL(v4len, v3bytes.size() + sizeof(int32_t));
}

// ★ The stamp sites + COPY-not-mutate, pinned structurally (the connect
// materialization is not unit-drivable; the drill verifies it live).
// Positive limbs: the fresh stamp and the inheritance (with the pre-v4
// predecessor fallback) exist in ProcessBlock.  Negative limb: NOTHING
// assigns to the predecessor's idle_since_height (copy, never mutate - the
// undo-clean pin).  RED: altering the fresh stamp or adding a predecessor
// mutation flips the counts.
BOOST_AUTO_TEST_CASE(W4L_StampSites_Structural)
{
    const std::string src = PTX_SRCDIR;
    BOOST_REQUIRE_MESSAGE(!src.empty(), "W4L: PTX_SRCDIR not injected");
    const std::string st = P5_slurp(src + "/src/ptx/ptx_quorum_store.cpp");
    BOOST_REQUIRE(!st.empty());
    BOOST_CHECK(P5_count(st, "rec.idle_since_height = pindex->nHeight;") == 1);   // fresh stamp
    BOOST_CHECK(P5_count(st, "rec.idle_since_height = lineagePred.idle_since_height >= 0") == 1); // inherit
    BOOST_CHECK(P5_count(st, ": lineagePred.mined_height;") == 1);                // pre-v4 fallback
    BOOST_CHECK(P5_count(st, "lineagePred.idle_since_height =") == 0);            // COPY not mutate
    BOOST_CHECK(P5_count(st, "predRec.idle_since_height =") == 0);                // (either spelling)
}

// ===========================================================================
// W2.5a INTERACTION HARDENING (ODC-054) — the multi-quorum compositions.
// ===========================================================================

// (IH) ★ THE GUARD-2 × GATE COUPLING, both arms.
// Arm 1 (gate OFF — the defect the CheckParams coupling makes unreachable in
// production): a rotation-impossible quorum's age never resets (the KDD-076
// yield doesn't run), it ages past the fairness floor, and Guard 2 hands it
// the boundary REGARDLESS of hash — the ODC-045-amendment fleet-halt
// reintroduced through the guard built to prevent starvation.  Pinned as the
// MECHANISM the G4a hard-reject exists for.
// Arm 2 (gate ON — YIELD-BEATS-OVERRIDE): the SAME quorum is
// due-and-impossible past grace -> terminal-eligible -> yields at the TOP of
// the due-loop, BEFORE Guard 2's overdue tracking ever sees it -> the
// healthy rival wins.  True by code order since P3; previously UNPINNED.
// RED (inversion: move Guard-2's overdue tracking ABOVE the yield): arm 2
// fails (the overdue-eligible quorum captures the slot); arm 1 and the G2
// rows hold (no eligibility there) — single-row attribution.
BOOST_AUTO_TEST_CASE(IH_YieldBeatsOverride_GateCouplingMechanism)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    G2RetireAllActive(store);

    std::vector<CBlockIndex> chain = W4dChain(200);
    const Consensus::PTXFormationParams params = G2Params();   // gate OFF (grace 0)

    // The impossible quorum: HIGHEST hash (would lose every tie-break),
    // formed one boundary before its healthy rival.
    const uint256 target = QHk(0xEE), rival = QHk(0x22);
    Pb6bSeedActive(target, 100, 102);
    Pb6bSeedActive(rival,  110, 112);
    auto emptyReader = [](const CBlockIndex*, CBlock& out) { out = CBlock(); return true; };
    auto impossibleTarget = [&](const CPTXQuorumRecord& r, const CBlockIndex*) {
        return r.quorum_hash == target;
    };

    // At chain[160]: target age 60 (= R + 2B, overdue), rival age 50 (due).
    // ARM 1 — gate off: no yield, the impossible quorum reaches Guard 2's
    // overdue candidacy and CAPTURES the slot.  The mechanism, demonstrated.
    {
        const PTXRotationDecision d = PTX_Formation_RotationDueAt(
                &chain[160], store, params, emptyReader, impossibleTarget);
        BOOST_REQUIRE(d.due);
        BOOST_CHECK_MESSAGE(d.predecessor_quorum_hash == target,
            "gate-off mechanism changed: the impossible quorum no longer "
            "captures via Guard 2 - re-examine the ODC-054 coupling rationale");
    }

    // ARM 2 — gate ON (grace 2): due-and-impossible at the last two
    // boundaries -> terminal-eligible -> YIELDS before Guard 2 sees it ->
    // the healthy rival wins the slot.
    {
        Consensus::PTXFormationParams gated = params;
        gated.nReformGrace = 2;
        const PTXRotationDecision d = PTX_Formation_RotationDueAt(
                &chain[160], store, gated, emptyReader, impossibleTarget);
        BOOST_REQUIRE(d.due);
        BOOST_CHECK_MESSAGE(d.predecessor_quorum_hash == rival,
            "the KDD-076 yield must remove the impossible quorum BEFORE "
            "Guard-2's overdue tracking - an overdue-eligible quorum captured "
            "the slot (yield-beats-override broken)");
    }
}

// (IH) ★ CORRELATED DEPARTURES + LIMITER SCALING at the producer level.
// Three simultaneously-eligible ACTIVE records with MIXED eligibility (two
// idle, one busy-but-impossible): exactly ONE reform per rate window, LRA
// first, deterministic drain across successive windows — the correlated-
// EVENT case (many quorums eligible at once) contained one-per-window.
// ★ The W4-f age-anchor key coherence, demonstrated live at the first
// boundary: idle-eligibles key on mined_height, which the age anchor
// (mined + window <= anchor) forces BELOW any in-window attribution — so
// long-idle quorums deterministically rank before the busy-but-impossible
// one.  rate_window (100) > boundary interval (80), so adjacent boundaries
// rate-limit out — the containment the KDD-074 limiter exists for.
// RED (inversion: drop the one-per-window gate in SelectReformCandidate):
// the rate-limited boundaries reform anyway (0-expected rows fail).
BOOST_AUTO_TEST_CASE(IH_CorrelatedDrain_OnePerWindowLRA)
{
    BOOST_REQUIRE(evoDb);
    PTX_BLS_WipeShares(evoDb.get());
    CPTXQuorumStore store(*evoDb);
    G2RetireAllActive(store);

    std::vector<CBlockIndex> chain = W4dChain(480);
    Consensus::PTXFormationParams params = W4fParams();   // B=R=80, window 50
    params.nReformGrace      = 2;
    params.nReformRateWindow = 100;                       // > B: adjacent boundaries limited

    // The correlated-eligible set: two long-idle, one busy-but-impossible.
    const uint256 idleA = QHk(0x41), idleB = QHk(0x52), busyC = QHk(0x63);
    Pb6bSeedActive(idleA, /*formation*/ 10, /*mined*/ 20);
    Pb6bSeedActive(idleB, /*formation*/ 12, /*mined*/ 30);
    Pb6bSeedActive(busyC, /*formation*/ 0,  /*mined*/ 5);

    // busyC rolls at 140 (inside the (110,160] window: NOT idle) and is
    // rotation-impossible — eligible via the FORCED arm only.
    std::map<int, CBlock> blocks = {{140, W4dRollBlock(busyC)}};
    auto reader = [&](const CBlockIndex* p, CBlock& out) {
        auto it = blocks.find(p->nHeight);
        out = (it != blocks.end()) ? it->second : CBlock();
        return true;
    };
    auto impossibleC = [&](const CPTXQuorumRecord& r, const CBlockIndex*) {
        return r.quorum_hash == busyC;
    };

    auto stateOf = [&](const uint256& qh) {
        CPTXQuorumRecord r;
        BOOST_REQUIRE(store.GetQuorumRecord(qh, r));
        return (PTXQuorumState)r.state;
    };

    // Boundary 160: all three eligible; LRA keys A=20 (mined), B=30 (mined),
    // C=140 (in-window attribution) — the age-anchor coherence: both idle
    // keys sit below the busy key by construction.  A drains first.
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[160], params, reader, impossibleC), 1u);
    BOOST_CHECK(stateOf(idleA) == PTXQuorumState::REFORMED);
    BOOST_CHECK(stateOf(idleB) == PTXQuorumState::ACTIVE);
    BOOST_CHECK(stateOf(busyC) == PTXQuorumState::ACTIVE);

    // Boundary 240: inside the rate window (240 - 160 = 80 < 100) — the
    // remaining eligibles QUEUE.  The correlated cascade, contained.
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[240], params, reader, impossibleC), 0u);

    // Boundary 320: window open again.  C's attribution has aged out of the
    // scan, so its key reverts to record antiquity (mined 5 < B's 30) — the
    // LRA re-derivation is stateless and deterministic.  C drains.
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[320], params, reader, impossibleC), 1u);
    BOOST_CHECK(stateOf(busyC) == PTXQuorumState::REFORMED);
    BOOST_CHECK(stateOf(idleB) == PTXQuorumState::ACTIVE);

    // Boundary 400: limited again (400 - 320 = 80 < 100) -> queue holds.
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[400], params, reader, impossibleC), 0u);

    // Boundary 480: the last eligible drains.  Three correlated eligibles,
    // three windows, deterministic order — nothing reformed en masse.
    BOOST_CHECK_EQUAL(store.MaybeReformAtBoundary(&chain[480], params, reader, impossibleC), 1u);
    BOOST_CHECK(stateOf(idleB) == PTXQuorumState::REFORMED);
}

// (ODC-055) ★ STRUCTURAL PIN — the catch-up relay's HOOK SITE.  The behavioral
// row (v_catchup_relays_stored_to_late_verified_member, validate_relay suite)
// proves the transport core; this pin proves the core is WIRED at the GMAUTH
// verify point — without the call the fix is dead code and the N98 qual=0
// race returns silently.  Call-shaped match (the W4d bare-name lesson).
// RED: remove the gmauth.cpp call -> this row fails while the behavioral row
// stays green — the exact split that makes an unwired fix visible.
BOOST_AUTO_TEST_CASE(ODC055_CatchupRelay_WiredAtVerify)
{
    const std::string src = PTX_SRCDIR;
    BOOST_REQUIRE_MESSAGE(!src.empty(), "PTX_SRCDIR not injected - pin cannot run");
    const std::string gmauth = P5_slurp(src + "/src/evo/gmauth.cpp");
    BOOST_REQUIRE(!gmauth.empty());
    // Line-anchored (newline + exact indentation + call): a COMMENTED-OUT
    // call must NOT satisfy this pin — a comment is not a caller (the W4d
    // lesson; the bare-substring form of this very pin passed its own
    // inversion and was strengthened to this).
    BOOST_CHECK_MESSAGE(
        P5_count(gmauth, "\n        PTX_Ceremony_CatchupRelayOnVerify(pnode);") == 1,
        "the catch-up relay is not called from the GMAUTH verify point - "
        "the ODC-055 fix is unwired and the announce race is back");
}

// ===========================================================================
// LINEAGE-SCOPED SCAN - the demanded-case half of seat-vs-record idleness.
// A demanded lineage's pre-rotation rolls (attributed to the predecessor's
// hash) must be SEEN by the successor's scan.  RED (hash-scoped inversion =
// pre-fix HEAD): the successor reads FALSE-IDLE - the gap reproduced.
// ===========================================================================

// A block carrying the rotation PTXDKG successor <- predecessor (the
// in-window lineage link the scan reads).
static CBlock W4SRotationBlock(const uint256& succ, const uint256& pred)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXDKG;
    mtx.vin.push_back(CTxIn(COutPoint(QHk(0xAB), 0)));
    PTXDKGPayload pl;
    pl.nVersion = PTXDKGPayload::ROTATION_VERSION;
    pl.quorum_hash = succ;
    pl.predecessor_quorum_hash = pred;
    SetTxPayload(mtx, pl);
    CBlock b;
    b.vtx.push_back(MakeTransactionRef(mtx));
    return b;
}

BOOST_AUTO_TEST_CASE(W4S_LineageScopedScan)
{
    const uint256 S = QHk(0x51), P = QHk(0x52), Q = QHk(0x53), other = QHk(0x54);
    std::vector<CBlockIndex> chain = W4dChain(200);
    const CBlockIndex* tip = &chain[200];
    const int N = 80;   // window (120, 200]

    std::map<int, CBlock> blocks;
    auto reader = [&blocks](const CBlockIndex* p2, CBlock& out) {
        auto it = blocks.find(p2->nHeight);
        out = (it != blocks.end()) ? it->second : CBlock();
        return true;
    };

    // ★ THE DEMANDED-LINEAGE ROW: P rolled at 130, P rotated into S at 150.
    // S's scan must SEE the seat's demand across the rotation -> NOT idle.
    // RED (hash-scoped): S sees nothing under its own hash -> FALSE-IDLE.
    blocks = {{130, W4dRollBlock(P)}, {150, W4SRotationBlock(S, P)}};
    BOOST_CHECK_MESSAGE(!PTX_Formation_QuorumIdleAt(S, tip, N, reader),
        "demanded lineage read FALSE-IDLE - the successor's scan cannot see "
        "the predecessor's in-window rolls (the hash-scoped gap)");

    // ORDER-FREE (the post-pass resolution pin): an in-flight roll attributed
    // to P but mined AFTER S's connect is still seen.
    blocks = {{150, W4SRotationBlock(S, P)}, {160, W4dRollBlock(P)}};
    BOOST_CHECK(!PTX_Formation_QuorumIdleAt(S, tip, N, reader));

    // MULTI-HOP: Q rolled at 125; Q->P at 135; P->S at 150.  Transitive.
    blocks = {{125, W4dRollBlock(Q)}, {135, W4SRotationBlock(P, Q)},
              {150, W4SRotationBlock(S, P)}};
    BOOST_CHECK(!PTX_Formation_QuorumIdleAt(S, tip, N, reader));

    // ATTRIBUTION STILL SCOPED: an UNRELATED quorum's roll does not count.
    blocks = {{130, W4dRollBlock(other)}, {150, W4SRotationBlock(S, P)}};
    BOOST_CHECK(PTX_Formation_QuorumIdleAt(S, tip, N, reader));

    // PRE-WINDOW rolls are irrelevant (the link is in-window, the roll not).
    blocks = {{119, W4dRollBlock(P)}, {150, W4SRotationBlock(S, P)}};
    BOOST_CHECK(PTX_Formation_QuorumIdleAt(S, tip, N, reader));

    // ★ THE BF-SAFE PIN: zero rolls anywhere -> lineage-scoped and
    // hash-scoped are IDENTICAL (both see nothing) -> idle.  The drill's
    // behaviour is untouched by this fix.
    blocks = {{150, W4SRotationBlock(S, P)}};
    BOOST_CHECK(PTX_Formation_QuorumIdleAt(S, tip, N, reader));
    blocks.clear();
    BOOST_CHECK(PTX_Formation_QuorumIdleAt(S, tip, N, reader));
}

// ===========================================================================
// W2.5a P1 — THE PARAMS DECOUPLE (KDD-079).  Two rows, both proving the
// SPLIT's load-bearing halves at DIVERGENT values (B != R), which is the only
// regime where the conflation is observable.
// ===========================================================================

// ★★ P1a — THE FOURTH-CONFLATION FIX.  ForcedReformGraceElapsed takes TWO
// intervals: boundary_interval steps back over boundaries, rotation_interval
// tests due-ness.  With ONE param serving both (the pre-fix form) the due-test
// runs against the BOUNDARY cadence, so at B=30/R=1440 a quorum only 30 blocks
// old reads as "due" and forced-reform trips wildly early.
// ★ A DEFECT FIX, not a rename — and the conflation had propagated INTO this
// function (written at W2.4 W4-d) because there was only one param to pass.
// RED: pass boundary_interval for BOTH (the pre-fix single-param form) -> the
// young quorum reads due -> grace elapses -> this row fails.
BOOST_AUTO_TEST_CASE(P1a_GraceUsesRotationIntervalForDueness)
{
    const int B = 30, R = 1440;
    std::vector<CBlockIndex> chain = W4dChain(3000);
    const CBlockIndex* anchor = &chain[3000];
    auto alwaysImpossible = [](const CBlockIndex*) { return true; };

    // A YOUNG quorum: formed 60 blocks ago — older than TWO boundaries (B=30)
    // but far younger than the rotation interval (R=1440).  It is NOT due.
    CPTXQuorumRecord young;
    young.formation_height = 2940;

    BOOST_CHECK_MESSAGE(
        !PTX_Formation_ForcedReformGraceElapsed(young, anchor, B, R, 2, alwaysImpossible),
        "a 60-block-old quorum is NOT rotation-due at R=1440 - grace must test "
        "due-ness against nRotationInterval, never the boundary cadence");

    // An OLD quorum (formed 2000 blocks ago) IS due at both of the last two
    // boundaries -> grace elapses.  Proves the row is not vacuously false.
    CPTXQuorumRecord old_q;
    old_q.formation_height = 1000;
    BOOST_CHECK(PTX_Formation_ForcedReformGraceElapsed(old_q, anchor, B, R, 2, alwaysImpossible));
}

// ★ P1b — GUARD 3, THE SEAM GUARD.  The ODC-050 stall-out budget must track
// nCeremonyBudget, NEVER the boundary cadence: a healthy ceremony spans ~27
// blocks (drill: formation 1120 -> connect 1147), so a 30-block boundary
// interval used as the budget would abort it mid-flight — the split breaking a
// shipped safety mechanism the moment it lands.
// RED: set max_span from the BOUNDARY interval (30) instead of the budget (80)
// -> the healthy ceremony aborts -> this row fails.
BOOST_AUTO_TEST_CASE(P1b_StallOutUsesCeremonyBudgetNotBoundary)
{
    const int B = 30, BUDGET = 80;
    // A ceremony still in a windowed phase 40 blocks in: past the BOUNDARY
    // interval, well inside the BUDGET.  It must NOT be stalled out.
    BOOST_CHECK_MESSAGE(40 < BUDGET,
        "a 40-block-old ceremony is inside the budget - must not stall out");
    // And the boundary interval, if wrongly used, WOULD have stalled it: the
    // arithmetic that makes Guard 3 load-bearing.
    BOOST_CHECK_MESSAGE(40 >= B,
        "the same ceremony IS past the boundary interval - which is exactly why "
        "the stall-out must not read the boundary cadence (KDD-079 Guard 3)");
    // The production wiring reads the budget, not the cadence.
    const std::string src = PTX_SRCDIR;
    BOOST_REQUIRE(!src.empty());
    const std::string form = P5_slurp(src + "/src/ptx/ptx_formation.cpp");
    BOOST_REQUIRE(!form.empty());
    BOOST_CHECK(P5_count(form, "max_span = Params().GetConsensus().ptxFormation.nCeremonyBudget") == 1);
    BOOST_CHECK(P5_count(form, "max_span = Params().GetConsensus().ptxFormation.nBoundaryInterval") == 0);
}

// ---------------------------------------------------------------------------
// Bug032_SignRefusesWithoutFundedCommitment  — THE INVARIANT RED (BUG-032)
//
// A roll's threshold signature IS its result (beacon = SigToBeacon(sig)). The
// current signing path (gm_bls_sign / FanOutSign) signs a round_seed the instant
// a share is held — BEFORE any funded commitment exists. That is the free
// preview/reroll: a caller learns the outcome for free and simply declines to
// commit if it dislikes it. The fix (Option A, fund-then-sign) gates signing on
// a funded commitment for the EXACT (round_seed, quorum_hash) — binding the
// result to payment AND to the canonical quorum (closing BUG-033's quorum-shop).
//
// PRIMARY (RED against current code): with a CURRENT share held but NO funded
//   commitment, the signing entry MUST refuse. Today it signs → CHECK fails → RED.
// ANTI-VACUITY (green before AND after the fix): once a funded commitment for the
//   SAME pair is present, signing MUST succeed — proving the gate is payment-
//   gated, not a blanket refusal. (Guards against the fix degenerating to
//   "always refuse", per the anti-vacuity discipline.)
// ---------------------------------------------------------------------------

// Forward decls (defined with the 2b helpers below): the invariant test now
// proves itself against the REAL mempool commitment, not the retired seam.
static CMutableTransaction CommitMakeTx(const uint256& qh, const uint256& seed,
                                        uint32_t nSeedHeight);
static void CommitInjectToMempool(const uint256& round_seed, const uint256& qh);

BOOST_AUTO_TEST_CASE(Bug032_SignRefusesWithoutFundedCommitment)
{
    mempool.clear();
    // A real DKG share (valid scalar) to sign with, installed under a synthetic
    // quorum_hash so the CURRENT-share store is populated and GetCurrentShare
    // succeeds — isolating the variable under test to the COMMITMENT gate alone.
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    uint8_t sk_bytes[32];
    blst_bendian_from_scalar(sk_bytes, &sessions[0].sk_share_i);

    uint256 quorum_hash = uint256S(
        "1111111111111111111111111111111111111111111111111111111111111111");
    uint256 round_seed = uint256S(
        "2222222222222222222222222222222222222222222222222222222222222222");

    std::string serr;
    BOOST_REQUIRE_MESSAGE(PTX_BLS_SetSkShare(quorum_hash, 1000, sk_bytes, serr),
                          "precondition: CURRENT share install failed: " + serr);

    // ── PRIMARY: no funded commitment for (round_seed, quorum_hash) → refuse.
    uint8_t sig[PTX_SIG_BYTES];
    std::string err;
    bool signed_uncommitted =
        PTX_SignRoundIfCommitted(round_seed, quorum_hash, sig, err);
    BOOST_CHECK_MESSAGE(!signed_uncommitted,
        "BUG-032: signing path produced a partial signature with NO funded "
        "commitment for round_seed (free preview / reroll). The fund-then-sign "
        "gate is absent.");

    // ── ANTI-VACUITY: with a real funded commitment in mempool, signing succeeds.
    CommitInjectToMempool(round_seed, quorum_hash);
    std::string err2;
    bool signed_committed =
        PTX_SignRoundIfCommitted(round_seed, quorum_hash, sig, err2);
    BOOST_CHECK_MESSAGE(signed_committed,
        "with a funded commitment present for the same (round_seed, quorum_hash), "
        "signing must succeed — the gate must be payment-gated, not blanket: " + err2);
    mempool.clear();
}

// ---------------------------------------------------------------------------
// BUG-032 increment 2a — the roll COMMITMENT tx (nType=12) + canonical-quorum gate
//
// The commitment is sig-less and results-less: it must be mempool-valid BEFORE
// signing (payment-before-reveal). Its contextual validity binds the round to
// the CANONICAL quorum — the one ACTIVE at nSeedHeight — closing the quorum-shop
// (BUG-033) at the commitment: the settle's sig will later be required to verify
// against exactly this quorum, chosen before any signature exists.
//
// RED  (NonCanonicalQuorumRejected): a commitment naming a recorded-but-NOT-
//   active-at-nSeedHeight quorum (superseded before the seed height) must be
//   REJECTED. Against the permissive stub it is accepted → CHECK fails → RED.
// ANTI-VACUITY (CanonicalQuorumAccepted): a commitment naming the quorum ACTIVE
//   at nSeedHeight is ACCEPTED — canonical-gated, not blanket-refuse. Green both.
// SIG-LESS (SigLessValid): a well-formed commitment carries no signature and no
//   results and is still valid — the whole point of fund-then-sign. Green both.
// ---------------------------------------------------------------------------

// Seed a PRIMARY quorum record (no DKG / no sig — the commitment verifies no
// signature) with controllable lineage so a test can make it active or not at a
// chosen height. GetQuorumRecord + PTX_QuorumRecordActiveAt read this.
static void CommitSeedRecord(const uint256& qh, int mined_h, int superseded_h,
                             PTXQuorumState st)
{
    CPTXQuorumRecord r;
    r.quorum_hash       = qh;
    r.formation_height  = mined_h;
    r.formed_size       = 11;
    r.completed_size    = 11;
    r.state             = static_cast<uint8_t>(st);
    r.mined_height      = mined_h;
    r.superseded_height = superseded_h;
    evoDb->Write(std::make_pair(PTX_QuorumRecordDBPrefix(), qh), r);
}

// A structurally-valid roll COMMITMENT tx: non-coinbase vin, one accum output at
// the service fee, sig-less/results-less payload, same-block window (expiry==seed).
static CMutableTransaction CommitMakeTx(const uint256& qh, const uint256& seed,
                                        uint32_t nSeedHeight)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXROLLCOMMIT;
    mtx.vin.push_back(CTxIn(COutPoint(QHk(0xAB), 0)));
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));

    CPTXRollCommitPayload p;
    p.nSeedHeight   = nSeedHeight;
    p.nExpiryHeight = nSeedHeight;   // same-block mandate: window 0
    p.count = 1; p.low = 1; p.high = 100;
    p.round_seed  = seed;
    p.quorum_hash = qh;
    SetTxPayload(mtx, p);
    return mtx;
}

BOOST_AUTO_TEST_CASE(Bug032_Commit_CanonicalQuorumAccepted)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xE1), seed = QHk(0x71);
    CommitSeedRecord(qh, /*mined*/1, /*superseded*/-1, PTXQuorumState::ACTIVE);
    BOOST_CHECK_EQUAL(W4bRunContextual(CommitMakeTx(qh, seed, /*nSeedHeight*/10)), "");
}

BOOST_AUTO_TEST_CASE(Bug032_Commit_NonCanonicalQuorumRejected)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xE2), seed = QHk(0x71);
    // Recorded, but SUPERSEDED at height 5 — NOT active at seed height 10. The
    // quorum-shop vector: an old/rotated quorum must not commit a current round.
    CommitSeedRecord(qh, /*mined*/1, /*superseded*/5, PTXQuorumState::SUPERSEDED);
    BOOST_CHECK_EQUAL(W4bRunContextual(CommitMakeTx(qh, seed, /*nSeedHeight*/10)),
                      "ptxcommit-noncanonical-quorum");
}

BOOST_AUTO_TEST_CASE(Bug032_Commit_SigLessValid)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xE3), seed = QHk(0x71);
    CommitSeedRecord(qh, 1, -1, PTXQuorumState::ACTIVE);
    // No signature, no results anywhere in the payload — and it validates.
    BOOST_CHECK_EQUAL(W4bRunContextual(CommitMakeTx(qh, seed, /*nSeedHeight*/10)), "");
}

// ---------------------------------------------------------------------------
// BUG-032 increment 2b — signing consults the REAL on-chain/mempool commitment
// (promotes increment 1's invariant off the in-memory registry seam) + the
// wait-not-reject latency property.
// ---------------------------------------------------------------------------

// Inject a REAL PTXROLLCOMMIT into the global mempool (mapTx) so the signing
// path's mempool scan finds it — the real commitment, not incr1's registry seam.
static void CommitInjectToMempool(const uint256& round_seed, const uint256& qh)
{
    CMutableTransaction mtx = CommitMakeTx(qh, round_seed, /*nSeedHeight*/10);
    TestMemPoolEntryHelper helper;
    LOCK(mempool.cs);
    mempool.addUnchecked(mtx.GetHash(), helper.FromTx(mtx));
}

// Install a real CURRENT share for qh so signing can proceed past the gate.
static void CommitInstallShare(const uint256& qh)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);
    uint8_t sk_bytes[32];
    blst_bendian_from_scalar(sk_bytes, &sessions[0].sk_share_i);
    std::string serr;
    BOOST_REQUIRE_MESSAGE(PTX_BLS_SetSkShare(qh, 1000, sk_bytes, serr),
                          "share install failed: " + serr);
}

// 2b-i: a PTXROLLCOMMIT broadcast to mempool (never registered via the retired
// in-memory seam) must ENABLE signing.
// RED against the in-memory registry: the mempool commitment is invisible to the
//   registry-based PTX_RollCommitmentPresent → signing refuses → CHECK fails.
// GREEN once the scan lands: the mempool commitment is found → signs.
BOOST_AUTO_TEST_CASE(Bug032_2b_SignsOnRealMempoolCommitment)
{
    mempool.clear();
    const uint256 qh = QHk(0xF1), seed = QHk(0x72);
    CommitInstallShare(qh);
    CommitInjectToMempool(seed, qh);   // the REAL commitment; retired registry NOT used
    uint8_t sig[PTX_SIG_BYTES]; std::string err;
    BOOST_CHECK_MESSAGE(PTX_SignRoundIfCommitted(seed, qh, sig, err),
        "signing must consult the real mempool PTXROLLCOMMIT, not the retired "
        "in-memory registry: " + err);
    mempool.clear();
}

// 2b-i anti-vacuity: with NO commitment anywhere (empty mempool), signing refuses.
BOOST_AUTO_TEST_CASE(Bug032_2b_RefusesWithNoMempoolCommitment)
{
    mempool.clear();
    const uint256 qh = QHk(0xF2), seed = QHk(0x72);
    CommitInstallShare(qh);
    uint8_t sig[PTX_SIG_BYTES]; std::string err;
    BOOST_CHECK_MESSAGE(!PTX_SignRoundIfCommitted(seed, qh, sig, err),
        "with no commitment in mempool or chain, signing must refuse");
    mempool.clear();
}

// 2b-ii — wait-not-reject (THE LATENCY PROPERTY). An ABSENT commitment is
// RETRYABLE (it may be in flight — propagation delay), a present-commitment-but-
// no-share is TERMINAL. The distinction is what lets the coordinator wait-and-
// retry legitimate rolls under network delay instead of failing them.
// RED against the stubbed-off retryable signal: absence returns retryable=false
//   → CHECK fails. GREEN once the fix sets retryable=true on absence.
BOOST_AUTO_TEST_CASE(Bug032_2b_UnseenCommitmentIsRetryable)
{
    mempool.clear();
    const uint256 qh = QHk(0xF3), seed = QHk(0x73);
    CommitInstallShare(qh);
    uint8_t sig[PTX_SIG_BYTES]; std::string err; bool retryable = false;
    BOOST_CHECK(!PTX_SignRoundIfCommitted(seed, qh, sig, err, &retryable));
    BOOST_CHECK_MESSAGE(retryable,
        "2b-ii: an unseen commitment must be RETRYABLE (wait for propagation), "
        "not a terminal refusal — else legitimate rolls fail under network delay");
    mempool.clear();
}

// 2b-ii terminal branch (anti-vacuity for the retryable signal): commitment
// present, but this node holds NO CURRENT share for the quorum → TERMINAL refusal
// (waiting cannot help). Proves retryable is a real discriminator, not always-true.
BOOST_AUTO_TEST_CASE(Bug032_2b_NoShareIsTerminal)
{
    mempool.clear();
    const uint256 qh = QHk(0xF4), seed = QHk(0x73);   // no share installed for 0xF4
    CommitInjectToMempool(seed, qh);                   // commitment present → past the gate
    uint8_t sig[PTX_SIG_BYTES]; std::string err; bool retryable = true; // init true; must be cleared
    BOOST_CHECK(!PTX_SignRoundIfCommitted(seed, qh, sig, err, &retryable));
    BOOST_CHECK_MESSAGE(!retryable,
        "2b-ii: no CURRENT share is a TERMINAL refusal (waiting cannot help), "
        "not retryable");
    mempool.clear();
}

// ---------------------------------------------------------------------------
// BUG-032 increment 2c (i+ii) — the coin-chained matched-pair rule (block-level).
// Every PTXSESS (reveal) must SPEND an output of a same-block PTXROLLCOMMIT
// (payment) and reveal the SAME round (round_seed) under the SAME quorum
// (quorum_hash) the commitment fixed. The coin-chain puts the never-separate
// guarantee in the UTXO layer; the payload binding closes the settle-side
// quorum-shop (Q2 / BUG-033). Q1 (results == MapBeacon) lands in 2c-iii.
// RED (NoCommitmentParent + QuorumMismatch + SeedMismatch): the bad settles pass
//   the permissive (commented-out) gate → CHECKs fail. GREEN once the gate lands.
// ---------------------------------------------------------------------------

// A settle (PTXSESS) that coin-chains to a parent commitment (spends its output).
// parentTxid null → an orphan input (no commitment parent, for the RED).
static CMutableTransaction SettleMakeTx(const uint256& round_seed, const uint256& qh,
                                        const uint256& parentTxid)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTX;
    mtx.vin.push_back(CTxIn(COutPoint(parentTxid.IsNull() ? QHk(0xBB) : parentTxid, 0)));
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));
    CProbabilisticTxPayload p;
    p.nSeedHeight     = 10;
    p.count = 1; p.low = 1; p.high = 100; p.results = {42};
    p.round_seed      = round_seed;
    p.quorum_hash     = qh;
    p.quorum_sig_hash = QHk(0xAB);   // non-null: satisfies the per-tx structural check
    SetTxPayload(mtx, p);
    return mtx;
}

static std::string RunPairing(const std::vector<CTransactionRef>& txs,
                              const std::map<uint256, CPTXRollCommitPayload>& confirmedParents = {})
{
    CBlock block;
    block.vtx = txs;
    CValidationState state;
    if (CheckPTXRollCommitSettlePairing(block, confirmedParents, state)) return "";
    return state.GetRejectReason();
}

// Anti-vacuity: a settle coin-chained to its commitment, matching seed + quorum → accepted.
BOOST_AUTO_TEST_CASE(Bug032_2c_MatchedPairAccepted)
{
    const uint256 qh = QHk(0xE1), seed = QHk(0x71);
    CMutableTransaction c = CommitMakeTx(qh, seed, 10);
    const uint256 ctxid = CTransaction(c).GetHash();
    CMutableTransaction s = SettleMakeTx(seed, qh, ctxid);
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(c), MakeTransactionRef(s)}), "");
}

// 2c-i RED: a settle with NO coin-chained commitment parent → rejected.
BOOST_AUTO_TEST_CASE(Bug032_2c_NoCommitmentParentRejected)
{
    const uint256 qh = QHk(0xE1), seed = QHk(0x71);
    CMutableTransaction c = CommitMakeTx(qh, seed, 10);        // present, but unspent by settle
    CMutableTransaction s = SettleMakeTx(seed, qh, uint256()); // orphan input
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(c), MakeTransactionRef(s)}),
                      "ptxsess-no-commitment-parent");
}

// 2c-ii Q2 RED: coin-chained, but the settle names a DIFFERENT quorum than its
// commitment (the settle-side quorum-shop, BUG-033) → rejected.
BOOST_AUTO_TEST_CASE(Bug032_2c_QuorumMismatchRejected)
{
    const uint256 qhC = QHk(0xE1), qhS = QHk(0xE9), seed = QHk(0x71);
    CMutableTransaction c = CommitMakeTx(qhC, seed, 10);
    const uint256 ctxid = CTransaction(c).GetHash();
    CMutableTransaction s = SettleMakeTx(seed, qhS, ctxid);
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(c), MakeTransactionRef(s)}),
                      "ptxsess-quorum-mismatch");
}

// 2c-ii RED: coin-chained, same quorum, but the settle reveals a DIFFERENT round
// than its commitment fixed → rejected.
BOOST_AUTO_TEST_CASE(Bug032_2c_SeedMismatchRejected)
{
    const uint256 qh = QHk(0xE1), seedC = QHk(0x71), seedS = QHk(0x79);
    CMutableTransaction c = CommitMakeTx(qh, seedC, 10);
    const uint256 ctxid = CTransaction(c).GetHash();
    CMutableTransaction s = SettleMakeTx(seedS, qh, ctxid);
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(c), MakeTransactionRef(s)}),
                      "ptxsess-seed-mismatch");
}

// ---------------------------------------------------------------------------
// BUG-034 RELAX — a valid settle must always be minable. The parent may be a
// commitment ALREADY CONFIRMED at any depth (resolved pre-spend by the caller
// and supplied via confirmedParents). Semantic RED: before the relax, every
// case below with a non-sibling parent rejected "ptxsess-no-commitment-parent"
// (the h5065 halt shape — a SUCCESSFUL roll's settle permanently unminable).
// Anti-vacuity: the gate is relaxed, not removed — no-parent-anywhere and the
// quorum/seed cross-checks (the anti-quorum-shop property carried over from
// the same-block rule) must still reject.
// ---------------------------------------------------------------------------

static CPTXRollCommitPayload ConfirmedCommitPayload(const uint256& qh, const uint256& seed)
{
    CPTXRollCommitPayload p;
    p.nSeedHeight   = 10;
    p.nExpiryHeight = 10;
    p.count = 1; p.low = 1; p.high = 100;
    p.round_seed  = seed;
    p.quorum_hash = qh;
    return p;
}

// GREEN (the BUG-034 core): a settle whose commitment mined in an EARLIER block
// (no sibling; parent payload pre-resolved) → accepted. Pre-relax: rejected.
BOOST_AUTO_TEST_CASE(Bug034_ConfirmedParentAccepted)
{
    const uint256 qh = QHk(0xD1), seed = QHk(0x61), parentTxid = QHk(0xC1);
    CMutableTransaction s = SettleMakeTx(seed, qh, parentTxid);
    const std::map<uint256, CPTXRollCommitPayload> parents{
        {parentTxid, ConfirmedCommitPayload(qh, seed)}};
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(s)}, parents), "");
}

// Anti-vacuity 1: no parent anywhere (not sibling, not confirmed) → still rejected.
BOOST_AUTO_TEST_CASE(Bug034_NoParentAnywhereStillRejected)
{
    const uint256 qh = QHk(0xD1), seed = QHk(0x61);
    CMutableTransaction s = SettleMakeTx(seed, qh, QHk(0xC1)); // parent unknown
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(s)}, {}),
                      "ptxsess-no-commitment-parent");
}

// Anti-vacuity 2 (anti-quorum-shop across blocks): confirmed parent committed a
// DIFFERENT quorum than the settle reveals under → still rejected.
BOOST_AUTO_TEST_CASE(Bug034_ConfirmedParentQuorumMismatchRejected)
{
    const uint256 qhC = QHk(0xD1), qhS = QHk(0xD9), seed = QHk(0x61), parentTxid = QHk(0xC1);
    CMutableTransaction s = SettleMakeTx(seed, qhS, parentTxid);
    const std::map<uint256, CPTXRollCommitPayload> parents{
        {parentTxid, ConfirmedCommitPayload(qhC, seed)}};
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(s)}, parents),
                      "ptxsess-quorum-mismatch");
}

// Anti-vacuity 3: confirmed parent fixed a DIFFERENT round → still rejected.
BOOST_AUTO_TEST_CASE(Bug034_ConfirmedParentSeedMismatchRejected)
{
    const uint256 qh = QHk(0xD1), seedC = QHk(0x61), seedS = QHk(0x69), parentTxid = QHk(0xC1);
    CMutableTransaction s = SettleMakeTx(seedS, qh, parentTxid);
    const std::map<uint256, CPTXRollCommitPayload> parents{
        {parentTxid, ConfirmedCommitPayload(qh, seedC)}};
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(s)}, parents),
                      "ptxsess-seed-mismatch");
}

// Precedence: a same-block sibling still binds FIRST (the normal fast path is
// unchanged by the relax) — sibling matches, confirmed map carries a decoy
// entry for the same txid with a different seed; sibling wins → accepted.
BOOST_AUTO_TEST_CASE(Bug034_SiblingTakesPrecedenceOverConfirmed)
{
    const uint256 qh = QHk(0xD1), seed = QHk(0x61);
    CMutableTransaction c = CommitMakeTx(qh, seed, 10);
    const uint256 ctxid = CTransaction(c).GetHash();
    CMutableTransaction s = SettleMakeTx(seed, qh, ctxid);
    const std::map<uint256, CPTXRollCommitPayload> parents{
        {ctxid, ConfirmedCommitPayload(qh, QHk(0x69))}};  // decoy: would seed-mismatch
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(c), MakeTransactionRef(s)}, parents), "");
}

// BUG-034 P3 — the shared verdict is the assembler filter's contract: the
// pairing suite above routes through PTX_SettleParentVerdict (equivalence by
// construction); these pin the enum semantics the filter switches on.
BOOST_AUTO_TEST_CASE(Bug034_P3_VerdictEnumSemantics)
{
    const uint256 qh = QHk(0xD1), seed = QHk(0x61), parentTxid = QHk(0xC1);
    const std::map<uint256, CPTXRollCommitPayload> parents{
        {parentTxid, ConfirmedCommitPayload(qh, seed)}};
    // OK via confirmed parent
    CMutableTransaction s1 = SettleMakeTx(seed, qh, parentTxid);
    BOOST_CHECK(PTX_SettleParentVerdict(CTransaction(s1), {}, parents) == PTXSettleParentVerdict::OK);
    // NO_PARENT with empty maps
    BOOST_CHECK(PTX_SettleParentVerdict(CTransaction(s1), {}, {}) == PTXSettleParentVerdict::NO_PARENT);
    // QUORUM_MISMATCH / SEED_MISMATCH against the confirmed parent
    CMutableTransaction s2 = SettleMakeTx(seed, QHk(0xD9), parentTxid);
    BOOST_CHECK(PTX_SettleParentVerdict(CTransaction(s2), {}, parents) == PTXSettleParentVerdict::QUORUM_MISMATCH);
    CMutableTransaction s3 = SettleMakeTx(QHk(0x69), qh, parentTxid);
    BOOST_CHECK(PTX_SettleParentVerdict(CTransaction(s3), {}, parents) == PTXSettleParentVerdict::SEED_MISMATCH);
    // ★ THE CRITICAL INERT CASE: parent in the SIBLING map (same-template pair,
    // the p50=0 production path) with NO confirmed entry → OK. A filter that
    // consulted confirmed state only would drop every healthy same-block pair —
    // BUG-034 re-created by its own fix.
    const std::map<uint256, CPTXRollCommitPayload> sib{
        {parentTxid, ConfirmedCommitPayload(qh, seed)}};
    BOOST_CHECK(PTX_SettleParentVerdict(CTransaction(s1), sib, {}) == PTXSettleParentVerdict::OK);
}

// BUG-034 P3 — filter-level inertness: PTX_TemplateUnpairableSettles must flag
// NOTHING on a healthy same-template pair (sibling map comes from the template's
// own vtx; view=nullptr proves siblings need no confirmed state), and must flag
// exactly the orphan-input settle when one is present.
BOOST_AUTO_TEST_CASE(Bug034_P3_FilterInertOnHealthySiblingPair)
{
    LOCK(cs_main);
    const uint256 qh = QHk(0xD1), seed = QHk(0x61);
    CMutableTransaction c = CommitMakeTx(qh, seed, 10);
    const uint256 ctxid = CTransaction(c).GetHash();
    CMutableTransaction s = SettleMakeTx(seed, qh, ctxid);
    CBlock healthy;
    healthy.vtx = {MakeTransactionRef(c), MakeTransactionRef(s)};
    BOOST_CHECK(PTX_TemplateUnpairableSettles(healthy, nullptr, nullptr).empty());

    // And the identification path: an orphan-input settle IS flagged (NO_PARENT).
    CMutableTransaction bad = SettleMakeTx(seed, qh, QHk(0xC9));
    CBlock poisoned;
    poisoned.vtx = {MakeTransactionRef(c), MakeTransactionRef(s), MakeTransactionRef(bad)};
    const auto flagged = PTX_TemplateUnpairableSettles(poisoned, nullptr, nullptr);
    BOOST_REQUIRE_EQUAL(flagged.size(), 1U);
    BOOST_CHECK(flagged[0].first == CTransaction(bad).GetHash());
    BOOST_CHECK(flagged[0].second == PTXSettleParentVerdict::NO_PARENT);
}

// ---------------------------------------------------------------------------
// MULTI-PAIR IN ONE BLOCK — coin-chain isolation. Rolls anchored to the same tip
// select the SAME quorum (PTX_LoadDKGSigningCtx keys on the block hash), so N
// rolls put N commit->settle pairs in ONE block under the same quorum. Each
// settle must pair with ITS OWN commitment by the COIN-CHAIN (the spent output),
// not loosely by quorum_hash/round_seed — or two pairs could cross-contaminate.
// A serial (one-caller-one-roll-per-block) test never exercises this; it is core
// coin-chain correctness, not deferred fleet coverage.
// ---------------------------------------------------------------------------

// POSITIVE: two distinct pairs in one block, each settle spends its own
// commitment (same quorum, different round_seeds) → both validate.
BOOST_AUTO_TEST_CASE(Bug032_2c_MultiPair_BothPairCorrectly)
{
    const uint256 qh = QHk(0xE1), seedA = QHk(0x71), seedB = QHk(0x72);
    CMutableTransaction cA = CommitMakeTx(qh, seedA, 10); const uint256 tA = CTransaction(cA).GetHash();
    CMutableTransaction cB = CommitMakeTx(qh, seedB, 10); const uint256 tB = CTransaction(cB).GetHash();
    BOOST_REQUIRE(tA != tB);   // real two pairs (anti-vacuity)
    CMutableTransaction sA = SettleMakeTx(seedA, qh, tA);   // A settles A
    CMutableTransaction sB = SettleMakeTx(seedB, qh, tB);   // B settles B
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(cA), MakeTransactionRef(cB),
                                  MakeTransactionRef(sA), MakeTransactionRef(sB)}), "");
}

// NEGATIVE (the discriminator — proves pairing is by-coin, not by-seed): a settle
// carrying A's round_seed but SPENDING B's output must bind to B by the coin-chain
// and then be rejected on the seed check. A LOOSE match-by-round_seed impl would
// wrongly accept it (find A by seed, ignore which output it spent).
BOOST_AUTO_TEST_CASE(Bug032_2c_MultiPair_CrossSpendRejected)
{
    const uint256 qh = QHk(0xE1), seedA = QHk(0x71), seedB = QHk(0x72);
    CMutableTransaction cA = CommitMakeTx(qh, seedA, 10);
    CMutableTransaction cB = CommitMakeTx(qh, seedB, 10); const uint256 tB = CTransaction(cB).GetHash();
    CMutableTransaction sCross = SettleMakeTx(seedA, qh, tB);   // A's seed, spends B
    BOOST_CHECK_EQUAL(RunPairing({MakeTransactionRef(cA), MakeTransactionRef(cB),
                                  MakeTransactionRef(sCross)}),
                      "ptxsess-seed-mismatch");
}

// ---------------------------------------------------------------------------
// BUG-032 2b-iii FEE RELOCATION. The service fee lives in the PTXROLLCOMMIT now
// (fund-then-sign: payment is forfeited at commit, before the result is knowable
// — only fee-in-commitment closes the free preview). The PTXSESS (reveal) is
// FORBIDDEN an accum fee output, else the roll pays twice. The pool is fed
// identically (the commitment's accum output is swept by the same coalesce C1),
// so this is a RELOCATION, not a model change.
// RED against the current "settle must carry the fee" rule:
//   with-fee: current ACCEPTS → the "reject" leg fails; without-fee: current
//   REJECTS (ptx-bad-accum-output) → the "accept" leg fails. GREEN on the flip.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(Bug032_2biii_SettleWithFeeRejected)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xD1), msg = QHk(0x77);
    std::vector<uint8_t> sig;
    W4bMakeQuorumAndSig(qh, msg, sig);
    // Add an accum fee output to the (now fee-less) settle → forbidden.
    CMutableTransaction s = W4bMakeRollTx(qh, msg, sig);
    s.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));
    BOOST_CHECK_EQUAL(W4bRunContextual(s), "ptxsess-redundant-fee");
}

// Anti-vacuity: a settle with NO accum fee output validates (not blanket-reject).
BOOST_AUTO_TEST_CASE(Bug032_2biii_SettleWithoutFeeAccepted)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xD2), msg = QHk(0x77);
    std::vector<uint8_t> sig;
    W4bMakeQuorumAndSig(qh, msg, sig);
    // W4bMakeRollTx now carries no accum fee output → the valid new-semantics settle.
    BOOST_CHECK_EQUAL(W4bRunContextual(W4bMakeRollTx(qh, msg, sig)), "");
}

// Leg 3: the fee's NEW home — a commitment carrying the accum fee is accepted (2a).
BOOST_AUTO_TEST_CASE(Bug032_2biii_CommitmentCarriesFee)
{
    W4bStoreGuard g;
    const uint256 qh = QHk(0xD3), seed = QHk(0x71);
    CommitSeedRecord(qh, 1, -1, PTXQuorumState::ACTIVE);
    BOOST_CHECK_EQUAL(W4bRunContextual(CommitMakeTx(qh, seed, 10)), "");
}

// ---------------------------------------------------------------------------
// BUG-039 — the boot-ordering wipe: init runs CVerifyDB (whose PTXStateSentry
// snapshots and restores the share map) BEFORE LoadTierTwo runs LoadShares.
// The sentry therefore snapshots the NOT-YET-LOADED (empty) map, and its dtor's
// RestoreShares(empty) rewrote ptx_shares.dat from the empty map — destroying
// every persisted share on EVERY restart, crash or clean. ODC-070 x BUG-029
// interaction: each fix correct alone; the composition, mediated by an init
// ordering neither reasoned about, loses the one state in the system that is
// NOT derivable from chain (ceremony secret material — no recovery path).
//
// These cases cross the seam at function level in init's exact order; the
// fleet-side persistence-survival standing test crosses the real init.
// ---------------------------------------------------------------------------

// THE seam: disk holds a share, the process "restarts" (map cleared), the
// sentry pair runs against the still-unloaded map, THEN LoadShares runs.
// The share must survive.
// RED (inversion, pre-fix): RestoreShares(empty) rewrites the file empty and
// LoadShares reads back nothing — the exact fleet-wide 2026-08-16 loss.
BOOST_AUTO_TEST_CASE(Bug039_BootOrder_SentryBeforeLoad_ShareSurvives)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    PTX_BLS_WipeShares(evoDb.get());

    const uint256 qh = QHk(0xE1);
    HeldShare hs; std::memset(hs.bytes, 0xE1, 32);
    hs.role = PTXShareRole::CURRENT; hs.formation_height = 1140;
    hs.promotion_height = -1;
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), qh, hs));

    PTX_TEST_ClearSkShareSlot();                    // process death; disk is truth

    // init's order: VerifyDB's sentry fires around the not-yet-loaded map...
    auto snap = PTX_BLS_SnapshotShares();
    BOOST_REQUIRE_EQUAL(snap.size(), 0u);           // the empty pre-load snapshot
    PTX_BLS_RestoreShares(std::move(snap), evoDb.get());

    // ...and only later does LoadTierTwo load the shares.
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(qh, out));  // survived the boot ordering
    BOOST_CHECK_EQUAL(out[0], 0xE1);

    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// The floor invariant, independent of the sentry: NO code path may write an
// empty map over a non-empty file. A legal erase of the LAST held share defers
// its disk erase (the stale entry reloads and re-erases in memory; explicit
// clearing is WipeShares' job) — retaining irreplaceable material too long is
// recoverable, destroying it is not.
// RED (inversion, pre-fix): RetireShare of the only share empties the file;
// the reload finds nothing.
BOOST_AUTO_TEST_CASE(Bug039_EmptyOverwriteRefused_LastShareRetainedOnDisk)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    PTX_BLS_WipeShares(evoDb.get());

    const uint256 qh = QHk(0xE2);
    HeldShare hs; std::memset(hs.bytes, 0xE2, 32);
    hs.role = PTXShareRole::CURRENT; hs.formation_height = 100;
    hs.promotion_height = -1;
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), qh, hs));

    BOOST_REQUIRE(PTX_BLS_RetireShare(qh, evoDb.get()));   // legal final erase (memory)
    BOOST_CHECK_EQUAL(g_ptx_my_shares.size(), 0u);

    PTX_TEST_ClearSkShareSlot();                    // restart
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(qh, out));  // disk floor held: share retained

    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// Erase-reaches-disk (P2 defect (a)) is KEPT whenever the write leaves the file
// non-empty: with a second live share held, an erased share is genuinely gone
// from disk after reload.
BOOST_AUTO_TEST_CASE(Bug039_EraseReachesDisk_WhenFileStaysNonEmpty)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    PTX_BLS_WipeShares(evoDb.get());

    const uint256 keep = QHk(0xE3), gone = QHk(0xE4);
    HeldShare hk; std::memset(hk.bytes, 0xE3, 32);
    hk.role = PTXShareRole::CURRENT; hk.formation_height = 100; hk.promotion_height = -1;
    HeldShare hg = hk; std::memset(hg.bytes, 0xE4, 32);
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), keep, hk));
    BOOST_REQUIRE(PTX_BLS_PersistShare(evoDb.get(), gone, hg));

    BOOST_REQUIRE(PTX_BLS_RetireShare(gone, evoDb.get()));

    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(keep, out));   // survivor intact
    BOOST_CHECK(!PTX_BLS_GetCurrentShare(gone, out));  // erased share gone from disk

    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

// The latent sibling fixed in the same pass: persistence must not depend on the
// evoDb global being constructed — the parameter is vestigial (ODC-070 file
// store) and a null must not silently skip the disk write.
// RED (inversion, pre-fix signature): the evoDb != nullptr guards around
// PersistShare call sites skipped persistence entirely when null.
BOOST_AUTO_TEST_CASE(Bug039_PersistShare_NullEvoDb_StillReachesDisk)
{
    BOOST_REQUIRE(evoDb);
    PTX_TEST_ClearSkShareSlot();
    PTX_BLS_WipeShares(evoDb.get());

    const uint256 qh = QHk(0xE5);
    HeldShare hs; std::memset(hs.bytes, 0xE5, 32);
    hs.role = PTXShareRole::CURRENT; hs.formation_height = 100;
    hs.promotion_height = -1;
    BOOST_REQUIRE(PTX_BLS_PersistShare(nullptr, qh, hs));  // no evoDb: file is the store

    PTX_TEST_ClearSkShareSlot();
    BOOST_REQUIRE_EQUAL(PTX_BLS_LoadShares(*evoDb), 0);
    uint8_t out[32];
    BOOST_CHECK(PTX_BLS_GetCurrentShare(qh, out));

    PTX_BLS_WipeShares(evoDb.get());
    PTX_TEST_ClearSkShareSlot();
}

BOOST_AUTO_TEST_SUITE_END()
