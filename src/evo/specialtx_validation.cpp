// Copyright (c) 2017 The Dash Core developers
// Copyright (c) 2020-2022 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "evo/specialtx_validation.h"

#include "chain.h"
#include "coins.h"
#include "chainparams.h"
#include "clientversion.h"
#include "consensus/validation.h"
#include "evo/deterministicgms.h"
#include "evo/providertx.h"
#include "llmq/quorums_blockprocessor.h"
#include "messagesigner.h"
#include "primitives/transaction.h"
#include "primitives/block.h"
#include "script/standard.h"
#include "ptx/ptx_accum_script.h"
#include "ptx/ptx_coalesce.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_winner_selection.h"
#include "spork.h"
#include "crypto/sha256.h"
#include "utilstrencodings.h"

/* -- Helper static functions -- */

static bool CheckService(const CService& addr, CValidationState& state)
{
    if (!addr.IsValid()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr");
    }
    if (!Params().IsRegTestNet() && !addr.IsRoutable()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr");
    }

    // IP port must be the default one on main-net, which cannot be used on other nets.
    static int mainnetDefaultPort = CreateChainParams(CBaseChainParams::MAIN)->GetDefaultPort();
    if (Params().NetworkIDString() == CBaseChainParams::MAIN) {
        if (addr.GetPort() != mainnetDefaultPort) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr-port");
        }
    } else if (addr.GetPort() == mainnetDefaultPort) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr-port");
    }

    // !TODO: add support for IPv6 and Tor
    if (!addr.IsIPv4()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ipaddr");
    }

    return true;
}

template <typename Payload>
static bool CheckHashSig(const Payload& pl, const CKeyID& keyID, CValidationState& state)
{
    std::string strError;
    if (!CHashSigner::VerifyHash(::SerializeHash(pl), keyID, pl.vchSig, strError)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-sig", false, strError);
    }
    return true;
}

template <typename Payload>
static bool CheckHashSig(const Payload& pl, const CBLSPublicKey& pubKey, CValidationState& state)
{
    if (!pl.sig.VerifyInsecure(pubKey, ::SerializeHash(pl))) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-sig", false);
    }
    return true;
}

template <typename Payload>
static bool CheckStringSig(const Payload& pl, const CKeyID& keyID, CValidationState& state)
{
    std::string strError;
    if (!CMessageSigner::VerifyMessage(keyID, pl.vchSig, pl.MakeSignString(), strError)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-sig", false, strError);
    }
    return true;
}

template <typename Payload>
static bool CheckInputsHash(const CTransaction& tx, const Payload& pl, CValidationState& state)
{
    if (CalcTxInputsHash(tx) != pl.inputsHash) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-inputs-hash");
    }

    return true;
}

static bool CheckCollateralOut(const CTxOut& out, const ProRegPL& pl, CValidationState& state, CTxDestination& collateralDestRet)
{
    if (!ExtractDestination(out.scriptPubKey, collateralDestRet)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-dest");
    }
    // don't allow reuse of collateral key for other keys (don't allow people to put the collateral key onto an online server)
    // this check applies to internal and external collateral, but internal collaterals are not necessarely a P2PKH
    if (collateralDestRet == CTxDestination(pl.keyIDOwner) ||
            collateralDestRet == CTxDestination(pl.keyIDVoting)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-reuse");
    }
    // check collateral amount
    if (out.nValue != Params().GetConsensus().nGMCollateralAmt) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-collateral-amount");
    }
    return true;
}

// ODC-022 KDD-033: validate a v3 ProRegPL node_id against Amendment 1 rules and the
// chain-derived suffix.  Exposed for unit testing (avoids needing a valid BLS pubkey
// in tests focused on node_id format correctness).
bool ValidateProRegNodeId(const std::string& node_id, const COutPoint& collateral, CValidationState& state)
{
    // Must have exactly one colon separating label from chain-computed suffix.
    size_t colon = node_id.find(':');
    if (colon == std::string::npos || node_id.find(':', colon + 1) != std::string::npos) {
        return state.DoS(100, error("%s: node_id not in label:suffix form", __func__),
                         REJECT_INVALID, "bad-protx-node-id-format");
    }
    std::string label  = node_id.substr(0, colon);
    std::string suffix = node_id.substr(colon + 1);

    // Amendment 1: label sanity envelope
    if (label.size() < 3 || label.size() > 24) {
        return state.DoS(100, error("%s: node_id label length %d not in [3,24]", __func__, (int)label.size()),
                         REJECT_INVALID, "bad-protx-node-id-label-length");
    }
    for (unsigned char c : label) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) {
            return state.DoS(100, error("%s: node_id label contains invalid character", __func__),
                             REJECT_INVALID, "bad-protx-node-id-label-charset");
        }
    }
    if (label.front() == '-' || label.front() == '_' ||
        label.back()  == '-' || label.back()  == '_') {
        return state.DoS(100, error("%s: node_id label has leading/trailing edge character", __func__),
                         REJECT_INVALID, "bad-protx-node-id-label-edge");
    }
    bool allNumeric = true;
    for (char c : label) { if (c < '0' || c > '9') { allNumeric = false; break; } }
    if (allNumeric) {
        return state.DoS(100, error("%s: node_id label is all-numeric", __func__),
                         REJECT_INVALID, "bad-protx-node-id-label-numeric");
    }
    static const std::set<std::string> reserved = {
        "admin","system","null","none","gm","gamemaster","node","default","test"};
    if (reserved.count(ToLower(label))) {
        return state.DoS(100, error("%s: node_id label is a reserved word", __func__),
                         REJECT_INVALID, "bad-protx-node-id-label-reserved");
    }

    // Verify suffix = hex_lower(SHA256(serialize(collateralOutpoint))[0:4])
    CDataStream ss(SER_GETHASH, PROTOCOL_VERSION);
    ss << collateral;
    unsigned char digest[32];
    CSHA256().Write((const unsigned char*)ss.data(), ss.size()).Finalize(digest);
    std::string expectedSuffix = HexStr(Span<const uint8_t>(digest, digest + 4));
    if (suffix != expectedSuffix) {
        return state.DoS(100, error("%s: node_id suffix %s does not match collateral-derived %s",
                                    __func__, suffix, expectedSuffix),
                         REJECT_INVALID, "bad-protx-node-id-suffix");
    }
    return true;
}

