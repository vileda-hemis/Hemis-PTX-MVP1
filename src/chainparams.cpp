// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin developers
// Copyright (c) 2014-2015 The Dash developers
// Copyright (c) 2015-2022 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparams.h"

#include "arith_uint256.h"
#include "chainparamsseeds.h"
#include "consensus/merkle.h"
#include "tinyformat.h"
#include "utilstrencodings.h"

#include <assert.h>
#include <cstdio>
#include <cstdlib>

#include <thread>
#include <atomic>
#include <vector>

std::atomic<bool> found(false); // global flag to indicate if genesis block found


static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.nVersion = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 00 << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.vtx.push_back(std::make_shared<const CTransaction>(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.nVersion = nVersion;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

void CChainParams::UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex idx, int nActivationHeight)
{
    assert(IsRegTestNet()); // only available for regtest
    assert(idx > Consensus::BASE_NETWORK && idx < Consensus::MAX_NETWORK_UPGRADES);
    consensus.vUpgrades[idx].nActivationHeight = nActivationHeight;
}

/**
 * Build the genesis block. Note that the output of the genesis coinbase cannot
 * be spent as it did not originally exist in the database.
 *
 * CBlock(hash=00000ffd590b14, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=e0028e, nTime=1390095618, nBits=1e0ffff0, nNonce=28917698, vtx=1)
 *   CTransaction(hash=e0028e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d01044c5957697265642030392f4a616e2f3230313420546865204772616e64204578706572696d656e7420476f6573204c6976653a204f76657273746f636b2e636f6d204973204e6f7720416363657074696e6720426974636f696e73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0xA9037BAC7050C479B121CF)
 *   vMerkleTree: e0028e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "Takosha Churu, a dear friend and Hemis developer, died too young on 31st May 2023. May he rest in peace";
    const CScript genesisOutputScript = CScript() << ParseHex("04c10e83b2703ccf322f7dbd62dd5855ac7c10bd055814ce121ba32607d573b8810c02c0582aed05b4deb9c4b77b26d92428c61256cd42774babea0a073b2ed0c9") << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

// this one is for testing only
static Consensus::LLMQParams llmq_test = {
        .type = Consensus::LLMQ_TEST,
        .name = "llmq_test",
        .size = 3,
        .minSize = 2,
        .threshold = 2,

        .dkgInterval = 20, // one every 20 minutes
        .dkgPhaseBlocks = 2,
        .dkgMiningWindowStart = 10, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 15,
        .dkgBadVotesThreshold = 2,

        .signingActiveQuorumCount = 2, // just a few ones to allow easier testing

        .keepOldConnections = 3,
        .recoveryMembers = 3,

        .cacheDkgInterval = 60,
};

static Consensus::LLMQParams llmq50_60 = {
        .type = Consensus::LLMQ_50_60,
        .name = "llmq_50_60",
        .size = 50,
        .minSize = 40,
        .threshold = 30,

        .dkgInterval = 60, // one DKG per hour
        .dkgPhaseBlocks = 6,
        .dkgMiningWindowStart = 30, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 40,
        .dkgBadVotesThreshold = 40,

        .signingActiveQuorumCount = 24, // a full day worth of LLMQs

        .keepOldConnections = 25,
        .recoveryMembers = 25,

        .cacheDkgInterval = 600,
};

static Consensus::LLMQParams llmq400_60 = {
        .type = Consensus::LLMQ_400_60,
        .name = "llmq_400_60",
        .size = 400,
        .minSize = 300,
        .threshold = 240,

        .dkgInterval = 60 * 12, // one DKG every 12 hours
        .dkgPhaseBlocks = 10,
        .dkgMiningWindowStart = 50, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 70,
        .dkgBadVotesThreshold = 300,

        .signingActiveQuorumCount = 4, // two days worth of LLMQs

        .keepOldConnections = 5,
        .recoveryMembers = 100,

        .cacheDkgInterval = 60 * 12 * 10, // dkgInterval * 10
};

// Used for deployment and min-proto-version signaling, so it needs a higher threshold
static Consensus::LLMQParams llmq400_85 = {
        .type = Consensus::LLMQ_400_85,
        .name = "llmq_400_85",
        .size = 400,
        .minSize = 350,
        .threshold = 340,

        .dkgInterval = 60 * 24, // one DKG every 24 hours
        .dkgPhaseBlocks = 10,
        .dkgMiningWindowStart = 50, // dkgPhaseBlocks * 5 = after finalization
        .dkgMiningWindowEnd = 70, // give it a larger mining window to make sure it is mined
        .dkgBadVotesThreshold = 300,

        .signingActiveQuorumCount = 4, // four days worth of LLMQs

        .keepOldConnections = 5,
        .recoveryMembers = 100,

        .cacheDkgInterval = 60 * 24 * 10, // dkgInterval * 10
};

/**
 * Main network
 */
/**
 * What makes a good checkpoint block?
 * + Is surrounded by blocks with reasonable timestamps
 *   (no blocks before with a timestamp after, none after with
 *    timestamp before)
 * + Contains no strange transactions
 */
static MapCheckpoints mapCheckpoints = {
{0, uint256S   ("0x000000956c582b70df5d2c9b4b83d05b5331978e40d639739bdc96c29e156ce7")},
{9500, uint256S("0xa69189f2e1fd4ba82d177710158cae3e8aaad2b52993e63628a8d7120840577b")},
{276055, uint256S("0xca1660689e66c0cc5cce36abeb92b4209bc0978ec11c8ca4f339dec9dbfab8e4")},
{695496, uint256S("0x1287dc1709ffc2779d6993bcb2c9df45c8590dbba9333f4e8e7b0a0cba10e342")}
};

static const CCheckpointData data = {
    &mapCheckpoints,
    1748731575, // * UNIX timestamp of last checkpoint block
    725760,      // * total number of transactions between genesis and last checkpoint
                //   (the tx=... number in the UpdateTip debug.log lines)
    100         // * estimated number of transactions per day after checkpoint
};

static MapCheckpoints mapCheckpointsTestnet = {
    {0, uint256S("0x000000798b274f80ef80da249806ed8d86dec9338a58b34073b7014096e3d0c5")},
    {500, uint256S("0x5b6f9b4b781a4a22d3d0ffdaa0289ed4f9ff8f70293eee0f8c1a0c2966b6f18c")},
    //{    201, uint256S("6ae7d52092fd918c8ac8d9b1334400387d3057997e6e927a88e57186dc395231")},     // v5 activation (PoS/Sapling)
};

static const CCheckpointData dataTestnet = {
    &mapCheckpointsTestnet,
    1710619302,
    0,
    3000};

static MapCheckpoints mapCheckpointsRegtest = {{0, uint256S("0x001")}};
static const CCheckpointData dataRegtest = {
    &mapCheckpointsRegtest,
    1454124731,
    0,
    100};

//static void findGenesisBlock(uint32_t nTime, uint32_t startNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
//{
//    arith_uint256 bnTarget;
//    bool fNegative, fOverflow;
//    bnTarget.SetCompact(nBits, &fNegative, &fOverflow);
//    uint32_t nNonce = startNonce;
//    while (!found) {
//        CBlock genesis = CreateGenesisBlock(nTime, nNonce, nBits, nVersion, genesisReward);
//        uint256 h = genesis.GetHash();
//        if (UintToArith256(h) <= bnTarget) {
//            std::cout << "nonce: " << nNonce << "  hash: " << h.GetHex()
//                      << "  merkle: " << genesis.hashMerkleRoot.GetHex() << std::endl;
//            found = true; break;
//        }
//        nNonce++;
//    }
//}
//static void findGenesisPTXBea() {
//    found = false;
//    uint32_t nTime = 1779926400; // 2026-05-28 00:00:00 UTC
//    const unsigned N = std::thread::hardware_concurrency();
//    std::vector<std::thread> ts(N);
//    for (unsigned i = 0; i < N; ++i)
//        ts[i] = std::thread(findGenesisBlock, nTime, i*(UINT32_MAX/N), 0x1e00ffff, 1, (CAmount)0);
//    for (auto& t : ts) t.join();
//    std::exit(0);
//}

class CMainParams : public CChainParams
{
public:
    CMainParams()
    {
        strNetworkID = "main";

        genesis = CreateGenesisBlock(1705298400, 1356045, 0x1e00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();

        assert(consensus.hashGenesisBlock == uint256S("0x000000956c582b70df5d2c9b4b83d05b5331978e40d639739bdc96c29e156ce7"));
        assert(genesis.hashMerkleRoot == uint256S("0x93ad7b455294f429da00d11b656d62f7fb197a72b7315f58de8c9380dbdaa113"));

        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.powLimit   = uint256S("0x000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV1 = uint256S("0x000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV2 = uint256S("0x0000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nBudgetCycleBlocks = 43200;       // approx. 1 every 30 days
        consensus.nBudgetFeeConfirmations = 6;      // Number of confirmations for the finalization fee
        consensus.nCoinbaseMaturity = 100;
        consensus.nFutureTimeDriftPoW = 7200;
        consensus.nFutureTimeDriftPoS = 180;
        consensus.nMaxMoneyOut = 30000000 * COIN;
        consensus.nGMCollateralAmt = 1000 * COIN;
        consensus.nGMBlockReward = 0.775 * COIN;
        consensus.nNewGMBlockReward = 2.674999995 * COIN;
        consensus.nGMCollateralMinConf = 15;
        consensus.nProposalEstablishmentTime = 60 * 60 * 24;    // must be at least a day old to make it into a budget
        consensus.nStakeMinAge = 60 * 60;
        consensus.nStakeMinDepth = 300;
        consensus.nTargetTimespan = 20 * 60;
        consensus.nTargetTimespanV2 = 20 * 60;
        consensus.nTargetSpacing = 1 * 60;
        consensus.nTimeSlotLength = 15;
        consensus.nMaxProposalPayments = 6;

        // spork keys
        consensus.strSporkPubKey = "0489BF9B08FE8F2A971390A0C1A05F5B1C8A4F4F2ECF5413622CA8B21E296520A82431AFB5A6A7D788335EBB021301806D9653EFADB3AE132CBF507F58942F1BF4";

        // height-based activations
        consensus.height_last_invalid_UTXO = 1;
        consensus.height_last_ZC_AccumCheckpoint = 1;
        consensus.height_last_ZC_WrappedSerials = 1;

        // validation by-pass
//        consensus.nHemisBadBlockTime = 1471401614;    // Skip nBit validation of Block 259201 per PR #915
//        consensus.nHemisBadBlockBits = 0x1c056dac;    // Skip nBit validation of Block 259201 per PR #915

        // Zerocoin-related params
        consensus.ZC_Modulus = "25195908475657893494027183240048398571429282126204032027777137836043662020707595556264018525880784"
                "4069182906412495150821892985591491761845028084891200728449926873928072877767359714183472702618963750149718246911"
                "6507761337985909570009733045974880842840179742910064245869181719511874612151517265463228221686998754918242243363"
                "7259085141865462043576798423387184774447920739934236584823824281198163815010674810451660377306056201619676256133"
                "8441436038339044149526344321901146575444541784240209246165157233507787077498171257724679629263863563732899121548"
                "31438167899885040445364023527381951378636564391212010397122822120720357";
        consensus.ZC_MaxPublicSpendsPerTx = 637;    // Assume about 220 bytes each input
        consensus.ZC_MaxSpendsPerTx = 7;            // Assume about 20kb each input
        consensus.ZC_MinMintConfirmations = 20;
        consensus.ZC_MinMintFee = 1 * CENT;
        consensus.ZC_MinStakeDepth = 200;
        consensus.ZC_TimeStart = 1808214600;        // October 17, 2017 4:30:00 AM
        consensus.ZC_HeightStart = 1;

        // Network upgrades
        consensus.vUpgrades[Consensus::BASE_NETWORK].nActivationHeight = 
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight = 
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight           = 500;
        consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight        = 500;
        consensus.vUpgrades[Consensus::UPGRADE_ZC].nActivationHeight            = 71000000;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight         = 71000000;
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight         = 500;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].nActivationHeight     = 71000000;
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight          = 500;
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight          = 501;
        consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight          = 502;
        consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight          = 503;
        consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight          = 504;
        consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight          = 505;
        consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight 		    = 
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;

        consensus.vUpgrades[Consensus::UPGRADE_ZC].hashActivationBlock =
                uint256S("0x5b2482eca24caf2a46bb22e0545db7b7037282733faa3a42ec20542509999a64");
        consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].hashActivationBlock =
                uint256S("0x37ea75fe1c9314171cff429a91b25b9f11331076d1c9de50ee4054d61877f8af");
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].hashActivationBlock =
                uint256S("0x82629b7a9978f5c7ea3f70a12db92633a7d2e436711500db28b97efd48b1e527");
        consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].hashActivationBlock =
                uint256S("0xe2448b76d88d37aba4194ffed1041b680d779919157ddf5cbf423373d7f8078e");
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].hashActivationBlock =
                uint256S("0x0ef2556e40f3b9f6e02ce611b832e0bbfe7734a8ea751c7b555310ee49b61456");
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].hashActivationBlock =
                uint256S("0x14e477e597d24549cac5e59d97d32155e6ec2861c1003b42d0566f9bf39b65d5");

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0xa0;
        pchMessageStart[1] = 0x14;
        pchMessageStart[2] = 0xdd;
        pchMessageStart[3] = 0x99;
        nDefaultPort = 49165;

        // Note that of those with the service bits flag, most only support a subset of possible options
        vSeeds.emplace_back("seed.Hemis.tech", true);
	    vSeeds.emplace_back("vps.hemis.tech", true);     // Primary DNS Seeder from Fuzzbawls

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 40);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 13);
        base58Prefixes[STAKING_ADDRESS] = std::vector<unsigned char>(1, 63);     // starting with 'S'
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 212);

        base58Prefixes[EXT_PUBLIC_KEY] = {0xa0, 0xf2, 0xf5, 0xf3};
        base58Prefixes[EXT_SECRET_KEY] = {0xa0, 0xf3, 0xf1, 0xfB};

        // BIP44 coin type is from https://github.com/satoshilabs/slips/blob/master/slip-0044.md
        base58Prefixes[EXT_COIN_TYPE] = {0x80, 0x00, 0x02, 0xac};

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_main), std::end(chainparams_seed_main));

        // Reject non-standard transactions by default
        fRequireStandard = true;

        // Sapling
        bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "ps";
        bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "pviews";
        bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "pivks";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "p-secret-spending-key-main";
        bech32HRPs[SAPLING_EXTENDED_FVK]         = "pxviews";

        bech32HRPs[BLS_SECRET_KEY]               = "bls-sk";
        bech32HRPs[BLS_PUBLIC_KEY]               = "bls-pk";

        // long living quorum params
        consensus.llmqs[Consensus::LLMQ_50_60] = llmq50_60;
        consensus.llmqs[Consensus::LLMQ_400_60] = llmq400_60;
        consensus.llmqs[Consensus::LLMQ_400_85] = llmq400_85;

        // PTX formation cadence (W2.2 SG-1b): N = 1440 = 24h at 60s spacing,
        // security-ceiling-only (KDD-063 handover-at-accept).
        consensus.ptxFormation = {"main", 1440, 1440, 1440, 1};   // KDD-079 decouple: B=R=budget (L=1-preserving)

        nLLMQConnectionRetryTimeout = 60;

        consensus.llmqChainLocks = Consensus::LLMQ_400_60;

        // Tier two
        nFulfilledRequestExpireTime = 60 * 60; // fulfilled requests expire in 1 hour
    }

    const CCheckpointData& Checkpoints() const
    {
        return data;
    }

};

