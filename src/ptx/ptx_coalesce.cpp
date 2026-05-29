// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx_coalesce.h"

#include "primitives/transaction.h"
#include "ptx/ptx_accum_script.h"
#include "script/script.h"

CTransactionRef PTX_BuildCoalesceTx(
    const COutPoint&               priorOutpoint,
    CAmount                        priorValue,
    const std::vector<AccumInput>& newFeeInputs)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXCOALESCE;

    CAmount total = 0;

    // First input: prior accumulator UTXO (omitted when no accumulator exists yet).
    if (!priorOutpoint.IsNull()) {
        mtx.vin.emplace_back(CTxIn(priorOutpoint));
        total += priorValue;
    }

    // Remaining inputs: PTXSESS fee outputs from this block, in block order.
    // Relies on Step 5 invariant: every PTXSESS carries exactly nPTXServiceFee to
    // LOTTERY_ACCUM_SCRIPT (specialtx_validation.cpp, ptx-bad-accum-output rule).
    // If that rule changes, this arithmetic must be revisited.
    for (const AccumInput& inp : newFeeInputs) {
        mtx.vin.emplace_back(CTxIn(inp.outpoint));
        total += inp.value;
    }

    // Single output: all accumulated value to LOTTERY_ACCUM_SCRIPT, zero miner fee (C4).
    mtx.vout.emplace_back(CTxOut(total, GetLotteryAccumScript()));

    // extraPayload: present-but-empty (NOT nullopt — absent payload triggers BUG-014 cap).
    mtx.extraPayload.emplace();

    // All input scriptSigs remain default-empty (consensus VerifyScript exemption, C6).
    return MakeTransactionRef(std::move(mtx));
}

std::vector<AccumInput> PTX_CollectPTXSESSFeeOutputs(
    const std::vector<CTransactionRef>& vtx)
{
    const CScript& accumScript = GetLotteryAccumScript();
    std::vector<AccumInput> result;

    for (const CTransactionRef& tx : vtx) {
        if (!tx->IsProbabilisticTx()) continue;
        for (uint32_t i = 0; i < tx->vout.size(); ++i) {
            if (tx->vout[i].scriptPubKey == accumScript) {
                result.push_back({COutPoint(tx->GetHash(), i), tx->vout[i].nValue});
                break;  // exactly one per PTXSESS (Step 5 enforces this invariant)
            }
        }
    }
    return result;
}
