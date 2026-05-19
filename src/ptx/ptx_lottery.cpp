// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_lottery.h"

#include "chainparams.h"
#include "consensus/validation.h"
#include "key_io.h"
#include "logging.h"
#include "primitives/transaction.h"
#include "ptx/ptx_fanout.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_quorum.h"
#include "script/sign.h"
#include "script/standard.h"
#include "sync.h"
#include "txmempool.h"
#include "validation.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <algorithm>
#include <set>
#include <string>
#include <univalue.h>

// RelayTx is defined in rpc/rawtransaction.cpp; no public header.
extern void RelayTx(const uint256& hashTx);

static RecursiveMutex cs_ptx_pool_balance;
static CAmount g_ptx_pool_balance{0};

void PTX_AddToPoolBalance(CAmount amount)
{
    LOCK(cs_ptx_pool_balance);
    g_ptx_pool_balance += amount;
    LogPrintf("PTX: lottery pool +%d sat → %d sat\n", amount, g_ptx_pool_balance);
}

CAmount PTX_GetPoolBalance()
{
    LOCK(cs_ptx_pool_balance);
    return g_ptx_pool_balance;
}

std::string PTX_SelectLotteryWinner(const uint256& beacon)
{
    auto all_records = g_ptx_pose_tracker.GetAllRecords();

    // Collect eligible GMs that have lottery tickets this window.
    std::vector<std::string> candidates;
    for (const auto& kv : all_records) {
        if (kv.second.quorum_eligible && kv.second.lottery_tickets > 0)
            candidates.push_back(kv.first);
    }
    if (candidates.empty()) return "";

    // Deterministic sort by node_id (KDD-030: sort by identity key).
    std::sort(candidates.begin(), candidates.end());

    // Derive winner index from first 8 bytes of beacon (little-endian uint64).
    uint64_t rnd = 0;
    for (int i = 0; i < 8; i++)
        rnd |= static_cast<uint64_t>(beacon.begin()[i]) << (8 * i);
    size_t winner_idx = static_cast<size_t>(rnd % candidates.size());

    LogPrintf("PTX: lottery winner selected: %s (idx=%zu/%zu candidates)\n",
              candidates[winner_idx], winner_idx, candidates.size());
    return candidates[winner_idx];
}

std::string PTX_SettleLotteryWindow(int height, const uint256& beacon)
{
    CAmount pool_balance;
    {
        LOCK(cs_ptx_pool_balance);
        pool_balance = g_ptx_pool_balance;
    }

    if (pool_balance <= 0) {
        LogPrintf("PTX: lottery window h=%d: pool empty, no distribution\n", height);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    std::string winner_id = PTX_SelectLotteryWinner(beacon);
    if (winner_id.empty()) {
        LogPrintf("PTX: lottery window h=%d: no eligible nodes, no distribution\n", height);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    // Find the winner's node info for RPC fanout.
    const PTXNodeInfo* winner_node = nullptr;
    for (const auto& ni : g_ptx_nodes) {
        if (ni.node_id == winner_id) { winner_node = &ni; break; }
    }
    if (!winner_node) {
        LogPrintf("PTX: lottery winner %s not in node list, skipping\n", winner_id);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    // Ask the winner for a fresh payment address.
    UniValue gna_params(UniValue::VARR);
    gna_params.push_back("ptx-reward");
    PTXRpcResponse addr_resp = PTX_CallNodeRpc(*winner_node, "getnewaddress", gna_params);

    std::string winner_addr;
    if (!addr_resp.body.empty()) {
        UniValue resp_obj;
        if (resp_obj.read(addr_resp.body)) {
            const UniValue& res_val = resp_obj["result"];
            if (res_val.isStr()) winner_addr = res_val.get_str();
        }
    }
    if (winner_addr.empty()) {
        LogPrintf("PTX: lottery winner %s RPC failed or offline, deferring\n", winner_id);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    LogPrintf("PTX: lottery window h=%d: distributing %d sat to %s (%s)\n",
              height, pool_balance, winner_id, winner_addr);

    std::string txid;

#ifdef ENABLE_WALLET
    if (vpwallets.empty()) {
        LogPrintf("PTX: no wallet available for lottery distribution\n");
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }
    CWallet* pwallet = vpwallets[0];

    const CTxDestination winner_dest = DecodeDestination(winner_addr);
    if (!IsValidDestination(winner_dest)) {
        LogPrintf("PTX: lottery: invalid winner address: %s\n", winner_addr);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::NORMAL;

    // vout[0]: pool_balance to winner; miner fee deducted from this output.
    mtx.vout.push_back(CTxOut(pool_balance, GetScriptForDestination(winner_dest)));

    {
        CAmount nFee;
        int nChangePos = -1;
        std::string strFailReason;
        // Deduct miner fee from vout[0] so distribution is self-contained.
        const std::set<int> subtractFee = {0};
        if (!pwallet->FundTransaction(mtx, nFee, false, CFeeRate(0), nChangePos,
                                      strFailReason, false, false, subtractFee)) {
            LogPrintf("PTX: lottery FundTransaction failed: %s\n", strFailReason);
            g_ptx_pose_tracker.AdvanceLotteryWindow();
            return "";
        }
    }

    {
        LOCK2(cs_main, pwallet->cs_wallet);
        for (unsigned int i = 0; i < mtx.vin.size(); i++) {
            CTxIn& txin = mtx.vin[i];
            const Coin& coin = pcoinsTip->AccessCoin(txin.prevout);
            if (coin.IsSpent()) {
                LogPrintf("PTX: lottery signing: input %d spent\n", i);
                g_ptx_pose_tracker.AdvanceLotteryWindow();
                return "";
            }
            txin.scriptSig.clear();
            SignatureData sigdata;
            const SigVersion sv = mtx.GetRequiredSigVersion();
            if (!ProduceSignature(
                    MutableTransactionSignatureCreator(pwallet, &mtx, i, coin.out.nValue, SIGHASH_ALL),
                    coin.out.scriptPubKey, sigdata, sv, false)) {
                LogPrintf("PTX: lottery signing: input %d failed\n", i);
                g_ptx_pose_tracker.AdvanceLotteryWindow();
                return "";
            }
            UpdateTransaction(mtx, i, sigdata);
        }
    }

    // AcceptToMemoryPool acquires cs_main internally.
    // We do NOT use TryATMP here: TryATMP's promise-wait pattern deadlocks when
    // called from the CValidationInterface scheduler thread (BlockConnected).
    const uint256 settle_txid = mtx.GetHash();
    {
        CValidationState state;
        bool fMissingInputs = false;
        if (!AcceptToMemoryPool(mempool, state, MakeTransactionRef(CTransaction(mtx)),
                                /*fLimitFree=*/true, &fMissingInputs,
                                /*fOverrideMempoolLimit=*/false, /*nAbsurdFee=*/false)) {
            const std::string reason = fMissingInputs ? "missing inputs" : state.GetRejectReason();
            LogPrintf("PTX: lottery settle rejected: %s %s\n", reason, state.GetDebugMessage());
        } else {
            RelayTx(settle_txid);
            txid = settle_txid.GetHex();
            LogPrintf("PTX: lottery settled h=%d winner=%s addr=%s pool=%d sat txid=%s\n",
                      height, winner_id, winner_addr, pool_balance, txid);
            LOCK(cs_ptx_pool_balance);
            g_ptx_pool_balance = 0;
        }
    }
#else
    LogPrintf("PTX: wallet not compiled in, cannot distribute lottery\n");
#endif

    g_ptx_pose_tracker.AdvanceLotteryWindow();
    return txid;
}