// ODC-022 Step 10: validate the optional scriptPTXPayment field on a ProRegPL.
// Empty is the legitimate opt-out; non-empty must be P2PKH so the PTXPAYOUT output
// is always spendable.  Mirrors the identical constraint on scriptPayout.
bool ValidateProRegPTXPayee(const ProRegPL& pl, CValidationState& state)
{
    if (!pl.scriptPTXPayment.empty() && !pl.scriptPTXPayment.IsPayToPublicKeyHash()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-ptx-payee");
    }
    return true;
}

// Provider Register Payload
static bool CheckProRegTx(const CTransaction& tx, const CBlockIndex* pindexPrev, const CCoinsViewCache* view, CValidationState& state)
{
    assert(tx.nType == CTransaction::TxType::PROREG);

    ProRegPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > ProRegPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-version");
    }
    if (pl.nType != 0) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-type");
    }
    if (pl.nMode != 0) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-mode");
    }

    if (pl.keyIDOwner.IsNull() || pl.keyIDVoting.IsNull()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-key-null");
    }
    if (!pl.pubKeyOperator.IsValid()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-key-invalid");
    }
    // we may support other kinds of scripts later, but restrict it for now
    if (!pl.scriptPayout.IsPayToPublicKeyHash()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee");
    }
    if (!pl.scriptOperatorPayout.empty() && !pl.scriptOperatorPayout.IsPayToPublicKeyHash()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-payee");
    }
    if (!ValidateProRegPTXPayee(pl, state)) return false;

    CTxDestination payoutDest;
    if (!ExtractDestination(pl.scriptPayout, payoutDest)) {
        // should not happen as we checked script types before
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-dest");
    }
    // don't allow reuse of payout key for other keys (don't allow people to put the payee key onto an online server)
    if (payoutDest == CTxDestination(pl.keyIDOwner) ||
            payoutDest == CTxDestination(pl.keyIDVoting)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-reuse");
    }

    // It's allowed to set addr to 0, which will put the GM into PoSe-banned state and require a ProUpServTx to be issues later
    // If any of both is set, it must be valid however
    if (pl.addr != CService() && !CheckService(pl.addr, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (pl.nOperatorReward > 10000) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-reward");
    }

    // ODC-022 KDD-033: v3 node_id validation (Amendment 1 label envelope + suffix verification)
    if (pl.nVersion >= 3 && !pl.node_id.empty()) {
        COutPoint actual = pl.collateralOutpoint.hash.IsNull()
            ? COutPoint(tx.GetHash(), pl.collateralOutpoint.n)
            : pl.collateralOutpoint;
        if (!ValidateProRegNodeId(pl.node_id, actual, state)) return false;
    }

    if (pl.collateralOutpoint.hash.IsNull()) {
        // collateral included in the proReg tx
        if (pl.collateralOutpoint.n >= tx.vout.size()) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-index");
        }
        CTxDestination collateralTxDest;
        if (!CheckCollateralOut(tx.vout[pl.collateralOutpoint.n], pl, state, collateralTxDest)) {
            // pass the state returned by the function above
            return false;
        }
        // collateral is part of this ProRegTx, so we know the collateral is owned by the issuer
        if (!pl.vchSig.empty()) {
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-sig");
        }
    } else if (pindexPrev != nullptr) {
        assert(view != nullptr);

        // Referenced external collateral.
        // This is checked only when pindexPrev is not null (thus during ConnectBlock-->CheckSpecialTx),
        // because this is a contextual check: we need the updated utxo set, to verify that
        // the coin exists and it is unspent.
        Coin coin;
        if (!view->GetUTXOCoin(pl.collateralOutpoint, coin)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral");
        }
        CTxDestination collateralTxDest;
        if (!CheckCollateralOut(coin.out, pl, state, collateralTxDest)) {
            // pass the state returned by the function above
            return false;
        }
        // Extract key from collateral. This only works for P2PK and P2PKH collaterals and will fail for P2SH.
        // Issuer of this ProRegTx must prove ownership with this key by signing the ProRegTx
        const CKeyID* keyForPayloadSig = boost::get<CKeyID>(&collateralTxDest);
        if (!keyForPayloadSig) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-pkh");
        }
        // collateral is not part of this ProRegTx, so we must verify ownership of the collateral
        if (!CheckStringSig(pl, *keyForPayloadSig, state)) {
            // pass the state returned by the function above
            return false;
        }
    }

    if (!CheckInputsHash(tx, pl, state)) {
        return false;
    }

    if (pindexPrev) {
        auto gmList = deterministicGMManager->GetListForBlock(pindexPrev);
        // only allow reusing of addresses when it's for the same collateral (which replaces the old GM)
        if (gmList.HasUniqueProperty(pl.addr) && gmList.GetUniquePropertyGM(pl.addr)->collateralOutpoint != pl.collateralOutpoint) {
            return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-IP-address");
        }
        // never allow duplicate keys, even if this ProTx would replace an existing GM
        if (gmList.HasUniqueProperty(pl.keyIDOwner)) {
            return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-owner-key");
        }
        if (gmList.HasUniqueProperty(pl.pubKeyOperator)) {
            return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-operator-key");
        }
        // ODC-022: case-insensitive full compound node_id uniqueness (prevents confusable squatting)
        if (pl.nVersion >= 3 && !pl.node_id.empty()) {
            const std::string lowerFull = ToLower(pl.node_id);
            bool dup = false;
            gmList.ForEachGM(false, [&](const CDeterministicGMCPtr& dgm) {
                if (!dup && !dgm->pdgmState->node_id.empty() &&
                    ToLower(dgm->pdgmState->node_id) == lowerFull) {
                    dup = true;
                }
            });
            if (dup) {
                return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-node-id");
            }
        }
    }

    return true;
}

