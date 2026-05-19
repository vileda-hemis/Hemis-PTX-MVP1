// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_bls.h"

#include "crypto/sha256.h"
#include "logging.h"
#include "random.h"

#include <algorithm>
#include <cstring>

PTXBLSState    g_ptx_bls_state;
RecursiveMutex cs_ptx_bls;
const char*    PTX_BLS_DST = "BLS_SIG_HEMIS_PTX_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_";

// ---------------------------------------------------------------------------
// PTX_BLS_Init — trusted-dealer DKG
// ---------------------------------------------------------------------------

bool PTX_BLS_Init(const std::vector<std::string>& node_ids, int threshold)
{
    if (node_ids.empty() || threshold < 1 || threshold > (int)node_ids.size())
        return false;

    PTXBLSState state;
    state.n = (int)node_ids.size();
    state.t = threshold;

    // Assign 1-indexed positions by sorted order (deterministic).
    std::vector<std::string> sorted_ids = node_ids;
    std::sort(sorted_ids.begin(), sorted_ids.end());
    for (int i = 0; i < (int)sorted_ids.size(); i++)
        state.node_index[sorted_ids[i]] = i + 1;

    // Generate polynomial coefficients a[0]..a[t-1] over Zr.
    // f(x) = a[0] + a[1]*x + ... + a[t-1]*x^(t-1)
    // master secret = a[0] = f(0)
    std::vector<blst_fr> coeffs(threshold);
    for (int i = 0; i < threshold; i++) {
        uint8_t ikm[32];
        GetStrongRandBytes(ikm, 32);
        blst_scalar tmp;
        blst_keygen(&tmp, ikm, 32, nullptr, 0);
        blst_fr_from_scalar(&coeffs[i], &tmp);
        if (i == 0)
            blst_scalar_from_fr(&state.master_sk, &coeffs[0]);
    }

    // Group public key: G1 * master_sk
    uint8_t sk_bytes[32];
    blst_bendian_from_scalar(sk_bytes, &state.master_sk);
    blst_p1 pk_p1;
    blst_p1_mult(&pk_p1, blst_p1_generator(), sk_bytes, 256);
    blst_p1_to_affine(&state.group_pk, &pk_p1);

    // Compute per-GM shares: share[i] = f(i+1) for i in [0, n-1].
    state.shares.resize(state.n);
    for (int i = 0; i < state.n; i++) {
        int xi = i + 1;

        uint64_t xi_val[4] = {(uint64_t)xi, 0, 0, 0};
        blst_fr xi_fr;
        blst_fr_from_uint64(&xi_fr, xi_val);

        blst_fr share = coeffs[0];
        blst_fr xi_pow;
        { const uint64_t one[4] = {1,0,0,0}; blst_fr_from_uint64(&xi_pow, one); }

        for (int j = 1; j < threshold; j++) {
            blst_fr_mul(&xi_pow, &xi_pow, &xi_fr);   // xi^j
            blst_fr term;
            blst_fr_mul(&term, &coeffs[j], &xi_pow);
            blst_fr_add(&share, &share, &term);
        }
        blst_scalar_from_fr(&state.shares[i], &share);
    }

    state.initialized = true;

    LOCK(cs_ptx_bls);
    g_ptx_bls_state = std::move(state);
    LogPrintf("PTX BLS: initialized n=%d t=%d (blst/BLS12-381)\n",
              g_ptx_bls_state.n, g_ptx_bls_state.t);
    return true;
}

// ---------------------------------------------------------------------------
// PTX_BLS_GetShareBytes
// ---------------------------------------------------------------------------

bool PTX_BLS_GetShareBytes(const std::string& node_id, uint8_t sk_out[32])
{
    LOCK(cs_ptx_bls);
    auto it = g_ptx_bls_state.node_index.find(node_id);
    if (it == g_ptx_bls_state.node_index.end())
        return false;
    int idx = it->second - 1;  // 0-based
    if (idx < 0 || idx >= (int)g_ptx_bls_state.shares.size())
        return false;
    blst_bendian_from_scalar(sk_out, &g_ptx_bls_state.shares[idx]);
    return true;
}

// ---------------------------------------------------------------------------
// PTX_BLS_GetNodeIndex
// ---------------------------------------------------------------------------

int PTX_BLS_GetNodeIndex(const std::string& node_id)
{
    LOCK(cs_ptx_bls);
    auto it = g_ptx_bls_state.node_index.find(node_id);
    if (it == g_ptx_bls_state.node_index.end())
        return 0;
    return it->second;
}

// ---------------------------------------------------------------------------
// PTX_BLS_PartialSign — GM-side signing with raw 32-byte scalar
// ---------------------------------------------------------------------------

