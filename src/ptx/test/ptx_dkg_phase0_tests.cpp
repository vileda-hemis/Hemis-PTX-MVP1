// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.2-P0: Phase 0 (HASH_COMMIT) unit tests.
// Direct-call tests — no daemon, no P2P.  P2P dispatch is tested at W1.2-integration.
//
// Falsification discipline: every acceptance test is paired with a mutation that
// changes exactly one thing and verifies the result changes.
//
// T0-HARNESS proves VerifyInsecure actually rejects a corrupted sig (the
// primitive works).  T0-15 proves ReceivePhase0Msg is wired to it (the receive
// path uses the primitive — bad sig through the receive path is rejected).
// Together they give complete sig-check coverage without needing a manual stub.
//
// Test inventory:
//   T0-HARNESS  VerifyInsecure rejects a corrupted sig (harness integrity)
//   T0-1        score: deterministic and matches CSHA256(inner||fbh)
//   T0-2        score: different inner hash → different score
//   T0-3        InnerHash matches CDGMState::UpdateConfirmedHash formula
//   T0-4        SortMembers: non-increasing score, share_index 1..11
//   T0-5        SortMembers: share_index 1 is the highest-score member
//   T0-6        SortMembers: rejects null confirmedHash (KDD-052 precondition)
//   T0-7        InitSession: rejects wrong member count
//   T0-8        InitSession: non-member observer (my_idx -1) succeeds
//   T0-9        GenerateLocalContrib: correct sizes, non-null commitment
//   T0-10       GenerateLocalContrib: rejects when not a member
//   T0-11       Commitment: recomputing from same vvec gives byte-identical hash
//   T0-12       Commitment: flipping one vvec byte changes the hash (vvec binding)
//   T0-13       Commitment: changing quorum_hash changes the hash (replay defence)
//   T0-14       ReceivePhase0Msg: accepts a valid message
//   T0-15       ReceivePhase0Msg: rejects bad sig  ← load-bearing falsification
//   T0-16       ReceivePhase0Msg: rejects unknown member
//   T0-17       ReceivePhase0Msg: rejects duplicate
//   T0-18       ReceivePhase0Msg: rejects wrong quorum_hash
//   T0-19       IsPhase0Complete: false at 10, true at 11
//   T0-20       ClosePhase0: full QUAL → CONTRIB, qual size 11, commits accessible
//   T0-21       ClosePhase0: |QUAL|=t-1 → ABORTED; |QUAL|=t → CONTRIB (boundary)

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"

#include "arith_uint256.h"
#include "bls/bls_wrapper.h"
#include "crypto/sha256.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_phase0_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Build 11 PTXDKGMember structs with distinct identities and operator keys.
// key_map is keyed by proTxHash so lookups survive the sort inside InitSession.
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

// Build a signed Phase0Msg for session.members[member_idx] using key_map.
// commitment is a per-member dummy value unless override_commitment is non-null.
static PTXDKGPhase0Msg BuildValidPhase0Msg(
    const PTXDKGSession& session,
    int member_idx,
    const std::map<uint256, CBLSSecretKey>& key_map,
    const uint256& override_commitment = uint256())
{
    const PTXDKGMember& m = session.members[member_idx];
    PTXDKGPhase0Msg msg;
    msg.quorum_hash = session.quorum_hash;
    msg.proTxHash   = m.proTxHash;

    if (override_commitment.IsNull()) {
        std::vector<unsigned char> buf(32, 0);
        buf[0] = (unsigned char)member_idx;
        buf[1] = 0xC0;
        msg.commitment = uint256(buf);
    } else {
        msg.commitment = override_commitment;
    }

    msg.sig = key_map.at(m.proTxHash).Sign(msg.GetSignHash());
    return msg;
}

// ---------------------------------------------------------------------------
// T0-HARNESS — VerifyInsecure rejects a corrupted sig.
// Standalone so CI names it explicitly.  If this fails, T0-15 is unreliable.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_HarnessIntegrity_BadSigRejected)
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    CBLSPublicKey pk = sk.GetPublicKey();

    std::vector<unsigned char> hbuf(32, 0);
    hbuf[0] = 0x42;
    uint256 hash(hbuf);

    CBLSSignature good = sk.Sign(hash);
    BOOST_REQUIRE(good.VerifyInsecure(pk, hash));

    std::vector<uint8_t> bytes = good.ToByteVector();
    bytes[4] ^= 0xFF;
    CBLSSignature bad;
    bad.SetByteVector(bytes);

    BOOST_REQUIRE(!bad.VerifyInsecure(pk, hash));
}

