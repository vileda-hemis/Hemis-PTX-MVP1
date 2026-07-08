// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W2.0b C2: anti-DoS + validate-before-relay falsification.
//
// THE SECURITY PROPERTY UNDER TEST is that BAD messages are DROPPED-NOT-
// RELAYED — not that good messages relay.  Each reject arm installs a
// relay-recorder + penalty-recorder on the transport and OBSERVES:
//   - bad message → recorder EMPTY (not relayed), receive NOT called, and
//     (for ban arms) penalty recorded;
//   - the paired stub->RED lives in the standup falsification log: stubbing
//     the arm makes the bad message relay/accept (recorder non-empty) — run
//     container-local per the gate, not committed as a permanently-RED row.
// V-ACCEPT is the CONTRAST row (envelope-valid → relayed + received), present
// only to prove the recorder can observe a relay at all — it is NOT the
// evidence; the reject rows are.
//
// Resolution 1: the RESOLVED session here is a TEST-CONSTRUCTED struct set
// via SetActiveSession (the transport's producer seam).  That exercises the
// validate-against-the-resolved-session contract; route-to-live-session
// (a session produced by real W2.2 formation) is W2.2-bound.
//
// ORDERING (cheap→expensive) is asserted structurally by V-ORDER: a message
// that is BOTH over-member-cap AND bad-sig is dropped by the CAP (cheap, no
// ban) — proving the expensive sig verify did not run first (else it would
// ban).  Per-node cap + seen-hash dedup run even earlier, at enqueue
// (proven in ptx_dkg_net_tests N-3/N-4).
//
// Reject-arm inventory (all lead with the reject, not the accept):
//   V-MALFORMED   truncated bytes            → drop + ban, not relayed   (R1)
//   V-UNKNOWNQ    unknown quorum_hash        → drop, NO ban, not relayed (R2)
//   V-NONMEMBER   sender not in resolved set → drop + ban, not relayed   (R3)
//   V-BADSIG      operator-key sig fails     → drop + ban, not relayed   (R4)
//   V-MEMBERCAP   >2 accepted from a member  → drop, NO ban, not relayed
//   V-ORDER       cap fires before sig (cheap-first)
//   V-ACCEPT      envelope-valid             → relayed + received (contrast)

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg_net.h"

#include "bls/bls_wrapper.h"
#include "protocol.h"
#include "streams.h"
#include "uint256.h"
#include "version.h"

#include <boost/test/unit_test.hpp>

#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_validate_relay_tests, BasicTestingSetup)

// ---- fixture: a resolved session of 11 real-keyed members ------------------

static std::vector<PTXDKGMember> MakeMembers(std::map<uint256, CBLSSecretKey>& key_map)
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
        m.confirmedHashWithProRegTxHash = PTX_DKG_ComputeInnerHash(m.proTxHash, m.confirmedHash);
        m.node_id = "gm" + std::to_string(i) + ":8080";
        CBLSSecretKey sk;
        sk.MakeNewKey();
        m.pubKeyOperator = sk.GetPublicKey();
        key_map[m.proTxHash] = sk;
        members.push_back(m);
    }
    return members;
}

static uint256 QHash()
{
    std::vector<unsigned char> buf(32, 0);
    buf[0] = 0xFB;
    buf[31] = 0x01;
    return uint256(buf);
}

// A recorder harness: a transport with relay + penalty capture installed.
struct Harness {
    CPTXCeremonyTransport transport;
    std::vector<CInv> relayed;
    std::vector<std::pair<NodeId, int>> penalties;
    std::map<uint256, CBLSSecretKey> key_map;
    PTXDKGSession session; // the resolved session

    Harness()
    {
        auto members = MakeMembers(key_map);
        session.quorum_hash = QHash();
        session.members = members;
        session.phase = PTXDKGPhase::HASH_COMMIT;
        transport.SetActiveSession(&session);
        transport.relayHook = [this](const CInv& inv, const PTXDKGSession&) { relayed.push_back(inv); };
        transport.penaltyHook = [this](NodeId id, int score) { penalties.emplace_back(id, score); };
    }

