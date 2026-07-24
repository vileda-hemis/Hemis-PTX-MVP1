// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// PTX DKG ceremony — GJKR-hardened Feldman VSS over blst (KDD-051).
// Scope: src/ptx/ only.  No llmq:: dependencies in this file.
//
// Key separation invariant (impl plan §6):
//   CBLSSecretKey (operator key, chiabls) — signs ceremony P2P messages;
//     NEVER stored in PTXDKGSession; passed as a parameter at signing time.
//   blst_scalar sk_share_i (ceremony arithmetic, blst) — the output of
//     Phase 4/5 aggregation; held in PTXDKGSession during the ceremony, then
//     written to the keyed store g_ptx_my_shares via the guarded setter
//     PTX_BLS_SetSkShare in Phase 5 (PTX_DKG_StoreSkShare; KDD-057 Option A,
//     §C1 replay guard; keyed KDD-070 P1).
//   These two key types serve different purposes with different lifetimes;
//   conflating them is the silent-keying-bug risk from the impl plan's §6.

#ifndef HEMIS_PTX_DKG_H
#define HEMIS_PTX_DKG_H

#include "blst.h"
#include "bls/bls_wrapper.h"
// bls_ies.h is included for transport-encryption of ceremony shares only (Phase 1).
// No llmq:: namespace dependency: bls_ies.h lives in src/bls/ and its transitive
// includes contain no llmq headers.  The share bytes are blst arithmetic throughout;
// IES carries 32 opaque bytes (blst_bendian_from_scalar in, blst_scalar_from_bendian
// out) — the IMP-D1 scalar-representation seam is never crossed.
#include "bls/bls_ies.h"
#include "primitives/transaction.h"  // CMutableTransaction, SetTxPayload, SERIALIZE_METHODS
#include "sync.h"                     // RecursiveMutex (SG-2a session-state lock)
#include "uint256.h"

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

// Forward declarations: full definitions in evo/deterministicgms.h, included by ptx_dkg.cpp.
// Kept out of this header to avoid dragging evo/chain transitive deps into every TU.
class CDeterministicGM;
class CDeterministicGMList;
class CValidationState;

// ---------------------------------------------------------------------------
// PTXDKGPhase
//
// Session phase numbering vs. plan/doc numbering (DKG_IMPLEMENTATION_PLAN_v1.md):
//   HASH_COMMIT  = the GJKR-added round, "Phase 0" in session shorthand;
//                  the "one added gossip round" that precedes the five-phase
//                  base (KDD-051).  Docs say "five-phase ceremony" referring
//                  to CONTRIB through FINALIZE; HASH_COMMIT is the extra round
//                  layered before them.
//   CONTRIB      = Phase 1 of the five-phase base: contribution reveal.
//   COMPLAINT    = Phase 2: per-recipient Feldman VSS check and complaints.
//   JUSTIFY      = Phase 3: justification by accused members.
//   PREMIT       = Phase 4: premature commitment broadcast.
//   FINALIZE     = Phase 5: finalize → PTXDKG special transaction.
// ---------------------------------------------------------------------------
enum class PTXDKGPhase {
    IDLE,
    HASH_COMMIT,
    CONTRIB,
    COMPLAINT,
    JUSTIFY,
    PREMIT,
    FINALIZE,
    DONE,
    ABORTED,
};

// ---------------------------------------------------------------------------
// PTXDKGMember — formation-time descriptor for one ceremony participant.
// Caller populates this from the DGM list at formation height.
//
// pubKeyOperator authenticates ceremony P2P messages (chiabls operator key).
// It is NOT the ceremony arithmetic key; see the key-separation note above.
// ---------------------------------------------------------------------------
struct PTXDKGMember {
    uint256       proTxHash;
    std::string   node_id;
    uint256       confirmedHash;                  // must not be null (KDD-052 precondition)
    uint256       confirmedHashWithProRegTxHash;  // SHA256(proTxHash||confirmedHash); copy from DGM state
    int           share_index{0};                 // 1-indexed; CalculateQuorum position (KDD-060)
    CBLSPublicKey pubKeyOperator;                 // chiabls; verifies ceremony message signatures only
};

// ---------------------------------------------------------------------------
// PTXDKGLocalContrib — this node's secret contribution.
// Held in the session struct during the ceremony.  Never broadcast.
// All fields are blst (ceremony arithmetic); no chiabls material here.
// ---------------------------------------------------------------------------
struct PTXDKGLocalContrib {
    std::vector<blst_fr>        coeffs;     // t polynomial coefficients over Zr; secret
    std::vector<blst_p1_affine> vvec;       // t G1 points: vvec[k] = g^{coeffs[k]}; public at Phase 1
    std::vector<blst_scalar>    evals;      // n evaluations f(share_index_i), 0-indexed; sent encrypted in Phase 1
    uint256                     commitment; // SHA256(quorum_hash||proTxHash||vvec_bytes); broadcast in Phase 0
};

// ---------------------------------------------------------------------------
// PTXDKGPhase0Msg — the HASH_COMMIT P2P message.
// Binds the sender to its vvec (= full polynomial) without revealing it.
// The sig authenticates the message using the sender's chiabls operator key.
// ---------------------------------------------------------------------------
struct PTXDKGPhase0Msg {
    uint256       quorum_hash;   // identifies the ceremony (formation block hash)
    uint256       proTxHash;     // committer identity
    uint256       commitment;    // SHA256(quorum_hash||proTxHash||vvec_bytes)
    CBLSSignature sig;           // operator key sig over GetSignHash()