// Provider Update Service Payload
static bool CheckProUpServTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    assert(tx.nType == CTransaction::TxType::PROUPSERV);

    ProUpServPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > ProUpServPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-version");
    }

    if (!CheckService(pl.addr, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (!CheckInputsHash(tx, pl, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (pindexPrev) {
        auto gmList = deterministicGMManager->GetListForBlock(pindexPrev);
        auto gm = gmList.GetGM(pl.proTxHash);
        if (!gm) {
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
        }

        // don't allow updating to addresses already used by other GMs
        if (gmList.HasUniqueProperty(pl.addr) && gmList.GetUniquePropertyGM(pl.addr)->proTxHash != pl.proTxHash) {
            return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-addr");
        }

        if (!pl.scriptOperatorPayout.empty()) {
            if (gm->nOperatorReward == 0) {
                // don't allow to set operator reward payee in case no operatorReward was set
                return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-payee");
            }
            // we may support other kinds of scripts later, but restrict it for now
            if (!pl.scriptOperatorPayout.IsPayToPublicKeyHash()) {
                return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-payee");
            }
        }

        // we can only check the signature if pindexPrev != nullptr and the GM is known
        if (!CheckHashSig(pl, gm->pdgmState->pubKeyOperator.Get(), state)) {
            // pass the state returned by the function above
            return false;
        }
    }

    return true;
}

// Provider Update Registrar Payload
static bool CheckProUpRegTx(const CTransaction& tx, const CBlockIndex* pindexPrev, const CCoinsViewCache* view, CValidationState& state)
{
    assert(tx.nType == CTransaction::TxType::PROUPREG);

    ProUpRegPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > ProUpRegPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-version");
    }
    if (pl.nMode != 0) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-mode");
    }

    if (!pl.pubKeyOperator.IsValid()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-operator-key-invalid");
    }
    if (pl.keyIDVoting.IsNull()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-voting-key-null");
    }
    // !TODO: enable other scripts
    if (!pl.scriptPayout.IsPayToPublicKeyHash()) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee");
    }

    CTxDestination payoutDest;
    if (!ExtractDestination(pl.scriptPayout, payoutDest)) {
        // should not happen as we checked script types before
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-dest");
    }

    // don't allow reuse of payee key for other keys
    if (payoutDest == CTxDestination(pl.keyIDVoting)) {
        return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-reuse");
    }

    if (!CheckInputsHash(tx, pl, state)) {
        return false;
    }

    if (pindexPrev) {
        assert(view != nullptr);

        // ProUpReg txes are disabled when the legacy system is still active
        // !TODO: remove after complete transition to DGM
        if (!deterministicGMManager->LegacyGMObsolete(pindexPrev->nHeight + 1)) {
            return state.DoS(10, false, REJECT_INVALID, "spork-21-inactive");
        }

        auto gmList = deterministicGMManager->GetListForBlock(pindexPrev);
        auto dgm = gmList.GetGM(pl.proTxHash);
        if (!dgm) {
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");
        }

        // don't allow reuse of payee key for owner key
        if (payoutDest == CTxDestination(dgm->pdgmState->keyIDOwner)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-payee-reuse");
        }

        Coin coin;
        if (!view->GetUTXOCoin(dgm->collateralOutpoint, coin)) {
            // this should never happen (there would be no dgm otherwise)
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-collateral");
        }

        // don't allow reuse of collateral key for other keys (don't allow people to put the payee key onto an online server)
        CTxDestination collateralTxDest;
        if (!ExtractDestination(coin.out.scriptPubKey, collateralTxDest)) {
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-collateral-dest");
        }
        if (collateralTxDest == CTxDestination(dgm->pdgmState->keyIDOwner) ||
                collateralTxDest == CTxDestination(pl.keyIDVoting)) {
            return state.DoS(10, false, REJECT_INVALID, "bad-protx-collateral-reuse");
        }

        if (gmList.HasUniqueProperty(pl.pubKeyOperator)) {
            auto otherDgm = gmList.GetUniquePropertyGM(pl.pubKeyOperator);
            if (pl.proTxHash != otherDgm->proTxHash) {
                return state.DoS(10, false, REJECT_DUPLICATE, "bad-protx-dup-key");
            }
        }

        if (!CheckHashSig(pl, dgm->pdgmState->keyIDOwner, state)) {
            // pass the state returned by the function above
            return false;
        }

    }

    return true;
}

// Provider Update Revoke Payload
static bool CheckProUpRevTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state)
{
    assert(tx.nType == CTransaction::TxType::PROUPREV);

    ProUpRevPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > ProUpRevPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-version");
    }

    // pl.nReason < ProUpRevPL::REASON_NOT_SPECIFIED is always `false` since
    // pl.nReason is unsigned and ProUpRevPL::REASON_NOT_SPECIFIED == 0
    if (pl.nReason > ProUpRevPL::REASON_LAST) {
        return state.DoS(100, false, REJECT_INVALID, "bad-protx-reason");
    }

    if (!CheckInputsHash(tx, pl, state)) {
        // pass the state returned by the function above
        return false;
    }

    if (pindexPrev) {
        auto gmList = deterministicGMManager->GetListForBlock(pindexPrev);
        auto dgm = gmList.GetGM(pl.proTxHash);
        if (!dgm)
            return state.DoS(100, false, REJECT_INVALID, "bad-protx-hash");

        if (!CheckHashSig(pl, dgm->pdgmState->pubKeyOperator.Get(), state)) {
            // pass the state returned by the function above
            return false;
        }
    }

    return true;
}