// ---------------------------------------------------------------------------
// T0-1  Score is deterministic and matches CSHA256(inner||fbh) directly
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Score_Deterministic)
{
    std::vector<unsigned char> ibuf(32, 0x11);
    std::vector<unsigned char> fbuf(32, 0x22);
    uint256 inner(ibuf), fbh(fbuf);

    uint256 s1 = PTX_DKG_ComputeMemberScore(inner, fbh);
    uint256 s2 = PTX_DKG_ComputeMemberScore(inner, fbh);
    BOOST_CHECK(s1 == s2);

    uint256 expected;
    CSHA256 h;
    h.Write(inner.begin(), inner.size());
    h.Write(fbh.begin(), fbh.size());
    h.Finalize(expected.begin());
    BOOST_CHECK(s1 == expected);
}

// ---------------------------------------------------------------------------
// T0-2  Different inner hashes produce different scores
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Score_DiffersOnDiffInput)
{
    std::vector<unsigned char> a(32, 0x11), b(32, 0x22), f(32, 0x33);
    uint256 fbh(f);
    BOOST_CHECK(PTX_DKG_ComputeMemberScore(uint256(a), fbh) !=
                PTX_DKG_ComputeMemberScore(uint256(b), fbh));
}

// ---------------------------------------------------------------------------
// T0-3  InnerHash matches CDGMState::UpdateConfirmedHash: SHA256(ptx||chash)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_InnerHash_MatchesDGMFormula)
{
    std::vector<unsigned char> pb(32, 0xAA), cb(32, 0xBB);
    uint256 ptx(pb), chash(cb);

    uint256 got = PTX_DKG_ComputeInnerHash(ptx, chash);

    uint256 expected;
    CSHA256 h;
    h.Write(ptx.begin(), ptx.size());
    h.Write(chash.begin(), chash.size());
    h.Finalize(expected.begin());

    BOOST_CHECK(got == expected);
}

// ---------------------------------------------------------------------------
// T0-4  SortMembers: non-increasing score order, share_index 1..11
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Sort_DescendingScoreOrder)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    BOOST_REQUIRE(PTX_DKG_SortMembers(members, fbh));

    for (int i = 0; i + 1 < (int)members.size(); i++) {
        arith_uint256 si = UintToArith256(PTX_DKG_ComputeMemberScore(
            members[i].confirmedHashWithProRegTxHash, fbh));
        arith_uint256 sn = UintToArith256(PTX_DKG_ComputeMemberScore(
            members[i + 1].confirmedHashWithProRegTxHash, fbh));
        BOOST_CHECK(si >= sn);
        BOOST_CHECK_EQUAL(members[i].share_index, i + 1);
    }
    BOOST_CHECK_EQUAL(members[10].share_index, 11);
}

// ---------------------------------------------------------------------------
// T0-5  share_index 1 is the member with the highest score
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Sort_IndexOneIsMaxScore)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    BOOST_REQUIRE(PTX_DKG_SortMembers(members, fbh));

    arith_uint256 top = UintToArith256(PTX_DKG_ComputeMemberScore(
        members[0].confirmedHashWithProRegTxHash, fbh));
    for (int i = 1; i < 11; i++) {
        arith_uint256 s = UintToArith256(PTX_DKG_ComputeMemberScore(
            members[i].confirmedHashWithProRegTxHash, fbh));
        BOOST_CHECK(top >= s);
    }
}

// ---------------------------------------------------------------------------
// T0-6  SortMembers rejects null confirmedHash (KDD-052 precondition)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Sort_RejectsNullConfirmedHash)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    members[3].confirmedHash = uint256();
    BOOST_CHECK(!PTX_DKG_SortMembers(members, fbh));
}

// ---------------------------------------------------------------------------
// T0-7  InitSession rejects wrong member count
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Init_RejectsWrongCount)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    members.pop_back(); // 10 members
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_CHECK(!PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
}

// ---------------------------------------------------------------------------
// T0-8  InitSession: my_proTxHash not in list → my_idx -1, session still valid
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Init_NonMemberObserverValid)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    std::vector<unsigned char> xb(32, 0xFF);
    PTXDKGSession session;
    BOOST_CHECK(PTX_DKG_InitSession(session, members, fbh, uint256(xb)));
    BOOST_CHECK_EQUAL(session.my_idx, -1);
    BOOST_CHECK(session.phase == PTXDKGPhase::HASH_COMMIT);
}

