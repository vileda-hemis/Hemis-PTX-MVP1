// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// KDD-128 / KDD-129 (v0.5.0-testnet): the height-dependent PTX settlement cadence.
//
// What is under test is the ONE predicate every consensus site reads
// (PTXIsSettlementBoundary / PTXSettlementWindow(h) / PTXCadenceActive /
// PTXNextSettlementHeight) and the startup sanity that pins H to a boundary under
// BOTH windows. Params are constructed per network with CreateChainParams so the
// mainnet-inert property is asserted against the built tree, not read off a comment.
//
// Control legs (feedback: a RED whose legs all fail identically is vacuous): every
// "active" assertion on ptxtestnet is paired with the same height on a network that
// must NOT activate, and the bad-H sanity leg is paired with the good-H one.

#include "test/test_Hemis.h"

#include "chainparams.h"
#include "chainparamsbase.h"
#include "consensus/params.h"
#include "evo/specialtx_validation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "validation.h"

#include <boost/test/unit_test.hpp>
#include <memory>
#include <string>

namespace {
const int H = 15840;   // the ONE literal in chainparams.cpp; asserted here, not derived

std::unique_ptr<CChainParams> Net(const std::string& chain) { return CreateChainParams(chain); }
}

BOOST_FIXTURE_TEST_SUITE(ptx_cadence_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(Cadence_H_is_set_on_ptxtestnet_only)
{
    BOOST_CHECK_EQUAL(Net(CBaseChainParams::PTXTESTNET)->GetConsensus().nPTXCadenceActivationHeight, H);
    // Mainnet-inert, and inert on every other network: NO_ACTIVATION_HEIGHT.
    for (const std::string& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET,
                                     CBaseChainParams::REGTEST, CBaseChainParams::PTXBEATESTNET}) {
        BOOST_CHECK_MESSAGE(Net(chain)->GetConsensus().nPTXCadenceActivationHeight ==
                                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT,
                            chain + " must not carry a cadence activation height");
        BOOST_CHECK_MESSAGE(!Net(chain)->PTXCadenceActive(H), chain + " must not activate at H");
        BOOST_CHECK_MESSAGE(!Net(chain)->PTXCadenceActive(1 << 30), chain + " must never activate");
    }
}

BOOST_AUTO_TEST_CASE(Window_is_5_below_H_and_1440_from_H)
{
    auto p = Net(CBaseChainParams::PTXTESTNET);
    BOOST_CHECK_EQUAL(p->PTXSettlementWindow(0), 5);
    BOOST_CHECK_EQUAL(p->PTXSettlementWindow(H - 1), 5);
    BOOST_CHECK_EQUAL(p->PTXSettlementWindow(H), 1440);
    BOOST_CHECK_EQUAL(p->PTXSettlementWindow(H + 1), 1440);
    BOOST_CHECK(!p->PTXCadenceActive(H - 1));
    BOOST_CHECK(p->PTXCadenceActive(H));
    // Controls: the other networks keep ONE window at every height.
    auto m = Net(CBaseChainParams::MAIN);
    BOOST_CHECK_EQUAL(m->PTXSettlementWindow(0), 1440);
    BOOST_CHECK_EQUAL(m->PTXSettlementWindow(H), 1440);
    auto b = Net(CBaseChainParams::PTXBEATESTNET);
    BOOST_CHECK_EQUAL(b->PTXSettlementWindow(H - 1), 60);
    BOOST_CHECK_EQUAL(b->PTXSettlementWindow(H), 60);
}

