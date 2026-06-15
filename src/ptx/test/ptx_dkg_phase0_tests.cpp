// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.3-P0 (rebase): Phase 0 (HASH_COMMIT) unit tests — KDD-060 migration.
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
//   T0-1        score: CalculateScores deterministic (two calls produce identical results)
//   T0-2        score: different identities → different CalculateScores results
//   T0-3        InnerHash matches CDGMState::UpdateConfirmedHash formula
//   T0-4        CalculateQuorum order: position+1 == share_index; proTxHash binding to quorum
//   T0-5        CalculateQuorum[0] is the member with the highest score
//   T0-6        InitSession: rejects null confirmedHash (KDD-060 precondition)
//   T0-7        InitSession: rejects wrong member count
//   T0-8        InitSession: non-member observer (my_idx -1) succeeds
//   T0-9        GenerateLocalContrib: correct sizes, non-null commitment
//   T0-10       GenerateLocalContrib: rejects when not a member
//   T0-11       Commitment: recomputing from same vvec gives byte-identical hash
//   T0-12       Commitment: flipping one vvec byte changes the hash (vvec binding)
//   T0-13       Commitment: changing quorum_hash changes the hash (replay defence)
//   T0-14       ReceivePhase0Msg: accepts a valid message
//   T0-15       ReceivePhase0Msg: rejects bad sig  <- load-bearing falsification
//   T0-16       ReceivePhase0Msg: rejects unknown member
//   T0-17       ReceivePhase0Msg: rejects duplicate
//   T0-18       ReceivePhase0Msg: rejects wrong quorum_hash
//   T0-19       IsPhase0Complete: false at 10, true at 11
//   T0-20       ClosePhase0: full QUAL -> CONTRIB, qual size 11, commits accessible
//   T0-21       ClosePhase0: |QUAL|=t-1 -> ABORTED; |QUAL|=t -> CONTRIB (boundary)
//   T0-TIE      CalculateQuorum tie-break: larger collateralOutpoint -> index 0

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"
#include "evo/deterministicgms.h"

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
// key_map is keyed by proTxHash so lookups survive the order changes in InitSession.
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

// Build a CDeterministicGMList from a PTXDKGMember vector + matching BLS key_map.
// UpdateConfirmedHash is called from (proTxHash, confirmedHash) so that
// confirmedHashWithProRegTxHash in the state matches the formula used by
// CalculateScores -- if PTX_DKG_ComputeInnerHash and UpdateConfirmedHash ever
// diverge, CalculateScores disagrees with MakeTestMembers expectations and
// tests fail here, not silently (KDD-060 §20.3 guard).
static CDeterministicGMList MakeTestDGMList(
    const std::vector<PTXDKGMember>& members,
    const std::map<uint256, CBLSSecretKey>& key_map)
{
    CDeterministicGMList list;
    for (uint64_t i = 0; i < (uint64_t)members.size(); ++i) {
        const PTXDKGMember& m = members[i];
        auto dgm = std::make_shared<CDeterministicGM>(i);
        dgm->proTxHash          = m.proTxHash;
        dgm->collateralOutpoint = COutPoint(m.proTxHash, 0);
        auto state = std::make_shared<CDeterministicGMState>();
        // UpdateConfirmedHash recomputes confirmedHashWithProRegTxHash from
        // (proTxHash, confirmedHash). NOT copying m.confirmedHashWithProRegTxHash
        // -- if ComputeInnerHash and UpdateConfirmedHash ever diverge,
        // CalculateScores disagrees with MakeTestMembers expectations and the
        // tests fail here, not silently pass (KDD-060 §20.3 guard).
        state->UpdateConfirmedHash(m.proTxHash, m.confirmedHash);
        state->pubKeyOperator.Set(key_map.at(m.proTxHash).GetPublicKey());
        uint160 k20; memcpy(k20.begin(), m.proTxHash.begin(), 20);
        state->keyIDOwner  = CKeyID(k20);
        state->keyIDVoting = state->keyIDOwner;
        // Deterministic unique non-empty node_id so PTX_DKG_IsGMPTXEligible passes.
        // Full 64-char hex: GetHex() is big-endian so the varying low bytes (buf[0]=i)
        // appear only in the last 4 chars — substr(0,8) would collide for all 11 members.
        state->node_id = "gm:" + m.proTxHash.GetHex();
        dgm->pdgmState = state;
        list.AddGM(dgm);
    }
    return list;
}

