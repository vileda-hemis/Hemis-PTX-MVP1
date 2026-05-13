// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_VALIDATION_INTERFACE_H
#define HEMIS_PTX_VALIDATION_INTERFACE_H

#include "chain.h"
#include "validationinterface.h"
#include "ptx/ptx_pose.h"

class PTXValidationInterface : public CValidationInterface {
public:
    void BlockConnected(const std::shared_ptr<const CBlock>& block,
                        const CBlockIndex* pindex) override {
        if (pindex && pindex->nHeight % 1440 == 0)
            g_ptx_pose_tracker.AdvanceLotteryWindow();
    }
};

extern PTXValidationInterface* g_ptx_validation_interface;

#endif // HEMIS_PTX_VALIDATION_INTERFACE_H
