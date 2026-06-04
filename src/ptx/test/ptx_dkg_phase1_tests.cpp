// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.2-P1: Phase 1 (CONTRIB / contribution reveal) unit tests.
// Direct-call tests — no daemon, no P2P.  P2P dispatch tested at W1.2-integration.
//
// Falsification discipline: every acceptance test is paired with a mutation that
// changes exactly one thing and verifies the result changes.
//
// Two load-bearing falsification cycles (Correction A):
//   Cycle 1: sig check stubbed to always-pass → T1-5 RED, T1-16 RED, T1-4 GREEN.
//   Cycle 2: commitment check stubbed to always-pass → T1-6 RED, T1-17 RED, T1-4 GREEN.
//   Two tests redden per cycle: this is correct, not a regression.
//   T1-16 joins Cycle 1 because blob-binding is only caught by the sig check —
//     stubbing the sig un-catches a tampered blob, which IS the binding property.
//   T1-17 joins Cycle 2 because step-1 of that test needs the commitment check to
//     mark the member bad; stubbing it breaks the monotonic-bad property.
//
// T1-HARNESS proves CBLSIESMultiRecipientBlobs per-recipient key distinctness:
//   same ephemeral key + ivSeed, distinct recipient pubkeys → distinct AES keys →
//   different ciphertexts; cross-decrypt produces wrong bytes.
//
// T1-11 (wrong-sk decrypt): Decrypt() returns TRUE with 32 bytes of AES garbage
//   (AES-CBC no-padding always "succeeds"); the test asserts the stored scalar ≠
//   expected, NOT that the function returned false.  Share validity requires the
//   Phase 2 Feldman check; see DecryptMyShare header comment.
//
// T1-17 (bad-then-good resend): regression guard for bad_members monotonicity
//   (FIX 1).  A member marked bad cannot clear its status by re-sending a valid
//   message.  Without the bad_members early-reject added by FIX 1 the second
//   receive returns true and stores the vvec — this test goes RED on unfixed code.
//
// Test inventory:
//   T1-HARNESS  IES per-recipient key distinctness (blobs differ; cross-decrypt wrong)
//   T1-1        BuildPhase1Msg: vvec.size()==t=6; blobs.size()==n=11; sig valid
//   T1-2        BuildPhase1Msg: vvec recomputes to stored phase0_commits value
//   T1-3        BuildPhase1Msg falsification: mutated vvec recomputes to different hash
//   T1-4        ReceivePhase1Msg: accepts valid message; stores vvec + encrypted blob
//   T1-5        ReceivePhase1Msg: rejects bad sig  ← load-bearing falsification (Cycle 1)
//   T1-6        ReceivePhase1Msg: rejects commitment mismatch → marks bad  ← GJKR (Cycle 2)
//   T1-7        ReceivePhase1Msg: rejects non-QUAL sender; does NOT mark bad
//   T1-8        ReceivePhase1Msg: rejects wrong quorum_hash
//   T1-9        ReceivePhase1Msg: rejects duplicate
//   T1-10       DecryptMyShare: correct sk → stored scalar == sender's eval[receiver_my_idx]
//   T1-11       DecryptMyShare: wrong sk → stored scalar ≠ expected (not "returns false")
//   T1-12       IsPhase1Complete: false at |QUAL|-1 reveals, true at |QUAL|
//   T1-13       ClosePhase1: all revealed → COMPLAINT; bad_members empty
//   T1-14       ClosePhase1: one non-revealer → bad_members contains it; COMPLAINT (≥t remaining)
//   T1-15       ClosePhase1 boundary: |revealed|=t-1=5 → ABORTED; |revealed|=t=6 → COMPLAINT
//   T1-16       GetSignHash blob-binding: flipped blob byte → sig fails → message rejected
//   T1-17       bad-then-good resend stays rejected (FIX 1 regression guard)

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"

#include "arith_uint256.h"
#include "bls/bls_ies.h"
#include "bls/bls_wrapper.h"
#include "crypto/sha256.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_phase1_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers
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

