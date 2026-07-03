// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// W1.3 Package 3: block-inject wiring unit tests (spec §4, KDD-058).
// Direct-call tests — no daemon, no P2P, no chain fixture.
//
// Test inventory:
//   W1_CheckTransaction_AcceptsRealPTXDKG        Green  BuildPTXDKGTx output passes
//                                                       CheckTransaction (tx_verify
//                                                       empty-vin/vout exemption)
//   W2_CheckTransaction_RejectsDegenerateNType11 Green  nType=11 with no payload is NOT
//                                                       exempted (IsPTXDKGTx requires
//                                                       IsSpecialTx) → bad-txns-vin-empty
//   W3_CheckTransaction_RejectsOrdinaryEmptyVin  Green  guard not weakened for normal txs
//   W4_BlockRules_RejectsTwoPTXDKG               Green  two PTXDKG in a block → ptxdkg-duplicate
//   W5_BlockRules_AcceptsOnePTXDKG               Green  one PTXDKG in a block passes
//   W6_BlockRules_AcceptsZeroPTXDKG              Green  PTXDKG-free block passes
//   P1_Pending_SetGetClear                       Green  slot set → get returns it; clear → empty
//   P2_Pending_RefuseWhileSet                    Green  second Set refused while occupied (E-4)
//   P3_Pending_ValidateBeforeInject              Green  structurally-invalid tx refused at populate
//   P4_Pending_ClearOnInclusion                  Green  clear keys on txid match, not any PTXDKG
//   R1_NetGate_RefusesNonPermittedNets           Green  RPC hard-errors on main/testnet/ptxtestnet
//                                                       (C5 net-gate — the safety property)
//   R2_Populate_SmokeValidRegtest                Green  RPC populates on regtest; occupied on 2nd
//   R3_NetGate_AdmitsPtxBea                      Green  RPC populates on ptxbea (allowlist arm 2)
//   R4_Force_BypassesPopulateGuard               Green  force seats a bad tx over an occupied slot
//                                                       (E-1); generate-half still rejects it
//
// The P-cases cover the STRUCTURAL half of the C4 pending slot only: with no
// chain in test_ptx (null tip / null pindexPrev) CheckPTXDKGTx runs its
// structural body alone.  The CONTEXTUAL behaviour — populate-refusal against
// a real chain, and the generate-time reorg skip (F-9) — is functional-owed
// at the C6 phase.

#include "test/test_Hemis.h"
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_dkg_pending.h"
#include "consensus/tx_verify.h"
#include "consensus/validation.h"
#include "evo/specialtx_validation.h"
#include "primitives/block.h"
#include "primitives/transaction.h"

#include "bls/bls_wrapper.h"
#include "chainparamsbase.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "sync.h"
#include "uint256.h"

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <map>
#include <string>
#include <vector>

// Forward-declare the C5 debug RPC — external linkage in rpc/ptx.cpp; must be
// declared at file scope, BEFORE the suite macro opens its namespace
// (ptx_explorer_rpc_tests precedent).
UniValue ptx_debug_ptxdkgpopulate(const JSONRPCRequest& request);

BOOST_FIXTURE_TEST_SUITE(ptx_dkg_wiring_tests, BasicTestingSetup)

// ---------------------------------------------------------------------------
// Helpers — copied verbatim from ptx_dkg_phase5_tests.cpp (the established
// copy-per-TU pattern; phase0/1/2/4/5 each carry this block). If the phase5
// helpers change, re-sync this block.
// ---------------------------------------------------------------------------

static std::vector<PTXDKGMember> MakeTestMembers(std::map<uint256, CBLSSecretKey>& key_map)
{
    key_map.clear();
    std::vector<PTXDKGMember> members;
    for (int i = 0; i < 11; i++) {
        PTXDKGMember m;

        std::vector<unsigned char> buf(32, 0);
        buf[0] = (unsigned char)i;
        buf[1] = 0xAA;
        m.proTxHash = uint256(buf);

        buf[1] = 0xBB;
        m.confirmedHash = uint256(buf);

        m.confirmedHashWithProRegTxHash =
            PTX_DKG_ComputeInnerHash(m.proTxHash, m.confirmedHash);

        m.node_id = "gm" + std::to_string(i) + ":8080";

        CBLSSecretKey sk;
        sk.MakeNewKey();
        m.pubKeyOperator = sk.GetPublicKey();
        key_map[m.proTxHash] = sk;

        members.push_back(m);
    }
    return members;
}

