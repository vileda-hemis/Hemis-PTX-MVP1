// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_mempool.h"

#include "chainparams.h"
#include "consensus/validation.h"
#include "core_io.h"          // KDD-088: EncodeHexTx for direct-attach
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
                                    COutPoint& out_chain,
                                    std::string* out_raw_hex)
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

    // Sign funding inputs against pcoinsTip — the CONFIRMED UTXO set. ★ This is a
    // KNOWN LIMITATION, left in place deliberately; the reason is at the end of
    // this comment and it is a measured deadlock, not an oversight.
    //
    // ★ The previous comment here read "all confirmed → pcoinsTip" as though it
    // were a guarantee. It is an assumption, and it breaks. Under sustained rolls
    // the wallet's CONFIRMED coins
    // are exhausted -- every roll spends one and returns its change UNCONFIRMED --
    // so FundTransaction starts selecting this caller's own unconfirmed change.
    // pcoinsTip is the confirmed UTXO set and has no entry for such an outpoint, so
    // AccessCoin returned a default-constructed Coin, IsSpent() was true, and the
    // roll died at 62 ms before the quorum was ever contacted.
    //
    // ★★ AND THE OLD MESSAGE SAID "already spent", WHICH IS FALSE. The input is
    // not spent; it is NOT YET CONFIRMED, and a default-constructed Coin is
    // indistinguishable from a spent one. That string cost a whole session chasing
    // an RPC work queue -- the measured truth was that a SINGLE sequential roll
    // (N=1, no concurrency at all) failed once the mempool was deep, which no
    // thread or queue setting can explain.
    //
    // The obvious fix is the one the settle path below already uses: resolve
    // through a CCoinsViewMemPool so unconfirmed change is visible.
    //
    // ★★★ IT WAS NOT APPLIED HERE, AND THE REASON WAS A MEASURED
    // DEADLOCK. Resolving through CCoinsViewMemPool requires mempool.cs, and taking
    // it here as `cs_main -> mempool.cs -> cs_wallet` (copying the settle path as
    // it then stood) INVERTS against the wallet, which runs
    // `cs_main -> cs_wallet -> mempool.cs`: CWallet::ReacceptWalletTransactions
    // (wallet/wallet.cpp:1977) holds LOCK2(cs_main, cs_wallet), at :1999 calls
    // CWalletTx::AcceptToMemoryPool, which at wallet.cpp:4473 calls
    // mempool.exists() -- CTxMemPool::exists takes LOCK(cs) at txmempool.h:642.
    //
    // A DEBUG_LOCKORDER daemon carrying that change aborted on the FIRST roll:
    //   POTENTIAL DEADLOCK DETECTED ... Assertion failed: detected inconsistent
    //   lock order at sync.cpp:122
    // Correct order is cs_main -> cs_wallet -> mempool.cs.
    //
    // ★★ AND THAT INDICTED THE SETTLE PATH BELOW, which used the inverted order in
    // shipping code and carried the same latent deadlock -- invisible only because
    // the fleet binary is built WITHOUT DEBUG_LOCKORDER (unstripped, not
    // instrumented). BUG-048.
    //
    // ★ STATUS: the settle block below now takes `cs_main -> cs_wallet ->
    // mempool.cs`, so the canonical order is established in PTX code and the
    // mempool-aware resolution IS NOW UNBLOCKED HERE -- it would be written as
    //   LOCK2(cs_main, pwallet->cs_wallet); LOCK(mempool.cs);
    // with a CCoinsViewMemPool over pcoinsTip, exactly as the settle path does.
    // It is deliberately NOT taken in the same change as the lock fix: that fix is
    // a pure reorder with no behavioural delta, and this is a funding-semantics
    // change (unconfirmed self-change becomes spendable at commit time) that wants
    // its own verification. Landing it is a scheduling decision, no longer a
    // lock-order one.
    //
    // What IS applied below is the message fix, which needs no lock change and is
    // worth landing alone: the old text claimed "already spent".
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        for (unsigned int i = 0; i < mtx.vin.size(); i++) {
            CTxIn& txin = mtx.vin[i];
            const Coin& coin = pcoinsTip->AccessCoin(txin.prevout);
            if (coin.IsSpent()) {
                unlockFundedInputs();
                throw JSONRPCError(RPC_PTX_SETTLEMENT_FAILED,
                                   "commitment input " + std::to_string(i) +
                                   " not in the confirmed UTXO set (not yet "
                                   "confirmed, or spent)");
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
        // KDD-088: hand back the serialized commitment AFTER acceptance, so the
        // fan-out can only ever attach bytes this node itself accepted.
        if (out_raw_hex) *out_raw_hex = EncodeHexTx(CTransaction(mtx));
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
    //
    // ★ BUG-048: the lock order here is `cs_main -> cs_wallet -> mempool.cs`, and
    // it is not a free choice. `CWallet::ReacceptWalletTransactions`
    // (wallet/wallet.cpp:1977) holds LOCK2(cs_main, cs_wallet) and at :1999 calls
    // `CWalletTx::AcceptToMemoryPool`, which at wallet.cpp:4473 calls
    // `mempool.exists()` — `CTxMemPool::exists` takes LOCK(cs) at txmempool.h:642.
    // That path, plus 84 canonical LOCK2(cs_main, [p]wallet->cs_wallet) sites
    // (grep, 2026-08-24), is what establishes the order; this block previously took `cs_main -> mempool.cs ->
    // cs_wallet` and inverted the last two, a latent deadlock that shipped and was
    // invisible only because the fleet binary carries no DEBUG_LOCKORDER (KDD-102).
    // ★ mempool.cs is taken LAST and innermost on purpose: it is the hot lock
    // (PTX_RollCommitmentPresent scans all of mapTx under it, twice per
    // gm_bls_sign), so it must not be held across the cs_wallet acquisition.
    {
        LOCK2(cs_main, pwallet->cs_wallet);
        LOCK(mempool.cs);
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
// ── KDD-085 §9.4: GENERATION-CACHED INDEX ───────────────────────────────────
// This predicate used to walk all of mapTx on EVERY call, copying a
// CTransactionRef (an atomic refcount pair) per entry, while holding mempool.cs
// — and it is the FIRST statement of both signing entry points
// (PTX_SignRoundIfCommitted and PTX_AcceptAttachedCommitment).
//
// ★★ WHY THAT WAS THE WHOLE DoS SURFACE, AND WHY THE FIX IS SHAPED LIKE THIS.
// The scan is linear in mempool size, and mempool.cs is contended by ATMP and
// block assembly — so a caller that can trigger this cheaply is mounting a
// LIVENESS attack on block production, not merely burning CPU. Measured
// (W4B_COST_AND_KDD085_SCOPE.md §9.4): at the DEFAULT maxmempool=300
// (policy/policy.h:25 — the fleet's 50 is NOT what operators get, KDD-106),
// ~47 kbit/s of requests holds this lock continuously. Today the RPC credential
// is what bounds that; KDD-085 removes the credential, so the bound has to be
// structural before the endpoint can be opened.
//
// ★ THE KEY PROPERTY: A FLOOD OF REQUESTS DOES NOT MUTATE THE MEMPOOL. The
// expensive rebuild is gated on mempool CHANGE, not on being ASKED. An attacker
// controls how often they ask; they do not cheaply control how often mapTx
// changes. So the scan is paid per mempool mutation (which the node was already
// doing work for) and every request in between is an O(log n) set lookup.
//
// ★ WHY A CACHE KEYED ON THE UPDATE COUNTER RATHER THAN AN INDEX MAINTAINED BY
// HOOKS. A hook-maintained index is stored-and-trusted: it must be updated at
// every add/remove/clear site, and if one is ever missed it DRIFTS silently and
// this predicate starts lying — in the direction that refuses legitimate rolls
// or, worse, admits illegitimate ones. That is exactly BUG-036's REGISTER 2
// (derive-don't-store: stored copies drift; a lost write is a latent divergence
// source). Here the cache is DERIVED and recomputed whenever mapTx changes, so
// staleness is unrepresentable rather than merely unlikely, and there is no new
// invariant for a future mempool change to violate.
//
// nTransactionsUpdated is a complete mutation counter for mapTx: incremented in
// addUnchecked (txmempool.cpp:471), removeUnchecked (:568) and clear() (:884).
// GetTransactionsUpdated() takes cs, which is a RecursiveMutex (txmempool.h:471),
// so calling it under our own LOCK(mempool.cs) is safe.
//
// A validity FLAG rather than a sentinel generation: unsigned wrap-around cannot
// then alias "never built" onto a live counter value.
static bool                                  g_ptx_commit_index_valid = false;
static unsigned int                          g_ptx_commit_index_gen   = 0;
static std::set<std::pair<uint256, uint256>> g_ptx_commit_index;

bool PTX_RollCommitmentPresent(const uint256& round_seed, const uint256& quorum_hash)
{
    LOCK(mempool.cs);

    const unsigned int gen = mempool.GetTransactionsUpdated();
    if (!g_ptx_commit_index_valid || gen != g_ptx_commit_index_gen) {
        g_ptx_commit_index.clear();
        for (const auto& e : mempool.mapTx) {
            const CTransactionRef tx = e.GetSharedTx();
            if (!tx->IsPTXRollCommitTx()) continue;
            CPTXRollCommitPayload p;
            if (!GetTxPayload(*tx, p)) continue;
            g_ptx_commit_index.emplace(p.round_seed, p.quorum_hash);
        }
        g_ptx_commit_index_gen   = gen;
        g_ptx_commit_index_valid = true;
    }

    return g_ptx_commit_index.count(std::make_pair(round_seed, quorum_hash)) > 0;
}

// ── KDD-088 direct-attach: accept caller-supplied commitment bytes ───────────
// Contract and rationale in ptx_mempool.h. Every cheap rejection happens BEFORE
// any validation work, because these are CALLER-SUPPLIED bytes and that is the
// one genuinely new exposure this feature introduces.
bool PTX_AcceptAttachedCommitment(const std::string& commit_hex,
                                  const uint256& round_seed,
                                  const uint256& quorum_hash,
                                  std::string& err)
{
    // Already present (gossip won the race) — the attachment is a no-op. This is
    // the common case once propagation catches up, so it is checked first.
    if (PTX_RollCommitmentPresent(round_seed, quorum_hash)) return true;

    // A commitment is a few hundred bytes; anything near this cap is abuse, not
    // a legitimate round. Size is checked before hex-validation before decode.
    static const size_t PTX_ATTACH_MAX_HEX = 200000;   // 100 kB of transaction
    if (commit_hex.size() > PTX_ATTACH_MAX_HEX) {
        err = "attached commitment too large";
        LogPrintf("PTX attach: REFUSED (size %u > %u)\n",
                  (unsigned)commit_hex.size(), (unsigned)PTX_ATTACH_MAX_HEX);
        return false;
    }
    if (!IsHex(commit_hex)) { err = "attached commitment not hex"; return false; }

    CMutableTransaction mtx;
    if (!DecodeHexTx(mtx, commit_hex)) { err = "attached commitment decode failed"; return false; }
    const CTransaction attached(mtx);
    if (!attached.IsPTXRollCommitTx()) {
        err = "attached tx is not a PTXROLLCOMMIT";
        LogPrintf("PTX attach: REFUSED (%s is not a PTXROLLCOMMIT)\n",
                  attached.GetHash().GetHex());
        return false;
    }
    // Bind the attachment to THIS round BEFORE spending validation on it: an
    // attachment for another round is not our business, and accepting it would
    // let a caller push unrelated traffic through us.
    CPTXRollCommitPayload ap;
    if (!GetTxPayload(attached, ap) ||
        ap.round_seed != round_seed || ap.quorum_hash != quorum_hash) {
        err = "attached commitment does not match this round";
        LogPrintf("PTX attach: REFUSED (payload does not match round_seed/quorum_hash)\n");
        return false;
    }

    return PTX_AcceptVettedCommitment(MakeTransactionRef(std::move(mtx)),
                                      round_seed, quorum_hash, err);
}

// ── KDD-085 component 2: the acceptance tail, shared by both arms ────────────
// Contract and rationale in ptx_mempool.h.
//
// ★★ THIS IS THE ONE ACCEPTANCE IMPLEMENTATION AND IT HAS EXACTLY TWO CALLERS:
// the RPC attach path above (which hex-decodes first) and the P2P sign handler
// (which already holds a decoded, cheap-checked commitment). Written as one
// function because the h385 lesson on this codebase is that two paths asked to
// agree about validity will eventually disagree — and here a disagreement means
// one transport accepts a commitment the other rejects, i.e. a member that signs
// when its peers will not.
//
// ★★ AND THIS IS WHERE "SELF-VERIFYING" STOPS BEING A WORD. Everything before
// this point establishes only that the caller sent bytes SHAPED like a
// commitment for this round — the caller ASSERTS payment. TryATMP is what
// PROVES it, against THIS node's UTXO set and THIS node's chainparams: the
// signatures verify, the inputs exist and are unspent, and CheckPTXRollCommitTx
// (specialtx_validation.cpp:950-1000) runs the service-fee output check, the
// BUG-033 canonical-quorum gate at nSeedHeight, and the ODC-073 anchor-lag
// bound. Nothing in that list reads anything about the requester. That is the
// whole no-identity claim, discharged by an existing code path rather than a
// new one.
bool PTX_AcceptVettedCommitment(const CTransactionRef& commit,
                                const uint256& round_seed,
                                const uint256& quorum_hash,
                                std::string& err)
{
    // Already present (gossip won the race, or a previous request for this same
    // round already accepted it) — nothing to do. Checked first because once a
    // round is under way this is the common case, and it costs an O(log n)
    // lookup against the bfea163 index rather than a validation pass.
    if (PTX_RollCommitmentPresent(round_seed, quorum_hash)) return true;

    // The NORMAL acceptance path — byte-for-byte what gossip-delivered bytes get.
    // If it is unfunded, unsigned, or spends spent inputs, this rejects and the
    // gate refuses exactly as it would have without any attachment.
    try {
        CMutableTransaction mtx(*commit);
        TryATMP(mtx, false);
        RelayTx(commit->GetHash());   // our own relay carries it onward
        LogPrintf("PTX attach: accepted commitment %s (round_seed=%s)\n",
                  commit->GetHash().GetHex(), round_seed.ToString());
    } catch (const UniValue& objError) {
        err = "attached commitment mempool-rejected: " + objError["message"].getValStr();
        LogPrintf("PTX attach: %s\n", err);
        return false;
    } catch (const std::exception& e) {
        err = std::string("attached commitment error: ") + e.what();
        LogPrintf("PTX attach: %s\n", err);
        return false;
    }
    return PTX_RollCommitmentPresent(round_seed, quorum_hash);
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