// LLMQ final commitment Payload
bool VerifyLLMQCommitment(const llmq::CFinalCommitment& qfc, const CBlockIndex* pindexPrev, CValidationState& state)
{
    AssertLockHeld(cs_main);

    // Check DKG maintenance mode
    if (sporkManager.IsSporkActive(SPORK_22_LLMQ_DKG_MAINTENANCE) && !IsInitialBlockDownload()) {
        // only null commitments are accepted
        if (!qfc.IsNull()) {
            return state.DoS(50, false, REJECT_INVALID, "bad-qc-not-null-spork22");
        }
    }

    // Check version
    if (qfc.nVersion == 0 || qfc.nVersion > llmq::CFinalCommitment::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-version");
    }

    // Check type
    Optional<Consensus::LLMQParams> params = Params().GetConsensus().GetLLMQParams(qfc.llmqType);
    if (params == nullopt) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-type");
    }

    // Check sizes
    if (!qfc.VerifySizes(*params)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-sizes");
    }

    if (pindexPrev) {
        // Get quorum index
        CBlockIndex* pindexQuorum = LookupBlockIndex(qfc.quorumHash);
        if (!pindexQuorum) {
            return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-hash-not-found");
        }

        // Check height
        if (pindexQuorum->nHeight % params->dkgInterval != 0) {
            // not first block of DKG interval
            return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-height");
        }

        // Check height limit
        if (pindexPrev->nHeight - pindexQuorum->nHeight > params->cacheDkgInterval) {
            return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-height-old");
        }

        if (pindexQuorum != pindexPrev->GetAncestor(pindexQuorum->nHeight)) {
            // not part of active chain
            return state.DoS(100, false, REJECT_INVALID, "bad-qc-quorum-hash-not-active-chain");
        }

        // Get members and check signatures (for not-null commitments)
        if (!qfc.IsNull()) {
            std::vector<CBLSPublicKey> allkeys;
            for (const auto& m : deterministicGMManager->GetAllQuorumMembers((Consensus::LLMQType)qfc.llmqType, pindexQuorum)) {
                allkeys.emplace_back(m->pdgmState->pubKeyOperator.Get());
            }
            if (!qfc.Verify(allkeys, *params)) {
                return state.DoS(100, false, REJECT_INVALID, "bad-qc-invalid");
            }
        }
    }

    return true;
}

static bool CheckLLMQCommitmentTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state) EXCLUSIVE_LOCKS_REQUIRED(cs_main)
{
    AssertLockHeld(cs_main);

    llmq::LLMQCommPL pl;
    if (!GetTxPayload(tx, pl)) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-payload");
    }

    if (pl.nVersion == 0 || pl.nVersion > llmq::LLMQCommPL::CURRENT_VERSION) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-version");
    }

    if (pindexPrev && pl.nHeight != (uint32_t)pindexPrev->nHeight + 1) {
        return state.DoS(100, false, REJECT_INVALID, "bad-qc-height");
    }

    return VerifyLLMQCommitment(pl.commitment, pindexPrev, state);
}

// Basic non-contextual checks for all tx types
static bool CheckSpecialTxBasic(const CTransaction& tx, CValidationState& state)
{
    bool hasExtraPayload = tx.hasExtraPayload();

    if (tx.IsNormalType()) {
        // Type-0 txes don't have extra payload
        if (hasExtraPayload) {
            return state.DoS(100, error("%s: Type 0 doesn't support extra payload", __func__),
                             REJECT_INVALID, "bad-txns-type-payload");
        }
        // Normal transaction. Nothing to check
        return true;
    }

    // Special txes need at least version 2
    if (!tx.isSaplingVersion()) {
        return state.DoS(100, error("%s: Type %d not supported with version %d", __func__, tx.nType, tx.nVersion),
                         REJECT_INVALID, "bad-txns-type-version");
    }

    // Cannot be coinbase/coinstake tx
    if (tx.IsCoinBase() || tx.IsCoinStake()) {
        return state.DoS(10, error("%s: Special tx is coinbase or coinstake", __func__),
                         REJECT_INVALID, "bad-txns-special-coinbase");
    }

    // Special txes must have a non-empty payload.
    // Exception: PTXCOALESCE/PTXPAYOUT carry present-but-empty extraPayload by
    // design (ODC-022 §3.1) — their validity is established by consensus rules,
    // not by payload content.  Exempt both here so rules in CheckSpecialTx run.
    bool isPTXBlockOnly = (tx.nType == CTransaction::TxType::PTXCOALESCE ||
                           tx.nType == CTransaction::TxType::PTXPAYOUT);
    if (!hasExtraPayload && !isPTXBlockOnly) {
        return state.DoS(100, error("%s: Special tx (type=%d) without extra payload", __func__, tx.nType),
                         REJECT_INVALID, "bad-txns-payload-empty");
    }

    // Size limits (skipped for present-but-empty payloads; size() == 0 is always safe)
    if (hasExtraPayload && tx.extraPayload->size() > MAX_SPECIALTX_EXTRAPAYLOAD) {
        return state.DoS(100, error("%s: Special tx payload oversize (%d)", __func__, tx.extraPayload->size()),
                         REJECT_INVALID, "bad-txns-payload-oversize");
    }

    return true;
}