// SetupFullPhase0Sessions: runs all 11 members through a complete Phase 0.
// Returns 11 sessions all in CONTRIB phase with qual == all 11 members.
// key_map is keyed by proTxHash (survives the score-based sort inside InitSession).
// All sessions share the same sorted members[] — only my_idx differs.
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

    // Build Phase 0 msgs using the real commitments from GenerateLocalContrib.
    std::vector<PTXDKGPhase0Msg> p0msgs(11);
    for (int i = 0; i < 11; i++) {
        const uint256& my_ptx = sessions[i].members[sessions[i].my_idx].proTxHash;
        p0msgs[i] = PTX_DKG_BuildPhase0Msg(sessions[i], key_map.at(my_ptx));
    }

    // All sessions receive all Phase 0 msgs including self-receive.
    for (int recv = 0; recv < 11; recv++)
        for (int sender = 0; sender < 11; sender++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase0Msg(sessions[recv], p0msgs[sender]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase0(sessions[i]));
}

// Build a real Phase 1 message from sender_session's perspective.
static PTXDKGPhase1Msg BuildRealPhase1Msg(
    const PTXDKGSession& sender_session,
    const std::map<uint256, CBLSSecretKey>& key_map)
{
    const uint256& sender_ptx = sender_session.members[sender_session.my_idx].proTxHash;
    return PTX_DKG_BuildPhase1Msg(sender_session, key_map.at(sender_ptx));
}

// ---------------------------------------------------------------------------
// T1-HARNESS — IES per-recipient key distinctness.
//
// Same 32-byte plaintext P encrypted to two distinct recipients A and B under
// the same ephemeral key + ivSeed.  Demonstrates:
//   (a) blobs[0] != blobs[1]  — same input, different AES keys → different ciphertext
//   (b) Decrypt(0, skA) recovers P  — correct key works for slot 0
//   (c) Decrypt(1, skB) recovers P  — correct key works for slot 1
//   (d) Decrypt(0, skB) produces wrong bytes  — cross-decrypt fails
//
// The common ivSeed is safe: per-recipient ECDH gives each slot a distinct AES key.
// If this fails, T1-10 and T1-11 are unreliable.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_HARNESS_IESPerRecipientKeyDistinctness)
{
    CBLSSecretKey skA, skB;
    skA.MakeNewKey();
    skB.MakeNewKey();

    // Known 32-byte plaintext (2 × AES_BLOCKSIZE, no padding required).
    uint8_t plain[32];
    for (int i = 0; i < 32; i++) plain[i] = (uint8_t)(0x42 + i);
    CBLSIESMultiRecipientBlobs::Blob blobIn(plain, plain + 32);

    CBLSIESMultiRecipientBlobs enc;
    enc.InitEncrypt(2);
    BOOST_REQUIRE(enc.Encrypt(0, skA.GetPublicKey(), blobIn));
    BOOST_REQUIRE(enc.Encrypt(1, skB.GetPublicKey(), blobIn));

    // (a) Same plaintext, different recipients → different ciphertexts.
    BOOST_CHECK(enc.blobs[0] != enc.blobs[1]);

    // (b) Correct key for slot 0 recovers plaintext.
    CBLSIESMultiRecipientBlobs::Blob outA;
    BOOST_REQUIRE(enc.Decrypt(0, skA, outA));
    BOOST_REQUIRE_EQUAL(outA.size(), 32u);
    BOOST_CHECK(memcmp(outA.data(), plain, 32) == 0);

    // (c) Correct key for slot 1 recovers plaintext.
    CBLSIESMultiRecipientBlobs::Blob outB;
    BOOST_REQUIRE(enc.Decrypt(1, skB, outB));
    BOOST_REQUIRE_EQUAL(outB.size(), 32u);
    BOOST_CHECK(memcmp(outB.data(), plain, 32) == 0);

    // (d) Wrong key: skB used for slot 0 → output is not the plaintext.
    // Decrypt returns true (AES-CBC always runs); output is 32 bytes of garbage.
    CBLSIESMultiRecipientBlobs::Blob outWrong;
    enc.Decrypt(0, skB, outWrong);
    if (outWrong.size() == 32)
        BOOST_CHECK(memcmp(outWrong.data(), plain, 32) != 0);
}

