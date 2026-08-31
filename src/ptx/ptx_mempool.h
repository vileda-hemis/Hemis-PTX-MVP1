// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_MEMPOOL_H
#define HEMIS_PTX_MEMPOOL_H

#include "primitives/transaction.h"
#include "ptx/ptx_commit_reveal.h"

#include <string>

// BUG-032 2b-iii producer: build+fund+sign+broadcast the roll COMMITMENT
// (PTXROLLCOMMIT, nType=12) BEFORE the quorum signs. It carries the relocated
// accum service fee (the payment, forfeited at commit) AND a purpose-built chain
// output that the settle will spend — the coin-chain 2c requires. Returns the
// commitment txid; out_chain receives that chain output's outpoint (the settle
// must spend EXACTLY this outpoint — deterministic, never left to coin selection,
// or the coin-chain breaks and 2c rejects the settle).
// KDD-088: out_raw_hex additionally returns the serialized commitment so the
// fan-out can ATTACH it to each sign request (direct-attach). Optional — pass
// nullptr for the legacy gossip-only flow.
std::string PTX_BuildRollCommitment(const CPTXRollCommitPayload& payload,
                                    COutPoint& out_chain,
                                    std::string* out_raw_hex = nullptr);

// Build and submit the PTXSESS (settle/reveal) to the memory pool.
// Returns the txid hex on acceptance, or "pending" if mempool rejected.
// KDD-027: called immediately after TryResolve — no delay.
// BUG-032 2b-iii: the settle SPENDS chain_input (the commitment's chain output,
// still UNCONFIRMED in mempool — signed via a mempool-aware coins view) and
// carries NO accum fee (the fee relocated to the commitment).
std::string PTX_AutoCommit(const PTXCommitRevealRound& round,
                            const CProbabilisticTxPayload& payload,
                            const COutPoint& chain_input);

// ---------------------------------------------------------------------------
// BUG-032 (Option A, fund-then-sign): the payment-before-reveal gate.
// ---------------------------------------------------------------------------
// A roll's signature IS its result, so signing must not happen until a funded
// commitment for the exact round_seed is irrevocably broadcast.  PTX_RollCommitmentPresent
// scans the local mempool for a real PTXROLLCOMMIT (nType=12) naming this exact
// (round_seed, quorum_hash) — the commitment binds BOTH (quorum_hash = the
// canonical selection active at nSeedHeight, closing BUG-033), so the gate also
// refuses a sign request for a non-committed quorum. (Increment 2b replaced the
// earlier in-memory registry seam with this real mempool lookup.)
// KDD-088 DIRECT-ATTACH — accept a caller-supplied PTXROLLCOMMIT for THIS round
// into the local mempool via the NORMAL acceptance path, so the unchanged BUG-032
// gate can then pass without waiting for gossip.
//
// ★ ACCEPT-INTO-MEMPOOL IS THE SECURITY MECHANISM. The gate's predicate has always
// been "is this commitment in MY mempool", never "was it broadcast" — so attaching
// swaps the courier and leaves the predicate identical. Verify-without-accepting
// would break it: no UTXO check (inputs could be spent), no relay (a caller could
// collect partials and withhold), no fee policy. AcceptToMemoryPool is what makes
// the fee real, so "well-formed" is insufficient — the attack needs MEMPOOL-VALID,
// and mempool-valid means paid.
//
// Deliberately TRANSPORT-AGNOSTIC (takes bytes, not a request) so KDD-085
// sign-over-P2P can call it without unpicking the RPC call site.
//
// Returns true iff the commitment is present after the attempt. Never throws:
// a bad attachment must not fail louder than no attachment at all — the caller
// falls through to the gate's ordinary retryable refusal. `err` is diagnostic.
bool PTX_AcceptAttachedCommitment(const std::string& commit_hex,
                                  const uint256& round_seed,
                                  const uint256& quorum_hash,
                                  std::string& err);

bool PTX_RollCommitmentPresent(const uint256& round_seed, const uint256& quorum_hash);

// The gated signing entry the RPC/fan-out path uses.  Signs round_seed with this
// node's CURRENT share for quorum_hash and returns the partial sig — ONLY if a
// funded commitment for (round_seed, quorum_hash) is present.  Returns false
// (err set) if no share is held OR no commitment is present.
//
// out_retryable (2b-ii, the LATENCY property): when the refusal is "no commitment
// SEEN YET", *out_retryable is set true — the member cannot distinguish a
// commitment still in flight (propagation delay) from one never broadcast, so it
// defaults to RETRYABLE and lets the coordinator's retry budget bound the wait.
// A hard reject on a not-yet-propagated commitment would fail legitimate rolls
// under normal network delay. Terminal refusals (no share, sign failure) set it
// false. On success it is false. nullptr is accepted (caller does not care).
// ── KDD-085 component 2 ─────────────────────────────────────────────────────
// The acceptance tail, shared by the RPC attach path and the P2P sign handler.
// ★ ONE implementation, TWO callers, by construction: a commitment this node
// would accept over RPC is exactly one it would accept over P2P. Two paths asked
// to agree about validity eventually disagree (the h385 lesson), and here a
// disagreement is a member that signs when its peers will not.
//
// ★★ THIS CALL IS WHAT MAKES THE ATTACHMENT SELF-VERIFYING RATHER THAN MERELY
// PRESENT. The caller ASSERTS payment by attaching bytes; TryATMP PROVES it
// against THIS node's UTXO set and chainparams -- signatures verify, inputs
// exist and are unspent, and CheckPTXRollCommitTx runs the service-fee output
// check, the BUG-033 canonical-quorum gate at nSeedHeight, and the ODC-073
// anchor-lag bound. None of those reads anything about the requester, which is
// the entire no-identity claim (KDD-105) discharged by an existing path.
//
// `commit` must already be a decoded PTXROLLCOMMIT whose payload names
// (round_seed, quorum_hash); the P2P arm proves that in its cheap check and the
// RPC arm proves it inline. true == a commitment for this round is now present.
bool PTX_AcceptVettedCommitment(const CTransactionRef& commit,
                                const uint256& round_seed,
                                const uint256& quorum_hash,
                                std::string& err);

bool PTX_SignRoundIfCommitted(const uint256& round_seed, const uint256& quorum_hash,
                              uint8_t out_sig[96], std::string& err,
                              bool* out_retryable = nullptr);

#endif // HEMIS_PTX_MEMPOOL_H