static uint256 TestFormationHash()
{
    std::vector<unsigned char> buf(32, 0);
    buf[0] = 0xFB;
    buf[31] = 0x01;
    return uint256(buf);
}

static uint256 PtxOf(const std::vector<PTXDKGSession>& sessions, int i)
{
    return sessions[i].members[sessions[i].my_idx].proTxHash;
}

static void SetupFullPhase0Sessions(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    uint256 fbh = TestFormationHash();
    auto members = MakeTestMembers(key_map);

    sessions.resize(11);
    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_InitSession(sessions[i], members, fbh, members[i].proTxHash));

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(sessions[i].my_idx >= 0);
        BOOST_REQUIRE(PTX_DKG_GenerateLocalContrib(sessions[i]));
    }

    std::vector<PTXDKGPhase0Msg> p0msgs(11);
    for (int i = 0; i < 11; i++) {
        const uint256& my_ptx = sessions[i].members[sessions[i].my_idx].proTxHash;
        p0msgs[i] = PTX_DKG_BuildPhase0Msg(sessions[i], key_map.at(my_ptx));
    }

    for (int recv = 0; recv < 11; recv++)
        for (int sender = 0; sender < 11; sender++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase0Msg(sessions[recv], p0msgs[sender]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase0(sessions[i]));
}

static void AdvanceToComplaint(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    SetupFullPhase0Sessions(key_map, sessions);

    std::vector<PTXDKGPhase1Msg> p1msgs(11);
    for (int i = 0; i < 11; i++)
        p1msgs[i] = PTX_DKG_BuildPhase1Msg(sessions[i], key_map.at(PtxOf(sessions, i)));

    for (int r = 0; r < 11; r++)
        for (int s = 0; s < 11; s++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase1Msg(sessions[r], p1msgs[s]));

    for (int r = 0; r < 11; r++) {
        const uint256& rptx = PtxOf(sessions, r);
        for (int s = 0; s < 11; s++)
            PTX_DKG_DecryptMyShare(sessions[r], PtxOf(sessions, s), key_map.at(rptx));
    }

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase1(sessions[i]));
}

static void AdvanceToPremit(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    AdvanceToComplaint(key_map, sessions);

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase2(sessions[i]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(PTX_DKG_ClosePhase3(sessions[i]));

    for (int i = 0; i < 11; i++)
        BOOST_REQUIRE(sessions[i].phase == PTXDKGPhase::PREMIT);
}

// Drive all 11 sessions through P4 (compute + broadcast + close) → FINALIZE.
static void AdvanceToFinalize(
    std::map<uint256, CBLSSecretKey>& key_map,
    std::vector<PTXDKGSession>& sessions)
{
    AdvanceToPremit(key_map, sessions);

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ComputeSkShare(sessions[i]));
        BOOST_REQUIRE(PTX_DKG_ComputeGroupPk(sessions[i]));
    }

    // Build and broadcast all 11 Phase 4 messages to all sessions.
    std::vector<PTXDKGPhase4Msg> p4msgs(11);
    for (int s = 0; s < 11; s++)
        p4msgs[s] = PTX_DKG_BuildPhase4Msg(sessions[s], key_map.at(PtxOf(sessions, s)));

    for (int r = 0; r < 11; r++)
        for (int s = 0; s < 11; s++)
            BOOST_REQUIRE(PTX_DKG_ReceivePhase4Msg(sessions[r], p4msgs[s]));

    for (int i = 0; i < 11; i++) {
        BOOST_REQUIRE(PTX_DKG_ClosePhase4(sessions[i]));
        BOOST_REQUIRE(sessions[i].phase == PTXDKGPhase::FINALIZE);
    }
}