    // Covers quorum_hash || proTxHash || commitment.
    uint256 GetSignHash() const;

    // Wire form (W2.0b transport). GetSignHash is a direct field hash and does
    // not depend on this encoding — auth is unchanged by serialization.
    SERIALIZE_METHODS(PTXDKGPhase0Msg, obj)
    {
        READWRITE(obj.quorum_hash, obj.proTxHash, obj.commitment, obj.sig);
    }
};

// Deserialize-time cap on the Phase 1 vvec element count (memory-DoS hygiene
// only; t=6 today, cap = n = 11).  The EXACT size/content gate is downstream:
// the vvec bytes are bound by the Phase 0 commitment and the operator-key sig,
// so any tampered or wrong-sized vvec fails those checks in ReceivePhase1Msg.
static const uint64_t PTX_DKG_WIRE_MAX_VVEC = 11;

// ---------------------------------------------------------------------------
// Serialization formatters for the raw blst types (KDD-054 seams).  Split
// Ser/Unser (the Using<> pattern) so each stream direction instantiates only
// its own path — REQUIRED because ceremony messages serialize over the wire
// through CVectorWriter (write-only, via CNetMsgMaker), which has no read():
// a raw SER_READ(s.read(...)) fails to COMPILE against it.  LLMQ dodges this
// by only READWRITE-ing types with their own Serialize/Unserialize; PTX's raw
// blst points/scalars need these formatters.
// ---------------------------------------------------------------------------

// A blst_p1_affine as 48 compressed bytes; Unser rejects points that fail
// decompress (the C0 "ptxdkg-phase1-bad-point" gate).
struct PTXBlstG1Formatter {
    template <typename Stream> void Ser(Stream& s, const blst_p1_affine& pt) const {
        uint8_t buf[48];
        blst_p1_affine_compress(buf, &pt);
        s.write(reinterpret_cast<const char*>(buf), 48);
    }
    template <typename Stream> void Unser(Stream& s, blst_p1_affine& pt) const {
        uint8_t buf[48];
        s.read(reinterpret_cast<char*>(buf), 48);
        if (blst_p1_uncompress(&pt, buf) != BLST_SUCCESS)
            throw std::ios_base::failure("ptxdkg-phase1-bad-point");
    }
};

// std::vector<blst_p1_affine> as compactsize count (capped) + compressed points.
struct PTXVvecFormatter {
    template <typename Stream> void Ser(Stream& s, const std::vector<blst_p1_affine>& vvec) const {
        ::WriteCompactSize(s, vvec.size());
        PTXBlstG1Formatter pf;
        for (const auto& pt : vvec) pf.Ser(s, pt);
    }
    template <typename Stream> void Unser(Stream& s, std::vector<blst_p1_affine>& vvec) const {
        const uint64_t n = ::ReadCompactSize(s);
        if (n > PTX_DKG_WIRE_MAX_VVEC)
            throw std::ios_base::failure("ptxdkg-phase1-vvec-oversize");
        vvec.clear();
        vvec.reserve(n);
        PTXBlstG1Formatter pf;
        for (uint64_t k = 0; k < n; k++) {
            blst_p1_affine pt;
            pf.Unser(s, pt);
            vvec.push_back(pt);
        }
    }
};

// A blst_scalar as 32 opaque big-endian bytes (IMP-D1 seam); Unser rejects
// non-canonical scalars (the C0 "ptxdkg-phase3-noncanonical-scalar" gate).
struct PTXBlstScalarFormatter {
    template <typename Stream> void Ser(Stream& s, const blst_scalar& sc) const {
        uint8_t buf[32];
        blst_bendian_from_scalar(buf, &sc);
        s.write(reinterpret_cast<const char*>(buf), 32);
    }
    template <typename Stream> void Unser(Stream& s, blst_scalar& sc) const {
        uint8_t buf[32];
        s.read(reinterpret_cast<char*>(buf), 32);
        blst_scalar_from_bendian(&sc, buf);
        if (!blst_scalar_fr_check(&sc))
            throw std::ios_base::failure("ptxdkg-phase3-noncanonical-scalar");
    }
};

// Fixed N raw bytes (no length prefix) — for group_pk_bytes[48].  Ser/Unser
// split so the write path never references s.read.
template <int N>
struct PTXFixedBytesFormatter {
    template <typename Stream> void Ser(Stream& s, const uint8_t (&arr)[N]) const {
        s.write(reinterpret_cast<const char*>(arr), N);
    }
    template <typename Stream> void Unser(Stream& s, uint8_t (&arr)[N]) const {
        s.read(reinterpret_cast<char*>(arr), N);
    }
};