BOOST_AUTO_TEST_CASE(Boundary_predicate_across_the_activation)
{
    auto p = Net(CBaseChainParams::PTXTESTNET);
    // Old window, below H: every 5th height.
    BOOST_CHECK(p->PTXIsSettlementBoundary(H - 5));
    BOOST_CHECK(!p->PTXIsSettlementBoundary(H - 4));
    BOOST_CHECK(!p->PTXIsSettlementBoundary(H - 1));
    // H itself: a boundary under BOTH windows (11 x 1440 = 3168 x 5).
    BOOST_CHECK(p->PTXIsSettlementBoundary(H));
    BOOST_CHECK_EQUAL(H % 5, 0);
    BOOST_CHECK_EQUAL(H % 1440, 0);
    // ★ The divergence point for a non-upgraded node: H+5 is a boundary under the
    // OLD window and must NOT be one under the new. This is the assertion that
    // makes "not upgrading before H means diverging" a measured statement.
    BOOST_CHECK(!p->PTXIsSettlementBoundary(H + 5));
    BOOST_CHECK(!p->PTXIsSettlementBoundary(H + 1439));
    BOOST_CHECK(p->PTXIsSettlementBoundary(H + 1440));
    // Control: on ptxbea the same heights answer by its single 60-window.
    auto b = Net(CBaseChainParams::PTXBEATESTNET);
    BOOST_CHECK(b->PTXIsSettlementBoundary(H));          // 15840 % 60 == 0
    BOOST_CHECK(!b->PTXIsSettlementBoundary(H + 5));
    BOOST_CHECK(b->PTXIsSettlementBoundary(H + 60));      // NOT a ptxtestnet boundary post-H
    BOOST_CHECK(!p->PTXIsSettlementBoundary(H + 60));
}

BOOST_AUTO_TEST_CASE(Next_settlement_height_across_the_activation)
{
    auto p = Net(CBaseChainParams::PTXTESTNET);
    BOOST_CHECK_EQUAL(p->PTXNextSettlementHeight(0), 5);
    BOOST_CHECK_EQUAL(p->PTXNextSettlementHeight(11043), 11045);   // the live RPC answer today
    BOOST_CHECK_EQUAL(p->PTXNextSettlementHeight(H - 6), H - 5);
    BOOST_CHECK_EQUAL(p->PTXNextSettlementHeight(H - 5), H);       // last old-window boundary -> H
    BOOST_CHECK_EQUAL(p->PTXNextSettlementHeight(H - 1), H);
    BOOST_CHECK_EQUAL(p->PTXNextSettlementHeight(H), H + 1440);     // NOT H+5
    BOOST_CHECK_EQUAL(p->PTXNextSettlementHeight(H + 5), H + 1440);
    BOOST_CHECK_EQUAL(p->PTXNextSettlementHeight(H + 1439), H + 1440);
    // The unit test's own "first boundary" idiom on the suite's network.
    auto b = Net(CBaseChainParams::PTXBEATESTNET);
    BOOST_CHECK_EQUAL(b->PTXNextSettlementHeight(0), 60);
    BOOST_CHECK_EQUAL(b->PTXNextSettlementHeight(H), H + 60);
    auto r = Net(CBaseChainParams::REGTEST);
    BOOST_CHECK_EQUAL(r->PTXNextSettlementHeight(0), 1440);
}

// The sanity check is what makes "H is a boundary under both windows" a property the
// daemon refuses to start without, rather than a fact about today's constant.
namespace {
struct CadenceParamsProbe : public CChainParams {
    CadenceParamsProbe(int h, int w1, int w2)
    {
        consensus.nPTXCadenceActivationHeight = h;
        nPTXSettlementWindow = w1;
        nPTXSettlementWindowV2 = w2;
    }
    const CCheckpointData& Checkpoints() const override { return checkpointData; }
    CCheckpointData checkpointData;
};
}