/**
 * Testnet (v5)
 */
class CTestNetParams : public CChainParams
{
public:
    CTestNetParams()
    {

//	findGenesis();

        strNetworkID = "test";

        genesis = CreateGenesisBlock(1710619302, 12048181, 0x1e00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();

	    //std::cout << "genesis Block Hash: " << consensus.hashGenesisBlock.GetHex() << std::endl;
	    //std::cout << "Merkle Root Hash: " << genesis.hashMerkleRoot.GetHex() << std::endl;


        assert(consensus.hashGenesisBlock == uint256S("0x000000798b274f80ef80da249806ed8d86dec9338a58b34073b7014096e3d0c5"));
        assert(genesis.hashMerkleRoot == uint256S("0x93ad7b455294f429da00d11b656d62f7fb197a72b7315f58de8c9380dbdaa113"));

        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.powLimit   = uint256S("0x000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV1 = uint256S("0x000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV2 = uint256S("0x0000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nBudgetCycleBlocks = 144;       // approx. 10 cycles per day
        consensus.nBudgetFeeConfirmations = 6;      // Number of confirmations for the finalization fee
        consensus.nCoinbaseMaturity = 100;
        consensus.nFutureTimeDriftPoW = 7200;
        consensus.nFutureTimeDriftPoS = 180;
        consensus.nMaxMoneyOut = 30000000 * COIN;
        consensus.nGMCollateralAmt = 1000 * COIN;
        consensus.nGMBlockReward = 0.775 * COIN;
        consensus.nNewGMBlockReward = 2.674999995 * COIN;
        consensus.nGMCollateralMinConf = 15;
        consensus.nProposalEstablishmentTime = 60 * 5;    // must be at least a day old to make it into a budget
        consensus.nStakeMinAge = 60 * 60;
        consensus.nStakeMinDepth = 300;
        consensus.nTargetTimespan = 20 * 60;
        consensus.nTargetTimespanV2 = 20 * 60;
        consensus.nTargetSpacing = 1 * 60;
        consensus.nTimeSlotLength = 15;
        consensus.nMaxProposalPayments = 6;

        // spork keys
        consensus.strSporkPubKey = "047F4B276E7852D6FC9AE1869B758C479759FD8CD0DC0E760EAE370161E0A75076E2CB12FE351431BA2ECAEF749FDADE63F18C0BB9BD5A0B7183C223724D9CDB48";

        // height-based activations
        consensus.height_last_invalid_UTXO = 1;
        consensus.height_last_ZC_AccumCheckpoint = 1;
        consensus.height_last_ZC_WrappedSerials = 1;

        // validation by-pass
//        consensus.nHemisBadBlockTime = 1471401614;    // Skip nBit validation of Block 259201 per PR #915
//        consensus.nHemisBadBlockBits = 0x1c056dac;    // Skip nBit validation of Block 259201 per PR #915

        // Zerocoin-related params
        consensus.ZC_Modulus = "25195908475657893494027183240048398571429282126204032027777137836043662020707595556264018525880784"
                "4069182906412495150821892985591491761845028084891200728449926873928072877767359714183472702618963750149718246911"
                "6507761337985909570009733045974880842840179742910064245869181719511874612151517265463228221686998754918242243363"
                "7259085141865462043576798423387184774447920739934236584823824281198163815010674810451660377306056201619676256133"
                "8441436038339044149526344321901146575444541784240209246165157233507787077498171257724679629263863563732899121548"
                "31438167899885040445364023527381951378636564391212010397122822120720357";
        consensus.ZC_MaxPublicSpendsPerTx = 637;    // Assume about 220 bytes each input
        consensus.ZC_MaxSpendsPerTx = 7;            // Assume about 20kb each input
        consensus.ZC_MinMintConfirmations = 20;
        consensus.ZC_MinMintFee = 1 * CENT;
        consensus.ZC_MinStakeDepth = 200;
        consensus.ZC_TimeStart = 1808214600;        // October 17, 2017 4:30:00 AM
        consensus.ZC_HeightStart = 1;

        // Network upgrades
        consensus.vUpgrades[Consensus::BASE_NETWORK].nActivationHeight = 
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight = 
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight           = 500;
        consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight        = 500;
        consensus.vUpgrades[Consensus::UPGRADE_ZC].nActivationHeight            = 71000000;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight         = 71000000;
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight         = 500;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].nActivationHeight     = 71000000;
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight          = 500;
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight          = 501;
        consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight          = 502;
        consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight          = 503;
        consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight          = 504;
        consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight          = 505;
        consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight 		    = 
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;

        consensus.vUpgrades[Consensus::UPGRADE_ZC].hashActivationBlock =
                uint256S("0x5b2482eca24caf2a46bb22e0545db7b7037282733faa3a42ec20542509999a64");
        consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].hashActivationBlock =
                uint256S("0x37ea75fe1c9314171cff429a91b25b9f11331076d1c9de50ee4054d61877f8af");
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].hashActivationBlock =
                uint256S("0x82629b7a9978f5c7ea3f70a12db92633a7d2e436711500db28b97efd48b1e527");
        consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].hashActivationBlock =
                uint256S("0xe2448b76d88d37aba4194ffed1041b680d779919157ddf5cbf423373d7f8078e");
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].hashActivationBlock =
                uint256S("0x0ef2556e40f3b9f6e02ce611b832e0bbfe7734a8ea751c7b555310ee49b61456");
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].hashActivationBlock =
                uint256S("0x14e477e597d24549cac5e59d97d32155e6ec2861c1003b42d0566f9bf39b65d5");
        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0xf5;
        pchMessageStart[1] = 0xe6;
        pchMessageStart[2] = 0xd5;
        pchMessageStart[3] = 0xca;
        nDefaultPort = 51474;

        // nodes with support for servicebits filtering should be at the top
        vSeeds.emplace_back("hemis-testnet.hypur.xyz", true);

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 139); // Testnet Hemis addresses start with 'x' or 'y'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 19);  // Testnet Hemis script addresses start with '8' or '9'
        base58Prefixes[STAKING_ADDRESS] = std::vector<unsigned char>(1, 73);     // starting with 'W'
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);     // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
        // Testnet Hemis BIP32 pubkeys start with 'DRKV'
        base58Prefixes[EXT_PUBLIC_KEY] = {0x3a, 0x80, 0x61, 0xa0};
        // Testnet Hemis BIP32 prvkeys start with 'DRKP'
        base58Prefixes[EXT_SECRET_KEY] = {0x3a, 0x80, 0x58, 0x37};
        // Testnet Hemis BIP44 coin type is '1' (All coin's testnet default)
        base58Prefixes[EXT_COIN_TYPE] = {0x80, 0x00, 0x00, 0x01};

        vFixedSeeds = std::vector<uint8_t>(std::begin(chainparams_seed_test), std::end(chainparams_seed_test));

        fRequireStandard = false;

        // Sapling
        bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "ptestsapling";
        bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "pviewtestsapling";
        bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "pivktestsapling";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "p-secret-spending-key-test";
        bech32HRPs[SAPLING_EXTENDED_FVK]         = "pxviewtestsapling";

        bech32HRPs[BLS_SECRET_KEY]               = "bls-sk-test";
        bech32HRPs[BLS_PUBLIC_KEY]               = "bls-pk-test";

        // long living quorum params
        consensus.llmqs[Consensus::LLMQ_50_60] = llmq50_60;
        consensus.llmqs[Consensus::LLMQ_400_60] = llmq400_60;
        consensus.llmqs[Consensus::LLMQ_400_85] = llmq400_85;

        // PTX formation cadence (W2.2 SG-1b): public testnet mirrors the
        // mainnet posture (SG-1b plan-gate decision 1).
        consensus.ptxFormation = {"test", 1440, 1440, 1440, 1};   // KDD-079 decouple: B=R=budget (L=1-preserving)

        nLLMQConnectionRetryTimeout = 60;

        consensus.llmqChainLocks = Consensus::LLMQ_400_60;

        // Tier two
        nFulfilledRequestExpireTime = 60 * 60; // fulfilled requests expire in 1 hour
    }

    const CCheckpointData& Checkpoints() const
    {
        return dataTestnet;
    }
};

