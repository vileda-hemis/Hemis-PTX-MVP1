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
#include <set>
#include <string>
#include <vector>

class CEvoDB;

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

// KDD-070 P2: quorum_hashes whose share is held in MEMORY ONLY because its
// persist failed (the (b) degraded path). Pure runtime state — never serialized
// (a persisted share is by definition not memory-only). Cleared on wipe and on
// reconcile-discard. Lets a node REPORT its degraded state rather than relying
// on someone having seen a single ceremony-time ERROR line.
extern std::set<uint256>            g_ptx_memory_only_shares;

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

// ---------------------------------------------------------------------------
// KDD-070 P2 — persistence (evoDb RAW layer), startup reconciliation, wipe.
// ---------------------------------------------------------------------------

// Flat 41-byte wire form of a HeldShare: bytes[32] + formation_height[i32,LE] +
// role[u8] + promotion_height[i32,LE]. Round-trips all four fields. Pure.
std::vector<uint8_t> PTX_BLS_SerializeHeldShare(const HeldShare& hs);
bool PTX_BLS_DeserializeHeldShare(const std::vector<uint8_t>& blob, HeldShare& out);

// Persist one held share to evoDb's RAW DB (store-pending at FINALIZE has no
// block transaction to ride — Dash DB_QUORUM_SK_SHARE precedent). Keyed by
// quorum_hash under the DB_PTX_SKSHARE prefix. Returns the underlying Write
// result — the CALLER MUST check it (a swallowed failure leaves a share in
// memory that will not survive restart, the ODC-035 mode this package fixes).
bool PTX_BLS_PersistShare(CEvoDB& evoDb, const uint256& quorum_hash, const HeldShare& hs);

// Load all persisted shares from evoDb's RAW DB into g_ptx_my_shares. Called
// once on daemon start, AFTER chain load (§3 ordering). Returns the number of
// CORRUPT/undeserializable entries encountered (logged as ERRORs naming the
// quorum_hash, never swallowed) so the caller can distinguish never-had-a-share
// from had-one-and-it-is-unreadable. Corrupt entries are NOT loaded.
int PTX_BLS_LoadShares(CEvoDB& evoDb);

// Discard held shares whose quorum_hash is NOT in known_quorums (orphans); keeps
// live quorums' shares regardless of role; logs each discard by quorum_hash.
// If evoDb != nullptr, also ERASES each orphan from the RAW DB (else it would
// reload on next start). Returns discard count. The caller supplies known_quorums
// from the quorum store AFTER chain load. NOTE: an EMPTY known set discards EVERY
// share (the init-order failure mode in miniature — why reconciliation must run
// only after the store is populated, §3).
size_t PTX_BLS_ReconcileShares(const std::set<uint256>& known_quorums, CEvoDB* evoDb = nullptr);

// The set of quorum_hashes currently held (a snapshot, under the lock). Used by
// the startup orchestration to build the known set from the quorum store.
std::set<uint256> PTX_BLS_HeldQuorumHashes();

// Clear all held shares. Always clears the in-memory map; if evoDb != nullptr,
// ERASES all persisted RAW-DB entries by iterating the DB_PTX_SKSHARE prefix
// DIRECTLY (so corrupt/undeserializable entries cannot survive). Returns the
// number of in-memory shares cleared. NOT RPC-reachable — invoked only via the
// -ptxwipeshares startup flag.
size_t PTX_BLS_WipeShares(CEvoDB* evoDb);

// The RAW-DB key prefix for persisted shares (exposed for tests that inject a
// malformed blob to exercise the corrupt-entry path).
const std::string& PTX_BLS_ShareDBPrefix();

// Mark a held share as MEMORY-ONLY (its persist failed — the (b) degraded path).
// Idempotent. Called by StoreSkShare on a persist failure.
void PTX_BLS_MarkMemoryOnly(const uint256& quorum_hash);

// Snapshot of the quorum_hashes currently held in memory only (persist failed).
// A node reports these as its degraded state, alongside the LoadShares corrupt
// count — both distinguish "durable" from "held but will not survive restart".
std::set<uint256> PTX_BLS_MemoryOnlyShares();

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
