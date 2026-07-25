// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.2-P4: Phase 4 (PREMIT / premature commitment) unit tests.
// Direct-call tests — no daemon, no P2P.
//
// Falsification discipline:
//   Every acceptance test paired with a mutation.  Two load-bearing stub cycles
//   (spec §6, C2 resolution):
//
//   Stub 1 (own-vvec[0] omission): ComputeGroupPk skips my_proTxHash branch.
//     Predicted RED: P4_ComputeGroupPk_IncludesOwnVvec,
//                    P4_ComputeGroupPk_ExcludesBadMembers  (both vs independent manual sum).
//     Consistency tests (P4_ClosePhase4_*) are SECONDARY — all sessions run the
//     same stub, agree on the wrong value, stay GREEN.  Not a falsification signal.
//
//   Stub 2 (zero sk_share): ComputeSkShare stores zero.
//     Predicted RED: P4_ComputeSkShare_ValidSession.
//     ComputeGroupPk tests stay GREEN (independent of sk_share).
//
// P4_ComputeGroupPk_* compare against an INDEPENDENT manual sum built directly
// from session vvec data in the test body — NOT from another session or a
// reference call to ComputeGroupPk.  This is what makes the stub cycle
// harness-independent (spec §6).
//
// Test inventory (15 cases):
//   P4_ComputeSkShare_ValidSession             Green  non-zero scalar, 11 members
//   P4_ComputeSkShare_ExcludesBadMembers       Green  result differs from full-set sum
//   P4_ComputeSkShare_MissingShareAborts       Green  missing entry → false (abort)
//   P4_ComputeGroupPk_IncludesOwnVvec          Green  vs independent manual sum (primary stub target)
//   P4_ComputeGroupPk_ExcludesBadMembers       Green  vs independent manual sum excluding bad member
//   P4_AcceptValidPhase4Msg                    Green  Build+Receive → accepted, stored
//   P4_RejectWrongQuorumHash                   Red falsification
//   P4_RejectBadSig                            Red falsification
//   P4_RejectNonQualMember                     Red falsification
//   P4_RejectAlreadyBadMember                  Red falsification
//   P4_RejectDuplicate                         Red falsification
//   P4_RejectBadGroupPkDecompress              Red falsification
//   P4_ClosePhase4_SucceedsAtThreshold         Green  ≥ t=6 consistent → FINALIZE
//   P4_ClosePhase4_AbortsBelowThreshold        Green  5 consistent → ABORTED
//   P4_ClosePhase4_InconsistentMsgsNotCounted  Green  4 consistent + 2 differing → ABORTED

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"

#include "bls/bls_wrapper.h"
#include "random.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_phase4_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers — mirrors P2 pattern; static to this translation unit
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

// proTxHash for "session i" (the member self-identifying as sessions[i])
static uint256 PtxOf(const std::vector<PTXDKGSession>& sessions, int i)
{
    return sessions[i].members[sessions[i].my_idx].proTxHash;
}

// Drive all 11 sessions through P0 (hash-commit) — mirrors P2 test helpers.
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

// Drive all 11 sessions through P1 (contribution reveal + share decrypt) → COMPLAINT.
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

// Drive all 11 sessions through P2 + P3 (no complaints) → PREMIT.
static void AdvanceToPremit(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    AdvanceToComplaint(key_map, sessions);

    // No complaints filed — all members pass Feldman check.
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase2(sessions[i]));

    // No justifications needed.
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase3(sessions[i]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(sessions[i].phase == PTXDKGPhase::PREMIT);
}

// Build the independent manual group_pk sum for a given session and bad-member set.
// Does NOT call ComputeGroupPk — computes directly from session vvec data.
// The returned bytes are blst_p1_affine_compress output (48 bytes).
static void ManualGroupPkBytes(
    const PTXDKGSession& session,
    uint8_t out[48])
{
    const uint256& my_ptx = session.members[session.my_idx].proTxHash;

    blst_p1 acc;
    bool acc_set = false;

    for (const auto& ptx : session.qual) {
        if (session.bad_members.count(ptx))
            continue;

        const blst_p1_affine* vvec0 = nullptr;
        blst_p1_affine own_copy;
        if (ptx == my_ptx) {
            own_copy = session.local_contrib.vvec[0];
            vvec0 = &own_copy;
        } else {
            vvec0 = &session.phase1_vvecs.at(ptx)[0];
        }

        if (!acc_set) {
            blst_p1_from_affine(&acc, vvec0);
            acc_set = true;
        } else {
            blst_p1_add_or_double_affine(&acc, &acc, vvec0);
        }
    }

    BOOST_REQUIRE(acc_set);
    blst_p1_affine result;
    blst_p1_to_affine(&result, &acc);
    blst_p1_affine_compress(out, &result);
}