BOOST_AUTO_TEST_CASE(Sanity_refuses_H_that_is_not_a_boundary_under_both_windows)
{
    std::string err;
    // Every shipped network passes.
    for (const std::string& chain : {CBaseChainParams::MAIN, CBaseChainParams::TESTNET,
                                     CBaseChainParams::REGTEST, CBaseChainParams::PTXTESTNET,
                                     CBaseChainParams::PTXBEATESTNET}) {
        err.clear();
        BOOST_CHECK_MESSAGE(Net(chain)->PTXCheckCadenceParams(err), chain + ": " + err);
    }
    // GREEN control: the shipped shape, rebuilt by hand.
    BOOST_CHECK(CadenceParamsProbe(15840, 5, 1440).PTXCheckCadenceParams(err));
    // RED legs, each a different way to be wrong; each must be refused.
    BOOST_CHECK(!CadenceParamsProbe(15841, 5, 1440).PTXCheckCadenceParams(err));   // not a v1 boundary
    BOOST_CHECK(!CadenceParamsProbe(15845, 5, 1440).PTXCheckCadenceParams(err));   // v1 boundary, not v2
    BOOST_CHECK(!CadenceParamsProbe(15840, 5, 0).PTXCheckCadenceParams(err));      // v2 window unset
    BOOST_CHECK(!CadenceParamsProbe(0, 5, 1440).PTXCheckCadenceParams(err));       // H must be > 0
    // NO_ACTIVATION_HEIGHT is always fine, whatever the windows (the inert posture).
    BOOST_CHECK(CadenceParamsProbe(Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT, 5, 0).PTXCheckCadenceParams(err));
}

// ---------------------------------------------------------------------------
// THE SAFETY PROPERTY: "upgrade as early as you like" rests on v0.5.0 and v0.4.4
// computing IDENTICAL results at every height below H. v0.4.4 had no activation
// concept: the window was the bare constant 5 and every site computed
//     boundary  =  nHeight % 5 == 0                 (blockassembler.cpp:343, P9, P11)
//     next_at   =  height + (5 - height % 5)        (rpc/ptx.cpp:1523)
// Those two formulas are written out LITERALLY below — not derived from the new
// accessors — and the new accessors are required to agree with them at EVERY
// height in [0, H). Exhaustive, not sampled: a gated site that leaked below H by
// even one block fails this. The control leg proves the comparison discriminates:
// the same formulas must DISAGREE with v0.5.0 somewhere at/after H.
BOOST_AUTO_TEST_CASE(Agreement_below_H_is_exhaustive_against_the_v044_formulas)
{
    auto p = Net(CBaseChainParams::PTXTESTNET);
    const int W044 = 5;   // v0.4.4's nPTXSettlementWindow on ptxtestnet, KDD-030, as a literal
    int checked = 0, window_diff = 0, active_below = 0, boundary_diff = 0, next_diff = 0;
    for (int h = 0; h < H; ++h) {
        ++checked;
        if (p->PTXSettlementWindow(h) != W044) ++window_diff;
        if (p->PTXCadenceActive(h)) ++active_below;
        const bool old_boundary = (h % W044 == 0);
        if (p->PTXIsSettlementBoundary(h) != old_boundary) ++boundary_diff;
        const int old_next = h + (W044 - h % W044);
        if (p->PTXNextSettlementHeight(h) != old_next) ++next_diff;
    }
    BOOST_CHECK_EQUAL(checked, H);
    BOOST_CHECK_MESSAGE(window_diff == 0,   "window differs from v0.4.4 at " << window_diff << " heights below H");
    BOOST_CHECK_MESSAGE(active_below == 0,  "cadence reports ACTIVE at " << active_below << " heights below H");
    BOOST_CHECK_MESSAGE(boundary_diff == 0, "boundary predicate differs from v0.4.4 at " << boundary_diff << " heights below H");
    BOOST_CHECK_MESSAGE(next_diff == 0,     "next-settlement differs from v0.4.4 at " << next_diff << " heights below H");
    // Control: at/after H the v0.4.4 formulas must stop agreeing, or the loop above proves nothing.
    int post_boundary_diff = 0, post_next_diff = 0;
    for (int h = H; h < H + 2 * 1440; ++h) {
        if (p->PTXIsSettlementBoundary(h) != (h % W044 == 0)) ++post_boundary_diff;
        if (p->PTXNextSettlementHeight(h) != h + (W044 - h % W044)) ++post_next_diff;
    }
    BOOST_CHECK_MESSAGE(post_boundary_diff > 0, "control: v0.4.4 boundary formula still agrees after H -- comparison is vacuous");
    BOOST_CHECK_MESSAGE(post_next_diff > 0,     "control: v0.4.4 next-at formula still agrees after H -- comparison is vacuous");
    BOOST_CHECK(p->PTXIsSettlementBoundary(H + 5) != ((H + 5) % W044 == 0));   // the 15845 divergence, by name
}

