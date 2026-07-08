// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W2.0b C0: wire-serialization round-trip tests for the ceremony messages.
// Direct-call tests — no daemon, no P2P.  Transport dispatch is W2.0b C1+.
//
// What these rows establish (and what they do NOT):
//   - Encode/decode fidelity: serialize → deserialize → re-serialize is
//     byte-stable, GetSignHash is unchanged, and the operator-key sig still
//     verifies — i.e. the wire layer does not disturb the existing auth
//     (GetSignHash is a direct field hash, independent of SERIALIZE_METHODS).
//   - Decoder reject arms: truncation, an invalid compressed G1 point
//     (blst_p1_uncompress failure), an oversize vvec count, a non-canonical
//     scalar (blst_scalar_fr_check failure) each throw ios_base::failure.
//   - A wire-tampered message that still decodes is rejected by the EXISTING
//     receive-path sig check (auth reused unchanged, S-9).
// They do NOT establish transport routing, relay, or anti-DoS — those are
// C1/C2 rows — nor anything about a live ceremony (W2.2-bound).
//
// Phase 0/1 messages are built through the real ceremony builders on real
// sessions (the phase-1 TU fixture pattern) and the deserialized message is
// fed to the real receive function.  Phase 2/3/4 messages are hand-populated
// and operator-key signed: the serializer's contract is byte-form fidelity +
// sign-hash binding, which does not depend on how the struct was produced,
// and building a real complaint/justify/premit requires the full multi-phase
// machinery already exercised in their own TUs.
//
// Test inventory:
//   S-0  Phase0 round-trip: byte-stable; sign-hash stable; sig verifies;
//        deserialized msg ACCEPTED by ReceivePhase0Msg on a fresh session
//   S-1  Phase1 round-trip: same properties through the full Phase-0 fixture;
//        deserialized msg ACCEPTED by ReceivePhase1Msg
//   S-2  Phase2 round-trip: byte-stable; sign-hash stable; sig verifies
//   S-3  Phase3 round-trip: byte-stable; sign-hash stable; sig verifies
//   S-4  Phase4 wire round-trip: byte-stable (encoding is consensus-frozen —
//        it ships inside the on-chain PTXDKGPayload; this row guards drift)
//   S-5  truncated stream → ios_base::failure (Phase0 and Phase1)
//   S-6  Phase1 invalid point bytes → "ptxdkg-phase1-bad-point"
//   S-7  Phase1 oversize vvec count → "ptxdkg-phase1-vvec-oversize"
//   S-8  Phase3 non-canonical scalar → "ptxdkg-phase3-noncanonical-scalar"
//   S-9  Phase0 wire tamper (commitment byte flip): decodes, then the
//        EXISTING sig check rejects it (receive returns false)

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"

#include "bls/bls_wrapper.h"
#include "streams.h"
#include "uint256.h"
#include "version.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_serialization_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers (the phase-1 TU fixture pattern)
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

// All 11 sessions initialized + local contribs generated (HASH_COMMIT phase).
static void SetupCommitSessions(
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
}

// Full Phase 0 exchange on top of SetupCommitSessions → all in CONTRIB.
static void SetupFullPhase0Sessions(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    SetupCommitSessions(key_map, sessions);

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

template <typename Msg>
static std::vector<unsigned char> SerBytes(const Msg& msg)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << msg;
    return std::vector<unsigned char>(ss.begin(), ss.end());
}

template <typename Msg>
static Msg DeserBytes(const std::vector<unsigned char>& bytes)
{
    CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
    Msg out;
    ss >> out;
    return out;
}

static bool FailureContains(const std::ios_base::failure& e, const char* needle)
{
    return std::string(e.what()).find(needle) != std::string::npos;
}

// ---------------------------------------------------------------------------
// S-0 — Phase0 round-trip: byte-stable, sign-hash stable, sig verifies,
//        and the deserialized message is accepted by the real receive path.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s0_phase0_roundtrip_accepted)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupCommitSessions(key_map, sessions);

    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;
    PTXDKGPhase0Msg msg = PTX_DKG_BuildPhase0Msg(sessions[0], key_map.at(sender_ptx));

    auto bytes = SerBytes(msg);
    PTXDKGPhase0Msg out = DeserBytes<PTXDKGPhase0Msg>(bytes);

    BOOST_CHECK(SerBytes(out) == bytes);                       // byte-stable
    BOOST_CHECK(out.GetSignHash() == msg.GetSignHash());       // sign-hash stable
    BOOST_CHECK(out.sig.VerifyInsecure(key_map.at(sender_ptx).GetPublicKey(),
                                       out.GetSignHash()));    // auth unchanged

    // The real receive path accepts the wire round-tripped message.
    BOOST_CHECK(PTX_DKG_ReceivePhase0Msg(sessions[1], out));
}

