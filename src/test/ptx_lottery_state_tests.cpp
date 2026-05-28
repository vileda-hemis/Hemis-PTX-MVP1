// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "ptx/ptx_lottery_state.h"

#include "evo/evodb.h"
#include "serialize.h"
#include "streams.h"
#include "sync.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>

BOOST_FIXTURE_TEST_SUITE(ptx_lottery_state_tests, BasicTestingSetup)

static LotteryState MakePopulatedState()
{
    LotteryState s;
    s.accumulator_outpoint = COutPoint(
        uint256S("aabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccdd"), 7);
    s.accumulator_value = 123456789LL;
    s.last_settle.height = 42;
    s.last_settle.winner_protx =
        uint256S("deadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeefdeadbeef");
    s.last_settle.winner_script = CScript() << OP_DUP << OP_HASH160
                                            << std::vector<uint8_t>(20, 0xAB)
                                            << OP_EQUALVERIFY << OP_CHECKSIG;
    s.last_settle.amount = 987654321LL;
    s.last_settle.selection_entropy =
        uint256S("cafebabecafebabecafebabecafebabecafebabecafebabecafebabecafebabe");
    s.last_settle.payout_txid =
        uint256S("1234567812345678123456781234567812345678123456781234567812345678");
    return s;
}

static std::vector<uint8_t> Serialize(const LotteryState& s)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << s;
    return {ss.begin(), ss.end()};
}

BOOST_AUTO_TEST_CASE(LotteryState_RoundTrip)
{
    LotteryState orig;
    BOOST_CHECK(orig.accumulator_outpoint.IsNull());
    BOOST_CHECK_EQUAL(orig.accumulator_value, 0);
    BOOST_CHECK_EQUAL(orig.last_settle.height, 0);

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << orig;
    LotteryState restored;
    ss >> restored;

    BOOST_CHECK(Serialize(orig) == Serialize(restored));
}

BOOST_AUTO_TEST_CASE(LotteryState_PopulatedRoundTrip)
{
    LotteryState orig = MakePopulatedState();

    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << orig;
    LotteryState restored;
    ss >> restored;

    BOOST_CHECK(Serialize(orig) == Serialize(restored));
    BOOST_CHECK_EQUAL(restored.accumulator_outpoint.hash, orig.accumulator_outpoint.hash);
    BOOST_CHECK_EQUAL(restored.accumulator_outpoint.n,    orig.accumulator_outpoint.n);
    BOOST_CHECK_EQUAL(restored.accumulator_value,         orig.accumulator_value);
    BOOST_CHECK_EQUAL(restored.last_settle.height,        orig.last_settle.height);
    BOOST_CHECK_EQUAL(restored.last_settle.winner_protx,  orig.last_settle.winner_protx);
    BOOST_CHECK(restored.last_settle.winner_script ==     orig.last_settle.winner_script);
    BOOST_CHECK_EQUAL(restored.last_settle.amount,        orig.last_settle.amount);
    BOOST_CHECK_EQUAL(restored.last_settle.selection_entropy, orig.last_settle.selection_entropy);
    BOOST_CHECK_EQUAL(restored.last_settle.payout_txid,   orig.last_settle.payout_txid);
}

BOOST_AUTO_TEST_CASE(LotteryState_Reset)
{
    LotteryState s = MakePopulatedState();
    BOOST_CHECK(!s.accumulator_outpoint.IsNull());
    BOOST_CHECK(s.accumulator_value > 0);

    s.Reset();

    BOOST_CHECK(s.accumulator_outpoint.IsNull());
    BOOST_CHECK_EQUAL(s.accumulator_value, 0);
    BOOST_CHECK_EQUAL(s.last_settle.height, 0);
    BOOST_CHECK(s.last_settle.winner_protx.IsNull());
    BOOST_CHECK(s.last_settle.winner_script.empty());
    BOOST_CHECK_EQUAL(s.last_settle.amount, 0);
    BOOST_CHECK(s.last_settle.selection_entropy.IsNull());
    BOOST_CHECK(s.last_settle.payout_txid.IsNull());
    BOOST_CHECK(Serialize(s) == Serialize(LotteryState{}));
}

