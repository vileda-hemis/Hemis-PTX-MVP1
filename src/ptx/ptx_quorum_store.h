// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef PTX_QUORUM_STORE_H
#define PTX_QUORUM_STORE_H

#include "bls/bls_wrapper.h"
#include "serialize.h"
#include "sync.h"
#include "uint256.h"

#include <cstring>
#include <functional>   // W2.4 W4-f: injected eligibility sources
#include <map>
#include <memory>
#include <string>
#include <vector>

class CBlock;
class CBlockIndex;
class CEvoDB;
class CValidationState;
namespace Consensus { struct PTXFormationParams; }  // W2.4 W4-f (the ptx_formation.h idiom)

// ---------------------------------------------------------------------------
// W2.1 quorum registry + PTXDKG persistence (evodb).
//
// Persists one CPTXQuorumRecord per ACCEPTED PTXDKG at block-connect and erases
// it at block-disconnect — the LLMQ mined-commitment store pattern
// (llmq/quorums_blockprocessor: DB_MINED_COMMITMENT + inversed-height key),
// which is the discrete-record variant of the DGM evodb pattern.  The store
// only Write/Erase's through CEvoDB's CurTransaction: atomicity with the block,
// rollback, and EVODB_BEST_BLOCK chain-coherency all come from the surrounding
// ConnectTip/DisconnectTip machinery.
//
// Undo is EXPLICIT-ERASE, not DGM-style cache-only: this store has existence
// semantics (Exists(quorum_hash) backs ODC-030 uniqueness) and a height-keyed
// iteration index, so a stale key after reorg would be visible (a phantom
// quorum and a wrongly-firing uniqueness reject).  E-5 boundary held: undo does
// NOT re-pend the tx — re-submission policy is W2.2 formation's (producer-
// pending, register-marked).
// ---------------------------------------------------------------------------

// Quorum lifecycle states (full set designed at W2.1 — schema-stable so
// W2.2/W2.3/W2.4 slot in without a record migration).  Only ACTIVE has a
// producer at HEAD; the rest are PRODUCER-PENDING (see transition functions).
enum class PTXQuorumState : uint8_t {
    // Ceremony in flight.  NODE-LOCAL ONLY — never persisted (nothing is on
    // chain until the PTXDKG is accepted).  Producer: W2.2 formation trigger.
    FORMING = 0,
    // Accepted PTXDKG connected.  THE §C1 committed/active mark — the
    // forming-vs-active discriminant W1.3 deferred to the registry is exactly
    // "a persisted record with state == ACTIVE".  The active-predicate is
    // EVENT-based (accepted-at-connect), not provenance-based: W2.2 formation
    // produces the same connect event and inherits this predicate unchanged.
    ACTIVE = 1,
    // W2.3 rotation — KDD-063's "ROTATING->SUPERSEDED repurpose", landed at
    // KDD-072 P-b4 (value 2 UNCHANGED — persisted bytes stable).  A quorum this
    // state's record names was ACTIVE until its successor connected; the
    // predicate keeps it as-of-active below superseded_height.  Producer:
    // MarkSuperseded (ProcessBlock, successor connect).
    SUPERSEDED = 2,
    // W2.4 disband.  No producer, no writer at W2.1.
    DISBANDED = 3,
    // W2.4 reform (KDD-074/075/076, Decision-3 closure).  The BLAMELESS
    // terminal state: the quorum INSTANCE ended (idle past N_retire, or
    // rotation-impossible while due — KDD-076) and its members dissolved to
    // the pool with no §8 consequence.  DISTINCT from DISBANDED (failed) by
    // design: the two carry different member consequences and different
    // futures.  Producer: MarkReformed — DORMANT until W4-f wires the
    // block-driven transition.
    REFORMED = 4,
};

// Formation provenance — RESERVED, always UNSET at W2.1 (Confirmation 2).
// Provenance is NOT chain-derivable: a debug-injected and a ceremony-formed
// PTXDKG are byte-identical by design, and this record is consensus-derived
// state (a -reindex must reproduce records byte-identically on every node).
// Any future setter must preserve that reindex-determinism.  The LEGIT
// producer of an ACTIVE quorum is W2.2 formation (a completed ceremony); the
// W2.1 debug-injected substrate exercises the same connect event.
enum class PTXQuorumProvenance : uint8_t {
    UNSET  = 0,
    FORMED = 1, // reserved for W2.2
};

