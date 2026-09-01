// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_ceremony_driver.h"

#include "logging.h"
#include "protocol.h"      // NetMsgType::PTXQ*
#include "streams.h"       // CDataStream
#include "validation.h"    // cs_main (invariant-2 assert)
#include "version.h"       // PROTOCOL_VERSION

namespace {

// Serialize a typed phase message into an outbound (wire form — the same
// SERIALIZE_METHODS the transport de/serializes).
template <typename Msg>
PTXCeremonyOutbound MakeOutbound(const std::string& command, const Msg& msg)
{
    CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
    ss << msg;
    return PTXCeremonyOutbound{command, std::vector<unsigned char>(ss.begin(), ss.end())};
}

// ODC-096.  The tripwire is this assert, NOT the default-less switch below:
// measured on this toolchain, a default-less switch with an unhandled
// enumerator compiles under -Wall and exits 0 (warning [-Wswitch] only) --
// -Werror is opt-in here (configure.ac:348) and adds only vla +
// thread-safety-analysis.  The switch still earns its keep: it catches a phase
// inserted in the MIDDLE, which leaves _COUNT unchanged.  The pair is the
// guard.  Copied from KDD-085's PTXMemberSignState, the one enum in this tree
// ever proven to fire (on TOO_OLD).
//
// ★★ IF THIS ASSERT JUST FIRED, YOU ARE ADDING A PHASE. Three switches over
// PTXDKGPhase have a `default:` arm and will NOT tell you they need you:
//   * PTXCeremonyDeadlines::OffsetEnd  -- `default: return 0`, so your phase
//     gets a ZERO-WIDTH window: `current_height >= formation_height + 0` is
//     true immediately and the phase closes on its first step, silently. This
//     is ODC-050's class (a width-derived budget failing exactly when a width
//     is the bug). GIVE IT A REAL WIDTH -- this is the one that will waste
//     your day.
//   * the own-emission switch  -- your phase broadcasts nothing.
//   * the windowed-advance switch -- your phase never advances.
static_assert((int)PTXDKGPhase::_COUNT == 9,
              "PTXDKGPhase gained or lost a phase: give it a width in "
              "PTXCeremonyDeadlines::OffsetEnd (its `default:` returns 0 = a "
              "zero-width window that closes on the first step), an emission "
              "arm, an advance arm, and a name below.");

// Short phase name for the CP-4 transition log (fleet-grep timeline).
const char* PhaseName(PTXDKGPhase p)
{
    switch (p) {
        case PTXDKGPhase::_COUNT:      break;   // sentinel, never a value
        case PTXDKGPhase::IDLE:        return "IDLE";
        case PTXDKGPhase::HASH_COMMIT: return "HASH_COMMIT";
        case PTXDKGPhase::CONTRIB:     return "CONTRIB";
        case PTXDKGPhase::COMPLAINT:   return "COMPLAINT";
        case PTXDKGPhase::JUSTIFY:     return "JUSTIFY";
        case PTXDKGPhase::PREMIT:      return "PREMIT";
        case PTXDKGPhase::FINALIZE:    return "FINALIZE";
        case PTXDKGPhase::DONE:        return "DONE";
        case PTXDKGPhase::ABORTED:     return "ABORTED";
    }
    return "?";
}

// Decrypt every revealed (qual, non-bad) dealer's share to this node — incl.
// self (self-delivered its own Phase 1, so phase1_encrypted_shares[self] is
// present).  Best-effort: a wrong/absent blob leaves received_shares without
// that dealer; ComputeSkShare's completeness gate is the backstop.
void DecryptRevealedShares(PTXDKGSession& session, const CBLSSecretKey& operator_sk)
{
    for (const auto& ptx : session.qual) {
        if (session.bad_members.count(ptx)) continue;
        PTX_DKG_DecryptMyShare(session, ptx, operator_sk);
    }
}

// COMPLAINT-window emission, IDEMPOTENT per dealer: Feldman-check every OTHER
// revealed dealer's share to me; complain per failure.  Self-delivery records
// the complaint in complaints_against, which is also the re-entry guard — a
// later step in the same window emits nothing new.  0 complaints on the
// honest path.
void EmitComplaints(PTXDKGSession& session,
                    const CBLSSecretKey& operator_sk,
                    std::vector<PTXCeremonyOutbound>& outbounds)
{
    const uint256& myptx = session.members[session.my_idx].proTxHash;
    const int j = session.members[session.my_idx].share_index;
    for (const auto& dealer : session.qual) {
        if (dealer == myptx) continue;
        if (session.bad_members.count(dealer)) continue;
        auto cit = session.complaints_against.find(dealer);
        if (cit != session.complaints_against.end() && cit->second.count(myptx))
            continue; // already complained (self-delivered) — idempotent
        auto sit = session.received_shares.find(dealer);
        auto vit = session.phase1_vvecs.find(dealer);
        if (sit == session.received_shares.end() || vit == session.phase1_vvecs.end())
            continue; // nothing to check — absence is not a share complaint
        if (!PTX_DKG_FeldmanCheck(sit->second, vit->second, j)) {
            PTXDKGPhase2Msg m = PTX_DKG_BuildPhase2Msg(session, dealer, operator_sk);
            PTX_DKG_ReceivePhase2Msg(session, m);
            outbounds.push_back(MakeOutbound(NetMsgType::PTXQCOMPLAINT, m));
        }
    }
}

// JUSTIFY-window emission, IDEMPOTENT per complainant: answer every complaint
// filed against me that is not yet resolved.  Self-delivery runs the receiver
// branch (Feldman on the revealed share) which inserts into justified_for —
// also the re-entry guard.  Re-scanned EVERY step in the window so a complaint
// that arrives after window entry is still answered.  0 on the honest path.
void EmitJustifications(PTXDKGSession& session,
                        const CBLSSecretKey& operator_sk,
                        std::vector<PTXCeremonyOutbound>& outbounds)
{
    const uint256& myptx = session.members[session.my_idx].proTxHash;
    auto cit = session.complaints_against.find(myptx);
    if (cit == session.complaints_against.end()) return;
    const auto jit = session.justified_for.find(myptx);
    for (const auto& complainant : cit->second) {
        if (jit != session.justified_for.end() && jit->second.count(complainant))
            continue; // already resolved — idempotent
        PTXDKGPhase3Msg m = PTX_DKG_BuildPhase3Msg(session, complainant, operator_sk);
        PTX_DKG_ReceivePhase3Msg(session, m);
        outbounds.push_back(MakeOutbound(NetMsgType::PTXQJUSTIFICATION, m));
    }
}

} // namespace

