// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W2.0b C1: transport skeleton unit tests — command→queue routing, the
// resolve() seam, and the queue-hygiene arms (dedup, per-node cap, batch pop).
// Direct-call tests on a locally-constructed CPTXCeremonyTransport — no
// daemon, no P2P socket.  The tiertwo MessageDispatcher branch itself is a
// 6-line mirror of the LLMQ branch and is exercised daemon-level at the C4
// fleet battery (dispatch-reachability row) — recorded, not claimed here.
//
// Resolution-1 note: sessions here are TEST-CONSTRUCTED structs (the W1.2
// fixture pattern).  That is the resolve() CONTRACT under test — active-slot
// match vs nullptr-drop — not route-to-live-session, which is W2.2-bound.
//
// Test inventory:
//   N-1  resolve(): empty slot → nullptr for any hash
//   N-2  resolve(): match → the session; mismatch → nullptr; clear → nullptr
//   N-3  dedup: identical bytes pushed twice → second dropped (even from
//        another peer); HasSeen true                       ← stub→RED arm
//   N-4  per-node cap: msg #23 from one node dropped; other node unaffected
//                                                          ← stub→RED arm
//   N-5  batch pop: 10 queued → pop(8)=8, pop(8)=2, pop(8)=0
//   N-6  command routing: each of the 5 commands lands in ITS queue only;
//        unknown command → false (the Misbehaving contract at the dispatcher)

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg_net.h"

#include "protocol.h"
#include "streams.h"
#include "uint256.h"
#include "version.h"

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_net_tests, BasicTestingSetup)

static CDataStream MakeStream(unsigned char tag, size_t len = 64)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    std::vector<unsigned char> payload(len, tag);
    ss.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    return ss;
}

static uint256 HashOf(unsigned char tag, size_t len = 64)
{
    std::vector<unsigned char> payload(len, tag);
    CHashWriter hw(SER_GETHASH, 0);
    hw.write(reinterpret_cast<const char*>(payload.data()), payload.size());
    return hw.GetHash();
}

// N-1
BOOST_AUTO_TEST_CASE(n1_resolve_empty_slot)
{
    CPTXCeremonyTransport transport;
    std::vector<unsigned char> buf(32, 0xFB);
    BOOST_CHECK(transport.resolve(uint256(buf)) == nullptr);
    BOOST_CHECK(transport.resolve(uint256()) == nullptr);
}

// N-2
BOOST_AUTO_TEST_CASE(n2_resolve_match_mismatch_clear)
{
    CPTXCeremonyTransport transport;

    PTXDKGSession session;
    std::vector<unsigned char> buf(32, 0xFB);
    session.quorum_hash = uint256(buf);

    transport.SetActiveSession(&session);
    BOOST_CHECK(transport.resolve(session.quorum_hash) == &session);

    std::vector<unsigned char> other(32, 0xEE);
    BOOST_CHECK(transport.resolve(uint256(other)) == nullptr); // unknown → drop

    transport.SetActiveSession(nullptr);
    BOOST_CHECK(transport.resolve(session.quorum_hash) == nullptr);
}

// N-3 — dedup is content-based and cross-peer (the LLMQ seenMessages arm).
BOOST_AUTO_TEST_CASE(n3_dedup_drops_duplicate)
{
    CPTXPendingMessages queue(PTX_DKG_TRANSPORT_MAX_PER_NODE);

    CDataStream a = MakeStream(0x01);
    BOOST_CHECK(queue.PushPendingMessage(7, a, 0));
    BOOST_CHECK_EQUAL(queue.Size(), 1U);
    BOOST_CHECK(queue.HasSeen(HashOf(0x01)));

    CDataStream b = MakeStream(0x01); // identical bytes, same peer
    BOOST_CHECK(!queue.PushPendingMessage(7, b, 0));
    CDataStream c = MakeStream(0x01); // identical bytes, DIFFERENT peer
    BOOST_CHECK(!queue.PushPendingMessage(8, c, 0));
    BOOST_CHECK_EQUAL(queue.Size(), 1U);

    CDataStream d = MakeStream(0x02); // distinct bytes still accepted
    BOOST_CHECK(queue.PushPendingMessage(7, d, 0));
    BOOST_CHECK_EQUAL(queue.Size(), 2U);
}

// N-4 — per-node cap (22): message #23 from one node is dropped; another
// node is unaffected (cap is per-node, not global).
BOOST_AUTO_TEST_CASE(n4_per_node_cap)
{
    CPTXPendingMessages queue(PTX_DKG_TRANSPORT_MAX_PER_NODE);

    for (size_t i = 0; i < PTX_DKG_TRANSPORT_MAX_PER_NODE; i++) {
        CDataStream ss = MakeStream((unsigned char)i);
        BOOST_CHECK(queue.PushPendingMessage(7, ss, 0));
    }
    BOOST_CHECK_EQUAL(queue.Size(), PTX_DKG_TRANSPORT_MAX_PER_NODE);

    CDataStream over = MakeStream(0xFE);
    BOOST_CHECK(!queue.PushPendingMessage(7, over, 0)); // #23 dropped
    BOOST_CHECK_EQUAL(queue.Size(), PTX_DKG_TRANSPORT_MAX_PER_NODE);
    BOOST_CHECK(!queue.HasSeen(HashOf(0xFE))); // dropped BEFORE hashing/seen

    CDataStream fromOther = MakeStream(0xFD);
    BOOST_CHECK(queue.PushPendingMessage(8, fromOther, 0)); // other peer fine
    BOOST_CHECK_EQUAL(queue.Size(), PTX_DKG_TRANSPORT_MAX_PER_NODE + 1);
}

// N-5
BOOST_AUTO_TEST_CASE(n5_batch_pop)
{
    CPTXPendingMessages queue(PTX_DKG_TRANSPORT_MAX_PER_NODE);
    for (int i = 0; i < 10; i++) {
        CDataStream ss = MakeStream((unsigned char)(0x10 + i));
        BOOST_REQUIRE(queue.PushPendingMessage(7, ss, 0));
    }
    BOOST_CHECK_EQUAL(queue.PopPendingMessages(PTX_DKG_TRANSPORT_BATCH).size(), 8U);
    BOOST_CHECK_EQUAL(queue.PopPendingMessages(PTX_DKG_TRANSPORT_BATCH).size(), 2U);
    BOOST_CHECK_EQUAL(queue.PopPendingMessages(PTX_DKG_TRANSPORT_BATCH).size(), 0U);
}

// N-6 — command→queue routing, one queue per phase, unknown → false.
BOOST_AUTO_TEST_CASE(n6_command_routing)
{
    CPTXCeremonyTransport transport;

    const std::string commands[5] = {
        NetMsgType::PTXQHASHCOMMIT,
        NetMsgType::PTXQCONTRIB,
        NetMsgType::PTXQCOMPLAINT,
        NetMsgType::PTXQJUSTIFICATION,
        NetMsgType::PTXQPCOMMITMENT,
    };

    for (int phase = 0; phase < 5; phase++) {
        CDataStream ss = MakeStream((unsigned char)(0x40 + phase));
        BOOST_CHECK(transport.ProcessMessage(7, commands[phase], ss));
        for (int q = 0; q < 5; q++) {
            BOOST_CHECK_EQUAL(transport.PendingForPhase(q).Size(),
                              (q <= phase) ? 1U : 0U); // its queue only
        }
    }

    CDataStream ss = MakeStream(0x77);
    BOOST_CHECK(!transport.ProcessMessage(7, "ptxqbogus", ss)); // unroutable
}

BOOST_AUTO_TEST_SUITE_END()