// ---------------------------------------------------------------------------
// S-1 — Phase1 round-trip through the full Phase-0 fixture.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s1_phase1_roundtrip_accepted)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);

    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;
    PTXDKGPhase1Msg msg = PTX_DKG_BuildPhase1Msg(sessions[0], key_map.at(sender_ptx));
    BOOST_REQUIRE_EQUAL(msg.vvec.size(), 6U); // t=6 — fixes the corruption offsets below

    auto bytes = SerBytes(msg);
    PTXDKGPhase1Msg out = DeserBytes<PTXDKGPhase1Msg>(bytes);

    BOOST_CHECK(SerBytes(out) == bytes);
    BOOST_CHECK(out.GetSignHash() == msg.GetSignHash());
    BOOST_CHECK(out.sig.VerifyInsecure(key_map.at(sender_ptx).GetPublicKey(),
                                       out.GetSignHash()));

    BOOST_CHECK(PTX_DKG_ReceivePhase1Msg(sessions[1], out));
}

// ---------------------------------------------------------------------------
// S-2 — Phase2 (complaint) round-trip.  Hand-populated + operator-key signed;
//        the serializer's contract does not depend on construction path.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s2_phase2_roundtrip)
{
    CBLSSecretKey sk;
    sk.MakeNewKey();

    PTXDKGPhase2Msg msg;
    msg.quorum_hash = TestFormationHash();
    std::vector<unsigned char> buf(32, 0x11);
    msg.complainant_proTxHash = uint256(buf);
    buf.assign(32, 0x22);
    msg.dealer_proTxHash = uint256(buf);
    msg.share_index_j = 7;
    msg.sig = sk.Sign(msg.GetSignHash());

    auto bytes = SerBytes(msg);
    PTXDKGPhase2Msg out = DeserBytes<PTXDKGPhase2Msg>(bytes);

    BOOST_CHECK(SerBytes(out) == bytes);
    BOOST_CHECK(out.GetSignHash() == msg.GetSignHash());
    BOOST_CHECK_EQUAL(out.share_index_j, 7);
    BOOST_CHECK(out.sig.VerifyInsecure(sk.GetPublicKey(), out.GetSignHash()));
}

// ---------------------------------------------------------------------------
// S-3 — Phase3 (justification) round-trip with a canonical scalar.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s3_phase3_roundtrip)
{
    CBLSSecretKey sk;
    sk.MakeNewKey();

    PTXDKGPhase3Msg msg;
    msg.quorum_hash = TestFormationHash();
    std::vector<unsigned char> buf(32, 0x33);
    msg.dealer_proTxHash = uint256(buf);
    buf.assign(32, 0x44);
    msg.complainant_proTxHash = uint256(buf);

    uint8_t sbytes[32];
    memset(sbytes, 0, 32);
    sbytes[31] = 0x07; // 7 < r — canonical
    blst_scalar_from_bendian(&msg.revealed_share, sbytes);
    BOOST_REQUIRE(blst_scalar_fr_check(&msg.revealed_share));

    msg.sig = sk.Sign(msg.GetSignHash());

    auto bytes = SerBytes(msg);
    PTXDKGPhase3Msg out = DeserBytes<PTXDKGPhase3Msg>(bytes);

    BOOST_CHECK(SerBytes(out) == bytes);
    BOOST_CHECK(out.GetSignHash() == msg.GetSignHash());
    uint8_t rt[32];
    blst_bendian_from_scalar(rt, &out.revealed_share);
    BOOST_CHECK(memcmp(rt, sbytes, 32) == 0); // scalar bytes survive the seam
    BOOST_CHECK(out.sig.VerifyInsecure(sk.GetPublicKey(), out.GetSignHash()));
}

