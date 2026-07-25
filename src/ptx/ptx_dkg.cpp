// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_dkg.h"
#include "ptx/ptx_bls.h"
#include "ptx/ptx_dkg_commitments.h"  // KDD-058-A: replicated minable-commitments store

#include "evo/deterministicgms.h"  // CDeterministicGMList, CDeterministicGMCPtr (KDD-060)
#include "evo/evodb.h"             // evoDb (KDD-070 P2 share persistence)
#include "consensus/validation.h"  // CValidationState, REJECT_INVALID (Package 2 validator)

#include "arith_uint256.h"
#include "crypto/sha256.h"
#include "logging.h"
#include "primitives/transaction.h"
#include "random.h"
#include "compat/endian.h"

#include <algorithm>
#include <cassert>
#include <cstring>

// ---------------------------------------------------------------------------
// PTXDKGPhase0Msg::GetSignHash
// Covers quorum_hash || proTxHash || commitment so the signature binds the
// sender identity, the ceremony, and the committed polynomial simultaneously.
// ---------------------------------------------------------------------------

uint256 PTXDKGPhase0Msg::GetSignHash() const
{
    uint256 result;
    CSHA256 h;
    h.Write(quorum_hash.begin(), quorum_hash.size());
    h.Write(proTxHash.begin(), proTxHash.size());
    h.Write(commitment.begin(), commitment.size());
    h.Finalize(result.begin());
    return result;
}

// ---------------------------------------------------------------------------
// ComputeVvecCommitment (internal)
// SHA256(quorum_hash || proTxHash || compressed-G1-bytes-for-each-vvec-point).
// Binds the sender to its vvec (= full polynomial in public form) before reveal.
//
// Phase 1 reveal verification recomputes this exact hash from the revealed vvec
// and checks it against the Phase 0 stored value.
//
// Per-recipient share verification is separate: the Feldman VSS check in Phase 2
// (Complaint) lets each receiver verify only its own received share against the
// revealed vvec — g^{f_i(j)} == prod_k vvec_i[k]^{j^k}.  The commitment does
// not cover evaluations because each receiver can only check the share at its
// own index.  Complaint logic is designed around per-recipient Feldman checks,
// not whole-commitment re-hashing.
// ---------------------------------------------------------------------------

