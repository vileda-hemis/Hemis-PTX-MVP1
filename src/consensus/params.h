// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include "amount.h"
#include "libzerocoin/Params.h"
#include "optional.h"
#include "uint256.h"
#include <map>
#include <string>

namespace Consensus {

/**
* Index into Params.vUpgrades and NetworkUpgradeInfo
*
* Being array indices, these MUST be numbered consecutively.
*
* The order of these indices MUST match the order of the upgrades on-chain, as
* several functions depend on the enum being sorted.
*/
enum UpgradeIndex : uint32_t {
    BASE_NETWORK,
    UPGRADE_POS,
    UPGRADE_POS_V2,
    UPGRADE_ZC,
    UPGRADE_ZC_V2,
    UPGRADE_BIP65,
    UPGRADE_ZC_PUBLIC,
    UPGRADE_V3_4,
    UPGRADE_V4_0,
    UPGRADE_V5_0,
    UPGRADE_V5_2,
    UPGRADE_V5_3,
    UPGRADE_V5_5,
    UPGRADE_GM_ENABLE,
    UPGRADE_V6_0,
    UPGRADE_TESTDUMMY,
    // NOTE: Also add new upgrades to NetworkUpgradeInfo in upgrades.cpp
    MAX_NETWORK_UPGRADES
};

struct NetworkUpgrade {
    /**
     * The first protocol version which will understand the new consensus rules
     */
    int nProtocolVersion;

    /**
     * Height of the first block for which the new consensus rules will be active
     */
    int nActivationHeight;

    /**
     * Special value for nActivationHeight indicating that the upgrade is always active.
     * This is useful for testing, as it means tests don't need to deal with the activation
     * process (namely, faking a chain of somewhat-arbitrary length).
     *
     * New blockchains that want to enable upgrade rules from the beginning can also use
     * this value. However, additional care must be taken to ensure the genesis block
     * satisfies the enabled rules.
     */
    static constexpr int ALWAYS_ACTIVE = 0;

    /**
     * Special value for nActivationHeight indicating that the upgrade will never activate.
     * This is useful when adding upgrade code that has a testnet activation height, but
     * should remain disabled on mainnet.
     */
    static constexpr int NO_ACTIVATION_HEIGHT = -1;

    /**
     * The hash of the block at height nActivationHeight, if known. This is set manually
     * after a network upgrade activates.
     *
     * We use this in IsInitialBlockDownload to detect whether we are potentially being
     * fed a fake alternate chain. We use NU activation blocks for this purpose instead of
     * the checkpoint blocks, because network upgrades (should) have significantly more
     * scrutiny than regular releases. nMinimumChainWork MUST be set to at least the chain
     * work of this block, otherwise this detection will have false positives.
     */
    Optional<uint256> hashActivationBlock;
};

enum LLMQType : uint8_t
{
    LLMQ_NONE = 0xff,

    LLMQ_50_60 = 1, // 50 members, 30 (60%) threshold, one per hour
    LLMQ_400_60 = 2, // 400 members, 240 (60%) threshold, one every 12 hours
    LLMQ_400_85 = 3, // 400 members, 340 (85%) threshold, one every 24 hours

    // for testing only
    LLMQ_TEST = 100, // 3 members, 2 (66%) threshold, one per hour. Params might differ when -llmqtestparams is used

    // KDD-065: PTX ceremony member-connection pseudo-type. A TierTwoConnMan
    // MAP KEY ONLY (setQuorumNodes/removeQuorumNodes) — the connman path is
    // type-opaque. CONSTRAINT: never insert this type into consensus.llmqs
    // and never key a params lookup (GetLLMQParams/llmqs.at) by it; it names
    // no LLMQ and has no LLMQParams.
    LLMQ_TYPE_PTX_CEREMONY = 200,
};

// Configures a LLMQ and its DKG
// See https://github.com/dashpay/dips/blob/master/dip-0006.md for more details
struct LLMQParams {
    LLMQType type;

