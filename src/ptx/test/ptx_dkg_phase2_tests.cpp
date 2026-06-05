// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.2-P2: Phase 2 (Complaint) + Phase 3 (Justify) unit tests.
// Direct-call tests — no daemon, no P2P.  P2P dispatch tested at W1.2-integration.
//
// Falsification discipline: every acceptance test paired with a mutation.
// Two load-bearing stub cycles (Correction C, report §6):
//   Cycle 1: FeldmanCheck always-false → T2-9 RED, T2-11 RED; T2-10 GREEN, T2-12 GREEN.
//   Cycle 2: FeldmanCheck always-true  → T2-10 RED; T2-9 GREEN, T2-11 GREEN, T2-12 GREEN.
//   All four of T2-9/10/11/12 run under each stub; full RED/GREEN partition reported.
//
// T2-10/T2-22: the "wrong polynomial" scalar is explicitly verified to fail FeldmanCheck
//   BEFORE calling ReceivePhase3Msg, so the test proves it is genuinely exercising
//   Branch 3a and not accidentally taking Branch 2.  Mirrors T2-11's explicit precondition.
//
// T2-23: sign-hash binding — flip one byte of revealed_share, no re-sign → sig fails,
//   neither party in bad_members.  P3 analog of P1's T1-16.
//
// Test inventory (25 cases):
//   T2-HARNESS  Standalone Feldman: known poly, eval at j, check passes; +1 → fails
//   T2-index    share_index vs slot trap: FeldmanCheck(share, vvec, 3) passes, (share, vvec, 2) fails
//   T2-1        ReceivePhase2Msg: accepts valid complaint; stored in complaints_against
//   T2-2        ReceivePhase2Msg: rejects wrong quorum_hash
//   T2-3        ReceivePhase2Msg: rejects bad sig; complaints_against unchanged
//   T2-4        ReceivePhase2Msg: rejects duplicate (same complainant, dealer)
//   T2-5        ReceivePhase2Msg: rejects complaint from already-bad complainant (step 6 / FIX-1)
//   T2-6        ReceivePhase2Msg: rejects complaint against already-bad dealer (step 8)
//   T2-7        ReceivePhase2Msg: rejects complainant not in qual (step 5)
//   T2-8        ReceivePhase2Msg: rejects forged share_index_j (step 11)
//   T2-9        ReceivePhase3Msg Branch 2 (load-bearing): valid share → complainant bad, dealer stays
//   T2-10       ReceivePhase3Msg Branch 3a (load-bearing): invalid share → dealer bad, complainant stays
//   T2-11       ReceivePhase3Msg vvec-ground-truth / Branch 2 explicit: unique s satisfying vvec →
//                 complainant bad; asserts vvec is the adjudicator, not the P1 ciphertext
//   T2-12       ClosePhase3 Branch 3b sweep: complaint filed, no justify → dealer bad at ClosePhase3
//   T2-13       ReceivePhase3Msg: rejected when dealer already in bad_members (step 5)
//   T2-14       ReceivePhase3Msg: rejected when no outstanding complaint for (D,C) (step 7)
//   T2-15       ReceivePhase3Msg: rejected as duplicate — (D,C) already in justified_for (step 8)
//   T2-16       ReceivePhase3Msg: rejected for bad sig (step 9); neither bad_members entry appears
//   T2-17       Member bad in P1 cannot file P2 complaint (step 6 / FIX-1)
//   T2-18       Complainant bad from Branch 2 cannot file again (step 6; bad_members monotonic)
//   T2-19       ClosePhase2: advances COMPLAINT → JUSTIFY
//   T2-20       ClosePhase3 all-resolved → PREMIT
//   T2-21       ClosePhase3 effective-QUAL < 6 → ABORTED
//   T2-22       ClosePhase3 mixed: Branch 2 + Branch 3a + Branch 3b; all markings correct
//   T2-23       GetSignHash revealed_share binding: flip byte, no re-sign → sig rejection, neither bad

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"

#include "bls/bls_wrapper.h"
#include "random.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_phase2_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers (mirrors P1 pattern; static to this translation unit)
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

