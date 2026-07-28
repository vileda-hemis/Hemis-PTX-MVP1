// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_CEREMONY_DRIVER_H
#define HEMIS_PTX_CEREMONY_DRIVER_H

// SG-2a — the ceremony phase DRIVER (shared-height-WINDOWED advance).
//
// The GJKR crypto (ptx_dkg.{h,cpp}) is a pure, unit-tested state machine of
// Build/Receive/Close functions.  This module is the LIVE DRIVER that walks
// it: an at-most-one-transition step function called on a tick.
//
// ADVANCE MODEL (SG-2a build-gate decision, supersedes early-IsComplete):
// every phase occupies a FORMATION-ANCHORED height window
//     phase k window = [formation_height + offset_k, formation_height + offset_{k+1})
// and a node advances k -> k+1 exactly when tip-height reaches the window
// end.  formation_height and the widths are shared by all members, so all
// nodes advance in LOCKSTEP.  Rationale (the phase-skew catch): the transport
// DROPS a message whose phase is ahead of the receiver's session (the
// ReceivePhaseN phase gate rejects, and ProcessBatchT never re-queues —
// ptx_dkg_net.cpp:267ff).  A node advancing EARLY on IsPhaseNComplete would
// emit phase-k+1 messages that nodes still in k discard — skew -> loss ->
// divergent qual.  Windowed advance removes the skew.  The IsComplete early
// -exit fast path is DEFERRED (owed, SG-2b) until the transport can BUFFER
// future-phase messages instead of dropping them.
//
// WIDTH CALIBRATION (recorded basis, not magic numbers):
// widths are a SAFETY parameter, not latency padding — a width shorter than
// the worst-case delivery skew reintroduces divergence via nodes closing a
// phase on different delivered sets (proven by the R-width harness row).
//   - floor: >= 2 blocks per phase.  >= 1 so tip-height can advance within
//     the window at all; +1 to cover the boundary-adjacent race (a message
//     emitted in the same inter-block interval as the boundary block).
//   - gossip propagation (inv->getdata->serve round-trip) is milliseconds-
//     to-seconds against a 60s block time on ALL nets (nTargetSpacing=60,
//     chainparams.cpp) — so 2 blocks already carries ~100x margin.
//   - the 11-message COLLECTION phases (P0 commit, P1 reveal — P1 carries
//     the largest payload: vvec + 11 encrypted blobs — and P4 premit) get
//     6 blocks; the usually-EMPTY dispute rounds (P2/P3) get 4.
//   - total = 6+6+4+4+6 = 26 blocks, well inside the formation interval on
//     every net (dev N=80, main N=1440); same widths on all nets (same
//     block spacing).
// DIVERGENCE FLOOR (what under-width degrades to): differing delivered sets
// produce differing effective-QUAL -> differing group_pk -> the ClosePhase4
// premit-consistency gate (>= t bytewise-equal to MY group_pk) fails on the
// minority view -> ABORT, never a divergent finalization.  R-width proves
// this floor in-suite.
//
// TWO SEAMS keep the step deterministically testable off-thread:
//   - HEIGHT (the shared clock, tip-height anchoring): the caller reads it
//     (chainActive tip under cs_main in the daemon; a harness-fake in the
//     suite) and passes it in — the step never touches the chain.
//   - SEND: the step RETURNS its outbound messages; the CALLER transmits
//     them AFTER the step returns (lock-order invariant 1 — never send
//     under session.cs).

#include "bls/bls_wrapper.h"        // CBLSSecretKey
#include "primitives/transaction.h" // CMutableTransaction
#include "ptx/ptx_dkg.h"            // PTXDKGSession, PTXDKGPhase

#include <string>
#include <vector>

// Reserved NodeId for a node's OWN messages routed through its transport (the
// production send seam: transport.ProcessMessage(self, ...) -> store + relay
// to peers; the redundant self-dispatch is a dedup no-op since the step
// already self-delivered).  Real peer NodeIds are >= 0.
static const int PTX_CEREMONY_SELF_NODE_ID = -2;