// ---------------------------------------------------------------------------
// T1-1  BuildPhase1Msg: correct sizes and valid sig
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Build_Sizes)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[0], key_map);

    BOOST_CHECK_EQUAL(msg.vvec.size(), 6u);                     // t=6 (KDD-048)
    BOOST_CHECK_EQUAL(msg.encrypted_shares.blobs.size(), 11u);  // n=11 slots

    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;
    const PTXDKGMember* sender_member = nullptr;
    for (const auto& m : sessions[0].members)
        if (m.proTxHash == sender_ptx) { sender_member = &m; break; }
    BOOST_REQUIRE(sender_member != nullptr);
    BOOST_CHECK(msg.sig.VerifyInsecure(sender_member->pubKeyOperator, msg.GetSignHash()));
}

// ---------------------------------------------------------------------------
// T1-2  BuildPhase1Msg: recomputed commitment from msg.vvec matches stored value
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Build_VvecMatchesCommitment)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[0], key_map);
    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;

    uint256 recomputed;
    CSHA256 h;
    h.Write(msg.quorum_hash.begin(), msg.quorum_hash.size());
    h.Write(msg.proTxHash.begin(), msg.proTxHash.size());
    for (const auto& pt : msg.vvec) {
        uint8_t buf[48];
        blst_p1_affine_compress(buf, &pt);
        h.Write(buf, 48);
    }
    h.Finalize(recomputed.begin());

    BOOST_REQUIRE(sessions[0].phase0_commits.count(sender_ptx));
    BOOST_CHECK(recomputed == sessions[0].phase0_commits.at(sender_ptx));
}

// ---------------------------------------------------------------------------
// T1-3  BuildPhase1Msg falsification: mutated vvec recomputes to a different hash
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Build_MutatedVvecDiffersFromCommitment)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[0], key_map);
    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;

    uint256 tampered;
    CSHA256 h;
    h.Write(msg.quorum_hash.begin(), msg.quorum_hash.size());
    h.Write(msg.proTxHash.begin(), msg.proTxHash.size());
    for (int k = 0; k < (int)msg.vvec.size(); k++) {
        uint8_t buf[48];
        blst_p1_affine_compress(buf, &msg.vvec[k]);
        if (k == 0) buf[4] ^= 0x01;
        h.Write(buf, 48);
    }
    h.Finalize(tampered.begin());

    BOOST_REQUIRE(sessions[0].phase0_commits.count(sender_ptx));
    BOOST_CHECK(tampered != sessions[0].phase0_commits.at(sender_ptx));
}

// ---------------------------------------------------------------------------
// T1-4  ReceivePhase1Msg: accepts a valid message; stores vvec + encrypted blob
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Receive_AcceptsValid)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[1], key_map);
    const uint256& sender_ptx = sessions[1].members[sessions[1].my_idx].proTxHash;

    BOOST_CHECK(PTX_DKG_ReceivePhase1Msg(sessions[0], msg));

    BOOST_CHECK_EQUAL(sessions[0].phase1_vvecs.count(sender_ptx), 1u);
    BOOST_CHECK_EQUAL(sessions[0].phase1_encrypted_shares.count(sender_ptx), 1u);
    BOOST_CHECK_EQUAL(sessions[0].bad_members.count(sender_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T1-5  ReceivePhase1Msg: rejects bad sig  ← load-bearing falsification (Cycle 1)
//
// Bad sig → return false AND not stored AND NOT marked bad.
// Sig failure is a message-level reject, not provable misbehaviour.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Receive_RejectsBadSig)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[1], key_map);

    std::vector<uint8_t> bytes = msg.sig.ToByteVector();
    bytes[0] ^= 0xFF;
    msg.sig.SetByteVector(bytes);

    const uint256& sender_ptx = sessions[1].members[sessions[1].my_idx].proTxHash;

    BOOST_CHECK(!PTX_DKG_ReceivePhase1Msg(sessions[0], msg));
    BOOST_CHECK_EQUAL(sessions[0].phase1_vvecs.count(sender_ptx), 0u);  // not stored
    BOOST_CHECK_EQUAL(sessions[0].bad_members.count(sender_ptx), 0u);   // not marked bad
}

// ---------------------------------------------------------------------------
// T1-6  ReceivePhase1Msg: rejects commitment mismatch → marks sender bad
//        THE GJKR CHECK — load-bearing falsification (Cycle 2)
//
// vvec[0] replaced with a point from a different session (different polynomial →
// commitment mismatch).  Sig re-computed with the correct key to isolate the
// commitment check.
// Proves: false returned AND sender in bad_members AND vvec not stored.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Receive_RejectsCommitmentMismatch)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[1], key_map);
    const uint256& sender_ptx = sessions[1].members[sessions[1].my_idx].proTxHash;

    // Substitute vvec[0] with a point from a different polynomial.
    msg.vvec[0] = sessions[2].local_contrib.vvec[0];
    // Re-sign so sig check passes; isolates the commitment check.
    msg.sig = key_map.at(sender_ptx).Sign(msg.GetSignHash());

    BOOST_CHECK(!PTX_DKG_ReceivePhase1Msg(sessions[0], msg));
    BOOST_CHECK_EQUAL(sessions[0].phase1_vvecs.count(sender_ptx), 0u);  // not stored
    BOOST_CHECK_EQUAL(sessions[0].bad_members.count(sender_ptx), 1u);   // marked bad
}

