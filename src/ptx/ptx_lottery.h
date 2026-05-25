// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_LOTTERY_H
#define HEMIS_PTX_LOTTERY_H

#include "serialize.h"
#include "uint256.h"

#include <cstdint>
#include <string>

struct CPTXSettlePayload {
    uint64_t settlement_height{0};  // block height at which this settlement fires
    std::string winner_node_id;     // eligible GM node ID selected by beacon
    uint64_t pool_balance_sat{0};   // total pool input value (sum of all pool UTXOs spent)
    uint256 beacon_hash;            // block hash at settlement_height

    SERIALIZE_METHODS(CPTXSettlePayload, obj)
    {
        READWRITE(obj.settlement_height, obj.winner_node_id,
                  obj.pool_balance_sat, obj.beacon_hash);
    }
};

// Select lottery winner node_id from eligible pose records using beacon as entropy.
// Returns "" if no eligible nodes have lottery tickets this window.
// Sorts candidates by node_id (KDD-030: deterministic sort by identity key),
// then maps beacon[0..7] as little-endian uint64 modulo candidate count.
std::string PTX_SelectLotteryWinner(const uint256& beacon);

// Execute settlement at the given block height:
//   1. Enumerate all pool UTXOs from UTXO set (capped at 1000).
//   2. Derive beacon from chainActive[height]->GetBlockHash().
//   3. Select winner from eligible GMs using beacon entropy.
//   4. Fetch a fresh payment address from the winning GM via getnewaddress RPC.
//   5. Build and broadcast a PTXSETTLE tx paying pool total (fee-deducted) to winner.
// Returns txid hex on success, "" if no distribution was made this window.
std::string PTX_SettleLotteryWindow(int height, const uint256& beacon);

#endif // HEMIS_PTX_LOTTERY_H
