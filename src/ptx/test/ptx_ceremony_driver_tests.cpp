// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// SG-2a: the ceremony DRIVER falsification harness.
//
// 11 driver-stepped nodes, each with its OWN CPTXCeremonyTransport (the real
// enqueue→drain→validate→dispatch pipeline, hand-stepped — the
// validate_relay_tests Harness pattern), joined by a DETERMINISTIC seeded
// scheduler that models the network: per-edge DROP (permanent — Option B; no
// retransmit in-suite, getdata-recovery owed to SG-2b), per-edge DELAY (in
// heights), and per-tick shuffled delivery/step ORDER (reorder).  Height is
// the harness-fake shared clock (the injectable seam).  Single-threaded BY
// DESIGN for determinism — the locking the session mutex fixes is therefore
// NOT exercised here (registered owed to SG-2d; the AssertLockNotHeld guards
// are the in-between tripwire).
//
// Test inventory (RED forms are STANDING rows asserting the failure shape —
// the harness proves its own red-capability on every run):
//   CD_R1_ConvergenceUnderReorderDelay   Green  3 seeds, delay 0-1, shuffled: 11 DONE, one group_pk
//   CD_R1b_PermanentEdgeDropFailSafe     Green  one P1 edge dropped: victim ABORTS at the P4
//                                               premit gate (fail-safe), 10 DONE one pk
//   CD_R2_ThresholdCarry                 Green  1 absent: 10 DONE one pk, absent not in qual
//   CD_R2red_NoDeadlineStalls            RED    absent + no reachable window end: ceremony
//                                               waits forever (all stuck in HASH_COMMIT)
//   CD_R3_EmptyComplaintAdvances         Green  honest run: zero complaints, COMPLAINT/JUSTIFY
//                                               advance on the window boundary alone
//   CD_R3red_NoDeadlineComplaintHangs    RED    no reachable P2 window end: all hang in COMPLAINT
//   CD_R4_BadShareExcluded               Green  corrupted eval → complaint → justify FAILS →
//                                               dealer in bad_members on ALL 11, 11 DONE one pk
//   CD_R4red_ComplaintSuppressed         RED    complaints dropped: dealer NOT excluded on
//                                               non-victims; victim aborts (SG-5 matrix preview)
//   CD_RWIDTH_UnderWidthDivergesToAbort  Floor  w_contrib=1 + delay-2 edges: victim closes P1 on
//                                               a divergent set → ABORTS at the P4 premit gate
//                                               (no divergent finalization); SAME schedule at
//                                               width 6 → 11 DONE (width > skew = safe margin)

#include "test/test_Hemis.h"

#include "ptx/ptx_bls.h"             // sk-share slot clear (per-process model)
#include "ptx/ptx_ceremony_driver.h"
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_dkg_net.h"
#include "ptx/ptx_dkg_pending.h"
#include "protocol.h"
#include "streams.h"
#include "version.h"

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_ceremony_driver_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers — copied verbatim from ptx_dkg_phase5_tests.cpp (the established
// copy-per-TU pattern).  If the phase5 helpers change, re-sync this block.
// ---------------------------------------------------------------------------

static std::vector<PTXDKGMember> MakeTestMembers(std::map<uint256, CBLSSecretKey>& key_map)
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

        m.confirmedHashWithProRegTxHash =
            PTX_DKG_ComputeInnerHash(m.proTxHash, m.confirmedHash);

        m.node_id = "gm" + std::to_string(i) + ":8080";

        CBLSSecretKey sk;
        sk.MakeNewKey();
        m.pubKeyOperator = sk.GetPublicKey();
        key_map[m.proTxHash] = sk;

        members.push_back(m);
    }
    return members;
}

static uint256 TestFormationHash()
{
    std::vector<unsigned char> buf(32, 0);
    buf[0] = 0xFB;
    buf[31] = 0x01;
    return uint256(buf);
}

// §C1 (KDD-057): the sk-share slot is process-global refuse-unless-empty.
// The harness models 11 separate processes in one binary — clear before each
// node's FINALIZE close (the phase5-TU precedent).
static void PTX_TEST_ClearSkShareSlot()
{
    LOCK(cs_ptx_my_bls_sk);
    g_ptx_my_bls_sk_set = false;
}

// ---------------------------------------------------------------------------
// The deterministic lossy scheduler
// ---------------------------------------------------------------------------