static uint256 ComputeVvecCommitment(const uint256& quorum_hash,
                                     const uint256& proTxHash,
                                     const std::vector<blst_p1_affine>& vvec)
{
    uint256 result;
    CSHA256 h;
    h.Write(quorum_hash.begin(), quorum_hash.size());
    h.Write(proTxHash.begin(), proTxHash.size());
    for (const auto& pt : vvec) {
        uint8_t buf[48];
        blst_p1_affine_compress(buf, &pt);
        h.Write(buf, 48);
    }
    h.Finalize(result.begin());
    return result;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ComputeInnerHash
// SHA256(proTxHash || confirmedHash) — matches CDGMState::UpdateConfirmedHash
// (deterministicgms.h:116-119).  Single SHA256, raw byte writes.
// ---------------------------------------------------------------------------

uint256 PTX_DKG_ComputeInnerHash(const uint256& proTxHash, const uint256& confirmedHash)
{
    uint256 result;
    CSHA256 h;
    h.Write(proTxHash.begin(), proTxHash.size());
    h.Write(confirmedHash.begin(), confirmedHash.size());
    h.Finalize(result.begin());
    return result;
}

// ---------------------------------------------------------------------------
// PTX_DKG_IsGMPTXEligible (KDD-060)
// node_id-only predicate: answers "can this GM run the ceremony."
// scriptPTXPayment is NOT required for ceremony participation (only for
// winner-selection eligibility; see ptx_winner_selection.cpp Amendment 2).
// Exported so the Package 2 validator in specialtx_validation.cpp calls the
// same function — the predicate is never re-inlined elsewhere.
// ---------------------------------------------------------------------------

bool PTX_DKG_IsGMPTXEligible(const CDeterministicGMCPtr& dgm)
{
    return !dgm->pdgmState->node_id.empty();
}

// ---------------------------------------------------------------------------
// PTX_DKG_SelectQuorumFromList (KDD-060)
// The single canonical quorum-selection core.  Filter-then-CalculateQuorum:
// build the eligible sublist first, then score and rank.  Post-filter drop is
// unsafe (changes the scoring competition).  Both ceremony formation (via
// PTX_DKG_BuildMemberVectorFromList) and the Package 2 validator call this, so
// formation and validation reconstruct byte-identical membership.
// ---------------------------------------------------------------------------

std::vector<CDeterministicGMCPtr> PTX_DKG_SelectQuorumFromList(
        const CDeterministicGMList& list,
        const uint256& formation_block_hash)
{
    CDeterministicGMList eligible;
    list.ForEachGM(true, [&](const CDeterministicGMCPtr& dgm) {
        if (PTX_DKG_IsGMPTXEligible(dgm))
            eligible.AddGM(dgm);
    });

    return eligible.CalculateQuorum(11, formation_block_hash);
}

// ---------------------------------------------------------------------------
// PTX_DKG_BuildMemberVectorFromList (KDD-060)
// Thin wrapper over the selection core that maps each selected GM to a
// PTXDKGMember.
// ---------------------------------------------------------------------------

std::vector<PTXDKGMember> PTX_DKG_BuildMemberVectorFromList(
        const CDeterministicGMList& list,
        const uint256& formation_block_hash)
{
    auto quorum = PTX_DKG_SelectQuorumFromList(list, formation_block_hash);
    std::vector<PTXDKGMember> result;
    result.reserve(quorum.size());
    for (const auto& dgm : quorum) {
        PTXDKGMember m;
        m.proTxHash                     = dgm->proTxHash;
        m.confirmedHash                 = dgm->pdgmState->confirmedHash;
        m.confirmedHashWithProRegTxHash = dgm->pdgmState->confirmedHashWithProRegTxHash;
        m.node_id                       = dgm->pdgmState->node_id;
        m.pubKeyOperator                = dgm->pdgmState->pubKeyOperator.Get();
        result.push_back(std::move(m));
    }
    return result;
}

// ---------------------------------------------------------------------------
// PTX_DKG_VerifyPremits (KDD-059/060, Package 2) — V6–V8 of CheckPTXDKGTx.
// Per-premit operator-key signature AGREEMENT against the canonically-selected
// quorum.  Accountability, not correctness: proves >= t quorum members each
// attested (group_pk, vvec_hash) with their DGM-registered operator key; it does
// NOT prove group_pk is the correct DKG output.  First failing entry rejects;
// DoS 100 throughout, matching the LLMQ commitment template severity.
// ---------------------------------------------------------------------------

bool PTX_DKG_VerifyPremits(const std::vector<CDeterministicGMCPtr>& quorum11,
                           const PTXDKGPayload& payload,
                           CValidationState& state)
{
    // V6: proTxHash -> GM map over the canonical quorum.  V7f reads the operator
    // key off this map (the selected CDeterministicGMCPtr), so no second registry
    // lookup is needed.
    std::map<uint256, CDeterministicGMCPtr> quorum_map;
    for (const auto& dgm : quorum11)
        quorum_map.emplace(dgm->proTxHash, dgm);

    // V7: per premit entry, first failure rejects.  V8 strictness is exactly
    // "every entry must pass" — there is no count-the-survivors path.  The
    // >= t=6 entry count is enforced structurally in the null-path body before
    // this runs.
    for (const auto& kv : payload.premit_commitments) {
        const uint256& key        = kv.first;
        const PTXDKGPhase4Msg& p4 = kv.second;

        // V7a: map key must equal the attested inner identity, so distinct map
        // keys imply distinct attesting members (the sig covers the inner
        // proTxHash, not the key).
        if (key != p4.proTxHash)
            return state.DoS(100, false, REJECT_INVALID, "ptxdkg-premit-key-mismatch");

        // V7b: committer must be in the canonically-selected quorum (KDD-060),
        // not merely DGM-registered.
        auto it = quorum_map.find(key);
        if (it == quorum_map.end())
            return state.DoS(100, false, REJECT_INVALID, "ptxdkg-committer-not-in-quorum");

        // V7c: premit must reference this ceremony.
        if (p4.quorum_hash != payload.quorum_hash)
            return state.DoS(100, false, REJECT_INVALID, "ptxdkg-premit-quorum-mismatch");

        // V7d: each premit's group_pk must agree with the payload's (each-equals
        // -payload gives pairwise agreement, the KDD-059 clause).
        if (memcmp(p4.group_pk_bytes, payload.group_pk_bytes, 48) != 0)
            return state.DoS(100, false, REJECT_INVALID, "ptxdkg-premit-grouppk-mismatch");

        // V7e: each premit's vvec_hash must agree with the payload's, else the
        // payload vvec_hash is unattested data.
        if (p4.vvec_hash != payload.vvec_hash)
            return state.DoS(100, false, REJECT_INVALID, "ptxdkg-premit-vvechash-mismatch");

        // V7f: operator key off the V6 map.  Lazy .Get() returns a static invalid
        // object on bad bytes and never throws, so fail fast on !IsValid().
        CBLSPublicKey pk = it->second->pdgmState->pubKeyOperator.Get();
        if (!pk.IsValid())
            return state.DoS(100, false, REJECT_INVALID, "ptxdkg-bad-operator-key");

        // V7g: chiabls BasicSchemeMPL basic verify over the premit sign-hash — the
        // exact path the ceremony sign/receive use (NOT VerifySecureAggregated;
        // PTX has no aggregate sig). KDD-072 P-b2: the PAYLOAD's predecessor view
        // — for a v1 payload the field is zero (never serialized) and the preimage
        // is the pre-P-b2 144 bytes; for a rotation the premits must have been
        // SIGNED over this exact predecessor or they fail here (§3: a formation's
        // premits cannot be re-cast as a rotation).
        if (!p4.sig.VerifyInsecure(pk, p4.GetSignHash(payload.predecessor_quorum_hash)))
            return state.DoS(100, false, REJECT_INVALID, "ptxdkg-bad-premit-sig");
    }

    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_InitSession (KDD-060)
// Caller supplies members in CalculateQuorum output order; InitSession
// preserves that order, never re-sorts.  Assigns share_index 1..n in the
// order received.
// ---------------------------------------------------------------------------

bool PTX_DKG_InitSession(PTXDKGSession& session,
                          std::vector<PTXDKGMember> members,
                          const uint256& formation_block_hash,
                          const uint256& my_proTxHash)
{
    if ((int)members.size() != 11)
        return false;

    // KDD-060 precondition: every member must have a non-null confirmedHash.
    for (const auto& m : members) {
        if (m.confirmedHash.IsNull())
            return false;
    }

    // Dup-proTxHash guard: a duplicate causes share_index collisions and index
    // mapping ambiguity throughout the ceremony.
    std::set<uint256> seen;
    for (const auto& m : members) {
        if (!seen.insert(m.proTxHash).second)
            return false;
    }

    for (int i = 0; i < (int)members.size(); i++)
        members[i].share_index = i + 1;

    session.quorum_hash = formation_block_hash;
    session.members     = std::move(members);
    session.my_idx      = -1;
    session.phase       = PTXDKGPhase::HASH_COMMIT;

    for (int i = 0; i < (int)session.members.size(); i++) {
        if (session.members[i].proTxHash == my_proTxHash) {
            session.my_idx = i;
            break;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_GenerateLocalContrib
// ---------------------------------------------------------------------------

bool PTX_DKG_GenerateLocalContrib(PTXDKGSession& session)
{
    if (session.phase != PTXDKGPhase::HASH_COMMIT || session.my_idx < 0)
        return false;

    const int n = (int)session.members.size();
    const int t = 6; // KDD-048

    PTXDKGLocalContrib& c = session.local_contrib;
    c.coeffs.resize(t);
    c.vvec.resize(t);
    c.evals.resize(n);

    // Generate t random polynomial coefficients over Zr.
    // Arithmetic mirrors PTX_BLS_Init (ptx_bls.cpp:40-49) exactly.
    for (int i = 0; i < t; i++) {
        uint8_t ikm[32];
        GetStrongRandBytes(ikm, 32);
        blst_scalar tmp;
        blst_keygen(&tmp, ikm, 32, nullptr, 0);
        blst_fr_from_scalar(&c.coeffs[i], &tmp);
    }

    // vvec[k] = g^{coeffs[k]} in G1.
    for (int k = 0; k < t; k++) {
        blst_scalar sk;
        blst_scalar_from_fr(&sk, &c.coeffs[k]);
        blst_p1 pt;
        blst_sk_to_pk_in_g1(&pt, &sk);
        blst_p1_to_affine(&c.vvec[k], &pt);
    }

    // evals[i] = f(share_index_i) by Horner's method.
    // Mirrors PTX_BLS_Init (ptx_bls.cpp:58-76), using chain-determined
    // share_index (KDD-052) as x rather than the old alphabetical position.
    for (int i = 0; i < n; i++) {
        int xi = session.members[i].share_index;

        uint64_t xi_val[4] = {(uint64_t)xi, 0, 0, 0};
        blst_fr xi_fr;
        blst_fr_from_uint64(&xi_fr, xi_val);

        blst_fr share = c.coeffs[0];
        blst_fr xi_pow;
        { const uint64_t one[4] = {1, 0, 0, 0}; blst_fr_from_uint64(&xi_pow, one); }

        for (int j = 1; j < t; j++) {
            blst_fr_mul(&xi_pow, &xi_pow, &xi_fr);
            blst_fr term;
            blst_fr_mul(&term, &c.coeffs[j], &xi_pow);
            blst_fr_add(&share, &share, &term);
        }
        blst_scalar_from_fr(&c.evals[i], &share);
    }

    const uint256& my_proTxHash = session.members[session.my_idx].proTxHash;
    c.commitment = ComputeVvecCommitment(session.quorum_hash, my_proTxHash, c.vvec);

    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_BuildPhase0Msg
// operator_sk: the GM's chiabls operator key — at real call sites, pass
//   *activeGamemasterManager->OperatorKey().  Not fetched internally so this
//   function is testable without a live daemon and so the key is never stored
//   in the session struct (key-separation invariant, impl plan §6).
// ---------------------------------------------------------------------------

PTXDKGPhase0Msg PTX_DKG_BuildPhase0Msg(const PTXDKGSession& session,
                                        const CBLSSecretKey& operator_sk)
{
    assert(session.my_idx >= 0);
    const PTXDKGMember& me = session.members[session.my_idx];

    PTXDKGPhase0Msg msg;
    msg.quorum_hash = session.quorum_hash;
    msg.proTxHash   = me.proTxHash;
    msg.commitment  = session.local_contrib.commitment;
    msg.sig         = operator_sk.Sign(msg.GetSignHash());
    return msg;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ReceivePhase0Msg
// ---------------------------------------------------------------------------

bool PTX_DKG_ReceivePhase0Msg(PTXDKGSession& session, const PTXDKGPhase0Msg& msg)
{
    if (session.phase != PTXDKGPhase::HASH_COMMIT)
        return false;
    if (msg.quorum_hash != session.quorum_hash)
        return false;

    const PTXDKGMember* member = nullptr;
    for (const auto& m : session.members) {
        if (m.proTxHash == msg.proTxHash) {
            member = &m;
            break;
        }
    }
    if (!member)
        return false;

    if (session.phase0_commits.count(msg.proTxHash))
        return false;

    // VerifyInsecure is deliberate: Phase 0 messages are verified one-at-a-time
    // on receipt against a single known public key from the on-chain DGM record.
    // There is no aggregation of Phase 0 signatures in the protocol, so the
    // rogue-key/aggregation attack surface that VerifySecure defends against
    // does not apply.  LLMQ uses the same pattern for individual message
    // verification (quorums_dkgsessionhandler.cpp:422).
    if (!msg.sig.VerifyInsecure(member->pubKeyOperator, msg.GetSignHash()))
        return false;

    session.phase0_commits[msg.proTxHash] = msg.commitment;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_IsPhase0Complete / PTX_DKG_ClosePhase0
// ---------------------------------------------------------------------------

bool PTX_DKG_IsPhase0Complete(const PTXDKGSession& session)
{
    return session.phase0_commits.size() == session.members.size();
}

bool PTX_DKG_ClosePhase0(PTXDKGSession& session)
{
    const int t = 6; // KDD-048

    // Threshold floor: refuse to advance if fewer than t members committed.
    // A ceremony with |QUAL| < t cannot produce a valid threshold keypair.
    // Fail loud here rather than silently advancing to the expensive reveal
    // and complaint rounds with a session that cannot complete.
    if ((int)session.phase0_commits.size() < t) {
        session.phase = PTXDKGPhase::ABORTED;
        return false;
    }

    // Lock QUAL before any vvec is broadcast — QUAL-locks-before-reveal (KDD-051).
    // From this point no member can enter or leave QUAL by observing another
    // member's revealed g^{a_0}: reveal happens only after this call returns.
    session.qual.clear();
    for (const auto& kv : session.phase0_commits)
        session.qual.insert(kv.first);
    session.phase = PTXDKGPhase::CONTRIB;
    return true;
}

// ---------------------------------------------------------------------------
// PTXDKGPhase1Msg::GetSignHash
//
// Covers quorum_hash || proTxHash || compressed-vvec-bytes ||
//         ephemeralPubKey(48B) || ivSeed(32B) || len-prefixed blob bytes.
//
// The vvec is serialised with blst_p1_affine_compress — the same primitive used
// in ComputeVvecCommitment above.  Both notions of "the vvec" in one message
// (the commitment recompute in ReceivePhase1Msg and the sign-hash here) therefore
// produce identical bytes; an inconsistency would be a silent mismatched encoding.
//
// The blob bytes are length-prefixed so that empty (non-QUAL) slots and 32-byte
// (QUAL) slots produce distinct, unambiguous encodings.
// ---------------------------------------------------------------------------

uint256 PTXDKGPhase1Msg::GetSignHash() const
{
    uint256 result;
    CSHA256 h;
    h.Write(quorum_hash.begin(), quorum_hash.size());
    h.Write(proTxHash.begin(), proTxHash.size());
    for (const auto& pt : vvec) {
        uint8_t buf[48];
        blst_p1_affine_compress(buf, &pt);
        h.Write(buf, 48);
    }
    std::vector<uint8_t> epk = encrypted_shares.ephemeralPubKey.ToByteVector();
    h.Write(epk.data(), epk.size()); // BLS_CURVE_PUBKEY_SIZE = 48 bytes
    h.Write(encrypted_shares.ivSeed.begin(), encrypted_shares.ivSeed.size());
    for (const auto& blob : encrypted_shares.blobs) {
        uint32_t sz = (uint32_t)blob.size();
        h.Write(reinterpret_cast<const uint8_t*>(&sz), sizeof(sz));
        if (!blob.empty())
            h.Write(blob.data(), blob.size());
    }
    h.Finalize(result.begin());
    return result;
}

// ---------------------------------------------------------------------------
// PTX_DKG_BuildPhase1Msg
//
// Index basis: evals[k] = f(members[k].share_index); slot k = position in the
// globally-sorted members[].  All nodes sort identically (KDD-052), so
// Encrypt(k, ...) on the sender equals Decrypt(my_idx, ...) on the receiver.
// Non-QUAL slots are skipped; their blob remains empty (size 0) in the message.
// ---------------------------------------------------------------------------

PTXDKGPhase1Msg PTX_DKG_BuildPhase1Msg(const PTXDKGSession& session,
                                        const CBLSSecretKey& operator_sk)
{
    assert(session.phase == PTXDKGPhase::CONTRIB);
    assert(session.my_idx >= 0);

    const PTXDKGMember& me = session.members[session.my_idx];
    const int n = (int)session.members.size();

    PTXDKGPhase1Msg msg;
    msg.quorum_hash = session.quorum_hash;
    msg.proTxHash   = me.proTxHash;
    msg.vvec        = session.local_contrib.vvec;

    msg.encrypted_shares.InitEncrypt(n);
    for (int k = 0; k < n; k++) {
        if (!session.qual.count(session.members[k].proTxHash))
            continue; // non-QUAL slot: leave blob empty

        // Wire encoding: blst_scalar → big-endian 32 bytes → Blob.
        // 32 = 2 × AES_BLOCKSIZE; no padding needed or applied.
        uint8_t buf[32];
        blst_bendian_from_scalar(buf, &session.local_contrib.evals[k]);
        CBLSIESMultiRecipientBlobs::Blob blob(buf, buf + 32);
        msg.encrypted_shares.Encrypt(k, session.members[k].pubKeyOperator, blob);
    }

    msg.sig = operator_sk.Sign(msg.GetSignHash());
    return msg;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ReceivePhase1Msg
//
// Check order: phase==CONTRIB → quorum_hash → sender in QUAL → no duplicate
//              → sig VerifyInsecure → commitment match.
//
// VerifyInsecure rationale: per-receipt single-key verification, no aggregation
// attack surface.  Matches the Phase 0 pattern (ptx_dkg.cpp ReceivePhase0Msg).
//
// Commitment match is the GJKR redemption check (KDD-051): recomputes
// SHA256(quorum_hash||proTxHash||compressed-vvec-bytes) from msg.vvec and compares
// to phase0_commits[proTxHash].  Uses blst_p1_affine_compress — same primitive as
// ComputeVvecCommitment above — so both hash computations over the same vvec bytes.
// Mismatch = the member revealed a different polynomial than it committed; it is
// inserted into bad_members and the message is rejected.
// ---------------------------------------------------------------------------

bool PTX_DKG_ReceivePhase1Msg(PTXDKGSession& session, const PTXDKGPhase1Msg& msg)
{
    if (session.phase != PTXDKGPhase::CONTRIB)
        return false;
    if (msg.quorum_hash != session.quorum_hash)
        return false;

    const PTXDKGMember* member = nullptr;
    for (const auto& m : session.members) {
        if (m.proTxHash == msg.proTxHash) { member = &m; break; }
    }
    if (!member)
        return false;

    if (!session.qual.count(msg.proTxHash))
        return false;

    // Reject any further messages from a member already in bad_members.
    // Without this guard a member that sent a mismatched vvec (committed →
    // bad_members, not stored) could then send a valid vvec that clears the
    // duplicate check (phase1_vvecs is still empty) and ends up in BOTH
    // bad_members and phase1_vvecs.  bad_members is monotonic once a member
    // is marked bad; it stays bad for the remainder of the ceremony.
    if (session.bad_members.count(msg.proTxHash))
        return false;

    if (session.phase1_vvecs.count(msg.proTxHash))
        return false; // duplicate

    if (!msg.sig.VerifyInsecure(member->pubKeyOperator, msg.GetSignHash()))
        return false;

    // GJKR commitment-match check (KDD-051, load-bearing):
    // recompute SHA256(quorum_hash || proTxHash || compressed-vvec-bytes) from the
    // revealed vvec and match against the Phase 0 stored commitment.
    // ComputeVvecCommitment is the single canonical implementation of this hash;
    // calling it here keeps byte-identity structural rather than maintained-by-eye.
    {
        uint256 recomputed = ComputeVvecCommitment(msg.quorum_hash, msg.proTxHash, msg.vvec);

        auto it = session.phase0_commits.find(msg.proTxHash);
        if (it == session.phase0_commits.end() || recomputed != it->second) {
            session.bad_members.insert(msg.proTxHash);
            return false;
        }
    }

    session.phase1_vvecs[msg.proTxHash]            = msg.vvec;
    session.phase1_encrypted_shares[msg.proTxHash] = msg.encrypted_shares;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_DecryptMyShare
//
// Decodes the 32-byte big-endian blob from the sender's encrypted_shares at
// my_idx and stores the resulting blst_scalar in received_shares.
//
// blob.size()==32 is a structural guard only (ensures we have exactly 32 bytes
// to pass to blst_scalar_from_bendian).  It does NOT establish share validity:
// a wrong-key decrypt returns true from Decrypt() with 32 bytes of AES garbage,
// and the size guard passes.  Share validity requires the Phase 2 Feldman check.
// ---------------------------------------------------------------------------

bool PTX_DKG_DecryptMyShare(PTXDKGSession& session,
                             const uint256& sender_proTxHash,
                             const CBLSSecretKey& operator_sk)
{
    if (session.my_idx < 0)
        return false;

    auto it = session.phase1_encrypted_shares.find(sender_proTxHash);
    if (it == session.phase1_encrypted_shares.end())
        return false;

    CBLSIESMultiRecipientBlobs::Blob blob;
    if (!it->second.Decrypt(session.my_idx, operator_sk, blob))
        return false;

    // Structural/encoding guard: blst_scalar is 32 bytes = 2 × AES_BLOCKSIZE.
    // This guard does NOT establish share validity; see header comment.
    if (blob.size() != 32)
        return false;

    blst_scalar share;
    blst_scalar_from_bendian(&share, blob.data());
    session.received_shares[sender_proTxHash] = share;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_IsPhase1Complete / PTX_DKG_ClosePhase1
// ---------------------------------------------------------------------------

bool PTX_DKG_IsPhase1Complete(const PTXDKGSession& session)
{
    return session.phase1_vvecs.size() == session.qual.size();
}

bool PTX_DKG_ClosePhase1(PTXDKGSession& session)
{
    const int t = 6; // KDD-048

    // Mark QUAL members who failed to reveal as bad.
    for (const auto& ptx : session.qual) {
        if (!session.phase1_vvecs.count(ptx))
            session.bad_members.insert(ptx);
    }

    // Revealed set = QUAL members who are not bad.
    // bad_members at this point contains only Phase 1 failures (commitment-mismatch
    // revealers added by ReceivePhase1Msg, and non-revealers added just above).
    int revealed = (int)session.qual.size() - (int)session.bad_members.size();

    if (revealed < t) {
        session.phase = PTXDKGPhase::ABORTED;
        return false;
    }

    session.phase = PTXDKGPhase::COMPLAINT;
    return true;
}

// ---------------------------------------------------------------------------
// PTXDKGPhase2Msg::GetSignHash
//
// SHA256( quorum_hash[32] || complainant_proTxHash[32] ||
//         dealer_proTxHash[32] || htole32(share_index_j)[4] )
//
// j is written with explicit htole32 (fixed-size scalar — not a length prefix,
// not reinterpret_cast).  Carry-forward D does not apply here: P2/P3 messages
// carry no variable-length fields, so there is no new reinterpret_cast site.
// ---------------------------------------------------------------------------

uint256 PTXDKGPhase2Msg::GetSignHash() const
{
    uint256 result;
    CSHA256 h;
    h.Write(quorum_hash.begin(), quorum_hash.size());
    h.Write(complainant_proTxHash.begin(), complainant_proTxHash.size());
    h.Write(dealer_proTxHash.begin(), dealer_proTxHash.size());
    uint32_t j_le = htole32((uint32_t)share_index_j);
    // htole32 normalizes byte order; the reinterpret_cast reads the 4
    // already-little-endian bytes — NOT a carry-forward-D host-endian site.
    h.Write(reinterpret_cast<const uint8_t*>(&j_le), sizeof(j_le));
    h.Finalize(result.begin());
    return result;
}

// ---------------------------------------------------------------------------
// PTXDKGPhase3Msg::GetSignHash
//
// SHA256( quorum_hash[32] || dealer_proTxHash[32] ||
//         complainant_proTxHash[32] || blst_bendian(revealed_share)[32] )
//
// The revealed scalar is bound in the hash so a tampered scalar fails the
// signature check before the Feldman check runs.  This is the T2-23 invariant:
// a 1-byte flip in revealed_share without re-signing causes sig rejection and
// neither party gets marked bad — the falsifier guard for this binding.
// ---------------------------------------------------------------------------

uint256 PTXDKGPhase3Msg::GetSignHash() const
{
    uint256 result;
    CSHA256 h;
    h.Write(quorum_hash.begin(), quorum_hash.size());
    h.Write(dealer_proTxHash.begin(), dealer_proTxHash.size());
    h.Write(complainant_proTxHash.begin(), complainant_proTxHash.size());
    uint8_t share_be[32];
    blst_bendian_from_scalar(share_be, &revealed_share);
    h.Write(share_be, 32);
    h.Finalize(result.begin());
    return result;
}

// ---------------------------------------------------------------------------
// PTX_DKG_FeldmanCheck
//
// Verify that share = f_D(j) against D's committed vvec.
// LHS: g^{share}  (same generator as vvec generation, ptx_dkg.cpp:201-207).
// RHS: Π_{k=0}^{t-1} vvec[k]^{j^k}  (naive per-term accumulate; t=6).
//
// j must be session.members[session.my_idx].share_index (1-indexed per KDD-052),
// NOT my_idx (0-indexed slot).  Passing my_idx instead of share_index is the
// index-basis trap explicitly guarded by T2-index.
//
// KDD-054 boundary: pure blst — no src/bls/, no chiabls.
// ---------------------------------------------------------------------------

bool PTX_DKG_FeldmanCheck(const blst_scalar& share,
                           const std::vector<blst_p1_affine>& vvec,
                           int j)
{
    const int t = (int)vvec.size();

    // LHS: g^{share}
    blst_p1 lhs;
    blst_sk_to_pk_in_g1(&lhs, &share); // generator = BLS12-381 G1 (implicit)

    // RHS: Π vvec[k]^{j^k}
    blst_fr j_fr;
    {
        uint64_t v[4] = {(uint64_t)j, 0, 0, 0};
        blst_fr_from_uint64(&j_fr, v);
    }

    blst_p1 acc;
    bool acc_set = false;

    blst_fr jpow;
    {
        const uint64_t one[4] = {1, 0, 0, 0};
        blst_fr_from_uint64(&jpow, one); // j^0 = 1
    }

    for (int k = 0; k < t; k++) {
        blst_scalar jpow_s;
        blst_scalar_from_fr(&jpow_s, &jpow);
        uint8_t jpow_le[32];
        blst_lendian_from_scalar(jpow_le, &jpow_s); // blst_p1_mult expects LE
        blst_p1 base;
        blst_p1_from_affine(&base, &vvec[k]);
        blst_p1 term;
        blst_p1_mult(&term, &base, jpow_le, 256);
        if (!acc_set) {
            acc = term;
            acc_set = true;
        } else {
            blst_p1_add_or_double(&acc, &acc, &term);
        }
        blst_fr_mul(&jpow, &jpow, &j_fr); // j^k → j^{k+1}
    }

    // Compare in affine coordinates
    blst_p1_affine lhs_affine, rhs_affine;
    blst_p1_to_affine(&lhs_affine, &lhs);
    blst_p1_to_affine(&rhs_affine, &acc);
    return blst_p1_affine_is_equal(&lhs_affine, &rhs_affine);
}

// ---------------------------------------------------------------------------
// PTX_DKG_BuildPhase2Msg
// ---------------------------------------------------------------------------

PTXDKGPhase2Msg PTX_DKG_BuildPhase2Msg(const PTXDKGSession& session,
                                        const uint256& dealer_proTxHash,
                                        const CBLSSecretKey& operator_sk)
{
    assert(session.phase == PTXDKGPhase::COMPLAINT);
    assert(session.my_idx >= 0);

    const PTXDKGMember& me = session.members[session.my_idx];

    PTXDKGPhase2Msg msg;
    msg.quorum_hash           = session.quorum_hash;
    msg.complainant_proTxHash = me.proTxHash;
    msg.dealer_proTxHash      = dealer_proTxHash;
    msg.share_index_j         = me.share_index;
    msg.sig                   = operator_sk.Sign(msg.GetSignHash());
    return msg;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ReceivePhase2Msg
//
// Check order (11 steps, report §4):
//  1. phase == COMPLAINT
//  2. quorum_hash match
//  3. complainant found in session.members
//  4. dealer found in session.members
//  5. complainant in qual
//  6. complainant not in bad_members (FIX-1: already-bad cannot file)
//  7. dealer in qual
//  8. dealer not in bad_members (moot complaint against already-bad dealer)
//  9. no duplicate: complainant not already in complaints_against[dealer]
// 10. sig VerifyInsecure(complainant.pubKeyOperator, GetSignHash())
// 11. share_index_j == session.members[complainant_slot].share_index (reject forged j)
// ---------------------------------------------------------------------------

bool PTX_DKG_ReceivePhase2Msg(PTXDKGSession& session, const PTXDKGPhase2Msg& msg)
{
    // 1
    if (session.phase != PTXDKGPhase::COMPLAINT)
        return false;
    // 2
    if (msg.quorum_hash != session.quorum_hash)
        return false;

    // 3 — find complainant
    const PTXDKGMember* complainant = nullptr;
    for (const auto& m : session.members) {
        if (m.proTxHash == msg.complainant_proTxHash) { complainant = &m; break; }
    }
    if (!complainant)
        return false;

    // 4 — find dealer
    const PTXDKGMember* dealer = nullptr;
    for (const auto& m : session.members) {
        if (m.proTxHash == msg.dealer_proTxHash) { dealer = &m; break; }
    }
    if (!dealer)
        return false;

    // 5
    if (!session.qual.count(msg.complainant_proTxHash))
        return false;
    // 6
    if (session.bad_members.count(msg.complainant_proTxHash))
        return false;
    // 7
    if (!session.qual.count(msg.dealer_proTxHash))
        return false;
    // 8
    if (session.bad_members.count(msg.dealer_proTxHash))
        return false;
    // 9
    {
        auto it = session.complaints_against.find(msg.dealer_proTxHash);
        if (it != session.complaints_against.end() &&
            it->second.count(msg.complainant_proTxHash))
            return false;
    }
    // 10
    if (!msg.sig.VerifyInsecure(complainant->pubKeyOperator, msg.GetSignHash()))
        return false;
    // 11
    if (msg.share_index_j != complainant->share_index)
        return false;

    session.complaints_against[msg.dealer_proTxHash].insert(msg.complainant_proTxHash);
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ClosePhase2
// Advances COMPLAINT → JUSTIFY.  No threshold check; P3 markings haven't
// happened and a premature abort would be wrong.  Threshold is at ClosePhase3.
// ---------------------------------------------------------------------------

bool PTX_DKG_ClosePhase2(PTXDKGSession& session)
{
    if (session.phase != PTXDKGPhase::COMPLAINT)
        return false;
    session.phase = PTXDKGPhase::JUSTIFY;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_BuildPhase3Msg
//
// Correction B: complainant_slot is derived as the index of
// complainant_proTxHash within session.members[] — the same sorted array and
// derivation that the receive path uses for j.
// revealed_share = session.local_contrib.evals[complainant_slot], slot-indexed
// (evals[k] = f(members[k].share_index), confirmed P1).
// ---------------------------------------------------------------------------

PTXDKGPhase3Msg PTX_DKG_BuildPhase3Msg(const PTXDKGSession& session,
                                        const uint256& complainant_proTxHash,
                                        const CBLSSecretKey& operator_sk)
{
    assert(session.phase == PTXDKGPhase::JUSTIFY);
    assert(session.my_idx >= 0);

    const PTXDKGMember& me = session.members[session.my_idx];

    // Derive complainant_slot from members[] — not a passed-in index.
    int complainant_slot = -1;
    for (int i = 0; i < (int)session.members.size(); i++) {
        if (session.members[i].proTxHash == complainant_proTxHash) {
            complainant_slot = i;
            break;
        }
    }
    assert(complainant_slot >= 0);

    PTXDKGPhase3Msg msg;
    msg.quorum_hash           = session.quorum_hash;
    msg.dealer_proTxHash      = me.proTxHash;
    msg.complainant_proTxHash = complainant_proTxHash;
    msg.revealed_share        = session.local_contrib.evals[complainant_slot];

    msg.sig = operator_sk.Sign(msg.GetSignHash());
    return msg;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ReceivePhase3Msg
//
// Check order (report §4), then Feldman, then §15 branch mark:
//  1.  phase == JUSTIFY
//  2.  quorum_hash match
//  3.  dealer in session.members
//  4.  complainant in session.members
//  5.  dealer in qual, dealer not in bad_members (already-bad cannot justify)
//  6.  complainant in qual
//  7.  complaint outstanding: complainant in complaints_against[dealer]
//  8.  no duplicate: complainant not in justified_for[dealer]
//  9.  sig VerifyInsecure(dealer.pubKeyOperator, GetSignHash())
// 10.  revealed_share is a valid field element (blst_scalar_fr_check)
// 11.  derive j = session.members[complainant_slot].share_index (from session, NOT wire)
// 12.  PTX_DKG_FeldmanCheck:
//        passes → §15 Branch 2: complainant → bad_members; justified_for[D] updated
//        fails  → §15 Branch 3a: dealer → bad_members;    justified_for[D] updated
// ---------------------------------------------------------------------------

bool PTX_DKG_ReceivePhase3Msg(PTXDKGSession& session, const PTXDKGPhase3Msg& msg)
{
    // 1
    if (session.phase != PTXDKGPhase::JUSTIFY)
        return false;
    // 2
    if (msg.quorum_hash != session.quorum_hash)
        return false;

    // 3 — find dealer
    const PTXDKGMember* dealer = nullptr;
    for (const auto& m : session.members) {
        if (m.proTxHash == msg.dealer_proTxHash) { dealer = &m; break; }
    }
    if (!dealer)
        return false;

    // 4 — find complainant and record its slot (needed for j derivation at step 11)
    const PTXDKGMember* complainant = nullptr;
    int complainant_slot = -1;
    for (int i = 0; i < (int)session.members.size(); i++) {
        if (session.members[i].proTxHash == msg.complainant_proTxHash) {
            complainant = &session.members[i];
            complainant_slot = i;
            break;
        }
    }
    if (!complainant)
        return false;

    // 5
    if (!session.qual.count(msg.dealer_proTxHash))
        return false;
    if (session.bad_members.count(msg.dealer_proTxHash))
        return false;
    // 6
    if (!session.qual.count(msg.complainant_proTxHash))
        return false;
    // 7
    {
        auto it = session.complaints_against.find(msg.dealer_proTxHash);
        if (it == session.complaints_against.end() ||
            !it->second.count(msg.complainant_proTxHash))
            return false;
    }
    // 8
    {
        auto it = session.justified_for.find(msg.dealer_proTxHash);
        if (it != session.justified_for.end() &&
            it->second.count(msg.complainant_proTxHash))
            return false;
    }
    // 9
    if (!msg.sig.VerifyInsecure(dealer->pubKeyOperator, msg.GetSignHash()))
        return false;
    // 10
    if (!blst_scalar_fr_check(&msg.revealed_share))
        return false;
    // 11 — j derived from session state, NOT from wire
    int j = session.members[complainant_slot].share_index;

    // 12 — Feldman check → §15 branch dispatch
    auto vvec_it = session.phase1_vvecs.find(msg.dealer_proTxHash);
    // Soft reject: vvec must be present if dealer is in qual and not bad (check 5
    // passed), but an assert on a message-handling path is a DoS surface if the
    // reasoning is ever wrong.  Return false to safely reject.
    if (vvec_it == session.phase1_vvecs.end())
        return false;

    if (PTX_DKG_FeldmanCheck(msg.revealed_share, vvec_it->second, j)) {
        // §15 Branch 2: justify passes → complainant was wrong, complainant bad
        session.bad_members.insert(msg.complainant_proTxHash);
    } else {
        // §15 Branch 3a: justify fails → dealer's share invalid, dealer bad
        session.bad_members.insert(msg.dealer_proTxHash);
    }
    session.justified_for[msg.dealer_proTxHash].insert(msg.complainant_proTxHash);
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ClosePhase3
//
// Branch 3b sweep: for each D in qual not already bad, any unresolved complaint
// (complaints_against[D] − justified_for[D] non-empty) marks D bad.
// Then threshold check: effective = |qual| − |bad_members|.
// < t=6 → ABORTED; ≥ t → PREMIT.
// ---------------------------------------------------------------------------

bool PTX_DKG_ClosePhase3(PTXDKGSession& session)
{
    const int t = 6; // KDD-048

    if (session.phase != PTXDKGPhase::JUSTIFY)
        return false;

    // Branch 3b sweep
    for (const auto& d_ptx : session.qual) {
        if (session.bad_members.count(d_ptx))
            continue;

        auto cit = session.complaints_against.find(d_ptx);
        if (cit == session.complaints_against.end())
            continue; // no complaints filed against this dealer

        const std::set<uint256>& complained = cit->second;

        // justified_for[d] may be absent if no justifications were received
        const std::set<uint256>* justified = nullptr;
        auto jit = session.justified_for.find(d_ptx);
        if (jit != session.justified_for.end())
            justified = &jit->second;

        // Check whether any complaint is unresolved
        bool has_unresolved = false;
        for (const auto& c_ptx : complained) {
            if (!justified || !justified->count(c_ptx)) {
                has_unresolved = true;
                break;
            }
        }
        if (has_unresolved)
            session.bad_members.insert(d_ptx);
    }

    int effective = (int)session.qual.size() - (int)session.bad_members.size();
    if (effective < t) {
        session.phase = PTXDKGPhase::ABORTED;
        return false;
    }

    session.phase = PTXDKGPhase::PREMIT;
    return true;
}

// ---------------------------------------------------------------------------
// PTXDKGPhase4Msg::GetSignHash — KDD-072 P-b2 (§3 fix)
// predecessor zero:     SHA256( quorum_hash[32] || proTxHash[32] ||
//                               group_pk_bytes[48] || vvec_hash[32] )   — 144 B,
//                       byte-identical to the pre-P-b2 preimage (golden-guarded
//                       by Pb2_FreshSignHash_Golden).
// predecessor non-zero: the above || predecessor_quorum_hash[32]        — 176 B.
// Append-iff-non-zero: the fresh path takes no layout change; distinct lengths
// give clean domain separation. See the header contract for the four callers
// and their views.
// ---------------------------------------------------------------------------

uint256 PTXDKGPhase4Msg::GetSignHash(const uint256& predecessor) const
{
    uint256 result;
    CSHA256 h;
    h.Write(quorum_hash.begin(), quorum_hash.size());
    h.Write(proTxHash.begin(), proTxHash.size());
    h.Write(group_pk_bytes, 48);
    h.Write(vvec_hash.begin(), vvec_hash.size());
    if (!predecessor.IsNull()) {
        h.Write(predecessor.begin(), predecessor.size());
    }
    h.Finalize(result.begin());
    return result;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ComputeSkShare — §5.1
//
// Completeness invariant (C6): a missing effective-QUAL entry in received_shares
// means DecryptMyShare was not called — a local session error.  ABORT (return false).
// Silently skipping the dealer produces a wrong sk_share_i with no failure signal;
// fail loud so the session can be repaired.
// ---------------------------------------------------------------------------

bool PTX_DKG_ComputeSkShare(PTXDKGSession& session)
{
    if (session.phase != PTXDKGPhase::PREMIT || session.my_idx < 0)
        return false;

    // Completeness check: abort if any effective-QUAL dealer has no received share.
    for (const auto& ptx : session.qual) {
        if (session.bad_members.count(ptx))
            continue;
        if (!session.received_shares.count(ptx))
            return false;
    }

    blst_fr acc;
    { const uint64_t zero[4] = {0, 0, 0, 0}; blst_fr_from_uint64(&acc, zero); }

    for (const auto& ptx : session.qual) {
        if (session.bad_members.count(ptx))
            continue;
        blst_fr share_fr;
        blst_fr_from_scalar(&share_fr, &session.received_shares.at(ptx));
        blst_fr_add(&acc, &acc, &share_fr);
    }

    blst_scalar_from_fr(&session.sk_share_i, &acc);
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ComputeGroupPk — §5.2
//
// Own-vvec[0] inclusion invariant (GF2): phase1_vvecs does NOT contain this
// node's own entry.  Source own vvec[0] from local_contrib.vvec[0] explicitly.
// Iterating only phase1_vvecs silently misses this node's contribution.
// ---------------------------------------------------------------------------

bool PTX_DKG_ComputeGroupPk(PTXDKGSession& session)
{
    if (session.phase != PTXDKGPhase::PREMIT || session.my_idx < 0)
        return false;

    const uint256& my_proTxHash = session.members[session.my_idx].proTxHash;

    blst_p1 acc;
    bool acc_set = false;

    for (const auto& ptx : session.qual) {
        if (session.bad_members.count(ptx))
            continue;

        const blst_p1_affine* vvec0 = nullptr;
        blst_p1_affine own_vvec0_copy;
        if (ptx == my_proTxHash) {
            // Own vvec[0] — never in phase1_vvecs (own Phase 1 msg not processed there).
            own_vvec0_copy = session.local_contrib.vvec[0];
            vvec0 = &own_vvec0_copy;
        } else {
            vvec0 = &session.phase1_vvecs.at(ptx)[0];
        }

        if (!acc_set) {
            blst_p1_from_affine(&acc, vvec0);
            acc_set = true;
        } else {
            blst_p1_add_or_double_affine(&acc, &acc, vvec0);
        }
    }

    if (!acc_set)
        return false; // empty effective-QUAL — should not reach post-ClosePhase3

    blst_p1_to_affine(&session.group_pk, &acc);
    session.phase4_computed = true;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_BuildPhase4Msg
// ---------------------------------------------------------------------------

PTXDKGPhase4Msg PTX_DKG_BuildPhase4Msg(const PTXDKGSession& session,
                                        const CBLSSecretKey& operator_sk)
{
    assert(session.phase == PTXDKGPhase::PREMIT);
    assert(session.my_idx >= 0);
    assert(session.phase4_computed);

    const uint256& my_proTxHash = session.members[session.my_idx].proTxHash;

    PTXDKGPhase4Msg msg;
    msg.quorum_hash = session.quorum_hash;
    msg.proTxHash   = my_proTxHash;

    // Compress this node's group_pk.
    blst_p1_affine_compress(msg.group_pk_bytes, &session.group_pk);

    // vvec_hash: SHA256 over effective-QUAL vvec[0] compressed bytes.
    // qual is std::set<uint256> — iterates in ascending order for determinism.
    CSHA256 vh;
    for (const auto& ptx : session.qual) {
        if (session.bad_members.count(ptx))
            continue;
        uint8_t buf[48];
        if (ptx == my_proTxHash) {
            blst_p1_affine_compress(buf, &session.local_contrib.vvec[0]);
        } else {
            blst_p1_affine_compress(buf, &session.phase1_vvecs.at(ptx)[0]);
        }
        vh.Write(buf, 48);
    }
    vh.Finalize(msg.vvec_hash.begin());

    // KDD-072 P-b2: sign over the SESSION's predecessor view (zero = fresh =
    // pre-P-b2 preimage; non-zero = this member attests "rotation of X").
    msg.sig = operator_sk.Sign(msg.GetSignHash(session.predecessor_quorum_hash));
    return msg;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ReceivePhase4Msg
//
// Check order (§4):
//  1. phase == PREMIT
//  2. quorum_hash match
//  3. sender found in session.members
//  4. sender in qual
//  5. sender not in bad_members
//  6. no duplicate (not already in phase4_premit_msgs)
//  7. sig VerifyInsecure(sender.pubKeyOperator, GetSignHash())
//  8. group_pk_bytes decompresses (blst_p1_uncompress == BLST_SUCCESS)
// ---------------------------------------------------------------------------

bool PTX_DKG_ReceivePhase4Msg(PTXDKGSession& session, const PTXDKGPhase4Msg& msg)
{
    // 1
    if (session.phase != PTXDKGPhase::PREMIT)
        return false;
    // 2
    if (msg.quorum_hash != session.quorum_hash)
        return false;
    // 3 — find sender
    const PTXDKGMember* sender = nullptr;
    for (const auto& m : session.members)
        if (m.proTxHash == msg.proTxHash) { sender = &m; break; }
    if (!sender)
        return false;
    // 4
    if (!session.qual.count(msg.proTxHash))
        return false;
    // 5
    if (session.bad_members.count(msg.proTxHash))
        return false;
    // 6
    if (session.phase4_premit_msgs.count(msg.proTxHash))
        return false;
    // 7 — KDD-072 P-b2: verified against THIS session's predecessor view. A
    // peer signing over a different rotation view (or none) fails here — the
    // safe direction: divergent views cannot co-premit.
    if (!msg.sig.VerifyInsecure(sender->pubKeyOperator, msg.GetSignHash(session.predecessor_quorum_hash)))
        return false;
    // 8
    blst_p1_affine tmp;
    if (blst_p1_uncompress(&tmp, msg.group_pk_bytes) != BLST_SUCCESS)
        return false;

    session.phase4_premit_msgs[msg.proTxHash] = msg;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_IsPhase4Complete
// ---------------------------------------------------------------------------

bool PTX_DKG_IsPhase4Complete(const PTXDKGSession& session)
{
    const int t = 6;
    if (!session.phase4_computed)
        return false;

    uint8_t my_bytes[48];
    blst_p1_affine_compress(my_bytes, &session.group_pk);

    int consistent = 0;
    for (const auto& kv : session.phase4_premit_msgs) {
        if (std::memcmp(kv.second.group_pk_bytes, my_bytes, 48) == 0)
            consistent++;
    }
    return consistent >= t;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ClosePhase4
//
// Count accepted Phase 4 msgs whose group_pk_bytes are bytewise equal to this
// node's computed group_pk.  Honest nodes compute identical group_pk given the
// same effective-QUAL and vvec data.
// < t consistent → ABORTED.  ≥ t consistent → FINALIZE.
// ---------------------------------------------------------------------------

bool PTX_DKG_ClosePhase4(PTXDKGSession& session)
{
    const int t = 6;
    if (session.phase != PTXDKGPhase::PREMIT)
        return false;
    if (!session.phase4_computed)
        return false;

    uint8_t my_bytes[48];
    blst_p1_affine_compress(my_bytes, &session.group_pk);

    int consistent = 0;
    for (const auto& kv : session.phase4_premit_msgs) {
        if (std::memcmp(kv.second.group_pk_bytes, my_bytes, 48) == 0)
            consistent++;
    }

    if (consistent < t) {
        session.phase = PTXDKGPhase::ABORTED;
        return false;
    }

    session.phase = PTXDKGPhase::FINALIZE;
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_StoreSkShare — Phase 5 (Option A, KDD-057)
//
// Write site to the keyed share store g_ptx_my_shares (DKG-produced share, KDD-070
// P1).  Routes through the §C1 guarded setter PTX_BLS_SetSkShare (KDD-057):
// per-key refuse-unless-empty — a set
// slot is NOT overwritten (returns false; ClosePhase5 maps that to ABORTED).
// Safe at W1.3: fires only at phase==FINALIZE (local completion), and no W1.3
// path re-forms into a set slot.
// ---------------------------------------------------------------------------

bool PTX_DKG_StoreSkShare(const PTXDKGSession& session, int formation_height,
                          PTXShareRole role)
{
    if (session.phase != PTXDKGPhase::FINALIZE)
        return false;

    uint8_t sk_bytes[32];
    blst_bendian_from_scalar(sk_bytes, &session.sk_share_i);

    // KDD-070 P1/P3: keyed by the ceremony's quorum_hash. §C1 refuse-unless-empty
    // is per-key; role is CURRENT for a fresh formation, PENDING for a rotation
    // successor (store-pending at FINALIZE, promoted at the successor's connect).
    std::string set_err;
    if (!PTX_BLS_SetSkShare(session.quorum_hash, formation_height, sk_bytes, role, set_err)) {
        LogPrintf("PTX DKG: StoreSkShare: refusing sk_share store for quorum_hash=%s (%s)\n",
                  session.quorum_hash.ToString(), set_err);
        return false;
    }
    // KDD-070 P2: persist to evoDb's RAW layer (store-pending at FINALIZE has no
    // block transaction to ride). On a persist FAILURE we do NOT abort — the
    // share is already usable in memory (signing reads the map, not disk); a
    // local disk fault must not fail a global formation below t (KDD-070 (b)).
    // We degrade to the pre-P2 memory-only baseline and warn LOUDLY: the share
    // will not survive restart (ODC-035), surfaced on next start by the
    // "in_qual but no share" warning. The map store above already succeeded.
    if (evoDb != nullptr) {
        HeldShare hs;
        std::memcpy(hs.bytes, sk_bytes, 32);
        hs.formation_height = formation_height;
        hs.role             = role;
        hs.promotion_height = -1;
        if (!PTX_BLS_PersistShare(*evoDb, session.quorum_hash, hs)) {
            PTX_BLS_MarkMemoryOnly(session.quorum_hash);   // trackable degraded state
            LogPrintf("PTX DKG: ERROR: StoreSkShare: FAILED to persist sk_share for quorum %s "
                      "to evoDb — share held in MEMORY ONLY, will NOT survive restart "
                      "(ODC-035). Ceremony continues; this member is degraded.\n",
                      session.quorum_hash.ToString());
        }
    }
    LogPrintf("PTX DKG: StoreSkShare: sk_share written for ceremony quorum_hash=%s\n",
              session.quorum_hash.ToString());
    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_BuildPTXDKGTx — Phase 5
//
// Construct the PTXDKG special transaction.  Does NOT submit (ODC-029 open).
// ---------------------------------------------------------------------------

CMutableTransaction PTX_DKG_BuildPTXDKGTx(const PTXDKGSession& session,
                                            int formation_height)
{
    assert(session.phase == PTXDKGPhase::FINALIZE);
    assert(session.phase4_computed);

    const uint256& my_proTxHash = session.members[session.my_idx].proTxHash;

    PTXDKGPayload payload;
    // KDD-072 P-a/P-b2 — EXPLICIT, not default-reliant. Version and predecessor
    // both derive from the SAME session field the Phase 4 premits were signed
    // over (one source, no skew): fresh -> v1 (byte-identical emission, no
    // predecessor serialized); rotation -> v2 + the attested predecessor.
    // DORMANT in P-b2: nothing feeds a non-zero predecessor until P-b6.
    payload.nVersion         = session.predecessor_quorum_hash.IsNull()
                                   ? PTXDKGPayload::CURRENT_VERSION
                                   : PTXDKGPayload::ROTATION_VERSION;
    payload.predecessor_quorum_hash = session.predecessor_quorum_hash;
    payload.quorum_hash      = session.quorum_hash;
    payload.formation_height = formation_height;

    // group_pk_bytes: compress this node's computed group_pk.
    blst_p1_affine_compress(payload.group_pk_bytes, &session.group_pk);

    // vvec_hash: SHA256 over effective-QUAL vvec[0] compressed bytes.
    // qual is std::set<uint256> — iterates in ascending order for determinism.
    {
        CSHA256 vh;
        for (const auto& ptx : session.qual) {
            if (session.bad_members.count(ptx))
                continue;
            uint8_t buf[48];
            if (ptx == my_proTxHash) {
                blst_p1_affine_compress(buf, &session.local_contrib.vvec[0]);
            } else {
                blst_p1_affine_compress(buf, &session.phase1_vvecs.at(ptx)[0]);
            }
            vh.Write(buf, 48);
        }
        vh.Finalize(payload.vvec_hash.begin());
    }

    // member_node_ids: effective-QUAL in share_index order.
    {
        struct EffMember { int share_index; std::string node_id; };
        std::vector<EffMember> eff;
        for (const auto& ptx : session.qual) {
            if (session.bad_members.count(ptx))
                continue;
            for (const auto& m : session.members) {
                if (m.proTxHash == ptx) {
                    eff.push_back({m.share_index, m.node_id});
                    break;
                }
            }
        }
        std::sort(eff.begin(), eff.end(),
                  [](const EffMember& a, const EffMember& b) {
                      return a.share_index < b.share_index;
                  });
        for (const auto& em : eff)
            payload.member_node_ids.push_back(em.node_id);
    }

    // premit_commitments: all accepted Phase 4 messages.
    payload.premit_commitments = session.phase4_premit_msgs;

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType    = CTransaction::TxType::PTXDKG;
    SetTxPayload(tx, payload);
    return tx;
}

// ---------------------------------------------------------------------------
// PTX_DKG_ClosePhase5
// ---------------------------------------------------------------------------

bool PTX_DKG_ClosePhase5(PTXDKGSession& session,
                           int formation_height,
                           CMutableTransaction& ptxdkg_tx_out)
{
    if (session.phase != PTXDKGPhase::FINALIZE)
        return false;

    // KDD-072 P-b2 / KDD-070 §8: a rotation successor's share is stored PENDING
    // at FINALIZE (promoted at the successor's block-connect, P-b4/P-b6); a
    // fresh formation stores CURRENT as before. Derived from the same session
    // field as the sign-hash and the payload version. DORMANT in P-b2.
    const PTXShareRole role = session.predecessor_quorum_hash.IsNull()
                                  ? PTXShareRole::CURRENT
                                  : PTXShareRole::PENDING;
    if (!PTX_DKG_StoreSkShare(session, formation_height, role)) {
        session.phase = PTXDKGPhase::ABORTED;
        return false;
    }

    ptxdkg_tx_out = PTX_DKG_BuildPTXDKGTx(session, formation_height);

    // KDD-058-A: store + RELAY the finished commitment network-wide (the
    // qfc shape) — every node holds it in the replicated minable store, so
    // ANY staker's assembler can land it (members are near-zero-stake by
    // design; the member-only slot this replaces landed 1-of-3 fleet
    // results, by whale-luck).  RelayInv scope — deliberately NOT the
    // member-mesh ceremony relayHook.  Best-effort posture kept: a refusal
    // is logged, not fatal — the ceremony result and sk_share are already
    // stored.
    {
        CValidationState pendState;
        if (!PTX_DKG_Commitments_AddAndRelay(MakeTransactionRef(ptxdkg_tx_out), pendState)) {
            LogPrintf("PTX DKG: ClosePhase5: commitment store/relay refused (%s)\n",
                      pendState.GetRejectReason());
        }
    }

    session.phase = PTXDKGPhase::DONE;
    return true;
}