static void SetupFullPhase0Sessions(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    uint256 fbh = TestFormationHash();
    auto members = MakeTestMembers(key_map);

    sessions.resize(11);
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_InitSession(sessions[i], members, fbh, members[i].proTxHash));

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(sessions[i].my_idx >= 0);
        BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(sessions[i]));
    }

    std::vector<PTXDKGPhase0Msg> p0msgs(11);
    for (int i = 0; i < 11; i++) {
        const uint256& my_ptx = sessions[i].members[sessions[i].my_idx].proTxHash;
        p0msgs[i] = PTX_DKG_BuildPhase0Msg(sessions[i], key_map.at(my_ptx));
    }

    for (int recv = 0; recv < 11; recv++)
        for (int sender = 0; sender < 11; sender++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase0Msg(sessions[recv], p0msgs[sender]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase0(sessions[i]));
}

// proTxHash for "session i" (the member self-identifying as sessions[i])
static uint256 PtxOf(const std::vector<PTXDKGSession>& sessions, int i)
{
    return sessions[i].members[sessions[i].my_idx].proTxHash;
}

// Drive all 11 sessions through Phase 1 (all messages exchanged, all shares decrypted)
// and ClosePhase1 → all sessions in COMPLAINT phase with real vvecs and received_shares.
static void AdvanceToComplaint(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    SetupFullPhase0Sessions(key_map, sessions);

    std::vector<PTXDKGPhase1Msg> p1msgs(11);
    for (int i = 0; i < 11; i++)
        p1msgs[i] = PTX_DKG_BuildPhase1Msg(sessions[i], key_map.at(PtxOf(sessions, i)));

    for (int r = 0; r < 11; r++)
        for (int s = 0; s < 11; s++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(sessions[r], p1msgs[s]));

    for (int r = 0; r < 11; r++) {
        const uint256& rptx = PtxOf(sessions, r);
        for (int s = 0; s < 11; s++)
            PTX_DKG_DecryptMyShare(sessions[r], PtxOf(sessions, s), key_map.at(rptx));
    }

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase1(sessions[i]));
}

// File one complaint (sessions[0] → sessions[1]) to all sessions, ClosePhase2 on all.
// Post-state: all sessions in JUSTIFY phase; all have complaints_against[d1][c1].
static void SetupJustifySessions(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    AdvanceToComplaint(key_map, sessions);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(
        sessions[0], PtxOf(sessions, 1), key_map.at(PtxOf(sessions, 0)));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ReceivePhase2Msg(sessions[i], comp));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase2(sessions[i]));
}

// Derive the share_index j that ReceivePhase3Msg will use for (dealer, complainant)
// in a given session.  Mirrors the step-11 derivation in ptx_dkg.cpp.
static int DeriveJ(const PTXDKGSession& session, const uint256& complainant_ptx)
{
    for (int i = 0; i < (int)session.members.size(); i++)
        if (session.members[i].proTxHash == complainant_ptx)
            return session.members[i].share_index;
    return -1;
}

// ---------------------------------------------------------------------------
// T2-HARNESS — standalone Feldman check with a known polynomial
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_HARNESS_FeldmanStandalone)
{
    const int t = 6;
    const int j = 3;

    // Generate t random polynomial coefficients over Zr
    blst_fr coeffs[6];
    for (int k = 0; k < t; k++) {
        uint8_t ikm[32];
        GetStrongRandBytes(ikm, 32);
        blst_scalar tmp;
        blst_keygen(&tmp, ikm, 32, nullptr, 0);
        blst_fr_from_scalar(&coeffs[k], &tmp);
    }

    // Compute vvec: vvec[k] = g^{coeffs[k]}
    std::vector<blst_p1_affine> vvec(t);
    for (int k = 0; k < t; k++) {
        blst_scalar sk;
        blst_scalar_from_fr(&sk, &coeffs[k]);
        blst_p1 pt;
        blst_sk_to_pk_in_g1(&pt, &sk);
        blst_p1_to_affine(&vvec[k], &pt);
    }

    // Evaluate f(j) by Horner's method (mirrors GenerateLocalContrib)
    blst_fr j_fr;
    { uint64_t v[4] = {(uint64_t)j, 0, 0, 0}; blst_fr_from_uint64(&j_fr, v); }
    blst_fr eval = coeffs[0];
    blst_fr xi_pow;
    { const uint64_t one[4] = {1, 0, 0, 0}; blst_fr_from_uint64(&xi_pow, one); }
    for (int k = 1; k < t; k++) {
        blst_fr_mul(&xi_pow, &xi_pow, &j_fr);
        blst_fr term;
        blst_fr_mul(&term, &coeffs[k], &xi_pow);
        blst_fr_add(&eval, &eval, &term);
    }
    blst_scalar share;
    blst_scalar_from_fr(&share, &eval);

    // Correct share → check passes
    BOOST_CHECK(PTX_DKG_FeldmanCheck(share, vvec, j));

    // Mutate share +1 → check fails (falsification pair)
    blst_fr one_fr;
    { const uint64_t one[4] = {1, 0, 0, 0}; blst_fr_from_uint64(&one_fr, one); }
    blst_fr eval_bad;
    blst_fr_add(&eval_bad, &eval, &one_fr);
    blst_scalar bad_share;
    blst_scalar_from_fr(&bad_share, &eval_bad);
    BOOST_CHECK(!PTX_DKG_FeldmanCheck(bad_share, vvec, j));
}