/**
 * Regression test
 */
class CRegTestParams : public CChainParams
{
public:
    CRegTestParams()
    {
        strNetworkID = "regtest";

        genesis = CreateGenesisBlock(1692423000, 4820300, 0x1e00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256S("0x0000009efeaed4a864a417a90065b12da5bd89ac7742c4f88a93d8687a94abb0"));
        assert(genesis.hashMerkleRoot == uint256S("0x93ad7b455294f429da00d11b656d62f7fb197a72b7315f58de8c9380dbdaa113"));

        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.fPowNoRetargeting = true;
        consensus.powLimit   = uint256S("0x7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV1 = uint256S("0x000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV2 = uint256S("0x00000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nBudgetCycleBlocks = 144;         // approx 10 cycles per day
        consensus.nBudgetFeeConfirmations = 3;      // (only 8-blocks window for finalization on regtest)
        consensus.nCoinbaseMaturity = 100;
        consensus.nFutureTimeDriftPoW = 7200;
        consensus.nFutureTimeDriftPoS = 180;
        consensus.nMaxMoneyOut = 43199500 * COIN;
        consensus.nGMCollateralAmt = 100 * COIN;
        consensus.nGMBlockReward = 3 * COIN;
        consensus.nNewGMBlockReward = 6 * COIN;
        consensus.nGMCollateralMinConf = 1;
        consensus.nProposalEstablishmentTime = 60 * 5;  // at least 5 min old to make it into a budget
        consensus.nStakeMinAge = 0;
        consensus.nStakeMinDepth = 20;
        consensus.nTargetTimespan = 40 * 60;
        consensus.nTargetTimespanV2 = 30 * 60;
        consensus.nTargetSpacing = 1 * 60;
        consensus.nTimeSlotLength = 15;
        consensus.nMaxProposalPayments = 20;

        /* Spork Key for RegTest:
        WIF private key: 932HEevBSujW2ud7RfB1YF91AFygbBRQj3de3LyaCRqNzKKgWXi
        private key hex: bd4960dcbd9e7f2223f24e7164ecb6f1fe96fc3a416f5d3a830ba5720c84b8ca
        Address: yCvUVd72w7xpimf981m114FSFbmAmne7j9
        */
        consensus.strSporkPubKey = "043969b1b0e6f327de37f297a015d37e2235eaaeeb3933deecd8162c075cee0207b13537618bde640879606001a8136091c62ec272dd0133424a178704e6e75bb7";

        // height based activations
        consensus.height_last_invalid_UTXO = -1;
        consensus.height_last_ZC_AccumCheckpoint = 310;     // no checkpoints on regtest
        consensus.height_last_ZC_WrappedSerials = -1;

        // Zerocoin-related params
        consensus.ZC_Modulus = "25195908475657893494027183240048398571429282126204032027777137836043662020707595556264018525880784"
                "4069182906412495150821892985591491761845028084891200728449926873928072877767359714183472702618963750149718246911"
                "6507761337985909570009733045974880842840179742910064245869181719511874612151517265463228221686998754918242243363"
                "7259085141865462043576798423387184774447920739934236584823824281198163815010674810451660377306056201619676256133"
                "8441436038339044149526344321901146575444541784240209246165157233507787077498171257724679629263863563732899121548"
                "31438167899885040445364023527381951378636564391212010397122822120720357";
        consensus.ZC_MaxPublicSpendsPerTx = 637;    // Assume about 220 bytes each input
        consensus.ZC_MaxSpendsPerTx = 7;            // Assume about 20kb each input
        consensus.ZC_MinMintConfirmations = 10;
        consensus.ZC_MinMintFee = 1 * CENT;
        consensus.ZC_MinStakeDepth = 10;
        consensus.ZC_TimeStart = 4070908800;                 // not implemented on regtest
        consensus.ZC_HeightStart = 100000000;

        // Network upgrades
        consensus.vUpgrades[Consensus::BASE_NETWORK].nActivationHeight =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight           = 250;
        consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight        = 250;
        consensus.vUpgrades[Consensus::UPGRADE_ZC].nActivationHeight            = 1000000;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight         = 1000000;
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight         =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].nActivationHeight     = 1000000;
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight          = 251;
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight          =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight          = 250;
        consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight          = 250;
        consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight          = 250;
        consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight          = 250;
        consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight =
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 4-byte int at any alignment.
         */
        pchMessageStart[0] = 0xa1;
        pchMessageStart[1] = 0xcf;
        pchMessageStart[2] = 0x7e;
        pchMessageStart[3] = 0xac;
        nDefaultPort = 51476;

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 139); // Testnet Hemis addresses start with 'x' or 'y'
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 19);  // Testnet Hemis script addresses start with '8' or '9'
        base58Prefixes[STAKING_ADDRESS] = std::vector<unsigned char>(1, 73);     // starting with 'W'
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);     // Testnet private keys start with '9' or 'c' (Bitcoin defaults)
        // Testnet Hemis BIP32 pubkeys start with 'DRKV'
        base58Prefixes[EXT_PUBLIC_KEY] = {0x3a, 0x80, 0x61, 0xa0};
        // Testnet Hemis BIP32 prvkeys start with 'DRKP'
        base58Prefixes[EXT_SECRET_KEY] = {0x3a, 0x80, 0x58, 0x37};
        // Testnet Hemis BIP44 coin type is '1' (All coin's testnet default)
        base58Prefixes[EXT_COIN_TYPE] = {0x80, 0x00, 0x00, 0x01};

        // Reject non-standard transactions by default
        fRequireStandard = true;

        // Sapling
        bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "ptestsapling";
        bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "pviewtestsapling";
        bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "pivktestsapling";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "p-secret-spending-key-test";
        bech32HRPs[SAPLING_EXTENDED_FVK]         = "pxviewtestsapling";

        bech32HRPs[BLS_SECRET_KEY]               = "bls-sk-test";
        bech32HRPs[BLS_PUBLIC_KEY]               = "bls-pk-test";

        // long living quorum params
        consensus.llmqs[Consensus::LLMQ_TEST] = llmq_test;
        nLLMQConnectionRetryTimeout = 10;

        consensus.llmqChainLocks = Consensus::LLMQ_TEST;

        // PTX formation cadence (W2.2 SG-1b): dev test-N = 80, M-coupled
        // (~1.7x the ceremony floor M~47; SG-1b plan-gate decision 1).
        consensus.ptxFormation = {"regtest", 80, 80, 80, 1, 200, 1, 40}; // KDD-079 decouple (B=R=budget, L=1-preserving) + W4-f reform gate LIVE (drill chains only)

        // ODC-073 Step 1 — nSeedHeight past-anchor bound, drill net. Same
        // rationale as ptxbea: 60 sits above the ~12-block legitimate commit-lag
        // floor and below the nRetireWindow=200 idle-window ceiling, and under
        // the 80-block boundary so a legitimate anchor stays in the current epoch.
        nPTXSeedHeightWindow = 60;

        // Tier two
        nFulfilledRequestExpireTime = 60 * 60; // fulfilled requests expire in 1 hour
    }

    const CCheckpointData& Checkpoints() const
    {
        return dataRegtest;
    }
};

