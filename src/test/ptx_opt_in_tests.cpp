// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "chainparams.h"
#include "chainparamsbase.h"
#include "consensus/validation.h"
#include "evo/deterministicgms.h"
#include "evo/providertx.h"
#include "evo/specialtx_validation.h"
#include "key.h"
#include "key_io.h"
#include "pubkey.h"
#include "script/script.h"
#include "uint256.h"

#include <boost/test/unit_test.hpp>

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

BOOST_FIXTURE_TEST_SUITE(ptx_opt_in_tests, PTXBeaTestingSetup)

// ---------------------------------------------------------------------------
// Helper
// ---------------------------------------------------------------------------

// Run ValidateProRegPTXPayee and return the reject reason ("" = pass).
static std::string RunPTXPayeeCheck(const CScript& script)
{
    LOCK(cs_main);
    ProRegPL pl;
    pl.scriptPTXPayment = script;
    CValidationState state;
    if (ValidateProRegPTXPayee(pl, state)) return "";
    return state.GetRejectReason();
}

// Build a minimal P2PKH script from a fresh key.
static CScript MakeP2PKH()
{
    CKey key; key.MakeNewKey(true);
    CKeyID keyid = key.GetPubKey().GetID();
    return GetScriptForDestination(CTxDestination(keyid));
}

// Build a P2SH script (wrapping a dummy script) — non-P2PKH.
static CScript MakeP2SH()
{
    CScript inner = CScript() << OP_TRUE;
    CScriptID scriptID(inner);
    return GetScriptForDestination(CTxDestination(scriptID));
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Empty scriptPTXPayment (legitimate opt-out) must always pass.
// Falsification: if the check fires on empty, this goes RED.
BOOST_AUTO_TEST_CASE(OptIn_PTXPayee_AcceptsEmpty)
{
    BOOST_CHECK_EQUAL(RunPTXPayeeCheck(CScript()), "");
}

// A valid P2PKH script must be accepted.
// Falsification target: flip !IsPayToPublicKeyHash() → IsPayToPublicKeyHash() in
// ValidateProRegPTXPayee; this test goes RED (P2PKH now rejected).
BOOST_AUTO_TEST_CASE(OptIn_PTXPayee_AcceptsP2PKH)
{
    BOOST_CHECK_EQUAL(RunPTXPayeeCheck(MakeP2PKH()), "");
}

// A non-P2PKH script (P2SH here) must be rejected with the canonical reason.
// Falsification target: same flip as above; this test goes RED (P2SH now passes).
// Together with AcceptsP2PKH, the two tests bound both sides of the comparator.
BOOST_AUTO_TEST_CASE(OptIn_PTXPayee_RejectsNonP2PKH)
{
    BOOST_CHECK_EQUAL(RunPTXPayeeCheck(MakeP2SH()), "bad-protx-ptx-payee");
}

// Verify that CDeterministicGMState copies scriptPTXPayment from ProRegPL.
// This pins the propagation path from registration to DGM state.
// If deterministicgms.h:65 (the copy line) is removed, the DGM state has an empty
// script and PTX_SelectWinner skips the GM — a silent consensus regression.
BOOST_AUTO_TEST_CASE(OptIn_PTXPayee_PropagatesFromProRegToGMState)
{
    CScript payScript = MakeP2PKH();

    ProRegPL pl;
    pl.scriptPTXPayment = payScript;

    CDeterministicGMState gmState(pl);
    BOOST_CHECK(gmState.scriptPTXPayment == payScript);
}

BOOST_AUTO_TEST_SUITE_END()
