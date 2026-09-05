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
#include "validation.h"
#include "wallet/wallet.h"

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


// ---------------------------------------------------------------------------
// BUG-061 -- a ProRegTx could spend the very collateral it was registering.
//
// Coin selection had no reason not to: the collateral is a confirmed,
// correctly-sized, wallet-owned coin, and at 100 HMS it is exactly what
// AvailableCoins reaches for to pay a fee. On 2026-09-05 it did, and the
// resulting registration was invalid the moment it was built -- it named
// bbfeb633:1 as its collateral and spent bbfeb633:1 as its input. Its three
// siblings survived on OUTPUT ORDERING ALONE.
//
// The fix locks the collateral in ProTxRegister BEFORE FundSpecialTx, relying
// on AvailableCoins skipping locked coins (wallet.cpp:2504). This proves that
// mechanism on the real wallet path, and the third leg is the discriminator:
// re-running with fIncludeLocked=true must bring the coin BACK, which is what
// makes "it disappeared" evidence about the LOCK rather than about anything
// else the filter might have rejected it for.
// ---------------------------------------------------------------------------

// ★ Deliberately reuses the file's existing BasicTestingSetup-based fixture.
// A full TestingSetup would be the obvious choice and is WRONG here: it calls
// SetDataDir()+ClearDatadirCache() and then removes that tree on teardown, and
// ptx_pose_rpc_tests (BasicTestingSetup, no SetDataDir of its own) reads
// GetDataDir() -- so introducing the first TestingSetup into test_ptx left the
// cache pointing at a deleted directory and broke two pose tests that had
// nothing to do with this change. Depth is faked off a bare CBlockIndex
// instead, which is all GetDepthInMainChain() actually consults.
BOOST_FIXTURE_TEST_SUITE(ptx_collateral_lock_tests, PTXBeaTestingSetup)

BOOST_AUTO_TEST_CASE(Bug061_CollateralIsSelectableUntilItIsLocked)
{
    CWallet wallet("bug061", WalletDatabase::CreateDummy());
    {
        LOCK(wallet.cs_wallet);
        wallet.SetupSPKM(false);
    }

    // A key the wallet owns, and a 100 HMS P2PKH output to it: the collateral.
    CKey key;
    key.MakeNewKey(true);
    BOOST_CHECK(wallet.AddKeyPubKey(key, key.GetPubKey()));
    const CScript collScript = GetScriptForDestination(key.GetPubKey().GetID());

    CMutableTransaction mtx;
    mtx.nLockTime = 0;                      // final, so CheckFinalTx passes
    mtx.vin.emplace_back(COutPoint(UINT256_ZERO, 999));
    mtx.vout.emplace_back(CTxOut(100 * COIN, collScript));
    CTransactionRef txRef = MakeTransactionRef(CTransaction(mtx));

    CWalletTx wtx(&wallet, txRef);
    wallet.LoadToWallet(wtx);

    // Give it positive depth without a chain: GetDepthInMainChain() reads only
    // the wallet's last-processed height and the tx's recorded block height.
    static const uint256 fakeHash = uint256S("01");
    CBlockIndex fakeTip;
    fakeTip.nHeight = 101;
    fakeTip.phashBlock = &fakeHash;
    {
        LOCK(wallet.cs_wallet);
        wallet.SetLastBlockProcessed(&fakeTip);
        wallet.mapWallet.at(txRef->GetHash()).m_confirm =
                CWalletTx::Confirmation(CWalletTx::Status::CONFIRMED, 100, fakeHash, 0);
    }

    const COutPoint collateral(txRef->GetHash(), 0);
    CWallet::AvailableCoinsFilter filter;
    filter.fOnlySafe = false;
    filter.minDepth  = 0;

    auto contains = [&collateral](const std::vector<COutput>& v) {
        for (const COutput& o : v) {
            if (o.tx->GetHash() == collateral.hash && (unsigned int)o.i == collateral.n) return true;
        }
        return false;
    };

    // (a) RED -- the defect itself: with nothing locked the collateral is an
    //     ordinary candidate input, and coin selection can spend it to pay the
    //     fee of the very registration that names it.
    std::vector<COutput> before;
    wallet.AvailableCoins(&before, nullptr, filter);
    BOOST_CHECK_MESSAGE(contains(before),
        "pre-lock: the collateral must be a selectable input -- that IS the bug");

    // (b) GREEN -- the mechanism the fix relies on (wallet.cpp:2504).
    WITH_LOCK(wallet.cs_wallet, wallet.LockCoin(collateral); );
    std::vector<COutput> after;
    wallet.AvailableCoins(&after, nullptr, filter);
    BOOST_CHECK_MESSAGE(!contains(after),
        "post-lock: the collateral must NOT be selectable");

    // (c) DISCRIMINATOR -- prove the disappearance is the LOCK and nothing else.
    //     Without this, (b) would pass just as well if the filter had rejected
    //     the coin for an unrelated reason, and would be evidence of nothing.
    CWallet::AvailableCoinsFilter includeLocked = filter;
    includeLocked.fIncludeLocked = true;
    std::vector<COutput> relaxed;
    wallet.AvailableCoins(&relaxed, nullptr, includeLocked);
    BOOST_CHECK_MESSAGE(contains(relaxed),
        "fIncludeLocked=true must bring it back -- otherwise (b) proves nothing");
}

BOOST_AUTO_TEST_SUITE_END()