// ---------------------------------------------------------------------------
// T0-9  GenerateLocalContrib: correct sizes, non-null commitment
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_GenerateContrib_Sizes)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
    BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(session));

    const auto& c = session.local_contrib;
    BOOST_CHECK_EQUAL(c.coeffs.size(), 6u);   // t=6 (KDD-048)
    BOOST_CHECK_EQUAL(c.vvec.size(),   6u);
    BOOST_CHECK_EQUAL(c.evals.size(), 11u);   // n=11
    BOOST_CHECK(!c.commitment.IsNull());
}

// ---------------------------------------------------------------------------
// T0-10  GenerateLocalContrib rejects when node is not a session member
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_GenerateContrib_RejectsNonMember)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    std::vector<unsigned char> xb(32, 0xFF);
    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, uint256(xb)));
    BOOST_CHECK_EQUAL(session.my_idx, -1);
    BOOST_CHECK(!PTX_DKG_GenerateLocalContrib(session));
}

// ---------------------------------------------------------------------------
// T0-11  Commitment: recomputing from the same vvec gives byte-identical hash
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Commitment_Deterministic)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
    BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(session));

    const uint256& qh     = session.quorum_hash;
    const uint256& my_ptx = session.members[session.my_idx].proTxHash;
    const auto&    vvec   = session.local_contrib.vvec;

    uint256 recomputed;
    CSHA256 h;
    h.Write(qh.begin(), qh.size());
    h.Write(my_ptx.begin(), my_ptx.size());
    for (const auto& pt : vvec) {
        uint8_t buf[48];
        blst_p1_affine_compress(buf, &pt);
        h.Write(buf, 48);
    }
    h.Finalize(recomputed.begin());

    BOOST_CHECK(session.local_contrib.commitment == recomputed);
}

// ---------------------------------------------------------------------------
// T0-12  Commitment: flipping one vvec byte changes the hash (vvec binding)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Commitment_BindsVvec)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
    BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(session));

    const uint256& qh     = session.quorum_hash;
    const uint256& my_ptx = session.members[session.my_idx].proTxHash;
    const auto&    vvec   = session.local_contrib.vvec;

    uint256 tampered;
    CSHA256 h;
    h.Write(qh.begin(), qh.size());
    h.Write(my_ptx.begin(), my_ptx.size());
    for (int k = 0; k < (int)vvec.size(); k++) {
        uint8_t buf[48];
        blst_p1_affine_compress(buf, &vvec[k]);
        if (k == 0) buf[4] ^= 0x01;
        h.Write(buf, 48);
    }
    h.Finalize(tampered.begin());

    BOOST_CHECK(session.local_contrib.commitment != tampered);
}

// ---------------------------------------------------------------------------
// T0-13  Commitment: changing quorum_hash changes the hash (replay defence)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Commitment_BindsQuorumHash)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
    BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(session));

    const uint256& my_ptx = session.members[session.my_idx].proTxHash;
    const auto&    vvec   = session.local_contrib.vvec;

    std::vector<unsigned char> other_buf(32, 0xDE);
    uint256 other_qh(other_buf);

    uint256 with_other_qh;
    CSHA256 h;
    h.Write(other_qh.begin(), other_qh.size());
    h.Write(my_ptx.begin(), my_ptx.size());
    for (const auto& pt : vvec) {
        uint8_t buf[48];
        blst_p1_affine_compress(buf, &pt);
        h.Write(buf, 48);
    }
    h.Finalize(with_other_qh.begin());

    BOOST_CHECK(session.local_contrib.commitment != with_other_qh);
}

// ---------------------------------------------------------------------------
// T0-14  ReceivePhase0Msg: accepts a valid message
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Receive_AcceptsValid)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));

    PTXDKGPhase0Msg msg = BuildValidPhase0Msg(session, 3, key_map);
    BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(session, msg));

    const uint256& ptx = session.members[3].proTxHash;
    BOOST_CHECK_EQUAL(session.phase0_commits.count(ptx), 1u);
    BOOST_CHECK(session.phase0_commits.at(ptx) == msg.commitment);
}

// ---------------------------------------------------------------------------
// T0-15  ReceivePhase0Msg: rejects bad signature  ← load-bearing falsification
//        Proves ReceivePhase0Msg is wired to VerifyInsecure (T0-HARNESS proves
//        the primitive; T0-15 proves the receive path uses it).
//        Must reject both: return false AND not store the commitment.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Receive_RejectsBadSig)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));

    PTXDKGPhase0Msg msg = BuildValidPhase0Msg(session, 3, key_map);

    std::vector<uint8_t> bytes = msg.sig.ToByteVector();
    bytes[0] ^= 0xFF;
    msg.sig.SetByteVector(bytes);

    BOOST_CHECK(!PTX_DKG_ReceivePhase0Msg(session, msg));
    BOOST_CHECK_EQUAL(session.phase0_commits.count(session.members[3].proTxHash), 0u);
}