// ---------------------------------------------------------------------------
// W1_CheckTransaction_AcceptsRealPTXDKG
//
// A real BuildPTXDKGTx output (empty vin, empty vout, populated payload)
// passes CheckTransaction: the tx_verify empty-vin/vout guards exempt
// IsPTXDKGTx() (spec §4.1, KDD-058 mechanism a).
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(W1_CheckTransaction_AcceptsRealPTXDKG)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);

    CMutableTransaction mtx = PTX_DKG_BuildPTXDKGTx(sessions[0], 1000);
    CTransaction tx(mtx);

    BOOST_REQUIRE(tx.vin.empty());
    BOOST_REQUIRE(tx.vout.empty());
    BOOST_REQUIRE(tx.IsPTXDKGTx());

    CValidationState state;
    BOOST_CHECK(CheckTransaction(tx, state, true /* fColdStakingActive */));
    BOOST_CHECK(state.IsValid());
}

// ---------------------------------------------------------------------------
// W2_CheckTransaction_RejectsDegenerateNType11
//
// nType=11 with absent extraPayload: IsPTXDKGTx() is false (requires
// IsSpecialTx() → populated payload), so the exemption does NOT apply and the
// vin-empty guard still rejects. Proves the IsSpecialTx keying.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(W2_CheckTransaction_RejectsDegenerateNType11)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXDKG;
    // no SetTxPayload — extraPayload absent
    CTransaction tx(mtx);

    BOOST_REQUIRE(!tx.IsPTXDKGTx());

    CValidationState state;
    BOOST_CHECK(!CheckTransaction(tx, state, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-vin-empty");
}

// ---------------------------------------------------------------------------
// W3_CheckTransaction_RejectsOrdinaryEmptyVin
//
// An ordinary (nType=NORMAL) sapling-version tx with empty vin still rejects:
// the guard is not weakened for non-PTXDKG transactions.
// ---------------------------------------------------------------------------

BOOST_AUTO_TEST_CASE(W3_CheckTransaction_RejectsOrdinaryEmptyVin)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
    CTransaction tx(mtx);

    BOOST_REQUIRE(tx.vin.empty());
    BOOST_REQUIRE(!tx.IsPTXDKGTx());

    CValidationState state;
    BOOST_CHECK(!CheckTransaction(tx, state, true));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-txns-vin-empty");
}

// ---------------------------------------------------------------------------
// W4/W5/W6 — CheckPTXDKGBlockRules (W1.3 spec §4.4, one PTXDKG per block)
//
// The block-rule scan keys on IsPTXDKGTx() only (type + populated payload);
// it never parses the payload, so a type-only tx with a one-byte payload is
// the honest minimal fixture — same direct-call pattern the C7/P8 coalesce
// and payout block-rule tests use.
// ---------------------------------------------------------------------------

static CMutableTransaction MakeTypeOnlyPTXDKGTx()
{
    CMutableTransaction mtx;
    mtx.nVersion     = CTransaction::TxVersion::SAPLING;
    mtx.nType        = CTransaction::TxType::PTXDKG;
    mtx.extraPayload = std::vector<uint8_t>{0x01}; // populated => IsSpecialTx()
    return mtx;
}