    // Build a valid Phase0 message signed by member[idx].
    PTXDKGPhase0Msg ValidP0(int idx)
    {
        PTXDKGPhase0Msg msg;
        msg.quorum_hash = session.quorum_hash;
        msg.proTxHash = session.members[idx].proTxHash;
        std::vector<unsigned char> c(32, 0x5A);
        msg.commitment = uint256(c);
        msg.sig = key_map.at(msg.proTxHash).Sign(msg.GetSignHash());
        return msg;
    }

    void Enqueue(const std::string& cmd, const PTXDKGPhase0Msg& msg, NodeId from = 7)
    {
        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
        ss << msg;
        transport.ProcessMessage(from, cmd, ss);
    }

    void EnqueueRaw(const std::string& cmd, const std::vector<unsigned char>& bytes, NodeId from = 7)
    {
        CDataStream ss(bytes, SER_NETWORK, PROTOCOL_VERSION);
        transport.ProcessMessage(from, cmd, ss);
    }
};

// V-MALFORMED (R1) — truncated → drop + ban, NOT relayed.
BOOST_AUTO_TEST_CASE(v_malformed_dropped_not_relayed)
{
    Harness h;
    auto msg = h.ValidP0(0);
    CDataStream full(SER_NETWORK, PROTOCOL_VERSION);
    full << msg;
    std::vector<unsigned char> truncated(full.begin(), full.begin() + 40);
    h.EnqueueRaw(NetMsgType::PTXQHASHCOMMIT, truncated);
    h.transport.ProcessBatch(0);

    BOOST_CHECK(h.relayed.empty());               // NOT relayed
    BOOST_REQUIRE_EQUAL(h.penalties.size(), 1U);  // banned
    BOOST_CHECK_EQUAL(h.penalties[0].second, 100);
}

// V-UNKNOWNQ (R2) — unknown quorum_hash → DROP, NO ban, NOT relayed.
BOOST_AUTO_TEST_CASE(v_unknownq_dropped_not_relayed_no_ban)
{
    Harness h;
    auto msg = h.ValidP0(0);
    std::vector<unsigned char> q(32, 0xEE);
    msg.quorum_hash = uint256(q); // no session for this hash
    msg.sig = h.key_map.at(msg.proTxHash).Sign(msg.GetSignHash());
    h.Enqueue(NetMsgType::PTXQHASHCOMMIT, msg);
    h.transport.ProcessBatch(0);

    BOOST_CHECK(h.relayed.empty());     // NOT relayed — the anti-DoS arm
    BOOST_CHECK(h.penalties.empty());   // NO ban (may legitimately not know it)
}

// V-NONMEMBER (R3) — sender not in the resolved set → drop + ban, NOT relayed.
BOOST_AUTO_TEST_CASE(v_nonmember_dropped_not_relayed)
{
    Harness h;
    // A validly-signed message from a key that is NOT a member.
    CBLSSecretKey stranger;
    stranger.MakeNewKey();
    PTXDKGPhase0Msg msg;
    msg.quorum_hash = h.session.quorum_hash;
    std::vector<unsigned char> p(32, 0x99);
    msg.proTxHash = uint256(p); // not in members
    std::vector<unsigned char> c(32, 0x5A);
    msg.commitment = uint256(c);
    msg.sig = stranger.Sign(msg.GetSignHash());
    h.Enqueue(NetMsgType::PTXQHASHCOMMIT, msg);
    h.transport.ProcessBatch(0);

    BOOST_CHECK(h.relayed.empty());
    BOOST_REQUIRE_EQUAL(h.penalties.size(), 1U);
    BOOST_CHECK_EQUAL(h.penalties[0].second, 100);
}

// V-BADSIG (R4) — operator-key sig fails → drop + ban, NOT relayed.
BOOST_AUTO_TEST_CASE(v_badsig_dropped_not_relayed)
{
    Harness h;
    auto msg = h.ValidP0(0);
    // Flip the signed commitment AFTER signing → sig no longer matches.
    std::vector<unsigned char> c(32, 0x11);
    msg.commitment = uint256(c);
    h.Enqueue(NetMsgType::PTXQHASHCOMMIT, msg);
    h.transport.ProcessBatch(0);

    BOOST_CHECK(h.relayed.empty());
    BOOST_REQUIRE_EQUAL(h.penalties.size(), 1U);
    BOOST_CHECK_EQUAL(h.penalties[0].second, 100);
}

