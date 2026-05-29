// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "test/test_Hemis.h"

#include "chainparams.h"
#include "chainparamsbase.h"
#include "consensus/validation.h"
#include "crypto/sha256.h"
#include "evo/specialtx_validation.h"
#include "primitives/transaction.h"
#include "serialize.h"
#include "uint256.h"
#include "utilstrencodings.h"

#include <boost/test/unit_test.hpp>

struct PTXBeaTestingSetup : public BasicTestingSetup {
    PTXBeaTestingSetup() : BasicTestingSetup(CBaseChainParams::PTXBEATESTNET) {}
};

BOOST_FIXTURE_TEST_SUITE(ptx_node_id_tests, PTXBeaTestingSetup)

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Compute the expected suffix for a given collateral outpoint.
static std::string ComputeExpectedSuffix(const COutPoint& outpoint)
{
    CDataStream ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << outpoint;
    unsigned char digest[32];
    CSHA256().Write((const unsigned char*)ss.data(), ss.size()).Finalize(digest);
    return HexStr(Span<const uint8_t>(digest, digest + 4));
}

// Run ValidateProRegNodeId and return the reject reason (or "" for pass).
// Uses the extracted standalone function so tests don't need a valid BLS pubkey.
static std::string RunNodeIdCheck(const std::string& node_id, const COutPoint& collateral)
{
    LOCK(cs_main);
    CValidationState state;
    if (ValidateProRegNodeId(node_id, collateral, state)) return "";
    return state.GetRejectReason();
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Two DGMs with the same label but different collateral outpoints produce different full
// node_ids: the suffix is collateral-derived, so label collision alone is allowed.
BOOST_AUTO_TEST_CASE(NodeId_LabelOnlyCollisionAllowed)
{
    COutPoint op1(uint256S("1111111111111111111111111111111111111111111111111111111111111111"), 0);
    COutPoint op2(uint256S("2222222222222222222222222222222222222222222222222222222222222222"), 0);

    std::string suffix1 = ComputeExpectedSuffix(op1);
    std::string suffix2 = ComputeExpectedSuffix(op2);
    BOOST_CHECK(suffix1 != suffix2);  // different collaterals → different suffixes

    // Both pass ValidateProRegNodeId (correct suffix for their respective collaterals).
    BOOST_CHECK_EQUAL(RunNodeIdCheck("gm01:" + suffix1, op1), "");
    BOOST_CHECK_EQUAL(RunNodeIdCheck("gm01:" + suffix2, op2), "");
}

// A node_id whose suffix doesn't match the collateral is rejected.
BOOST_AUTO_TEST_CASE(NodeId_FullStringCollisionRejected)
{
    COutPoint op(uint256S("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"), 0);
    std::string correct_suffix = ComputeExpectedSuffix(op);
    // Flip first hex char to make the suffix wrong.
    std::string wrong_suffix = correct_suffix;
    wrong_suffix[0] = (wrong_suffix[0] == '0') ? '1' : '0';
    BOOST_CHECK_EQUAL(RunNodeIdCheck("gm01:" + wrong_suffix, op), "bad-protx-node-id-suffix");
}

// Suffix derivation is deterministic: same collateral always produces the same 8-char string.
BOOST_AUTO_TEST_CASE(NodeId_SuffixDerivationDeterministic)
{
    COutPoint op(uint256S("cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"), 1);
    std::string s1 = ComputeExpectedSuffix(op);
    std::string s2 = ComputeExpectedSuffix(op);
    std::string s3 = ComputeExpectedSuffix(op);
    BOOST_CHECK_EQUAL(s1, s2);
    BOOST_CHECK_EQUAL(s2, s3);
    BOOST_CHECK_EQUAL(s1.size(), 8u);  // 4 bytes → 8 hex chars
}

// Suffix derivation against a hand-computed expected value — pins the formula.
// Falsification: if the code uses digest[4:8] instead of [0:4], this test turns RED.
// If it stays GREEN under that change, the fixture is tautological — investigate.
BOOST_AUTO_TEST_CASE(NodeId_SuffixDerivationOnChain)
{
    COutPoint op(uint256S("dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"), 0);

    // Hand-compute independently using direct CSHA256 on the streamed outpoint.
    CDataStream ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << op;
    unsigned char digest[32];
    CSHA256().Write((const unsigned char*)ss.data(), ss.size()).Finalize(digest);
    std::string expected = HexStr(Span<const uint8_t>(digest, digest + 4));

    // ValidateProRegNodeId uses the same formula.
    std::string full = "gm01:" + expected;
    BOOST_CHECK_EQUAL(RunNodeIdCheck(full, op), "");  // correct suffix → passes

    // Confirm the expected value is what ComputeExpectedSuffix produces.
    BOOST_CHECK_EQUAL(ComputeExpectedSuffix(op), expected);
}

// An operator who provides a colon in the label (attempting to forge a suffix) is rejected.
// The colon is outside the allowed charset [a-zA-Z0-9_-] so it hits the format check.
BOOST_AUTO_TEST_CASE(NodeId_RejectsOperatorSuppliedSuffix)
{
    COutPoint op(uint256S("eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"), 0);
    // Two colons → format check fires (more than one colon not allowed)
    std::string reason = RunNodeIdCheck("gm01:abcd:efgh", op);
    BOOST_CHECK_EQUAL(reason, "bad-protx-node-id-format");
}

// Amendment 1: all five label reject reasons in one test case.
BOOST_AUTO_TEST_CASE(NodeId_LabelValidation)
{
    COutPoint op(uint256S("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"), 0);
    std::string suf = ComputeExpectedSuffix(op);

    auto check = [&](const std::string& label) -> std::string {
        return RunNodeIdCheck(label + ":" + suf, op);
    };

    // Length: too short (< 3)
    BOOST_CHECK_EQUAL(check("ab"), "bad-protx-node-id-label-length");
    // Length: too long (> 24)
    BOOST_CHECK_EQUAL(check("abcdefghijklmnopqrstuvwxy"), "bad-protx-node-id-label-length");

    // Charset: space is invalid
    BOOST_CHECK_EQUAL(check("gm 1"), "bad-protx-node-id-label-charset");
    // Charset: control character
    BOOST_CHECK_EQUAL(check("gm\x01"), "bad-protx-node-id-label-charset");

    // Edge: leading hyphen
    BOOST_CHECK_EQUAL(check("-gm01"), "bad-protx-node-id-label-edge");
    // Edge: trailing underscore
    BOOST_CHECK_EQUAL(check("gm01_"), "bad-protx-node-id-label-edge");

    // All-numeric label
    BOOST_CHECK_EQUAL(check("1234"), "bad-protx-node-id-label-numeric");

    // Reserved words (case-insensitive; all are 3+ chars to reach the reserved check)
    BOOST_CHECK_EQUAL(check("Admin"),      "bad-protx-node-id-label-reserved");
    BOOST_CHECK_EQUAL(check("SYSTEM"),     "bad-protx-node-id-label-reserved");
    BOOST_CHECK_EQUAL(check("null"),       "bad-protx-node-id-label-reserved");
    BOOST_CHECK_EQUAL(check("none"),       "bad-protx-node-id-label-reserved");
    BOOST_CHECK_EQUAL(check("gamemaster"), "bad-protx-node-id-label-reserved");
    BOOST_CHECK_EQUAL(check("node"),       "bad-protx-node-id-label-reserved");
    BOOST_CHECK_EQUAL(check("default"),    "bad-protx-node-id-label-reserved");
    BOOST_CHECK_EQUAL(check("test"),       "bad-protx-node-id-label-reserved");
    // "gm" is only 2 chars — caught by length check before reserved-word check
    BOOST_CHECK_EQUAL(check("gm"),         "bad-protx-node-id-label-length");

    // Valid label passes
    BOOST_CHECK_EQUAL(check("gm01"), "");
    BOOST_CHECK_EQUAL(check("my-node"), "");
    BOOST_CHECK_EQUAL(check("validator_1"), "");
}

BOOST_AUTO_TEST_SUITE_END()
