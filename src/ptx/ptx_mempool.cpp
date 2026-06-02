// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_mempool.h"

#include "chainparams.h"
#include "consensus/validation.h"
#include "key_io.h"
#include "logging.h"
#include "primitives/transaction.h"
#include "ptx/ptx_accum_script.h"
#include "rpc/protocol.h"
#include "script/script.h"
#include "script/sign.h"
#include "script/standard.h"
#include "sync.h"
#include "txmempool.h"
#include "validation.h"

#ifdef ENABLE_WALLET
#include "wallet/wallet.h"
#endif

#include <string>
#include <univalue.h>

extern void TryATMP(const CMutableTransaction& mtx, bool fOverrideFees);
extern void RelayTx(const uint256& hashTx);

std::string PTX_AutoCommit(const PTXCommitRevealRound& round,
                            const CProbabilisticTxPayload& payload)
{
#ifndef ENABLE_WALLET
    LogPrintf("PTX: wallet not compiled in, cannot fund PTXSESS transaction\n");
    throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, "wallet not compiled in");
#else
    if (vpwallets.empty()) {
        LogPrintf("PTX: no wallet available, cannot fund PTXSESS transaction\n");
        throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, "no wallet available");
    }
    CWallet* pwallet = vpwallets[0];

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTX;

    // vout[0]: OP_RETURN carrying the round seed (zero value, non-standard)
    CScript opret;
    opret << OP_RETURN << ToByteVector(round.round_seed);
    mtx.vout.push_back(CTxOut(0, opret));

    // vout[1]: 1 HMS service fee to LOTTERY_ACCUM_SCRIPT (ODC-022 §3.3)
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));

    // Embed payload before FundTransaction so tx size (and thus fee) is accurate
    SetTxPayload(mtx, payload);

    // Fund: adds UTXOs covering service fee + miner fee, plus change output.
    // lockUnspents=true: FundTransaction locks each selected UTXO under its own
    // LOCK2(cs_main, cs_wallet), so concurrent rolls cannot select the same UTXO
    // (AvailableCoins skips locked coins). BUG-018 fix.
    {
        CAmount nFee;
        int nChangePos = -1;
        std::string strFailReason;
        if (!pwallet->FundTransaction(mtx, nFee, false, CFeeRate(0), nChangePos, strFailReason, false, true, {})) {
            LogPrintf("PTX: FundTransaction failed: %s\n", strFailReason);
            throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, strFailReason);
        }
    }

    // Release the coin locks set by FundTransaction on any error path below.
    // On success, AddToSpends (triggered when the tx enters the wallet via
    // TransactionAddedToMempool, which TryATMP waits for) clears them automatically.
    auto unlockFundedInputs = [&]() {
        LOCK(pwallet->cs_wallet);
        for (const CTxIn& txin : mtx.vin)
            pwallet->UnlockCoin(txin.prevout);
    };

    // Sign all inputs added by FundTransaction
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        for (unsigned int i = 0; i < mtx.vin.size(); i++) {
            CTxIn& txin = mtx.vin[i];
            const Coin& coin = pcoinsTip->AccessCoin(txin.prevout);
            if (coin.IsSpent()) {
                LogPrintf("PTX: input %d already spent\n", i);
                unlockFundedInputs();
                throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED,
                                   "input " + std::to_string(i) + " already spent");
            }
            const SigVersion sv = mtx.GetRequiredSigVersion();
            txin.scriptSig.clear();
            SignatureData sigdata;
            if (!ProduceSignature(MutableTransactionSignatureCreator(pwallet, &mtx, i, coin.out.nValue, SIGHASH_ALL),
                                  coin.out.scriptPubKey, sigdata, sv, false)) {
                LogPrintf("PTX: signing input %d failed\n", i);
                unlockFundedInputs();
                throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED,
                                   "signing input " + std::to_string(i) + " failed");
            }
            UpdateTransaction(mtx, i, sigdata);
        }
    }

    const uint256 txid = mtx.GetHash();
    try {
        TryATMP(mtx, false);
        RelayTx(txid);
        LogPrintf("PTX: committed and relayed %s\n", txid.GetHex());
        return txid.GetHex();
    } catch (const UniValue& objError) {
        LogPrintf("PTX: mempool rejected: %s\n", objError["message"].getValStr());
        unlockFundedInputs();
        throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED,
                           "mempool rejected: " + objError["message"].getValStr());
    } catch (const std::exception& e) {
        LogPrintf("PTX: error: %s\n", e.what());
        unlockFundedInputs();
        throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, e.what());
    }
#endif
}