// ---------------------------------------------------------------------------
// T0-HARNESS -- VerifyInsecure rejects a corrupted sig.
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
// T0-1  CalculateScores is deterministic: two calls on the same list + modifier
//       return identical results (same size, same per-entry score and proTxHash).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Score_Deterministic)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();
    CDeterministicGMList list = MakeTestDGMList(members, key_map);

    auto s1 = list.CalculateScores(fbh);
    auto s2 = list.CalculateScores(fbh);

    BOOST_REQUIRE_EQUAL(s1.size(), s2.size());
    for (size_t i = 0; i < s1.size(); i++) {
        BOOST_CHECK(s1[i].first  == s2[i].first);
        BOOST_CHECK(s1[i].second->proTxHash == s2[i].second->proTxHash);
    }
}

// ---------------------------------------------------------------------------
// T0-2  Different GM identities produce different CalculateScores results.
//       Two single-member lists, one per distinct member; assert scores differ.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Score_DiffersOnDiffInput)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto all = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    // Single-member lists from two distinct members in the MakeTestMembers set.
    CDeterministicGMList list1 = MakeTestDGMList({all[0]}, key_map);
    CDeterministicGMList list2 = MakeTestDGMList({all[1]}, key_map);

    auto s1 = list1.CalculateScores(fbh);
    auto s2 = list2.CalculateScores(fbh);

    BOOST_REQUIRE_EQUAL(s1.size(), 1u);
    BOOST_REQUIRE_EQUAL(s2.size(), 1u);
    BOOST_CHECK(s1[0].first != s2[0].first);
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
// T0-4  CalculateQuorum order contract: BuildMemberVectorFromList output fed
//       into InitSession -> members[i].share_index == i+1 and
//       members[i].proTxHash == quorum[i]->proTxHash (Amendment B).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Sort_DescendingScoreOrder)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members_in = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();
    CDeterministicGMList list = MakeTestDGMList(members_in, key_map);

    // Canonical quorum order from deterministicgms -- the sole ordering authority.
    auto quorum = list.CalculateQuorum(11, fbh);
    BOOST_REQUIRE_EQUAL(quorum.size(), 11u);

    // Build member vector via the new helper and init a session.
    auto member_vec = PTX_DKG_BuildMemberVectorFromList(list, fbh);
    BOOST_REQUIRE_EQUAL(member_vec.size(), 11u);

    PTXDKGSession session;
    BOOST_REQUIRE(PTX_DKG_InitSession(session, member_vec, fbh, member_vec[0].proTxHash));

    // Contract glue (KDD-060): position+1 == share_index; proTxHash order matches quorum.
    for (int i = 0; i < 11; i++) {
        BOOST_CHECK_EQUAL(session.members[i].share_index, i + 1);
        BOOST_CHECK(session.members[i].proTxHash == quorum[i]->proTxHash);
    }
}

// ---------------------------------------------------------------------------
// T0-5  CalculateQuorum[0] (share_index 1) has the highest score among all GMs.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Sort_IndexOneIsMaxScore)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members_in = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();
    CDeterministicGMList list = MakeTestDGMList(members_in, key_map);

    auto quorum = list.CalculateQuorum(11, fbh);
    BOOST_REQUIRE_EQUAL(quorum.size(), 11u);

    auto scores = list.CalculateScores(fbh);
    BOOST_REQUIRE(!scores.empty());

    // Find the maximum score across all GMs.
    arith_uint256 maxScore(0);
    for (size_t i = 0; i < scores.size(); i++) {
        if (scores[i].first > maxScore)
            maxScore = scores[i].first;
    }

    // quorum[0] must be the GM carrying that maximum score.
    bool found = false;
    for (size_t i = 0; i < scores.size(); i++) {
        if (scores[i].second->proTxHash == quorum[0]->proTxHash) {
            BOOST_CHECK(scores[i].first == maxScore);
            found = true;
            break;
        }
    }
    BOOST_CHECK(found);
}

