// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef Hemis_PTX_WALLET_H
#define Hemis_PTX_WALLET_H

#include "evo/deterministicgms.h"
#include "keystore.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_pose.h"
#include "script/script.h"

#include <string>
#include <vector>

/**
 * Result entry for a wallet-owned GM from the DGM list.
 *
 * KDD-035: my_gms uses payout-key ownership (scriptPTXPayment spendable by
 * this wallet), not operational ownership (collateral / operator / voting key).
 * This answers "which GMs pay me?" not "which GMs do I operate?"
 */
struct WalletGMInfo {
    std::string node_id;
    CScript     payment_script;   // scriptPTXPayment; rendered to Base58Check at JSON time
    int64_t     tickets{0};
    bool        eligible{false};
    int         pose_score{0};
};

/**
 * Filter settlement_history to entries whose winner_script is spendable by ks.
 *
 * Returns entries in the same order they appear in `history` (i.e. the order
 * the caller stored them — ptx_lottery_history reverses to newest-first before
 * emitting JSON). Returns empty vector when history is empty or none match.
 *
 * Intended for wallet RPC: call with *pwallet (CWallet inherits CKeyStore).
 * Testable without CWallet: call with a CBasicKeyStore populated with test keys.
 */
std::vector<LastSettlement> PTX_FilterWalletSettlements(
    const CKeyStore&                    ks,
    const std::vector<LastSettlement>&  history);

/**
 * Filter the DGM list to GMs whose scriptPTXPayment is spendable by ks,
 * and annotate each with current pose-tracker data.
 *
 * A DGM with an empty scriptPTXPayment or whose payment script is not spendable
 * by ks is excluded. Pose-tracker fields default to 0/false if no record exists.
 *
 * See KDD-035 for the payout-ownership vs operational-ownership distinction.
 */
std::vector<WalletGMInfo> PTX_FilterWalletGMs(
    const CKeyStore&         ks,
    const CDeterministicGMList& gmList,
    const PTXPoSeTracker&    tracker);

#endif // Hemis_PTX_WALLET_H
