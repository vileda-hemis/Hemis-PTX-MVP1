// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_LOTTERY_H
#define HEMIS_PTX_LOTTERY_H

#include "amount.h"
#include "uint256.h"

#include <string>

// Add service fee to accumulated pool balance (called after each successful PTX tx).
void PTX_AddToPoolBalance(CAmount amount);

// Return current accumulated pool balance (satoshis).
CAmount PTX_GetPoolBalance();

// Select lottery winner node_id from eligible pose records using beacon as entropy.
// Returns "" if no eligible nodes have lottery tickets this window.
// Sorts candidates by node_id (KDD-030: deterministic sort by identity key),
// then maps beacon[0..7] as little-endian uint64 modulo candidate count.
std::string PTX_SelectLotteryWinner(const uint256& beacon);

// Execute settlement at the given block height:
//   1. Select winner from eligible GMs using beacon entropy.
//   2. Fetch a fresh payment address from the winning GM via getnewaddress RPC.
//   3. Build and broadcast a distribution tx paying pool_balance (fee-deducted) to winner.
//   4. Advance the lottery window (reset per-window pose state).
// Resets pool balance only if the distribution tx is successfully relayed.
// Returns txid hex on success, "" if no distribution was made this window.
std::string PTX_SettleLotteryWindow(int height, const uint256& beacon);

#endif // HEMIS_PTX_LOTTERY_H