// ---------------------------------------------------------------------------
// T0-6  InitSession rejects null confirmedHash (KDD-060 precondition).
//       Migrated from SortMembers -- InitSession now owns this guard.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Sort_RejectsNullConfirmedHash)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    members[3].confirmedHash = uint256();  // null confirmedHash
    PTXDKGSession session;
    BOOST_CHECK(!PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
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
// T0-8  InitSession: my_proTxHash not in list -> my_idx -1, session still valid
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
// T0-15  ReceivePhase0Msg: rejects bad signature  <- load-bearing falsification
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
// T0-20  ClosePhase0: full QUAL -> CONTRIB, 11 entries in qual, commits accessible
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
// T0-21  ClosePhase0: |QUAL|=t-1 -> ABORTED; |QUAL|=t -> CONTRIB (boundary)
//        The pairing proves the floor is at exactly t, not t-1 or t+1.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_ClosePhase0_SubThresholdAborts)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    // |QUAL| = t-1 = 5 -> must abort.
    {
        PTXDKGSession session;
        BOOST_REQUIRE(PTX_DKG_InitSession(session, members, fbh, members[0].proTxHash));
        for (int i = 0; i < 5; i++)
            BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(session,
                BuildValidPhase0Msg(session, i, key_map)));
        BOOST_CHECK(!PTX_DKG_ClosePhase0(session));
        BOOST_CHECK(session.phase == PTXDKGPhase::ABORTED);
    }

    // |QUAL| = t = 6 -> must succeed.  Boundary is exactly t.
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

// ---------------------------------------------------------------------------
// T0-TIE  CalculateQuorum tie-break: two GMs with a forced score tie
//         (identical confirmedHashWithProRegTxHash => identical SHA256(inner||fbh))
//         are broken by collateralOutpoint ascending under the rbegin/rend sort,
//         so the LARGER collateral lands at index 0 (share_index 1).
//
// The confirmedHashWithProRegTxHash direct-set here is the ONE legitimate
// use of that pattern in this test file.  It is justified because:
//   (a) A score tie cannot arise from natural UpdateConfirmedHash inputs on
//       two distinct GMs (distinct proTxHashes produce distinct SHA256 outputs
//       with overwhelming probability), so the tie can ONLY be forced by
//       bypassing UpdateConfirmedHash after the fact.
//   (b) The purpose of this test is precisely to pin the tie-break comparator
//       behaviour against upstream drift -- the override is the mechanism, not
//       a measurement.
// This is NOT the Amendment-D-forbidden copy-across that governs T0-1/2/4/5:
// those tests must derive expectations from CalculateScores/CalculateQuorum
// alone, without hand-rolling the hash chain.  Here there is no hash chain
// to roll -- we are manufacturing an input condition, not predicting an output.
//
// UpdateConfirmedHash is still called first so confirmedHash is non-null
// (CalculateScores skips null-confirmedHash GMs).  The override affects only
// confirmedHashWithProRegTxHash, not confirmedHash.  AddGM does not recompute
// or clobber confirmedHashWithProRegTxHash (verified: deterministicgms.cpp
// AddGM body only calls gmMap.set, gmInternalIdMap.set, and AddUniqueProperty
// on collateral/keyIDOwner/pubKeyOperator -- pdgmState fields are untouched).
//
// Prediction (Amendment C): uint256::operator< uses memcmp on raw m_data bytes
// (uint256.h:49,53); 0xBB-filled > 0xAA-filled byte-for-byte => bigProTx is the
// larger collateral => quorum[0]->proTxHash == bigProTx.
// If this assertion fails on first run, record the actual deterministic order,
// pin assertions to it, and report the corrected prediction.  Do NOT modify
// deterministicgms.cpp.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_TieBreak_CollateralDeterminesOrder)
{
    uint256 fbh = TestFormationHash();

    // Shared confirmedHashWithProRegTxHash -- forces identical SHA256(inner||fbh) scores.
    std::vector<unsigned char> tieBuf(32, 0x77);
    uint256 tieInner(tieBuf);

    // DGM with larger collateral hash (0xBB... > 0xAA... under memcmp).
    std::vector<unsigned char> bigBuf(32, 0xBB);
    uint256 bigProTx(bigBuf);
    std::vector<unsigned char> bigConf(32, 0x11);

    // DGM with smaller collateral hash.
    std::vector<unsigned char> smBuf(32, 0xAA);
    uint256 smProTx(smBuf);
    std::vector<unsigned char> smConf(32, 0x11);

    CDeterministicGMList list;

    auto addGM = [&](uint64_t id, const uint256& proTxHash, const uint256& confHash) {
        CBLSSecretKey sk;
        sk.MakeNewKey();

        auto dgm = std::make_shared<CDeterministicGM>(id);
        dgm->proTxHash          = proTxHash;
        dgm->collateralOutpoint = COutPoint(proTxHash, 0);

        auto state = std::make_shared<CDeterministicGMState>();
        // Call UpdateConfirmedHash to set confirmedHash non-null (required by
        // CalculateScores), then override confirmedHashWithProRegTxHash to
        // tieInner to force identical scores.  See block comment above.
        state->UpdateConfirmedHash(proTxHash, confHash);
        state->confirmedHashWithProRegTxHash = tieInner;

        state->pubKeyOperator.Set(sk.GetPublicKey());
        uint160 k20;
        memcpy(k20.begin(), proTxHash.begin(), 20);
        state->keyIDOwner  = CKeyID(k20);
        state->keyIDVoting = state->keyIDOwner;
        dgm->pdgmState = state;
        list.AddGM(dgm);
    };

    addGM(0, bigProTx, uint256(bigConf));
    addGM(1, smProTx,  uint256(smConf));

    auto quorum = list.CalculateQuorum(2, fbh);
    BOOST_REQUIRE_EQUAL(quorum.size(), 2u);

    // Prediction: larger collateral (bigProTx, 0xBB-filled) -> index 0.
    BOOST_CHECK(quorum[0]->proTxHash == bigProTx);
    BOOST_CHECK(quorum[1]->proTxHash == smProTx);
}