BOOST_AUTO_TEST_CASE(LotteryState_HasAccumulator)
{
    LotteryState s;
    BOOST_CHECK(!s.HasAccumulator());

    s.accumulator_outpoint = COutPoint(
        uint256S("aabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccddaabbccdd"), 0);
    BOOST_CHECK(s.HasAccumulator());

    s.accumulator_outpoint.SetNull();
    BOOST_CHECK(!s.HasAccumulator());

    // value alone does not constitute having an accumulator
    s.accumulator_value = 5000;
    BOOST_CHECK(!s.HasAccumulator());
}

// Proves that after ConnectBlock writes a snapshot, later mutations (a following
// block) can be rolled back to byte-exact state via DisconnectBlock restore.
BOOST_AUTO_TEST_CASE(LotteryState_SnapshotAndRestore)
{
    const uint256 blockHashA =
        uint256S("aaaa0000aaaa0000aaaa0000aaaa0000aaaa0000aaaa0000aaaa0000aaaa0000");

    LotteryState stateA = MakePopulatedState();
    const std::vector<uint8_t> origBytes = Serialize(stateA);

    {
        LOCK(cs_main);
        GetLotteryState() = stateA;
        WriteLotteryStateSnapshotForBlock(blockHashA, stateA);
    }

    // Simulate mutations from a subsequent block applied on top of A.
    {
        LOCK(cs_main);
        LotteryState& live = GetLotteryState();
        live.accumulator_outpoint = COutPoint(
            uint256S("bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"), 1);
        live.accumulator_value  = 999999999LL;
        live.last_settle.height = 100;
        live.last_settle.amount = 555LL;
        BOOST_CHECK(Serialize(live) != origBytes);
    }

    // Simulate DisconnectBlock: restore from block A's snapshot.
    {
        LOCK(cs_main);
        LotteryState restored;
        BOOST_CHECK(ReadLotteryStateSnapshotForBlock(blockHashA, restored));
        GetLotteryState() = restored;
    }

    // Post-restore: live state must byte-match stateA exactly.
    {
        LOCK(cs_main);
        BOOST_CHECK(Serialize(GetLotteryState()) == origBytes);
        BOOST_CHECK_EQUAL(GetLotteryState().accumulator_outpoint.hash,
                          stateA.accumulator_outpoint.hash);
        BOOST_CHECK_EQUAL(GetLotteryState().accumulator_outpoint.n,
                          stateA.accumulator_outpoint.n);
        BOOST_CHECK_EQUAL(GetLotteryState().last_settle.winner_protx,
                          stateA.last_settle.winner_protx);
    }
}

// Write N snapshots, purge to keepCount, verify oldest are erased and newest remain.
BOOST_AUTO_TEST_CASE(LotteryState_PurgeStaleSnapshots)
{
    std::vector<uint256> hashes;
    for (int i = 0; i < 5; i++) {
        unsigned char buf[32] = {};
        buf[0] = static_cast<unsigned char>(i);
        buf[1] = 0xBB;
        uint256 bh(std::vector<unsigned char>(buf, buf + 32));

        LotteryState s;
        s.accumulator_value = static_cast<CAmount>(i + 1) * 100000;

        LOCK(cs_main);
        WriteLotteryStateSnapshotForBlock(bh, s);
        hashes.push_back(bh);
    }

    PurgeStaleSnapshots(3);

    // Oldest 2 (index 0, 1) must be erased.
    for (int i = 0; i < 2; i++) {
        LotteryState tmp;
        BOOST_CHECK(!ReadLotteryStateSnapshotForBlock(hashes[i], tmp));
    }

    // Newest 3 (index 2, 3, 4) must survive with correct values.
    for (int i = 2; i < 5; i++) {
        LotteryState tmp;
        BOOST_CHECK(ReadLotteryStateSnapshotForBlock(hashes[i], tmp));
        BOOST_CHECK_EQUAL(tmp.accumulator_value, static_cast<CAmount>(i + 1) * 100000);
    }
}

BOOST_AUTO_TEST_SUITE_END()