// contextual and non-contextual per-type checks
// - pindexPrev=null: CheckBlock-->CheckSpecialTxNoContext
// - pindexPrev=chainActive.Tip: AcceptToMemoryPoolWorker-->CheckSpecialTx
// - pindexPrev=pindex->pprev: ConnectBlock-->ProcessSpecialTxsInBlock-->CheckSpecialTx
bool CheckSpecialTx(const CTransaction& tx, const CBlockIndex* pindexPrev, const CCoinsViewCache* view, CValidationState& state)
{
    AssertLockHeld(cs_main);

    if (!CheckSpecialTxBasic(tx, state)) {
        // pass the state returned by the function above
        return false;
    }
    if (pindexPrev) {
        // reject special transactions before enforcement
        if (!tx.IsNormalType() && !tx.IsProbabilisticTx() &&
            !tx.IsPTXCoalesceTx() && !tx.IsPTXPayoutTx() &&
            !Params().GetConsensus().NetworkUpgradeActive(pindexPrev->nHeight + 1, Consensus::UPGRADE_V6_0)) {
            return state.DoS(100, error("%s: Special tx when v6 upgrade not enforced yet", __func__),
                             REJECT_INVALID, "bad-txns-v6-not-active");
        }
    }
    // per-type checks
    switch (tx.nType) {
        case CTransaction::TxType::NORMAL: {
            // nothing to check
            return true;
        }
        case CTransaction::TxType::PROREG: {
            // provider-register
            return CheckProRegTx(tx, pindexPrev, view, state);
        }
        case CTransaction::TxType::PROUPSERV: {
            // provider-update-service
            return CheckProUpServTx(tx, pindexPrev, state);
        }
        case CTransaction::TxType::PROUPREG: {
            // provider-update-registrar
            return CheckProUpRegTx(tx, pindexPrev, view, state);
        }
        case CTransaction::TxType::PROUPREV: {
            // provider-update-revoke
            return CheckProUpRevTx(tx, pindexPrev, state);
        }
        case CTransaction::TxType::LLMQCOMM: {
            // quorum commitment
            return CheckLLMQCommitmentTx(tx, pindexPrev, state);
        }
        case CTransaction::TxType::PTX: {
            CProbabilisticTxPayload payload;
            if (!GetTxPayload(tx, payload))
                return state.Invalid(false, REJECT_INVALID, "ptx-bad-payload");
            if (payload.low > payload.high)
                return state.Invalid(false, REJECT_INVALID, "ptx-bad-range");
            if (payload.count == 0)
                return state.Invalid(false, REJECT_INVALID, "ptx-zero-count");
            if (payload.results.size() != payload.count)
                return state.Invalid(false, REJECT_INVALID, "ptx-result-count-mismatch");
            for (int64_t v : payload.results)
                if (v < payload.low || v > payload.high)
                    return state.Invalid(false, REJECT_INVALID, "ptx-result-out-of-range");
            if (payload.quorum_sig_hash.IsNull())
                return state.Invalid(false, REJECT_INVALID, "ptx-missing-sig");
            if (payload.nSeedHeight == 0)
                return state.Invalid(false, REJECT_INVALID, "ptx-bad-height");
            // ODC-022 §3.3: every PTXSESS must carry exactly one output to
            // LOTTERY_ACCUM_SCRIPT at value nPTXServiceFee.
            {
                const CScript& accumScript = GetLotteryAccumScript();
                const CAmount  serviceFee  = Params().PTXServiceFee();
                int accumCount = 0;
                for (const CTxOut& out : tx.vout) {
                    if (out.scriptPubKey == accumScript && out.nValue == serviceFee)
                        ++accumCount;
                }
                if (accumCount != 1)
                    return state.Invalid(false, REJECT_INVALID, "ptx-bad-accum-output");
            }
            return true;
        }
        case CTransaction::TxType::PTXCOALESCE: {
            // ODC-022 §3.4 — per-tx rules C1–C6.
            // C7 (at-most-one-per-block) and C8 (mandatory-iff-PTXSESS) are
            // block-scoped and checked in ProcessSpecialTxsInBlock below.

            const CScript& accumScript = GetLotteryAccumScript();

            // C1: every input must spend a LOTTERY_ACCUM_SCRIPT UTXO.
            if (tx.vin.empty()) {
                return state.DoS(100, error("%s: PTXCOALESCE has no inputs", __func__),
                                 REJECT_INVALID, "ptxcoalesce-no-inputs");
            }
            // view is non-null in all production paths (ConnectBlock→ProcessSpecialTxsInBlock
            // always provides a view).  CheckSpecialTxNoContext (used by CheckBlock) passes
            // view=nullptr; the authoritative enforcement is in ConnectBlock.
            if (view) {
                for (const CTxIn& txin : tx.vin) {
                    const Coin& coin = view->AccessCoin(txin.prevout);
                    // If spent: inputs were consumed by UpdateCoins earlier in this block's
                    // per-tx loop; HaveInputs + VerifyScript exemption already verified them
                    // as LOTTERY_ACCUM_SCRIPT.  Skip to avoid false positive.
                    if (coin.IsSpent()) continue;
                    if (coin.out.scriptPubKey != accumScript) {
                        return state.DoS(100, error("%s: PTXCOALESCE input is not LOTTERY_ACCUM_SCRIPT", __func__),
                                         REJECT_INVALID, "ptxcoalesce-non-accum-input");
                    }
                }
            }

            // C2: exactly one output.
            if (tx.vout.size() != 1) {
                return state.DoS(100, error("%s: PTXCOALESCE must have exactly 1 output, got %d",
                                            __func__, tx.vout.size()),
                                 REJECT_INVALID, "ptxcoalesce-bad-output-count");
            }

            // C3: output scriptPubKey is LOTTERY_ACCUM_SCRIPT.
            if (tx.vout[0].scriptPubKey != accumScript) {
                return state.DoS(100, error("%s: PTXCOALESCE output script is not LOTTERY_ACCUM_SCRIPT", __func__),
                                 REJECT_INVALID, "ptxcoalesce-bad-output-script");
            }

            // C4: output value equals sum of input values (zero miner fee).
            // When all inputs are spent (post-UpdateCoins path in ConnectBlock), this check
            // is skipped here and enforced instead by the block-level structural check in
            // ProcessSpecialTxsInBlock (Step 7), which derives the expected value from
            // LotteryState and the PTXSESS fee set.
            if (view) {
                CAmount inputSum = 0;
                bool anyUnspent = false;
                for (const CTxIn& txin : tx.vin) {
                    const Coin& coin = view->AccessCoin(txin.prevout);
                    if (!coin.IsSpent()) {
                        anyUnspent = true;
                        inputSum += coin.out.nValue;
                    }
                }
                if (anyUnspent && tx.vout[0].nValue != inputSum) {
                    return state.DoS(100, error("%s: PTXCOALESCE output value %d != input sum %d",
                                                __func__, tx.vout[0].nValue, inputSum),
                                     REJECT_INVALID, "ptxcoalesce-value-mismatch");
                }
            }

            // C5: extraPayload must be present-but-empty (not nullopt, not non-empty).
            if (tx.extraPayload == nullopt || !tx.extraPayload->empty()) {
                return state.DoS(100, error("%s: PTXCOALESCE extraPayload must be present-but-empty", __func__),
                                 REJECT_INVALID, "ptxcoalesce-bad-payload");
            }

            // C6: all scriptSigs must be empty.
            for (unsigned int i = 0; i < tx.vin.size(); i++) {
                if (!tx.vin[i].scriptSig.empty()) {
                    return state.DoS(100, error("%s: PTXCOALESCE input %d has non-empty scriptSig", __func__, i),
                                     REJECT_INVALID, "ptxcoalesce-nonempty-scriptsig");
                }
            }

            return true;
        }
        case CTransaction::TxType::PTXPAYOUT: {
            // ODC-022 §3.5 — per-tx rules P1, P3, P4-structural, P6, P7.
            // P2 (input matches accumulator), P5 (output value), P8 (at-most-one),
            // P9 (boundary height), and P10 (winner) are block-scoped; checked in
            // CheckPTXPayoutBlockRules / CheckAndApplyPTXPayout.

            // P1: exactly one input.
            if (tx.vin.size() != 1) {
                return state.DoS(100, error("%s: PTXPAYOUT must have exactly 1 input, got %d",
                                            __func__, (int)tx.vin.size()),
                                 REJECT_INVALID, "ptxpayout-bad-input-count");
            }

            // P3: exactly one output.
            if (tx.vout.size() != 1) {
                return state.DoS(100, error("%s: PTXPAYOUT must have exactly 1 output, got %d",
                                            __func__, (int)tx.vout.size()),
                                 REJECT_INVALID, "ptxpayout-bad-output-count");
            }

            // P4 (structural): output must not loop back to the accumulator.
            if (tx.vout[0].scriptPubKey == GetLotteryAccumScript()) {
                return state.DoS(100, error("%s: PTXPAYOUT output must not be LOTTERY_ACCUM_SCRIPT", __func__),
                                 REJECT_INVALID, "ptxpayout-output-is-accum");
            }

            // P6: extraPayload must be present-but-empty (not nullopt, not non-empty).
            if (tx.extraPayload == nullopt || !tx.extraPayload->empty()) {
                return state.DoS(100, error("%s: PTXPAYOUT extraPayload must be present-but-empty", __func__),
                                 REJECT_INVALID, "ptxpayout-bad-payload");
            }

            // P7: input scriptSig must be empty.
            if (!tx.vin[0].scriptSig.empty()) {
                return state.DoS(100, error("%s: PTXPAYOUT input has non-empty scriptSig", __func__),
                                 REJECT_INVALID, "ptxpayout-nonempty-scriptsig");
            }

            return true;
        }
    }

    return state.DoS(10, error("%s: special tx %s with invalid type %d", __func__, tx.GetHash().ToString(), tx.nType),
                     REJECT_INVALID, "bad-tx-type");
}

