// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.3 Package 2 — CheckPTXDKGTx contextual validator unit tests.
// Covers V5 (PTX_DKG_SelectQuorumFromList) and V6–V8 (PTX_DKG_VerifyPremits) on
// hand-built GM lists / quorum vectors with real BLS-signed premits.
//
// ACCOUNTABILITY, not correctness (KDD-059): these tests assert that the
// validator confirms >= t quorum members signed AGREEMENT on (group_pk,
// vvec_hash) with their operator key — never that group_pk is a correct DKG
// output.  Test names use attestation/agreement language deliberately.
//
// V1–V4 (LookupBlockIndex / height / GetAncestor reorg / GetListForBlock) are
// NOT unit-coverable here — they need a real CBlockIndex + populated DGM
// snapshot, and the TestChainSetup chain-fixture layer does not run in test_ptx.
// Their falsification is bound to Package 3 per ODC-031 (out of C-2 scope).
//
// Each negative test asserts the EXACT reject reason, so a check implemented with
// the wrong comparator/code is caught. The load-bearing manual stub->RED cycle
// (break the comparator in source, rebuild, observe RED) is run separately.

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"
#include "evo/deterministicgms.h"

#include "bls/bls_wrapper.h"
#include "consensus/validation.h"
#include "key.h"
#include "primitives/transaction.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

#include <cstring>
#include <map>
#include <string>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_validation_tests, BasicTestingSetup)

namespace {

// Fixed test constants (group_pk_bytes is only memcmp'd / fed to the sign-hash in
// V6–V8; it does not need to decompress — that is a structural check upstream).
static const uint8_t kGroupPk[48] = {0x07};   // {0x07, 0, 0, ...}
static const uint8_t kGroupPk2[48] = {0x08};  // a different 48-byte value

static uint256 Bytes32(unsigned char b0, unsigned char b1)
{
    std::vector<unsigned char> v(32, 0);
    v[0] = b0; v[1] = b1;
    return uint256(v);
}

static uint256 QuorumHash() { return Bytes32(0xFB, 0x01); }
static uint256 VvecHash()   { return Bytes32(0x5E, 0xCC); }

static CBLSSecretKey NewSk() { CBLSSecretKey sk; sk.MakeNewKey(); return sk; }

// Build a CDeterministicGM with a distinct identity and (optionally) an operator
// key set.  node_id non-empty => PTX-eligible.
static std::shared_ptr<CDeterministicGM> MakeGM(uint64_t id,
                                                const std::string& node_id,
                                                const CBLSSecretKey& opSk,
                                                bool setOpKey = true)
{
    std::vector<unsigned char> pb(32, 0);
    pb[0] = (unsigned char)id; pb[1] = 0xA1;
    uint256 proTx(pb);

    auto dgm = std::make_shared<CDeterministicGM>(id);
    dgm->proTxHash          = proTx;
    dgm->collateralOutpoint = COutPoint(proTx, 0);

    auto st = std::make_shared<CDeterministicGMState>();
    std::vector<unsigned char> cb(32, 0x11);
    cb[0] = (unsigned char)id; // vary confirmedHash so scores differ
    st->UpdateConfirmedHash(proTx, uint256(cb));
    if (setOpKey) st->pubKeyOperator.Set(opSk.GetPublicKey());
    uint160 k20; memcpy(k20.begin(), proTx.begin(), 20);
    st->keyIDOwner  = CKeyID(k20);
    st->keyIDVoting = st->keyIDOwner;
    st->node_id     = node_id;
    dgm->pdgmState = st;
    return dgm;
}

struct Quorum {
    std::vector<CDeterministicGMCPtr> gms;        // the quorum vector (V6 input)
    std::map<uint256, CBLSSecretKey> sks;         // proTxHash -> operator sk
};

static Quorum MakeQuorum(int n)
{
    Quorum q;
    for (int i = 0; i < n; i++) {
        CBLSSecretKey sk = NewSk();
        auto dgm = MakeGM((uint64_t)i, "gm" + std::to_string(i) + ":8080", sk);
        q.gms.push_back(dgm);
        q.sks[dgm->proTxHash] = sk;
    }
    return q;
}

// A premit signed by signSk over GetSignHash(predecessor), with the given inner
// proTxHash and agreement fields. Default zero predecessor = the fresh (v1)
// preimage every existing row expects (KDD-072 P-b2 — test-helper default only;
// the production method has no default by design).
static PTXDKGPhase4Msg MakePremit(const uint256& inner, const uint256& qh,
                                  const uint8_t gpk[48], const uint256& vvec,
                                  const CBLSSecretKey& signSk,
                                  const uint256& predecessor = uint256())
{
    PTXDKGPhase4Msg p;
    p.quorum_hash = qh;
    p.proTxHash   = inner;
    memcpy(p.group_pk_bytes, gpk, 48);
    p.vvec_hash   = vvec;
    p.sig         = signSk.Sign(p.GetSignHash(predecessor));
    return p;
}

// A payload with nPremits valid self-signed premits from the first nPremits quorum
// members, all agreeing on (group_pk, vvec_hash).
static PTXDKGPayload MakePayload(const Quorum& q, int nPremits)
{
    PTXDKGPayload pl;
    pl.quorum_hash = QuorumHash();
    memcpy(pl.group_pk_bytes, kGroupPk, 48);
    pl.vvec_hash        = VvecHash();
    pl.formation_height = 100;
    for (const auto& g : q.gms) pl.member_node_ids.push_back(g->pdgmState->node_id);
    for (int i = 0; i < nPremits; i++) {
        const auto& g = q.gms[i];
        pl.premit_commitments[g->proTxHash] =
            MakePremit(g->proTxHash, QuorumHash(), kGroupPk, VvecHash(), q.sks.at(g->proTxHash));
    }
    return pl;
}

} // namespace