// One selected member at the formation anchor.  The FULL selected-11 is
// recorded (not just committed survivors): share_index is the KDD-052/060
// score-order rank materialized per member (KDD-061 — recovery-x must equal
// formation-x, gaps preserved under exclusion), and in_qual marks membership
// in the committed effective-QUAL list, so under-strength quorums
// (formed-11/completed-k, k >= t) are represented natively.
struct PTXQuorumMemberRecord {
    std::string node_id;
    uint256 proTxHash;
    uint8_t share_index{0}; // 1..11 — CalculateQuorum output position + 1
    bool in_qual{false};    // committed in payload.member_node_ids

    SERIALIZE_METHODS(PTXQuorumMemberRecord, obj)
    {
        READWRITE(LIMITED_STRING(obj.node_id, 40), obj.proTxHash,
                  obj.share_index, obj.in_qual);
    }
};

// The persisted per-quorum record — VERSIONED (Confirmation 1): evodb records
// are consensus-persisted chainstate, so future fields land ADDITIVE under a
// bumped nVersion and a reader keys on the version; no migration/reindex risk.
// Undo never deserializes the record (it erases by key derived from the
// disconnected block's payload), so undo is version-agnostic by construction.
class CPTXQuorumRecord
{
public:
    // KDD-072 P-b4 (ODC-042): v2 adds the state-transition height stamps that
    // make the as-of-height predicate answerable. v1 records (written pre-P-b4)
    // deserialize with both sentinels and can only be ACTIVE — no supersede or
    // disband producer existed when they were written — so the ACTIVE arm
    // answers them correctly. No migration.
    // v3 (W2.4 W4-c): + reformed_height, same additive contract — v1/v2 records
    // deserialize with the -1 sentinel and are never REFORMED (no producer
    // existed when they were written), so every arm answers them correctly.
    // No migration.
    // v4 (W2.4 lineage clock): + idle_since_height, same contract — pre-v4
    // records deserialize with the -1 sentinel and the age anchor falls back
    // to mined_height (the pre-lineage behaviour).  No migration.
    static const uint8_t CURRENT_VERSION = 4;

    uint8_t nVersion{CURRENT_VERSION};
    uint256 quorum_hash;             // formation anchor block hash — the identity
    int32_t formation_height{0};
    // blst_p1_affine_compress bytes (48).  A vector, not a C-array: the evodb
    // batch-sizing path instantiates the serializer under CSizeComputer, where
    // raw SER_READ s.read() does not compile; vector serialization is uniform
    // across all streams and version-stable (v1 defines the 1-byte size prefix).
    std::vector<uint8_t> group_pk_bytes;
    uint256 vvec_hash;
    std::vector<PTXQuorumMemberRecord> members; // full selected-11, score order
    uint8_t formed_size{0};          // selection size (11)
    uint8_t completed_size{0};       // committed effective-QUAL count (>= t)
    uint8_t state{static_cast<uint8_t>(PTXQuorumState::ACTIVE)};
    uint8_t provenance{static_cast<uint8_t>(PTXQuorumProvenance::UNSET)};
    uint256 accepted_txid;
    uint256 mined_block_hash;
    int32_t mined_height{0};
    // Reserved lifecycle fields — schema-stable for W2.2/W2.3/W2.4; written as
    // the sentinels below at W2.1 (no producer exists for any of them yet).
    int32_t last_rotation_height{-1};
    int32_t drift_offset{-1};        // assigned by W2.2 formation
    int32_t consecutive_inquorate_blocks{0};
    // v2 (KDD-072 P-b4, ODC-042) — state-transition height stamps, PINDEX-
    // DERIVED only (never wall-clock, never receive-order — the reindex-
    // determinism guarantee). Sentinel -1 = transition never happened.
    int32_t superseded_height{-1};   // stamped by MarkSuperseded (successor connect)
    int32_t disbanded_height{-1};    // DORMANT: no producer until W2.4 wires
                                     // MarkDisbanded's stamp — present now so
                                     // W2.4 adds a producer, not a schema
                                     // change under a live format (the P-b2
                                     // present-but-unfed posture)
    // v3 (W2.4 W4-c, KDD-074/075/076) — same contract as the v2 stamps:
    // pindex-derived only, sentinel -1 = never reformed.  Stamped by
    // MarkReformed IN THE SAME WRITE that sets state == REFORMED (the ODC-044
    // lesson: MarkDisbanded sets state but never stamps — its as-of arm reads
    // the sentinel and answers inactive-at-every-height; this writer must
    // never reproduce that).
    int32_t reformed_height{-1};     // stamped by MarkReformed (DORMANT until W4-f)
    // v4 (W2.4 LINEAGE CLOCK) — the height from which this SEAT'S silence is
    // measured.  Stamped at connect: fresh formation = own mined_height (the
    // seat's silence starts at birth — the young-quorum grace intact);
    // rotation successor = COPY of the predecessor's idle_since_height (the
    // seat's clock survives rotation — rotation must NOT reset it, or an idle
    // lineage rotates forever and never reforms: Hazard A through the
    // per-record clock, the pre-drill finding).  COPY not mutate: the
    // predecessor's own field is untouched (undo-clean — the successor's
    // record carries the copy and is erased whole on disconnect).  Sentinel
    // -1 (pre-v4 record): the age anchor falls back to mined_height.
    int32_t idle_since_height{-1};

