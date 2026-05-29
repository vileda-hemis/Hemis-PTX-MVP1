// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef Hemis_PTX_PAYOUT_H
#define Hemis_PTX_PAYOUT_H

#include "evo/deterministicgms.h"
#include "optional.h"
#include "primitives/transaction.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_pose.h"
#include "uint256.h"

/**
 * Build a PTXPAYOUT transaction (nType=10, ODC-022 §3.5).
 *
 * Pure generator: no global state mutation.  Calls PTX_SelectWinner with
 * entropy derived from prevBlockHash (the parent block's hash — not the
 * current block's hash, which would be circular).
 *
 * Returns nullopt in three legitimate cases:
 *   1. No eligible winner (§5.4 rollover).
 *   2. ls.accumulator_outpoint.IsNull() — no accumulator to spend.
 *   3. ls.accumulator_value < Params().PTXPayoutMinerFee() — payout would be
 *      non-positive.  Logged as an explicit warning; should not occur on a
 *      correctly-operating chain.
 *
 * The returned transaction satisfies all Step 8 rules (P1–P10) and the
 * Step 9 P11 generator contract by construction — both generator and
 * validator call PTX_SelectWinner with the same entropy.
 */
Optional<CTransactionRef> PTX_BuildPayoutTx(
    const LotteryState&         ls,
    const CDeterministicGMList& gmList,
    const PTXPoSeTracker&       poseTracker,
    int                         blockHeight,
    const uint256&              prevBlockHash);

#endif // Hemis_PTX_PAYOUT_H