/**
 * PTX closed testnet — isolated network for PTX protocol development.
 * pchMessageStart = "PTXT"  P2P port 29994  -- both set in CPTXTestNetParams below.
 * RPC port 29995 -- set in CBaseChainParams for "ptxtestnet", NOT here.
 *
 * ★ This line said `"PTX2" … RPC port 29902` until 2026-08-23 and was wrong on both
 * counts: the magic became PTXT and the RPC default became 29995, BOTH on 2026-08-21,
 * and this summary was not carried along. The stale RPC figure is the more dangerous
 * half — 29902-vs-29995 is the exact mismatch that had the PTX fan-out dialling a port
 * nothing listened on (see the note above the ptxtestnet entry in chainparamsbase.cpp).
 * ★ Deliberately NO line numbers: this block sits ~190 lines above what it describes,
 * so any :NNN written here rots on the next edit. It did, immediately — the first
 * version of this correction cited three line numbers and two were already stale.
 * A summary comment is a second source of truth; point at symbols, not lines.
 */
static MapCheckpoints mapCheckpointsPTXTestNet = {
    {0, uint256S("0x00000094a6ac77ae3503093e9529981cc724c4410265f727bc762f713b0da6e2")}
};
static const CCheckpointData dataPTXTestNet = {
    &mapCheckpointsPTXTestNet,
    1787443200,
    0,
    0
};

static MapCheckpoints mapCheckpointsPTXBeaTestNet = {
    {0, uint256S("0x000000dccda161efd26813bfd5322d7c7fb598ef20bf4fd872b7335eb8beb58b")}
};
static const CCheckpointData dataPTXBeaTestNet = {
    &mapCheckpointsPTXBeaTestNet,
    1779926400,
    0,
    0
};