    // not consensus critical, only used in logging, RPC and UI
    std::string name;

    // the size of the quorum, e.g. 50 or 400
    int size;

    // The minimum number of valid members after the DKK. If less members are determined valid, no commitment can be
    // created. Should be higher then the threshold to allow some room for failing nodes, otherwise quorum might end up
    // not being able to ever created a recovered signature if more nodes fail after the DKG
    int minSize;

    // The threshold required to recover a final signature. Should be at least 50%+1 of the quorum size. This value
    // also controls the size of the public key verification vector and has a large influence on the performance of
    // recovery. It also influences the amount of minimum messages that need to be exchanged for a single signing session.
    // This value has the most influence on the security of the quorum. The number of total malicious gamemasters
    // required to negatively influence signing sessions highly correlates to the threshold percentage.
    int threshold;

    // The interval in number blocks for DKGs and the creation of LLMQs. If set to 60 for example, a DKG will start
    // every 60 blocks, which is approximately once every hour.
    int dkgInterval;

    // The number of blocks per phase in a DKG session. There are 6 phases plus the mining phase that need to be processed
    // per DKG. Set this value to a number of blocks so that each phase has enough time to propagate all required
    // messages to all members before the next phase starts. If blocks are produced too fast, whole DKG sessions will
    // fail.
    int dkgPhaseBlocks;

    // The starting block inside the DKG interval for when mining of commitments starts. The value is inclusive.
    // Starting from this block, the inclusion of (possibly null) commitments is enforced until the first non-null
    // commitment is mined. The chosen value should be at least 5 * dkgPhaseBlocks so that it starts right after the
    // finalization phase.
    int dkgMiningWindowStart;

    // The ending block inside the DKG interval for when mining of commitments ends. The value is inclusive.
    // Choose a value so that miners have enough time to receive the commitment and mine it. Also take into consideration
    // that miners might omit real commitments and revert to always including null commitments. The mining window should
    // be large enough so that other miners have a chance to produce a block containing a non-null commitment. The window
    // should at the same time not be too large so that not too much space is wasted with null commitments in case a DKG
    // session failed.
    int dkgMiningWindowEnd;

    // In the complaint phase, members will vote on other members being bad (missing valid contribution). If at least
    // dkgBadVotesThreshold have voted for another member to be bad, it will considered to be bad by all other members
    // as well. This serves as a protection against late-comers who send their contribution on the bring of
    // phase-transition, which would otherwise result in inconsistent views of the valid members set
    int dkgBadVotesThreshold;

    // Number of quorums to consider "active" for signing sessions
    int signingActiveQuorumCount;

    // Used for inter-quorum communication. This is the number of quorums for which we should keep old connections. This
    // should be at least one more then the active quorums set.
    int keepOldConnections;

    // How many members should we try to send all sigShares to before we give up.
    int recoveryMembers;

    // The limit of blocks up until where the dkg qfc will be accepted.
    int cacheDkgInterval;
};

// W2.2 SG-1b — PTX formation cycle parameters. A single struct (not an
// LLMQ-style map): there is exactly one PTX quorum type; the map is a cheap
// refactor if types ever multiply (SG-1b plan-gate decision 2, 2026-07-13).
struct PTXFormationParams {
    // not consensus critical, only used in logging
    std::string name;