// ---------------------------------------------------------------------------
// PTXDKGPhase1Msg — the CONTRIB P2P message.
// Reveals the sender's vvec and carries per-QUAL-member encrypted evaluations.
//
// Transport confidentiality: CBLSIESMultiRecipientBlobs (ECIES, one blob per
// members[] slot; non-QUAL slots are empty).
//
// In-flight integrity: the sig covers quorum_hash || proTxHash || compressed-vvec
// || ephemeralPubKey || ivSeed || all blob bytes (length-prefixed).  A tampered
// blob therefore fails the sig check and the whole message is rejected — the
// receiver never sees a garbage scalar from an in-transit blob swap.
//
// Share validity is NOT established here.  The Phase 2 Feldman check
// (g^{share} == Π vvec[k]^{j^k}) is the correctness gate.
// ---------------------------------------------------------------------------
struct PTXDKGPhase1Msg {
    uint256                         quorum_hash;
    uint256                         proTxHash;
    std::vector<blst_p1_affine>     vvec;              // t=6 G1 points (compressed 48B each on wire)
    CBLSIESMultiRecipientBlobs      encrypted_shares;  // one blob per members[] slot
    CBLSSignature                   sig;               // operator key sig over GetSignHash()

    // Covers quorum_hash || proTxHash || compressed-vvec-bytes ||
    //         ephemeralPubKey(48B) || ivSeed(32B) || len-prefixed blob bytes.
    // Identical compressed-G1 primitive (blst_p1_affine_compress) as
    // ComputeVvecCommitment so both notions of "the vvec" are the same bytes.
    uint256 GetSignHash() const;

    // Wire form (W2.0b transport).  vvec: compactsize count + 48B compressed
    // G1 per point — the SAME blst_p1_affine_compress bytes GetSignHash and
    // ComputeVvecCommitment hash, so wire/sig/commitment share one canonical
    // byte form of "the vvec".  Deserialize rejects: count > cap (memory-DoS
    // hygiene) and bytes that fail blst_p1_uncompress (the same check the
    // in-process premit path applies to group_pk_bytes, ptx_dkg.cpp
    // ReceivePhase4Msg).  Subgroup (in-G1) validity is deliberately NOT
    // checked here — no in-process receive path checks it either; share
    // correctness is the Phase 2 Feldman gate's job (audit note for W3.2,
    // alongside ODC-027/028).  The IES blobs serialize whole via their own
    // src/bls serializer and stay bound INSIDE the outer sig (KDD-054
    // invariant) — this encoding adds no unsigned-blob path.
    SERIALIZE_METHODS(PTXDKGPhase1Msg, obj)
    {
        READWRITE(obj.quorum_hash, obj.proTxHash);
        READWRITE(Using<PTXVvecFormatter>(obj.vvec));
        READWRITE(obj.encrypted_shares, obj.sig);
    }
};

// ---------------------------------------------------------------------------
// PTXDKGPhase2Msg — complaint (COMPLAINT phase, KDD-055 §15).
// Filed by complainant C against dealer D when C's Feldman check on D's share
// fails.  Identifies the specific evaluation point j under dispute.
// All fields are fixed-size; j is written with explicit htole32 in GetSignHash
// (not reinterpret_cast — carry-forward D does not apply to P2/P3 messages).
// ---------------------------------------------------------------------------
struct PTXDKGPhase2Msg {
    uint256       quorum_hash;             // ceremony identity
    uint256       complainant_proTxHash;   // C's identity (filer)
    uint256       dealer_proTxHash;        // D's identity (accused)
    int           share_index_j{0};        // C's share_index — evaluation point under dispute (1-indexed)
    CBLSSignature sig;                     // operator-key sig over GetSignHash()

    // SHA256( quorum_hash[32] || complainant_proTxHash[32] ||
    //         dealer_proTxHash[32] || htole32(share_index_j)[4] )
    uint256 GetSignHash() const;

    // Wire form (W2.0b transport).  share_index_j as int32 (the
    // PTXDKGPayload::formation_height precedent); range validity is
    // ReceivePhase2Msg's existing check, not the decoder's.
    SERIALIZE_METHODS(PTXDKGPhase2Msg, obj)
    {
        READWRITE(obj.quorum_hash, obj.complainant_proTxHash,
                  obj.dealer_proTxHash, obj.share_index_j, obj.sig);
    }
};

// ---------------------------------------------------------------------------
// PTXDKGPhase3Msg — justification (JUSTIFY phase, KDD-055 §15).
// Filed by dealer D to answer a specific complaint from complainant C.
// Reveals the share in clear as a raw blst_scalar (big-endian, 32 bytes) —
// no ECIES (KDD-054 boundary: justify reveals the share in clear).
// j is NOT a wire field: ReceivePhase3Msg derives it from
// session.members[complainant_slot].share_index — never trusted from the wire.
// ---------------------------------------------------------------------------
struct PTXDKGPhase3Msg {
    uint256       quorum_hash;             // ceremony identity
    uint256       dealer_proTxHash;        // D's identity
    uint256       complainant_proTxHash;   // identifies the (D,C) complaint being answered
    blst_scalar   revealed_share;          // s = f_D(j), big-endian on wire (blst_bendian_from_scalar)
    CBLSSignature sig;                     // operator-key sig over GetSignHash()

    // SHA256( quorum_hash[32] || dealer_proTxHash[32] ||
    //         complainant_proTxHash[32] || blst_bendian(revealed_share)[32] )
    // The revealed scalar IS bound in the sign hash; a tampered scalar fails
    // the sig check before the Feldman check runs (regression-guarded by T2-23).
    uint256 GetSignHash() const;

