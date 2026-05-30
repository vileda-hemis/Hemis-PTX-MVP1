// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "bls/bls_wrapper.h"
#include "evo/deterministicgms.h"
#include "key.h"
#include "keystore.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_wallet.h"
#include "script/standard.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Generate a fresh key, return (private key, P2PKH script for its pubkey).
static std::pair<CKey, CScript> MakeKeyAndScript()
{
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    CKeyID keyid = key.GetPubKey().GetID();
    CScript script = GetScriptForDestination(CTxDestination(keyid));
    return {key, script};
}

static LastSettlement MakeSettlement(int height, const CScript& script, CAmount amount)
{
    LastSettlement s;
    s.height        = height;
    s.winner_script = script;
    s.amount        = amount;
    return s;
}

// Build a minimal DGM with the given node_id and scriptPTXPayment.
static CDeterministicGMCPtr MakeWalletDGM(const std::string& nodeId,
                                           const CScript&     payScript,
                                           uint64_t           internalId)
{
    uint256 proTxHash = uint256S(strprintf("%064x", internalId + 1));
    auto dgm   = std::make_shared<CDeterministicGM>(internalId);
    dgm->proTxHash          = proTxHash;
    dgm->collateralOutpoint = COutPoint(proTxHash, 0);

    CBLSSecretKey sk; sk.MakeNewKey();

    auto state = std::make_shared<CDeterministicGMState>();
    state->node_id          = nodeId;
    state->scriptPTXPayment = payScript;
    uint160 keyBytes; memcpy(keyBytes.begin(), proTxHash.begin(), 20);
    state->keyIDOwner  = CKeyID(keyBytes);
    state->keyIDVoting = state->keyIDOwner;
    state->pubKeyOperator.Set(sk.GetPublicKey());
    dgm->pdgmState = state;
    return dgm;
}

// ---------------------------------------------------------------------------
// PTX_FilterWalletSettlements — 3 tests
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_SUITE(ptx_wallet_tests, PTXBeaTestingSetup)

// Empty history → empty result regardless of keystore contents.
BOOST_AUTO_TEST_CASE(WalletFilter_Settlements_EmptyHistory)
{
    auto [key, script] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(key);

    std::vector<LastSettlement> empty;
    auto result = PTX_FilterWalletSettlements(ks, empty);
    BOOST_CHECK(result.empty());
}

// History has one wallet-owned entry → result contains that entry.
// Falsification target 3a: inverting IsMine check makes this RED.
BOOST_AUTO_TEST_CASE(WalletFilter_Settlements_IncludesOwnedEntry)
{
    auto [key, script] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(key);

    auto [_key2, otherScript] = MakeKeyAndScript();  // not added to ks

    std::vector<LastSettlement> history = {
        MakeSettlement(10, otherScript, 1 * COIN),  // not mine
        MakeSettlement(20, script,      2 * COIN),  // mine
    };

    auto result = PTX_FilterWalletSettlements(ks, history);
    BOOST_REQUIRE_EQUAL(result.size(), 1U);
    BOOST_CHECK_EQUAL(result[0].height, 20);
    BOOST_CHECK_EQUAL(result[0].amount, 2 * COIN);
}

// History has only non-owned entries → result is empty.
// Falsification target 3a: inverting IsMine check makes this RED (non-owned included).
BOOST_AUTO_TEST_CASE(WalletFilter_Settlements_ExcludesNonOwnedEntry)
{
    auto [key, script] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(key);

    auto [otherKey, otherScript] = MakeKeyAndScript();  // different key, not in ks

    std::vector<LastSettlement> history = {
        MakeSettlement(5, otherScript, 1 * COIN),
    };

    auto result = PTX_FilterWalletSettlements(ks, history);
    BOOST_CHECK(result.empty());
}

// ---------------------------------------------------------------------------
// PTX_FilterWalletGMs — 3 tests
// ---------------------------------------------------------------------------

// DGM list empty → result is empty.
BOOST_AUTO_TEST_CASE(WalletFilter_GMs_EmptyList)
{
    auto [key, script] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(key);

    CDeterministicGMList emptyList;
    auto result = PTX_FilterWalletGMs(ks, emptyList, g_ptx_pose_tracker);
    BOOST_CHECK(result.empty());
}

