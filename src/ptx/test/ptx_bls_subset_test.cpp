// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W3.1-Test1 — Known-polynomial subset exhaustion (direct-call).
// Gate: W1.2 (ceremony code) blocked until this passes in full.
//
// Validates PTX_BLS_Init -> PartialSign -> Recover -> Verify produces
// internally consistent output across all C(n,t) subsets, three polynomial
// configurations. No external oracle (IMP-D5: chiabls rejected — RELIC
// unreviewed, DST mismatch). PTX_BLS_Verify called with explicit group_pk
// (KDD-049).
//
// Direct-call test: calls PTX_BLS_Init with synthetic node IDs, no daemon,
// no accessor (KDD-050 — Test1 needs neither; the accessor is a Test2 concern,
// built at W1.2).
//
// Limitation: group_pk correctness (does PTX_BLS_Init produce the correct
// group key for the polynomial?) is not independently verified — in scope
// for W3.2 external audit.
//
// Harness integrity: harness_rejects_corruption() is called as the first
// statement of run_subset_exhaustion(), before any subset is processed. It
// is also exposed as BLSSubset_FalsificationCheck for named CI output. Both
// paths use BOOST_REQUIRE, so a broken harness produces explicit failures in
// every TV case — no TV can silently pass with a broken verify gate.
// Boost.Test execution order is not relied upon.

#include "test/test_Hemis.h"
#include "ptx/ptx_bls.h"

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_bls_subset_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Advances combo (0-indexed, size k, drawn from {0,...,n-1}) to next
// lexicographic combination. Returns false when all C(n,k) are exhausted.
static bool next_combination(std::vector<int>& c, int n)
{
    int k = (int)c.size();
    for (int i = k - 1; i >= 0; --i) {
        if (c[i] < n - k + i) {
            ++c[i];
            for (int j = i + 1; j < k; ++j)
                c[j] = c[j - 1] + 1;
            return true;
        }
    }
    return false;
}

// Build a uint256 whose 32 bytes are all `fill`.
static uint256 make_msg(uint8_t fill)
{
    uint256 m;
    memset(m.begin(), fill, 32);
    return m;
}