class CPTXTestNetParams : public CChainParams
{
public:
    CPTXTestNetParams()
    {
        strNetworkID = "ptxtestnet";

        genesis = CreateGenesisBlock(1787443200, 10954950, 0x1e00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();

        assert(consensus.hashGenesisBlock == uint256S("0x00000094a6ac77ae3503093e9529981cc724c4410265f727bc762f713b0da6e2"));
        assert(genesis.hashMerkleRoot == uint256S("0x93ad7b455294f429da00d11b656d62f7fb197a72b7315f58de8c9380dbdaa113"));

        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.powLimit   = uint256S("0x000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV1 = uint256S("0x000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV2 = uint256S("0x0000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.nBudgetCycleBlocks = 144;
        consensus.nBudgetFeeConfirmations = 3;
        consensus.nCoinbaseMaturity = 10;
        consensus.nFutureTimeDriftPoW = 7200;
        consensus.nFutureTimeDriftPoS = 180;
        consensus.nMaxMoneyOut = 30000000 * COIN;
        consensus.nGMCollateralAmt = 100 * COIN;
        consensus.nGMBlockReward = 3 * COIN;
        // ★★ 6 -> 2.674999995, matching mainnet (:256), Hemis testnet (:428) and
        // ptxbea (:1038). This is the post-V5_5 payment: GetGamemasterPayment
        // (validation.cpp:881-886) returns nNewGMBlockReward above the UPGRADE_V5_5
        // activation height and nGMBlockReward below it.
        //
        // ★ IT IS INERT TODAY AND WILL NOT STAY THAT WAY. GM payments are OFF:
        // IsSporkActive is `GetSporkValue(id) < GetAdjustedTime()` (spork.cpp:223)
        // and SPORK_7's default 4070908800 is the year 2099, so the spork is
        // inactive -- spork.cpp:17 labels it `// OFF` in source. Measured on the
        // fleet: block 10443 pays 0.0 in coinbase and the payee GM's payoutAddress
        // appears nowhere in the block, while `lastPaidHeight` advances anyway
        // because deterministicgms.cpp:801-805 records the rotation with no spork
        // check. The plumbing is correct and the tap is closed.
        //
        // ★★ SO THIS VALUE MUST BE RIGHT BEFORE THE SPORK IS EVER FLIPPED, which is
        // why it lands with the tag rather than after it. And the cadence it will
        // run at is NOT one this project has observed: payout is ONE GM PER BLOCK on
        // a rotating queue (153 GMs on the fleet -> 153 distinct lastPaidHeight
        // values spanning exactly 152 blocks, measured). At testnet's ~20 GMs that
        // is every ~20 blocks per GM -- roughly 7.6x more often than the fleet would
        // have seen. The payment PATH is proven in production; that CADENCE is not,
        // and wallet fragmentation under it is unmeasured. For scale, the fleet
        // already carries 5,540 dust outputs against 365 real coins with payments
        // OFF entirely.
        consensus.nNewGMBlockReward = 2.674999995 * COIN;
        consensus.nGMCollateralMinConf = 1;
        consensus.nProposalEstablishmentTime = 60 * 5;
        consensus.nStakeMinAge = 0;
        consensus.nStakeMinDepth = 20;
        consensus.nTargetTimespan = 20 * 60;
        consensus.nTargetTimespanV2 = 20 * 60;
        consensus.nTargetSpacing = 1 * 60;
        // ★ 1 -> 15 (2026-08-21). This is NOT only a stake-slot knob: it also sets
        // the P2P clock-skew tolerance, `abs64(nTimeOffset) < 2 * nTimeSlotLength`
        // (net_processing.cpp:1507-1513), so 1 gave a +/-2s window. Across five
        // internet-separated operators with ordinary NTP drift that is constant
        // mutual disconnection. 15 gives +/-30s and matches main (:264), test
        // (:436), regtest (:596) and ptxbea (:920) -- ptxtestnet was the outlier.
        // ★ It also sets FutureBlockTimeDrift under Time Protocol v2
        // (consensus/params.h: `return nTimeSlotLength - 1`) = 14s, and the block
        // timestamp mask `nTime % nTimeSlotLength == 0`.
        consensus.nTimeSlotLength = 15;
        consensus.nMaxProposalPayments = 6;

        // ★★ NO LONGER SHARED WITH CTestNetParams (:440). Replaced 2026-08-23 with a
        // key generated for this chain alone. Until then this held the byte-identical
        // Hemis-testnet key, so anyone able to sign a spork there could sign one here.
        // The PRIVATE half lives in $PTXTESTNET_SPORK_KEY in the deployment environment
        // and NEVER in this repository -- same discipline as the ptxbea FLEET-ONLY
        // DIVERGENCE marker at :923.
        //
        // ★ COMPRESSED (66 hex chars, `03` prefix), and that is deliberate, not an
        // oversight against the 130-char key it replaces. CKey::SignCompact encodes the
        // compression flag in the recovery header (key.cpp:243) and verification compares
        // the CKeyID HASH160 recovered from the signature (messagesigner.cpp:88-104), so
        // re-expanding this to the uncompressed form would simply never verify.
        // Validated before landing: 66 chars, hex, x < field p, on-curve, y-parity
        // matches the `03` prefix.
        consensus.strSporkPubKey = "03612ded861c44f9c9e56baff7d0a6acee59b720c8972085551a59a65f14a4e1ff";

        // ★★ SET EXPLICITLY BECAUSE THE DEFAULT IS INDETERMINATE, NOT ZERO.
        // These five have no in-class initialiser (consensus/params.h:310-312,
        // :320-321), CChainParams has a user-provided empty ctor
        // (chainparams.h:126) and `Consensus::Params consensus;` (:130) is in no
        // mem-init list -- so they are default-initialised and their values are
        // INDETERMINATE, on every network including mainnet. They are then READ on
        // live paths: CheckWork (validation.cpp:2962-2963) will accept a block with
        // the wrong PoW threshold if its nTime/nBits happen to match the garbage,
        // and spork.cpp:149/:153/:275/:321 decide whether the OLD spork key is
        // still accepted. Explicit no-ops here; the general fix is BUG-046.
        consensus.strSporkPubKeyOld = consensus.strSporkPubKey;
        consensus.nTime_EnforceNewSporkKey = 0;
        consensus.nTime_RejectOldSporkKey = 0;
        consensus.nHemisBadBlockTime = 0;
        consensus.nHemisBadBlockBits = 0;

        consensus.height_last_invalid_UTXO = -1;
        consensus.height_last_ZC_AccumCheckpoint = -1;
        consensus.height_last_ZC_WrappedSerials = -1;

        consensus.ZC_Modulus = "25195908475657893494027183240048398571429282126204032027777137836043662020707595556264018525880784"
                "4069182906412495150821892985591491761845028084891200728449926873928072877767359714183472702618963750149718246911"
                "6507761337985909570009733045974880842840179742910064245869181719511874612151517265463228221686998754918242243363"
                "7259085141865462043576798423387184774447920739934236584823824281198163815010674810451660377306056201619676256133"
                "8441436038339044149526344321901146575444541784240209246165157233507787077498171257724679629263863563732899121548"
                "31438167899885040445364023527381951378636564391212010397122822120720357";
        consensus.ZC_MaxPublicSpendsPerTx = 637;
        consensus.ZC_MaxSpendsPerTx = 7;
        consensus.ZC_MinMintConfirmations = 10;
        consensus.ZC_MinMintFee = 1 * CENT;
        consensus.ZC_MinStakeDepth = 10;
        consensus.ZC_TimeStart = 4070908800;
        consensus.ZC_HeightStart = 100000000;

        // Network upgrades.
        //
        // ★★ UPGRADE_POS / POS_V2 = 50 and V3_4 = 51 ARE BOOTSTRAP HEIGHTS AND MUST
        // NOT BE MOVED TO 0 OR 1. doc/ptx/PTX_TESTNET_GENESIS_CONFIG.md §3 says
        // "state 1 and mean it" for every activation height. That principle is right
        // for grandfathering gates and WRONG for these, and following it here
        // produces a chain permanently stuck at height 0:
        //
        //   rpc/mining.cpp:321-322
        //     const int nHeight = chainActive.Height() + 1;              // = 1 at genesis
        //     if (fGenerate && NetworkUpgradeActive(nHeight, UPGRADE_POS))
        //         throw JSONRPCError(..., "Proof of Work phase has already ended");
        //
        // With POS active at height 1, NetworkUpgradeActive(1, POS) is true
        // (upgrades.cpp:92, nHeight >= nActivationHeight) and the FIRST generate call
        // is refused. No PoW block can be mined, so no coins exist, so nothing can
        // stake -- the chain cannot leave block 0. Blocks 1..49 must be PoW to mint
        // the initial supply before anything can stake.
        //
        // ptxbea proves 50/51 works: its chain has PoW blocks 1-49 and PoS from h50
        // (verified live 2026-08-21). ★ If you are here to "fix" these back to 1
        // because §3 says so, read rpc/mining.cpp:321 first.
        consensus.vUpgrades[Consensus::BASE_NETWORK].nActivationHeight =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight           = 50;
        consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight        = 50;
        consensus.vUpgrades[Consensus::UPGRADE_ZC].nActivationHeight            = 100000000;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight         = 100000000;
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight         =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].nActivationHeight     = 100000000;
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight          = 51;
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight          = 265;
        consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight          = 50;
        consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight          = 50;
        consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight          = 50;
        consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight          = 50;
        // ★★ THE MASTER PTX GATE. Was NO_ACTIVATION_HEIGHT (-1), which is not
        // "later" -- it is NEVER (upgrades.cpp:90-91 returns UPGRADE_DISABLED).
        // With it disabled the entire PTX/DGM subsystem is dead on this network:
        // every protx_* RPC throws "Evo upgrade is not active yet"
        // (rpc/rpcevo.cpp:177-182), CheckSpecialTx rejects any ProRegTx as
        // bad-txns-v6-not-active (specialtx_validation.cpp:1017-1023), and
        // IsDIP3Enforced() is false so no deterministic gamemaster list exists.
        // A chain that syncs and stakes and can never register a gamemaster.
        // ★ Measured both ways 2026-08-21: on ptxbea (ALWAYS_ACTIVE, :976-977)
        // protx_list returns 153 registered GMs; on the ptxtestnet node running
        // this value it returns the "Evo upgrade is not active yet" error.
        consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight          =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;

        // ★★ MAGIC "PTX2" -> "PTXT" (2026-08-21), and it is load-bearing for the
        // genesis regeneration. A pre-existing ptxtestnet chain runs PTX2 with the
        // OLD genesis (a node at height 62069 was still live when this changed).
        // Keeping PTX2 while regenerating genesis means old and new nodes SHARE
        // magic and DISAGREE on genesis: they handshake, fail, and ban each other.
        // Changing the magic means they never meet. Verified absent elsewhere:
        // main a014dd99, Hemis testnet f5e6d5ca, regtest a1cf7eac, ptxbea "PTX3".
        pchMessageStart[0] = 0x50; // 'P'
        pchMessageStart[1] = 0x54; // 'T'
        pchMessageStart[2] = 0x58; // 'X'
        pchMessageStart[3] = 0x54; // 'T'  -- "PTXT"
        // ★★★ 29993 -> 29994 (2026-09-06, BUG-071). THE TWIN OF THE 29902->29995 FIX
        // ALREADY RECORDED IN chainparamsbase.cpp:49. That note says this class of
        // mismatch "used to be load-bearing for CONSENSUS-ADJACENT behaviour"; the RPC
        // half was corrected on 2026-08-21 and the P2P half was not, because nothing
        // was visibly broken. It was not broken. It was throttled.
        //
        // ★ 29993 WAS deliberate -- for a different chain than the one now running.
        // e60e116 (2026-05-18) created CPTXTestNetParams as an "isolated closed
        // testnet", and 29993 kept it clear of ptxbea's 29994. When this network
        // became the public testnet nobody revisited it: install.sh writes
        // port=29994, every node runs 29994, and eleven ProRegTxs record :29994 on
        // chain. Both genesis specs say so too -- PTX_TESTNET_GENESIS_CONFIG.md:120
        // and TESTNET_GENESIS_PARAMS.md:50 both give nDefaultPort 29994 "as
        // specified". Nothing anywhere recorded a decision to keep 29993 after the
        // operators moved.
        //
        // ★★ WHAT IT COSTS -- and NOT what I first claimed (ODC-122). The first
        // version of this comment said net.cpp:1832's non-default-port skip
        // de-prioritised every live address and throttled discovery. That was a
        // MISREADING: `nTries` counts addresses DRAWN FROM addrman in one selection
        // pass, not connection attempts -- the comment above it says so -- so passing
        // 50 costs microseconds and then non-default ports are accepted. A
        // pre-deployment baseline across 11 hosts measured 47 outbound connections to
        // addresses in no seed list, ptxwallet01 reaching 8 of them ~50s after its 3
        // seeds. Discovery is fast. There is no throttle to fix here.
        //
        // ★★ THE REAL COST IS A FOOT-GUN, AND IT REACHES THE CHAIN. rpcevo.cpp:416
        // resolves a portless protx_register address against this value, so a
        // registration written without a port recorded :29993 -- a port nothing on
        // this network listens on. weirdness.md documents the result: the
        // registration succeeds, the node syncs, getgamemasterstatus says Ready, and
        // nothing goes wrong until arming. Same shape at net.cpp:1882 (a portless
        // addnode) and net.cpp:107 (a node with no port= binds it and is invisible).
        //
        // ★★ NOT CONSENSUS. The only validation-side reader of a default port is
        // specialtx_validation.cpp:45, and it reads MAIN's default explicitly, never
        // the active chain's. A partial rollout therefore DEGRADES, it does not
        // break: an un-upgraded node merely prefers different addresses, and no
        // handshake, wire format or validity rule involves this number.
        //
        // ★ KNOWN COLLISION, stated rather than discovered later: ptxbea's default is
        // also 29994 (see CPTXBeaTestNetParams below), which TESTNET_GENESIS_PARAMS.md:50
        // flagged -- "the two must never share a host. Pick 29996 if they ever might".
        // They are separated by magic and datadir, so this is a port-binding hazard on
        // a host running BOTH chains with neither given an explicit -port, not a
        // network-safety one. Accepted knowingly: matching what is deployed and what
        // both genesis specs say beats keeping a number no node has ever listened on.
        nDefaultPort = 29994;

        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 139);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 19);
        base58Prefixes[STAKING_ADDRESS] = std::vector<unsigned char>(1, 73);
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x3a, 0x80, 0x61, 0xa0};
        base58Prefixes[EXT_SECRET_KEY] = {0x3a, 0x80, 0x58, 0x37};
        base58Prefixes[EXT_COIN_TYPE] = {0x80, 0x00, 0x00, 0x01};