bool CheckSpecialTxNoContext(const CTransaction& tx, CValidationState& state)
{
    return CheckSpecialTx(tx, nullptr, nullptr, state);
}

bool CheckPTXCoalesceBlockRules(const CBlock& block, CValidationState& state)
{
    AssertLockHeld(cs_main);
    int ptxSessCount  = 0;
    int coalesceCount = 0;
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsProbabilisticTx()) ++ptxSessCount;
        if (tx->IsPTXCoalesceTx())   ++coalesceCount;
    }
    // C7: at most one PTXCOALESCE per block.
    if (coalesceCount > 1) {
        return state.DoS(100, error("%s: block contains %d PTXCOALESCE txs, max 1", __func__, coalesceCount),
                         REJECT_INVALID, "ptxcoalesce-duplicate");
    }
    // C8: PTXCOALESCE mandatory iff PTXSESS present.
    if (ptxSessCount > 0 && coalesceCount == 0) {
        return state.DoS(100, error("%s: block has %d PTXSESS but no PTXCOALESCE", __func__, ptxSessCount),
                         REJECT_INVALID, "ptxcoalesce-missing");
    }
    if (ptxSessCount == 0 && coalesceCount > 0) {
        return state.DoS(100, error("%s: block has PTXCOALESCE but no PTXSESS", __func__),
                         REJECT_INVALID, "ptxcoalesce-unexpected");
    }
    return true;
}

bool CheckPTXPayoutBlockRules(const CBlock& block, const CBlockIndex* pindex, CValidationState& state)
{
    AssertLockHeld(cs_main);
    int payoutCount = 0;
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsPTXPayoutTx()) ++payoutCount;
    }
    // P8: at most one PTXPAYOUT per block.
    if (payoutCount > 1) {
        return state.DoS(100, error("%s: block contains %d PTXPAYOUT txs, max 1", __func__, payoutCount),
                         REJECT_INVALID, "ptxpayout-duplicate");
    }
    // P9: PTXPAYOUT only at settlement boundary heights.
    if (payoutCount > 0 && pindex != nullptr) {
        const int window = Params().PTXSettlementWindow();
        if (window > 0 && pindex->nHeight % window != 0) {
            return state.DoS(100, error("%s: PTXPAYOUT at non-boundary height %d (window=%d)",
                                        __func__, pindex->nHeight, window),
                             REJECT_INVALID, "ptxpayout-wrong-height");
        }
    }
    return true;
}