BOOST_AUTO_TEST_SUITE_END()

// ---------------------------------------------------------------------------
// The same property through the CONSENSUS PATH, on ptxtestnet params selected as
// the global chain (the way the daemon runs): rule P9 (CheckPTXPayoutBlockRules)
// must answer at every height below H exactly what v0.4.4's `nHeight % 5 == 0`
// answered — accept a PTXPAYOUT at a multiple of 5, reject it otherwise — and
// must first disagree with that rule at H+5 = 15845. Standings (the ticket reset)
// cannot be driven here: that call site lives in ProcessSpecialTxsInBlock, which
// needs the chain fixture BUG-079 records as hanging in this build; its guard is
// the same PTXCadenceActive(h) predicate this suite proves false below H.
// ---------------------------------------------------------------------------
namespace {
struct PTXTestNetSetup : public BasicTestingSetup {
    PTXTestNetSetup() : BasicTestingSetup(CBaseChainParams::PTXTESTNET) {}
};
CMutableTransaction MinimalPayout()
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXPAYOUT;
    mtx.vin.push_back(CTxIn(COutPoint(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0)));
    CScript winner; winner << OP_DUP << OP_HASH160 << std::vector<unsigned char>(20, 0xAA) << OP_EQUALVERIFY << OP_CHECKSIG;
    mtx.vout.push_back(CTxOut(9000, winner));
    mtx.extraPayload.emplace();
    return mtx;
}
std::string P9At(int height)
{
    LOCK(cs_main);
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(MinimalPayout()));
    CValidationState state;
    CBlockIndex idx; idx.nHeight = height;
    if (CheckPTXPayoutBlockRules(block, &idx, state)) return "";
    return state.GetRejectReason();
}
}

BOOST_FIXTURE_TEST_SUITE(ptx_cadence_consensus_tests, PTXTestNetSetup)

BOOST_AUTO_TEST_CASE(P9_agrees_with_v044_below_H_and_diverges_at_H_plus_5)
{
    BOOST_REQUIRE_EQUAL(Params().NetworkIDString(), CBaseChainParams::PTXTESTNET);
    BOOST_REQUIRE_EQUAL(Params().GetConsensus().nPTXCadenceActivationHeight, H);
    // The block containing a payout is REJECTED at a boundary by nothing in P8/P9 — so an
    // empty reason means "P9 accepts this height", exactly v0.4.4's answer at h % 5 == 0.
    int mismatches = 0;
    for (int h = 1; h < H; ++h) {
        const std::string want = (h % 5 == 0) ? "" : "ptxpayout-wrong-height";
        if (P9At(h) != want) ++mismatches;
    }
    BOOST_CHECK_MESSAGE(mismatches == 0, "P9 differs from v0.4.4 at " << mismatches << " heights below H");
    BOOST_CHECK_EQUAL(P9At(H - 5), "");                         // last old-window boundary: accepted by both
    BOOST_CHECK_EQUAL(P9At(H),     "");                         // H: accepted by both (a boundary under both windows)
    BOOST_CHECK_EQUAL(P9At(H + 5), "ptxpayout-wrong-height");   // v0.4.4 would accept: THE divergence
    BOOST_CHECK_EQUAL(P9At(H + 1440), "");                      // 17280: the new cadence
    // Control leg: a genuinely wrong height below H is still rejected (P9 is live, not bypassed).
    BOOST_CHECK_EQUAL(P9At(H - 4), "ptxpayout-wrong-height");
}

BOOST_AUTO_TEST_SUITE_END()