        vFixedSeeds.clear();

        fRequireStandard = false;

        bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "ptxtest";
        bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "ptxtestview";
        bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "ptxtestivk";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "p-secret-spending-key-ptxtest";
        bech32HRPs[SAPLING_EXTENDED_FVK]         = "ptxtestxview";

        bech32HRPs[BLS_SECRET_KEY]               = "bls-sk-ptx";
        bech32HRPs[BLS_PUBLIC_KEY]               = "bls-pk-ptx";

        // Small LLMQ suitable for a 4-node PTX testnet
        consensus.llmqs[Consensus::LLMQ_TEST] = llmq_test;
        nLLMQConnectionRetryTimeout = 10;
        consensus.llmqChainLocks = Consensus::LLMQ_TEST;

        // ★ ALL TEN FIELDS, POSITIONAL, WRITTEN EXPLICITLY. This struct is brace
        // aggregate-initialised, so a field-name grep finds nothing and a short
        // literal silently takes in-class defaults for everything it omits. The
        // previous {"ptxtest", 80, 80, 80, 1} set five and defaulted five.
        // Order: {name, B, R, budget, L, retire, grace, rate, statelessH, boundaryH}
        //
        //  name  "ptxtestnet"  matches strNetworkID; the old "ptxtest" did not.
        //  B     60   formation boundary. doc/ptx/PTX_TESTNET_GENESIS_CONFIG.md §4:
        //             M = S + 6*pb + W_mine, so B bounds tolerated per-phase
        //             propagation as 6*pb + 11 <= B. B=30 (ptxbea) => pb <= 3, which
        //             only holds at single-host RTT; B=60 => pb <= 8, 33% above the
        //             mainnet-grade pb=6 baseline. B is a security ceiling only --
        //             handover-at-accept (KDD-063) means a wider B never costs
        //             availability; rotation cadence is R, not B.
        //  R     1440 KDD-045 key-compromise window, ~1 day at 60s spacing.
        //  bud   80   ODC-050 stall-out span; must NOT track B (a ~27-block ceremony
        //             under a 30-block cadence lives on exactly this separation).
        //  L     1    five operators support ONE quorum. L_max = floor(pool/11) = 1
        //             at both 15 and 20 GMs. Declaring 8 like ptxbea would be a lie
        //             Guard 1 cannot catch.
        //  retire 200 KDD-074 idle arm, as ptxbea.
        //  grace  1   ODC-054: L>1 hard-requires grace>0 or AppInitSanityChecks
        //             aborts (ptx_formation.cpp:140-149). Harmless at L=1, correct
        //             if L ever rises.
        //  rate   40  KDD-074 limiter, as ptxbea.
        //  statelessH 0  NOT 900. ptxbea's 900 exists only so its pre-BUG-036
        //             history replays byte-identically; a fresh genesis has no such
        //             history, and 900 would run the launch window on the known-
        //             fragile stored-stamp path with its self-heal disarmed.
        //  boundaryH  0  V11 enforced from genesis -- correct for a fresh chain, so
        //             every PTXDKG this network accepts is on-boundary and the gate
        //             never needs to move.
        consensus.ptxFormation = {"ptxtestnet", 60, 1440, 80, 1, 200, 1, 40, 0, 0};