bool CheckAndApplyPTXPayout(const CBlock& block,
                             const CBlockIndex* pindex,
                             const CDeterministicGMList& gmList,
                             const PTXPoSeTracker& poseTracker,
                             CValidationState& state,
                             bool fJustCheck)
{
    AssertLockHeld(cs_main);

    const CTransaction* payoutTx = nullptr;
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsPTXPayoutTx()) {
            payoutTx = tx.get();
            break;
        }
    }

    if (payoutTx == nullptr) {
        // P11: at settlement boundaries, reject if accumulator exists and eligible winner found.
        // §5.4 rollover (no eligible GMs) is the only legitimate reason to omit PTXPAYOUT.
        if (pindex != nullptr && pindex->pprev != nullptr &&
            pindex->nHeight % Params().PTXSettlementWindow() == 0) {
            const LotteryState& ls = GetLotteryState();
            if (!ls.accumulator_outpoint.IsNull() &&
                ls.accumulator_value >= Params().PTXPayoutMinerFee()) {
                const uint256 entropy = PTX_ComputeSelectionEntropy(
                    pindex->nHeight, pindex->pprev->GetBlockHash());
                if (PTX_SelectWinner(gmList, poseTracker, entropy)) {
                    return state.DoS(100,
                        error("%s: settlement-boundary block h=%d has eligible winner but no PTXPAYOUT",
                              __func__, pindex->nHeight),
                        REJECT_INVALID, "ptxpayout-missing-at-boundary");
                }
            }
        }
        // No PTXPAYOUT (legitimate): write unchanged snapshot so DisconnectBlock finds a pprev snapshot.
        if (!fJustCheck && pindex != nullptr) {
            WriteLotteryStateSnapshotForBlock(pindex->GetBlockHash(), GetLotteryState());
        }
        return true;
    }

    const LotteryState& ls = GetLotteryState();

    // P2: input must spend the current accumulator UTXO.
    if (payoutTx->vin[0].prevout != ls.accumulator_outpoint) {
        return state.DoS(100, error("%s: PTXPAYOUT input %s:%u != accumulator %s:%u",
                                    __func__,
                                    payoutTx->vin[0].prevout.hash.GetHex(),
                                    payoutTx->vin[0].prevout.n,
                                    ls.accumulator_outpoint.hash.GetHex(),
                                    ls.accumulator_outpoint.n),
                         REJECT_INVALID, "ptxpayout-wrong-input");
    }

    // P5: output value must equal accumulator_value - nPTXPayoutMinerFee.
    const CAmount minerFee    = Params().PTXPayoutMinerFee();
    const CAmount expectedOut = ls.accumulator_value - minerFee;
    if (payoutTx->vout[0].nValue != expectedOut) {
        return state.DoS(100, error("%s: PTXPAYOUT output value %lld != expected %lld (accum=%lld fee=%lld)",
                                    __func__,
                                    (long long)payoutTx->vout[0].nValue,
                                    (long long)expectedOut,
                                    (long long)ls.accumulator_value,
                                    (long long)minerFee),
                         REJECT_INVALID, "ptxpayout-wrong-output-value");
    }

    // P10: output script must match the deterministically-selected winner (§5.3).
    // Entropy uses the PARENT block hash (pindex->pprev), not pindex->GetBlockHash().
    // The current block hash includes hashMerkleRoot which commits PTXPAYOUT — circular
    // for the generator.  Parent hash is fully determined before the block is assembled.
    if (pindex != nullptr && pindex->pprev != nullptr) {
        const uint256 entropy  = PTX_ComputeSelectionEntropy(pindex->nHeight, pindex->pprev->GetBlockHash());
        const Optional<CScript> winner = PTX_SelectWinner(gmList, poseTracker, entropy);
        if (!winner) {
            return state.DoS(100, error("%s: PTXPAYOUT present but no eligible GMs for selection", __func__),
                             REJECT_INVALID, "ptxpayout-no-eligible-winner");
        }
        if (payoutTx->vout[0].scriptPubKey != *winner) {
            return state.DoS(100, error("%s: PTXPAYOUT output script does not match expected winner", __func__),
                             REJECT_INVALID, "ptxpayout-wrong-recipient");
        }
    }

    if (!fJustCheck && pindex != nullptr) {
        LotteryState& mls = GetLotteryState();
        mls.last_settle.height        = pindex->nHeight;
        mls.last_settle.winner_script = payoutTx->vout[0].scriptPubKey;
        mls.last_settle.amount        = payoutTx->vout[0].nValue;
        mls.last_settle.payout_txid   = payoutTx->GetHash();
        // Resolve winner_protx by scanning gmList for the DGM whose scriptPTXPayment
        // matches the payout output.  O(N_GMs) — negligible at settlement frequency.
        mls.last_settle.winner_protx  = uint256();
        gmList.ForEachGM(false, [&](const CDeterministicGMCPtr& dgm) {
            if (dgm->pdgmState->scriptPTXPayment == mls.last_settle.winner_script) {
                mls.last_settle.winner_protx = dgm->proTxHash;
            }
        });
        // Push to settlement_history ring buffer; trim to cap.
        mls.settlement_history.push_back(mls.last_settle);
        if (mls.settlement_history.size() > kSettlementHistoryDepth) {
            mls.settlement_history.erase(mls.settlement_history.begin());
        }
        // Reset accumulator — payout UTXO goes to the winner, not back into the pool.
        mls.accumulator_outpoint.SetNull();
        mls.accumulator_value = 0;
        WriteLotteryStateSnapshotForBlock(pindex->GetBlockHash(), mls);
    }

    return true;
}