    CPTXQuorumRecord() : group_pk_bytes(48, 0) {}

    SERIALIZE_METHODS(CPTXQuorumRecord, obj)
    {
        READWRITE(obj.nVersion);
        // v1 layout.  Kept verbatim (the versioning contract); v2+ fields
        // append below under the version conditional.
        READWRITE(obj.quorum_hash, obj.formation_height, obj.group_pk_bytes,
                  obj.vvec_hash, obj.members, obj.formed_size, obj.completed_size,
                  obj.state, obj.provenance, obj.accepted_txid, obj.mined_block_hash,
                  obj.mined_height, obj.last_rotation_height, obj.drift_offset,
                  obj.consecutive_inquorate_blocks);
        if (obj.nVersion >= 2) {
            READWRITE(obj.superseded_height, obj.disbanded_height);
        }
        if (obj.nVersion >= 3) {
            READWRITE(obj.reformed_height);
        }
        if (obj.nVersion >= 4) {
            READWRITE(obj.idle_since_height);
        }
    }
};

// ---------------------------------------------------------------------------
// KDD-072 P-b4 (ODC-042) — THE AS-OF-HEIGHT PREDICATE.  Pure function; the
// single source of "was this quorum active at height h" for every consumer.
//
// Semantics: "the store as it stands AFTER block h connects" — forced by the
// formation driver's timing (it fires from NotifyUpdatedBlockTip, post-connect
// of the anchor block), and the validator must agree with the driver or an
// honest formation self-rejects (the ODC-042 split).
//
//   active_at(h) = mined_height <= h
//               && (    state == ACTIVE
//                    || (state == SUPERSEDED && superseded_height > h)
//                    || (state == DISBANDED  && disbanded_height  > h) )
//
// ★ STRICT > on both stamp arms — a record superseded AT h is NOT active at h
// (the driver at anchor h already saw the flip; >= would keep it active for
// the validator only → pool divergence → chain split).  A stamped-state record
// carrying the -1 sentinel (corrupt) answers inactive at every h — fail-safe.
// The DISBANDED arm is VACUOUS at HEAD (no producer until W2.4) — written now
// so W2.4 adds a producer, not a predicate change.
// ---------------------------------------------------------------------------
bool PTX_QuorumRecordActiveAt(const CPTXQuorumRecord& rec, int nHeight);

// evodb key prefix accessor for the record store — the PTX_BLS_ShareDBPrefix
// precedent (test seeding reaches the same key the store reads).
const std::string& PTX_QuorumRecordDBPrefix();

class CPTXQuorumStore
{
private:
    CEvoDB& evoDb;
    mutable RecursiveMutex cs;
    // Lazy cache (DGM idiom): seeded on write, faulted in on read, erased on
    // undo.  Quorum count is small at W2.1; no cleanup pass — revisit at W2.3
    // when rotation multiplies records.
    std::map<uint256, CPTXQuorumRecord> recordCache GUARDED_BY(cs);

public:
    explicit CPTXQuorumStore(CEvoDB& _evoDb) : evoDb(_evoDb) {}