// ===========================================================================
// V6–V8 — PTX_DKG_VerifyPremits
// ===========================================================================

// Positive: a full quorum with t=6 valid, agreeing, self-signed premits passes.
BOOST_AUTO_TEST_CASE(Premits_AcceptValidAttestations)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl = MakePayload(q, 6);
    CValidationState state;
    BOOST_CHECK(PTX_DKG_VerifyPremits(q.gms, pl, state));
}

// V7a (key<->inner binding) — ISOLATING falsification.
// A premit signed by member K's key but carrying inner proTxHash = P (!= K),
// inserted under map key K. Only V7a rejects it: V7g passes because K genuinely
// signed the (inner=P) hash. Stub V7a->true => this accepts (the stub->RED case).
BOOST_AUTO_TEST_CASE(V7a_KeyInnerBinding_RejectsMislabeledInner)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl = MakePayload(q, 6);

    const uint256 K = q.gms[0]->proTxHash;
    const uint256 P = q.gms[1]->proTxHash; // a different quorum member
    // premit: inner=P, but signed by K's operator key; agreement fields correct.
    pl.premit_commitments[K] =
        MakePremit(/*inner=*/P, QuorumHash(), kGroupPk, VvecHash(), q.sks.at(K));

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-premit-key-mismatch");
}

// Inflation attempt: one valid premit (from A) duplicated under 6 distinct map
// keys. Rejected — caught by V7a at the first key != inner (and, were V7a removed,
// by V7g, since A's sig does not verify under another member's operator key).
BOOST_AUTO_TEST_CASE(Inflation_OnePremitManyKeys_Rejected)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl;
    pl.quorum_hash = QuorumHash();
    memcpy(pl.group_pk_bytes, kGroupPk, 48);
    pl.vvec_hash        = VvecHash();
    pl.formation_height = 100;
    for (const auto& g : q.gms) pl.member_node_ids.push_back(g->pdgmState->node_id);

    const uint256 A = q.gms[0]->proTxHash;
    PTXDKGPhase4Msg pA = MakePremit(A, QuorumHash(), kGroupPk, VvecHash(), q.sks.at(A));
    // insert A's premit under 6 distinct quorum-member keys
    for (int i = 0; i < 6; i++)
        pl.premit_commitments[q.gms[i]->proTxHash] = pA;

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    // first offending entry is a key != inner(A)
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-premit-key-mismatch");
}

// V7b (quorum membership / GAP attack) — a correctly self-signed premit from a GM
// that is NOT in the canonical quorum is rejected.
BOOST_AUTO_TEST_CASE(V7b_CommitterNotInQuorum_RejectsGapAttack)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl = MakePayload(q, 6);

    // An outsider GM (not in q.gms), correctly self-signed.
    CBLSSecretKey outSk = NewSk();
    auto outsider = MakeGM(99, "gm99:8080", outSk);
    pl.premit_commitments[outsider->proTxHash] =
        MakePremit(outsider->proTxHash, QuorumHash(), kGroupPk, VvecHash(), outSk);

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-committer-not-in-quorum");

    // Non-vacuity: with the outsider added to the quorum vector, the same premit
    // is accepted (the check keys on quorum membership, nothing else).
    Quorum q2 = q;
    q2.gms.push_back(outsider);
    CValidationState state2;
    BOOST_CHECK(PTX_DKG_VerifyPremits(q2.gms, pl, state2));
}

