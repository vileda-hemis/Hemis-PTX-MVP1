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
// KDD-040's — and the W2.5 relaxation KEPT it (KDD-040's W2.5 amendment:
// multi-quorum network, one-quorum-per-GM retained; only ODC-024
// multi-membership, still deferred, would change it) — it is not an
// invariant of this type). Written ONLY through PTX_BLS_SetSkShare (the §C1
// guarded setter) from its single write site, PTX_DKG_StoreSkShare, on ceremony
// completion. Read for signing ONLY through PTX_BLS_GetCurrentShare. Under
// cs_ptx_my_bls_sk. No site touches the map directly.
enum class PTXShareRole : uint8_t {
    CURRENT             = 0,  // the servicing signer for its quorum
    PENDING             = 1,  // rotation successor, not yet promoted (P3)
    SUPERSEDED_RETAINED = 2,  // former CURRENT, retained for reorg undo (P4)
    UNDONE_RETAINED     = 3,  // former CURRENT, promotion UNDONE, retained for redo (BUG-028)
};

struct HeldShare {
    uint8_t      bytes[32]         = {};   // 32-byte big-endian blst scalar
    int          formation_height  = 0;    // height of the quorum's formation anchor
    PTXShareRole role              = PTXShareRole::CURRENT;
    // The RETENTION CLOCK for both retained roles — "the height of the event that
    // put me in this role", and the basis for the depth-discard. For
    // SUPERSEDED_RETAINED it is the promotion's connect height (P3/P4); for
    // UNDONE_RETAINED it is the DISCONNECT height (BUG-028). One field, one
    // meaning, so the 41-byte flat form is unchanged.
    int          promotion_height  = -1;   // set at promotion/undo; -1 until then
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
                        const uint8_t sk_bytes[32], PTXShareRole role, std::string& err);

// CURRENT-role convenience overload (P1 callers). Forwards to the single guarded
// setter above — NOT a second write path.
inline bool PTX_BLS_SetSkShare(const uint256& quorum_hash, int formation_height,
                               const uint8_t sk_bytes[32], std::string& err)
{
    return PTX_BLS_SetSkShare(quorum_hash, formation_height, sk_bytes,
                              PTXShareRole::CURRENT, err);
}

// Read the CURRENT-role share for quorum_hash for signing (the SINGLE read path
// for the signing selection, §5). Returns true and copies 32 bytes into out iff
// a share is held for quorum_hash AND its role is CURRENT; false otherwise (not
// held, or held but not CURRENT — e.g. PENDING/SUPERSEDED in later packages).
// Never returns another quorum's share. Under cs_ptx_my_bls_sk.
bool PTX_BLS_GetCurrentShare(const uint256& quorum_hash, uint8_t out[32]);

// ---------------------------------------------------------------------------
// KDD-070 P2 — persistence, startup reconciliation, wipe.
// ODC-070 (2026-08-16): the backend is <datadir>/ptx_shares.dat, NOT evoDb —
// reindex wipes evoDb and the BUG-037 fleet recovery destroyed every node's
// shares that way.  Shares are operator secrets (not chain-derivable); the
// file is born 0600 under the daemon's umask(077), and operators must treat
// it like wallet.dat (it IS a secret — back it up accordingly).  The CEvoDB
// parameters below are retained as the one-time migration source (LoadShares
// imports and erases legacy evoDb entries) — dropping them is follow-up
// cleanup; see the block comment in ptx_bls.cpp for the full rationale.
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
bool PTX_BLS_PersistShare(CEvoDB* evoDb, const uint256& quorum_hash, const HeldShare& hs);

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

// KDD-072 P-b6b: the CURRENT-role subset of the held keys. The residue sweep
// (store-side — it needs record state this TU cannot see) asks "which shares
// claim to be live?" and then checks each against its record.
std::set<uint256> PTX_BLS_HeldCurrentQuorumHashes();

// KDD-072 P-b6b: retire ONE share by key — memory + disk, the same erase both
// depth sweeps perform. TERMINAL: the share is DELETED, never moved to
// SUPERSEDED_RETAINED (that role exists for the undo, and a residue has no undo
// role — UndoPromote requires a promotion pair that never existed for it).
// Returns true if a share was held and erased.
bool PTX_BLS_RetireShare(const uint256& quorum_hash, CEvoDB* evoDb);

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

// ---------------------------------------------------------------------------
// KDD-070 P3 — PENDING role, promotion, TTL expiry.
// ---------------------------------------------------------------------------

