// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_lottery.h"

#include "chainparams.h"
#include "coins.h"
#include "consensus/validation.h"
#include "evo/deterministicgms.h"
#include "txdb.h"
#include "key_io.h"
#include "logging.h"
#include "primitives/transaction.h"
#include "ptx/ptx_fanout.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_quorum.h"
#include "script/standard.h"
#include "sync.h"
#include "txmempool.h"
#include "validation.h"

#include <algorithm>
#include <memory>
#include <string>
#include <univalue.h>
#include <vector>

// RelayTx is defined in rpc/rawtransaction.cpp; no public header.
extern void RelayTx(const uint256& hashTx);

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

std::string PTX_SettleLotteryWindow(int height, const uint256& /*beacon*/)
{
    const CChainParams& chainparams = Params();
    const std::string pool_addr_str = chainparams.PTXLotteryPoolAddress();
    const CTxDestination pool_dest = DecodeDestination(pool_addr_str);
    if (!IsValidDestination(pool_dest)) {
        LogPrintf("PTX: lottery: invalid pool address: %s\n", pool_addr_str);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }
    const CScript pool_script = GetScriptForDestination(pool_dest);

    // Enumerate all confirmed pool UTXOs from the on-chain UTXO set.
    // Cap at 1000 to bound tx size; remainder carries to the next window.
    struct PoolCoin { COutPoint outpoint; CAmount value; };
    std::vector<PoolCoin> pool_coins;
    CAmount total_input = 0;
    uint256 beacon_hash;

    {
        LOCK(cs_main);
        // Flush so pcoinsdbview reflects all confirmed coins.
        FlushStateToDisk();

        // Derive beacon from block hash at settlement height (prevents grinding).
        CBlockIndex* pindex_settle = chainActive[height];
        if (!pindex_settle) {
            LogPrintf("PTX: lottery window h=%d: block index not available\n", height);
            g_ptx_pose_tracker.AdvanceLotteryWindow();
            return "";
        }
        beacon_hash = pindex_settle->GetBlockHash();

        std::unique_ptr<CCoinsViewCursor> pcursor(pcoinsdbview->Cursor());
        while (pcursor->Valid()) {
            if (pool_coins.size() >= 1000) break;
            COutPoint key;
            Coin coin;
            if (pcursor->GetKey(key) && pcursor->GetValue(coin)) {
                if (coin.out.scriptPubKey == pool_script) {
                    pool_coins.push_back({key, coin.out.nValue});
                    total_input += coin.out.nValue;
                }
            }
            pcursor->Next();
        }
    }

    if (pool_coins.empty()) {
        LogPrintf("PTX: lottery window h=%d: pool empty, no distribution\n", height);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    // Select winner using beacon derived from chain (not caller-supplied beacon).
    std::string winner_id = PTX_SelectLotteryWinner(beacon_hash);
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

    // ODC-020: use the registered PTX payment address if the GM set one.
    std::string winner_addr;
    {
        uint256 proTxHash;
        proTxHash.SetHex(winner_id);
        LOCK(deterministicGMManager->cs);
        auto gmList = deterministicGMManager->GetListAtChainTip();
        auto dgm = gmList.GetGM(proTxHash);
        if (dgm && !dgm->pdgmState->scriptPTXPayment.empty()) {
            CTxDestination dest;
            if (ExtractDestination(dgm->pdgmState->scriptPTXPayment, dest))
                winner_addr = EncodeDestination(dest);
        }
    }

    if (winner_addr.empty()) {
        // Fallback: ask node for a fresh address (deprecated — GMs should register scriptPTXPayment).
        LogPrintf("PTX: lottery winner %s has no registered PTX payment address, falling back to getnewaddress\n", winner_id);
        UniValue gna_params(UniValue::VARR);
        gna_params.push_back("ptx-reward");
        PTXRpcResponse addr_resp = PTX_CallNodeRpc(*winner_node, "getnewaddress", gna_params);
        if (!addr_resp.body.empty()) {
            UniValue resp_obj;
            if (resp_obj.read(addr_resp.body)) {
                const UniValue& res_val = resp_obj["result"];
                if (res_val.isStr()) winner_addr = res_val.get_str();
            }
        }
    }

    if (winner_addr.empty()) {
        LogPrintf("PTX: lottery winner %s RPC failed or offline, deferring\n", winner_id);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    const CTxDestination winner_dest = DecodeDestination(winner_addr);
    if (!IsValidDestination(winner_dest)) {
        LogPrintf("PTX: lottery: invalid winner address: %s\n", winner_addr);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    // Estimate miner fee: P2PKH input ~150 bytes, P2PKH output 34 bytes, overhead 20.
    const unsigned int tx_bytes = 20 + (unsigned int)pool_coins.size() * 150 + 34;
    const CAmount fee = ::minRelayTxFee.GetFee(tx_bytes);
    const CAmount winner_amount = total_input - fee;
    if (winner_amount <= 0) {
        LogPrintf("PTX: lottery window h=%d: fee %lld >= pool total %lld sat, skipping\n",
                  height, (long long)fee, (long long)total_input);
        g_ptx_pose_tracker.AdvanceLotteryWindow();
        return "";
    }

    LogPrintf("PTX: lottery window h=%d: distributing %lld sat (%zu inputs, fee %lld) to %s (%s)\n",
              height, (long long)total_input, pool_coins.size(),
              (long long)fee, winner_id, winner_addr);

    // Build CPTXSettlePayload.
    CPTXSettlePayload payload;
    payload.settlement_height = (uint64_t)height;
    payload.winner_node_id   = winner_id;
    payload.pool_balance_sat = (uint64_t)total_input;
    payload.beacon_hash      = beacon_hash;

    // Build PTXSETTLE transaction. Pool inputs are valid by payload correctness
    // (Rule 8: scriptSig exempt — no private key exists for the pool address).
    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXSETTLE;
    for (const auto& pc : pool_coins)
        mtx.vin.push_back(CTxIn(pc.outpoint, CScript(), CTxIn::SEQUENCE_FINAL));
    mtx.vout.push_back(CTxOut(winner_amount, GetScriptForDestination(winner_dest)));
    SetTxPayload(mtx, payload);

    // AcceptToMemoryPool acquires cs_main internally.
    // We do NOT use TryATMP here: TryATMP's promise-wait pattern deadlocks when
    // called from the CValidationInterface scheduler thread (BlockConnected).
    const uint256 settle_txid = mtx.GetHash();
    {
        CValidationState val_state;
        bool fMissingInputs = false;
        if (!AcceptToMemoryPool(mempool, val_state, MakeTransactionRef(CTransaction(mtx)),
                                /*fLimitFree=*/true, &fMissingInputs,
                                /*fOverrideMempoolLimit=*/false, /*nAbsurdFee=*/false)) {
            const std::string reason = fMissingInputs ? "missing inputs" : val_state.GetRejectReason();
            LogPrintf("PTX: lottery settle rejected: %s %s\n", reason, val_state.GetDebugMessage());
            g_ptx_pose_tracker.AdvanceLotteryWindow();
            return "";
        }
    }

    RelayTx(settle_txid);
    const std::string txid_str = settle_txid.GetHex();
    LogPrintf("PTX: lottery settled h=%d winner=%s addr=%s pool=%lld sat txid=%s\n",
              height, winner_id, winner_addr, (long long)total_input, txid_str);

    g_ptx_pose_tracker.AdvanceLotteryWindow();
    return txid_str;
}
