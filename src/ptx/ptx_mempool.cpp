// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_mempool.h"

#include "chainparams.h"
#include "ptx/ptx_lottery.h"
#include "consensus/validation.h"
#include "key_io.h"
#include "logging.h"
#include "primitives/transaction.h"
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
                            CProbabilisticTxPayload& payload)
{
#ifndef ENABLE_WALLET
    LogPrintf("PTX: wallet not compiled in, cannot fund PTXSESS transaction\n");
    return "pending";
#else
    if (vpwallets.empty()) {
        LogPrintf("PTX: no wallet available, cannot fund PTXSESS transaction\n");
        return "pending";
    }
    CWallet* pwallet = vpwallets[0];

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTX;

    // vout[0]: OP_RETURN carrying the round seed (zero value, non-standard)
    CScript opret;
    opret << OP_RETURN << ToByteVector(round.round_seed);
    mtx.vout.push_back(CTxOut(0, opret));

    // vout[1]: ptx_service_fee — 1 HMS to the lottery pool (KDD-023 §9)
    const CChainParams& chainparams = Params();
    const CTxDestination poolDest = DecodeDestination(chainparams.PTXLotteryPoolAddress());
    if (!IsValidDestination(poolDest)) {
        LogPrintf("PTX: invalid lottery pool address: %s\n", chainparams.PTXLotteryPoolAddress());
        return "pending";
    }
    mtx.vout.push_back(CTxOut(chainparams.PTXServiceFee(), GetScriptForDestination(poolDest)));

    // KDD-031: pre-size extraPayload with a placeholder caller_address so FundTransaction
    // computes a fee that covers the full final payload. Real address is extracted from
    // the funding UTXOs after FundTransaction and swapped in before signing (same length
    // for P2PKH y-addresses → no fee change on re-embed).
    if (payload.caller_address.empty())
        payload.caller_address = std::string(34, '0');

    // Embed payload before FundTransaction so tx size (and thus fee) is accurate
    SetTxPayload(mtx, payload);

    // Fund: adds UTXOs covering service fee + miner fee, plus change output
    {
        CAmount nFee;
        int nChangePos = -1;
        std::string strFailReason;
        if (!pwallet->FundTransaction(mtx, nFee, false, CFeeRate(0), nChangePos, strFailReason, false, false, {})) {
            LogPrintf("PTX: FundTransaction failed: %s\n", strFailReason);
            return "pending";
        }
    }

    // KDD-031: extract caller_address from first funding UTXO; re-embed payload before signing
    // so the signature covers the complete payload including caller_address.
    {
        LOCK2(cs_main, pwallet->cs_wallet);

        for (const auto& txin : mtx.vin) {
            const Coin& c = pcoinsTip->AccessCoin(txin.prevout);
            if (c.IsSpent()) continue;
            CTxDestination dest;
            if (ExtractDestination(c.out.scriptPubKey, dest)) {
                payload.caller_address = EncodeDestination(dest);
                break;
            }
        }
        if (payload.caller_address.empty())
            throw std::runtime_error("PTX: KDD-031: could not determine caller address from funding UTXOs");
        SetTxPayload(mtx, payload);

        for (unsigned int i = 0; i < mtx.vin.size(); i++) {
            CTxIn& txin = mtx.vin[i];
            const Coin& coin = pcoinsTip->AccessCoin(txin.prevout);
            if (coin.IsSpent()) {
                LogPrintf("PTX: input %d already spent\n", i);
                return "pending";
            }
            const SigVersion sv = mtx.GetRequiredSigVersion();
            txin.scriptSig.clear();
            SignatureData sigdata;
            if (!ProduceSignature(MutableTransactionSignatureCreator(pwallet, &mtx, i, coin.out.nValue, SIGHASH_ALL),
                                  coin.out.scriptPubKey, sigdata, sv, false)) {
                LogPrintf("PTX: signing input %d failed\n", i);
                return "pending";
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
        return "pending";
    } catch (const std::exception& e) {
        LogPrintf("PTX: error: %s\n", e.what());
        return "pending";
    }
#endif
}