bool ProcessSpecialTxsInBlock(const CBlock& block, const CBlockIndex* pindex, const CCoinsViewCache* view, CValidationState& state, bool fJustCheck)
{
    AssertLockHeld(cs_main);

    // check special txes
    for (const CTransactionRef& tx: block.vtx) {
        if (!CheckSpecialTx(*tx, pindex->pprev, view, state)) {
            // pass the state returned by the function above
            return false;
        }
    }

    if (!CheckPTXCoalesceBlockRules(block, state)) {
        return false;
    }

    // PTXCOALESCE must be applied first so that LotteryState.accumulator_outpoint reflects
    // the post-coalesce value when PTXPAYOUT P2 runs (§3.5 validation ordering).
    if (!CheckAndApplyPTXCoalesce(block, pindex, state, fJustCheck)) {
        return false;
    }

    if (!CheckPTXPayoutBlockRules(block, pindex, state)) {
        return false;
    }

    // Fetch DGM list and pose tracker once for both P10 and the state update.
    CDeterministicGMList gmList;
    if (pindex->pprev != nullptr) {
        gmList = deterministicGMManager->GetListForBlock(pindex->pprev);
    }
    if (!CheckAndApplyPTXPayout(block, pindex, gmList, g_ptx_pose_tracker, state, fJustCheck)) {
        return false;
    }

    if (!llmq::quorumBlockProcessor->ProcessBlock(block, pindex, state, fJustCheck)) {
        // pass the state returned by the function above
        return false;
    }

    if (!deterministicGMManager->ProcessBlock(block, pindex, state, fJustCheck)) {
        // pass the state returned by the function above
        return false;
    }

    return true;
}

bool CheckAndApplyPTXCoalesce(const CBlock& block,
                              const CBlockIndex* pindex,
                              CValidationState& state,
                              bool fJustCheck)
{
    AssertLockHeld(cs_main);

    // Find the PTXCOALESCE (if present — C7/C8 guarantee at most one).
    const CTransaction* coalesceTx = nullptr;
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsPTXCoalesceTx()) {
            coalesceTx = tx.get();
            break;
        }
    }

    if (coalesceTx != nullptr) {
        const LotteryState& ls = GetLotteryState();

        // Build the expected input list from LotteryState + vtx scan.
        // Relies on Step 5 invariant: every PTXSESS carries exactly nPTXServiceFee to
        // LOTTERY_ACCUM_SCRIPT (specialtx_validation.cpp, ptx-bad-accum-output rule).
        // If that rule changes, this arithmetic must be revisited.
        std::vector<COutPoint> expectedVin;
        CAmount expectedValue = ls.accumulator_value;

        if (ls.HasAccumulator()) {
            expectedVin.push_back(ls.accumulator_outpoint);
        }

        std::vector<AccumInput> ptxsessFees = PTX_CollectPTXSESSFeeOutputs(block.vtx);
        for (const AccumInput& inp : ptxsessFees) {
            expectedVin.push_back(inp.outpoint);
            expectedValue += inp.value;
        }

        if (coalesceTx->vin.size() != expectedVin.size()) {
            return state.DoS(100, error("%s: PTXCOALESCE has %d inputs, expected %d",
                                        __func__, (int)coalesceTx->vin.size(), (int)expectedVin.size()),
                             REJECT_INVALID, "ptxcoalesce-wrong-input-count");
        }

        for (size_t i = 0; i < expectedVin.size(); ++i) {
            if (coalesceTx->vin[i].prevout != expectedVin[i]) {
                return state.DoS(100, error("%s: PTXCOALESCE vin[%d] wrong outpoint", __func__, (int)i),
                                 REJECT_INVALID, "ptxcoalesce-wrong-input");
            }
        }

        if (coalesceTx->vout[0].nValue != expectedValue) {
            return state.DoS(100, error("%s: PTXCOALESCE output value %lld != expected %lld",
                                        __func__, (long long)coalesceTx->vout[0].nValue,
                                        (long long)expectedValue),
                             REJECT_INVALID, "ptxcoalesce-wrong-output-value");
        }

        if (!fJustCheck) {
            LotteryState& mls = GetLotteryState();
            mls.accumulator_outpoint = COutPoint(coalesceTx->GetHash(), 0);
            mls.accumulator_value    = expectedValue;
            // Semantic count: ptxsessFees is the canonical list from PTX_CollectPTXSESSFeeOutputs,
            // not inferred from vin shape (robust to future PTXCOALESCE schema changes).
            mls.total_rolls += ptxsessFees.size();
            WriteLotteryStateSnapshotForBlock(pindex->GetBlockHash(), mls);
        }
    } else {
        // No PTXCOALESCE: LotteryState unchanged.  Still write the post-block snapshot
        // so DisconnectBlock can always find a valid pprev snapshot.
        if (!fJustCheck) {
            WriteLotteryStateSnapshotForBlock(pindex->GetBlockHash(), GetLotteryState());
        }
    }

    return true;
}

bool UndoSpecialTxsInBlock(const CBlock& block, const CBlockIndex* pindex)
{
    // Restore LotteryState to the pre-block state (= post-pprev snapshot).
    // Called from DisconnectBlock which holds cs_main; AssertLockHeld documents this.
    {
        AssertLockHeld(cs_main);
        // genesis is never disconnected — null pprev here is a programmer error
        assert(pindex->pprev != nullptr);
        LotteryState prevState;
        if (!ReadLotteryStateSnapshotForBlock(pindex->pprev->GetBlockHash(), prevState)) {
            // Missing snapshot indicates evodb integrity failure (lost write, corruption).
            // Refuse the disconnect rather than silently restoring a default-constructed state.
            LogPrintf("%s: missing LotteryState snapshot for %s — evodb integrity failure\n",
                      __func__, pindex->pprev->GetBlockHash().ToString());
            return false;
        }
        GetLotteryState() = prevState;
    }

    if (!deterministicGMManager->UndoBlock(block, pindex)) {
        return false;
    }
    if (!llmq::quorumBlockProcessor->UndoBlock(block, pindex)) {
        return false;
    }
    return true;
}

uint256 CalcTxInputsHash(const CTransaction& tx)
{
    CHashWriter hw(CLIENT_VERSION, SER_GETHASH);
    // transparent inputs
    for (const CTxIn& in: tx.vin) {
        hw << in.prevout;
    }
    // shield inputs
    if (tx.hasSaplingData()) {
        for (const SpendDescription& sd: tx.sapData->vShieldedSpend) {
            hw << sd.nullifier;
        }
    }
    return hw.GetHash();
}