// PENDING_TTL: how long a PENDING share survives without its successor
// connecting, in blocks. ★ PROVISIONAL — justified by FINALIZE→successor-connect
// latency under KDD-058-A any-staker inclusion (any staker can mine the successor
// as soon as it validates from the replicated minable-commitments store — a few
// blocks + propagation margin), NOT by the rotation interval N. Measurement OWED
// at W2.3's first live rotation (measure FINALIZE→connect over real rotations).
static const int PTX_PENDING_TTL_BLOCKS = 8;

// KDD-070 P4: safety MARGIN added to DEFAULT_MAX_REORG_DEPTH (consensus/
// consensus.h:35 = 100) before a SUPERSEDED_RETAINED share is safe to discard.
// Once a promotion is buried DEFAULT_MAX_REORG_DEPTH + this margin below the tip
// (120 blocks), no permitted reorg can reach it, so the retained predecessor can
// be dropped. Named here beside PENDING_TTL; the sum is formed in the .cpp where
// DEFAULT_MAX_REORG_DEPTH is in scope (keeps this header free of consensus.h).
// BUG-028: the SAME sum is the retention basis for UNDONE_RETAINED. Both halves
// of a promotion move together — if 20 is ever wrong, it is wrong for both.
//
// ★ KNOWN LIMIT (pinned by BUG-028 RED-3): DEFAULT_MAX_REORG_DEPTH is the
// DEFAULT of the -maxreorg runtime option (validation.cpp:
// `gArgs.GetArg("-maxreorg", DEFAULT_MAX_REORG_DEPTH)`), not a hard constant. A
// node started with -maxreorg ABOVE 120 permits reorgs deeper than either
// retained role is kept for, so a promotion could be undone after its retained
// counterpart had already been discarded. This basis is the DEFAULT permitted
// depth plus margin — it is not a guarantee against an operator-widened window.
static const int PTX_SUPERSEDED_REORG_MARGIN = 20;

// promote(successor): PENDING(successor_qh) -> CURRENT, and CURRENT(predecessor_qh)
// -> SUPERSEDED_RETAINED with promotion_height stamped = connect_height (P4's
// depth-discard basis). PURE over explicit inputs — ProcessBlock at block-connect
// merely calls this.
//
// ★ CONTRACT AMENDMENT (BUG-028): the promotable set is PENDING **or**
// UNDONE_RETAINED. The second is the REDO path — a reorg that disconnects a
// PTXDKG block and then re-applies it. Before BUG-028 the undo erased the
// successor, so the redo found nothing, no-opped, and left the node holding NO
// share for the quorum (it could not sign the rolls it was selected for).
//
// KEY ISOLATION IS UNCHANGED and still holds: it is keyed on QUORUM_HASH, not on
// role. A connect for quorum Y still never promotes a share for some other X —
// the lookup is find(successor_qh), and only that key can be promoted. The
// amendment widens which ROLES at that key are promotable; it does not widen
// which KEYS are reachable. If there is no PENDING/UNDONE_RETAINED share for
// successor_qh, it remains a NO-OP (returns 0).
//
// Re-persists changed shares via the RAW layer (P2). Returns 1 if promoted, else 0.
size_t PTX_BLS_Promote(const uint256& successor_qh, const uint256& predecessor_qh,
                       int connect_height, CEvoDB* evoDb = nullptr);

// Discard every PENDING share older than PENDING_TTL (tip_height - formation_height
// > TTL). Erases from DISK as well as memory (P2 defect (a) — never memory-only).
// Returns the number expired. CURRENT/SUPERSEDED shares are untouched.
size_t PTX_BLS_ExpirePending(int tip_height, CEvoDB* evoDb = nullptr);

// ---------------------------------------------------------------------------
// KDD-070 P4 — SUPERSEDED retention window: depth-discard + undo revert.
// ---------------------------------------------------------------------------

// Discard every SUPERSEDED_RETAINED share buried at least DEFAULT_MAX_REORG_DEPTH
// + PTX_SUPERSEDED_REORG_MARGIN (120) blocks below the tip: DEPTH-based on
// tip_height - promotion_height >= that sum (NOT height-based). Past this depth no
// permitted reorg can undo the promotion, so the retained predecessor is dropped.
// Erases from DISK as well as memory (P2 defect (a); same shape as ExpirePending).
// Returns the number discarded. CURRENT/PENDING shares are untouched.
size_t PTX_BLS_DiscardSuperseded(int tip_height, CEvoDB* evoDb = nullptr);

