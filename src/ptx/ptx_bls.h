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

// GM-side BLS key-share store (KDD-070 P1) — a keyed multi-share map replacing
// the single global. Keyed by quorum_hash: a GM may hold shares for more than
// one quorum, so the count is NOT bounded (the "one quorum per GM" limit is
// KDD-040's, a fact about today that W2.5 multi-quorum may relax — it is not an
// invariant of this type). Written ONLY through PTX_BLS_SetSkShare (the §C1
// guarded setter) from its single write site, PTX_DKG_StoreSkShare, on ceremony
// completion. Read for signing ONLY through PTX_BLS_GetCurrentShare. Under
// cs_ptx_my_bls_sk. No site touches the map directly.
enum class PTXShareRole : uint8_t {
    CURRENT             = 0,  // the servicing signer for its quorum
    PENDING             = 1,  // rotation successor, not yet promoted (P3)
    SUPERSEDED_RETAINED = 2,  // former CURRENT, retained for reorg undo (P4)
};

struct HeldShare {
    uint8_t      bytes[32]         = {};   // 32-byte big-endian blst scalar
    int          formation_height  = 0;    // height of the quorum's formation anchor
    PTXShareRole role              = PTXShareRole::CURRENT;
    int          promotion_height  = -1;   // set at promotion (P3); -1 until then
};

extern std::map<uint256, HeldShare> g_ptx_my_shares;  // key = quorum_hash
extern RecursiveMutex               cs_ptx_my_bls_sk;

// blst has no global init requirement — no BLS::Init() needed.

// ---------------------------------------------------------------------------
// GM-side API
// ---------------------------------------------------------------------------

// §C1 replay/double-store guard (KDD-057; rationale amended KDD-069; keyed
// KDD-070 P1): the SINGLE guarded write path for a GM-side sk-share. Post-069
// the only write site is PTX_DKG_StoreSkShare (the gm_bls_keyset RPC path was
// removed with the dealer); the guard protects against ceremony replay /
// double-store, not coordinator hijack. refuse-unless-empty PER quorum_hash: a
// first-set for a key stores (role CURRENT); a second write to the SAME key is
// REFUSED (err set). Distinct quorum_hashes coexist. Returns true on store,
// false (with err) on refusal.
bool PTX_BLS_SetSkShare(const uint256& quorum_hash, int formation_height,
                        const uint8_t sk_bytes[32], std::string& err);

// Read the CURRENT-role share for quorum_hash for signing (the SINGLE read path
// for the signing selection, §5). Returns true and copies 32 bytes into out iff
// a share is held for quorum_hash AND its role is CURRENT; false otherwise (not
// held, or held but not CURRENT — e.g. PENDING/SUPERSEDED in later packages).
// Never returns another quorum's share. Under cs_ptx_my_bls_sk.
bool PTX_BLS_GetCurrentShare(const uint256& quorum_hash, uint8_t out[32]);

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