    // Block-connect: persist the accepted PTXDKG (<= 1 per block by
    // CheckPTXDKGBlockRules) as an ACTIVE record.  Checks run under fJustCheck
    // too; the Write is !fJustCheck only (DGM/LLMQ contract).  Enforces the
    // persist-boundary guards (never overwrite an existing quorum_hash; every
    // committed member must have a rank in the canonical selection) as DoS-100
    // rejects — CheckPTXDKGTx gains the same predicates validation-side at
    // W2.1-C4 for populate/assembler-visible rejection locality.
    bool ProcessBlock(const CBlock& block, const CBlockIndex* pindex,
                      CValidationState& state, bool fJustCheck);

    // Block-disconnect: explicit-erase of both keys + cache entry.  No re-pend
    // (E-5; W2.2 owns re-submission).
    bool UndoBlock(const CBlock& block, const CBlockIndex* pindex);

    bool HasQuorumRecord(const uint256& quorum_hash);
    bool GetQuorumRecord(const uint256& quorum_hash, CPTXQuorumRecord& ret);

    // W2.1 C3 — the router's query: all quorums with mined_height <= nHeight
    // whose state is ACTIVE, most-recently-mined first (inversed-height
    // iteration).  At W2.1 every record is ACTIVE (no disband producer); the
    // state filter is written now so W2.3/W2.4 states are excluded by
    // construction, not by retrofit.  Reorg-consistency: records erase on
    // disconnect, so the store answers for the CURRENT chain.
    std::vector<CPTXQuorumRecord> GetActiveQuorumsAtHeight(int nHeight);

    // ------------------------------------------------------------------
    // W2.1 C2 — state-machine skeleton: PRODUCER-PENDING transitions.
    //
    // These compile, are reviewed, and have NO production caller.  Each is
    // register-marked: "transition defined; falsification BOUND to <producer>
    // when it exists."  They are NOT claimed tested — no synthetic
    // direct-state-injection test exercises them (the scope line the W2.1
    // plan holds).  The full transition table lives in DKG_DESIGN_DOC_v1
    // (W2.1 state-machine section).
    // ------------------------------------------------------------------

    // T-D (none)->FORMING.  PRODUCER-PENDING (W2.2 formation trigger).
    // FORMING is node-local only — never persisted (nothing is on chain until
    // the PTXDKG is accepted); an in-memory slot keyed by the formation
    // anchor.  Falsification bound to W2.2.
    void MarkForming(const uint256& quorum_hash, int formation_height);

    // T-F FORMING->aborted (§C1 abort-clears-slot authorization hook).
    // PRODUCER-PENDING (W2.2 ceremony failure/abort).  Clearing the FORMING
    // entry is what will AUTHORIZE the sk-share clear path W2.2 must build
    // (no runtime clear exists at HEAD — ptx_bls static-init only).
    // Falsification bound to W2.2.
    void ClearForming(const uint256& quorum_hash);

    // T-E consumption half of FORMING->ACTIVE.  PRODUCER-PENDING (W2.2).
    // The ACTIVE half (persist-at-connect) is LIVE and falsified (C1);
    // consuming a FORMING entry when its PTXDKG connects is the pending part.
    // ProcessBlock will call this once W2.2 populates FORMING entries; today
    // it is a no-op on an always-empty map.  Falsification bound to W2.2.
    void ConsumeFormingOnConnect(const uint256& quorum_hash);

    // T-H ACTIVE->DISBANDED.  PRODUCER-PENDING (W2.4 disband trigger:
    // consecutive_inquorate_blocks == 30, KDD-047).  Rewrites the persisted
    // record's state (versioned layout unchanged).  NOTE: W2.4 must wire the
    // block-event driving this AND its disconnect-undo (state-mutation undo
    // mechanism deliberately not designed at W2.1 — see design doc).  ★ P-b4
    // forward-bind: when wired, T-H must ALSO stamp disbanded_height (the
    // record field exists, the predicate arm exists — W2.4 adds the producer).
    // Falsification bound to W2.4.
    bool MarkDisbanded(const uint256& quorum_hash, int disband_height);

