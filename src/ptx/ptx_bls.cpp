// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_bls.h"

#include "consensus/consensus.h"   // KDD-070 P4: DEFAULT_MAX_REORG_DEPTH (discard basis)
#include "crypto/sha256.h"
#include "dbwrapper.h"
#include "evo/evodb.h"
#include "logging.h"
#include "random.h"

#include <algorithm>
#include <cstring>
#include <memory>

// KDD-070 P2: RAW-DB key prefix for persisted GM sk-shares (Dash
// DB_QUORUM_SK_SHARE precedent). Keyed (prefix, quorum_hash) → 41-byte blob.
static const std::string DB_PTX_SKSHARE = "ptxSk";

const char*    PTX_BLS_DST = "BLS_SIG_HEMIS_PTX_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_";

// GM-side BLS key-share store (KDD-070 P1): keyed by quorum_hash, written by
// PTX_DKG_StoreSkShare on ceremony completion, read for signing by
// PTX_BLS_GetCurrentShare. Guarded by cs_ptx_my_bls_sk. NOT bounded in size.
std::map<uint256, HeldShare> g_ptx_my_shares;
RecursiveMutex               cs_ptx_my_bls_sk;
std::set<uint256>            g_ptx_memory_only_shares;   // KDD-070 P2 (b): persist-failed keys

// ---------------------------------------------------------------------------
// PTX_BLS_SetSkShare — §C1 replay guard (KDD-057; rationale updated KDD-069)
//
// The SINGLE guarded write path for the GM-side sk-share.  Post-KDD-069 there
// is exactly ONE write site: PTX_DKG_StoreSkShare (ptx_dkg.cpp), on local
// ceremony completion.  The former coordinator-supplied gm_bls_keyset RPC path
// was removed with the trusted dealer, so this guard no longer defends against
// a coordinator hijacking the slot — it now defends against CEREMONY REPLAY /
// double-store (a member re-running a ceremony, or a future rotation/re-select
// overwriting a live share).  No site writes g_ptx_my_shares
// directly (a direct write would bypass the guard).
//
// refuse-unless-empty PER quorum_hash: a first-set for a key stores the share
// (role CURRENT, P1); a second write to the SAME key is REFUSED.  Distinct
// quorum_hashes coexist — the store is NOT bounded (KDD-040's one-per-GM is a
// fact about today, relaxable by W2.5; not an invariant here).  The share is
// written only on local ceremony COMPLETION (StoreSkShare fires at
// phase==FINALIZE) and a failed/aborted formation leaves the key unset.  W2
// rotation/disband/re-formation MUST add (a) an authorized clear/rotate path
// (KDD-070 P3/P4) and (b) startup reconciliation + wipe (KDD-070 P2); a plain
// overwrite of a set key is refused here and no runtime clear exists yet.
// ---------------------------------------------------------------------------

