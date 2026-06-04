// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_dkg.h"
#include "ptx/ptx_bls.h"

#include "arith_uint256.h"
#include "crypto/sha256.h"
#include "random.h"

#include <algorithm>
#include <cassert>

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
// PTX_GetBLSState
// All ceremony code calls this instead of g_ptx_bls_state directly, so the
// W2.1 per-quorum registry refactor changes one function, not every call site.
// ---------------------------------------------------------------------------

PTXBLSState& PTX_GetBLSState()
{
    return g_ptx_bls_state;
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
// PTX_DKG_ComputeMemberScore
// Single SHA256(confirmedHashWithProRegTxHash || formation_block_hash).
// Matches deterministicgms.cpp CalculateScores exactly (single SHA256, raw
// byte writes, not double-hashing).
//
// KDD-052 describes the formula as SHA256(SHA256(proTxHash, confirmedHash), fbh)
// which is consistent: confirmedHashWithProRegTxHash = SHA256(proTxHash||confirmedHash),
// so this is SHA256(that_result || fbh) — two sequential single-SHA256 steps,
// not SHA256(SHA256(x)) over the same input.  Wording clarification deferred
// to W1.2-integration per PTX_LE_STANDUP.md note.
// ---------------------------------------------------------------------------

uint256 PTX_DKG_ComputeMemberScore(const uint256& confirmedHashWithProRegTxHash,
                                   const uint256& formation_block_hash)
{
    uint256 score;
    CSHA256 h;
    h.Write(confirmedHashWithProRegTxHash.begin(), confirmedHashWithProRegTxHash.size());
    h.Write(formation_block_hash.begin(), formation_block_hash.size());
    h.Finalize(score.begin());
    return score;
}

// ---------------------------------------------------------------------------
// PTX_DKG_SortMembers
// ---------------------------------------------------------------------------

bool PTX_DKG_SortMembers(std::vector<PTXDKGMember>& members,
                          const uint256& formation_block_hash)
{
    for (const auto& m : members) {
        if (m.confirmedHash.IsNull())
            return false;
    }

    std::stable_sort(members.begin(), members.end(),
        [&](const PTXDKGMember& a, const PTXDKGMember& b) {
            arith_uint256 sa = UintToArith256(PTX_DKG_ComputeMemberScore(
                a.confirmedHashWithProRegTxHash, formation_block_hash));
            arith_uint256 sb = UintToArith256(PTX_DKG_ComputeMemberScore(
                b.confirmedHashWithProRegTxHash, formation_block_hash));
            return sa > sb; // descending: highest score → share_index 1
        });

    for (int i = 0; i < (int)members.size(); i++)
        members[i].share_index = i + 1;

    return true;
}

// ---------------------------------------------------------------------------
// PTX_DKG_InitSession
// ---------------------------------------------------------------------------

bool PTX_DKG_InitSession(PTXDKGSession& session,
                          std::vector<PTXDKGMember> members,
                          const uint256& formation_block_hash,
                          const uint256& my_proTxHash)
{
    if (!PTX_DKG_SortMembers(members, formation_block_hash))
        return false;

    if ((int)members.size() != 11)
        return false;

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
