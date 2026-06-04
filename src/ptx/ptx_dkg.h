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
//     Phase 4/5 aggregation; held in PTXDKGLocalContrib during the ceremony,
//     then written to g_ptx_bls_state via PTX_GetBLSState() in Phase 5.
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
#include "uint256.h"

#include <map>
#include <set>
#include <string>
#include <vector>

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
    int           share_index{0};                 // 1-indexed; assigned by PTX_DKG_SortMembers (KDD-052)
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
};

// ---------------------------------------------------------------------------
// API
// ---------------------------------------------------------------------------

// SHA256(proTxHash||confirmedHash): the inner hash stored in DGM state as
// confirmedHashWithProRegTxHash.  Matches CDGMState::UpdateConfirmedHash
// (deterministicgms.h:116-119).  Exposed so formation code and tests can
// build PTXDKGMember without instantiating CDGMState.
uint256 PTX_DKG_ComputeInnerHash(const uint256& proTxHash, const uint256& confirmedHash);

// Score for one member at formation.
// confirmedHashWithProRegTxHash = PTX_DKG_ComputeInnerHash(proTxHash, confirmedHash).
// Matches deterministicgms.cpp CalculateScores (single SHA256, raw byte writes).
uint256 PTX_DKG_ComputeMemberScore(const uint256& confirmedHashWithProRegTxHash,
                                   const uint256& formation_block_hash);

// Sort members by score descending, assign share_index 1..n (KDD-052).
// Returns false if any member has a null confirmedHash.
bool PTX_DKG_SortMembers(std::vector<PTXDKGMember>& members,
                          const uint256& formation_block_hash);

// Initialise session from a caller-supplied member list.
// Applies SortMembers, requires exactly 11 members, finds my_idx.
// Returns false on: null confirmedHash, n != 11.
// my_idx == -1 after success means this node is not in the session (valid for
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

// Accessor for the global BLS state singleton.
// All ceremony code uses this instead of g_ptx_bls_state directly, preparing
// for the W2.1 per-quorum registry refactor.
struct PTXBLSState;
PTXBLSState& PTX_GetBLSState();

#endif // HEMIS_PTX_DKG_H