// ---------------------------------------------------------------------------
// T2-index — share_index (1-indexed) vs my_idx (0-indexed slot) trap
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_index_ShareIndexVsSlotTrap)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    // Find the session with my_idx == 2 (share_index == 3)
    int target = -1;
    for (int i = 0; i < 11; i++)
        if (sessions[i].my_idx == 2) { target = i; break; }
    BOOST_REQUIRE(target >= 0);

    int dealer_i   = (target == 0) ? 1 : 0;
    uint256 dealer_ptx = PtxOf(sessions, dealer_i);

    BOOST_REQUIRE(sessions[target].received_shares.count(dealer_ptx));
    const blst_scalar& share = sessions[target].received_shares.at(dealer_ptx);
    BOOST_REQUIRE(sessions[target].phase1_vvecs.count(dealer_ptx));
    const auto& vvec = sessions[target].phase1_vvecs.at(dealer_ptx);

    int share_index = sessions[target].members[sessions[target].my_idx].share_index;
    int my_idx      = sessions[target].my_idx;

    BOOST_CHECK_EQUAL(share_index, 3); // slot 2 → 1-indexed share_index 3
    BOOST_CHECK_EQUAL(my_idx,      2); // 0-indexed slot

    // Correct basis (share_index) passes
    BOOST_CHECK(PTX_DKG_FeldmanCheck(share, vvec, share_index));
    // Wrong basis (my_idx, the slot) fails — the index-basis trap
    BOOST_CHECK(!PTX_DKG_FeldmanCheck(share, vvec, my_idx));
}

// ---------------------------------------------------------------------------
// T2-1 — ReceivePhase2Msg accepts valid complaint; stored in complaints_against
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_AcceptsValid)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));

    BOOST_CHECK(PTX_DKG_ReceivePhase2Msg(sessions[2], comp));
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 1u);
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 0u);
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(d_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-2 — rejects wrong quorum_hash
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_RejectsWrongQuorumHash)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));
    std::vector<unsigned char> ob(32, 0xDE);
    comp.quorum_hash = uint256(ob);
    // quorum_hash check (step 2) fires before sig check; no re-sign needed here

    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[2], comp));
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-3 — rejects bad sig; complaints_against unchanged  ← load-bearing (pair: T2-1)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_RejectsBadSig)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));

    std::vector<uint8_t> bytes = comp.sig.ToByteVector();
    bytes[0] ^= 0xFF;
    comp.sig.SetByteVector(bytes);

    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[2], comp));
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 0u);
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-4 — rejects duplicate (same complainant, dealer)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_RejectsDuplicate)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));

    BOOST_CHECK(PTX_DKG_ReceivePhase2Msg(sessions[2], comp));  // first: accepted
    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[2], comp)); // duplicate: rejected
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 1u); // stored once
}

// ---------------------------------------------------------------------------
// T2-5 — rejects complaint from already-bad complainant (step 6 / FIX-1)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_RejectsBadComplainant)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    sessions[2].bad_members.insert(c_ptx);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));

    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[2], comp));
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-6 — rejects complaint against already-bad dealer (step 8)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_RejectsBadDealer)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    sessions[2].bad_members.insert(d_ptx);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));

    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[2], comp));
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-7 — rejects complainant not in qual (step 5)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_RejectsComplainantNotInQual)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    sessions[2].qual.erase(c_ptx); // remove from receiver's qual

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));

    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[2], comp));
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 0u);
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 0u); // qual-reject, not bad-marked
}