    // Wire form (W2.0b transport).  revealed_share crosses as 32 opaque
    // big-endian bytes (the KDD-054 / IMP-D1 seam: blst_bendian_from_scalar /
    // blst_scalar_from_bendian, no chiabls type touches it).  Deserialize
    // rejects non-canonical scalars with the SAME blst_scalar_fr_check that
    // ReceivePhase3Msg applies (ptx_dkg.cpp, justify check 10) — the decoder
    // never constructs a scalar the receive path would call invalid.
    SERIALIZE_METHODS(PTXDKGPhase3Msg, obj)
    {
        READWRITE(obj.quorum_hash, obj.dealer_proTxHash, obj.complainant_proTxHash);
        READWRITE(Using<PTXBlstScalarFormatter>(obj.revealed_share));
        READWRITE(obj.sig);
    }
};

// ---------------------------------------------------------------------------
// PTXDKGPhase4Msg — premature commitment (PREMIT phase).
// Each GM broadcasts this after computing sk_share_i and group_pk.
// Binds the sender's computed group_pk and vvec_hash; signed by operator key.
// ---------------------------------------------------------------------------
struct PTXDKGPhase4Msg {
    uint256       quorum_hash;
    uint256       proTxHash;
    uint8_t       group_pk_bytes[48]; // blst_p1_affine_compress output
    uint256       vvec_hash;          // SHA256 over effective-QUAL vvec[0] compressed bytes
    CBLSSignature sig;                // operator key sig over GetSignHash()

    // SHA256( quorum_hash[32] || proTxHash[32] || group_pk_bytes[48] || vvec_hash[32] )
    uint256 GetSignHash() const;

    SERIALIZE_METHODS(PTXDKGPhase4Msg, obj)
    {
        READWRITE(obj.quorum_hash, obj.proTxHash);
        // group_pk_bytes: fixed 48 bytes, no size prefix.  Formatter (not raw
        // SER_READ) so this compiles over the write-only wire stream too; the
        // 48 emitted bytes are IDENTICAL to the prior encoding — the
        // consensus-frozen on-chain form is unchanged (S-4 guards it).
        READWRITE(Using<PTXFixedBytesFormatter<48>>(obj.group_pk_bytes));
        READWRITE(obj.vvec_hash, obj.sig);
    }
};

// ---------------------------------------------------------------------------
// PTXDKGPayload — extraPayload for a PTXDKG special transaction (nType=11).
// Carries the DKG ceremony result: group public key, vvec hash, effective-QUAL
// member list, and ≥ t signed premature commitments from Phase 4.
// Estimated wire size at t=6: ~1,580 bytes; MAX_SPECIALTX_EXTRAPAYLOAD = 10,000.
// ---------------------------------------------------------------------------
struct PTXDKGPayload {
    uint256                            quorum_hash;
    uint8_t                            group_pk_bytes[48]; // blst_p1_affine_compress
    uint256                            vvec_hash;
    std::vector<std::string>           member_node_ids;    // effective-QUAL, share_index order
    int                                formation_height{0};
    std::map<uint256, PTXDKGPhase4Msg> premit_commitments; // ≥ t entries (W1.2: structural only)

    SERIALIZE_METHODS(PTXDKGPayload, obj)
    {
        READWRITE(obj.quorum_hash);
        READWRITE(Using<PTXFixedBytesFormatter<48>>(obj.group_pk_bytes));
        READWRITE(obj.vvec_hash, obj.member_node_ids,
                  obj.formation_height, obj.premit_commitments);
    }
};

// ---------------------------------------------------------------------------
// PTXDKGSession — ceremony state machine.
// Phase 0 state only; later phases extend this.
//
// The CBLSSecretKey (operator key) is NOT stored here — it is accessed via
// the caller's reference at signing time (PTX_DKG_BuildPhase0Msg parameter).
// This enforces the key-separation invariant at the type level.
// ---------------------------------------------------------------------------
struct PTXDKGSession {
    uint256                    quorum_hash;     // formation block hash
    std::vector<PTXDKGMember>  members;         // sorted by chain-determined score desc (KDD-052)
    int                        my_idx{-1};      // index into members[] for this node; -1 = not a member
    PTXDKGPhase                phase{PTXDKGPhase::IDLE};

    // Local contribution — blst ceremony arithmetic; no operator key material
    PTXDKGLocalContrib         local_contrib;

    // Phase 0 received commitments: proTxHash -> commitment_hash
    std::map<uint256, uint256> phase0_commits;

    // QUAL: proTxHash set of members who committed before Phase 0 closed.
    // Locked by PTX_DKG_ClosePhase0() before any Phase 1 reveal begins
    // (the QUAL-locks-before-reveal invariant from KDD-051).
    std::set<uint256>          qual;

    // Phase 1 revealed vvecs: proTxHash → vvec (t G1 points).
    // Populated by PTX_DKG_ReceivePhase1Msg on acceptance.
    std::map<uint256, std::vector<blst_p1_affine>>  phase1_vvecs;

