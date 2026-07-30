// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "bls/bls_wrapper.h"
#include "evo/deterministicgms.h"
#include "key.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_wallet.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "uint256.h"

#include <univalue.h>

#include <boost/test/unit_test.hpp>

// Forward-declare the RPC handler (external linkage in rpc/ptx.cpp).
UniValue ptx_pose_status(const JSONRPCRequest& request);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

static JSONRPCRequest MakeEmptyRequest()
{
    JSONRPCRequest req;
    req.fHelp   = false;
    req.params  = UniValue(UniValue::VARR);
    return req;
}

// Build a minimal DGM with the given node_id and scriptPTXPayment.
static CDeterministicGMCPtr MakePoseTestDGM(const std::string& nodeId,
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

static std::pair<CKey, CScript> MakeKeyAndScript()
{
    CKey key;
    key.MakeNewKey(/*fCompressed=*/true);
    CKeyID keyid = key.GetPubKey().GetID();
    CScript script = GetScriptForDestination(CTxDestination(keyid));
    return {key, script};
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

BOOST_FIXTURE_TEST_SUITE(ptx_pose_rpc_tests, PTXBeaTestingSetup)

// ptx_pose_status emits penalized_this_window=true after RecordWithhold.
// Falsification target 3a: PTX_BuildPoseJson tickets+1 makes
// GmPose_PoseFieldsFromTracker RED; this test checks window_zeroed mapping only.
BOOST_AUTO_TEST_CASE(PoseStatus_PenalizedThisWindowField)
{
    g_ptx_pose_tracker.AdvanceLotteryWindow();  // clean slate
    g_ptx_pose_tracker.RecordWithhold("node01:aabbccdd");

    UniValue result = ptx_pose_status(MakeEmptyRequest());
    BOOST_REQUIRE(result.isArray());

    bool found = false;
    for (size_t i = 0; i < result.size(); ++i) {
        const UniValue& entry = result[i];
        if (entry["node_id"].get_str() == "node01:aabbccdd") {
            BOOST_CHECK(entry["penalized_this_window"].getBool());
            found = true;
        }
    }
    BOOST_CHECK_MESSAGE(found, "node01:aabbccdd not found in ptx_pose_status output");

    g_ptx_pose_tracker.AdvanceLotteryWindow();  // clean up
}

// PTX_GetGMPoseDetail returns false for a node_id not in the DGM list.
BOOST_AUTO_TEST_CASE(GmPose_ThrowsOnUnknownNode)
{
    CDeterministicGMList emptyList;
    GMPoseDetail out;
    bool found = PTX_GetGMPoseDetail("unknown:12345678", emptyList, g_ptx_pose_tracker, out);
    BOOST_CHECK(!found);
}

// PTX_GetGMPoseDetail returns true with payment_configured=false when the DGM
// has no scriptPTXPayment set.
BOOST_AUTO_TEST_CASE(GmPose_KnownNodePaymentNotConfigured)
{
    CDeterministicGMList list;
    list.AddGM(MakePoseTestDGM("gm01:aabbccdd", CScript(), 1));  // empty payment script

    GMPoseDetail out;
    bool found = PTX_GetGMPoseDetail("gm01:aabbccdd", list, g_ptx_pose_tracker, out);
    BOOST_REQUIRE(found);
    BOOST_CHECK(!out.payment_configured);
}

// PTX_GetGMPoseDetail returns payment_configured=true when scriptPTXPayment is set.
BOOST_AUTO_TEST_CASE(GmPose_PaymentConfigured)
{
    auto [_key, payScript] = MakeKeyAndScript();
    CDeterministicGMList list;
    list.AddGM(MakePoseTestDGM("gm01:aabbccdd", payScript, 1));

    GMPoseDetail out;
    bool found = PTX_GetGMPoseDetail("gm01:aabbccdd", list, g_ptx_pose_tracker, out);
    BOOST_REQUIRE(found);
    BOOST_CHECK(out.payment_configured);
}

// ptx_pose_status emits the correct tickets value from the tracker.
// Falsification target 3a (revised): PTX_BuildPoseJson tickets+1 makes this RED.
// Gap-closure test: GmPose_PoseFieldsFromTracker tests the struct output, not JSON output;
// this test exercises PTX_BuildPoseJson's tickets mapping via the RPC JSON path.
BOOST_AUTO_TEST_CASE(PoseStatus_TicketsFieldMapped)
{
    g_ptx_pose_tracker.AdvanceLotteryWindow();
    g_ptx_pose_tracker.RecordHonestParticipation("node02:aabbccdd");
    g_ptx_pose_tracker.RecordHonestParticipation("node02:aabbccdd");
    g_ptx_pose_tracker.RecordHonestParticipation("node02:aabbccdd");

    UniValue result = ptx_pose_status(MakeEmptyRequest());
    BOOST_REQUIRE(result.isArray());

    bool found = false;
    for (size_t i = 0; i < result.size(); ++i) {
        const UniValue& entry = result[i];
        if (entry["node_id"].get_str() == "node02:aabbccdd") {
            BOOST_CHECK_EQUAL(entry["tickets"].get_int64(), 3);
            found = true;
        }
    }
    BOOST_CHECK_MESSAGE(found, "node02:aabbccdd not found in ptx_pose_status output");

    g_ptx_pose_tracker.AdvanceLotteryWindow();
}

// PTX_GetGMPoseDetail populates pose fields from the tracker correctly.
// (struct output test — complements PoseStatus_TicketsFieldMapped which tests JSON output)
BOOST_AUTO_TEST_CASE(GmPose_PoseFieldsFromTracker)
{
    g_ptx_pose_tracker.AdvanceLotteryWindow();  // clean slate
    g_ptx_pose_tracker.RecordHonestParticipation("gm01:aabbccdd");
    g_ptx_pose_tracker.RecordHonestParticipation("gm01:aabbccdd");

    CDeterministicGMList list;
    list.AddGM(MakePoseTestDGM("gm01:aabbccdd", CScript(), 1));

    GMPoseDetail out;
    bool found = PTX_GetGMPoseDetail("gm01:aabbccdd", list, g_ptx_pose_tracker, out);
    BOOST_REQUIRE(found);
    BOOST_CHECK_EQUAL(out.pose.lottery_tickets, 2);
    BOOST_CHECK(!out.pose.window_zeroed);

    g_ptx_pose_tracker.AdvanceLotteryWindow();  // clean up
}

// ---------------------------------------------------------------------------
// BUG-025: ptx_pose.dat is consensus-affecting DERIVED state that survived
// -reindex.  PoSe feeds PTXPAYOUT winner selection (P10/P11), the file is
// node-local and wall-clock-accumulated, it loads unconditionally at startup,
// and -reindex wiped only evoDb.  A replay therefore inherited records from
// heights it had not reached yet and judged committed blocks against the
// future — gm01's reindex rejected committed h480 (ptxpayout-wrong-recipient)
// against 88 stale records and forked at 479.
//
// Two cases: the inheritance MECHANISM (why the fix is needed) and the fix.
// ---------------------------------------------------------------------------

// Plant a ptx_pose.dat by hand, exactly as a pre-reindex run would have left it.
static void PlantPoseFile(const std::string& nodeId, int poseScore, bool eligible)
{
    UniValue rec(UniValue::VOBJ);
    rec.pushKV("node_id",         nodeId);
    rec.pushKV("pose_score",      poseScore);
    rec.pushKV("quorum_eligible", eligible);
    rec.pushKV("lottery_tickets", 7);
    rec.pushKV("window_zeroed",   false);
    UniValue arr(UniValue::VARR);
    arr.push_back(rec);
    UniValue root(UniValue::VOBJ);
    root.pushKV("records", arr);
    const std::string json = root.write(2);

    fs::path dat = GetDataDir() / "ptx_pose.dat";
    FILE* f = fopen(dat.string().c_str(), "w");
    BOOST_REQUIRE_MESSAGE(f != nullptr, "could not plant " + dat.string());
    BOOST_REQUIRE_EQUAL(fwrite(json.data(), 1, json.size(), f), json.size());
    fclose(f);
}

// The MECHANISM pin: a planted file really is inherited by Load(), and it
// really does change an eligibility verdict that P10/P11 consult.  If this
// ever stops holding, the fix below is guarding nothing and the pair is
// vacuous — keep both.
BOOST_AUTO_TEST_CASE(Bug025_StalePoseFileIsInherited)
{
    const std::string nid = "gm025:deadbeef";
    PlantPoseFile(nid, /*poseScore=*/100, /*eligible=*/false);

    g_ptx_pose_tracker.Load();

    // The replay now believes this node is banned — on evidence it has not
    // replayed yet.  This is the contamination, reproduced.
    BOOST_CHECK(!g_ptx_pose_tracker.IsEligible(nid));
    BOOST_CHECK_EQUAL(g_ptx_pose_tracker.GetRecord(nid).pose_score, 100);
}

// The FIX: ResetForReindex clears memory AND removes the file, so a replay
// starts from the chain alone.
BOOST_AUTO_TEST_CASE(Bug025_ResetForReindexClearsMemoryAndFile)
{
    const std::string nid = "gm025:feedface";
    PlantPoseFile(nid, /*poseScore=*/100, /*eligible=*/false);
    g_ptx_pose_tracker.Load();

    // Preconditions: the contamination is present in BOTH places.
    const fs::path dat = GetDataDir() / "ptx_pose.dat";
    BOOST_REQUIRE(fs::exists(dat));
    BOOST_REQUIRE(!g_ptx_pose_tracker.IsEligible(nid));

    g_ptx_pose_tracker.ResetForReindex();

    // In-memory cleared — the clear is what actually protects the replay,
    // because the startup Load() runs before the reindex flags are known.
    BOOST_CHECK(g_ptx_pose_tracker.GetAllRecords().empty());
    BOOST_CHECK(g_ptx_pose_tracker.IsEligible(nid));   // unknown ⇒ eligible
    // File removed — so a LATER restart cannot re-inherit it either.
    BOOST_CHECK(!fs::exists(dat));

    // And a fresh Load() over the removed file stays clean.
    g_ptx_pose_tracker.Load();
    BOOST_CHECK(g_ptx_pose_tracker.GetAllRecords().empty());
}

BOOST_AUTO_TEST_SUITE_END()
