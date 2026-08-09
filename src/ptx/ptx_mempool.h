// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_MEMPOOL_H
#define HEMIS_PTX_MEMPOOL_H

#include "primitives/transaction.h"
#include "ptx/ptx_commit_reveal.h"

#include <string>

// Build and submit a PTX special transaction to the memory pool.
// Returns the txid hex on acceptance, or "pending" if mempool rejected.
// KDD-027: called immediately after TryResolve — no delay.
std::string PTX_AutoCommit(const PTXCommitRevealRound& round,
                            const CProbabilisticTxPayload& payload);

// ---------------------------------------------------------------------------
// BUG-032 (Option A, fund-then-sign): the payment-before-reveal gate.
// ---------------------------------------------------------------------------
// A roll's signature IS its result, so signing must not happen until a funded
// commitment for the exact round_seed is irrevocably broadcast.  The full fix
// checks the mempool/chain for the commitment tx (new nType); this registry is
// the gatable seam the signing entry consults — wired to real commitments as the
// commitment tx type lands.  Keyed on (round_seed, quorum_hash): the commitment
// binds BOTH (quorum_hash = the canonical selection at nSeedHeight, closing
// BUG-033), so the gate also rejects a sign request for a non-committed quorum.
void PTX_RegisterRollCommitment(const uint256& round_seed, const uint256& quorum_hash);
bool PTX_RollCommitmentPresent(const uint256& round_seed, const uint256& quorum_hash);

// The gated signing entry the RPC/fan-out path uses.  Signs round_seed with this
// node's CURRENT share for quorum_hash and returns the partial sig — ONLY if a
// funded commitment for (round_seed, quorum_hash) is present.  Returns false
// (err set) if no share is held OR (post-fix) no commitment exists.  ★ INITIALLY
// UNGATED so the invariant RED proves the current hole; the fix adds the gate.
bool PTX_SignRoundIfCommitted(const uint256& round_seed, const uint256& quorum_hash,
                              uint8_t out_sig[96], std::string& err);

#endif // HEMIS_PTX_MEMPOOL_H