// ---------------------------------------------------------------------------
// T1-7  ReceivePhase1Msg: rejects non-QUAL sender; does NOT mark bad
//
// QUAL-reject fires before sig and commitment checks; returns false without
// inserting into bad_members.  This distinguishes a QUAL rejection
// (bad_members unchanged) from a commitment rejection (T1-6, bad_members+=1).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Receive_RejectsNonQualSender)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    uint256 fbh = TestFormationHash();
    auto members = MakeTestMembers(key_map);

    // Build all 11 sender sessions (need real local_contrib for a Phase 1 msg).
    sessions.resize(11);
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_InitSession(sessions[i], members, fbh, members[i].proTxHash));
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(sessions[i]));

    std::vector<PTXDKGPhase0Msg> p0msgs(11);
    for (int i = 0; i < 11; i++) {
        const uint256& my_ptx = sessions[i].members[sessions[i].my_idx].proTxHash;
        p0msgs[i] = PTX_DKG_BuildPhase0Msg(sessions[i], key_map.at(my_ptx));
    }

    // Receiver gets Phase 0 from everyone except sorted slot 5.
    PTXDKGSession receiver;
    BOOST_REQUIRE(PTX_DKG_InitSession(receiver, members, fbh, members[0].proTxHash));
    BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(receiver));
    const uint256& excluded_ptx = receiver.members[5].proTxHash;
    for (int sender = 0; sender < 11; sender++) {
        if (p0msgs[sender].proTxHash == excluded_ptx) continue;
        BOOST_REQUIRE(PTX_DKG_ReceivePhase0Msg(receiver, p0msgs[sender]));
    }
    BOOST_REQUIRE(PTX_DKG_ClosePhase0(receiver));
    BOOST_CHECK_EQUAL(receiver.qual.count(excluded_ptx), 0u); // confirm absent from QUAL

    // Find the session whose self-identity is the excluded member.
    int excl_sess = -1;
    for (int i = 0; i < 11; i++)
        if (sessions[i].members[sessions[i].my_idx].proTxHash == excluded_ptx) {
            excl_sess = i; break;
        }
    BOOST_REQUIRE(excl_sess >= 0);

    // Advance excluded session to CONTRIB and give it a QUAL set so
    // BuildPhase1Msg's assert(phase==CONTRIB) passes.
    for (int sender = 0; sender < 11; sender++)
        PTX_DKG_ReceivePhase0Msg(sessions[excl_sess], p0msgs[sender]);
    sessions[excl_sess].phase = PTXDKGPhase::CONTRIB;
    sessions[excl_sess].qual  = receiver.qual; // copy receiver's QUAL

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[excl_sess], key_map);

    BOOST_CHECK(!PTX_DKG_ReceivePhase1Msg(receiver, msg));
    BOOST_CHECK_EQUAL(receiver.phase1_vvecs.count(excluded_ptx), 0u);  // not stored
    BOOST_CHECK_EQUAL(receiver.bad_members.count(excluded_ptx), 0u);   // NOT marked bad
}

