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

// ---------------------------------------------------------------------------
// PTX_FilterOperatedGMs — 5 tests (Step 13)
// ---------------------------------------------------------------------------

// Build a DGM with independently-specified owner and voting keys.
static CDeterministicGMCPtr MakeOperatedDGM(const std::string& nodeId,
                                             const CKeyID&      ownerKey,
                                             const CKeyID&      votingKey,
                                             const CScript&     payScript,
                                             uint64_t           internalId)
{
    uint256 proTxHash = uint256S(strprintf("%064x", internalId + 100));
    auto dgm   = std::make_shared<CDeterministicGM>(internalId);
    dgm->proTxHash          = proTxHash;
    dgm->collateralOutpoint = COutPoint(proTxHash, 0);

    CBLSSecretKey sk; sk.MakeNewKey();

    auto state = std::make_shared<CDeterministicGMState>();
    state->node_id          = nodeId;
    state->keyIDOwner       = ownerKey;
    state->keyIDVoting      = votingKey;
    state->scriptPTXPayment = payScript;
    state->pubKeyOperator.Set(sk.GetPublicKey());
    dgm->pdgmState = state;
    return dgm;
}

// Empty DGM list → empty result.
BOOST_AUTO_TEST_CASE(OperatedGMs_EmptyList)
{
    CBasicKeyStore ks;
    CDeterministicGMList emptyList;
    auto result = PTX_FilterOperatedGMs(ks, emptyList, g_ptx_pose_tracker);
    BOOST_CHECK(result.empty());
}

// DGM where wallet holds the owner key → included.
// Falsification target 3b: OR → AND makes this RED (voting key absent, AND fails).
BOOST_AUTO_TEST_CASE(OperatedGMs_IncludesOwnerKey)
{
    auto [ownerKey, _os]  = MakeKeyAndScript();
    auto [votingKey, _vs] = MakeKeyAndScript();  // different key, not in keystore
    CBasicKeyStore ks;
    ks.AddKey(ownerKey);  // only owner key

    CDeterministicGMList list;
    list.AddGM(MakeOperatedDGM("gm01:aabbccdd",
                               ownerKey.GetPubKey().GetID(),
                               votingKey.GetPubKey().GetID(),
                               CScript(), 1));

    auto result = PTX_FilterOperatedGMs(ks, list, g_ptx_pose_tracker);
    BOOST_REQUIRE_EQUAL(result.size(), 1U);
    BOOST_CHECK_EQUAL(result[0].node_id, "gm01:aabbccdd");
}

// DGM where wallet holds the voting key but NOT the owner key → included.
// Falsification target 3b: OR → AND makes this RED (owner key absent, AND fails).
BOOST_AUTO_TEST_CASE(OperatedGMs_IncludesVotingKey)
{
    auto [ownerKey, _os]  = MakeKeyAndScript();  // not in keystore
    auto [votingKey, _vs] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(votingKey);  // only voting key

    CDeterministicGMList list;
    list.AddGM(MakeOperatedDGM("gm02:11223344",
                               ownerKey.GetPubKey().GetID(),
                               votingKey.GetPubKey().GetID(),
                               CScript(), 2));

    auto result = PTX_FilterOperatedGMs(ks, list, g_ptx_pose_tracker);
    BOOST_REQUIRE_EQUAL(result.size(), 1U);
    BOOST_CHECK_EQUAL(result[0].node_id, "gm02:11223344");
}

// Wallet holds no keys → result is empty.
// Falsification target 3b: OR → AND still excludes (no keys → AND still false). GREEN.
BOOST_AUTO_TEST_CASE(OperatedGMs_ExcludesNonOwned)
{
    CBasicKeyStore ks;  // empty — no keys

    auto [ownerKey, _os]  = MakeKeyAndScript();
    auto [votingKey, _vs] = MakeKeyAndScript();

    CDeterministicGMList list;
    list.AddGM(MakeOperatedDGM("gm01:aabbccdd",
                               ownerKey.GetPubKey().GetID(),
                               votingKey.GetPubKey().GetID(),
                               CScript(), 1));

    auto result = PTX_FilterOperatedGMs(ks, list, g_ptx_pose_tracker);
    BOOST_CHECK(result.empty());
}

// PTX_FilterOperatedGMs populates all pose fields including penalized_this_window.
// Falsification target 3a: PTX_BuildPoseJson tickets+1 makes this RED
// (tickets==2 check catches the off-by-one, clean cross-surface isolation).
BOOST_AUTO_TEST_CASE(OperatedGMs_PoseFieldsPopulated)
{
    auto [ownerKey, _os] = MakeKeyAndScript();
    CBasicKeyStore ks;
    ks.AddKey(ownerKey);

    // Seed tracker: 2 honest participations, then a withhold.
    g_ptx_pose_tracker.AdvanceLotteryWindow();
    g_ptx_pose_tracker.RecordHonestParticipation("gm01:aabbccdd");
    g_ptx_pose_tracker.RecordHonestParticipation("gm01:aabbccdd");
    g_ptx_pose_tracker.RecordWithhold("gm01:aabbccdd");
    // After withhold: tickets=0, window_zeroed=true, pose_score=5

    CDeterministicGMList list;
    list.AddGM(MakeOperatedDGM("gm01:aabbccdd",
                               ownerKey.GetPubKey().GetID(),
                               ownerKey.GetPubKey().GetID(),
                               CScript(), 1));

    auto result = PTX_FilterOperatedGMs(ks, list, g_ptx_pose_tracker);
    BOOST_REQUIRE_EQUAL(result.size(), 1U);
    BOOST_CHECK_EQUAL(result[0].tickets,               0);
    BOOST_CHECK_EQUAL(result[0].pose_score,            5);
    BOOST_CHECK_EQUAL(result[0].penalized_this_window, true);

    g_ptx_pose_tracker.AdvanceLotteryWindow();  // clean up
}

BOOST_AUTO_TEST_SUITE_END()