// ---------------------------------------------------------------------------
// T2-8 — rejects forged share_index_j (step 11)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_RejectsForgedShareIndex)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));
    comp.share_index_j = comp.share_index_j + 1; // forge a different j
    comp.sig = key_map.at(c_ptx).Sign(comp.GetSignHash()); // re-sign to isolate step 11

    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[2], comp));
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-9 — Branch 2 (load-bearing): valid justify → complainant bad, dealer stays
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive3_Branch2_ComplainantBad)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    // D reveals its actual eval at C's slot → FeldmanCheck passes → Branch 2
    PTXDKGPhase3Msg just = PTX_DKG_BuildPhase3Msg(sessions[1], c_ptx, key_map.at(d_ptx));

    BOOST_CHECK(PTX_DKG_ReceivePhase3Msg(sessions[2], just));

    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 1u); // complainant bad
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(d_ptx), 0u); // dealer stays
    BOOST_CHECK_EQUAL(sessions[2].justified_for.at(d_ptx).count(c_ptx), 1u);
}

// ---------------------------------------------------------------------------
// T2-10 — Branch 3a (load-bearing): invalid justify → dealer bad, complainant stays
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive3_Branch3a_DealerBad)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    // Build valid justify, substitute a wrong-polynomial scalar, re-sign.
    // sessions[2].local_contrib.evals[0] is a valid field element but != f_D(j).
    PTXDKGPhase3Msg just = PTX_DKG_BuildPhase3Msg(sessions[1], c_ptx, key_map.at(d_ptx));
    just.revealed_share  = sessions[2].local_contrib.evals[0];
    just.sig             = key_map.at(d_ptx).Sign(just.GetSignHash());

    // Pin the precondition: explicitly verify the wrong scalar FAILS Feldman
    // so the test provably exercises Branch 3a (not accidentally Branch 2).
    int j = DeriveJ(sessions[2], c_ptx);
    BOOST_REQUIRE(j > 0);
    BOOST_REQUIRE(sessions[2].phase1_vvecs.count(d_ptx));
    BOOST_REQUIRE(!PTX_DKG_FeldmanCheck(just.revealed_share,
                                         sessions[2].phase1_vvecs.at(d_ptx), j));

    BOOST_CHECK(PTX_DKG_ReceivePhase3Msg(sessions[2], just));

    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(d_ptx), 1u); // dealer bad
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 0u); // complainant stays
    BOOST_CHECK_EQUAL(sessions[2].justified_for.at(d_ptx).count(c_ptx), 1u);
}

// ---------------------------------------------------------------------------
// T2-11 — vvec-ground-truth / Branch 2 explicit
//
// D honest; revealed share IS the unique s satisfying g^s == Π vvec_D[k]^{j^k}.
// Verify FeldmanCheck PASSES as a precondition, then assert complainant bad, dealer stays.
// Explicitly states: the vvec is the adjudicator, not the P1 ciphertext.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive3_VvecGroundTruth_Branch2Explicit)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase3Msg just = PTX_DKG_BuildPhase3Msg(sessions[1], c_ptx, key_map.at(d_ptx));

    // Derive j — same derivation as ReceivePhase3Msg step 11
    int j = DeriveJ(sessions[2], c_ptx);
    BOOST_REQUIRE(j > 0);
    BOOST_REQUIRE(sessions[2].phase1_vvecs.count(d_ptx));

    // Pin the precondition: revealed share satisfies the vvec commitment
    // (vvec IS the adjudicator — not the P1-transmitted ciphertext)
    BOOST_REQUIRE(PTX_DKG_FeldmanCheck(just.revealed_share,
                                        sessions[2].phase1_vvecs.at(d_ptx), j));

    BOOST_CHECK(PTX_DKG_ReceivePhase3Msg(sessions[2], just));

    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 1u); // complainant bad
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(d_ptx), 0u); // dealer stays
}