// ---------------------------------------------------------------------------
// T0-ELIG  Eligibility filter: empty-node_id interloper is excluded from the
//          CalculateQuorum result even when 11 eligible GMs exist alongside it.
//
// Falsification of PTX_DKG_IsGMPTXEligible / BuildMemberVectorFromList filter:
// if the filter were absent, the interloper could displace an eligible GM and
// corrupt the quorum.  The test proves the interloper never appears in the result.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase0_Eligibility_EmptyNodeIdExcluded)
{
    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    uint256 fbh = TestFormationHash();

    // Build a list with the 11 eligible GMs PLUS one interloper with empty node_id.
    // The interloper has internalId=11 (distinct from 0..10) and a unique proTxHash.
    CDeterministicGMList list = MakeTestDGMList(members, key_map);

    CBLSSecretKey interloper_sk;
    interloper_sk.MakeNewKey();
    std::vector<unsigned char> ib(32, 0xCC);
    uint256 interloper_ptx(ib);

    auto interloper_dgm = std::make_shared<CDeterministicGM>(uint64_t(11));
    interloper_dgm->proTxHash          = interloper_ptx;
    interloper_dgm->collateralOutpoint = COutPoint(interloper_ptx, 0);
    auto interloper_state = std::make_shared<CDeterministicGMState>();
    std::vector<unsigned char> cb(32, 0xDD);
    interloper_state->UpdateConfirmedHash(interloper_ptx, uint256(cb));
    interloper_state->pubKeyOperator.Set(interloper_sk.GetPublicKey());
    uint160 ik20; memcpy(ik20.begin(), interloper_ptx.begin(), 20);
    interloper_state->keyIDOwner  = CKeyID(ik20);
    interloper_state->keyIDVoting = interloper_state->keyIDOwner;
    // node_id intentionally left empty — this GM must be excluded by the filter.
    interloper_dgm->pdgmState = interloper_state;
    list.AddGM(interloper_dgm);

    // BuildMemberVectorFromList must return exactly 11 entries (the eligible GMs)
    // and the interloper must not appear in the result.
    auto member_vec = PTX_DKG_BuildMemberVectorFromList(list, fbh);
    BOOST_REQUIRE_EQUAL(member_vec.size(), 11u);

    for (const auto& m : member_vec) {
        BOOST_CHECK(m.proTxHash != interloper_ptx);
        BOOST_CHECK(!m.node_id.empty());
    }
}

BOOST_AUTO_TEST_SUITE_END()