    // Phase 1 encrypted share blobs: proTxHash → blob set.
    // Held between PTX_DKG_ReceivePhase1Msg and PTX_DKG_DecryptMyShare.
    std::map<uint256, CBLSIESMultiRecipientBlobs>   phase1_encrypted_shares;

    // Decrypted shares: proTxHash_of_sender → f_sender(my share_index).
    // Populated by PTX_DKG_DecryptMyShare.  Values are NOT validated here;
    // share validity is established by the Phase 2 Feldman check.
    std::map<uint256, blst_scalar>                  received_shares;

    // Bad members, accumulated across phases: proTxHash set.
    // Phase 1 adds: commitment-mismatch revealers + non-revealers (at ClosePhase1).
    // Phase 2 adds: Feldman-VSS failures.  Phase 3 adds: unjustified accusations.
    std::set<uint256>                               bad_members;

    // Phase 2 — complaints received.
    // complaints_against[dealer] = set of complainant proTxHashes with
    // outstanding complaints against that dealer.
    std::map<uint256, std::set<uint256>>            complaints_against;

    // Phase 3 — resolution tracking.
    // justified_for[dealer] = set of complainant proTxHashes whose (D,C) pair
    // has been resolved (Branch 2 or 3a); removes the pair from the
    // ClosePhase3 unresolved sweep (Branch 3b).
    std::map<uint256, std::set<uint256>>            justified_for;

    // Phase 4 — premature commitments received: proTxHash → PTXDKGPhase4Msg.
    std::map<uint256, PTXDKGPhase4Msg>              phase4_premit_msgs;

    // Phase 4 — this member's computed aggregates (set by ComputeSkShare + ComputeGroupPk).
    blst_scalar    sk_share_i;
    blst_p1_affine group_pk;
    bool           phase4_computed{false}; // guard: prevents double-compute