bool PTX_BLS_PartialSign(const uint8_t sk_bytes[32], const uint256& msg,
                          uint8_t sig_out[PTX_SIG_BYTES])
{
    blst_scalar sk;
    blst_scalar_from_bendian(&sk, sk_bytes);

    blst_p2 hash_point;
    blst_hash_to_g2(&hash_point,
                    msg.begin(), 32,
                    (const uint8_t*)PTX_BLS_DST, strlen(PTX_BLS_DST),
                    nullptr, 0);

    uint8_t sk_be[32];
    blst_bendian_from_scalar(sk_be, &sk);

    blst_p2 sig_p2;
    blst_p2_mult(&sig_p2, &hash_point, sk_be, 256);

    blst_p2_affine sig_affine;
    blst_p2_to_affine(&sig_affine, &sig_p2);
    blst_p2_affine_compress(sig_out, &sig_affine);
    return true;
}

// ---------------------------------------------------------------------------
// PTX_BLS_Recover — Lagrange interpolation in G2
// ---------------------------------------------------------------------------

bool PTX_BLS_Recover(
    const std::vector<int>&                   indices,
    const std::vector<std::vector<uint8_t>>&  partial_sigs,
    uint8_t                                   combined_out[PTX_SIG_BYTES])
{
    int t = (int)indices.size();
    if (t == 0 || partial_sigs.size() != (size_t)t)
        return false;

    // Decompress each partial signature.
    std::vector<blst_p2_affine> sigs(t);
    for (int i = 0; i < t; i++) {
        if ((int)partial_sigs[i].size() != PTX_SIG_BYTES)
            return false;
        if (blst_p2_uncompress(&sigs[i], partial_sigs[i].data()) != BLST_SUCCESS)
            return false;
    }

    // Compute combined = sum_i( lambda_i * sig_i ) in G2.
    // lambda_i = prod_{j≠i}( xj / (xj - xi) ) in Zr (Lagrange at x=0).
    blst_p2 combined;
    memset(&combined, 0, sizeof(combined));  // point at infinity

    for (int i = 0; i < t; i++) {
        uint64_t xi_val[4] = {(uint64_t)indices[i], 0, 0, 0};
        blst_fr xi;
        blst_fr_from_uint64(&xi, xi_val);

        blst_fr lambda;
        { const uint64_t one[4] = {1,0,0,0}; blst_fr_from_uint64(&lambda, one); }

        for (int j = 0; j < t; j++) {
            if (j == i) continue;

            uint64_t xj_val[4] = {(uint64_t)indices[j], 0, 0, 0};
            blst_fr xj;
            blst_fr_from_uint64(&xj, xj_val);

            blst_fr diff;
            blst_fr_sub(&diff, &xj, &xi);       // xj - xi

            blst_fr diff_inv;
            blst_fr_eucl_inverse(&diff_inv, &diff);  // 1/(xj - xi)

            blst_fr factor;
            blst_fr_mul(&factor, &xj, &diff_inv);    // xj/(xj - xi)
            blst_fr_mul(&lambda, &lambda, &factor);
        }

        blst_scalar lambda_scalar;
        blst_scalar_from_fr(&lambda_scalar, &lambda);
        uint8_t lambda_bytes[32];
        blst_bendian_from_scalar(lambda_bytes, &lambda_scalar);

        blst_p2 sig_jac, scaled;
        blst_p2_from_affine(&sig_jac, &sigs[i]);
        blst_p2_mult(&scaled, &sig_jac, lambda_bytes, 256);

        blst_p2_affine scaled_affine;
        blst_p2_to_affine(&scaled_affine, &scaled);
        blst_p2_add_or_double_affine(&combined, &combined, &scaled_affine);
    }

    blst_p2_affine combined_affine;
    blst_p2_to_affine(&combined_affine, &combined);
    blst_p2_affine_compress(combined_out, &combined_affine);
    return true;
}

// ---------------------------------------------------------------------------
// PTX_BLS_SigToBeacon — SHA256(96-byte sig). Unchanged from chiabls era.
// ---------------------------------------------------------------------------

uint256 PTX_BLS_SigToBeacon(const uint8_t sig[PTX_SIG_BYTES])
{
    uint256 beacon;
    CSHA256().Write(sig, PTX_SIG_BYTES).Finalize(beacon.begin());
    return beacon;
}

// ---------------------------------------------------------------------------
// PTX_BLS_Verify — pairing check against group_pk
// ---------------------------------------------------------------------------

bool PTX_BLS_Verify(const uint256& msg, const uint8_t sig[PTX_SIG_BYTES])
{
    LOCK(cs_ptx_bls);
    if (!g_ptx_bls_state.initialized) return false;

    blst_p2_affine sig_affine;
    if (blst_p2_uncompress(&sig_affine, sig) != BLST_SUCCESS) return false;

    blst_pairing* ctx = (blst_pairing*)malloc(blst_pairing_sizeof());
    if (!ctx) return false;

    blst_pairing_init(ctx, true,
                      (const uint8_t*)PTX_BLS_DST, strlen(PTX_BLS_DST));

    BLST_ERROR err = blst_pairing_chk_n_mul_n_aggr_pk_in_g1(
        ctx,
        &g_ptx_bls_state.group_pk, true,
        &sig_affine,               true,
        msg.begin(),               32,
        nullptr,                   0);

    blst_pairing_commit(ctx);
    bool valid = (err == BLST_SUCCESS) && blst_pairing_finalverify(ctx, nullptr);
    free(ctx);
    return valid;
}