// ---------------------------------------------------------------------------
// T0-16  ReceivePhase0Msg: rejects message from unknown member
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Receive_RejectsUnknownMember)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));

    CBLSSecretKey outsider_sk;
    outsider_sk.MakeNewKey();

    std::vector<unsigned char> xb(32, 0xFF);
    PTXDKGPhase0Msg msg;
    msg.quorum_hash = session.quorum_hash;
    msg.proTxHash   = uint256(xb);
    msg.commitment  = uint256(xb);
    msg.sig         = outsider_sk.Sign(msg.GetSignHash());

    BOOST_CHECK(!PTX_DKG_ReceivePhase0Msg(session, msg));
}

// ---------------------------------------------------------------------------
// T0-17  ReceivePhase0Msg: rejects duplicate from the same member
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Receive_RejectsDuplicate)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));

    PTXDKGPhase0Msg msg = BuildValidPhase0Msg(session, 5, key_map);
    BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(session, msg));   // first: accepted
    BOOST_CHECK(!PTX_DKG_ReceivePhase0Msg(session, msg));  // duplicate: rejected

    BOOST_CHECK_EQUAL(session.phase0_commits.count(session.members[5].proTxHash), 1u);
}

// ---------------------------------------------------------------------------
// T0-18  ReceivePhase0Msg: rejects wrong quorum_hash
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Receive_RejectsWrongQuorumHash)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));

    const PTXDKGMember& m = session.members[2];
    std::vector<unsigned char> ob(32, 0xDE);
    PTXDKGPhase0Msg msg;
    msg.quorum_hash = uint256(ob);      // wrong
    msg.proTxHash   = m.proTxHash;
    msg.commitment  = uint256(ob);
    msg.sig         = key_map.at(m.proTxHash).Sign(msg.GetSignHash());

    BOOST_CHECK(!PTX_DKG_ReceivePhase0Msg(session, msg));
}

// ---------------------------------------------------------------------------
// T0-19  IsPhase0Complete: false at 10, true at 11
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_IsComplete_FalseAt10_TrueAt11)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));

    for (int i = 0; i < 10; i++) {
        BOOST_CHECK(!PTX_DKG_IsPhase0Complete(session));
        BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(session, BuildValidPhase0Msg(session, i, key_map)));
    }
    BOOST_CHECK(!PTX_DKG_IsPhase0Complete(session));

    BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(session, BuildValidPhase0Msg(session, 10, key_map)));
    BOOST_CHECK(PTX_DKG_IsPhase0Complete(session));
}

// ---------------------------------------------------------------------------
// T0-20  ClosePhase0: full QUAL → CONTRIB, 11 entries in qual, commits accessible
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_ClosePhase0_FullQualSucceeds)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));

    for (int i = 0; i < 11; i++)
        BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(session, BuildValidPhase0Msg(session, i, key_map)));

    BOOST_CHECK(PTX_DKG_ClosePhase0(session));
    BOOST_CHECK(session.phase == PTXDKGPhase::CONTRIB);
    BOOST_CHECK_EQUAL(session.qual.size(), 11u);

    // Every QUAL member's commitment is still accessible for Phase 1 reveal verification.
    for (const auto& ptx : session.qual)
        BOOST_CHECK_EQUAL(session.phase0_commits.count(ptx), 1u);
}

// ---------------------------------------------------------------------------
// T0-21  ClosePhase0: |QUAL|=t-1 → ABORTED; |QUAL|=t → CONTRIB (boundary)
//        The pairing proves the floor is at exactly t, not t-1 or t+1.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_ClosePhase0_SubThresholdAborts)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    // |QUAL| = t-1 = 5 → must abort.
    {
        PTXDKGSession session;
        BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
        for (int i = 0; i < 5; i++)
            BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(session,
                BuildValidPhase0Msg(session, i, key_map)));
        BOOST_CHECK(!PTX_DKG_ClosePhase0(session));
        BOOST_CHECK(session.phase == PTXDKGPhase::ABORTED);
    }

    // |QUAL| = t = 6 → must succeed.  Boundary is exactly t.
    {
        PTXDKGSession session;
        BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
        for (int i = 0; i < 6; i++)
            BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(session,
                BuildValidPhase0Msg(session, i, key_map)));
        BOOST_CHECK(PTX_DKG_ClosePhase0(session));
        BOOST_CHECK(session.phase == PTXDKGPhase::CONTRIB);
        BOOST_CHECK_EQUAL(session.qual.size(), 6u);
    }
}

BOOST_AUTO_TEST_SUITE_END()