// ---------------------------------------------------------------------------
// T2-12 — Branch 3b sweep: complaint filed, no justify → dealer bad at ClosePhase3
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_ClosePhase3_Branch3b_DealerBad)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    // No ReceivePhase3Msg — complaint is unresolved at ClosePhase3

    BOOST_CHECK(PTX_DKG_ClosePhase3(sessions[2]));

    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(d_ptx), 1u); // dealer bad (Branch 3b sweep)
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 0u); // complainant stays
    BOOST_CHECK(sessions[2].phase == PTXDKGPhase::PREMIT);        // 10 ≥ 6 threshold met
}

// ---------------------------------------------------------------------------
// T2-13 — justify rejected when dealer already in bad_members (step 5)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive3_RejectsDealerAlreadyBad)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    sessions[2].bad_members.insert(d_ptx);

    PTXDKGPhase3Msg just = PTX_DKG_BuildPhase3Msg(sessions[1], c_ptx, key_map.at(d_ptx));

    BOOST_CHECK(!PTX_DKG_ReceivePhase3Msg(sessions[2], just));
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 0u);
    BOOST_CHECK_EQUAL(sessions[2].justified_for[d_ptx].count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-14 — justify rejected when no outstanding complaint for (D,C) (step 7)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive3_RejectsNoComplaint)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 d_ptx  = PtxOf(sessions, 1);
    uint256 c2_ptx = PtxOf(sessions, 2); // never complained against D

    // Construct a justify for (D, C2) with no complaint on record in receiver.
    int c2_slot = -1;
    for (int i = 0; i < (int)sessions[1].members.size(); i++)
        if (sessions[1].members[i].proTxHash == c2_ptx) { c2_slot = i; break; }
    BOOST_REQUIRE(c2_slot >= 0);

    PTXDKGPhase3Msg just;
    just.quorum_hash           = sessions[2].quorum_hash;
    just.dealer_proTxHash      = d_ptx;
    just.complainant_proTxHash = c2_ptx;
    just.revealed_share        = sessions[1].local_contrib.evals[c2_slot];
    just.sig                   = key_map.at(d_ptx).Sign(just.GetSignHash());

    BOOST_CHECK(!PTX_DKG_ReceivePhase3Msg(sessions[2], just)); // step 7 rejects
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(d_ptx),  0u);
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c2_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-15 — justify rejected as duplicate: (D,C) already in justified_for (step 8)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive3_RejectsDuplicate)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase3Msg just = PTX_DKG_BuildPhase3Msg(sessions[1], c_ptx, key_map.at(d_ptx));

    BOOST_CHECK(PTX_DKG_ReceivePhase3Msg(sessions[2], just));  // first: accepted
    BOOST_CHECK(!PTX_DKG_ReceivePhase3Msg(sessions[2], just)); // duplicate: rejected (step 8)
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 1u); // from first accept; unchanged
    BOOST_CHECK_EQUAL(sessions[2].justified_for.at(d_ptx).count(c_ptx), 1u); // stored once
}

// ---------------------------------------------------------------------------
// T2-16 — justify rejected for bad sig (step 9); neither bad_members entry appears
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive3_RejectsBadSig)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase3Msg just = PTX_DKG_BuildPhase3Msg(sessions[1], c_ptx, key_map.at(d_ptx));

    std::vector<uint8_t> bytes = just.sig.ToByteVector();
    bytes[0] ^= 0xFF;
    just.sig.SetByteVector(bytes);

    BOOST_CHECK(!PTX_DKG_ReceivePhase3Msg(sessions[2], just));
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 0u);
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(d_ptx), 0u);
    BOOST_CHECK_EQUAL(sessions[2].justified_for[d_ptx].count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-17 — member bad in P1 cannot file P2 complaint (step 6 / FIX-1)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_P1BadMemberCannotComplain)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    // Simulate Phase 1 bad-marking in receiver (e.g. non-revealer or commitment mismatch)
    sessions[2].bad_members.insert(c_ptx);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));

    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[2], comp)); // step 6 rejects
    BOOST_CHECK_EQUAL(sessions[2].complaints_against[d_ptx].count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-18 — complainant bad from Branch 2 cannot file again (bad_members monotonic)
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive2_Branch2BadComplainantCannotFile)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx  = PtxOf(sessions, 0);
    uint256 d2_ptx = PtxOf(sessions, 2); // different dealer C wants to complain against

    // Simulate Branch 2 having already marked C bad in receiver sessions[3]
    sessions[3].bad_members.insert(c_ptx);

    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d2_ptx, key_map.at(c_ptx));

    BOOST_CHECK(!PTX_DKG_ReceivePhase2Msg(sessions[3], comp)); // step 6 rejects
    BOOST_CHECK_EQUAL(sessions[3].complaints_against[d2_ptx].count(c_ptx), 0u);
}