// V7c — a premit referencing a different quorum_hash (internally consistent, so
// V7g passes) is rejected by the field-agreement check.
BOOST_AUTO_TEST_CASE(V7c_PremitQuorumMismatch_Rejects)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl = MakePayload(q, 6);

    const uint256 K = q.gms[0]->proTxHash;
    uint256 otherQh = Bytes32(0xFB, 0x02);
    pl.premit_commitments[K] =
        MakePremit(K, /*qh=*/otherQh, kGroupPk, VvecHash(), q.sks.at(K));

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-premit-quorum-mismatch");
}

// V7d — a premit whose group_pk disagrees with the payload (signed over its own,
// so V7g passes) is rejected.
BOOST_AUTO_TEST_CASE(V7d_PremitGroupPkMismatch_Rejects)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl = MakePayload(q, 6);

    const uint256 K = q.gms[0]->proTxHash;
    pl.premit_commitments[K] =
        MakePremit(K, QuorumHash(), /*gpk=*/kGroupPk2, VvecHash(), q.sks.at(K));

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-premit-grouppk-mismatch");
}

// V7e — a premit whose vvec_hash disagrees with the payload is rejected.
BOOST_AUTO_TEST_CASE(V7e_PremitVvecHashMismatch_Rejects)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl = MakePayload(q, 6);

    const uint256 K = q.gms[0]->proTxHash;
    uint256 otherVvec = Bytes32(0x5E, 0xDD);
    pl.premit_commitments[K] =
        MakePremit(K, QuorumHash(), kGroupPk, /*vvec=*/otherVvec, q.sks.at(K));

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-premit-vvechash-mismatch");
}

// V7f — a quorum member with an unset/invalid operator key fails fast (before any
// signature verification).
BOOST_AUTO_TEST_CASE(V7f_BadOperatorKey_Rejects)
{
    Quorum q = MakeQuorum(11);
    // Replace member 0 with one whose operator key is never set => .Get() invalid.
    CBLSSecretKey throwaway = NewSk();
    auto noKeyGm = MakeGM(0, "gm0:8080", throwaway, /*setOpKey=*/false);
    q.gms[0] = noKeyGm;

    PTXDKGPayload pl = MakePayload(q, 6); // includes a premit "from" member 0
    // member 0's premit must otherwise be well-formed (inner=key, fields agree).
    pl.premit_commitments[noKeyGm->proTxHash] =
        MakePremit(noKeyGm->proTxHash, QuorumHash(), kGroupPk, VvecHash(), throwaway);

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-bad-operator-key");
}

// V7g — flipping one byte of an otherwise-valid premit signature is rejected.
BOOST_AUTO_TEST_CASE(V7g_BadPremitSig_Rejects)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl = MakePayload(q, 6);

    const uint256 K = q.gms[0]->proTxHash;
    PTXDKGPhase4Msg good = MakePremit(K, QuorumHash(), kGroupPk, VvecHash(), q.sks.at(K));
    std::vector<uint8_t> sb = good.sig.ToByteVector();
    sb[4] ^= 0xFF;
    good.sig.SetByteVector(sb);
    pl.premit_commitments[K] = good;

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-bad-premit-sig");
}

