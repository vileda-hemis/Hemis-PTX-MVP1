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
#include "ptx/ptx_bls.h"
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

#include <set>
#include <string>
#include <univalue.h>
#include <utility>

extern void TryATMP(const CMutableTransaction& mtx, bool fOverrideFees);
extern void RelayTx(const uint256& hashTx);

#ifdef ENABLE_WALLET
// Non-dust chain-output value; reclaimed by the settle's change, so nothing leaks.
static const CAmount PTX_CHAIN_OUTPUT_VALUE = 100000;   // 0.001 HMS
#endif

std::string PTX_BuildRollCommitment(const CPTXRollCommitPayload& payload,
                                    COutPoint& out_chain)
{
#ifndef ENABLE_WALLET
    throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, "wallet not compiled in");
#else
    if (vpwallets.empty())
        throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, "no wallet available");
    CWallet* pwallet = vpwallets[0];

    CMutableTransaction mtx;
    mtx.nVersion = CTransaction::TxVersion::SAPLING;
    mtx.nType    = CTransaction::TxType::PTXROLLCOMMIT;

    // vout[0]: the RELOCATED service fee to LOTTERY_ACCUM_SCRIPT — the payment,
    // forfeited at commit (before the result is knowable): this closes the preview.
    mtx.vout.push_back(CTxOut(Params().PTXServiceFee(), GetLotteryAccumScript()));

    // vout[1]: the purpose-built chain output the settle spends (the 2c coin-chain).
    // A fresh wallet key (so the settle can sign it), non-dust; value reclaimed by
    // the settle's change.
    CReserveKey reservekey(pwallet);
    CPubKey chainPubKey;
    if (!reservekey.GetReservedKey(chainPubKey, true))
        throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, "keypool exhausted (chain output)");
    const CScript chainScript = GetScriptForDestination(chainPubKey.GetID());
    mtx.vout.push_back(CTxOut(PTX_CHAIN_OUTPUT_VALUE, chainScript));

    SetTxPayload(mtx, payload);

    {
        CAmount nFee; int nChangePos = -1; std::string strFailReason;
        if (!pwallet->FundTransaction(mtx, nFee, false, CFeeRate(0), nChangePos,
                                      strFailReason, false, true, {}))
            throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED,
                               "commitment FundTransaction failed: " + strFailReason);
    }

    auto unlockFundedInputs = [&]() {
        LOCK(pwallet->cs_wallet);
        for (const CTxIn& txin : mtx.vin) pwallet->UnlockCoin(txin.prevout);
    };

    // FundTransaction may insert change, shifting indices — locate the chain output
    // by its unique reservekey script, never a fixed index.
    int chainIdx = -1;
    for (size_t i = 0; i < mtx.vout.size(); i++)
        if (mtx.vout[i].scriptPubKey == chainScript &&
            mtx.vout[i].nValue == PTX_CHAIN_OUTPUT_VALUE) { chainIdx = (int)i; break; }
    if (chainIdx < 0) {
        unlockFundedInputs();
        throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, "chain output vanished after funding");
    }

    // Sign funding inputs (all confirmed → pcoinsTip).
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        for (unsigned int i = 0; i < mtx.vin.size(); i++) {
            CTxIn& txin = mtx.vin[i];
            const Coin& coin = pcoinsTip->AccessCoin(txin.prevout);
            if (coin.IsSpent()) {
                unlockFundedInputs();
                throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, "commitment input already spent");
            }
            const SigVersion sv = mtx.GetRequiredSigVersion();
            txin.scriptSig.clear();
            SignatureData sigdata;
            if (!ProduceSignature(MutableTransactionSignatureCreator(pwallet, &mtx, i, coin.out.nValue, SIGHASH_ALL),
                                  coin.out.scriptPubKey, sigdata, sv, false)) {
                unlockFundedInputs();
                throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, "commitment signing failed");
            }
            UpdateTransaction(mtx, i, sigdata);
        }
    }

    const uint256 txid = mtx.GetHash();
    try {
        TryATMP(mtx, false);
        RelayTx(txid);
        reservekey.KeepKey();   // the chain key is now in use (the settle spends it)
        out_chain = COutPoint(txid, (uint32_t)chainIdx);
        LogPrintf("PTX: commitment %s broadcast; chain output %s:%d\n",
                  txid.GetHex(), txid.GetHex(), chainIdx);
        return txid.GetHex();
    } catch (const UniValue& objError) {
        unlockFundedInputs();
        throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED,
                           "commitment mempool rejected: " + objError["message"].getValStr());
    } catch (const std::exception& e) {
        unlockFundedInputs();
        throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED, std::string("commitment error: ") + e.what());
    }