    // SG-2a — session-state lock (Resolution A).  EXTERNAL / coarse: the pure
    // PTX_DKG_Receive*/Close*/Build*/Compute* functions stay lock-AGNOSTIC (so
    // the single-threaded unit rows and the in-suite harness call them without
    // locking).  Two writers exist in the daemon: the transport DRAIN thread
    // (ProcessBatchT → ReceiveDispatch) and the SG-2 ceremony STEP thread
    // (PTX_Ceremony_Step).  Both serialise on this mutex at their call
    // boundaries.  Under SG-1c the ceremony thread was parked (drain = sole
    // mutator, race-free); SG-2 makes it a second writer — this lock closes
    // that race.  NEVER field-GUARDED_BY (that would force locking inside the
    // pure fns and trip -Wthread-safety in the unlocked test callers).
    //
    // LOCK-ORDER INVARIANTS (SG-2a, assertable — see ptx_ceremony_driver.cpp):
    //   1. `cs` is NEVER held across a send (transport.ProcessMessage →
    //      PushPendingMessage takes the pending-queue lock).  The step mutates
    //      under `cs`, RELEASES, then the caller transmits.  Pairs with
    //      ProcessBatchT, which pops the pending queue (releasing its lock)
    //      BEFORE taking `cs` for ReceiveDispatch — neither side nests them.
    //   2. `cs` and cs_main are NEVER held simultaneously.  Height is read
    //      under cs_main (transient) strictly BEFORE `cs` is taken; the sole
    //      DKG call reaching cs_main — ClosePhase5 (→ SetPendingTx,
    //      ptx_dkg_pending.cpp order cs_main→pending) — is invoked OUTSIDE
    //      `cs` (sound: at FINALIZE every ReceivePhaseN early-returns on its
    //      phase gate, so the terminal close is single-writer).
    //
    // unique_ptr-held (not a bare member): RecursiveMutex is non-movable and
    // the phase-test TUs hold sessions in std::vector (resize requires
    // MoveInsertable) — the indirection keeps the struct movable.
    std::unique_ptr<RecursiveMutex> cs{new RecursiveMutex()};
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// SHA256(proTxHash||confirmedHash): the inner hash stored in DGM state as
// confirmedHashWithProRegTxHash.  Matches CDGMState::UpdateConfirmedHash
// (deterministicgms.h:116-119).  Exposed so formation code and tests can
// build PTXDKGMember without instantiating CDGMState.
uint256 PTX_DKG_ComputeInnerHash(const uint256& proTxHash, const uint256& confirmedHash);

// PTX quorum eligibility predicate (KDD-060).
// A GM is eligible for PTX ceremony formation iff it has a non-empty node_id
// (requires v3+ ProRegPL; v1/v2 and v3-with-empty-id are excluded).
// node_id-ONLY by design: answers "can this GM run the ceremony" (sign messages,
// contribute BLS shares).  scriptPTXPayment is NOT required here — a GM without
// a payout script is a valid ceremony participant; the payout script is only
// required for winner-selection eligibility (ptx_winner_selection.cpp Amendment 2).
// Exported (not static) because the Package 2 validator in specialtx_validation.cpp
// must call the same function; the predicate is never re-inlined.
bool PTX_DKG_IsGMPTXEligible(const std::shared_ptr<const CDeterministicGM>& dgm);

// Canonical quorum selection core (KDD-060): filter the list by
// PTX_DKG_IsGMPTXEligible into a fresh CDeterministicGMList, then
// CalculateQuorum(11, formation_block_hash).  This is the SINGLE selection
// implementation shared by ceremony formation (via
// PTX_DKG_BuildMemberVectorFromList, which wraps it) and the Package 2
// consensus validator (which calls it directly for the proTxHash set), so both
// reconstruct byte-identical membership.  GetListForBlock is intentionally NOT
// performed here — the caller supplies the formation-block snapshot.
std::vector<std::shared_ptr<const CDeterministicGM>> PTX_DKG_SelectQuorumFromList(
        const CDeterministicGMList& list,
        const uint256& formation_block_hash);

// Build the ordered PTXDKGMember vector from an on-chain GM list (KDD-060).
// Thin wrapper over PTX_DKG_SelectQuorumFromList that maps each selected GM to a
// PTXDKGMember.  Quorum is selected from the eligible candidate set only.
// share_index fields are NOT assigned here — PTX_DKG_InitSession assigns them.
std::vector<PTXDKGMember> PTX_DKG_BuildMemberVectorFromList(
        const CDeterministicGMList& list,
        const uint256& formation_block_hash);

// Contextual premit verification — V6–V8 of CheckPTXDKGTx (KDD-059/060, Package 2).
// ACCOUNTABILITY, not correctness: confirms that >= t members of the
// canonically-selected quorum each signed AGREEMENT on (group_pk, vvec_hash) with
// their DGM-registered operator key.  It does NOT prove group_pk is the correct
// DKG output (vvec_hash is checked for agreement, never against a real vvec; the
// group key is never verified as an aggregate).  quorum11 is the V5 output of
// PTX_DKG_SelectQuorumFromList.  First failing premit rejects with DoS 100.
bool PTX_DKG_VerifyPremits(
        const std::vector<std::shared_ptr<const CDeterministicGM>>& quorum11,
        const PTXDKGPayload& payload,
        CValidationState& state);

// Initialise session from a caller-supplied, already-ordered member list (KDD-060).
// Caller is responsible for supplying members in CalculateQuorum output order
// (e.g. via PTX_DKG_BuildMemberVectorFromList). InitSession preserves that order
// and never re-sorts. Assigns share_index 1..n in the order received.
// Returns false on: n != 11, null confirmedHash in any member, duplicate proTxHash.
// my_idx == -1 after success means this node is not a session member (valid for
// a non-member observer); caller decides whether that is an error.
bool PTX_DKG_InitSession(PTXDKGSession& session,
                          std::vector<PTXDKGMember> members,
                          const uint256& formation_block_hash,
                          const uint256& my_proTxHash);

// Generate this node's local contribution: polynomial, vvec, evals, commitment.
// Session must be in HASH_COMMIT phase with my_idx >= 0.
//
// Phase 0 commitment binds the vvec (= full polynomial in public form).
// Phase 1 reveal verification recomputes SHA256(quorum_hash||proTxHash||vvec_bytes)
// against this stored value.
// Per-recipient share verification (Feldman VSS check) happens in Phase 2
// (Complaint) per-recipient — each member checks only its own share against the
// revealed vvec.  The commitment does not bind evaluations; receivers cannot
// verify evaluations at indices other than their own.
bool PTX_DKG_GenerateLocalContrib(PTXDKGSession& session);

// Build the Phase 0 P2P message (commitment only; vvec NOT revealed here).
// operator_sk: the GM's chiabls operator key — at real call sites, pass
//   *activeGamemasterManager->OperatorKey().  Taken as a parameter so the
//   function is testable without a live daemon and the operator key is never
//   stored in the session struct (key-separation invariant, impl plan §6).
PTXDKGPhase0Msg PTX_DKG_BuildPhase0Msg(const PTXDKGSession& session,
                                        const CBLSSecretKey& operator_sk);

// Receive and validate a Phase 0 message.
// Rejects: wrong quorum_hash, unknown sender, duplicate, bad signature.
// On acceptance, stores commitment in session.phase0_commits.
bool PTX_DKG_ReceivePhase0Msg(PTXDKGSession& session, const PTXDKGPhase0Msg& msg);

// True when every session member has sent a valid Phase 0 message.
bool PTX_DKG_IsPhase0Complete(const PTXDKGSession& session);

// Close Phase 0: lock QUAL = all who committed, advance phase to CONTRIB.
// Returns false and sets phase = ABORTED if qual.size() < t=6.
// Closing with fewer than t committed members produces a broken under-threshold
// ceremony; fail loud here rather than propagating a broken session through the
// expensive reveal and complaint rounds.
// Must be called before any Phase 1 processing; this is the
// QUAL-locks-before-reveal gate (KDD-051).
bool PTX_DKG_ClosePhase0(PTXDKGSession& session);

// ---------------------------------------------------------------------------
// Phase 1 — contribution reveal (CONTRIB phase)
// ---------------------------------------------------------------------------

// Build the Phase 1 P2P message (vvec reveal + per-QUAL-member encrypted evals).
// Session must be in CONTRIB phase with my_idx >= 0.
// operator_sk: signs the message; never stored (key-separation invariant, impl plan §6).
//
// Index basis: evals[k] = f(members[k].share_index), slot k = position in sorted
// members[].  Encrypt(k, ...) so Decrypt(my_idx, ...) on the receiver reaches the
// right blob without any index translation.
PTXDKGPhase1Msg PTX_DKG_BuildPhase1Msg(const PTXDKGSession& session,
                                        const CBLSSecretKey& operator_sk);

// Receive and validate a Phase 1 message.
// Check order: phase==CONTRIB → quorum_hash match → sender in QUAL → no duplicate
//              → sig VerifyInsecure → commitment match.
//
// Commitment match: recomputes SHA256(quorum_hash||proTxHash||compressed-vvec-bytes)
// from msg.vvec and compares to phase0_commits[proTxHash].  This is the GJKR
// redemption check — mismatch means the sender revealed a different polynomial
// than it committed; sender is inserted into bad_members, false returned.
//
// On acceptance: stores vvec in phase1_vvecs and blob set in phase1_encrypted_shares.
// Does NOT decrypt — call PTX_DKG_DecryptMyShare separately.
bool PTX_DKG_ReceivePhase1Msg(PTXDKGSession& session, const PTXDKGPhase1Msg& msg);

// Decrypt this node's share from sender_proTxHash's Phase 1 message.
// Requires phase1_encrypted_shares to contain sender_proTxHash.
// operator_sk: this node's operator secret key; never stored.
// On success: stores the decoded blst_scalar in received_shares[sender_proTxHash].
//
// IMPORTANT: Decrypt success and blob.size()==32 do NOT establish share validity.
// A wrong-key or wrong-routed decrypt yields a well-formed but incorrect scalar
// (AES-CBC without padding always "succeeds" and always returns 32 bytes regardless
// of whether the key is right).  Share validity is established only by the Phase 2
// Feldman check (g^{share} == Π vvec[k]^{j^k}).  The size==32 guard is a
// structural/encoding guard, not a correctness guarantee.
bool PTX_DKG_DecryptMyShare(PTXDKGSession& session,
                             const uint256& sender_proTxHash,
                             const CBLSSecretKey& operator_sk);

// True when every QUAL member has sent a valid (accepted) Phase 1 message.
bool PTX_DKG_IsPhase1Complete(const PTXDKGSession& session);

// Close Phase 1: mark non-revealing QUAL members bad, advance to COMPLAINT.
// Returns false and sets phase = ABORTED if revealed set (qual − bad_members) < t=6.
bool PTX_DKG_ClosePhase1(PTXDKGSession& session);

// ---------------------------------------------------------------------------
// Phase 2 — complaint (COMPLAINT phase)
// ---------------------------------------------------------------------------

// Feldman VSS check: verify share == f_D(j) against D's committed vvec.
// share = decrypted value from received_shares[dealer]; vvec = phase1_vvecs[dealer];
// j = session.members[session.my_idx].share_index (1-indexed, NOT my_idx).
// Returns true iff g^{share} == Π_{k=0}^{t-1} vvec[k]^{j^k}.
bool PTX_DKG_FeldmanCheck(const blst_scalar& share,
                           const std::vector<blst_p1_affine>& vvec,
                           int j);

// Build a Phase 2 complaint message against dealer_proTxHash.
// Pre: phase == COMPLAINT, my_idx >= 0, dealer_proTxHash in effective-QUAL,
//      received_shares contains dealer_proTxHash.
PTXDKGPhase2Msg PTX_DKG_BuildPhase2Msg(const PTXDKGSession& session,
                                        const uint256& dealer_proTxHash,
                                        const CBLSSecretKey& operator_sk);

// Receive and validate a Phase 2 complaint message.
// Check order: phase==COMPLAINT → quorum_hash → complainant in members →
//   dealer in members → complainant in qual → complainant not in bad_members →
//   dealer in qual → dealer not in bad_members → no duplicate →
//   sig VerifyInsecure → share_index_j matches complainant's share_index.
// On acceptance: complaints_against[dealer].insert(complainant); return true.
bool PTX_DKG_ReceivePhase2Msg(PTXDKGSession& session, const PTXDKGPhase2Msg& msg);

// Close Phase 2: advance COMPLAINT → JUSTIFY.
// No threshold check here — P3 markings haven't happened; premature abort
// would be wrong.  Threshold is enforced at ClosePhase3.
// Returns false only if phase != COMPLAINT (programmer error).
bool PTX_DKG_ClosePhase2(PTXDKGSession& session);

// ---------------------------------------------------------------------------
// Phase 3 — justification (JUSTIFY phase)
// ---------------------------------------------------------------------------

// Build a Phase 3 justification message answering complainant_proTxHash's complaint.
// Pre: phase == JUSTIFY, my_idx >= 0,
//      complainant_proTxHash in complaints_against[my_proTxHash].
// complainant_slot derived from session.members[] (correction B);
// revealed_share = session.local_contrib.evals[complainant_slot] (slot-indexed).
PTXDKGPhase3Msg PTX_DKG_BuildPhase3Msg(const PTXDKGSession& session,
                                        const uint256& complainant_proTxHash,
                                        const CBLSSecretKey& operator_sk);

// Receive and validate a Phase 3 justification message.
// Check order: phase==JUSTIFY → quorum_hash → dealer in members →
//   complainant in members → dealer in qual → dealer not in bad_members →
//   complainant in qual → complaint outstanding → no duplicate resolution →
//   sig VerifyInsecure → revealed_share is valid field element (blst_scalar_fr_check) →
//   derive j from session.members[complainant_slot].share_index →
//   PTX_DKG_FeldmanCheck(revealed_share, phase1_vvecs[dealer], j):
//     passes → §15 Branch 2: bad_members.insert(complainant); justified_for[dealer].insert(complainant)
//     fails  → §15 Branch 3a: bad_members.insert(dealer);     justified_for[dealer].insert(complainant)
// Returns true on acceptance (both branches); false on any pre-Feldman rejection.
bool PTX_DKG_ReceivePhase3Msg(PTXDKGSession& session, const PTXDKGPhase3Msg& msg);

// Close Phase 3: Branch 3b sweep + threshold check.
// For each D in qual not already in bad_members: if any complaint against D
// remains unresolved (complaints_against[D] − justified_for[D] non-empty),
// bad_members.insert(D).
// Then: effective = |qual| − |bad_members|.
// If effective < t=6: session.phase = ABORTED; return false.
// Otherwise: session.phase = PREMIT; return true.
bool PTX_DKG_ClosePhase3(PTXDKGSession& session);

// ---------------------------------------------------------------------------
// Phase 4 — premature commitment (PREMIT phase)
// ---------------------------------------------------------------------------

// Aggregate valid contributions at my share_index across effective-QUAL dealers.
// Pre: phase == PREMIT, my_idx >= 0.
// ABORT (return false) if any effective-QUAL dealer is missing from received_shares
// (local session error — missing DecryptMyShare call). NEVER skip and continue:
// a missing dealer omitted silently produces a wrong sk_share_i with no failure signal.
// Stores result in session.sk_share_i.
bool PTX_DKG_ComputeSkShare(PTXDKGSession& session);

// Compute group_pk = Σ vvec[0] over effective-QUAL.
// Pre: phase == PREMIT.
// Uses local_contrib.vvec[0] for this node's own vvec[0] (NOT from phase1_vvecs —
// own Phase 1 message is never stored there). See GF2 trap.
// Uses blst_p1_add_or_double_affine for accumulation (blst.h:176).
// Stores result in session.group_pk; sets phase4_computed = true.
bool PTX_DKG_ComputeGroupPk(PTXDKGSession& session);

// Build Phase 4 premature commitment message.
// Pre: phase == PREMIT, my_idx >= 0, phase4_computed == true.
// operator_sk never stored (key-separation invariant, impl plan §6).
PTXDKGPhase4Msg PTX_DKG_BuildPhase4Msg(const PTXDKGSession& session,
                                        const CBLSSecretKey& operator_sk);

// Receive and validate a Phase 4 message.
// Check order: phase==PREMIT → quorum_hash → sender in members → sender in qual →
//   sender not in bad_members → no duplicate → sig VerifyInsecure →
//   group_pk_bytes decompresses (blst_p1_uncompress == BLST_SUCCESS).
// On acceptance: stores msg in phase4_premit_msgs[proTxHash].
bool PTX_DKG_ReceivePhase4Msg(PTXDKGSession& session, const PTXDKGPhase4Msg& msg);

// True when ≥ t accepted Phase 4 messages agree on group_pk_bytes with this node's.
bool PTX_DKG_IsPhase4Complete(const PTXDKGSession& session);

// Close Phase 4: count consistent premature commitments (group_pk_bytes bytewise equal
// to this node's computed group_pk). < t consistent → phase = ABORTED, return false.
// ≥ t consistent → phase = FINALIZE, return true.
bool PTX_DKG_ClosePhase4(PTXDKGSession& session);

// ---------------------------------------------------------------------------
// Phase 5 — finalize
// ---------------------------------------------------------------------------

// Write sk_share_i to the keyed share store g_ptx_my_shares (KDD-070 P1).
// Serializes session.sk_share_i with blst_bendian_from_scalar → 32 bytes and
// stores it under session.quorum_hash (role CURRENT), with formation_height.
// Writes under cs_ptx_my_bls_sk lock.
//
// The SINGLE write site to the share store post KDD-069. Routes through the §C1
// per-key refuse-unless-empty guard (PTX_BLS_SetSkShare), which now protects
// against ceremony replay / double-store (the gm_bls_keyset RPC path was removed
// with the trusted dealer).
//
// Pre: phase == FINALIZE, sk_share_i computed.
bool PTX_DKG_StoreSkShare(const PTXDKGSession& session, int formation_height);

// Construct the PTXDKG special transaction.
// Sets nType = PTXDKG (11), nVersion = SAPLING.
// Populates PTXDKGPayload and serializes to extraPayload.
// Does NOT submit — submission model TBD per ODC-029.
// Pre: phase == FINALIZE, phase4_computed == true.
CMutableTransaction PTX_DKG_BuildPTXDKGTx(const PTXDKGSession& session,
                                            int formation_height);

// Phase 5 close: StoreSkShare → BuildPTXDKGTx → phase = DONE.
// Returns false (phase = ABORTED) if StoreSkShare fails.
// ptxdkg_tx_out receives the constructed transaction.
bool PTX_DKG_ClosePhase5(PTXDKGSession& session,
                           int formation_height,
                           CMutableTransaction& ptxdkg_tx_out);

#endif // HEMIS_PTX_DKG_H