// Build "node_01".."node_NN" (zero-padded to 2 digits, alphabetical = numeric).
static std::vector<std::string> make_node_ids(int n)
{
    std::vector<std::string> ids;
    ids.reserve(n);
    for (int i = 1; i <= n; ++i) {
        char buf[12];
        snprintf(buf, sizeof(buf), "node_%02d", i);
        ids.emplace_back(buf);
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Harness integrity check.
//
// Uses already-initialised state (group_pk_bytes, share_bytes) from the
// caller. Signs with the first t shares, recovers, verifies the valid sig,
// then corrupts recovered[0] ^= 0x01 and verifies again. Returns true only
// if the corrupted signature is correctly rejected.
//
// Called as the first statement of run_subset_exhaustion() so that every TV
// case independently enforces this gate before processing any subset.
// Execution order independence: if this returns false, BOOST_REQUIRE aborts
// the calling TV case; no TV can silently pass with a broken harness.
// ---------------------------------------------------------------------------

static bool harness_rejects_corruption(
    const uint8_t                              group_pk_bytes[48],
    const uint256&                             msg,
    const std::vector<std::array<uint8_t,32>>& share_bytes,
    int                                        t)
{
    std::vector<int>                  indices;
    std::vector<std::vector<uint8_t>> partial_sigs;
    indices.reserve(t);
    partial_sigs.reserve(t);

    for (int i = 0; i < t; ++i) {
        uint8_t sig[PTX_SIG_BYTES];
        if (!PTX_BLS_PartialSign(share_bytes[i].data(), msg, sig))
            return false;
        indices.push_back(i + 1);
        partial_sigs.emplace_back(sig, sig + PTX_SIG_BYTES);
    }

    uint8_t recovered[PTX_SIG_BYTES];
    if (!PTX_BLS_Recover(indices, partial_sigs, recovered))
        return false;

    // Valid signature must be accepted.
    if (!PTX_BLS_Verify(group_pk_bytes, msg, recovered))
        return false;

    // Corrupt one byte: must now be rejected.
    recovered[0] ^= 0x01;
    return !PTX_BLS_Verify(group_pk_bytes, msg, recovered);
}

// ---------------------------------------------------------------------------
// Falsification check — standalone case for named CI output only.
// Enforcement of the harness gate lives in run_subset_exhaustion().
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(BLSSubset_FalsificationCheck)
{
    const int t = 6, n = 11;
    std::vector<std::string> ids = make_node_ids(n);
    BOOST_REQUIRE_MESSAGE(PTX_BLS_Init(ids, t),
                          "FalsificationCheck: PTX_BLS_Init failed");

    uint8_t group_pk_bytes[48];
    std::vector<std::array<uint8_t, 32>> share_bytes(n);
    {
        LOCK(cs_ptx_bls);
        // Guard against append-instead-of-replace and OOB in the loop below.
        BOOST_REQUIRE_EQUAL((int)g_ptx_bls_state.shares.size(), n);
        blst_p1_affine_compress(group_pk_bytes, &g_ptx_bls_state.group_pk);
        for (int i = 0; i < n; ++i)
            blst_bendian_from_scalar(share_bytes[i].data(),
                                     &g_ptx_bls_state.shares[i]);
    }

    const uint256 msg = make_msg(0xAB);

    BOOST_REQUIRE_MESSAGE(
        harness_rejects_corruption(group_pk_bytes, msg, share_bytes, t),
        "FalsificationCheck: HARNESS BROKEN — PTX_BLS_Verify accepted a "
        "corrupted signature. All TV results are invalid.");
}

// ---------------------------------------------------------------------------
// Core subset runner.
// ---------------------------------------------------------------------------

static void run_subset_exhaustion(const char* tv_name,
                                  int t, int n, uint8_t msg_fill)
{
    std::vector<std::string> ids = make_node_ids(n);
    BOOST_REQUIRE_MESSAGE(PTX_BLS_Init(ids, t),
                          tv_name << ": PTX_BLS_Init failed");

    // Snapshot group_pk (48-byte compressed G1, KDD-049) and all n shares
    // under a single cs_ptx_bls lock.
    uint8_t group_pk_bytes[48];
    std::vector<std::array<uint8_t, 32>> share_bytes(n);
    {
        LOCK(cs_ptx_bls);
        // Guard against append-instead-of-replace across sequential
        // PTX_BLS_Init calls and against OOB in the extraction loop.
        BOOST_REQUIRE_EQUAL((int)g_ptx_bls_state.shares.size(), n);
        blst_p1_affine_compress(group_pk_bytes, &g_ptx_bls_state.group_pk);
        for (int i = 0; i < n; ++i)
            blst_bendian_from_scalar(share_bytes[i].data(),
                                     &g_ptx_bls_state.shares[i]);
    }

    const uint256 msg = make_msg(msg_fill);

    // Harness integrity gate: enforced before any subset is processed.
    // If PTX_BLS_Verify cannot reject a corrupted sig, all subset results
    // are meaningless — abort this case immediately.
    BOOST_REQUIRE_MESSAGE(
        harness_rejects_corruption(group_pk_bytes, msg, share_bytes, t),
        tv_name << ": HARNESS BROKEN — PTX_BLS_Verify accepted a corrupted "
                   "signature. Subset results invalid. W1.2 gate blocked.");

    // Reference signature from subset 0 — all subsequent subsets must
    // produce byte-identical output (the defining property of t-of-n BLS).
    uint8_t first_sig[PTX_SIG_BYTES];
    int subset_count = 0;

    // Initial combination: 0-indexed {0, 1, ..., t-1}.
    std::vector<int> combo(t);
    std::iota(combo.begin(), combo.end(), 0);

    do {
        std::vector<int>                  indices;
        std::vector<std::vector<uint8_t>> partial_sigs;
        indices.reserve(t);
        partial_sigs.reserve(t);

        for (int idx : combo) {
            indices.push_back(idx + 1);  // 1-indexed polynomial position
            uint8_t sig[PTX_SIG_BYTES];
            BOOST_REQUIRE_MESSAGE(
                PTX_BLS_PartialSign(share_bytes[idx].data(), msg, sig),
                tv_name << " subset " << subset_count
                        << ": PartialSign failed for share " << (idx + 1));
            partial_sigs.emplace_back(sig, sig + PTX_SIG_BYTES);
        }

        uint8_t recovered[PTX_SIG_BYTES];
        BOOST_REQUIRE_MESSAGE(
            PTX_BLS_Recover(indices, partial_sigs, recovered),
            tv_name << " subset " << subset_count << ": Recover failed");

        BOOST_REQUIRE_MESSAGE(
            PTX_BLS_Verify(group_pk_bytes, msg, recovered),
            tv_name << " subset " << subset_count << ": Verify returned false");

        if (subset_count == 0) {
            memcpy(first_sig, recovered, PTX_SIG_BYTES);
        } else {
            BOOST_REQUIRE_MESSAGE(
                memcmp(recovered, first_sig, PTX_SIG_BYTES) == 0,
                tv_name << " subset " << subset_count
                        << ": recovered_sig differs from subset 0");
        }

        ++subset_count;
    } while (next_combination(combo, n));

    BOOST_TEST_MESSAGE(tv_name << " (t=" << t << ",n=" << n << "): "
                       << subset_count << "/" << subset_count << " subsets PASS");
}

// ---------------------------------------------------------------------------
// TV-1: t=6, n=11 — production parameters (KDD-048). C(11,6) = 462 subsets.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(BLSSubset_TV1_t6_n11)
{
    run_subset_exhaustion("TV-1", 6, 11, 0xAB);
}

// ---------------------------------------------------------------------------
// TV-2: t=1, n=11 — degenerate lower bound. C(11,1) = 11 subsets.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(BLSSubset_TV2_t1_n11)
{
    run_subset_exhaustion("TV-2", 1, 11, 0xCD);
}

// ---------------------------------------------------------------------------
// TV-3: t=11, n=11 — degenerate upper bound. C(11,11) = 1 subset.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(BLSSubset_TV3_t11_n11)
{
    run_subset_exhaustion("TV-3", 11, 11, 0xEF);
}

BOOST_AUTO_TEST_SUITE_END()
