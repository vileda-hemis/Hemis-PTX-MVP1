// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_payout.h"

#include "chainparams.h"
#include "logging.h"
#include "ptx/ptx_accum_script.h"
#include "ptx/ptx_winner_selection.h"

Optional<CTransactionRef> PTX_BuildPayoutTx(
    const LotteryState&         ls,
    const CDeterministicGMList& gmList,
    const PTXPoSeTracker&       poseTracker,
    int                         blockHeight,
    const uint256&              prevBlockHash)
{
    if (ls.accumulator_outpoint.IsNull()) {
        return nullopt;
    }

    const CAmount minerFee = Params().PTXPayoutMinerFee();
    if (ls.accumulator_value < minerFee) {
        LogPrintf("PTX: PTX_BuildPayoutTx h=%d: accumulator_value=%lld < minerFee=%lld, skipping payout\n",
                  blockHeight, (long long)ls.accumulator_value, (long long)minerFee);
        return nullopt;
    }

    const uint256 entropy = PTX_ComputeSelectionEntropy(blockHeight, prevBlockHash);
    const Optional<CScript> winner = PTX_SelectWinner(gmList, poseTracker, entropy);
    if (!winner) {
        return nullopt;
    }

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXPAYOUT;

    CTxIn txin(ls.accumulator_outpoint);
    // scriptSig must be empty — exempted from VerifyScript by consensus (P7).
    txin.scriptSig.clear();
    mtx.vin.push_back(txin);

    mtx.vout.push_back(CTxOut(ls.accumulator_value - minerFee, *winner));

    // extraPayload present-but-empty (avoids BUG-014 cap; load-bearing for P6).
    mtx.extraPayload.emplace();

    return MakeTransactionRef(CTransaction(mtx));
}