int PTXCeremonyDeadlines::OffsetEnd(PTXDKGPhase p) const
{
    switch (p) {
        case PTXDKGPhase::HASH_COMMIT:
            return w_hashcommit;
        case PTXDKGPhase::CONTRIB:
            return w_hashcommit + w_contrib;
        case PTXDKGPhase::COMPLAINT:
            return w_hashcommit + w_contrib + w_complaint;
        case PTXDKGPhase::JUSTIFY:
            return w_hashcommit + w_contrib + w_complaint + w_justify;
        case PTXDKGPhase::PREMIT:
            return w_hashcommit + w_contrib + w_complaint + w_justify + w_premit;
        default:
            return 0; // FINALIZE closes immediately; IDLE/DONE/ABORTED unused
    }
}

PTXStepResult PTX_Ceremony_Step(PTXDKGSession& session,
                                PTXCeremonyDriverState& state,
                                int current_height,
                                const PTXCeremonyDeadlines& deadlines,
                                const CBLSSecretKey& operator_sk,
                                int formation_height,
                                std::vector<PTXCeremonyOutbound>& outbounds_out,
                                CMutableTransaction& dkgtx_out)
{
    outbounds_out.clear();

    // Invariant 2: the caller pre-reads height under cs_main and must have
    // released it — *session.cs and cs_main never coexist.
    AssertLockNotHeld(cs_main);

    // session.phase is written ONLY by this (step) thread — every Close* is a
    // step call; the drain's Receive* never writes phase.  So reading it here
    // WITHOUT the lock is safe (single phase-writer), and lets FINALIZE branch
    // out before taking *session.cs.
    const PTXDKGPhase ph = session.phase;

    if (ph == PTXDKGPhase::DONE)    return PTXStepResult::DONE;
    if (ph == PTXDKGPhase::ABORTED) return PTXStepResult::ABORTED;

    if (ph == PTXDKGPhase::FINALIZE) {
        // Invariant 2 carve-out: ClosePhase5 → SetPendingTx takes cs_main
        // (order cs_main→pending).  It MUST run outside *session.cs.  Sound:
        // at FINALIZE every ReceivePhaseN early-returns on its phase gate, so
        // the drain is quiesced and this terminal close is single-writer.
        AssertLockNotHeld(*session.cs);
        if (PTX_DKG_ClosePhase5(session, formation_height, dkgtx_out))
            return PTXStepResult::DONE;
        return PTXStepResult::ABORTED;
    }

    PTXStepResult result = PTXStepResult::WAITING;

    // ------------------------------------------------------------------
    // Phases HASH_COMMIT..PREMIT run under the session-state lock.  All
    // outbound messages are BUILT here (byte vectors) but NOT sent — the
    // caller transmits after we return (invariant 1: never send under cs).
    // ------------------------------------------------------------------
    LOCK(*session.cs);

    // (1) Phase-entry bookkeeping + one-time entry actions.
    if (state.entered_phase != ph) {
        state.entered_phase = ph;
        state.own_sent      = false;

        if (ph == PTXDKGPhase::HASH_COMMIT && !state.contrib_generated) {
            if (!PTX_DKG_GenerateLocalContrib(session)) {
                session.phase = PTXDKGPhase::ABORTED;
                return PTXStepResult::ABORTED;
            }
            state.contrib_generated = true;
        }
        if (ph == PTXDKGPhase::PREMIT) {
            // Compute aggregates BEFORE building the Phase 4 message
            // (BuildPhase4Msg requires phase4_computed).
            if (!PTX_DKG_ComputeSkShare(session) || !PTX_DKG_ComputeGroupPk(session)) {
                session.phase = PTXDKGPhase::ABORTED;
                return PTXStepResult::ABORTED;
            }
        }
    }

    // (2) Own emission.  P0/P1/P4: build-once broadcast (own_sent latch).
    //     P2/P3: idempotent scans, re-run EVERY step in the window (a
    //     complaint can arrive mid-window; the session sets double as the
    //     duplicate guards).  Build → self-deliver (immediate, under the
    //     lock — a local processing step, not a network hop) → stash the
    //     outbound for the caller.
    switch (ph) {
        case PTXDKGPhase::HASH_COMMIT:
            if (!state.own_sent) {
                PTXDKGPhase0Msg m = PTX_DKG_BuildPhase0Msg(session, operator_sk);
                PTX_DKG_ReceivePhase0Msg(session, m);
                outbounds_out.push_back(MakeOutbound(NetMsgType::PTXQHASHCOMMIT, m));
                state.own_sent = true;
            }
            break;
        case PTXDKGPhase::CONTRIB:
            if (!state.own_sent) {
                PTXDKGPhase1Msg m = PTX_DKG_BuildPhase1Msg(session, operator_sk);
                PTX_DKG_ReceivePhase1Msg(session, m);
                outbounds_out.push_back(MakeOutbound(NetMsgType::PTXQCONTRIB, m));
                state.own_sent = true;
            }
            break;
        case PTXDKGPhase::COMPLAINT:
            EmitComplaints(session, operator_sk, outbounds_out);
            break;
        case PTXDKGPhase::JUSTIFY:
            EmitJustifications(session, operator_sk, outbounds_out);
            break;
        case PTXDKGPhase::PREMIT:
            if (!state.own_sent) {
                PTXDKGPhase4Msg m = PTX_DKG_BuildPhase4Msg(session, operator_sk);
                PTX_DKG_ReceivePhase4Msg(session, m);
                outbounds_out.push_back(MakeOutbound(NetMsgType::PTXQPCOMMITMENT, m));
                state.own_sent = true;
            }
            break;
        default:
            break;
    }

    // (3) WINDOWED ADVANCE: close the phase exactly when tip-height reaches
    //     the formation-anchored window end.  Shared anchor + shared widths
    //     = lockstep across members; no early exit (the phase-skew catch —
    //     the IsComplete fast path is owed to SG-2b behind transport
    //     buffering of future-phase messages).
    const bool boundary =
        current_height >= formation_height + deadlines.OffsetEnd(ph);
    if (boundary) {
        switch (ph) {
            case PTXDKGPhase::HASH_COMMIT:
                result = PTX_DKG_ClosePhase0(session) ? PTXStepResult::PROGRESSED
                                                      : PTXStepResult::ABORTED;
                break;
            case PTXDKGPhase::CONTRIB:
                if (PTX_DKG_ClosePhase1(session)) {
                    // Decrypt AFTER close: bad_members (non-revealers) final,
                    // so we decrypt exactly the revealed set (incl. self).
                    DecryptRevealedShares(session, operator_sk);
                    result = PTXStepResult::PROGRESSED;
                } else {
                    result = PTXStepResult::ABORTED;
                }
                break;
            case PTXDKGPhase::COMPLAINT:
                result = PTX_DKG_ClosePhase2(session) ? PTXStepResult::PROGRESSED
                                                      : PTXStepResult::ABORTED;
                break;
            case PTXDKGPhase::JUSTIFY:
                result = PTX_DKG_ClosePhase3(session) ? PTXStepResult::PROGRESSED
                                                      : PTXStepResult::ABORTED;
                break;
            case PTXDKGPhase::PREMIT:
                result = PTX_DKG_ClosePhase4(session) ? PTXStepResult::PROGRESSED
                                                      : PTXStepResult::ABORTED;
                break;
            default:
                break;
        }

        // CP-4 observability (SG-2b-0): the per-node phase-transition timeline.
        // Pure read of already-updated session state — no control-flow effect.
        // Matches the unconditional LogPrintf convention of the sibling
        // ceremony lines (STARTED/DONE); volume is one line per phase per
        // ceremony per node.
        if (result == PTXStepResult::PROGRESSED || result == PTXStepResult::ABORTED) {
            LogPrintf("PTX ceremony: phase %s->%s height=%d my_idx=%d qual=%d bad=%d\n",
                      PhaseName(ph), PhaseName(session.phase), current_height,
                      session.my_idx, (int)session.qual.size(),
                      (int)session.bad_members.size());
        }
    }

    // ★ SG-5 S1 — THE ABSOLUTE STALL-OUT (ODC-050).  Reached ONLY by a step
    // that could not advance: the windowed-advance block above runs first, so
    // any step able to close its phase already did, and no slack constant is
    // needed.  FINALIZE/DONE/ABORTED returned early at the top of this
    // function, so a session whose tx is built and merely waiting to be mined
    // is structurally excluded — correct, its work is done.
    //
    // Width-INDEPENDENT (see the header): keyed on the formation interval, not
    // on OffsetEnd, so it still fires when an inflated width is itself the bug.
    // NODE-LOCAL orchestration, never consensus (the only production caller is
    // the formation thread; no validator reaches here), yet DETERMINISTIC: a
    // shared formation_height and a shared span mean every node aborts at the
    // same height.  Fail-safe per KDD-077 §4 — a hang is not a state.
    if (!boundary && deadlines.max_span > 0 &&
        current_height >= formation_height + deadlines.max_span) {
        LogPrintf("PTX ceremony: STALL-OUT at height %d (formation %d + max_span %d) "
                  "in phase %s — window end unreachable, ABORTING (ODC-050)\n",
                  current_height, formation_height, deadlines.max_span, PhaseName(ph));
        // *session.cs is ALREADY held from the top of this function (the
        // single lock scope that ends at the return below) — no re-acquire.
        session.phase = PTXDKGPhase::ABORTED;
        result = PTXStepResult::ABORTED;
    }

    return result;
    // *session.cs released here; the caller now transmits outbounds_out.
}