namespace {

constexpr int SUBS = 3; // sub-ticks per height (delivery/step interleaving)

struct HNode {
    std::shared_ptr<PTXDKGSession> session;
    PTXCeremonyDriverState st;
    CPTXCeremonyTransport tr;
    CBLSSecretKey sk;
    PTXStepResult last{PTXStepResult::WAITING};
    CMutableTransaction tx;
    bool absent{false};
};

struct Delivery {
    int from, to;
    std::string cmd;
    std::vector<unsigned char> raw;
};

struct HarnessCfg {
    uint32_t seed{1};
    PTXCeremonyDeadlines widths;      // window widths (the safety parameter)
    int F{1000};                       // formation height (the shared anchor)
    int run_heights{34};               // bounded run length
    std::set<int> absent;              // nodes that never start (threshold-carry)
    // permanent drop predicate (Option B — no retransmit in-suite)
    std::function<bool(int from, int to, const std::string& cmd)> drop;
    // per-message delivery delay in HEIGHTS (0 = next sub-tick)
    std::function<int(int from, int to, const std::string& cmd, std::mt19937& rng)> delay;
    // session tamper hook, invoked at the start of each height (nullptr = none)
    std::function<void(int h, std::deque<HNode>& nodes)> on_height;
};

struct HarnessResult {
    // Per node: final phase, terminal step result, compressed group_pk (only
    // meaningful when DONE), qual + bad_members snapshots.
    std::vector<PTXDKGPhase> phase;
    std::vector<PTXStepResult> last;
    std::vector<std::vector<unsigned char>> pk;
    std::vector<std::set<uint256>> qual, bad;
    int done_count{0}, aborted_count{0};
};

static std::vector<unsigned char> CompressPk(const PTXDKGSession& s)
{
    uint8_t b[48];
    blst_p1_affine_compress(b, &s.group_pk);
    return std::vector<unsigned char>(b, b + 48);
}

static HarnessResult RunCeremony(const HarnessCfg& cfg,
                                 std::map<uint256, CBLSSecretKey>* key_map_out = nullptr)
{
    PTX_DKG_ClearPendingTx();
    PTX_TEST_ClearSkShareSlot();

    std::map<uint256, CBLSSecretKey> key_map;
    auto members = MakeTestMembers(key_map);
    const uint256 fbh = TestFormationHash();

    std::deque<HNode> nodes; // deque: CPTXCeremonyTransport is non-movable
    for (int i = 0; i < 11; i++) {
        nodes.emplace_back();
        HNode& n = nodes.back();
        n.session = std::make_shared<PTXDKGSession>();
        BOOST_REQUIRE(PTX_DKG_InitSession(*n.session, members, fbh, members[i].proTxHash));
        BOOST_REQUIRE(n.session->my_idx == i);
        n.sk = key_map.at(members[i].proTxHash);
        n.tr.SetActiveSession(n.session);
        n.absent = cfg.absent.count(i) > 0;
    }

    std::mt19937 rng(cfg.seed);
    std::map<int, std::vector<Delivery>> due; // tick -> deliveries

    for (int h = cfg.F; h < cfg.F + cfg.run_heights; h++) {
        if (cfg.on_height) cfg.on_height(h, nodes);
        for (int sub = 0; sub < SUBS; sub++) {
            const int tick = (h - cfg.F) * SUBS + sub;

            // (a) deliver due messages, shuffled (reorder)
            auto dit = due.find(tick);
            if (dit != due.end()) {
                std::shuffle(dit->second.begin(), dit->second.end(), rng);
                for (const auto& d : dit->second) {
                    if (nodes[d.to].absent) continue;
                    CDataStream ds(d.raw, SER_NETWORK, PROTOCOL_VERSION);
                    nodes[d.to].tr.ProcessMessage(d.from, d.cmd, ds);
                }
                due.erase(dit);
            }

            // (b) pump + step every live node, shuffled (reorder)
            std::vector<int> order;
            for (int i = 0; i < 11; i++) order.push_back(i);
            std::shuffle(order.begin(), order.end(), rng);
            for (int i : order) {
                HNode& n = nodes[i];
                if (n.absent) continue;
                if (n.last == PTXStepResult::DONE || n.last == PTXStepResult::ABORTED)
                    continue;
                for (int ph = 0; ph < 5; ph++) n.tr.ProcessBatch(ph, 64);

                // per-process key-store model: empty slot before a FINALIZE close
                if (n.session->phase == PTXDKGPhase::FINALIZE) PTX_TEST_ClearSkShareSlot();

                std::vector<PTXCeremonyOutbound> obs;
                n.last = PTX_Ceremony_Step(*n.session, n.st, h, cfg.widths, n.sk,
                                           cfg.F, obs, n.tx);

                for (const auto& ob : obs) {
                    for (int to = 0; to < 11; to++) {
                        if (to == i || nodes[to].absent) continue;
                        if (cfg.drop && cfg.drop(i, to, ob.command)) continue;
                        const int d = cfg.delay ? cfg.delay(i, to, ob.command, rng) : 0;
                        due[tick + 1 + d * SUBS].push_back(Delivery{i, to, ob.command, ob.raw});
                    }
                }
            }
        }

        bool all_terminal = true;
        for (const auto& n : nodes)
            if (!n.absent && n.last != PTXStepResult::DONE && n.last != PTXStepResult::ABORTED)
                all_terminal = false;
        if (all_terminal) break;
    }

    HarnessResult r;
    for (int i = 0; i < 11; i++) {
        const HNode& n = nodes[i];
        r.phase.push_back(n.session->phase);
        r.last.push_back(n.last);
        r.pk.push_back(n.session->phase == PTXDKGPhase::DONE
                           ? CompressPk(*n.session) : std::vector<unsigned char>());
        r.qual.push_back(n.session->qual);
        r.bad.push_back(n.session->bad_members);
        if (n.last == PTXStepResult::DONE) r.done_count++;
        if (n.last == PTXStepResult::ABORTED) r.aborted_count++;
    }
    if (key_map_out) *key_map_out = key_map;
    PTX_DKG_ClearPendingTx();
    PTX_TEST_ClearSkShareSlot();
    return r;
}

// All DONE nodes must share ONE group_pk (the convergence assertion).
static void AssertOnePk(const HarnessResult& r)
{
    const std::vector<unsigned char>* first = nullptr;
    for (int i = 0; i < 11; i++) {
        if (r.last[i] != PTXStepResult::DONE) continue;
        if (!first) { first = &r.pk[i]; continue; }
        BOOST_CHECK(r.pk[i] == *first);
    }
}

static uint256 MemberPtx(int i)
{
    std::vector<unsigned char> buf(32, 0);
    buf[0] = (unsigned char)i;
    buf[1] = 0xAA;
    return uint256(buf);
}

} // namespace