        nFulfilledRequestExpireTime = 60 * 60;

        // ★ Accumulation is via LOTTERY_ACCUM_SCRIPT (ODC-022), a derived burn
        // script -- no named pool address is used or needed. The populated address
        // here was legacy; ptxbea correctly sets "" (:1028).
        strPTXLotteryPoolAddress = "";
        nPTXServiceFee = 1 * COIN;
        // KDD-030: 5-block window for testnet (~5 min at 1 block/min); mainnet default 1440.
        nPTXSettlementWindow = 5;
        // ★ These two live on CChainParams, not Consensus::Params, so a
        // Consensus::Params audit misses them -- and their in-class DEFAULTS are
        // consensus-live. ptxtestnet was taking both defaults.
        //   nPTXSeedHeightWindow default 0 = the ODC-073 stale-seed check DISABLED
        //     (chainparams.h:147). 60 is bracketed by the commit-to-mine lag (~12
        //     blocks) below and nRetireWindow (200) above, and equals the
        //     settlement horizon. Enforced at specialtx_validation.cpp:984-987.
        //   nPTXPayoutMinerFee default 0 (chainparams.h:148) = no miner incentive
        //     inside PTXPAYOUT. Enforced at specialtx_validation.cpp:1495, :1528
        //     and ptx_payout.cpp:23.
        nPTXSeedHeightWindow = 60;
        nPTXPayoutMinerFee = 10000; // 0.0001 HMS
    }

    const CCheckpointData& Checkpoints() const
    {
        return dataPTXTestNet;
    }
};

/**
 * ptx-bea testnet -- ODC-022 Solution 1 (PTXCOALESCE nType=9, PTXPAYOUT nType=10)
 * pchMessageStart = "PTX3"  P2P port 29994  RPC port 29995 (chainparamsbase.cpp:56)
 * UPGRADE_V6_0 = ALWAYS_ACTIVE (required for protx_register + scriptPTXPayment)
 */