// ---------------------------------------------------------------------------
// S-4 — Phase4 wire round-trip.  The encoding is consensus-frozen (it ships
//        inside the on-chain PTXDKGPayload) — this row guards against drift.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s4_phase4_roundtrip)
{
    CBLSSecretKey sk;
    sk.MakeNewKey();

    PTXDKGPhase4Msg msg;
    msg.quorum_hash = TestFormationHash();
    std::vector<unsigned char> buf(32, 0x55);
    msg.proTxHash = uint256(buf);
    memset(msg.group_pk_bytes, 0xAB, 48);
    buf.assign(32, 0x66);
    msg.vvec_hash = uint256(buf);
    msg.sig = sk.Sign(msg.GetSignHash());

    auto bytes = SerBytes(msg);
    PTXDKGPhase4Msg out = DeserBytes<PTXDKGPhase4Msg>(bytes);

    BOOST_CHECK(SerBytes(out) == bytes);
    BOOST_CHECK(out.GetSignHash() == msg.GetSignHash());
    BOOST_CHECK(memcmp(out.group_pk_bytes, msg.group_pk_bytes, 48) == 0);
    BOOST_CHECK(out.sig.VerifyInsecure(sk.GetPublicKey(), out.GetSignHash()));
}

// ---------------------------------------------------------------------------
// S-5 — truncated streams throw.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s5_truncation_rejected)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);
    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;

    {
        PTXDKGPhase0Msg msg = PTX_DKG_BuildPhase0Msg(sessions[0], key_map.at(sender_ptx));
        auto bytes = SerBytes(msg);
        bytes.resize(40); // mid-proTxHash
        CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
        PTXDKGPhase0Msg out;
        BOOST_CHECK_THROW(ss >> out, std::ios_base::failure);
    }
    {
        PTXDKGPhase1Msg msg = PTX_DKG_BuildPhase1Msg(sessions[0], key_map.at(sender_ptx));
        auto bytes = SerBytes(msg);
        bytes.resize(65 + 100); // mid-vvec (points start at 32+32+1 = 65)
        CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
        PTXDKGPhase1Msg out;
        BOOST_CHECK_THROW(ss >> out, std::ios_base::failure);
    }
}

// ---------------------------------------------------------------------------
// S-6 — Phase1 invalid point bytes → ptxdkg-phase1-bad-point.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s6_phase1_bad_point_rejected)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);
    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;

    PTXDKGPhase1Msg msg = PTX_DKG_BuildPhase1Msg(sessions[0], key_map.at(sender_ptx));
    auto bytes = SerBytes(msg);
    for (size_t i = 65; i < 65 + 48; i++) bytes[i] = 0xFF; // first vvec point

    CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
    PTXDKGPhase1Msg out;
    BOOST_CHECK_EXCEPTION(ss >> out, std::ios_base::failure,
        [](const std::ios_base::failure& e) {
            return FailureContains(e, "ptxdkg-phase1-bad-point");
        });
}

// ---------------------------------------------------------------------------
// S-7 — Phase1 oversize vvec count → ptxdkg-phase1-vvec-oversize.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s7_phase1_oversize_vvec_rejected)
{
    // Hand-crafted prefix: quorum_hash(32) + proTxHash(32) + compactsize(12).
    std::vector<unsigned char> bytes(64, 0x00);
    bytes.push_back(12); // compactsize 12 > PTX_DKG_WIRE_MAX_VVEC (11)

    CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
    PTXDKGPhase1Msg out;
    BOOST_CHECK_EXCEPTION(ss >> out, std::ios_base::failure,
        [](const std::ios_base::failure& e) {
            return FailureContains(e, "ptxdkg-phase1-vvec-oversize");
        });
}

// ---------------------------------------------------------------------------
// S-7b — far-beyond count: a stream declaring 1,000,000 vvec elements is
//        rejected on the OVERSIZE reason (not end-of-data, not
//        ReadCompactSize's 32M ceiling) BEFORE reserve()/reads — the
//        memory-DoS arm.  Stub the cap → decode proceeds past the gate
//        (reserve executes) → RED via wrong-reason.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s7b_oversize_far_beyond_count_rejected)
{
    std::vector<unsigned char> bytes(64, 0x00);
    bytes.push_back(0xFE); // compactsize marker: uint32 follows
    bytes.push_back(0x40); // 1,000,000 = 0x000F4240 LE
    bytes.push_back(0x42);
    bytes.push_back(0x0F);
    bytes.push_back(0x00);

    CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
    PTXDKGPhase1Msg out;
    BOOST_CHECK_EXCEPTION(ss >> out, std::ios_base::failure,
        [](const std::ios_base::failure& e) {
            return FailureContains(e, "ptxdkg-phase1-vvec-oversize");
        });
}