// undo(successor, predecessor): the SLOT-SIDE revert of a promotion, invoked on
// block-DISCONNECT (reorg). SUPERSEDED_RETAINED(predecessor_qh) -> CURRENT with
// promotion_height cleared to -1; CURRENT(successor_qh) -> UNDONE_RETAINED with
// promotion_height stamped = undo_height (its retention clock).
//
// ★ BUG-028: the successor used to be ERASED here. That made the undo a
// one-way door and broke the "is its inverse" property this contract claims:
// the inverse of PENDING -> CURRENT is CURRENT -> (retained, re-promotable),
// NOT deletion. The promotion did not CREATE the successor's share — the share
// already existed as PENDING (stored at ceremony FINALIZE, ptx_dkg.cpp) — so
// erasing it destroyed material the promotion never brought into being, and no
// later redo could reconstruct it. Retention makes the inverse actually total.
//
// SYMMETRY: both halves of a promotion are now retained on the same reorg-DEPTH
// clock and the same constant (DEFAULT_MAX_REORG_DEPTH + margin) — predecessor
// as SUPERSEDED_RETAINED, successor as UNDONE_RETAINED. Neither retained role is
// SIGNABLE: PTX_BLS_GetCurrentShare requires CURRENT.
//
// PURE keyed function over explicit inputs — mirrors PTX_BLS_Promote. Mutates
// role IN PLACE (NOT via the guarded setter — that would refuse on §C1 and would
// be a SECOND write path, §1 forbids it); re-persists BOTH shares via the RAW
// layer. IDEMPOTENT + KEY-ISOLATED: a call for a quorum with no reversible
// promotion (successor not held/not CURRENT, or predecessor not
// SUPERSEDED_RETAINED) is a clean NO-OP (returns 0, never an error) — so a
// multi-block disconnect that unwinds a block promoting nothing, or a second
// call, does no harm. Idempotency is UNCHANGED by the retention: after the first
// call the successor is UNDONE_RETAINED, so the successor-must-be-CURRENT guard
// still rejects the second. Returns 1 if a promotion was reverted, else 0.
//
// SCOPE (KDD-070 = the share slot): this is the SHARE-SLOT half only. The
// record-side revert (successor de-activated, predecessor SUPERSEDED->ACTIVE in
// CPTXQuorumStore::UndoBlock) is consensus-adjacent and owed to whichever of
// KDD-063 / W2.4 T-H lands first. Shaped to COMPOSE with that revert at the same
// disconnect: keyed by quorum_hash, idempotent, no assumption about call order
// relative to the record store.
size_t PTX_BLS_UndoPromote(const uint256& successor_qh, const uint256& predecessor_qh,
                           int undo_height, CEvoDB* evoDb = nullptr);

// BUG-028 — discard every UNDONE_RETAINED share buried at least
// DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN (120) blocks below the
// tip, DEPTH-based on tip_height - promotion_height (here: the UNDO height).
// Exact mirror of PTX_BLS_DiscardSuperseded — same basis, same constant, same
// disk-erase shape — for the other half of the promotion. Past this depth no
// permitted reorg can re-apply the disconnected block, so the retained successor
// is dropped. Returns the number discarded; other roles are untouched.
size_t PTX_BLS_DiscardUndone(int tip_height, CEvoDB* evoDb = nullptr);

// ---------------------------------------------------------------------------
// BUG-029 — snapshot/restore of the whole share store, for validation dry runs.
//
// VerifyDB's level-3 walk (validation.cpp) runs the REAL DisconnectBlock, which
// reaches PTX_BLS_UndoPromote and MUTATES g_ptx_my_shares. That mutation is
// outside both sandboxes protecting the walk: the throwaway CCoinsViewCache, and
// the evoDb rollback transaction — the latter because PTX_BLS_PersistShare writes
// through GetRawDB(), which bypasses CDBTransaction by design (ODC-035's degraded
// path wants persistence untied from block atomicity). These two mirror pose's
// GetAllRecords()/RestoreRecords() so a caller restores each global with one call.
//
// RestoreShares is DURABLE, not transactional: it re-persists every restored share
// and erases keys that the walk added, because the walk's raw writes have already
// reached disk and no rollback will remove them. Same scope caveat as pose's
// RestoreRecords — a crash mid-restore still leaves disk dirty.
//
// NOT snapshotted: g_ptx_memory_only_shares (a diagnostic set, not consensus
// state). A dry run that provokes a persist failure can leave a residue there.
std::map<uint256, HeldShare> PTX_BLS_SnapshotShares();
void PTX_BLS_RestoreShares(std::map<uint256, HeldShare> shares, CEvoDB* evoDb = nullptr);

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