class CPTXBeaTestNetParams : public CChainParams
{
public:
    CPTXBeaTestNetParams()
    {
        strNetworkID = "ptxbea";

        genesis = CreateGenesisBlock(1779926400, 2550273078, 0x1e00ffff, 1, 0 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();

        assert(consensus.hashGenesisBlock == uint256S("0x000000dccda161efd26813bfd5322d7c7fb598ef20bf4fd872b7335eb8beb58b"));
        assert(genesis.hashMerkleRoot == uint256S("0x93ad7b455294f429da00d11b656d62f7fb197a72b7315f58de8c9380dbdaa113"));

        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.fPowNoRetargeting = false;
        consensus.powLimit   = uint256S("0x000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV1 = uint256S("0x000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        consensus.posLimitV2 = uint256S("0x0000fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff");
        // ★ FLEET-ONLY DIVERGENCE (2026-08-06): 144 -> 720, one superblock per ~12h at
        // 60s spacing, so budget cycles are a ROUTINE event inside a soak rather than
        // one-per-fleet-month (mainnet is 43,200 = ~30 days).
        // ★ DELIBERATELY HOSTILE, and by how much: the payout window is
        // `nHeight % nBudgetCycleBlocks < 100` (gamemaster-payments.cpp:205), so here
        // 100/720 = ~14% OF ALL BLOCKS sit in the window, against mainnet's
        // 100/43,200 = 0.23% — a ~60x over-exposure. Same logic as the old fleet's
        // nTimeSlotLength accident, except on purpose this time: the MECHANISM is
        // under test, not the cadence. Do not read fleet superblock frequency as
        // representative of mainnet.
        consensus.nBudgetCycleBlocks = 720;
        consensus.nBudgetFeeConfirmations = 3;
        consensus.nCoinbaseMaturity = 10;
        consensus.nFutureTimeDriftPoW = 7200;
        consensus.nFutureTimeDriftPoS = 180;
        consensus.nMaxMoneyOut = 30000000 * COIN;
        consensus.nGMCollateralAmt = 100 * COIN;
        consensus.nGMBlockReward = 3 * COIN;
        consensus.nNewGMBlockReward = 2.674999995 * COIN;
        consensus.nGMCollateralMinConf = 1;
        consensus.nProposalEstablishmentTime = 60 * 5;
        consensus.nStakeMinAge = 0;
        consensus.nStakeMinDepth = 20;
        consensus.nTargetTimespan = 20 * 60;
        consensus.nTargetTimespanV2 = 20 * 60;
        consensus.nTargetSpacing = 1 * 60;
        // ★ FIDELITY CHANGE (2026-08-06): 60 -> 15 to MATCH MAINNET
        // (mainnet nTimeSlotLength = 15). nTargetSpacing stays 60, so block cadence
        // is unchanged; what changes is the PoS time-slot quantisation the kernel
        // hashes against. Consequence noted though irrelevant on one host: the peer
        // time-offset acceptance window narrows to +/-30s
        // (net_processing.cpp:1494-1496 derives it from nTimeSlotLength).
        consensus.nTimeSlotLength = 15;
        consensus.nMaxProposalPayments = 6;

        // ★ FLEET-ONLY DIVERGENCE (2026-08-06) — spork key the harness actually holds.
        // The previous ptxbea spork pubkey had no matching private key anywhere in the
        // harness or repo, so SPORK_13_ENABLE_SUPERBLOCKS (which defaults OFF,
        // spork.cpp:20 = 4070908800) could never be switched on and the DAO/budget path
        // was untestable. Replaced with a keypair generated for the fleet; the PRIVATE
        // half lives in $PTXBEA_SPORK_KEY in the environment, never in this repo
        // (testnet/scenarios/spork_gm_payment.py already reads it from there and mounts
        // it chmod-600 read-only).
        //
        // ★ ASYMMETRY WORTH REMEMBERING WHEN SUPERBLOCK TESTS PASS HERE: mainnet spork
        // key CUSTODY IS AN OPEN INHERITED QUESTION (flagged in
        // doc/ptx/INHERITED_PARAMS_AUDIT.md). This fleet can drive sporks because we
        // made it able to; mainnet's ability to do so is UNVERIFIED. A green superblock
        // test on ptxbea therefore proves the MECHANISM works — it proves nothing about
        // whether anyone can actually operate it on mainnet.
        consensus.strSporkPubKey = "04E54CED11E486341A9AB5A67A7790379BD2A15273E8CF3C726A34C372D035D6176D78017F905C517922B22F8B8FFBE9A814AD3BB4B9AC05AD6A01B62D4491302E";

        consensus.height_last_invalid_UTXO = -1;
        consensus.height_last_ZC_AccumCheckpoint = -1;
        consensus.height_last_ZC_WrappedSerials = -1;

        consensus.ZC_Modulus = "25195908475657893494027183240048398571429282126204032027777137836043662020707595556264018525880784"
                "4069182906412495150821892985591491761845028084891200728449926873928072877767359714183472702618963750149718246911"
                "6507761337985909570009733045974880842840179742910064245869181719511874612151517265463228221686998754918242243363"
                "7259085141865462043576798423387184774447920739934236584823824281198163815010674810451660377306056201619676256133"
                "8441436038339044149526344321901146575444541784240209246165157233507787077498171257724679629263863563732899121548"
                "31438167899885040445364023527381951378636564391212010397122822120720357";
        consensus.ZC_MaxPublicSpendsPerTx = 637;
        consensus.ZC_MaxSpendsPerTx = 7;
        consensus.ZC_MinMintConfirmations = 10;
        consensus.ZC_MinMintFee = 1 * CENT;
        consensus.ZC_MinStakeDepth = 10;
        consensus.ZC_TimeStart = 4070908800;
        consensus.ZC_HeightStart = 100000000;

        // Network upgrades — UPGRADE_V6_0 is ALWAYS_ACTIVE on ptx-bea (required for protx_register*/scriptPTXPayment)
        consensus.vUpgrades[Consensus::BASE_NETWORK].nActivationHeight =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_TESTDUMMY].nActivationHeight =
                Consensus::NetworkUpgrade::NO_ACTIVATION_HEIGHT;
        consensus.vUpgrades[Consensus::UPGRADE_POS].nActivationHeight           = 50;
        consensus.vUpgrades[Consensus::UPGRADE_POS_V2].nActivationHeight        = 50;
        consensus.vUpgrades[Consensus::UPGRADE_ZC].nActivationHeight            = 100000000;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_V2].nActivationHeight         = 100000000;
        consensus.vUpgrades[Consensus::UPGRADE_BIP65].nActivationHeight         =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;
        consensus.vUpgrades[Consensus::UPGRADE_ZC_PUBLIC].nActivationHeight     = 100000000;
        consensus.vUpgrades[Consensus::UPGRADE_V3_4].nActivationHeight          = 51;
        consensus.vUpgrades[Consensus::UPGRADE_V4_0].nActivationHeight          = 265;
        consensus.vUpgrades[Consensus::UPGRADE_V5_0].nActivationHeight          = 50;
        consensus.vUpgrades[Consensus::UPGRADE_V5_2].nActivationHeight          = 50;
        consensus.vUpgrades[Consensus::UPGRADE_V5_3].nActivationHeight          = 50;
        consensus.vUpgrades[Consensus::UPGRADE_V5_5].nActivationHeight          = 50;
        consensus.vUpgrades[Consensus::UPGRADE_V6_0].nActivationHeight          =
                Consensus::NetworkUpgrade::ALWAYS_ACTIVE;

        pchMessageStart[0] = 0x50; // 'P'
        pchMessageStart[1] = 0x54; // 'T'
        pchMessageStart[2] = 0x58; // 'X'
        pchMessageStart[3] = 0x33; // '3'
        nDefaultPort = 29994;

        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1, 139);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1, 19);
        base58Prefixes[STAKING_ADDRESS] = std::vector<unsigned char>(1, 73);
        base58Prefixes[SECRET_KEY] = std::vector<unsigned char>(1, 239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x3a, 0x80, 0x61, 0xa0};
        base58Prefixes[EXT_SECRET_KEY] = {0x3a, 0x80, 0x58, 0x37};
        base58Prefixes[EXT_COIN_TYPE] = {0x80, 0x00, 0x00, 0x01};

        vFixedSeeds.clear();

        fRequireStandard = false;

        bech32HRPs[SAPLING_PAYMENT_ADDRESS]      = "ptxbea";
        bech32HRPs[SAPLING_FULL_VIEWING_KEY]     = "ptxbeaview";
        bech32HRPs[SAPLING_INCOMING_VIEWING_KEY] = "ptxbeaivk";
        bech32HRPs[SAPLING_EXTENDED_SPEND_KEY]   = "p-secret-spending-key-ptxbea";
        bech32HRPs[SAPLING_EXTENDED_FVK]         = "ptxbeaxview";

        bech32HRPs[BLS_SECRET_KEY]               = "bls-sk-ptxbea";
        bech32HRPs[BLS_PUBLIC_KEY]               = "bls-pk-ptxbea";

        consensus.llmqs[Consensus::LLMQ_TEST] = llmq_test;
        nLLMQConnectionRetryTimeout = 10;
        consensus.llmqChainLocks = Consensus::LLMQ_TEST;

        // PTX formation cadence (W2.2 SG-1b): dev test-N = 80, M-coupled
        // (~1.7x the ceremony floor M~47; SG-1b plan-gate decision 1).
        // ★ W2.5b FLEET SHAPE (2026-07-28): the multi-quorum drill config —
        // B=30 boundaries / R=1440 rotation age / budget=80 (Guard 3: the
        // stall-out must NOT track the cadence — a ~27-block ceremony under a
        // 30-block boundary lives on this separation) / L=8 declared
        // (Guard 1: capacity 1440/30 = 48 >= 8, margin 48 >= 16 — quiet) /
        // reform gate LIVE 200,1,40 (ODC-054: L>1 HARD-REQUIRES grace>0 —
        // CheckParams refuses the gate-off variant at startup).  ptxbea is
        // the FLEET's chain (-ptxbea, GMAUTH subnet carve-out); main/test/
        // ptxtest/regtest stay at their L=1 shapes untouched.
        consensus.ptxFormation = {"ptxbea", 30, 1440, 80, 8, 200, 1, 40, 900};  // W2.5b fleet shape + BUG-036 stateless-reform activation @900 (fresh-genesis 2026-08-15 chain: legacy pacing replays <900 byte-identically; fleet upgrades hours before h900, first legacy reform can't fire before ~h500)

        nFulfilledRequestExpireTime = 60 * 60;

        // ptx-bea uses no lottery pool address — accumulation handled by LOTTERY_ACCUM_SCRIPT (ODC-022)
        strPTXLotteryPoolAddress = "";
        nPTXServiceFee = 1 * COIN;
        nPTXSettlementWindow = 60;  // ~60-min settlement window (testnet middle ground)
        // ODC-073 Step 1 — nSeedHeight past-anchor bound. Bracketed by two real
        // quantities: FLOOR = legitimate commit-to-mine lag (fund-then-sign mines
        // the commitment within 1-2 blocks; a dozen blocks covers congestion +
        // reorg re-mining + coordinator retry), so 60 >> ~12; CEILING = the
        // reform idle window nRetireWindow=200 (a quorum active at H keeps its
        // members' CURRENT shares until the idle arm can reform it, which needs
        // 200 idle blocks), so 60 < 200 makes this consensus bound strictly
        // TIGHTER than the practical current-share bound (reachable-past horizon
        // 200 -> 60, and UNBOUNDED -> 60 for long-lived active quorums). 60 also
        // equals the settlement horizon above (a roll is never anchored older
        // than its own settlement window). Aiming among currently-active quorums
        // stays open — that is ODC-073 Step 4 (routed-quorum enforcement), deferred.
        nPTXSeedHeightWindow = 60;
        nPTXPayoutMinerFee = 10000; // 0.0001 HMS — miner incentive inside PTXPAYOUT
    }

    const CCheckpointData& Checkpoints() const
    {
        return dataPTXBeaTestNet;
    }
};

static std::unique_ptr<CChainParams> globalChainParams;

const CChainParams &Params()
{
    assert(globalChainParams);
    return *globalChainParams;
}

std::unique_ptr<CChainParams> CreateChainParams(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN)
        return std::unique_ptr<CChainParams>(new CMainParams());
    else if (chain == CBaseChainParams::TESTNET)
        return std::unique_ptr<CChainParams>(new CTestNetParams());
    else if (chain == CBaseChainParams::REGTEST)
        return std::unique_ptr<CChainParams>(new CRegTestParams());
    else if (chain == CBaseChainParams::PTXTESTNET)
        return std::unique_ptr<CChainParams>(new CPTXTestNetParams());
    else if (chain == CBaseChainParams::PTXBEATESTNET)
        return std::unique_ptr<CChainParams>(new CPTXBeaTestNetParams());
    throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectParams(const std::string& network)
{
    SelectBaseParams(network);
    globalChainParams = CreateChainParams(network);
}

void UpdateNetworkUpgradeParameters(Consensus::UpgradeIndex idx, int nActivationHeight)
{
    globalChainParams->UpdateNetworkUpgradeParameters(idx, nActivationHeight);
}