    // KDD-072 P-b4 (KDD-063 swap, connect half): predecessor ACTIVE->SUPERSEDED
    // + superseded_height stamped = the successor's connect height (pindex-
    // derived).  REFUSE-unless-ACTIVE (returns false; a double-flip or a flip
    // of a missing/disbanded record is a no-op) — the UndoPromote idempotence
    // posture.  Rewrites through evoDb's CurTransaction (block-atomic, the
    // MarkDisbanded shape).  Caller: ProcessBlock on an accepted v2 rotation.
    bool MarkSuperseded(const uint256& quorum_hash, int superseded_at_height);

    // KDD-072 P-b4: the disconnect half — SUPERSEDED->ACTIVE, stamp cleared to
    // -1.  REFUSE-unless-SUPERSEDED (idempotent no-op otherwise).  Restore-to-
    // ACTIVE is unconditionally correct: V12 (P-b3) admits a successor only if
    // its predecessor was ACTIVE at connect, so the pre-flip state is known
    // without an undo journal — the revert is a pure function of the
    // disconnecting successor's payload.  Caller: UndoBlock.
    bool RestoreActiveOnUndo(const uint256& quorum_hash);

    // W2.4 W4-c (KDD-074/075/076): ACTIVE->REFORMED, reformed_height STAMPED
    // in the same write (the ODC-044 lesson — never state-without-stamp).
    // REFUSE-unless-ACTIVE (the MarkSuperseded posture): first-transition-wins
    // arbitration per KDD-075's mechanics note.  DORMANT: no production caller
    // until W4-f wires the rate-limited block-driven producer.
    bool MarkReformed(const uint256& quorum_hash, int reform_height);

    // W2.4 W4-c: the disconnect twin — REFORMED->ACTIVE, stamp cleared to -1.
    // REFUSE-unless-REFORMED (idempotent no-op otherwise).  Restore-to-ACTIVE
    // is unconditionally correct: MarkReformed refuses unless ACTIVE, so the
    // pre-flip state is known without an undo journal (the P-b4 argument).
    // The undo surface is this revert ALONE — idleness is DERIVED at boundary
    // time, never stored, so there is no counter to undo (the W2.4 planning
    // recon's derive-don't-store decision).  Caller: UndoBlock (W4-f).
    bool RestoreReformedOnUndo(const uint256& quorum_hash);

    // ------------------------------------------------------------------
    // KDD-072 P-b5 — the predecessor-uniqueness index ("pq_p"): at most one
    // successor per predecessor, as an EXPLICIT evodb key. ★ This index is the
    // PRIMARY durable guard (KDD-072 §6): a racing second rotation also fails
    // the V12b as-of-ACTIVE check, but that rejection depends on state-read
    // semantics a refactor could change — pq_p is an existence key with no
    // semantics to drift. Written at connect, erased at disconnect (a reorg
    // that unwinds the first successor RE-ALLOWS a second, the V9 pattern).
    // ------------------------------------------------------------------

    // True iff a successor has already rotated this predecessor (pq_p set).
    bool HasSuccessorOf(const uint256& predecessor_qh);

    // V12d — ONE implementation, TWO consensus call sites (validator + the
    // ProcessBlock guard, the CheckRotationAndResolve pattern): reject with
    // "ptxdkg-predecessor-already-rotated" if pq_p exists. Distinct rejection
    // from V12b's "not-active": an already-rotated predecessor is a different
    // failure than a superseded one, even though the race trips both.
    bool CheckPredecessorUnrotated(const uint256& predecessor_qh,
                                   CValidationState& state);

    // Connect-write, REFUSE-unless-absent (the ProcessBlock guard posture): a
    // second write to the same predecessor key is a defect, not an overwrite.
    // Returns false without writing if pq_p already exists.
    bool WriteSuccessorOf(const uint256& predecessor_qh, const uint256& successor_qh);

    // Undo-erase: idempotent (erase of an absent key is a no-op), payload-keyed,
    // rides the same CurTransaction atomicity as the record ops. Joins
    // RestoreActiveOnUndo + PTX_BLS_UndoPromote in UndoBlock's rotation block —
    // no ordering constraint among the three.
    void EraseSuccessorOf(const uint256& predecessor_qh);

