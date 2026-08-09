    // Copyright (c) 2017 The Dash Core developers
// Copyright (c) 2020-2022 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef Hemis_SPECIALTX_H
#define Hemis_SPECIALTX_H

#include "evo/deterministicgms.h"
#include "llmq/quorums_commitment.h"
#include "ptx/ptx_pose.h"
#include "validation.h" // cs_main needed by CheckLLMQCommitment (!TODO: remove)
#include "version.h"

class CBlock;
class CBlockIndex;
class CCoinsViewCache;
class CValidationState;
class CTransaction;
class uint256;

/** The maximum allowed size of the extraPayload (for any TxType) */
static const unsigned int MAX_SPECIALTX_EXTRAPAYLOAD = 10000;

/** Payload validity checks (including duplicate unique properties against list at pindexPrev)*/
// Note: for +v2, if the tx is not a special tx, this method returns true.
// Note2: This function only performs extra payload related checks, it does NOT checks regular inputs and outputs.
bool CheckSpecialTx(const CTransaction& tx, const CBlockIndex* pindexPrev, const CCoinsViewCache* view, CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// Basic non-contextual checks for special txes
// Note: for +v2, if the tx is not a special tx, this method returns true.
bool CheckSpecialTxNoContext(const CTransaction& tx, CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// Update internal tiertwo data when blocks containing special txes get connected/disconnected
bool ProcessSpecialTxsInBlock(const CBlock& block, const CBlockIndex* pindex, const CCoinsViewCache* view, CValidationState& state, bool fJustCheck) EXCLUSIVE_LOCKS_REQUIRED(cs_main);
bool UndoSpecialTxsInBlock(const CBlock& block, const CBlockIndex* pindex);

// ODC-022 KDD-033: validate a v3 ProRegPL node_id (Amendment 1 label rules + chain-derived
// suffix check).  collateral is the actual resolved outpoint (tx hash + index).
// Exposed for unit tests so node_id correctness can be checked without a valid BLS pubkey.
bool ValidateProRegNodeId(const std::string& node_id,
                           const COutPoint& collateral,
                           CValidationState& state);

// ODC-022 Step 10: validate the optional scriptPTXPayment field on a ProRegPL.
// Empty is allowed (operator opts out of lottery eligibility). Non-empty must be P2PKH —
// same constraint as scriptPayout — to prevent an unspendable script from becoming the
// PTXPAYOUT recipient and burning accumulated lottery funds.
// Exposed for unit tests (mirrors ValidateProRegNodeId extraction pattern).
bool ValidateProRegPTXPayee(const ProRegPL& pl, CValidationState& state);

// ODC-022: block-level PTXCOALESCE count rules C7 (≤1 per block) and C8
// (mandatory iff PTXSESS present, forbidden otherwise).  Called from
// ProcessSpecialTxsInBlock and from the integration test so the test
// exercises C7/C8 against the generator's output without invoking DGM or LLMQ.
bool CheckPTXCoalesceBlockRules(const CBlock& block,
                                CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// ODC-022 Step 7: PTXCOALESCE structural check + LotteryState update.
// Validates that the PTXCOALESCE in block (if any) spends exactly the expected inputs
// (prior accumulator + all PTXSESS fee outputs from this block, in block order) and
// carries the correct accumulated value.  When !fJustCheck, updates the in-memory
// LotteryState singleton and writes the post-block snapshot to evodb.
//
// BUG-024: pEffAccumOutpoint/pEffAccumValue (may be nullptr) receive the EFFECTIVE
// post-block accumulator — the coalesce's output if the block carries one, the
// current LotteryState otherwise — filled under BOTH fJustCheck values.  The payout
// checks downstream must read this, not the raw global: under fJustCheck the apply
// is skipped, and a coalesce+payout block must validate identically to connect.
//
// Called by ProcessSpecialTxsInBlock; exposed separately so unit tests can exercise
// the Step 7 logic without invoking deterministicGMManager or llmq::quorumBlockProcessor.
bool CheckAndApplyPTXCoalesce(const CBlock& block,
                              const CBlockIndex* pindex,
                              CValidationState& state,
                              bool fJustCheck,
                              COutPoint* pEffAccumOutpoint,
                              CAmount* pEffAccumValue) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// ODC-022 Step 8: block-level PTXPAYOUT count rule P8 (≤1 per block) and
// settlement-boundary rule P9 (height % nPTXSettlementWindow == 0).
// No DGM list access — safe to call with a dummy pindex (unit tests).
// Called from ProcessSpecialTxsInBlock after CheckAndApplyPTXCoalesce.
bool CheckPTXPayoutBlockRules(const CBlock& block,
                               const CBlockIndex* pindex,
                               CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// BUG-032 2c: the coin-chained matched-pair rule. Every PTXSESS (settle, the
// reveal) must be the child of a same-block PTXROLLCOMMIT (2a, the commitment) —
// it spends an output of the commitment, so the UTXO layer forbids separating
// them (a reorg that drops the commitment makes the settle's input nonexistent →
// the settle is not a valid tx at all, with no PTX rule involved). On that coin
// binding it binds the payloads: settle.round_seed / quorum_hash must equal the
// parent commitment's (Q2 / BUG-033 settle-side); Q1 (results == MapBeacon) lands
// in 2c-iii. Block-level (needs block.vtx) — the coalesce→payout / mandatory-iff
// precedent; per-tx CheckSpecialTx cannot see sibling txs. Exposed for unit tests.
bool CheckPTXRollCommitSettlePairing(const CBlock& block,
                                     CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// ODC-022 Step 8: PTXPAYOUT contextual checks (P2, P5, P10) + LotteryState update.
// gmList and poseTracker are pre-fetched by the caller so unit tests can inject
// hand-built fixtures without needing a live deterministicGMManager or chain.
// When !fJustCheck, updates LotteryState and writes the post-block snapshot to evodb.
//
// BUG-024: effAccumOutpoint/effAccumValue are the EFFECTIVE accumulator from
// CheckAndApplyPTXCoalesce — P2/P5/P11 evaluate against these, never the raw
// global, so a block whose payout spends its own coalesce's output passes
// TestBlockValidity exactly as it would connect (§3.5 ordering, both fJustCheck
// values).  Callers without a coalesce in the block pass the current
// LotteryState fields.
//
// Called by ProcessSpecialTxsInBlock; exposed separately for unit tests.
bool CheckAndApplyPTXPayout(const CBlock& block,
                             const CBlockIndex* pindex,
                             const CDeterministicGMList& gmList,
                             const PTXPoSeTracker& poseTracker,
                             const COutPoint& effAccumOutpoint,
                             CAmount effAccumValue,
                             CValidationState& state,
                             bool fJustCheck) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// Validate given LLMQ final commitment with the list at pindexQuorum
bool VerifyLLMQCommitment(const llmq::CFinalCommitment& qfc, const CBlockIndex* pindexPrev, CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

uint256 CalcTxInputsHash(const CTransaction& tx);

// Validation for PTXDKG transactions (nType=11).
// Structural (both paths): payload deserializes; group_pk_bytes decompresses;
// member list non-empty and count ≤ 11; premit_commitments.size() >= t=6; sig
// fields non-null.
// Contextual (pindexPrev != nullptr): V1–V8 attestation checks (KDD-059/060) —
// quorum anchoring (LookupBlockIndex / height / GetAncestor reorg-safety /
// GetListForBlock) and per-premit operator-key signature AGREEMENT against the
// canonically-selected quorum.  Accountability, not correctness.  Requires
// cs_main for chain access; the null (pindexPrev == nullptr) path is
// structural-only.
bool CheckPTXDKGTx(const CTransaction& tx, const CBlockIndex* pindexPrev,
                   CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// W1.3 spec §4.4 (KDD-058): block-level PTXDKG count rule — at most one
// PTXDKG per block, mirroring the C7/P8 pattern.  Cross-block per-formation
// uniqueness is ODC-030, resolved with W2 lifecycle — NOT here.
// Called from ProcessSpecialTxsInBlock; exposed separately for unit tests.
bool CheckPTXDKGBlockRules(const CBlock& block,
                           CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

#endif // Hemis_SPECIALTX_H