// ---------------------------------------------------------------------------
// T1-8  ReceivePhase1Msg: rejects wrong quorum_hash
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Receive_RejectsWrongQuorumHash)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[1], key_map);
    const uint256& sender_ptx = sessions[1].members[sessions[1].my_idx].proTxHash;

    std::vector<unsigned char> ob(32, 0xDE);
    msg.quorum_hash = uint256(ob);
    // Re-sign to isolate the quorum_hash check.
    msg.sig = key_map.at(sender_ptx).Sign(msg.GetSignHash());

    BOOST_CHECK(!PTX_DKG_ReceivePhase1Msg(sessions[0], msg));
    BOOST_CHECK_EQUAL(sessions[0].phase1_vvecs.count(sender_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T1-9  ReceivePhase1Msg: rejects duplicate from same sender
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Receive_RejectsDuplicate)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[2], key_map);
    const uint256& sender_ptx = sessions[2].members[sessions[2].my_idx].proTxHash;

    BOOST_CHECK(PTX_DKG_ReceivePhase1Msg(sessions[0], msg));    // first: accepted
    BOOST_CHECK(!PTX_DKG_ReceivePhase1Msg(sessions[0], msg));   // duplicate: rejected

    BOOST_CHECK_EQUAL(sessions[0].phase1_vvecs.count(sender_ptx), 1u); // stored once
}

// ---------------------------------------------------------------------------
// T1-10  DecryptMyShare: correct sk → stored scalar == sender's eval[receiver_my_idx]
//
// Uses receiver's actual session.my_idx (not a separately-computed constant) so
// that an off-by-one in slot derivation makes this test fail.
// Byte comparison via blst_bendian_from_scalar (canonical encoding, both sides).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_DecryptMyShare_CorrectSk)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    const int sender_idx   = 1;
    const int receiver_idx = 0;

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[sender_idx], key_map);
    const uint256& sender_ptx =
        sessions[sender_idx].members[sessions[sender_idx].my_idx].proTxHash;

    BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(sessions[receiver_idx], msg));

    const uint256& receiver_ptx =
        sessions[receiver_idx].members[sessions[receiver_idx].my_idx].proTxHash;
    BOOST_REQUIRE(PTX_DKG_DecryptMyShare(sessions[receiver_idx], sender_ptx,
                                          key_map.at(receiver_ptx)));
    BOOST_REQUIRE_EQUAL(sessions[receiver_idx].received_shares.count(sender_ptx), 1u);

    // Expected: sender's eval at the receiver's slot (the real index-derivation path).
    int receiver_slot = sessions[receiver_idx].my_idx;
    const blst_scalar& expected = sessions[sender_idx].local_contrib.evals[receiver_slot];
    const blst_scalar& got      = sessions[receiver_idx].received_shares.at(sender_ptx);

    uint8_t exp_buf[32], got_buf[32];
    blst_bendian_from_scalar(exp_buf, &expected);
    blst_bendian_from_scalar(got_buf, &got);
    BOOST_CHECK_EQUAL(memcmp(exp_buf, got_buf, 32), 0);
}

// ---------------------------------------------------------------------------
// T1-11  DecryptMyShare: wrong sk → stored scalar ≠ expected eval
//
// AES-CBC without padding always "succeeds" regardless of key correctness and
// always yields 32 bytes.  DecryptMyShare therefore returns true but stores
// garbage.  The test asserts stored ≠ expected — NOT that the function returned
// false (it won't for a wrong-but-valid sk).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_DecryptMyShare_WrongSk)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    const int sender_idx   = 1;
    const int receiver_idx = 0;

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[sender_idx], key_map);
    const uint256& sender_ptx =
        sessions[sender_idx].members[sessions[sender_idx].my_idx].proTxHash;

    BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(sessions[receiver_idx], msg));

    CBLSSecretKey wrong_sk;
    wrong_sk.MakeNewKey();
    // Return value not asserted — may be true (AES ran) or false (DH failed).
    PTX_DKG_DecryptMyShare(sessions[receiver_idx], sender_ptx, wrong_sk);

    int receiver_slot = sessions[receiver_idx].my_idx;
    const blst_scalar& expected = sessions[sender_idx].local_contrib.evals[receiver_slot];
    uint8_t exp_buf[32];
    blst_bendian_from_scalar(exp_buf, &expected);

    if (sessions[receiver_idx].received_shares.count(sender_ptx)) {
        uint8_t got_buf[32];
        blst_bendian_from_scalar(got_buf,
            &sessions[receiver_idx].received_shares.at(sender_ptx));
        BOOST_CHECK(memcmp(exp_buf, got_buf, 32) != 0); // garbage ≠ expected
    }
    // If received_shares has no entry (decrypt returned false), that is also correct.
}