// ---------------------------------------------------------------------------
// P4_ComputeSkShare_ValidSession
//
// 11 members, no bad_members: ComputeSkShare returns true; sk_share_i is non-zero.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_ComputeSkShare_ValidSession)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    BOOST_CHECK(PTX_DKG_ComputeSkShare(sessions[0]));

    // sk_share_i must be non-zero (with overwhelming probability for random polynomial)
    bool all_zero = true;
    for (int b = 0; b < 32; b++) {
        if (sessions[0].sk_share_i.b[b] != 0) { all_zero = false; break; }
    }
    BOOST_CHECK(!all_zero);
}

// ---------------------------------------------------------------------------
// P4_ComputeSkShare_ExcludesBadMembers
//
// Add 1 member to bad_members: result differs from the all-members sum.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_ComputeSkShare_ExcludesBadMembers)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    // Full-set sum on sessions[0] (no bad_members yet).
    BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[0]));
    blst_scalar full_sum = sessions[0].sk_share_i;

    // Add one member to bad_members and recompute.
    uint256 excluded_ptx = PtxOf(sessions, 5);
    sessions[0].bad_members.insert(excluded_ptx);
    sessions[0].phase4_computed = false; // reset guard so recompute is allowed

    BOOST_CHECK(PTX_DKG_ComputeSkShare(sessions[0]));
    blst_scalar partial_sum = sessions[0].sk_share_i;

    // Sums must differ (with overwhelming probability — excluded share is non-zero).
    bool equal = (std::memcmp(full_sum.b, partial_sum.b, 32) == 0);
    BOOST_CHECK(!equal);
}

// ---------------------------------------------------------------------------
// P4_ComputeSkShare_MissingShareAborts
//
// Remove one effective-QUAL entry from received_shares: ComputeSkShare aborts
// (returns false) rather than silently skipping the dealer.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_ComputeSkShare_MissingShareAborts)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    // Remove one effective-QUAL share from sessions[0].
    uint256 missing_ptx = PtxOf(sessions, 3);
    BOOST_REQUIRE(sessions[0].received_shares.count(missing_ptx));
    sessions[0].received_shares.erase(missing_ptx);

    BOOST_CHECK(!PTX_DKG_ComputeSkShare(sessions[0]));
}

// ---------------------------------------------------------------------------
// P4_ComputeGroupPk_IncludesOwnVvec   — PRIMARY STUB TARGET (stub 1)
//
// Compares computed group_pk against an INDEPENDENT manual sum built directly
// from session vvec data in this test body.  Does not call ComputeGroupPk in
// the reference path.  This test is harness-independent: it reddens under
// stub 1 (own-vvec[0] omission) regardless of how many sessions are stubbed.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_ComputeGroupPk_IncludesOwnVvec)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    // Independent manual sum for sessions[0], no bad members.
    uint8_t expected[48];
    ManualGroupPkBytes(sessions[0], expected);

    BOOST_CHECK(PTX_DKG_ComputeGroupPk(sessions[0]));
    BOOST_CHECK(sessions[0].phase4_computed);

    uint8_t actual[48];
    blst_p1_affine_compress(actual, &sessions[0].group_pk);

    BOOST_CHECK_EQUAL_COLLECTIONS(actual, actual + 48, expected, expected + 48);
}

// ---------------------------------------------------------------------------
// P4_ComputeGroupPk_ExcludesBadMembers   — SECONDARY STUB TARGET (stub 1)
//
// Same independent-manual-sum discipline as IncludesOwnVvec but with one
// bad member excluded.  bad_members must be set identically in both the
// ManualGroupPkBytes call and in the session before ComputeGroupPk.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_ComputeGroupPk_ExcludesBadMembers)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    // Exclude member at slot 7 from both the manual sum and the session.
    uint256 bad_ptx = PtxOf(sessions, 7);
    sessions[0].bad_members.insert(bad_ptx);

    // Independent manual sum reflecting the same bad_members set.
    uint8_t expected[48];
    ManualGroupPkBytes(sessions[0], expected);

    BOOST_CHECK(PTX_DKG_ComputeGroupPk(sessions[0]));

    uint8_t actual[48];
    blst_p1_affine_compress(actual, &sessions[0].group_pk);

    BOOST_CHECK_EQUAL_COLLECTIONS(actual, actual + 48, expected, expected + 48);
}