// ---------------------------------------------------------------------------
// CD_R1 — convergence under reorder + delay (3 seeds)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_R1_ConvergenceUnderReorderDelay)
{
    for (uint32_t seed : {7u, 1912u, 90210u}) {
        HarnessCfg cfg;
        cfg.seed = seed;
        cfg.delay = [](int, int, const std::string&, std::mt19937& rng) {
            return (int)(rng() % 2); // 0-1 heights, well inside every window
        };
        HarnessResult r = RunCeremony(cfg);
        BOOST_CHECK_EQUAL(r.done_count, 11);
        BOOST_CHECK_EQUAL(r.aborted_count, 0);
        AssertOnePk(r);
        for (int i = 0; i < 11; i++)
            BOOST_CHECK_EQUAL((int)r.qual[i].size(), 11); // full qual everywhere
    }
}

// ---------------------------------------------------------------------------
// CD_R1b — permanent edge drop (Option B): the victim FAILS SAFE
//
// Drop dealer 3's P1 reveal to node 0 only.  Node 0 marks 3 a non-revealer at
// its P1 close → effective-QUAL 10 vs everyone else's 11 → its group_pk
// differs → the P4 premit-consistency gate leaves it with 1 < t matches →
// ABORT.  The other 10 finalize with one pk.  Documents: a single-edge
// permanent loss costs the VICTIM its share (fail-safe), never a wrong key —
// and the quorum still forms (threshold t=6 << 10).  getdata re-request
// (SG-2b owed) is what turns this into full recovery.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_R1b_PermanentEdgeDropFailSafe)
{
    HarnessCfg cfg;
    cfg.seed = 42;
    cfg.drop = [](int from, int to, const std::string& cmd) {
        return from == 3 && to == 0 && cmd == NetMsgType::PTXQCONTRIB;
    };
    HarnessResult r = RunCeremony(cfg);
    BOOST_CHECK(r.last[0] == PTXStepResult::ABORTED);
    BOOST_CHECK_EQUAL(r.done_count, 10);
    AssertOnePk(r);
    // The victim's divergent view: dealer 3 marked bad locally, nowhere else.
    BOOST_CHECK(r.bad[0].count(MemberPtx(3)) == 1);
    for (int i = 1; i < 11; i++)
        BOOST_CHECK(r.bad[i].empty());
}

