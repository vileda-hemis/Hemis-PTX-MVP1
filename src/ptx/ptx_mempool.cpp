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

// ===========================================================================
// BUG-032 (Option A, fund-then-sign): the payment-before-reveal gate.
// ===========================================================================
// In-memory registry of funded roll commitments, keyed (round_seed, quorum_hash).
// This is the gatable seam PTX_SignRoundIfCommitted consults. The full fix wires
// it to the real commitment tx (new nType) seen in mempool/chain; until that tx
// type lands, callers register a commitment here once the funded tx is broadcast.
namespace {
RecursiveMutex cs_ptx_roll_commit;
std::set<std::pair<uint256, uint256>> g_ptx_roll_commitments GUARDED_BY(cs_ptx_roll_commit);
} // anonymous namespace

void PTX_RegisterRollCommitment(const uint256& round_seed, const uint256& quorum_hash)
{
    LOCK(cs_ptx_roll_commit);
    g_ptx_roll_commitments.insert(std::make_pair(round_seed, quorum_hash));
}

bool PTX_RollCommitmentPresent(const uint256& round_seed, const uint256& quorum_hash)
{
    LOCK(cs_ptx_roll_commit);
    return g_ptx_roll_commitments.count(std::make_pair(round_seed, quorum_hash)) > 0;
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
                              uint8_t out_sig[96], std::string& err)
{
    // ── BUG-032 GATE: refuse to reveal (sign) before payment is committed.
    if (!PTX_RollCommitmentPresent(round_seed, quorum_hash)) {
        err = "no funded commitment for round_seed " + round_seed.ToString() +
              " under quorum " + quorum_hash.ToString() + " — refuse to sign";
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
