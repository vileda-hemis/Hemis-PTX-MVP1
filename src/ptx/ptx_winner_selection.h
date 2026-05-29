// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef Hemis_PTX_WINNER_SELECTION_H
#define Hemis_PTX_WINNER_SELECTION_H

#include "evo/deterministicgms.h"
#include "ptx/ptx_pose.h"
#include "script/script.h"
#include "uint256.h"

#include <optional.h>

/**
 * Derive the selection entropy for a PTXPAYOUT at the given settlement block.
 *
 * Formula (ODC-022 §5.3):
 *   entropy = SHA256("PTX-LOTTERY-PAYOUT-" || block_height_uint32_le || block_hash)
 *
 * Single SHA256 (CSHA256), not double. Both inputs are available at
 * ProcessSpecialTxsInBlock time without any BLS operation.
 */
uint256 PTX_ComputeSelectionEntropy(int height, const uint256& blockHash);

/**
 * Select the PTXPAYOUT winner from the eligible GM set.
 *
 * Implements the ODC-022 §5.2 eligibility filter and §5.3 ticket-weighted
 * deterministic selection algorithm. Pure function: no mutation of inputs,
 * deterministic on identical inputs, no global state access.
 *
 * Eligibility (§5.2): DGM must have non-empty node_id (v3+ registration),
 * non-empty scriptPTXPayment, quorum_eligible==true in poseTracker, and
 * lottery_tickets > 0.
 *
 * v1/v2 DGM records (empty node_id) are skipped before touching poseTracker —
 * no tracker entry is created for them (backward compat without tracker pollution).
 *
 * @param gmList         Deterministic GM list at the settlement block's pindexPrev.
 * @param poseTracker    PTX pose tracker providing per-GM eligibility and ticket counts.
 * @param selectionEntropy  32-byte entropy derived via PTX_ComputeSelectionEntropy.
 * @return Winner's scriptPTXPayment, or nullopt if no eligible GMs (rollover).
 */
Optional<CScript> PTX_SelectWinner(const CDeterministicGMList& gmList,
                                    const PTXPoSeTracker& poseTracker,
                                    const uint256& selectionEntropy);

#endif // Hemis_PTX_WINNER_SELECTION_H