// ---------------------------------------------------------------------------
// CD_R2 — threshold-carry: an absent member is excluded at the shared P0
// boundary and the ceremony completes with 10.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_R2_ThresholdCarry)
{
    HarnessCfg cfg;
    cfg.seed = 5;
    cfg.absent = {10};
    HarnessResult r = RunCeremony(cfg);
    BOOST_CHECK_EQUAL(r.done_count, 10);
    BOOST_CHECK_EQUAL(r.aborted_count, 0);
    AssertOnePk(r);
    for (int i = 0; i < 10; i++) {
        BOOST_CHECK_EQUAL((int)r.qual[i].size(), 10);
        BOOST_CHECK(r.qual[i].count(MemberPtx(10)) == 0); // absent never in qual
    }
}

// ---------------------------------------------------------------------------
// CD_R2red — the deadline is the rescuer (STANDING RED): with no reachable
// window end, an absent member stalls the ceremony forever.  This is the
// failure mode the windowed deadline exists to prevent — asserted in its RED
// shape so the row proves the harness CAN show a stall.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_R2red_NoDeadlineStalls)
{
    HarnessCfg cfg;
    cfg.seed = 5;
    cfg.absent = {10};
    cfg.widths.w_hashcommit = 100000; // deadline unreachable in the bounded run
    HarnessResult r = RunCeremony(cfg);
    BOOST_CHECK_EQUAL(r.done_count, 0);
    for (int i = 0; i < 10; i++)
        BOOST_CHECK(r.phase[i] == PTXDKGPhase::HASH_COMMIT); // stuck — waits forever
}

// ---------------------------------------------------------------------------
// CD_R3 — the empty-complaint round advances on the window boundary alone
// (no IsComplete exists for P2 — silence is a valid outcome).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_R3_EmptyComplaintAdvances)
{
    HarnessCfg cfg;
    cfg.seed = 11;
    HarnessResult r = RunCeremony(cfg);
    BOOST_CHECK_EQUAL(r.done_count, 11);
    AssertOnePk(r);
    for (int i = 0; i < 11; i++)
        BOOST_CHECK(r.bad[i].empty()); // honest run: nobody complained, nobody bad
}

// ---------------------------------------------------------------------------
// CD_R3red — (STANDING RED) with no reachable P2 window end the ceremony
// hangs in COMPLAINT forever: nothing count-completes an empty complaint
// round; only the deadline advances it.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_R3red_NoDeadlineComplaintHangs)
{
    HarnessCfg cfg;
    cfg.seed = 11;
    cfg.widths.w_complaint = 100000;
    HarnessResult r = RunCeremony(cfg);
    BOOST_CHECK_EQUAL(r.done_count, 0);
    for (int i = 0; i < 11; i++)
        BOOST_CHECK(r.phase[i] == PTXDKGPhase::COMPLAINT); // hung — deadline is the only exit
}

// ---------------------------------------------------------------------------
// CD_R4 — bad share → complaint → justification FAILS → dealer excluded on
// ALL 11 nodes; ceremony completes over effective-QUAL 10 with one pk.
//
// Injection: after dealer 2 generates its contribution (P0 window) but before
// its P1 reveal is built, its eval destined to member 7 is overwritten with
// the eval for member 6 — a well-formed scalar that fails member 7's Feldman
// check (vvec untouched, so the P0/P1 commitment binding still passes; only
// the share is wrong).  The dealer's justification necessarily reveals the
// same corrupted eval → every receiver's Feldman re-check fails → §15 Branch
// 3a: dealer bad EVERYWHERE (including the dealer's own session, via its
// self-delivered justification).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_R4_BadShareExcluded)
{
    HarnessCfg cfg;
    cfg.seed = 23;
    cfg.on_height = [](int h, std::deque<HNode>& nodes) {
        if (h != 1001) return; // contrib generated at F=1000; P1 built at F+6
        nodes[2].session->local_contrib.evals[7] = nodes[2].session->local_contrib.evals[6];
    };
    HarnessResult r = RunCeremony(cfg);
    BOOST_CHECK_EQUAL(r.done_count, 11); // dealer still finalizes (excluded as dealer, kept as member)
    AssertOnePk(r);
    for (int i = 0; i < 11; i++) {
        BOOST_CHECK(r.bad[i].count(MemberPtx(2)) == 1); // excluded EVERYWHERE
        BOOST_CHECK_EQUAL((int)r.bad[i].size(), 1);
    }
}