// ---------------------------------------------------------------------------
// P4_AcceptValidPhase4Msg
//
// Build a Phase 4 msg and receive it: accepted and stored in phase4_premit_msgs.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_AcceptValidPhase4Msg)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[0]));
    BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[0]));

    PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[0], key_map.at(PtxOf(sessions, 0)));

    // Receive into sessions[1] (a different session acting as receiver).
    BOOST_CHECK(PTX_DKG_ReceivePhase4Msg(sessions[1], msg));
    BOOST_CHECK_EQUAL(sessions[1].phase4_premit_msgs.count(PtxOf(sessions, 0)), 1u);
}

// ---------------------------------------------------------------------------
// P4_RejectWrongQuorumHash
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_RejectWrongQuorumHash)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[0]));
    BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[0]));

    PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[0], key_map.at(PtxOf(sessions, 0)));

    // Corrupt quorum_hash (without re-signing — sig now covers the wrong hash too,
    // but the receiver's quorum_hash check fires first).
    msg.quorum_hash.begin()[0] ^= 0xFF;

    BOOST_CHECK(!PTX_DKG_ReceivePhase4Msg(sessions[1], msg));
    BOOST_CHECK_EQUAL(sessions[1].phase4_premit_msgs.count(PtxOf(sessions, 0)), 0u);
}

// ---------------------------------------------------------------------------
// P4_RejectBadSig
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_RejectBadSig)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[0]));
    BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[0]));

    PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[0], key_map.at(PtxOf(sessions, 0)));

    // Flip one byte in the signature.
    std::vector<uint8_t> bytes = msg.sig.ToByteVector();
    bytes[0] ^= 0xFF;
    msg.sig.SetByteVector(bytes);

    BOOST_CHECK(!PTX_DKG_ReceivePhase4Msg(sessions[1], msg));
    BOOST_CHECK_EQUAL(sessions[1].phase4_premit_msgs.count(PtxOf(sessions, 0)), 0u);
}

// ---------------------------------------------------------------------------
// P4_RejectNonQualMember
//
// Sender not in qual (forge a proTxHash that is in members but not in qual).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_RejectNonQualMember)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[0]));
    BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[0]));

    PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[0], key_map.at(PtxOf(sessions, 0)));

    // Remove sender from qual in the receiving session.
    sessions[1].qual.erase(PtxOf(sessions, 0));

    BOOST_CHECK(!PTX_DKG_ReceivePhase4Msg(sessions[1], msg));
    BOOST_CHECK_EQUAL(sessions[1].phase4_premit_msgs.count(PtxOf(sessions, 0)), 0u);
}

// ---------------------------------------------------------------------------
// P4_RejectAlreadyBadMember
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_RejectAlreadyBadMember)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[0]));
    BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[0]));

    PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[0], key_map.at(PtxOf(sessions, 0)));

    // Mark sender bad in the receiving session.
    sessions[1].bad_members.insert(PtxOf(sessions, 0));

    BOOST_CHECK(!PTX_DKG_ReceivePhase4Msg(sessions[1], msg));
    BOOST_CHECK_EQUAL(sessions[1].phase4_premit_msgs.count(PtxOf(sessions, 0)), 0u);
}

// ---------------------------------------------------------------------------
// P4_RejectDuplicate
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_RejectDuplicate)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[0]));
    BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[0]));

    PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[0], key_map.at(PtxOf(sessions, 0)));

    BOOST_CHECK(PTX_DKG_ReceivePhase4Msg(sessions[1], msg));   // first: accepted
    BOOST_CHECK(!PTX_DKG_ReceivePhase4Msg(sessions[1], msg));  // duplicate: rejected
    // Exactly one entry stored.
    BOOST_CHECK_EQUAL(sessions[1].phase4_premit_msgs.count(PtxOf(sessions, 0)), 1u);
}