// ---------------------------------------------------------------------------
// T1-12  IsPhase1Complete: false at |QUAL|-1 reveals, true at |QUAL|
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_IsComplete_FalseAt10_TrueAt11)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGSession& recv = sessions[0];

    // Receive from sessions[1..10] (10 messages) — still incomplete.
    for (int sender = 1; sender <= 10; sender++) {
        BOOST_CHECK(!PTX_DKG_IsPhase1Complete(recv));
        PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[sender], key_map);
        BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(recv, msg));
    }
    BOOST_CHECK(!PTX_DKG_IsPhase1Complete(recv));

    // 11th reveal: sessions[0] receives its own message.
    PTXDKGPhase1Msg self_msg = BuildRealPhase1Msg(sessions[0], key_map);
    BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(recv, self_msg));
    BOOST_CHECK(PTX_DKG_IsPhase1Complete(recv));
}

// ---------------------------------------------------------------------------
// T1-13  ClosePhase1: all revealed → COMPLAINT; bad_members empty
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_ClosePhase1_FullRevealSucceeds)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGSession& recv = sessions[0];
    for (int sender = 0; sender < 11; sender++) {
        PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[sender], key_map);
        BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(recv, msg));
    }

    BOOST_CHECK(PTX_DKG_ClosePhase1(recv));
    BOOST_CHECK(recv.phase == PTXDKGPhase::COMPLAINT);
    BOOST_CHECK_EQUAL(recv.bad_members.size(), 0u);
}

// ---------------------------------------------------------------------------
// T1-14  ClosePhase1: one non-revealer → bad_members contains it; COMPLAINT
//
// Non-revealer is at sessions[5].  Remaining revealed set = 10 ≥ t=6.
// ClosePhase1 must: return true, advance to COMPLAINT, mark exactly the
// non-revealer bad, not mark any other member.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_ClosePhase1_OneNonRevealerContinues)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGSession& recv = sessions[0];
    const uint256& non_revealer_ptx =
        sessions[5].members[sessions[5].my_idx].proTxHash;

    for (int sender = 0; sender < 11; sender++) {
        if (sessions[sender].members[sessions[sender].my_idx].proTxHash == non_revealer_ptx)
            continue;
        PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[sender], key_map);
        BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(recv, msg));
    }

    BOOST_CHECK(PTX_DKG_ClosePhase1(recv));
    BOOST_CHECK(recv.phase == PTXDKGPhase::COMPLAINT);
    BOOST_CHECK_EQUAL(recv.bad_members.count(non_revealer_ptx), 1u); // non-revealer marked bad
    BOOST_CHECK_EQUAL(recv.bad_members.size(), 1u);                   // only that one
}

// ---------------------------------------------------------------------------
// T1-15  ClosePhase1 boundary: |revealed|=t-1=5 → ABORTED; |revealed|=t=6 → COMPLAINT
//        Mirror of T0-21.  Floor is at exactly t=6.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_ClosePhase1_ThresholdBoundary)
{
    // |revealed| = t-1 = 5 → must ABORT.
    {
        std::map<uint256, CBLSSecretKey> km;
        std::vector<PTXDKGSession> sess;
        SetupFullPhase0Sessions(km, sess);

        int received = 0;
        for (int sender = 0; sender < 11 && received < 5; sender++) {
            PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sess[sender], km);
            if (PTX_DKG_ReceivePhase1Msg(sess[0], msg)) received++;
        }
        BOOST_REQUIRE_EQUAL(sess[0].phase1_vvecs.size(), 5u);

        BOOST_CHECK(!PTX_DKG_ClosePhase1(sess[0]));
        BOOST_CHECK(sess[0].phase == PTXDKGPhase::ABORTED);
    }

    // |revealed| = t = 6 → must advance to COMPLAINT.
    {
        std::map<uint256, CBLSSecretKey> km;
        std::vector<PTXDKGSession> sess;
        SetupFullPhase0Sessions(km, sess);

        int received = 0;
        for (int sender = 0; sender < 11 && received < 6; sender++) {
            PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sess[sender], km);
            if (PTX_DKG_ReceivePhase1Msg(sess[0], msg)) received++;
        }
        BOOST_REQUIRE_EQUAL(sess[0].phase1_vvecs.size(), 6u);

        BOOST_CHECK(PTX_DKG_ClosePhase1(sess[0]));
        BOOST_CHECK(sess[0].phase == PTXDKGPhase::COMPLAINT);
        BOOST_CHECK_EQUAL(sess[0].bad_members.size(), 5u); // 11-6 non-revealers marked bad
    }
}