    // ------------------------------------------------------------------
    // KDD-072 P-b6b — THE CURRENT-RESIDUE RETIREMENT (the KDD-070 §5 bound,
    // enforced).
    //
    // THE RESIDUE: a member that held a quorum's CURRENT share but did NOT
    // complete its rotation ceremony gets no PENDING successor share, so
    // PTX_BLS_Promote no-ops for them (key isolation) and their old share stays
    // role-CURRENT for a quorum the chain has marked SUPERSEDED — indefinitely,
    // across restarts (CURRENT has no TTL; depth-discard covers only
    // SUPERSEDED_RETAINED; startup reconciliation keeps it because the
    // superseded record IS still on the active chain).
    //
    // ★ WHY THIS IS WORSE THAN WHAT §5 FORBIDS: KDD-070 §5 bounds "two live
    // keys for the same membership" to maxreorg+margin blocks and refuses to
    // sign a SUPERSEDED share — but that guard keys on the ROLE, and a residue
    // is role-CURRENT, so the refusal never fires on it. §5's bound was
    // therefore unenforced for this case. This sweep restores it using exactly
    // the depth §6 already justifies (DEFAULT_MAX_REORG_DEPTH +
    // PTX_SUPERSEDED_REORG_MARGIN = 120).
    //
    // ★ IRREVERSIBILITY (deliberate): the share is DELETED, not retained. If
    // the supersede is later reorged out, the member holds nothing for the
    // restored quorum — correct, because they had missed that rotation and hold
    // no successor share either; and the 120-block depth means no permitted
    // reorg can reach the supersede by the time this fires.
    //
    // STORE-SIDE by necessity: the test is "is this share's quorum SUPERSEDED,
    // and how deep?", which reads CPTXQuorumRecord.superseded_height — record
    // state ptx_bls.cpp deliberately cannot see (it is pure over HeldShare).
    // Returns the number retired.
    // ------------------------------------------------------------------
    size_t RetireSupersededResidues(int tip_height);

    // ------------------------------------------------------------------
    // W2.4 W4-f — THE REFORM PRODUCER (the un-stub; KDD-074/075/076 live).
    // ------------------------------------------------------------------
    // Called from ProcessBlock (connect, !fJustCheck, BEFORE the no-PTXDKG
    // early-return — reform is not tx-driven) at FORMATION BOUNDARIES only
    // (height % interval == 0 — the same one evaluation point as the yield
    // and the rotation trigger, KDD-075).  Evaluates the ACTIVE set through
    // the shared TerminalEligible, orders candidates least-recently-active
    // (in-window last-attributed height; an idle-eligible has none by
    // definition, so LRA collapses to mined_height — record antiquity),
    // drains ONE through SelectReformCandidate, MarkReformed's it.  The
    // eligibility sources are injected (the W4-d/e posture); ProcessBlock
    // passes disk + resolver lambdas, tests inject.  Returns reforms fired
    // (0 or 1).  Gate-off params make this a no-op (dormant on main/test).
    size_t MaybeReformAtBoundary(
            const CBlockIndex* pindex,
            const Consensus::PTXFormationParams& params,
            const std::function<bool(const CBlockIndex*, CBlock&)>& read_block,
            const std::function<bool(const CPTXQuorumRecord&, const CBlockIndex*)>& impossible_at);

    // W4-f disconnect: THE STAMP IS THE UNDO JOURNAL — restore every record
    // whose reformed_height equals the disconnecting height (at most one by
    // construction: one reform per boundary through the limiter).  Runs from
    // UndoBlock on EVERY disconnect (cheap record walk; a non-reform height
    // matches nothing — idempotent no-op).  Joins the P-b4/P-b5 disconnect
    // composition with no ordering constraint (disjoint records/keys); the
    // downstream fresh formation needs NO reach-in — it disconnects first
    // (tip-first unwind) via its own record erase.
    size_t RestoreReformedAtHeight(int height);

    bool IsForming(const uint256& quorum_hash) const;

private:
    // FORMING entries (node-local, in-memory only; W2.2 is the sole producer).
    std::map<uint256, int> formingEntries GUARDED_BY(cs);
};

