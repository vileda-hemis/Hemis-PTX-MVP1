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

#include <boost/test/unit_test.hpp>
#include <memory>
#include <string>

BOOST_FIXTURE_TEST_SUITE(ptx_cadence_tests, BasicTestingSetup)

namespace {
const int H = 15840;   // the ONE literal in chainparams.cpp; asserted here, not derived

std::unique_ptr<CChainParams> Net(const std::string& chain) { return CreateChainParams(chain); }
}

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

BOOST_AUTO_TEST_SUITE_END()