#endif
}

std::string PTX_AutoCommit(const PTXCommitRevealRound& round,
                            const CProbabilisticTxPayload& payload,
                            const COutPoint& chain_input)
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

    // BUG-032 2b-iii: NO accum fee output — the fee relocated to the commitment.
    // Coin-chain: spend the commitment's chain output. Pre-adding it to vin makes
    // FundTransaction FORCE-select it (coinControl.Select) — deterministic, never
    // left to coin selection — while adding any additional funding + change.
    mtx.vin.push_back(CTxIn(chain_input));

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

    // Sign all inputs. The coin-chain input spends the commitment's UNCONFIRMED
    // output, so resolve coins through a mempool-aware view (pcoinsTip alone would
    // not find it) — the standard spending-unconfirmed-chained-output pattern.
    {
        LOCK2(cs_main, mempool.cs);
        LOCK(pwallet->cs_wallet);
        CCoinsViewMemPool viewMemPool(pcoinsTip.get(), mempool);
        CCoinsViewCache view(&viewMemPool);
        for (unsigned int i = 0; i < mtx.vin.size(); i++) {
            CTxIn& txin = mtx.vin[i];
            const Coin& coin = view.AccessCoin(txin.prevout);
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

// ===========================================================================
// BUG-032 (Option A, fund-then-sign): the payment-before-reveal gate.
// ===========================================================================
// PTX_RollCommitmentPresent — the REAL commitment lookup (2b). Scans the local
// mempool for a PTXROLLCOMMIT (nType=12) whose payload names this exact
// (round_seed, quorum_hash). This replaces increment 1's in-memory registry
// seam: the commitment is now a real, funded, broadcast transaction, and the
// signing decision reads the same mempool the rest of the node does. With the
// same-block mandate (nExpiryHeight == nSeedHeight) the commitment sits in
// mempool at sign time — settle rides its block — so the mempool scan is the
// authoritative signing-time check; a chain scan is unnecessary for the
// same-block flow and would only matter if a window > 0 were ever allowed.
bool PTX_RollCommitmentPresent(const uint256& round_seed, const uint256& quorum_hash)
{
    LOCK(mempool.cs);
    for (const auto& e : mempool.mapTx) {
        const CTransactionRef tx = e.GetSharedTx();
        if (!tx->IsPTXRollCommitTx()) continue;
        CPTXRollCommitPayload p;
        if (!GetTxPayload(*tx, p)) continue;
        if (p.round_seed == round_seed && p.quorum_hash == quorum_hash)
            return true;
    }
    return false;
}

// The gated signing entry. Signs round_seed with this node's CURRENT share for
// quorum_hash — but ONLY if a funded commitment for the exact pair is present.
// Fund-then-sign (Option A): a roll's signature IS its result, so it must not be
// produced until payment for THIS round_seed under THIS canonical quorum_hash is
// irrevocably committed. The gate closes BUG-032 (free preview/reroll) and, by
// binding quorum_hash, BUG-033 (quorum-shop: a sign request naming a non-committed
// quorum finds no commitment and is refused). Signing logic mirrors gm_bls_sign
// (rpc/ptx.cpp) minus the RPC/JSON; that RPC path routes through here.
bool PTX_SignRoundIfCommitted(const uint256& round_seed, const uint256& quorum_hash,
                              uint8_t out_sig[96], std::string& err,
                              bool* out_retryable)
{
    if (out_retryable) *out_retryable = false;
    // ── BUG-032 GATE: refuse to reveal (sign) before payment is committed.
    if (!PTX_RollCommitmentPresent(round_seed, quorum_hash)) {
        // ★ 2b-ii wait-not-reject: absence is NOT proof the commitment was never
        // made — it may still be in flight (propagation delay). The member cannot
        // distinguish "not yet arrived" from "never broadcast", so it defaults to
        // RETRYABLE and lets the coordinator's retry budget bound the wait. A hard
        // reject here would fail legitimate rolls under ordinary network delay.
        if (out_retryable) *out_retryable = true;
        err = "no commitment seen yet for round_seed " + round_seed.ToString() +
              " under quorum " + quorum_hash.ToString() + " — retry after propagation";
        return false;
    }

    uint8_t sk_bytes[32];
    if (!PTX_BLS_GetCurrentShare(quorum_hash, sk_bytes)) {
        err = "no CURRENT sk_share held for quorum " + quorum_hash.ToString();
        return false;
    }
    if (!PTX_BLS_PartialSign(sk_bytes, round_seed, out_sig)) {
        err = "BLS partial signing failed";
        return false;
    }
    return true;
}