extern std::unique_ptr<CPTXQuorumStore> ptxQuorumStore;

// KDD-070 P2: startup reconciliation of held sk-shares against chain records.
// ★ MUST be called AFTER chain load + store population (§3 ordering) — from
// LoadTierTwo (init.cpp), NOT from store construction. Loads persisted shares,
// discards orphans (quorum_hash not a record on the active chain), and warns for
// "in_qual on-chain but no share held". Safe to no-op if the store/evoDb are null.
void PTX_ReconcileHeldSharesOnStart();

// Pure helper: warn once for each ACTIVE quorum where node_id is an in_qual
// member but no share is held (ODC-035 degraded state). Logs, never throws.
// Returns the number of warnings. Injectable for unit tests.
int PTX_WarnMissingSharesForNode(const std::vector<CPTXQuorumRecord>& active,
                                 const std::string& node_id);

// ---------------------------------------------------------------------------
// SG-3 — DKG signing material selection (the index-space reconciliation).
//
// PURE: records in -> ctx out (zero globals; fully unit-testable — the
// PTX_Formation_BuildPool convention).  The caller supplies the ACTIVE set.
//
// x = PTXQuorumMemberRecord.share_index, which is 1-BASED score-order rank
// (assigned ptx_quorum_store.cpp:109 as i+1 over the canonical selection) —
// the SAME basis the DKG evaluated its polynomial at (ptx_dkg.cpp:261 ->
// :318).  Gaps are preserved: the record holds the full selected-11 and
// in_qual marks the committed survivors, so an in_qual subset carries
// non-contiguous x's — exactly the original evaluation points Lagrange needs.
// ---------------------------------------------------------------------------

struct PTXDKGSigningCtx {
    // An ACTIVE quorum record existed in the supplied set (regardless of
    // usability).  Load failure with this TRUE is a HARD ERROR for the caller:
    // it must NOT fall through to the trusted-dealer path (fail-closed).
    bool                      quorum_present{false};
    bool                      active{false};   // usable signing material below
    uint256                   quorum_hash;
    std::vector<std::string>  member_ids;      // in_qual (committed effective-QUAL) only
    std::map<std::string,int> share_index;     // node_id -> 1-based score-order x
    int                       threshold{0};    // t = majority(formed_size); QUORUM-scoped, NOT registry-derived (ODC-036)
    std::vector<uint8_t>      group_pk;        // 48 compressed bytes, as committed
};

// Select the signing quorum from an ACTIVE set.
//
// SELECTION RULE (KDD-066, PROVISIONAL — owed to W2.1): highest formation_height, ties
// broken by LOWEST quorum_hash lexicographically.  Deterministic, but this is
// a de facto multi-quorum selection policy and it does NOT implement the
// registered N>1 design (deterministic shuffle over
// H(anchor_block_hash || game_id || roll_index), chain-determined and
// ungrindable).  No consensus surface today — coordinator recovery side only.
// The signing threshold is QUORUM-SCOPED: t = majority(formed_size) — a
// property of the selected quorum, NOT of the coordinator's node registry
// (ODC-036: deriving it from registry size mis-signs when registered-nodes !=
// quorum-size).  No threshold parameter: it is derived here and returned in
// ctx.threshold, the single source the roll path reads.
// §7.4 ROUTING (W2.5a, KDD-079 §7(ii)) — distribution across the ACTIVE set,
// NOT newest-wins.  tip_hash is the SELECTION INPUT (decision A'): unforgeable
// by the caller, unlike round_seed whose caller_salt is free-form hex and would
// make selection a grindable targeting oracle.  PURE (the input is passed in),
// so it stays unit-testable.  Selection is over the ACTIVE vector SORTED by
// quorum_hash — storage iteration order must never leak into routing.
// ★ Residual (documented, not fixed here): a caller can TIMING-grind by waiting
// for the tip to reshuffle; the structural fix is commit-reveal, which needs a
// flow change (selection precedes the tx, so no inclusion hash exists here).
PTXDKGSigningCtx PTX_SelectDKGSigningCtx(const std::vector<CPTXQuorumRecord>& active,
                                         const uint256& tip_hash);

#endif // PTX_QUORUM_STORE_H