// ---------------------------------------------------------------------------
// CD_R4red — (STANDING RED / SG-5 preview) complaint SUPPRESSION: with all
// PTXQCOMPLAINT messages dropped, the bad dealer is NOT excluded on any
// non-victim node — the exclusion assertion fails in exactly the shape a
// broken complaint round would produce.  The victim (whose local complaint
// stands unresolved) sweeps the dealer bad at ClosePhase3, diverges, and
// fails safe at the P4 premit gate.  Full adversarial matrix: SG-5.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_R4red_ComplaintSuppressed)
{
    HarnessCfg cfg;
    cfg.seed = 23;
    cfg.on_height = [](int h, std::deque<HNode>& nodes) {
        if (h != 1001) return;
        nodes[2].session->local_contrib.evals[7] = nodes[2].session->local_contrib.evals[6];
    };
    cfg.drop = [](int, int, const std::string& cmd) {
        return cmd == NetMsgType::PTXQCOMPLAINT; // adversarial suppression
    };
    HarnessResult r = RunCeremony(cfg);
    // The RED shape: exclusion did NOT happen on the non-victims...
    for (int i = 0; i < 11; i++) {
        if (i == 7) continue;
        BOOST_CHECK(r.bad[i].count(MemberPtx(2)) == 0);
    }
    // ...the victim alone swept the dealer bad, diverged, and failed safe.
    BOOST_CHECK(r.bad[7].count(MemberPtx(2)) == 1);
    BOOST_CHECK(r.last[7] == PTXStepResult::ABORTED);
    BOOST_CHECK_EQUAL(r.done_count, 10);
    AssertOnePk(r);
}

// ---------------------------------------------------------------------------
// CD_RWIDTH — width is a SAFETY parameter (the under-width failure mode) and
// the P4 premit gate is the divergence FLOOR.
//
// Arm 1 (under-width): w_contrib=1 with delay-2 on dealers {1..5}→node 0.
// Node 0 reaches the P1 boundary missing five reveals the others have —
// closes on a DIVERGENT set (bad={1..5}, effective 6) → different group_pk →
// the P4 premit-consistency gate (≥t bytewise-equal) fails its view → ABORT.
// No node finalizes a divergent key: the floor converts width-divergence into
// minority abort, never silent wrong-key.
//
// Arm 2 (calibration): the SAME delivery schedule under the default width 6
// converges 11/11 — width > worst-case skew is the safe-operating condition.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(CD_RWIDTH_UnderWidthDivergesToAbort)
{
    auto delay_edges = [](int from, int to, const std::string& cmd, std::mt19937&) {
        if (cmd == NetMsgType::PTXQCONTRIB && to == 0 && from >= 1 && from <= 5)
            return 2; // lands past node 0's P1 boundary at width 1
        return 0;
    };

    // Arm 1: under-width → divergence detected (NOT all DONE) + floor holds.
    {
        HarnessCfg cfg;
        cfg.seed = 3;
        cfg.widths.w_contrib = 1;
        cfg.delay = delay_edges;
        HarnessResult r = RunCeremony(cfg);
        BOOST_CHECK(r.done_count < 11);                    // the naive convergence claim FAILS
        BOOST_CHECK(r.last[0] == PTXStepResult::ABORTED);  // the divergent node...
        BOOST_CHECK_EQUAL((int)r.bad[0].size(), 5);        // ...closed on a divergent set
        BOOST_CHECK_EQUAL(r.done_count, 10);
        AssertOnePk(r);                                    // FLOOR: one pk among all finalizers
        for (int i = 1; i < 11; i++)
            BOOST_CHECK(r.bad[i].empty());
    }

    // Arm 2: same schedule, default width — converges.  Width > skew = safe.
    {
        HarnessCfg cfg;
        cfg.seed = 3;
        cfg.delay = delay_edges;
        HarnessResult r = RunCeremony(cfg);
        BOOST_CHECK_EQUAL(r.done_count, 11);
        BOOST_CHECK_EQUAL(r.aborted_count, 0);
        AssertOnePk(r);
    }
}

BOOST_AUTO_TEST_SUITE_END()
