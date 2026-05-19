// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// PTX threshold BLS12-381 using supranational/blst (KDD-032)
// Scope: PTX only. src/bls/ (ChainLocks / LLMQ) is UNCHANGED.
// Wire format: quorum_sig = 96 bytes (compressed G2). Unchanged.

#ifndef HEMIS_PTX_BLS_H
#define HEMIS_PTX_BLS_H

#include "blst.h"   // src/blst/bindings/blst.h
#include "sync.h"
#include "uint256.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Domain separation tag — unique to Hemis PTX, prevents cross-protocol reuse.
extern const char* PTX_BLS_DST;

// Compressed G2 point = 96 bytes. Must match CProbabilisticTxPayload quorum_sig.
static const int PTX_SIG_BYTES = 96;

// Trusted-dealer DKG state (coordinator side).
// Coordinator generates the master polynomial f(x) of degree t-1 over Zr.
// Share for GM at 1-indexed position i = f(i).  Lagrange recovery at x=0
// yields f(0) = master_sk, whose paired G1 point is group_pk.
struct PTXBLSState {
    bool initialized = false;
    int  n           = 0;   // total GM count
    int  t           = 0;   // threshold
    blst_scalar        master_sk;
    blst_p1_affine     group_pk;
    std::vector<blst_scalar>    shares;      // shares[i] = f(i+1), 0-indexed
    std::map<std::string, int>  node_index;  // node_id -> 1-indexed position
};

extern PTXBLSState     g_ptx_bls_state;
extern RecursiveMutex  cs_ptx_bls;

// blst has no global init requirement — no BLS::Init() needed.

// ---------------------------------------------------------------------------
// Coordinator-side API
// ---------------------------------------------------------------------------

// Trusted-dealer DKG: generate master polynomial and per-GM key shares.
// Call once on ptx_roll() first call (lazy-init, as before).
// node_ids determines 1-indexed positions (alphabetical sort order).
bool PTX_BLS_Init(const std::vector<std::string>& node_ids, int threshold);

// Copy the 32-byte big-endian scalar share for node_id into sk_out.
// Used by PTX_FanOutKeySet to extract the share for a given GM.
bool PTX_BLS_GetShareBytes(const std::string& node_id, uint8_t sk_out[32]);

// Return the 1-indexed polynomial position for node_id (0 = not found).
int PTX_BLS_GetNodeIndex(const std::string& node_id);

// ---------------------------------------------------------------------------
// GM-side API
// ---------------------------------------------------------------------------

// Sign msg with a raw 32-byte blst scalar (the GM's stored share).
// Called by gm_bls_sign RPC handler on GM nodes.
bool PTX_BLS_PartialSign(const uint8_t sk_bytes[32], const uint256& msg,
                          uint8_t sig_out[PTX_SIG_BYTES]);

// ---------------------------------------------------------------------------
// Coordinator recovery / verification
// ---------------------------------------------------------------------------

// Lagrange interpolation: recover threshold sig from t partial sigs.
// indices: 1-indexed signer positions (matching polynomial evaluation points).
// partial_sigs: each element is PTX_SIG_BYTES compressed G2 bytes.
// combined_out: 96-byte result (the recovered threshold signature).
bool PTX_BLS_Recover(
    const std::vector<int>&                    indices,
    const std::vector<std::vector<uint8_t>>&   partial_sigs,
    uint8_t                                    combined_out[PTX_SIG_BYTES]);

// Compute beacon = SHA256(96-byte threshold sig). Unchanged from chiabls era.
uint256 PTX_BLS_SigToBeacon(const uint8_t sig[PTX_SIG_BYTES]);

// Verify the combined signature against group_pk.
// Used by ptx_roll() after recovery, and available to ptx_verify() RPC.
bool PTX_BLS_Verify(const uint256& msg, const uint8_t sig[PTX_SIG_BYTES]);

#endif // HEMIS_PTX_BLS_H