bool PTX_BLS_SetSkShare(const uint256& quorum_hash, int formation_height,
                        const uint8_t sk_bytes[32], PTXShareRole role, std::string& err)
{
    LOCK(cs_ptx_my_bls_sk);
    if (g_ptx_my_shares.count(quorum_hash)) {
        err = "sk-share already set for quorum " + quorum_hash.ToString() +
              "; refusing overwrite (C1 replay/double-store guard, KDD-069/070 — "
              "rotation/disband must use an authorized clear path)";
        return false;
    }
    if (role == PTXShareRole::PENDING) {
        // KDD-070 P3, KDD-040-era rule: at most ONE PENDING in flight — a member
        // is in one lineage, so at most one rotation-successor may be pending at
        // a time. A second store-pending is refused (the driver must not start a
        // second rotation before the first resolves; a stuck PENDING is cleaned
        // by TTL). W2.5 multi-quorum relaxes this to ONE-PER-LINEAGE; do NOT
        // encode a global cap in the type.
        for (const auto& kv : g_ptx_my_shares) {
            if (kv.second.role == PTXShareRole::PENDING) {
                err = "a PENDING share already exists (quorum " + kv.first.ToString() +
                      "); refusing a second (one rotation in flight per member, KDD-040)";
                return false;
            }
        }
    }
    HeldShare hs;
    std::memcpy(hs.bytes, sk_bytes, 32);
    hs.formation_height = formation_height;
    hs.role             = role;
    hs.promotion_height = -1;
    g_ptx_my_shares[quorum_hash] = hs;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_BLS_GetCurrentShare — the single read path for signing selection (§5).
// Returns true + copies bytes iff a CURRENT-role share is held for quorum_hash.
// Never returns another quorum's share; refuses a non-CURRENT role (PENDING /
// SUPERSEDED become reachable in P3/P4 — the role check is written now).
// ---------------------------------------------------------------------------

bool PTX_BLS_GetCurrentShare(const uint256& quorum_hash, uint8_t out[32])
{
    LOCK(cs_ptx_my_bls_sk);
    auto it = g_ptx_my_shares.find(quorum_hash);
    if (it == g_ptx_my_shares.end() || it->second.role != PTXShareRole::CURRENT)
        return false;
    std::memcpy(out, it->second.bytes, 32);
    return true;
}

// ---------------------------------------------------------------------------
// KDD-070 P2 — persistence, reconciliation, wipe.
// ---------------------------------------------------------------------------

static void put_i32_le(std::vector<uint8_t>& b, int32_t v)
{
    uint32_t u = (uint32_t)v;
    for (int i = 0; i < 4; ++i) b.push_back((uint8_t)((u >> (8 * i)) & 0xff));
}
static int32_t get_i32_le(const std::vector<uint8_t>& b, size_t off)
{
    uint32_t u = 0;
    for (int i = 0; i < 4; ++i) u |= (uint32_t)b[off + i] << (8 * i);
    return (int32_t)u;
}

std::vector<uint8_t> PTX_BLS_SerializeHeldShare(const HeldShare& hs)
{
    std::vector<uint8_t> b;
    b.reserve(41);
    b.insert(b.end(), hs.bytes, hs.bytes + 32);   // [0..31]
    put_i32_le(b, hs.formation_height);           // [32..35]
    b.push_back((uint8_t)hs.role);                // [36]
    put_i32_le(b, hs.promotion_height);           // [37..40]
    return b;
}

bool PTX_BLS_DeserializeHeldShare(const std::vector<uint8_t>& blob, HeldShare& out)
{
    if (blob.size() != 41) return false;
    std::memcpy(out.bytes, blob.data(), 32);
    out.formation_height = get_i32_le(blob, 32);
    out.role             = (PTXShareRole)blob[36];
    out.promotion_height = get_i32_le(blob, 37);
    return true;
}

const std::string& PTX_BLS_ShareDBPrefix() { return DB_PTX_SKSHARE; }

void PTX_BLS_MarkMemoryOnly(const uint256& quorum_hash)
{
    LOCK(cs_ptx_my_bls_sk);
    g_ptx_memory_only_shares.insert(quorum_hash);
}

std::set<uint256> PTX_BLS_MemoryOnlyShares()
{
    LOCK(cs_ptx_my_bls_sk);
    return g_ptx_memory_only_shares;
}

// ---------------------------------------------------------------------------
// KDD-070 P3 — promotion (pure function) and PENDING TTL expiry.
// ---------------------------------------------------------------------------

size_t PTX_BLS_Promote(const uint256& successor_qh, const uint256& predecessor_qh,
                       int connect_height, CEvoDB* evoDb)
{
    LOCK(cs_ptx_my_bls_sk);
    auto sit = g_ptx_my_shares.find(successor_qh);
    // KEY ISOLATION: only a PENDING share for THIS connecting quorum promotes.
    // A connect for a quorum with no PENDING (or a non-PENDING share) is a NO-OP
    // — it never touches PENDING(X) for some other X.
    if (sit == g_ptx_my_shares.end() || sit->second.role != PTXShareRole::PENDING)
        return 0;

    // PENDING(successor) -> CURRENT.
    sit->second.role             = PTXShareRole::CURRENT;
    sit->second.promotion_height = -1;

    // CURRENT(predecessor) -> SUPERSEDED_RETAINED, stamping promotion_height =
    // the successor's connect height (P4's depth-discard basis). The old share is
    // KEPT (P3 discards nothing; P4 adds retention/discard/undo).
    bool superseded = false;
    auto pit = g_ptx_my_shares.find(predecessor_qh);
    if (pit != g_ptx_my_shares.end() && pit->second.role == PTXShareRole::CURRENT) {
        pit->second.role             = PTXShareRole::SUPERSEDED_RETAINED;
        pit->second.promotion_height = connect_height;
        superseded = true;
    }

    // Re-persist the changed shares via the RAW layer (P2). ★ IRREVERSIBLE until
    // P4: neither the transactional nor the raw layer auto-reverts on disconnect
    // (the record store's UndoBlock does an explicit Erase, ptx_quorum_store.cpp)
    // — P4 adds the explicit undo revert. A persist failure marks memory-only
    // (the flag follows the material).
    if (evoDb != nullptr) {
        if (!PTX_BLS_PersistShare(*evoDb, successor_qh, sit->second))
            g_ptx_memory_only_shares.insert(successor_qh);
        if (superseded && !PTX_BLS_PersistShare(*evoDb, predecessor_qh, pit->second))
            g_ptx_memory_only_shares.insert(predecessor_qh);
    }
    return 1;
}

size_t PTX_BLS_ExpirePending(int tip_height, CEvoDB* evoDb)
{
    LOCK(cs_ptx_my_bls_sk);
    size_t expired = 0;
    for (auto it = g_ptx_my_shares.begin(); it != g_ptx_my_shares.end(); ) {
        if (it->second.role == PTXShareRole::PENDING &&
            (tip_height - it->second.formation_height) > PTX_PENDING_TTL_BLOCKS) {
            LogPrintf("PTX P3: expiring stale PENDING share for quorum %s "
                      "(age %d > TTL %d blocks)\n", it->first.ToString(),
                      tip_height - it->second.formation_height, PTX_PENDING_TTL_BLOCKS);
            if (evoDb != nullptr)                    // erase from DISK too (P2 defect (a))
                evoDb->GetRawDB().Erase(std::make_pair(DB_PTX_SKSHARE, it->first));
            g_ptx_memory_only_shares.erase(it->first);
            it = g_ptx_my_shares.erase(it);
            ++expired;
        } else {
            ++it;
        }
    }
    return expired;
}

// ---------------------------------------------------------------------------
// KDD-070 P4 — SUPERSEDED retention window: depth-discard and undo revert.
// ---------------------------------------------------------------------------

size_t PTX_BLS_DiscardSuperseded(int tip_height, CEvoDB* evoDb)
{
    LOCK(cs_ptx_my_bls_sk);
    // DEPTH-based: buried this far below the tip, no permitted reorg can reach
    // the promotion, so the retained predecessor is safe to drop.
    const int discard_depth = DEFAULT_MAX_REORG_DEPTH + PTX_SUPERSEDED_REORG_MARGIN;
    size_t discarded = 0;
    for (auto it = g_ptx_my_shares.begin(); it != g_ptx_my_shares.end(); ) {
        if (it->second.role == PTXShareRole::SUPERSEDED_RETAINED &&
            it->second.promotion_height >= 0 &&
            (tip_height - it->second.promotion_height) >= discard_depth) {
            LogPrintf("PTX P4: discarding SUPERSEDED share for quorum %s "
                      "(buried %d >= %d blocks; beyond reorg reach)\n",
                      it->first.ToString(),
                      tip_height - it->second.promotion_height, discard_depth);
            if (evoDb != nullptr)                    // erase from DISK too (P2 defect (a))
                evoDb->GetRawDB().Erase(std::make_pair(DB_PTX_SKSHARE, it->first));
            g_ptx_memory_only_shares.erase(it->first);
            it = g_ptx_my_shares.erase(it);
            ++discarded;
        } else {
            ++it;
        }
    }
    return discarded;
}

size_t PTX_BLS_UndoPromote(const uint256& successor_qh, const uint256& predecessor_qh,
                           CEvoDB* evoDb)
{
    LOCK(cs_ptx_my_bls_sk);
    // Reversible ONLY if the successor is CURRENT (was promoted) AND the
    // predecessor is SUPERSEDED_RETAINED (was superseded by that promotion). Any
    // other state — a disconnect for a block that promoted nothing, an
    // already-reverted pair (idempotency), a mismatched key (isolation) — is a
    // clean NO-OP. Both guards are required: it is the successor-CURRENT check
    // that makes a second call a no-op (the successor is gone after the first).
    auto sit = g_ptx_my_shares.find(successor_qh);
    if (sit == g_ptx_my_shares.end() || sit->second.role != PTXShareRole::CURRENT)
        return 0;
    auto pit = g_ptx_my_shares.find(predecessor_qh);
    if (pit == g_ptx_my_shares.end() || pit->second.role != PTXShareRole::SUPERSEDED_RETAINED)
        return 0;

    // predecessor: SUPERSEDED_RETAINED -> CURRENT, promotion_height cleared.
    // MUTATE IN PLACE (as Promote does) — routing the restore through the guarded
    // setter would refuse (predecessor already present, §C1) and would be a
    // SECOND write path (§1 forbids it).
    pit->second.role             = PTXShareRole::CURRENT;
    pit->second.promotion_height = -1;

    // successor: the promotion that added this CURRENT is being unwound -> DISCARD.
    g_ptx_memory_only_shares.erase(successor_qh);
    g_ptx_my_shares.erase(sit);   // pit stays valid (distinct element)

    // Persist the revert via the RAW layer: re-persist the restored predecessor,
    // erase the discarded successor from DISK (else it reloads on next start).
    if (evoDb != nullptr) {
        if (!PTX_BLS_PersistShare(*evoDb, predecessor_qh, pit->second))
            g_ptx_memory_only_shares.insert(predecessor_qh);
        evoDb->GetRawDB().Erase(std::make_pair(DB_PTX_SKSHARE, successor_qh));
    }
    return 1;
}

bool PTX_BLS_PersistShare(CEvoDB& evoDb, const uint256& quorum_hash, const HeldShare& hs)
{
    std::vector<uint8_t> blob = PTX_BLS_SerializeHeldShare(hs);
    // GetRawDB().Write returns false on a DB write error — propagate it; the
    // caller must not swallow it (memory-only persistence is the ODC-035 mode).
    return evoDb.GetRawDB().Write(std::make_pair(DB_PTX_SKSHARE, quorum_hash), blob);
}

int PTX_BLS_LoadShares(CEvoDB& evoDb)
{
    LOCK(cs_ptx_my_bls_sk);
    std::unique_ptr<CDBIterator> it(evoDb.GetRawDB().NewIterator());
    int loaded = 0, corrupt = 0;
    for (it->Seek(std::make_pair(DB_PTX_SKSHARE, uint256())); it->Valid(); it->Next()) {
        std::pair<std::string, uint256> key;
        if (!it->GetKey(key) || key.first != DB_PTX_SKSHARE) break;  // past the prefix range
        std::vector<uint8_t> blob;
        HeldShare hs;
        if (it->GetValue(blob) && PTX_BLS_DeserializeHeldShare(blob, hs)) {
            g_ptx_my_shares[key.second] = hs;
            ++loaded;
        } else {
            // NOT swallowed: a corrupt on-disk share is an ERROR — the member
            // HAD a share and it is now unreadable (distinct from never having
            // had one). Naming the quorum_hash lets the "in_qual but no share"
            // warning tell the two apart.
            LogPrintf("PTX P2: ERROR: LoadShares: CORRUPT persisted share for quorum %s "
                      "(unreadable/undeserializable) NOT loaded\n", key.second.ToString());
            ++corrupt;
        }
    }
    LogPrintf("PTX P2: LoadShares: %d share(s) loaded, %d CORRUPT\n", loaded, corrupt);
    return corrupt;
}

size_t PTX_BLS_ReconcileShares(const std::set<uint256>& known_quorums, CEvoDB* evoDb)
{
    LOCK(cs_ptx_my_bls_sk);
    size_t discarded = 0;
    for (auto it = g_ptx_my_shares.begin(); it != g_ptx_my_shares.end(); ) {
        if (known_quorums.count(it->first) == 0) {
            LogPrintf("PTX P2: reconcile: discarding ORPHAN share for quorum %s "
                      "(no record on the active chain)\n", it->first.ToString());
            // Erase from DISK too, else the orphan reloads on the next start.
            if (evoDb != nullptr)
                evoDb->GetRawDB().Erase(std::make_pair(DB_PTX_SKSHARE, it->first));
            g_ptx_memory_only_shares.erase(it->first);   // no longer held
            it = g_ptx_my_shares.erase(it);
            ++discarded;
        } else {
            ++it;   // live quorum — kept regardless of role
        }
    }
    return discarded;
}

std::set<uint256> PTX_BLS_HeldQuorumHashes()
{
    LOCK(cs_ptx_my_bls_sk);
    std::set<uint256> out;
    for (const auto& kv : g_ptx_my_shares) out.insert(kv.first);
    return out;
}

size_t PTX_BLS_WipeShares(CEvoDB* evoDb)
{
    LOCK(cs_ptx_my_bls_sk);
    size_t n = g_ptx_my_shares.size();
    int erased = 0;
    if (evoDb != nullptr) {
        // Iterate the RAW-DB prefix DIRECTLY and erase EVERY persisted share,
        // including corrupt/undeserializable entries that LoadShares skipped —
        // a map-driven wipe would leave those on disk. Collect-then-erase: do
        // not mutate the DB while its iterator is live.
        std::unique_ptr<CDBIterator> it(evoDb->GetRawDB().NewIterator());
        std::vector<uint256> keys;
        for (it->Seek(std::make_pair(DB_PTX_SKSHARE, uint256())); it->Valid(); it->Next()) {
            std::pair<std::string, uint256> key;
            if (!it->GetKey(key) || key.first != DB_PTX_SKSHARE) break;
            keys.push_back(key.second);
        }
        for (const uint256& k : keys) {
            evoDb->GetRawDB().Erase(std::make_pair(DB_PTX_SKSHARE, k));
            ++erased;
        }
    }
    g_ptx_my_shares.clear();
    g_ptx_memory_only_shares.clear();
    LogPrintf("PTX P2: wipe: cleared %u held share(s)%s (disk entries erased: %d)\n",
              (unsigned)n, evoDb ? " (memory + disk)" : " (memory only)", erased);
    return n;
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

    blst_p2 sig_p2;
    blst_sign_pk_in_g1(&sig_p2, &hash_point, &sk);

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
        blst_scalar xi_s = {}; xi_s.b[0] = (uint8_t)indices[i];
        blst_fr xi; blst_fr_from_scalar(&xi, &xi_s);

        blst_scalar one_s = {}; one_s.b[0] = 1;
        blst_fr lambda; blst_fr_from_scalar(&lambda, &one_s);

        for (int j = 0; j < t; j++) {
            if (j == i) continue;

            blst_scalar xj_s = {}; xj_s.b[0] = (uint8_t)indices[j];
            blst_fr xj; blst_fr_from_scalar(&xj, &xj_s);

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
        blst_lendian_from_scalar(lambda_bytes, &lambda_scalar);
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
// PTX_BLS_Verify — pairing check against explicit group_pk (KDD-049)
// ---------------------------------------------------------------------------

bool PTX_BLS_Verify(const uint8_t group_pk_bytes[48],
                    const uint256& msg,
                    const uint8_t sig[PTX_SIG_BYTES])
{
    blst_p1_affine group_pk;
    if (blst_p1_uncompress(&group_pk, group_pk_bytes) != BLST_SUCCESS) return false;

    blst_p2_affine sig_affine;
    if (blst_p2_uncompress(&sig_affine, sig) != BLST_SUCCESS) return false;

    BLST_ERROR err = blst_core_verify_pk_in_g1(
        &group_pk,
        &sig_affine,
        true,
        msg.begin(), 32,
        (const uint8_t*)PTX_BLS_DST, strlen(PTX_BLS_DST),
        nullptr, 0);

    return err == BLST_SUCCESS;
}