// ---------------------------------------------------------------------------
// T2-19 — ClosePhase2 advances COMPLAINT → JUSTIFY
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_ClosePhase2_AdvancesToJustify)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    BOOST_REQUIRE(sessions[0].phase == PTXDKGPhase::COMPLAINT);
    BOOST_CHECK(PTX_DKG_ClosePhase2(sessions[0]));
    BOOST_CHECK(sessions[0].phase == PTXDKGPhase::JUSTIFY);
}

// ---------------------------------------------------------------------------
// T2-20 — ClosePhase3 all-resolved → PREMIT
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_ClosePhase3_AllResolved_Premit)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    // D justifies for C → (D,C) enters justified_for; sweep finds no unresolved
    PTXDKGPhase3Msg just = PTX_DKG_BuildPhase3Msg(sessions[1], c_ptx, key_map.at(d_ptx));
    BOOST_REQUIRE(PTX_DKG_ReceivePhase3Msg(sessions[2], just));

    BOOST_CHECK(PTX_DKG_ClosePhase3(sessions[2]));
    BOOST_CHECK(sessions[2].phase == PTXDKGPhase::PREMIT);
}

// ---------------------------------------------------------------------------
// T2-21 — ClosePhase3 effective-QUAL < 6 → ABORTED
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_ClosePhase3_BelowThreshold_Aborted)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGSession& recv = sessions[9];

    // File complaint C → D, close Phase 2 on recv
    PTXDKGPhase2Msg comp = PTX_DKG_BuildPhase2Msg(sessions[0], d_ptx, key_map.at(c_ptx));
    BOOST_REQUIRE(PTX_DKG_ReceivePhase2Msg(recv, comp));
    BOOST_REQUIRE(PTX_DKG_ClosePhase2(recv));

    // Pre-populate 5 more bad members (slots 2..6); sweep adds D (slot 1) → total 6
    // effective = 11 - 6 = 5 < t=6 → ABORTED
    for (int i = 2; i <= 6; i++)
        recv.bad_members.insert(PtxOf(sessions, i));

    BOOST_CHECK(!PTX_DKG_ClosePhase3(recv));
    BOOST_CHECK(recv.phase == PTXDKGPhase::ABORTED);
    BOOST_CHECK_EQUAL(recv.bad_members.count(d_ptx), 1u); // sweep added D
}

