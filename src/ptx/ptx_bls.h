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

// GM-side BLS key share storage. Defined in ptx_bls.cpp; written ONLY through
// PTX_BLS_SetSkShare (the §C1 guarded setter) from its single write site,
// PTX_DKG_StoreSkShare (ptx_dkg.cpp), on ceremony completion. Under
// cs_ptx_my_bls_sk. No site writes these directly (that would bypass the guard).
extern uint8_t        g_ptx_my_bls_sk_bytes[32];
extern bool           g_ptx_my_bls_sk_set;
extern RecursiveMutex cs_ptx_my_bls_sk;

// blst has no global init requirement — no BLS::Init() needed.

// ---------------------------------------------------------------------------
// GM-side API
// ---------------------------------------------------------------------------

// §C1 replay guard (KDD-057; rationale updated KDD-069): the SINGLE guarded
// write path for the GM-side sk-share. Post-069 the only write site is
// PTX_DKG_StoreSkShare (the gm_bls_keyset RPC path was removed with the dealer);
// the guard now protects against ceremony replay / double-store, not coordinator
// hijack. refuse-unless-empty: first-set stores; overwrite of an already-set
// share is REFUSED and err is set. No site may write g_ptx_my_bls_sk_bytes/_set
// directly. Returns true on store, false (with err) on refusal.
bool PTX_BLS_SetSkShare(const uint8_t sk_bytes[32], std::string& err);

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

// Verify the combined signature against an explicit group public key.
// group_pk_bytes: 48-byte compressed G1 (caller supplies the committed
//   group_pk from the ACTIVE quorum's CPTXQuorumRecord).
// Pure function — reads no global state (KDD-049, 2026-06-03).
bool PTX_BLS_Verify(const uint8_t group_pk_bytes[48],
                    const uint256& msg,
                    const uint8_t sig[PTX_SIG_BYTES]);

#endif // HEMIS_PTX_BLS_H
