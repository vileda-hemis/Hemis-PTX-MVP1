// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_DKG_PENDING_H
#define HEMIS_PTX_DKG_PENDING_H

#include "primitives/transaction.h"
#include "sync.h"
#include "validation.h" // cs_main (lock annotations; same pattern as specialtx_validation.h)

class CBlock;
class CBlockIndex;
class CValidationState;

// W1.3 Package 3 C4 — the pending PTXDKG slot (KDD-058 direct-block-inject,
// spec §4; LLMQ minableCommitments precedent).
//
// A single-transaction slot holding the ceremony-result PTXDKG until a block
// template picks it up.  Guarded by its own leaf mutex; lock order is
// cs_main → slot lock, never the reverse.
//
// Validation is a PAIR (reorg-window mitigation, W1.3 C4 addendum):
//   (1) populate-time — SetPendingTx refuses a tx that fails CheckPTXDKGTx
//       at the current tip;
//   (2) generate-time — GetMinablePTXDKGTx re-validates against the block
//       assembler's captured anchor (pindexPrev) and SKIPS on reject.
// Either half alone leaves a window: a reorg between populate and generate
// can turn a valid pending tx into a rejecting one; without (2) the
// assembler would build an invalid block template.  A bad pending tx
// reaching the assembler is a LOCAL LIVENESS LOSS only (this node's staking
// thread exits until restart) — no consensus penalty (PTX has no slashing)
// and no effect on the collateral, a separate owner-held UTXO; the operator
// holds only the BLS operator key.

// Populate the slot.  REFUSE-WHILE-SET (E-4): fails with reject reason
// "ptxdkg-pending-occupied" if the slot already holds a tx — an explicit
// clear is required first (no last-wins).  VALIDATE-BEFORE-INJECT: runs
// CheckPTXDKGTx(tx, chainActive.Tip(), state) under cs_main and refuses on
// any reject (state carries the reason).  With no chain (unit tests) the
// tip is null and the structural body alone gates.
bool PTX_DKG_SetPendingTx(const CTransactionRef& tx, CValidationState& state);

// Fetch the pending tx for inclusion in a block template building on
// pindexPrev.  Returns false if the slot is empty.  RE-VALIDATES against
// the PASSED-IN pindexPrev (the assembler's captured anchor — never a fresh
// tip read); on a state reject the tx is SKIPPED: logged, false returned,
// and the slot LEFT SET (KEEP-BUT-SKIP, E-9) so it survives a transient
// reorg-and-back — the debug RPC's explicit clear covers permanent
// orphaning.  The skip keys on the CValidationState reject ONLY: the V4
// local-DB-corruption throw from GetListForBlock propagates untouched.
bool PTX_DKG_GetMinablePTXDKGTx(const CBlockIndex* pindexPrev,
                                CTransactionRef& ret) EXCLUSIVE_LOCKS_REQUIRED(cs_main);

// DEBUG-ONLY direct populate (C5 debug RPC force path, E-1): seats the tx
// with BOTH populate-time guards bypassed — no refuse-while-set (overwrites an
// occupied slot) and no validate-before-inject.  The generate-time
// re-validation half of the pair still runs untouched; exposing that reject to
// observation is the reason this exists.  Production code must never call
// this; the only caller is the net-gated ptx_debug_ptxdkgpopulate RPC.
void PTX_DKG_ForceSetPendingTx(const CTransactionRef& tx);

// Explicit clear (debug RPC / tests).
void PTX_DKG_ClearPendingTx();

// Clear-on-inclusion (LLMQ ProcessCommitment precedent): if the block
// contains a PTXDKG whose txid matches the slot, clear it.  Called from
// ProcessSpecialTxsInBlock on connect (!fJustCheck).  Clear-on-inclusion
// only — disconnect does NOT re-pend in W1.3 (E-5); W2 owns re-submission.
void PTX_DKG_ClearPendingIfIncluded(const CBlock& block);

// Raw occupancy probe (debug RPC / tests) — no validation, just slot state.
bool PTX_DKG_HasPendingTx();

#endif // HEMIS_PTX_DKG_PENDING_H
