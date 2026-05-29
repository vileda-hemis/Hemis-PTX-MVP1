// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef Hemis_PTX_COALESCE_H
#define Hemis_PTX_COALESCE_H

#include "amount.h"
#include "primitives/transaction.h"

#include <vector>

/** One LOTTERY_ACCUM_SCRIPT input to a PTXCOALESCE: its outpoint and value. */
struct AccumInput {
    COutPoint outpoint;
    CAmount   value;
};

/**
 * Build a PTXCOALESCE transaction (nType=9, ODC-022 §3.4).
 *
 * Inputs (in order):
 *   [1] prior accumulator UTXO — included iff priorOutpoint.IsNull() == false
 *   [2..] new PTXSESS fee outputs from this block, in block order
 *
 * Output: single LOTTERY_ACCUM_SCRIPT UTXO, value = sum of all inputs (zero fee, C4).
 * scriptSig: empty on all inputs (consensus-granted VerifyScript exemption, C6).
 * extraPayload: present-but-empty (NOT nullopt; avoids BUG-014 cap).
 *
 * priorOutpoint: outpoint of the existing accumulator UTXO.  Pass COutPoint()
 *               (IsNull() == true) when no accumulator exists yet.
 * priorValue:   value of the existing accumulator (0 when no accumulator).
 * newFeeInputs: LOTTERY_ACCUM_SCRIPT outputs from PTXSESS txs in this block.
 */
CTransactionRef PTX_BuildCoalesceTx(
    const COutPoint&               priorOutpoint,
    CAmount                        priorValue,
    const std::vector<AccumInput>& newFeeInputs);

/**
 * Scan vtx for PTXSESS (IsProbabilisticTx) transactions and return their
 * LOTTERY_ACCUM_SCRIPT fee outputs as AccumInput{outpoint, value} in block order.
 *
 * Called by both the block template builder (blockassembler.cpp) and the
 * block-level structural check in ProcessSpecialTxsInBlock.
 */
std::vector<AccumInput> PTX_CollectPTXSESSFeeOutputs(
    const std::vector<CTransactionRef>& vtx);

#endif // Hemis_PTX_COALESCE_H
