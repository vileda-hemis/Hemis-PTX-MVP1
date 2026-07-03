// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_dkg_pending.h"

#include "consensus/validation.h"
#include "evo/specialtx_validation.h"
#include "logging.h"
#include "primitives/block.h"

namespace {
// Leaf mutex (minableCommitmentsCs precedent); lock order cs_main → slot.
Mutex g_ptx_dkg_pending_cs;
CTransactionRef g_ptx_dkg_pending_tx GUARDED_BY(g_ptx_dkg_pending_cs);
} // namespace

bool PTX_DKG_SetPendingTx(const CTransactionRef& tx, CValidationState& state)
{
    LOCK(cs_main);
    LOCK(g_ptx_dkg_pending_cs);

    // REFUSE-WHILE-SET (E-4): explicit clear required, no last-wins.
    if (g_ptx_dkg_pending_tx) {
        return state.Invalid(error("%s: pending PTXDKG slot already occupied by %s", __func__,
                                   g_ptx_dkg_pending_tx->GetHash().ToString()),
                             REJECT_INVALID, "ptxdkg-pending-occupied");
    }

    // VALIDATE-BEFORE-INJECT — populate half of the pair.  Null tip (no
    // chain, unit tests) runs the structural body only.
    if (!CheckPTXDKGTx(*tx, chainActive.Tip(), state)) {
        LogPrintf("PTX DKG: %s: refusing pending PTXDKG %s (%s)\n", __func__,
                  tx->GetHash().ToString(), state.GetRejectReason());
        return false;
    }

    g_ptx_dkg_pending_tx = tx;
    LogPrintf("PTX DKG: %s: pending PTXDKG slot set to %s\n", __func__,
              tx->GetHash().ToString());
    return true;
}

bool PTX_DKG_GetMinablePTXDKGTx(const CBlockIndex* pindexPrev, CTransactionRef& ret)
{
    AssertLockHeld(cs_main);
    LOCK(g_ptx_dkg_pending_cs);

    if (!g_ptx_dkg_pending_tx) {
        return false;
    }

    // Generate-time RE-VALIDATION against the assembler's captured anchor —
    // the other half of the pair.  A reorg in the populate→generate window
    // otherwise turns the pending tx into a rejecting one inside the block
    // template (local liveness loss — the staking thread exits; no consensus
    // penalty, collateral untouched).  Keys on the CValidationState reject
    // ONLY — deliberately no try/catch, the V4 corruption throw from
    // GetListForBlock must propagate untouched.
    CValidationState state;
    if (!CheckPTXDKGTx(*g_ptx_dkg_pending_tx, pindexPrev, state)) {
        // KEEP-BUT-SKIP (E-9): the slot survives a transient reorg-and-back;
        // the debug RPC's explicit clear covers permanent orphaning.
        LogPrintf("PTX DKG: %s: pending PTXDKG %s skipped at generate time (%s) — slot kept\n",
                  __func__, g_ptx_dkg_pending_tx->GetHash().ToString(),
                  state.GetRejectReason());
        return false;
    }

    ret = g_ptx_dkg_pending_tx;
    return true;
}

void PTX_DKG_ForceSetPendingTx(const CTransactionRef& tx)
{
    LOCK(g_ptx_dkg_pending_cs);
    LogPrintf("PTX DKG: %s: FORCE-populating pending PTXDKG slot with %s (populate guards bypassed)%s\n",
              __func__, tx->GetHash().ToString(),
              g_ptx_dkg_pending_tx ? " — overwriting occupied slot" : "");
    g_ptx_dkg_pending_tx = tx;
}

void PTX_DKG_ClearPendingTx()
{
    LOCK(g_ptx_dkg_pending_cs);
    if (g_ptx_dkg_pending_tx) {
        LogPrintf("PTX DKG: %s: pending PTXDKG slot cleared (was %s)\n", __func__,
                  g_ptx_dkg_pending_tx->GetHash().ToString());
        g_ptx_dkg_pending_tx.reset();
    }
}

void PTX_DKG_ClearPendingIfIncluded(const CBlock& block)
{
    LOCK(g_ptx_dkg_pending_cs);
    if (!g_ptx_dkg_pending_tx) {
        return;
    }
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsPTXDKGTx() && tx->GetHash() == g_ptx_dkg_pending_tx->GetHash()) {
            LogPrintf("PTX DKG: %s: pending PTXDKG %s included in block — slot cleared\n",
                      __func__, tx->GetHash().ToString());
            g_ptx_dkg_pending_tx.reset();
            return;
        }
    }
}

bool PTX_DKG_HasPendingTx()
{
    LOCK(g_ptx_dkg_pending_cs);
    return g_ptx_dkg_pending_tx != nullptr;
}