// ---------------------------------------------------------------------------
// P4_RejectBadGroupPkDecompress
//
// Corrupt group_pk_bytes so blst_p1_uncompress fails.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_RejectBadGroupPkDecompress)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[0]));
    BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[0]));

    PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[0], key_map.at(PtxOf(sessions, 0)));

    // Corrupt group_pk_bytes and re-sign so sig check passes but decompress fails.
    std::memset(msg.group_pk_bytes, 0xFF, 48); // invalid compressed G1 point
    // KDD-072 P-b2: sign over the receiver's (fresh, zero-predecessor) view so
    // check 7 passes and the decompress gate (check 8) is what rejects.
    msg.sig = key_map.at(PtxOf(sessions, 0)).Sign(
        msg.GetSignHash(sessions[1].predecessor_quorum_hash));

    BOOST_CHECK(!PTX_DKG_ReceivePhase4Msg(sessions[1], msg));
    BOOST_CHECK_EQUAL(sessions[1].phase4_premit_msgs.count(PtxOf(sessions, 0)), 0u);
}

// ---------------------------------------------------------------------------
// P4_ClosePhase4_SucceedsAtThreshold
//
// ≥ t=6 consistent premature commitments → FINALIZE.
// All 11 sessions compute and exchange messages; sessions[0] calls ClosePhase4.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_ClosePhase4_SucceedsAtThreshold)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    // Compute aggregates for all sessions.
    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }

    // Build and broadcast all 11 Phase 4 messages to sessions[0].
    for (int s = 0; s < 11; s++) {
        PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[s], key_map.at(PtxOf(sessions, s)));
        BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[0], msg));
    }

    BOOST_CHECK(PTX_DKG_ClosePhase4(sessions[0]));
    BOOST_CHECK(sessions[0].phase == PTXDKGPhase::FINALIZE);
}

// ---------------------------------------------------------------------------
// P4_ClosePhase4_AbortsBelowThreshold
//
// Only 5 consistent premature commitments → ABORTED.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_ClosePhase4_AbortsBelowThreshold)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }

    // Deliver only 5 messages to sessions[0].
    for (int s = 1; s <= 5; s++) {
        PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[s], key_map.at(PtxOf(sessions, s)));
        BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[0], msg));
    }

    BOOST_CHECK(!PTX_DKG_ClosePhase4(sessions[0]));
    BOOST_CHECK(sessions[0].phase == PTXDKGPhase::ABORTED);
}

// ---------------------------------------------------------------------------
// P4_ClosePhase4_InconsistentMsgsNotCounted
//
// 4 consistent + 2 with differing group_pk_bytes → 4 < t=6 → ABORTED.
// The 2 differing messages are accepted (they decompress) but not counted.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(P4_ClosePhase4_InconsistentMsgsNotCounted)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToPremit(key_map, sessions);

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }

    // Deliver 4 honest messages (consistent group_pk) to sessions[0].
    for (int s = 1; s <= 4; s++) {
        PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[s], key_map.at(PtxOf(sessions, s)));
        BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[0], msg));
    }

    // Deliver 2 messages with a different (but valid) group_pk_bytes.
    // Construct them from sessions that have a bad member artificially added so
    // their group_pk differs from sessions[0]'s.
    for (int s = 5; s <= 6; s++) {
        // Give sessions[s] a bad member that sessions[0] does not have,
        // forcing a different group_pk.
        uint256 extra_bad = PtxOf(sessions, (s == 5) ? 9 : 10);
        sessions[s].bad_members.insert(extra_bad);
        // Recompute with the extra bad member.
        sessions[s].phase4_computed = false;
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[s]));

        PTXDKGPhase4Msg msg = PTX_DKG_BuildPhase4Msg(sessions[s], key_map.at(PtxOf(sessions, s)));
        // The message carries a different group_pk — still decompresses, so accepted.
        BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[0], msg));
    }

    // sessions[0] received 6 messages total; only 4 are consistent with its own group_pk.
    BOOST_CHECK_EQUAL(sessions[0].phase4_premit_msgs.size(), 6u);
    BOOST_CHECK(!PTX_DKG_ClosePhase4(sessions[0]));
    BOOST_CHECK(sessions[0].phase == PTXDKGPhase::ABORTED);
}

BOOST_AUTO_TEST_SUITE_END()
