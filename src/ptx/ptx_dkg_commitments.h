// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_DKG_COMMITMENTS_H
#define HEMIS_PTX_DKG_COMMITMENTS_H

// KDD-058-A — the replicated minable-commitments store (supersedes the
// member-only in-memory pending slot, ptx_dkg_pending).
//
// WHY (the landing amendment): locked collateral cannot stake
// (StakeableCoins fIncludeLocked=false), so near-zero GM stakeable balance
// is the DESIGNED steady state — a finished nType=11 result held only by
// quorum members lands ~never at scale (fleet proof: 3 convergences → 1
// landed by whale-luck, 1 restart-wiped, 1 unmineable).  The fix is the
// LLMQ-qfc shape (CQuorumBlockProcessor precedent): the finished commitment
// tx RELAYS network-wide (RelayInv scope — NOT the member-mesh ceremony
// relayHook), every node holds it in a replicated store, and ANY staker's
// assembler packages it.  Consensus is untouched — inclusion was always
// any-staker-ready (CheckPTXDKGTx has no staker-identity rule).
//
// Persistence: REPLICATION-ONLY, Dash-faithful (minableCommitments is
// memory+replication in the lineage; only MINED commitments hit disk).  A
// whole-network simultaneous restart loses un-mined entries — accepted, as
// in Dash (a fresh boundary re-forms); single-node restarts re-learn from
// peers via inv.
//
// Keying: per-quorum (quorum_hash → tx), better-completed-count replacement
// (the HasBetterMinableCommitment mirror).  This SUPERSEDES the old slot's
// E-4 refuse-while-set: outstanding results for DIFFERENT quorums coexist;
// within a quorum, more-complete wins.  Members build the tx
// deterministically, so same-quorum duplicates share a hash and dedup.

#include "primitives/transaction.h"
#include "uint256.h"

#include <string>

class CBlock;
class CBlockIndex;
class CDataStream;
class CValidationState;
typedef int NodeId; // matches net.h:94 (duplicate-identical typedef, no include cycle)

// Origin path (ClosePhase5 / debug RPC): validate (CheckPTXDKGTx at the
// current tip; null tip = structural body only, the unit-test seam), insert
// under the per-quorum replacement policy, then RelayInv network-wide.
// false + state populated on validation reject or not-better refusal.
bool PTX_DKG_Commitments_AddAndRelay(const CTransactionRef& tx, CValidationState& state);

// Debug-RPC force path (guards bypassed, mirrors the old ForceSetPendingTx):
// unconditional insert (replaces any same-quorum entry) + relay.
void PTX_DKG_Commitments_ForceAdd(const CTransactionRef& tx);

// Wire receipt (PTXDKGCOMMIT): deserialize the tx, gate on tiertwo sync
// (ignore-without-DoS while unsynced, the GMAUTH posture), then the
// AddAndRelay path (insert + re-relay = gossip).  retMisbehavingScore: 100
// structural garbage, 10 contextual validation reject (possibly stale view),
// 0 clean/ignored.  Returns false only on unroutable garbage.
bool PTX_DKG_Commitments_ProcessMessage(NodeId from, CDataStream& vRecv, int& retMisbehavingScore);

// AlreadyHave seam (net_processing MSG_PTX_DKG_COMMITMENT).
bool PTX_DKG_Commitments_Has(const uint256& txHash);

// Getdata-serve seam.
bool PTX_DKG_Commitments_GetByHash(const uint256& txHash, CTransactionRef& ret);

// Assembler seam (repointed from the old slot).  Iterates the store and
// returns the first entry that passes the generate-time CheckPTXDKGTx
// re-validation against pindexPrev — the reorg guard, kept verbatim from the
// slot (KEEP-BUT-SKIP: a failing entry is skipped and retained).
// AssertLockHeld(cs_main) — CreateNewBlock holds it.
bool PTX_DKG_Commitments_GetMinableTx(const CBlockIndex* pindexPrev, CTransactionRef& ret);

// Connect-time bookkeeping (specialtx ProcessSpecialTxsInBlock, !fJustCheck):
// erase every PTXDKG tx in the block from the store (mined → no longer
// minable).  Node-local bookkeeping only — no validation behavior.
void PTX_DKG_Commitments_EraseMined(const CBlock& block);

// Debug/test lifecycle: clear everything; emptiness probe.
void PTX_DKG_Commitments_Clear();
bool PTX_DKG_Commitments_Empty();

#endif // HEMIS_PTX_DKG_COMMITMENTS_H
