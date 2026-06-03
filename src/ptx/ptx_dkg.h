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

// Accessor for the global BLS state singleton.
// All ceremony code uses this instead of g_ptx_bls_state directly, preparing
// for the W2.1 per-quorum registry refactor.
struct PTXBLSState;
PTXBLSState& PTX_GetBLSState();

#endif // HEMIS_PTX_DKG_H
