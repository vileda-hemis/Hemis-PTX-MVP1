// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_MEMPOOL_H
#define HEMIS_PTX_MEMPOOL_H

#include "primitives/transaction.h"
#include "ptx/ptx_commit_reveal.h"

#include <string>

// Build and submit a PTX special transaction to the memory pool.
// Returns the txid hex on acceptance, or "pending" if mempool rejected.
// KDD-027: called immediately after TryResolve — no delay.
std::string PTX_AutoCommit(const PTXCommitRevealRound& round,
                            const CProbabilisticTxPayload& payload);

#endif // HEMIS_PTX_MEMPOOL_H
