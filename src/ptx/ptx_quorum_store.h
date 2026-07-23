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
#include <map>
#include <memory>
#include <string>
#include <vector>

class CBlock;
class CBlockIndex;
class CEvoDB;
class CValidationState;

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
    // W2.3 rotation.  In the enum for schema stability; no producer, no writer.
    ROTATING = 2,
    // W2.4 disband.  No producer, no writer at W2.1.
    DISBANDED = 3,
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
    static const uint8_t CURRENT_VERSION = 1;

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

    CPTXQuorumRecord() : group_pk_bytes(48, 0) {}

    SERIALIZE_METHODS(CPTXQuorumRecord, obj)
    {
        READWRITE(obj.nVersion);
        // v1 layout.  Future versions: keep this block verbatim, append new
        // fields under `if (obj.nVersion >= 2) READWRITE(...)`.
        READWRITE(obj.quorum_hash, obj.formation_height, obj.group_pk_bytes,
                  obj.vvec_hash, obj.members, obj.formed_size, obj.completed_size,
                  obj.state, obj.provenance, obj.accepted_txid, obj.mined_block_hash,
                  obj.mined_height, obj.last_rotation_height, obj.drift_offset,
                  obj.consecutive_inquorate_blocks);
    }
};

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
    // mechanism deliberately not designed at W2.1 — see design doc).
    // Falsification bound to W2.4.
    bool MarkDisbanded(const uint256& quorum_hash, int disband_height);

    bool IsForming(const uint256& quorum_hash) const;

private:
    // FORMING entries (node-local, in-memory only; W2.2 is the sole producer).
    std::map<uint256, int> formingEntries GUARDED_BY(cs);
};

extern std::unique_ptr<CPTXQuorumStore> ptxQuorumStore;

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
PTXDKGSigningCtx PTX_SelectDKGSigningCtx(const std::vector<CPTXQuorumRecord>& active);

#endif // PTX_QUORUM_STORE_H
