// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_mempool.h"

#include "consensus/validation.h"
#include "logging.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "script/standard.h"
#include "sync.h"
#include "txmempool.h"
#include "validation.h"

#include <string>

std::string PTX_AutoCommit(const PTXCommitRevealRound& round,
                            const CProbabilisticTxPayload& payload)
{
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTX;
    SetTxPayload(mtx, payload);

    CScript opret;
    opret << OP_RETURN << ToByteVector(round.round_seed);
    mtx.vout.push_back(CTxOut(0, opret));

    // Capture txid before std::move(mtx) consumes the fields.
    CTransaction tx_read(mtx);
    std::string txid = tx_read.GetHash().GetHex();

    CValidationState state;
    bool fMissingInputs = false;
    bool fLimitFree     = false;
    bool fOverrideFees  = false;
    {
        LOCK(cs_main);
        bool success = AcceptToMemoryPool(mempool, state,
                                          MakeTransactionRef(std::move(mtx)),
                                          fLimitFree, &fMissingInputs, false,
                                          !fOverrideFees);
        if (success) {
            LogPrintf("PTX: committed %s\n", txid);
            return txid;
        }
        LogPrintf("PTX: mempool rejected: %s\n", state.GetRejectReason());
        return "pending";
    }
}