// V-MEMBERCAP — a member's 3rd accepted message is dropped (no ban, no relay).
BOOST_AUTO_TEST_CASE(v_member_cap_drops_third)
{
    Harness h;
    // Three DISTINCT valid messages from the same member (vary commitment so
    // the seen-hash dedup does not catch them — this isolates the member cap).
    for (int k = 0; k < 3; k++) {
        PTXDKGPhase0Msg msg;
        msg.quorum_hash = h.session.quorum_hash;
        msg.proTxHash = h.session.members[0].proTxHash;
        std::vector<unsigned char> c(32, (unsigned char)(0x60 + k));
        msg.commitment = uint256(c);
        msg.sig = h.key_map.at(msg.proTxHash).Sign(msg.GetSignHash());
        h.Enqueue(NetMsgType::PTXQHASHCOMMIT, msg, 7);
    }
    h.transport.ProcessBatch(0, 8);

    BOOST_CHECK_EQUAL(h.relayed.size(), 2U); // 2 accepted+relayed, 3rd capped
    BOOST_CHECK(h.penalties.empty());        // cap is a drop, not a ban
}

// V-ORDER — cheap-before-expensive: a message that is BOTH over-cap AND
// bad-sig is dropped by the CAP (no ban), proving sig verify did not run
// first (it would ban).
BOOST_AUTO_TEST_CASE(v_order_cap_before_sig)
{
    Harness h;
    // Fill member[0]'s cap with 2 valid messages.
    for (int k = 0; k < 2; k++) {
        PTXDKGPhase0Msg msg;
        msg.quorum_hash = h.session.quorum_hash;
        msg.proTxHash = h.session.members[0].proTxHash;
        std::vector<unsigned char> c(32, (unsigned char)(0x70 + k));
        msg.commitment = uint256(c);
        msg.sig = h.key_map.at(msg.proTxHash).Sign(msg.GetSignHash());
        h.Enqueue(NetMsgType::PTXQHASHCOMMIT, msg);
    }
    h.transport.ProcessBatch(0, 8);
    BOOST_REQUIRE_EQUAL(h.relayed.size(), 2U);
    h.penalties.clear();

    // Now a 3rd from member[0] that is ALSO bad-sig.
    PTXDKGPhase0Msg bad;
    bad.quorum_hash = h.session.quorum_hash;
    bad.proTxHash = h.session.members[0].proTxHash;
    std::vector<unsigned char> c(32, 0x7F);
    bad.commitment = uint256(c);
    bad.sig = h.key_map.at(bad.proTxHash).Sign(bad.GetSignHash());
    std::vector<unsigned char> c2(32, 0x7E);
    bad.commitment = uint256(c2); // break the sig AFTER signing
    h.Enqueue(NetMsgType::PTXQHASHCOMMIT, bad);
    h.transport.ProcessBatch(0, 8);

    BOOST_CHECK_EQUAL(h.relayed.size(), 2U); // still 2 — dropped
    BOOST_CHECK(h.penalties.empty());        // by the CAP, not the sig (no ban)
}

// V-ACCEPT (contrast) — envelope-valid → relayed + stored.  Proves the
// recorder can observe a relay; NOT the security evidence.
BOOST_AUTO_TEST_CASE(v_accept_relays_and_stores)
{
    Harness h;
    auto msg = h.ValidP0(0);
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << msg;
    CHashWriter hw(SER_GETHASH, 0);
    hw.write(ss.data(), ss.size());
    uint256 hash = hw.GetHash();

    h.Enqueue(NetMsgType::PTXQHASHCOMMIT, msg);
    h.transport.ProcessBatch(0);

    BOOST_REQUIRE_EQUAL(h.relayed.size(), 1U);       // relayed
    BOOST_CHECK(h.relayed[0].type == MSG_PTX_QUORUM_HASH_COMMIT);
    BOOST_CHECK(h.relayed[0].hash == hash);
    BOOST_CHECK(h.penalties.empty());
    PTXDKGPhase0Msg got;
    BOOST_CHECK(h.transport.GetStoredPhase0(hash, got)); // stored for getdata
    BOOST_CHECK(h.transport.AlreadyHaveMsg(CInv(MSG_PTX_QUORUM_HASH_COMMIT, hash)));
}

BOOST_AUTO_TEST_SUITE_END()