// One outbound phase message: the NetMsgType command + its serialized bytes.
// The caller feeds these to the send path (daemon: ProcessMessage(self,...);
// harness: the lossy scheduler -> peers).
struct PTXCeremonyOutbound {
    std::string                command;
    std::vector<unsigned char> raw;
};

// Per-phase window widths in BLOCKS (see the calibration basis above).
struct PTXCeremonyDeadlines {
    int w_hashcommit = 6;
    int w_contrib    = 6;
    int w_complaint  = 4;
    int w_justify    = 4;
    int w_premit     = 6;
    // ★ SG-5 S1 — THE ABSOLUTE STALL-OUT BOUND (ODC-050).  A ceremony that
    // reaches formation_height + max_span while still in a WINDOWED phase
    // ABORTS: its next window end is unreachable, and a hang is not a state
    // (KDD-077 §4 — abort is the safe terminal state).
    //
    // ★ WIDTH-INDEPENDENT BY CONSTRUCTION, and that is the whole point: a
    // budget derived from the widths (sum-of-widths + slack) is unreachable
    // EXACTLY WHEN A WIDTH IS THE BUG — with w_hashcommit = 100000,
    // OffsetEnd(PREMIT) becomes 100020 and such a bound never fires.  This
    // bound is set from the FORMATION INTERVAL instead, which enforces an
    // invariant the design already asserts but never checked: N must exceed
    // the ceremony floor "so a new boundary cannot fire before the prior
    // ceremony completes" (§9.1).  A ceremony still running at the next
    // boundary is definitionally stale.
    //
    // Default 80 = the dev-chain interval; production overwrites it from
    // params.nFormationInterval at the call site.  Non-positive disables
    // (harness rows that want the pre-S1 hang behaviour say so explicitly).
    int max_span     = 80;
    // Formation-relative height offset at which phase p's window ENDS (the
    // k -> k+1 advance boundary).  Accumulates the widths in phase order.
    int OffsetEnd(PTXDKGPhase p) const;
};

// Driver bookkeeping, owned by the caller (host or harness), NOT the session.
// Kept off the crypto struct: it is orchestration state, not ceremony state.
struct PTXCeremonyDriverState {
    PTXDKGPhase entered_phase{PTXDKGPhase::IDLE}; // phase whose entry we last handled
    bool        contrib_generated{false};         // GenerateLocalContrib ran (once)
    bool        own_sent{false};                  // own broadcast for entered_phase done (P0/P1/P4)
};

enum class PTXStepResult {
    WAITING,     // no transition this tick (inside the current window)
    PROGRESSED,  // advanced one phase this tick
    DONE,        // reached DONE — dkgtx_out holds the nType=11 tx
    ABORTED,     // a Close aborted (sub-threshold / inconsistent) — session.phase == ABORTED
};

// Advance the ceremony by AT MOST ONE phase transition.
//   session          — the ceremony state (mutated under *session.cs internally)
//   state            — driver bookkeeping (see above)
//   current_height   — the tip-height clock, pre-read by the caller (seam)
//   deadlines        — per-phase window widths (formation-anchored)
//   operator_sk      — this node's operator key (Build*/Decrypt* need it; never stored)
//   formation_height — the shared window anchor; also passed to BuildPTXDKGTx
//   outbounds_out    — CLEARED then filled with this tick's own messages; the
//                      caller sends them AFTER return (invariant 1)
//   dkgtx_out        — on DONE, receives the constructed nType=11 transaction
//
// Lock discipline (invariants 1 & 2, ptx_dkg.h): *session.cs is taken around
// the session work and RELEASED before return; ClosePhase5 (the one
// cs_main-taking DKG call) runs OUTSIDE *session.cs; the caller must NOT hold
// cs_main or *session.cs at call time (asserted).
PTXStepResult PTX_Ceremony_Step(PTXDKGSession& session,
                                PTXCeremonyDriverState& state,
                                int current_height,
                                const PTXCeremonyDeadlines& deadlines,
                                const CBLSSecretKey& operator_sk,
                                int formation_height,
                                std::vector<PTXCeremonyOutbound>& outbounds_out,
                                CMutableTransaction& dkgtx_out);

#endif // HEMIS_PTX_CEREMONY_DRIVER_H
