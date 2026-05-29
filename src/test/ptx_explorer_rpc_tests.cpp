// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "key.h"
#include "key_io.h"
#include "ptx/ptx_lottery_state.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "sync.h"
#include "validation.h"

#include <univalue.h>

#include <boost/test/unit_test.hpp>

// Forward-declare the RPC handler — it has external linkage (not static) in rpc/ptx.cpp.
UniValue ptx_lottery_status(const JSONRPCRequest& request);

// Helper: invoke the handler with no help flag and no params.
static UniValue CallLotteryStatus()
{
    JSONRPCRequest req;
    req.fHelp = false;
    req.params = UniValue(UniValue::VARR);
    return ptx_lottery_status(req);
}

// Helper: build a minimal P2PKH script and return (script, expected_address).
static std::pair<CScript, std::string> MakeP2PKHScript()
{
    CKey key;
    key.MakeNewKey(true);
    CKeyID keyid = key.GetPubKey().GetID();
    CScript script = GetScriptForDestination(CTxDestination(keyid));
    CTxDestination dest;
    BOOST_REQUIRE(ExtractDestination(script, dest));
    std::string addr = EncodeDestination(dest);
    return {script, addr};
}

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

BOOST_FIXTURE_TEST_SUITE(ptx_explorer_rpc_tests, PTXBeaTestingSetup)

// ---------------------------------------------------------------------------
// total_rolls field
// ---------------------------------------------------------------------------

// Empty LotteryState → total_rolls == 0 in RPC output.
BOOST_AUTO_TEST_CASE(ExplorerRPC_TotalRolls_EmptyState)
{
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
    }
    UniValue result = CallLotteryStatus();
    BOOST_CHECK_EQUAL(result["total_rolls"].get_int64(), 0);
}

// LotteryState with total_rolls=7 → RPC output total_rolls==7.
// Falsification target A: changing handler to emit total_rolls+1 makes this RED.
BOOST_AUTO_TEST_CASE(ExplorerRPC_TotalRolls_ReflectsLotteryState)
{
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().total_rolls = 7;
    }
    UniValue result = CallLotteryStatus();
    BOOST_CHECK_EQUAL(result["total_rolls"].get_int64(), 7);
}

// ---------------------------------------------------------------------------
// settlement_history field
// ---------------------------------------------------------------------------

// No settlements yet → settlement_history is an empty array.
BOOST_AUTO_TEST_CASE(ExplorerRPC_SettlementHistory_EmptyWhenNoneYet)
{
    {
        LOCK(cs_main);
        GetLotteryState().Reset();
    }
    UniValue result = CallLotteryStatus();
    BOOST_CHECK(result["settlement_history"].isArray());
    BOOST_CHECK_EQUAL(result["settlement_history"].size(), 0U);
}

// One settlement in ring buffer → settlement_history[0] has correct fields.
// Falsification target B: changing handler to emit height+1 makes this RED.
BOOST_AUTO_TEST_CASE(ExplorerRPC_SettlementHistory_FieldValues)
{
    std::pair<CScript, std::string> p = MakeP2PKHScript();
    const CScript& script       = p.first;
    const std::string& expectedAddr = p.second;

    LastSettlement settle;
    settle.height        = 100;
    settle.amount        = 987654321LL;  // 9.87654321 HMS
    settle.payout_txid   =
        uint256S("fedcba09fedcba09fedcba09fedcba09fedcba09fedcba09fedcba09fedcba09");
    settle.winner_protx  =
        uint256S("abcdef12abcdef12abcdef12abcdef12abcdef12abcdef12abcdef12abcdef12");
    settle.winner_script = script;

    {
        LOCK(cs_main);
        GetLotteryState().Reset();
        GetLotteryState().settlement_history.push_back(settle);
    }

    UniValue result = CallLotteryStatus();

    BOOST_REQUIRE(result["settlement_history"].isArray());
    BOOST_REQUIRE_EQUAL(result["settlement_history"].size(), 1U);

    const UniValue& entry = result["settlement_history"][0];
    // Falsification target B — height is pinned independently by name.
    BOOST_CHECK_EQUAL(entry["height"].get_int64(), 100);
    // Amount string: 9.87654321 HMS.
    BOOST_CHECK_EQUAL(entry["amount"].get_str(), "9.87654321");
    // txid round-trips.
    BOOST_CHECK_EQUAL(entry["txid"].get_str(), settle.payout_txid.GetHex());
    // winner_protx round-trips (amendment 3).
    BOOST_CHECK_EQUAL(entry["winner_protx"].get_str(), settle.winner_protx.GetHex());
    // gm address derived from winner_script.
    BOOST_CHECK_EQUAL(entry["gm"].get_str(), expectedAddr);
}

BOOST_AUTO_TEST_SUITE_END()