BOOST_AUTO_TEST_CASE(W4_BlockRules_RejectsTwoPTXDKG)
{
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(MakeTypeOnlyPTXDKGTx()));
    block.vtx.push_back(MakeTransactionRef(MakeTypeOnlyPTXDKGTx()));
    BOOST_REQUIRE(block.vtx[0]->IsPTXDKGTx() && block.vtx[1]->IsPTXDKGTx());

    LOCK(cs_main); // CheckPTXDKGBlockRules asserts cs_main (block-connect contract)
    CValidationState state;
    BOOST_CHECK(!CheckPTXDKGBlockRules(block, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-duplicate");
}

BOOST_AUTO_TEST_CASE(W5_BlockRules_AcceptsOnePTXDKG)
{
    CBlock block;
    block.vtx.push_back(MakeTransactionRef(MakeTypeOnlyPTXDKGTx()));

    LOCK(cs_main);
    CValidationState state;
    BOOST_CHECK(CheckPTXDKGBlockRules(block, state));
    BOOST_CHECK(state.IsValid());
}

BOOST_AUTO_TEST_CASE(W6_BlockRules_AcceptsZeroPTXDKG)
{
    CBlock block;
    CMutableTransaction normal;
    normal.nVersion = CTransaction::TxVersion::SAPLING;
    normal.vout.emplace_back(1 * COIN, CScript() << OP_TRUE);
    block.vtx.push_back(MakeTransactionRef(normal));

    LOCK(cs_main);
    CValidationState state;
    BOOST_CHECK(CheckPTXDKGBlockRules(block, state));
    BOOST_CHECK(state.IsValid());
}

// ---------------------------------------------------------------------------
// P1–P4 — the C4 pending-injection slot (KDD-058; W1.3 spec §4).
//
// Structural level only (see the inventory note above).  Each case clears
// the slot at entry AND exit: the slot is process-global, and the phase5 TU
// runs earlier in this binary — its ClosePhase5 calls populate the slot via
// the (dormant) C4 wiring, so entry state is not empty by default.
// ---------------------------------------------------------------------------

// Build a structurally-VALID PTXDKG through the real ceremony (the honest
// fixture: with null tip, SetPendingTx's CheckPTXDKGTx runs its structural
// body, which a BuildPTXDKGTx output passes).
static CTransactionRef MakeRealPTXDKGTxRef(int formation_height, int session_idx = 0)
{
    std::map<uint256, CBLSSecretKey> key_map;
    std::vector<PTXDKGSession> sessions;
    AdvanceToFinalize(key_map, sessions);
    return MakeTransactionRef(PTX_DKG_BuildPTXDKGTx(sessions[session_idx], formation_height));
}

BOOST_AUTO_TEST_CASE(P1_Pending_SetGetClear)
{
    PTX_DKG_ClearPendingTx();
    BOOST_REQUIRE(!PTX_DKG_HasPendingTx());
    {
        LOCK(cs_main);
        CTransactionRef out;
        BOOST_CHECK(!PTX_DKG_GetMinablePTXDKGTx(nullptr, out)); // empty slot → false
    }

    CTransactionRef tx = MakeRealPTXDKGTxRef(1000);
    CValidationState state;
    BOOST_CHECK(PTX_DKG_SetPendingTx(tx, state));
    BOOST_CHECK(state.IsValid());
    BOOST_CHECK(PTX_DKG_HasPendingTx());

    // Structural GetMinable: null pindexPrev (no chain) → structural
    // re-validation passes → the slot tx comes back.
    {
        LOCK(cs_main);
        CTransactionRef out;
        BOOST_REQUIRE(PTX_DKG_GetMinablePTXDKGTx(nullptr, out));
        BOOST_CHECK(out->GetHash() == tx->GetHash());
    }

    PTX_DKG_ClearPendingTx();
    BOOST_CHECK(!PTX_DKG_HasPendingTx());
    {
        LOCK(cs_main);
        CTransactionRef out;
        BOOST_CHECK(!PTX_DKG_GetMinablePTXDKGTx(nullptr, out));
    }
}

BOOST_AUTO_TEST_CASE(P2_Pending_RefuseWhileSet)
{
    PTX_DKG_ClearPendingTx();

    CTransactionRef tx0 = MakeRealPTXDKGTxRef(1000);
    CTransactionRef tx1 = MakeRealPTXDKGTxRef(2000);
    BOOST_REQUIRE(tx0->GetHash() != tx1->GetHash());

    CValidationState state0;
    BOOST_REQUIRE(PTX_DKG_SetPendingTx(tx0, state0));

    // E-4: REFUSE-WHILE-SET — no last-wins; explicit clear required.
    CValidationState state1;
    BOOST_CHECK(!PTX_DKG_SetPendingTx(tx1, state1));
    BOOST_CHECK_EQUAL(state1.GetRejectReason(), "ptxdkg-pending-occupied");

    // The first tx is still the occupant.
    {
        LOCK(cs_main);
        CTransactionRef out;
        BOOST_REQUIRE(PTX_DKG_GetMinablePTXDKGTx(nullptr, out));
        BOOST_CHECK(out->GetHash() == tx0->GetHash());
    }

    PTX_DKG_ClearPendingTx();
}

BOOST_AUTO_TEST_CASE(P3_Pending_ValidateBeforeInject)
{
    PTX_DKG_ClearPendingTx();

    // Type-only PTXDKG: payload {0x01} fails GetTxPayload → the structural
    // body's first reject.  VALIDATE-BEFORE-INJECT must refuse it at
    // populate time and leave the slot empty.
    CTransactionRef bad = MakeTransactionRef(MakeTypeOnlyPTXDKGTx());
    BOOST_REQUIRE(bad->IsPTXDKGTx());

    CValidationState state;
    BOOST_CHECK(!PTX_DKG_SetPendingTx(bad, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "ptxdkg-bad-payload");
    BOOST_CHECK(!PTX_DKG_HasPendingTx());

    PTX_DKG_ClearPendingTx();
}

BOOST_AUTO_TEST_CASE(P4_Pending_ClearOnInclusion)
{
    PTX_DKG_ClearPendingTx();

    CTransactionRef tx = MakeRealPTXDKGTxRef(1000);
    CValidationState state;
    BOOST_REQUIRE(PTX_DKG_SetPendingTx(tx, state));

    // A block with a DIFFERENT PTXDKG does not clear — match is by txid.
    CBlock other;
    other.vtx.push_back(MakeTransactionRef(MakeTypeOnlyPTXDKGTx()));
    BOOST_REQUIRE(other.vtx[0]->IsPTXDKGTx());
    PTX_DKG_ClearPendingIfIncluded(other);
    BOOST_CHECK(PTX_DKG_HasPendingTx());

    // A PTXDKG-free block does not clear.
    CBlock empty;
    PTX_DKG_ClearPendingIfIncluded(empty);
    BOOST_CHECK(PTX_DKG_HasPendingTx());

    // The block that includes the slot tx clears it.
    CBlock incl;
    incl.vtx.push_back(tx);
    PTX_DKG_ClearPendingIfIncluded(incl);
    BOOST_CHECK(!PTX_DKG_HasPendingTx());

    PTX_DKG_ClearPendingTx();
}

// ---------------------------------------------------------------------------
// R-cases — C5 debug injection RPC (ptx_debug_ptxdkgpopulate).
//
// The RPC is called DIRECTLY (extern decl; ptx_explorer_rpc_tests precedent —
// test_ptx links LIBBITCOIN_SERVER).  The NET-GATE is the load-bearing safety
// property: the RPC feeds block production, so it must hard-error on every
// network except regtest and ptxbea.  JSONRPCError throws UniValue.
//
// Fixture note: BasicTestingSetup defaults to MAIN; RegtestSetup/PtxBeaSetup
// re-run it with the permitted networks.  Each case constructs a fresh
// fixture, so in-case SelectParams switches (R1) cannot leak forward.
// ---------------------------------------------------------------------------

namespace {

struct RegtestSetup : public BasicTestingSetup {
    RegtestSetup() : BasicTestingSetup(CBaseChainParams::REGTEST) {}
};

struct PtxBeaSetup : public BasicTestingSetup {
    PtxBeaSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

// A populate request with a spec the structural body accepts (premits=6,
// members=11) or rejects (premits<6), per the premits argument.
JSONRPCRequest MakePopulateRequest(int premits, bool force)
{
    UniValue payload(UniValue::VOBJ);
    payload.pushKV("quorum_hash",
                   "ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01ab01");
    payload.pushKV("formation_height", 100);
    payload.pushKV("group_pk", "generate");
    payload.pushKV("premits", premits);
    payload.pushKV("members", 11);
    JSONRPCRequest req;
    req.fHelp = false;
    req.params = UniValue(UniValue::VARR);
    req.params.push_back(payload);
    req.params.push_back(UniValue(force));
    return req;
}

} // namespace

BOOST_AUTO_TEST_CASE(R1_NetGate_RefusesNonPermittedNets)
{
    PTX_DKG_ClearPendingTx();

    // Fixture selected MAIN.  A fully-valid request must die at the gate —
    // before any state touch — on main, public testnet and ptxtestnet.  The
    // slot is cleared between sub-checks so each one tests the gate alone:
    // with the gate stubbed, every throw-assert fails on its own (no
    // accidental pass via ptxdkg-pending-occupied from the previous call).
    const JSONRPCRequest req = MakePopulateRequest(6, false);
    BOOST_CHECK_THROW(ptx_debug_ptxdkgpopulate(req), UniValue);
    PTX_DKG_ClearPendingTx();

    SelectParams(CBaseChainParams::TESTNET);
    BOOST_CHECK_THROW(ptx_debug_ptxdkgpopulate(req), UniValue);
    PTX_DKG_ClearPendingTx();

    SelectParams(CBaseChainParams::PTXTESTNET);
    BOOST_CHECK_THROW(ptx_debug_ptxdkgpopulate(req), UniValue);

    // The gate fired before the slot was touched.
    BOOST_CHECK(!PTX_DKG_HasPendingTx());
}

BOOST_FIXTURE_TEST_CASE(R2_Populate_SmokeValidRegtest, RegtestSetup)
{
    PTX_DKG_ClearPendingTx();

    // Valid spec through the RPC path → guarded populate succeeds.
    const UniValue ret = ptx_debug_ptxdkgpopulate(MakePopulateRequest(6, false));
    BOOST_CHECK(find_value(ret, "populated").get_bool());
    BOOST_CHECK(!find_value(ret, "force").get_bool());
    BOOST_REQUIRE(PTX_DKG_HasPendingTx());

    // The slot holds the returned txid (structural GetMinable, null tip).
    {
        LOCK(cs_main);
        CTransactionRef out;
        BOOST_REQUIRE(PTX_DKG_GetMinablePTXDKGTx(nullptr, out));
        BOOST_CHECK_EQUAL(out->GetHash().GetHex(), find_value(ret, "txid").get_str());
    }

    // Second unforced populate → the RPC surfaces E-4 as a JSONRPCError.
    BOOST_CHECK_THROW(ptx_debug_ptxdkgpopulate(MakePopulateRequest(6, false)), UniValue);

    PTX_DKG_ClearPendingTx();
}

BOOST_FIXTURE_TEST_CASE(R3_NetGate_AdmitsPtxBea, PtxBeaSetup)
{
    PTX_DKG_ClearPendingTx();

    // The second allowlist arm: the RPC runs on ptxbea params.
    const UniValue ret = ptx_debug_ptxdkgpopulate(MakePopulateRequest(6, false));
    BOOST_CHECK(find_value(ret, "populated").get_bool());
    BOOST_CHECK(PTX_DKG_HasPendingTx());

    PTX_DKG_ClearPendingTx();
}

BOOST_FIXTURE_TEST_CASE(R4_Force_BypassesPopulateGuard, RegtestSetup)
{
    PTX_DKG_ClearPendingTx();

    // Seat a valid tx first — minable at the structural level.
    const UniValue ok = ptx_debug_ptxdkgpopulate(MakePopulateRequest(6, false));
    const std::string validTxid = find_value(ok, "txid").get_str();
    {
        LOCK(cs_main);
        CTransactionRef out;
        BOOST_REQUIRE(PTX_DKG_GetMinablePTXDKGTx(nullptr, out));
        BOOST_CHECK_EQUAL(out->GetHash().GetHex(), validTxid);
    }

    // Bad spec (premits=3 < t=6) WITHOUT force → refused twice over (occupied
    // AND structurally invalid); the valid occupant is untouched.
    BOOST_CHECK_THROW(ptx_debug_ptxdkgpopulate(MakePopulateRequest(3, false)), UniValue);
    {
        LOCK(cs_main);
        CTransactionRef out;
        BOOST_REQUIRE(PTX_DKG_GetMinablePTXDKGTx(nullptr, out));
        BOOST_CHECK_EQUAL(out->GetHash().GetHex(), validTxid);
    }

    // Bad spec WITH force (E-1) → seated directly over the occupied slot.
    // The slot is occupied by the BAD tx: HasPendingTx true, but the
    // generate-time structural re-validation now rejects it (keep-but-skip),
    // proving force bypassed only the populate half of the pair.
    const UniValue forced = ptx_debug_ptxdkgpopulate(MakePopulateRequest(3, true));
    const std::string badTxid = find_value(forced, "txid").get_str();
    BOOST_CHECK(badTxid != validTxid);
    BOOST_REQUIRE(PTX_DKG_HasPendingTx());
    {
        LOCK(cs_main);
        CTransactionRef out;
        BOOST_CHECK(!PTX_DKG_GetMinablePTXDKGTx(nullptr, out));
    }

    PTX_DKG_ClearPendingTx();
}

BOOST_AUTO_TEST_SUITE_END()
