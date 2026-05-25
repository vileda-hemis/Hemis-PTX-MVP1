// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_VALIDATION_INTERFACE_H
#define HEMIS_PTX_VALIDATION_INTERFACE_H

#include "chain.h"
#include "chainparams.h"
#include "logging.h"
#include "validationinterface.h"
#include "ptx/ptx_lottery.h"
#include "ptx/ptx_quorum.h"

class PTXValidationInterface : public CValidationInterface {
public:
    void BlockConnected(const std::shared_ptr<const CBlock>& block,
                        const CBlockIndex* pindex) override {
        if (!pindex || pindex->nHeight <= 0) return;

        // KDD-034: consolidate pool UTXOs every block when count >= 150.
        const std::string con_txid = PTX_ConsolidateLotteryPool(pindex->nHeight);
        if (!con_txid.empty())
            LogPrintf("PTX: pool consolidation at h=%d txid=%s\n", pindex->nHeight, con_txid);

        const int window = Params().PTXSettlementWindow();
        if (pindex->nHeight % window != 0) return;

        // Use the last PTX beacon as settlement randomness (KDD-030).
        const uint256 beacon = PTX_GetLastBeacon();
        const std::string txid = PTX_SettleLotteryWindow(pindex->nHeight, beacon);
        if (!txid.empty())
            LogPrintf("PTX: lottery settled at h=%d txid=%s\n", pindex->nHeight, txid);
    }
};

extern PTXValidationInterface* g_ptx_validation_interface;

#endif // HEMIS_PTX_VALIDATION_INTERFACE_H