    // N — the formation cadence: a formation boundary occurs at every height
    // with height > 0 && height % N == 0 (height 0 excluded by construction —
    // no formation from genesis). N is a SECURITY CEILING only
    // (handover-at-accept keeps rotation available across boundaries, KDD-063);
    // it must exceed the ceremony floor M (~47 blocks) so a new boundary
    // cannot fire before the prior ceremony completes.
    // ★ W2.5a (KDD-079) — THE DECOUPLE.  This was ONE field doing THREE
    // semantically distinct jobs; they were numerically identical only
    // because there was one knob, and that conflation is what produced the
    // "53,000-block pile-up" the W2.5 recon computed (boundary cadence and
    // rotation interval are NOT the same quantity).  It also propagated INTO
    // new code: PTX_Formation_ForcedReformGraceElapsed used the single param
    // for both its boundary-step and its due-test, which is a defect at
    // divergent values (see its two-parameter signature).
    //
    // ★ DEFAULTS PRESERVE L=1 BEHAVIOUR: all three default to the old
    // nFormationInterval value, so behaviour is byte-identical until a
    // multi-quorum config sets them apart (the P-b3b param-gate idiom — the
    // split needs no gate of its own).
    //
    // The boundary cadence: V11's `height % N == 0` (inherited through
    // PTX_Formation_IsBoundary — the single consensus dependency) and the
    // fresh-formation schedule.  Staggering emerges from P-b6b's lowest-hash
    // tie-break at this cadence, which is why no drift producer is needed.
    int nBoundaryInterval;
    // The rotation AGE test only: a quorum is due once this many blocks have
    // elapsed since its own formation anchor.  KDD-045's key-compromise
    // window is bounded by THIS, not by the boundary cadence.
    int nRotationInterval;
    // The ODC-050 ceremony stall-out budget only.  ★ MUST NOT track the
    // boundary cadence: a ~27-block ceremony (drill: formation 1120 ->
    // connect 1147) under a 30-block boundary interval would be aborted
    // mid-flight by its own safety mechanism.
    int nCeremonyBudget;
    // ★ W2.5a Guard 1 (KDD-079 §3) — the network's DECLARED quorum count.
    // Guard 1 is unenforceable without a check target: the real L is chain
    // state (GetActiveQuorumsAtHeight) and unknown at startup, so the config
    // must declare what it is provisioned for.  Default 1 = single-quorum,
    // which is what every network runs today and what makes the check a
    // no-op on the current defaults.
    int nSupportedQuorums;

    // ------------------------------------------------------------------
    // W2.4 W4-e (KDD-074/075/076) — THE TERMINAL-ELIGIBILITY GATE.
    // ★ ALL THREE DEFAULT 0 == DISABLED (the P-b3b flip posture): with the
    // gate off, nothing is ever terminal-eligible, the KDD-075 yield never
    // fires, the limiter never selects — the whole W2.4 lifecycle is
    // dormant-by-parameter.  The defaults are the MAINNET posture:
    // main and test leave them 0; regtest, ptxbea AND ptxtestnet set them LIVE
    // in chainparams via the ptxFormation aggregate initializer (currently
    // {…, 200, 1, 40} on all three).  Mainnet reform stays off until
    // deliberately enabled.
    // ★ CORRECTED 2026-08-25 (BUG-053).  This sentence used to read
    // "main/test/ptxtest leave them 0 … drill chains only", which was true
    // when written and was falsified by `4e1c9e6` (2026-08-21): that commit
    // renamed the network's params from {"ptxtest", 80, 80, 80, 1} to
    // {"ptxtestnet", …, 200, 1, 40, 0, 0} — turning the gate ON for a LAUNCH
    // chain, not a drill chain — and the structural row that guards this
    // property keyed off the OLD name, so the same commit that made the
    // sentence false also blinded the only thing checking it.  "drill chains
    // only" is therefore retired as a description: the correct statement is
    // MAINNET-POSTURE networks stay dormant, and ptxtestnet is not one.
    // The per-field reasoning for ptxtestnet lives at chainparams.cpp:958-989
    // and doc/ptx/PTX_TESTNET_GENESIS_CONFIG.md §4.  ★ GREP TRAP
    // (cost a wrong eviction-model call, 2026-08-16): these fields are set
    // by POSITION in the chainparams aggregate init, so a search for the
    // field NAME finds only this header and tests — read the
    // `consensus.ptxFormation = {...}` lines against the field order here.
    // ------------------------------------------------------------------
    // KDD-074 idle window: a quorum with no attributed roll in the last
    // nRetireWindow blocks is idle-eligible.  0 = idle arm DISABLED.
    int nRetireWindow{0};
    // KDD-076 grace: forced reform only after due-AND-rotation-impossible at
    // this many consecutive boundaries.  0 = forced-reform arm DISABLED.
    int nReformGrace{0};
    // KDD-074 rate limiter: at most one reform transition per this many
    // blocks (least-recently-active first).  0 = limiter selects NOTHING
    // (the transition stays dormant even if eligibility is enabled).
    int nReformRateWindow{0};
    // ★ BUG-036 activation: from this height the reform pacing is STATELESS
    // (fires at heights divisible by stride = ceil(rate/boundary)*boundary — a
    // pure height predicate; derive-don't-store) and the one-boundary stamp
    // SELF-HEAL is armed. Below it, the legacy stored-max pacing applies so
    // pre-activation history replays byte-identically on reindex/IBD (the
    // h385 lesson: never let a new rule re-derive old history differently).
    // 0 = stateless from genesis (fresh chains).
    int nReformStatelessHeight{0};