// DGM list has one wallet-owned GM → result contains it with correct fields.
// Falsification target 3b: inverting IsMine check makes this RED.
BOOST_AUTO_TEST_CASE(WalletFilter_GMs_IncludesOwnedGM)
{
    auto [key, script] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(key);

    auto [_k2, otherScript] = MakeKeyAndScript();  // not in ks

    CDeterministicGMList list;
    list.AddGM(MakeWalletDGM("gm01:aabbccdd", script,       1));
    list.AddGM(MakeWalletDGM("gm02:11223344", otherScript,  2));  // not mine

    auto result = PTX_FilterWalletGMs(ks, list, g_ptx_pose_tracker);
    BOOST_REQUIRE_EQUAL(result.size(), 1U);
    BOOST_CHECK_EQUAL(result[0].node_id, "gm01:aabbccdd");
    BOOST_CHECK(result[0].payment_script == script);
}

// DGM list has only non-owned GMs → result is empty.
// Falsification target 3b: inverting IsMine check makes this RED (non-owned returned).
BOOST_AUTO_TEST_CASE(WalletFilter_GMs_ExcludesNonOwnedGM)
{
    CBasicKeyStore ks;  // empty keystore — owns nothing

    auto [key, script] = MakeKeyAndScript();

    CDeterministicGMList list;
    list.AddGM(MakeWalletDGM("gm01:aabbccdd", script, 1));

    auto result = PTX_FilterWalletGMs(ks, list, g_ptx_pose_tracker);
    BOOST_CHECK(result.empty());
}

// ---------------------------------------------------------------------------
// Happy-path output shape tests — 2 tests (one per RPC-level concern)
// ---------------------------------------------------------------------------

// PTX_FilterWalletGMs populates pose-tracker fields from g_ptx_pose_tracker.
BOOST_AUTO_TEST_CASE(WalletFilter_GMs_PoseFieldsPopulated)
{
    auto [key, script] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(key);

    // Seed the pose tracker: 3 tickets, still eligible.
    g_ptx_pose_tracker.AdvanceLotteryWindow();
    g_ptx_pose_tracker.RecordHonestParticipation("gm01:aabbccdd");
    g_ptx_pose_tracker.RecordHonestParticipation("gm01:aabbccdd");
    g_ptx_pose_tracker.RecordHonestParticipation("gm01:aabbccdd");

    CDeterministicGMList list;
    list.AddGM(MakeWalletDGM("gm01:aabbccdd", script, 1));

    auto result = PTX_FilterWalletGMs(ks, list, g_ptx_pose_tracker);
    BOOST_REQUIRE_EQUAL(result.size(), 1U);
    BOOST_CHECK_EQUAL(result[0].tickets,  3);
    BOOST_CHECK_EQUAL(result[0].eligible, true);

    // Cleanup: advance window to avoid polluting subsequent tests.
    g_ptx_pose_tracker.AdvanceLotteryWindow();
}

// PTX_FilterWalletSettlements preserves history order (caller does newest-first reversal).
BOOST_AUTO_TEST_CASE(WalletFilter_Settlements_PreservesHistoryOrder)
{
    auto [key, script] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(key);

    // history is stored oldest-at-front (ring buffer order from LotteryState).
    std::vector<LastSettlement> history = {
        MakeSettlement(100, script, 1 * COIN),  // oldest
        MakeSettlement(200, script, 2 * COIN),  // newer
        MakeSettlement(300, script, 3 * COIN),  // newest
    };

    auto result = PTX_FilterWalletSettlements(ks, history);
    BOOST_REQUIRE_EQUAL(result.size(), 3U);
    // Order preserved — RPC layer will reverse; this checks the contract.
    BOOST_CHECK_EQUAL(result[0].height, 100);
    BOOST_CHECK_EQUAL(result[1].height, 200);
    BOOST_CHECK_EQUAL(result[2].height, 300);
}

BOOST_AUTO_TEST_SUITE_END()
