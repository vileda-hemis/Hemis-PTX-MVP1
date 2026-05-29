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
// Called by ProcessSpecialTxsInBlock; exposed separately so unit tests can exercise
// the Step 7 logic without invoking deterministicGMManager or llmq::quorumBlockProcessor.
bool CheckAndApplyPTXCoalesce(const CBlock& block,
                              const CBlockIndex* pindex,
                              CValidationState& state,
                              bool fJustCheck) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// ODC-022 Step 8: block-level PTXPAYOUT count rule P8 (≤1 per block) and
// settlement-boundary rule P9 (height % nPTXSettlementWindow == 0).
// No DGM list access — safe to call with a dummy pindex (unit tests).
// Called from ProcessSpecialTxsInBlock after CheckAndApplyPTXCoalesce.
bool CheckPTXPayoutBlockRules(const CBlock& block,
                               const CBlockIndex* pindex,
                               CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// ODC-022 Step 8: PTXPAYOUT contextual checks (P2, P5, P10) + LotteryState update.
// gmList and poseTracker are pre-fetched by the caller so unit tests can inject
// hand-built fixtures without needing a live deterministicGMManager or chain.
// When !fJustCheck, updates LotteryState and writes the post-block snapshot to evodb.
//
// Called by ProcessSpecialTxsInBlock; exposed separately for unit tests.
bool CheckAndApplyPTXPayout(const CBlock& block,
                             const CBlockIndex* pindex,
                             const CDeterministicGMList& gmList,
                             const PTXPoSeTracker& poseTracker,
                             CValidationState& state,
                             bool fJustCheck) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// Validate given LLMQ final commitment with the list at pindexQuorum
bool VerifyLLMQCommitment(const llmq::CFinalCommitment& qfc, const CBlockIndex* pindexPrev, CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

uint256 CalcTxInputsHash(const CTransaction& tx);

#endif // Hemis_SPECIALTX_H