    // ★ V11 ACTIVATION GATE (pre-testnet, 2026-08-19). V11 requires a PTXDKG's
    // formation anchor to sit ON the boundary schedule (height % N == 0). That
    // predicate reads nBoundaryInterval, so CHANGING THE CADENCE ON A CHAIN WITH
    // HISTORY retroactively invalidates every PTXDKG whose anchor was on the old
    // schedule: a node syncing from genesis rejects blocks the network accepted.
    // SPLIT-ON-RESYNC — the h385 shape (never let a new rule re-derive old
    // history differently), and the reason nReformStatelessHeight above exists.
    //
    // Semantics deliberately mirror nReformStatelessHeight: the predicate is on
    // the height of the BLOCK BEING CONNECTED, not on the anchor's height, so
    // activation cleanly partitions "mined before" (grandfathered) from "mined
    // after" (must be on-boundary). Gating on the anchor instead would let a NEW
    // off-boundary PTXDKG in simply by naming an old anchor.
    //
    // 0 = ENFORCE FROM GENESIS — correct for every fresh chain, and the current
    // value on ALL networks: ptxbea has enforced V11 since its 2026-08-15 fresh
    // genesis, so 0 preserves byte-identical replay there. The knob exists for a
    // chain that accumulates history under one cadence and later changes it.
    int nBoundaryEnforceHeight{0};
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    bool fPowAllowMinDifficultyBlocks;
    bool fPowNoRetargeting;
    uint256 powLimit;
    uint256 posLimitV1;
    uint256 posLimitV2;
    int nBudgetCycleBlocks;
    int nBudgetFeeConfirmations;
    int nCoinbaseMaturity;
    int nFutureTimeDriftPoW;
    int nFutureTimeDriftPoS;
    CAmount nMaxMoneyOut;
    CAmount nGMCollateralAmt;
    int nGMCollateralMinConf;
    CAmount nGMBlockReward;
    CAmount nNewGMBlockReward;
    int64_t nProposalEstablishmentTime;
    int nStakeMinAge;
    int nStakeMinDepth;
    int64_t nTargetTimespan;
    int64_t nTargetTimespanV2;
    int64_t nTargetSpacing;
    int nTimeSlotLength;
    int nMaxProposalPayments;

    // spork keys
    std::string strSporkPubKey;
    std::string strSporkPubKeyOld;
    int64_t nTime_EnforceNewSporkKey;
    int64_t nTime_RejectOldSporkKey;

    // height-based activations
    int height_last_invalid_UTXO;
    int height_last_ZC_AccumCheckpoint;
    int height_last_ZC_WrappedSerials;

    // validation by-pass
    int64_t nHemisBadBlockTime;
    unsigned int nHemisBadBlockBits;