// ---------------------------------------------------------------------------
// T1-16  GetSignHash blob-binding falsification
//
// Build a valid Phase 1 message, flip ONE byte inside an encrypted_shares blob,
// do NOT re-sign.  ReceivePhase1Msg must reject because the sig check fails
// (GetSignHash covers blob bytes so the tampered blob invalidates the hash).
// If GetSignHash omitted the blobs, the sig would still verify and the message
// would be wrongly accepted — this test goes RED in that case.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Receive_RejectsTamperedBlob)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    PTXDKGPhase1Msg msg = BuildRealPhase1Msg(sessions[1], key_map);
    const uint256& sender_ptx = sessions[1].members[sessions[1].my_idx].proTxHash;

    // Find first non-empty QUAL blob and flip one byte.
    bool flipped = false;
    for (auto& blob : msg.encrypted_shares.blobs) {
        if (!blob.empty()) {
            blob[0] ^= 0x01;
            flipped = true;
            break;
        }
    }
    BOOST_REQUIRE(flipped); // sanity: at least one QUAL blob must exist

    // No re-sign — original sig now covers the old (unflipped) blob bytes.
    BOOST_CHECK(!PTX_DKG_ReceivePhase1Msg(sessions[0], msg));
    BOOST_CHECK_EQUAL(sessions[0].phase1_vvecs.count(sender_ptx), 0u);  // not stored
    BOOST_CHECK_EQUAL(sessions[0].bad_members.count(sender_ptx), 0u);   // not marked bad
}

// ---------------------------------------------------------------------------
// T1-17  bad-then-good resend stays rejected (FIX 1 regression guard)
//
// Step 1: member M sends a commitment-mismatch message (re-signed to isolate the
//   commitment check) → returns false, bad_members.count(M)==1, vvecs.count(M)==0.
// Step 2: M sends a fully valid message → returns false (early bad_members reject),
//   phase1_vvecs.count(M)==0 (still not stored), bad_members.count(M)==1 (unchanged).
//
// Without FIX 1, step 2 would return true and store the vvec (the member would sit
// in both bad_members and phase1_vvecs).  This test goes RED on unfixed code.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase1_Receive_BadThenGoodResendRejected)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    const uint256& sender_ptx = sessions[1].members[sessions[1].my_idx].proTxHash;

    // Step 1: commitment-mismatch → member marked bad.
    {
        PTXDKGPhase1Msg bad_msg = BuildRealPhase1Msg(sessions[1], key_map);
        bad_msg.vvec[0] = sessions[2].local_contrib.vvec[0]; // wrong vvec
        bad_msg.sig = key_map.at(sender_ptx).Sign(bad_msg.GetSignHash()); // valid sig
        BOOST_CHECK(!PTX_DKG_ReceivePhase1Msg(sessions[0], bad_msg));
        BOOST_CHECK_EQUAL(sessions[0].bad_members.count(sender_ptx), 1u);
        BOOST_CHECK_EQUAL(sessions[0].phase1_vvecs.count(sender_ptx), 0u);
    }

    // Step 2: valid message from same member → early bad_members reject.
    {
        PTXDKGPhase1Msg good_msg = BuildRealPhase1Msg(sessions[1], key_map);
        BOOST_CHECK(!PTX_DKG_ReceivePhase1Msg(sessions[0], good_msg));
        BOOST_CHECK_EQUAL(sessions[0].phase1_vvecs.count(sender_ptx), 0u);  // still not stored
        BOOST_CHECK_EQUAL(sessions[0].bad_members.count(sender_ptx), 1u);   // unchanged
    }
}

BOOST_AUTO_TEST_SUITE_END()