// ---------------------------------------------------------------------------
// S-7c — fully-DECODABLE oversize: 12 VALID points + intact IES/sig tail
//        (would decode end-to-end without the gate) is still rejected on the
//        oversize reason — kills the "only failed because bytes ran out"
//        alternative.  Stub the cap → the over-quorum message FULLY DECODES
//        (accepted + allocated) → RED via exception-not-raised.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s7c_oversize_with_valid_points_rejected)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupFullPhase0Sessions(key_map, sessions);
    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;
    PTXDKGPhase1Msg msg = PTX_DKG_BuildPhase1Msg(sessions[0], key_map.at(sender_ptx));
    auto bytes = SerBytes(msg);

    // Splice: header, count 12, the 6 real points twice, original IES+sig tail.
    std::vector<unsigned char> spliced(bytes.begin(), bytes.begin() + 64);
    spliced.push_back(12);
    for (int r = 0; r < 2; r++)
        spliced.insert(spliced.end(), bytes.begin() + 65, bytes.begin() + 65 + 288);
    spliced.insert(spliced.end(), bytes.begin() + 65 + 288, bytes.end());

    CDataStream ss(spliced, SER_NETWORK, PROTOCOL_VERSION);
    PTXDKGPhase1Msg out;
    BOOST_CHECK_EXCEPTION(ss >> out, std::ios_base::failure,
        [](const std::ios_base::failure& e) {
            return FailureContains(e, "ptxdkg-phase1-vvec-oversize");
        });
}

// ---------------------------------------------------------------------------
// S-8 — Phase3 non-canonical scalar → ptxdkg-phase3-noncanonical-scalar.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s8_phase3_noncanonical_scalar_rejected)
{
    CBLSSecretKey sk;
    sk.MakeNewKey();

    PTXDKGPhase3Msg msg;
    msg.quorum_hash = TestFormationHash();
    std::vector<unsigned char> buf(32, 0x33);
    msg.dealer_proTxHash = uint256(buf);
    buf.assign(32, 0x44);
    msg.complainant_proTxHash = uint256(buf);
    uint8_t sbytes[32];
    memset(sbytes, 0, 32);
    sbytes[31] = 0x07;
    blst_scalar_from_bendian(&msg.revealed_share, sbytes);
    msg.sig = sk.Sign(msg.GetSignHash());

    auto bytes = SerBytes(msg);
    for (size_t i = 96; i < 96 + 32; i++) bytes[i] = 0xFF; // scalar ≥ r

    CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
    PTXDKGPhase3Msg out;
    BOOST_CHECK_EXCEPTION(ss >> out, std::ios_base::failure,
        [](const std::ios_base::failure& e) {
            return FailureContains(e, "ptxdkg-phase3-noncanonical-scalar");
        });
}

// ---------------------------------------------------------------------------
// S-9 — wire tamper that still decodes is caught by the EXISTING sig check:
//        flip a commitment byte in a serialized Phase0 msg → deserializes,
//        sig fails, receive rejects.  Auth reused unchanged — the wire layer
//        adds no acceptance path around it.
// ---------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(s9_wire_tamper_rejected_by_sig)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupCommitSessions(key_map, sessions);

    const uint256& sender_ptx = sessions[0].members[sessions[0].my_idx].proTxHash;
    PTXDKGPhase0Msg msg = PTX_DKG_BuildPhase0Msg(sessions[0], key_map.at(sender_ptx));

    auto bytes = SerBytes(msg);
    bytes[70] ^= 0x01; // inside commitment (offset 64..95)

    PTXDKGPhase0Msg out = DeserBytes<PTXDKGPhase0Msg>(bytes);
    BOOST_CHECK(!out.sig.VerifyInsecure(key_map.at(sender_ptx).GetPublicKey(),
                                        out.GetSignHash()));
    BOOST_CHECK(!PTX_DKG_ReceivePhase0Msg(sessions[1], out));
}

BOOST_AUTO_TEST_SUITE_END()