    // Map with network updates
    NetworkUpgrade vUpgrades[MAX_NETWORK_UPGRADES];

    int64_t TargetTimespan(const bool fV2 = true) const { return fV2 ? nTargetTimespanV2 : nTargetTimespan; }
    uint256 ProofOfStakeLimit(const bool fV2) const { return fV2 ? posLimitV2 : posLimitV1; }
    bool MoneyRange(const CAmount& nValue) const { return (nValue >= 0 && nValue <= nMaxMoneyOut); }
    bool IsTimeProtocolV2(const int nHeight) const { return NetworkUpgradeActive(nHeight, UPGRADE_V4_0); }
    int GamemasterCollateralMinConf() const { return nGMCollateralMinConf; }

    int FutureBlockTimeDrift(const int nHeight) const
    {
        // PoS (TimeV2): 14 seconds
        if (IsTimeProtocolV2(nHeight)) return nTimeSlotLength - 1;
        // PoS (TimeV1): 3 minutes - PoW: 2 hours
        return (NetworkUpgradeActive(nHeight, UPGRADE_POS) ? nFutureTimeDriftPoS : nFutureTimeDriftPoW);
    }

    bool IsValidBlockTimeStamp(const int64_t nTime, const int nHeight) const
    {
        // Before time protocol V2, blocks can have arbitrary timestamps
        if (!IsTimeProtocolV2(nHeight)) return true;
        // Time protocol v2 requires time in slots
        return (nTime % nTimeSlotLength) == 0;
    }

    bool HasStakeMinAgeOrDepth(const int contextHeight, const uint32_t contextTime,
            const int utxoFromBlockHeight, const uint32_t utxoFromBlockTime) const
    {
        // before stake modifier V2, we require the utxo to be nStakeMinAge old
        if (!NetworkUpgradeActive(contextHeight, Consensus::UPGRADE_V3_4))
            return (utxoFromBlockTime + nStakeMinAge <= contextTime);
        // with stake modifier V2+, we require the utxo to be nStakeMinDepth deep in the chain
        return (contextHeight - utxoFromBlockHeight >= nStakeMinDepth);
    }


    /*
     * (Legacy) Zerocoin consensus params
     */
    std::string ZC_Modulus;  // parsed in Zerocoin_Params (either as hex or dec string)
    int ZC_MaxPublicSpendsPerTx;
    int ZC_MaxSpendsPerTx;
    int ZC_MinMintConfirmations;
    CAmount ZC_MinMintFee;
    int ZC_MinStakeDepth;
    int ZC_TimeStart;
    int ZC_HeightStart;

    libzerocoin::ZerocoinParams* Zerocoin_Params(bool useModulusV1) const
    {
        static CBigNum bnHexModulus = 0;
        if (!bnHexModulus) bnHexModulus.SetHex(ZC_Modulus);
        static libzerocoin::ZerocoinParams ZCParamsHex = libzerocoin::ZerocoinParams(bnHexModulus);
        static CBigNum bnDecModulus = 0;
        if (!bnDecModulus) bnDecModulus.SetDec(ZC_Modulus);
        static libzerocoin::ZerocoinParams ZCParamsDec = libzerocoin::ZerocoinParams(bnDecModulus);
        return (useModulusV1 ? &ZCParamsHex : &ZCParamsDec);
    }

    /**
     * Returns true if the given network upgrade is active as of the given block
     * height. Caller must check that the height is >= 0 (and handle unknown
     * heights).
     */
    bool NetworkUpgradeActive(int nHeight, Consensus::UpgradeIndex idx) const;

    // LLMQ
    std::map<LLMQType, LLMQParams> llmqs;
    Optional<LLMQParams> GetLLMQParams(uint8_t llmqtype) const;
    LLMQType llmqChainLocks;

    // PTX formation schedule (W2.2 SG-1b)
    PTXFormationParams ptxFormation;
};
} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