// ---------------------------------------------------------------------------
// T2-22 — mixed: Branch 2 + Branch 3a + Branch 3b in one session
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_ClosePhase3_Mixed_AllThreeBranches)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToComplaint(key_map, sessions);

    // C1→D1: D1 justifies correctly → C1 bad (Branch 2)
    // C2→D2: D2 justifies with wrong scalar → D2 bad (Branch 3a)
    // C3→D3: D3 does not justify → D3 bad at sweep (Branch 3b)
    uint256 c1_ptx = PtxOf(sessions, 0); uint256 d1_ptx = PtxOf(sessions, 1);
    uint256 c2_ptx = PtxOf(sessions, 2); uint256 d2_ptx = PtxOf(sessions, 3);
    uint256 c3_ptx = PtxOf(sessions, 4); uint256 d3_ptx = PtxOf(sessions, 5);

    PTXDKGSession& recv = sessions[6];

    // File three complaints to all sessions, advance all to JUSTIFY
    auto comp1 = PTX_DKG_BuildPhase2Msg(sessions[0], d1_ptx, key_map.at(c1_ptx));
    auto comp2 = PTX_DKG_BuildPhase2Msg(sessions[2], d2_ptx, key_map.at(c2_ptx));
    auto comp3 = PTX_DKG_BuildPhase2Msg(sessions[4], d3_ptx, key_map.at(c3_ptx));
    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ReceivePhase2Msg(sessions[i], comp1));
        BOOST_REQUIRE(PTX_DKG_ReceivePhase2Msg(sessions[i], comp2));
        BOOST_REQUIRE(PTX_DKG_ReceivePhase2Msg(sessions[i], comp3));
    }
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase2(sessions[i]));

    // Branch 2: D1 reveals correct eval for C1 → C1 bad
    auto just1 = PTX_DKG_BuildPhase3Msg(sessions[1], c1_ptx, key_map.at(d1_ptx));
    BOOST_CHECK(PTX_DKG_ReceivePhase3Msg(recv, just1));

    // Branch 3a: D2 reveals wrong-polynomial scalar (re-signed) → D2 bad
    auto just2 = PTX_DKG_BuildPhase3Msg(sessions[3], c2_ptx, key_map.at(d2_ptx));
    just2.revealed_share = sessions[7].local_contrib.evals[0]; // wrong polynomial
    just2.sig = key_map.at(d2_ptx).Sign(just2.GetSignHash());

    // Pin precondition: wrong scalar FAILS Feldman — confirms Branch 3a is exercised
    int j2 = DeriveJ(recv, c2_ptx);
    BOOST_REQUIRE(j2 > 0);
    BOOST_REQUIRE(recv.phase1_vvecs.count(d2_ptx));
    BOOST_REQUIRE(!PTX_DKG_FeldmanCheck(just2.revealed_share,
                                         recv.phase1_vvecs.at(d2_ptx), j2));

    BOOST_CHECK(PTX_DKG_ReceivePhase3Msg(recv, just2));

    // Branch 3b: D3 does not justify — swept at ClosePhase3

    BOOST_CHECK(PTX_DKG_ClosePhase3(recv));

    BOOST_CHECK_EQUAL(recv.bad_members.count(c1_ptx), 1u); // Branch 2: C1 bad
    BOOST_CHECK_EQUAL(recv.bad_members.count(d1_ptx), 0u); // D1 stays
    BOOST_CHECK_EQUAL(recv.bad_members.count(d2_ptx), 1u); // Branch 3a: D2 bad
    BOOST_CHECK_EQUAL(recv.bad_members.count(c2_ptx), 0u); // C2 stays
    BOOST_CHECK_EQUAL(recv.bad_members.count(d3_ptx), 1u); // Branch 3b: D3 bad
    BOOST_CHECK_EQUAL(recv.bad_members.count(c3_ptx), 0u); // C3 stays
    BOOST_CHECK(recv.phase == PTXDKGPhase::PREMIT);         // effective = 8 ≥ 6
    BOOST_CHECK_EQUAL((int)recv.qual.size() - (int)recv.bad_members.size(), 8);
}

// ---------------------------------------------------------------------------
// T2-23 — GetSignHash revealed_share binding: flip byte, no re-sign → sig rejection
//
// blst_scalar is typedef struct { byte b[256/8]; } — b[0] is publicly accessible.
// Flip one byte to tamper the scalar; sig was computed over blst_bendian(revealed_share)
// so the tamper invalidates GetSignHash() → VerifyInsecure fails → neither party bad.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(DKGPhase2_Receive3_RevealedShareBinding_SigRejects)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    SetupJustifySessions(key_map, sessions);

    uint256 c_ptx = PtxOf(sessions, 0);
    uint256 d_ptx = PtxOf(sessions, 1);

    PTXDKGPhase3Msg just = PTX_DKG_BuildPhase3Msg(sessions[1], c_ptx, key_map.at(d_ptx));

    // Flip one byte of revealed_share WITHOUT re-signing.
    // GetSignHash() covers blst_bendian(revealed_share), so the original sig no
    // longer matches the new hash → VerifyInsecure fails at step 9.
    just.revealed_share.b[0] ^= 0x01;

    BOOST_CHECK(!PTX_DKG_ReceivePhase3Msg(sessions[2], just));

    // Rejection at sig check: neither party is marked bad
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(c_ptx), 0u);
    BOOST_CHECK_EQUAL(sessions[2].bad_members.count(d_ptx), 0u);
    BOOST_CHECK_EQUAL(sessions[2].justified_for[d_ptx].count(c_ptx), 0u);
}

BOOST_AUTO_TEST_SUITE_END()