// V8 (strict) — 7 entries, exactly one invalid (bad sig): the WHOLE set is
// rejected. There is no count-the-survivors acceptance.
BOOST_AUTO_TEST_CASE(V8_StrictAllMustPass_OneBadRejectsWhole)
{
    Quorum q = MakeQuorum(11);
    PTXDKGPayload pl = MakePayload(q, 7); // 7 valid premits

    // Corrupt the signature of exactly one (member 6).
    const uint256 bad = q.gms[6]->proTxHash;
    PTXDKGPhase4Msg p = pl.premit_commitments[bad];
    std::vector<uint8_t> sb = p.sig.ToByteVector();
    sb[7] ^= 0xFF;
    p.sig.SetByteVector(sb);
    pl.premit_commitments[bad] = p;

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_VerifyPremits(q.gms, pl, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-bad-premit-sig");
}

// ===========================================================================
// V5 — PTX_DKG_SelectQuorumFromList (the C-1 shared selection core)
// ===========================================================================

// V5 node_id displacement (LOAD-BEARING) — an empty-node_id interloper is filtered
// out BEFORE CalculateQuorum, so it can never occupy a quorum slot. With 11
// candidates (10 eligible + 1 interloper) the result is 10, not 11; giving the
// interloper a node_id flips the result to 11 including it. Stub IsGMPTXEligible
// ->true => the empty-id case returns 11 with the interloper => this test RED.
BOOST_AUTO_TEST_CASE(V5_NodeIdFilter_ExcludesEmptyNodeIdInterloper)
{
    const uint256 fbh = QuorumHash();

    auto buildList = [&](bool interloperEligible) {
        CDeterministicGMList list;
        for (int i = 0; i < 10; i++)
            list.AddGM(MakeGM((uint64_t)i, "gm" + std::to_string(i) + ":8080", NewSk()));
        // interloper: id 10, node_id empty unless interloperEligible
        list.AddGM(MakeGM(10, interloperEligible ? "gm10:8080" : "", NewSk()));
        return list;
    };

    std::vector<unsigned char> ib(32, 0); ib[0] = 10; ib[1] = 0xA1;
    const uint256 interloper(ib);

    // Empty node_id: interloper filtered out => 10 selected, interloper absent.
    {
        CDeterministicGMList list = buildList(false);
        auto q = PTX_DKG_SelectQuorumFromList(list, fbh);
        BOOST_REQUIRE_EQUAL(q.size(), 10u);
        for (const auto& dgm : q)
            BOOST_CHECK(dgm->proTxHash != interloper);
    }
    // node_id set: interloper is eligible => 11 selected, interloper present.
    {
        CDeterministicGMList list = buildList(true);
        auto q = PTX_DKG_SelectQuorumFromList(list, fbh);
        BOOST_REQUIRE_EQUAL(q.size(), 11u);
        bool found = false;
        for (const auto& dgm : q) if (dgm->proTxHash == interloper) found = true;
        BOOST_CHECK(found);
    }
}

// V5 PoSe exclusion (CONFIRMATORY ONLY — not a node_id stub->RED). A PoSe-banned
// GM is excluded by CalculateScores' own ForEachGM(true); this stays GREEN even if
// the node_id eligibility filter were stubbed off. Documents inherited behaviour.
BOOST_AUTO_TEST_CASE(V5_PoSeBannedExcluded_Confirmatory)
{
    const uint256 fbh = QuorumHash();
    CDeterministicGMList list;
    for (int i = 0; i < 11; i++)
        list.AddGM(MakeGM((uint64_t)i, "gm" + std::to_string(i) + ":8080", NewSk()));
    // a 12th eligible GM, but PoSe-banned
    auto banned = MakeGM(11, "gm11:8080", NewSk());
    std::const_pointer_cast<CDeterministicGMState>(banned->pdgmState)->nPoSeBanHeight = 1;
    list.AddGM(banned);

    std::vector<unsigned char> bb(32, 0); bb[0] = 11; bb[1] = 0xA1;
    const uint256 bannedPtx(bb);

    auto q = PTX_DKG_SelectQuorumFromList(list, fbh);
    BOOST_REQUIRE_EQUAL(q.size(), 11u);
    for (const auto& dgm : q)
        BOOST_CHECK(dgm->proTxHash != bannedPtx);
}

// V5 underfull input — 10 eligible GMs select 10. The size != 11 REJECT itself is
// in CheckPTXDKGTx (chain path; ODC-031 / Package 3), not in the core; this asserts
// the underfull condition the reject keys on is reachable.
BOOST_AUTO_TEST_CASE(V5_Underfull_TenEligibleSelectsTen)
{
    const uint256 fbh = QuorumHash();
    CDeterministicGMList list;
    for (int i = 0; i < 10; i++)
        list.AddGM(MakeGM((uint64_t)i, "gm" + std::to_string(i) + ":8080", NewSk()));
    auto q = PTX_DKG_SelectQuorumFromList(list, fbh);
    BOOST_CHECK_EQUAL(q.size(), 10u);
}

BOOST_AUTO_TEST_SUITE_END()
