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
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_dkg_commitments.h"
#include "ptx/ptx_formation.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_bls.h"            // W2.4 W4-b: PTX_BLS_Verify in consensus
#include "ptx/ptx_quorum_store.h"
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
    if (!Params().IsRegTestNet() && !Params().IsPTXBeaTestNet() && !addr.IsRoutable()) {
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

    // ODC-066: accept IPv4 OR IPv6 (routability already enforced above).  Tor is
    // deliberately still excluded — an onion-v3 address needs BIP155/addrv2 wire
    // support, which the flat CService serialization here does not carry, so it is
    // a separate (wire-format) change, not this family widen.  This check is
    // reached only post-Evo: all special txs are rejected below UPGRADE_V6_0 by
    // the bad-txns-v6-not-active gate (CheckSpecialTx), so the v6 activation gate
    // is that pre-existing height gate — no new constant here.
    if (!addr.IsIPv4() && !addr.IsIPv6()) {
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

// ---------------------------------------------------------------------------
// CheckPTXDKGTx — W1.2 structural validation (§8)
// ---------------------------------------------------------------------------

bool CheckPTXDKGTx(const CTransaction& tx, const CBlockIndex* pindexPrev,
                   CValidationState& state)
{
    AssertLockHeld(cs_main);

    PTXDKGPayload payload;
    if (!GetTxPayload(tx, payload))
        return state.DoS(100, error("%s: PTXDKG payload failed to deserialize", __func__),
                         REJECT_INVALID, "ptxdkg-bad-payload");

    // KDD-072 P-a: payload version gate. FIRST substantive check — it runs on
    // BOTH the structural/null-pindexPrev (CheckBlock) path and the contextual
    // path, before the V1-V8 bifurcation below, so every node gates version
    // identically regardless of context. Mirrors the ProReg/CFinalCommitment
    // pattern (this file :198/:342/:539). nVersion==0 or > CURRENT is invalid.
    // KDD-072 P-b3b — THE FLIP, one token: the bound is ROTATION_VERSION (v1
    // and v2 both accepted, nothing above). CURRENT_VERSION stays 1 — it is
    // the FRESH-EMISSION version (BuildPTXDKGTx: fresh->1, rotation->2); a
    // literal CURRENT_VERSION bump would make every fresh formation emit
    // v2-with-zero-predecessor and die on the reject below (the breaker the
    // P-b3b recon caught; RED-pinned by Pb3b_FreshStillEmitsV1).
    if (payload.nVersion == 0 || payload.nVersion > PTXDKGPayload::ROTATION_VERSION)
        return state.DoS(100, error("%s: PTXDKG payload version %d invalid (max %d)", __func__,
                                    (int)payload.nVersion, (int)PTXDKGPayload::ROTATION_VERSION),
                         REJECT_INVALID, "bad-ptxdkg-version");

    // KDD-072 P-b3b — v2-without-predecessor: STRUCTURAL depth (fires on the
    // CheckBlock/null-pindexPrev path AND ahead of the contextual section —
    // the P-a version-gate pattern). BuildPTXDKGTx never emits this shape
    // (v2 <=> non-zero predecessor); a payload carrying it is malformed and
    // must never reach the fresh V4/V5 draw.
    if (payload.nVersion >= PTXDKGPayload::ROTATION_VERSION &&
        payload.predecessor_quorum_hash.IsNull())
        return state.DoS(100, error("%s: v2 PTXDKG without a predecessor", __func__),
                         REJECT_INVALID, "ptxdkg-v2-without-predecessor");

    // group_pk_bytes must decompress to a valid G1 point.
    blst_p1_affine group_pk;
    if (blst_p1_uncompress(&group_pk, payload.group_pk_bytes) != BLST_SUCCESS)
        return state.DoS(100, error("%s: PTXDKG group_pk_bytes failed to decompress", __func__),
                         REJECT_INVALID, "ptxdkg-bad-grouppk");

    // Member list non-empty and count ≤ 11.
    if (payload.member_node_ids.empty() || payload.member_node_ids.size() > 11)
        return state.DoS(100, error("%s: PTXDKG member count invalid (%d)", __func__,
                                    (int)payload.member_node_ids.size()),
                         REJECT_INVALID, "ptxdkg-bad-member-count");

    // ≥ t=6 premature commitments required.
    const int t = 6;
    if ((int)payload.premit_commitments.size() < t)
        return state.DoS(100, error("%s: PTXDKG premit count %d < t=%d", __func__,
                                    (int)payload.premit_commitments.size(), t),
                         REJECT_INVALID, "ptxdkg-insufficient-premits");

    // Sig fields present (non-null check only — structural; contextual sig
    // verification is below, only when chain context is available).
    for (const auto& kv : payload.premit_commitments) {
        if (!kv.second.sig.IsValid())
            return state.DoS(100, error("%s: PTXDKG premit from %s has null/invalid sig",
                                        __func__, kv.first.ToString()),
                             REJECT_INVALID, "ptxdkg-null-sig");
    }

    // ---- Contextual attestation checks (KDD-059/060; V1–V8) ----
    // Only runnable with chain context (block-connect / mempool).  The null path
    // (pindexPrev == nullptr; CheckBlock) stays structural-only above.
    // ACCOUNTABILITY, not correctness: this proves >= t members of the
    // canonically-selected quorum each signed agreement on (group_pk, vvec_hash)
    // with their DGM-registered operator key.  It does NOT prove group_pk is the
    // correct DKG output.  All rejects DoS 100, mirroring CheckLLMQCommitmentTx.
    if (pindexPrev != nullptr) {
        // V1: the formation anchor block must exist.
        const CBlockIndex* pindexQuorum = LookupBlockIndex(payload.quorum_hash);
        if (pindexQuorum == nullptr)
            return state.DoS(100, error("%s: PTXDKG quorum_hash %s not found", __func__,
                                        payload.quorum_hash.ToString()),
                             REJECT_INVALID, "ptxdkg-quorum-hash-not-found");

        // V2: payload height must match the anchor (the hash is the real anchor;
        // height is a redundant cross-check).
        if (pindexQuorum->nHeight != payload.formation_height)
            return state.DoS(100, error("%s: PTXDKG formation_height %d != anchor height %d",
                                        __func__, payload.formation_height, pindexQuorum->nHeight),
                             REJECT_INVALID, "ptxdkg-formation-height-mismatch");

        // V3: the anchor must be on the chain being validated (reorg safety;
        // anchored to pindexPrev's chain, NOT chainActive.Tip).
        if (pindexQuorum != pindexPrev->GetAncestor(pindexQuorum->nHeight))
            return state.DoS(100, error("%s: PTXDKG quorum_hash %s not on active chain", __func__,
                                        payload.quorum_hash.ToString()),
                             REJECT_INVALID, "ptxdkg-quorum-hash-not-active-chain");

        // V11 (W2.2 SG-1b-iii): the anchor must sit ON the formation
        // schedule — height % N == 0 via PTX_Formation_IsBoundary (genesis
        // excluded). SG-1b defines WHEN formation may fire; validation must
        // REQUIRE it or produce and validate diverge (the SG-1a asymmetry
        // class). Shape mirrors the LLMQ commitment's dkgInterval check
        // (CheckLLMQCommitmentTx above). Deliberately NO activation-height
        // gate — a RESETTABLE-FLEET simplification only; mainnet/public
        // testnet REQUIRE a height-gate before this ships there or historical
        // off-boundary PTXDKGs become retroactively invalid (forward-bind
        // recorded 2026-07-13, pre-testnet/mainnet-migration owed-list).
        if (!PTX_Formation_IsBoundary(pindexQuorum->nHeight,
                                      Params().GetConsensus().ptxFormation))
            return state.DoS(100, error("%s: PTXDKG anchor %s (height %d) is not a formation boundary",
                                        __func__, payload.quorum_hash.ToString(), pindexQuorum->nHeight),
                             REJECT_INVALID, "ptxdkg-anchor-not-boundary");

        // V9 (W2.1 C4, ODC-030 clause 2): one accepted PTXDKG per formation —
        // cross-block uniqueness against the persisted quorum index.  evodb
        // tracks the connecting chain through the scoped transaction stack, so
        // the answer is reorg-consistent for populate (tip), assembler
        // (pindexPrev) and block-connect alike.  Validation-surface twin of
        // the persist-boundary guard in CPTXQuorumStore::ProcessBlock — this
        // is the reject with observability (populate refusal / generate-time
        // keep-but-skip); the store guard is defense-in-depth.
        if (ptxQuorumStore && ptxQuorumStore->HasQuorumRecord(payload.quorum_hash))
            return state.DoS(100, error("%s: PTXDKG for formation %s already accepted", __func__,
                                        payload.quorum_hash.ToString()),
                             REJECT_INVALID, "ptxdkg-duplicate-formation");

        // ---- V12 vs V4+V5: the KDD-072 §5 SUBSTITUTION branch (P-b3a) ----
        // A rotation names its predecessor; its quorum11 IS the predecessor's
        // recorded 11 (same-set re-DKG, KDD-045/063) — no fresh draw. A fresh
        // formation runs V4+V5 unchanged. V10 and V6-V8 below consume the
        // substituted quorum11 verbatim (no rotation branch in them).
        std::vector<CDeterministicGMCPtr> quorum11;
        if (payload.nVersion >= PTXDKGPayload::ROTATION_VERSION) {
            // Defense-in-depth twin of the structural reject above: a v2
            // payload NEVER reaches the fresh V4/V5 draw. Unreachable through
            // the public sequence (the structural depth fires first);
            // inspection-only guard against a future structural refactor.
            if (payload.predecessor_quorum_hash.IsNull())
                return state.DoS(100, error("%s: v2 PTXDKG without a predecessor (contextual)", __func__),
                                 REJECT_INVALID, "ptxdkg-v2-without-predecessor");
            // V12a: the predecessor record must exist on this chain's store.
            CPTXQuorumRecord predRec;
            if (ptxQuorumStore == nullptr ||
                !ptxQuorumStore->GetQuorumRecord(payload.predecessor_quorum_hash, predRec))
                return state.DoS(100, error("%s: rotation predecessor %s unknown", __func__,
                                            payload.predecessor_quorum_hash.ToString()),
                                 REJECT_INVALID, "ptxdkg-rotation-predecessor-unknown");
            // The predecessor's formation anchor supplies the formation-time
            // DGM list for the key-agreement check (ResolveRotationQuorum).
            const CBlockIndex* pindexPred = LookupBlockIndex(predRec.quorum_hash);
            if (pindexPred == nullptr)
                return state.DoS(100, error("%s: rotation predecessor anchor %s unknown", __func__,
                                            predRec.quorum_hash.ToString()),
                                 REJECT_INVALID, "ptxdkg-rotation-predecessor-anchor-unknown");
            const CDeterministicGMList listRot =
                deterministicGMManager->GetListForBlock(pindexQuorum);
            const CDeterministicGMList listForm =
                deterministicGMManager->GetListForBlock(pindexPred);
            // V12b (ACTIVE as-of pindexPrev, the P-b4 predicate) + V12c
            // (same-set resolve, reject-not-exclude policy) — THE shared core;
            // the store connect guard runs this exact function (KDD-073).
            if (!PTX_DKG_CheckRotationAndResolve(predRec, pindexPrev->nHeight,
                                                 listRot, listForm, quorum11, state))
                return false;
            // V12d (KDD-072 P-b5): predecessor-uniqueness — at most one
            // successor per predecessor, on the EXPLICIT pq_p index. ★ The
            // index is the PRIMARY durable guard; a raced second rotation also
            // fails V12b's as-of-ACTIVE above, but that rejection rides
            // state-read semantics a refactor could change — pq_p cannot
            // drift. Distinct reject from "not-active".
            if (!ptxQuorumStore->CheckPredecessorUnrotated(
                    payload.predecessor_quorum_hash, state))
                return false;
        } else {
        // V4: snapshot the GM list at the formation block.  A missing snapshot
        // post-activation is local DB corruption — GetListForBlock throws and the
        // throw PROPAGATES (no try/catch; converting it to a reject would be wrong
        // in both directions).
        CDeterministicGMList dgmList = deterministicGMManager->GetListForBlock(pindexQuorum);

        // V5: reconstruct the canonical quorum through the shared selection core
        // (eligibility filter then CalculateQuorum) — never bare CalculateQuorum
        // on the unfiltered dgmList, which would score empty-node_id GMs that
        // formation excludes and split the chain.
        // SG-1a (2026-07-12): the KDD-040 pool join is CONSENSUS-SHARED — the
        // validator builds the SAME eligible-minus-active pool as formation
        // (PTX_Formation_BuildPool, D-SG1a-1 full formed-11 exclusion) or an
        // honest second formation would self-reject the moment quorum #2
        // exists. At zero ACTIVE records the pool equals the eligible list
        // byte-identically (proven at the SG-1a gate). D-SG1a-2 DISCHARGED at
        // P-b4: GetActiveQuorumsAtHeight answers state-AS-OF-HEIGHT through
        // PTX_QuorumRecordActiveAt. Null store = unit-test env only.
        std::vector<CPTXQuorumRecord> activeAtAnchor;
        if (ptxQuorumStore) {
            activeAtAnchor =
                ptxQuorumStore->GetActiveQuorumsAtHeight(pindexQuorum->nHeight);
        }
        const CDeterministicGMList formationPool =
            PTX_Formation_BuildPool(dgmList, activeAtAnchor);
        quorum11 =
            PTX_DKG_SelectQuorumFromList(formationPool, payload.quorum_hash);
        if (quorum11.size() != 11)
            return state.DoS(100, error("%s: PTXDKG quorum underfull (%d of 11) at anchor",
                                        __func__, (int)quorum11.size()),
                             REJECT_INVALID, "ptxdkg-quorum-underfull");
        }

        // V10 (W2.1 C4): committed member containment — every member_node_ids
        // entry must hold a rank in the canonical selection, no duplicates.
        // Load-bearing for KDD-061: a committed member outside the selection
        // has NO derivable share_index, making recovery unreconstructable from
        // committed data.  (V1-V8 only count-check member_node_ids; premit
        // checks V7* cover committers, not the committed member list.)
        {
            std::set<std::string> selected;
            for (const auto& dgm : quorum11) selected.insert(dgm->pdgmState->node_id);
            std::set<std::string> seen;
            for (const std::string& nid : payload.member_node_ids) {
                if (!selected.count(nid) || !seen.insert(nid).second)
                    return state.DoS(100, error("%s: PTXDKG committed member '%s' not in the "
                                                "canonical selection (or duplicated)", __func__, nid),
                                     REJECT_INVALID, "ptxdkg-member-not-in-quorum");
            }
        }

        // V6–V8: per-premit operator-key signature agreement against the quorum.
        if (!PTX_DKG_VerifyPremits(quorum11, payload, state))
            return false;
    }

    return true;
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

// BUG-032 (Option A, fund-then-sign): validate a roll COMMITMENT (nType=12).
// The commitment is sig-less and results-less by construction — that is the
// point: it must be mempool-valid BEFORE the quorum signs, so payment is bound
// to the round before any result is revealed. Its validity establishes:
//   - structural sanity of the round (seed present, params in order, height set);
//   - the same-block mandate: nExpiryHeight == nSeedHeight (window 0) — settle
//     must ride the commitment's block, collapsing reorg exposure to today's
//     single-tx level (a window > 0 would be a deliberate future exception, not
//     the default);
//   - a fee output to LOTTERY_ACCUM_SCRIPT at the service fee (the payment);
//   - (contextual) quorum_hash is the CANONICAL quorum — ACTIVE at nSeedHeight —
//     which is what closes the quorum-shop (BUG-033): the settle's sig will be
//     required to verify against this exact quorum, chosen before it signs.
// No signature is checked here (there is none). Structural path (pindexPrev ==
// nullptr, CheckBlock) skips the contextual canonical-quorum check, mirroring
// the PTXSESS posture (the store is chain state).
static bool CheckPTXRollCommitTx(const CTransaction& tx, const CBlockIndex* pindexPrev,
                                 const CCoinsViewCache* view, CValidationState& state)
{
    CPTXRollCommitPayload payload;
    if (!GetTxPayload(tx, payload))
        return state.Invalid(false, REJECT_INVALID, "ptxcommit-bad-payload");
    if (payload.round_seed.IsNull())
        return state.Invalid(false, REJECT_INVALID, "ptxcommit-missing-seed");
    if (payload.low > payload.high)
        return state.Invalid(false, REJECT_INVALID, "ptxcommit-bad-range");
    if (payload.count == 0)
        return state.Invalid(false, REJECT_INVALID, "ptxcommit-zero-count");
    if (payload.nSeedHeight == 0)
        return state.Invalid(false, REJECT_INVALID, "ptxcommit-bad-height");
    // ★ BUG-034 RELAX (no-bound): the same-block mandate (nExpiryHeight ==
    // nSeedHeight, window 0) is retired. Reorg-safety never came from the
    // window — it comes from the coin-chain (the settle spends the
    // commitment's output; drop the commitment and the settle's input — and
    // the fee — vanish with it, unwinding the roll consistently). Window 0
    // made every roll whose commitment mined alone in its own signing window
    // (fund-then-sign broadcasts the commitment before the settle can exist)
    // silently orphan a SUCCESSFUL roll's result, and the unbound settle then
    // poisoned block assembly (the h5065 halt). No upper bound is enforced:
    // no candidate mechanism is consensus-real (see the pairing-rule header
    // note). Only the structural floor remains.
    if (payload.nExpiryHeight < payload.nSeedHeight)
        return state.Invalid(false, REJECT_INVALID, "ptxcommit-window-negative");
    // The payment: exactly one output to LOTTERY_ACCUM_SCRIPT at the service fee.
    {
        const CScript& accumScript = GetLotteryAccumScript();
        const CAmount  serviceFee  = Params().PTXServiceFee();
        int accumCount = 0;
        for (const CTxOut& out : tx.vout) {
            if (out.scriptPubKey == accumScript && out.nValue == serviceFee)
                ++accumCount;
        }
        if (accumCount != 1)
            return state.Invalid(false, REJECT_INVALID, "ptxcommit-bad-accum-output");
    }
    // Contextual: the committed quorum must be the canonical one — ACTIVE at
    // nSeedHeight. Chain state, so structural-only path (pindexPrev == nullptr)
    // skips it, as PTXSESS does.
    if (pindexPrev != nullptr) {
        // ★ BUG-033 CANONICAL-QUORUM GATE: the committed quorum must be a real
        // record that is ACTIVE at nSeedHeight. Per-record (GetQuorumRecord +
        // PTX_QuorumRecordActiveAt) is equivalent to membership in
        // GetActiveQuorumsAtHeight(nSeedHeight) for a specific quorum_hash — the
        // predicate already requires mined_height <= nSeedHeight — and needs only
        // the primary record, not the by-inv-height index walk. Closes the
        // quorum-shop at the commitment: the settle's sig will be required to
        // verify against exactly this quorum, chosen before any signature exists.
        CPTXQuorumRecord qrec;
        if (ptxQuorumStore == nullptr ||
            !ptxQuorumStore->GetQuorumRecord(payload.quorum_hash, qrec) ||
            !PTX_QuorumRecordActiveAt(qrec, (int)payload.nSeedHeight)) {
            return state.Invalid(false, REJECT_INVALID, "ptxcommit-noncanonical-quorum");
        }
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
            !tx.IsPTXRollCommitTx() &&
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
        case CTransaction::TxType::PTXROLLCOMMIT: {
            // BUG-032 fund-then-sign commitment (sig-less, results-less).
            return CheckPTXRollCommitTx(tx, pindexPrev, view, state);
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
            // BUG-032 2b-iii FEE RELOCATION: the service fee has moved to the
            // PTXROLLCOMMIT (fund-then-sign — the payment is forfeited at commit,
            // before the result is knowable; only fee-in-commitment closes the
            // free preview, because the commitment is standalone-mineable via the
            // settle→requires→commitment direction of mandatory-iff). The PTXSESS
            // reveal must therefore NOT carry an accum fee output, else the roll
            // pays twice.  The pool is fed identically — the commitment's accum
            // output is swept by the same coalesce (C1) and read by the same
            // payout — so this is a RELOCATION of the one fee, not a model change
            // (ODC-022 §3.3's fee obligation now lives in CheckPTXRollCommitTx).
            {
                const CScript& accumScript = GetLotteryAccumScript();
                const CAmount  serviceFee  = Params().PTXServiceFee();
                for (const CTxOut& out : tx.vout) {
                    if (out.scriptPubKey == accumScript && out.nValue == serviceFee)
                        return state.Invalid(false, REJECT_INVALID, "ptxsess-redundant-fee");
                }
            }
            // W2.4 W4-b (KDD-076 prerequisite) — consensus verification of the
            // threshold signature, keyed by the W4-a attribution field.  Closes
            // the unverified-quorum_sig gap: before this, consensus checked only
            // quorum_sig_hash non-null, so any staker could fabricate sig bytes
            // and stamp an arbitrary quorum_hash (the spoofable-trigger hole).
            // Contextual-only: the store is chain state, so the structural path
            // (CheckSpecialTxNoContext, pindexPrev == nullptr) stays
            // structural-only — the same posture as CheckPTXDKGTx's V-sequence.
            if (pindexPrev != nullptr) {
                CPTXQuorumRecord qrec;
                if (ptxQuorumStore == nullptr ||
                    !ptxQuorumStore->GetQuorumRecord(payload.quorum_hash, qrec)) {
                    // Also covers a null quorum_hash: no record has a null key.
                    return state.Invalid(false, REJECT_INVALID, "ptx-unknown-quorum");
                }
                // A malformed sig (wrong length) is a bad sig, not a distinct
                // class.  The message is the payload's round_seed — byte-for-byte
                // the message the coordinator's own verify used (rpc/ptx.cpp).
                if (payload.quorum_sig.size() != (size_t)PTX_SIG_BYTES ||
                    qrec.group_pk_bytes.size() != 48 ||
                    !PTX_BLS_Verify(qrec.group_pk_bytes.data(), payload.round_seed,
                                    payload.quorum_sig.data())) {
                    return state.Invalid(false, REJECT_INVALID, "ptx-bad-quorum-sig");
                }
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
        case CTransaction::TxType::PTXDKG: {
            return CheckPTXDKGTx(tx, pindexPrev, state);
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
    int coalesceCount = 0;
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsPTXCoalesceTx())   ++coalesceCount;
    }
    // C7: at most one PTXCOALESCE per block.
    if (coalesceCount > 1) {
        return state.DoS(100, error("%s: block contains %d PTXCOALESCE txs, max 1", __func__, coalesceCount),
                         REJECT_INVALID, "ptxcoalesce-duplicate");
    }
    // C8: PTXCOALESCE mandatory-iff a ROLL-FEE SOURCE is present in the block.
    //
    // BUG-032 fee relocation, reverse-direction reconciliation (the LAST site —
    // fleet-audited, no fourth). The fee moved from the PTXSESS (settle) to the
    // PTXROLLCOMMIT, so "fee present" is no longer "PTXSESS present": an ORPHAN
    // COMMIT (roll committed, sign abandoned, settle never produced) carries a
    // real fee output with NO settle in its block. The assembler (blockassembler
    // .cpp:242) and the value/vin derivation (CheckAndApplyPTXCoalesce) already
    // key on PTX_CollectRollFeeOutputs; this rule was the straggler still using
    // ptxSessCount, so it HALTED the chain on the design's own forfeiture path
    // ("PTXCOALESCE but no PTXSESS"). Keying on the roll-fee-output count — the
    // SAME source, producer/validator lockstep — makes the orphan-commit block
    // legal (fee source present) while still rejecting a coalesce with no fee
    // source at all (anti-vacuity: no commit, no settle → nothing to coalesce).
    const size_t feeSourceCount = PTX_CollectRollFeeOutputs(block.vtx).size();
    if (feeSourceCount > 0 && coalesceCount == 0) {
        return state.DoS(100, error("%s: block has %d roll-fee source(s) but no PTXCOALESCE",
                                    __func__, (int)feeSourceCount),
                         REJECT_INVALID, "ptxcoalesce-missing");
    }
    if (feeSourceCount == 0 && coalesceCount > 0) {
        return state.DoS(100, error("%s: block has PTXCOALESCE but no roll-fee source", __func__),
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

// BUG-034: pre-spend resolver for settle parents mined in EARLIER blocks (see
// header). MUST run before the caller's spend loop: the coin's nHeight is the
// only deterministic locator for the parent's block, and SpendCoin erases it.
// Deterministic by construction — pre-block UTXO view + pindex ancestry are
// consensus state; deliberately NOT GetTransaction/pcoinsTip (connect-vs-replay
// divergence, the BUG-029 class). One block read per distinct confirmed parent.
std::map<uint256, CPTXRollCommitPayload> PTX_CollectConfirmedSettleParents(
        const CBlock& block, const CBlockIndex* pindex, const CCoinsViewCache* view)
{
    AssertLockHeld(cs_main);
    std::map<uint256, CPTXRollCommitPayload> ret;
    if (view == nullptr || pindex == nullptr) return ret;
    // Same-block siblings resolve via block.vtx in the pairing rule, not here.
    std::set<uint256> inBlock;
    for (const auto& tx : block.vtx) inBlock.insert(tx->GetHash());
    for (const auto& tx : block.vtx) {
        if (!tx->IsProbabilisticTx()) continue;
        for (const CTxIn& in : tx->vin) {
            const uint256& ph = in.prevout.hash;
            if (inBlock.count(ph) || ret.count(ph)) continue;
            const Coin& coin = view->AccessCoin(in.prevout);
            if (coin.IsSpent()) continue;   // not resolvable => pairing stays fail-closed
            const CBlockIndex* pAnc = pindex->GetAncestor((int)coin.nHeight);
            if (pAnc == nullptr) continue;
            CBlock parentBlock;
            if (!ReadBlockFromDisk(parentBlock, pAnc)) continue;
            for (const auto& ptxParent : parentBlock.vtx) {
                if (ptxParent->GetHash() != ph) continue;
                if (ptxParent->IsPTXRollCommitTx()) {
                    CPTXRollCommitPayload pl;
                    if (GetTxPayload(*ptxParent, pl)) ret.emplace(ph, pl);
                }
                break;   // txid located; commit or not, the search is over
            }
        }
    }
    return ret;
}

// BUG-032 2c: the coin-chained matched-pair rule (see header). Every PTXSESS
// must spend an output of a same-block PTXROLLCOMMIT and reveal the SAME round
// (round_seed) under the SAME quorum (quorum_hash) the commitment fixed.
// BUG-034 P3 — the ONE-FUNCTION per-settle verdict (the h385 assembler=validator
// lesson): the validator's pairing loop and the assembler's template filter both
// call THIS, so filter-fires ⇔ validator-rejects by construction — no divergence
// is representable.
PTXSettleParentVerdict PTX_SettleParentVerdict(const CTransaction& settle,
        const std::map<uint256, CPTXRollCommitPayload>& siblingCommits,
        const std::map<uint256, CPTXRollCommitPayload>& confirmedParents)
{
    CProbabilisticTxPayload s;
    if (!GetTxPayload(settle, s)) return PTXSettleParentVerdict::BAD_PAYLOAD;
    // The coin-chained parent: the settle must SPEND an output of a
    // PTXROLLCOMMIT — a same-block sibling, or (BUG-034 relax) a commitment
    // already CONFIRMED at any depth (pre-resolved by the caller). The
    // matched-pair binding and the reorg-atomicity live in the coin-chain
    // itself: drop the parent, the settle's input ceases to exist.
    const CPTXRollCommitPayload* parent = nullptr;
    for (const CTxIn& in : settle.vin) {
        auto it = siblingCommits.find(in.prevout.hash);
        if (it != siblingCommits.end()) { parent = &it->second; break; }
        auto itc = confirmedParents.find(in.prevout.hash);
        if (itc != confirmedParents.end()) { parent = &itc->second; break; }
    }
    // 2c-i: the coin-chained parent must exist — no commitment, no reveal.
    if (parent == nullptr) return PTXSettleParentVerdict::NO_PARENT;
    // 2c-ii Q2 (BUG-033 settle-side): the reveal must name the SAME canonical
    // quorum the commitment paid for — no quorum-shop at settle.
    if (s.quorum_hash != parent->quorum_hash) return PTXSettleParentVerdict::QUORUM_MISMATCH;
    // The reveal must be of the SAME round the commitment fixed.
    if (s.round_seed != parent->round_seed) return PTXSettleParentVerdict::SEED_MISMATCH;
    return PTXSettleParentVerdict::OK;
}

// Shared: index a tx set's PTXROLLCOMMITs by txid (sibling map for the verdict).
static std::map<uint256, CPTXRollCommitPayload> PTX_IndexSiblingCommits(
        const std::vector<CTransactionRef>& vtx)
{
    std::map<uint256, CPTXRollCommitPayload> commits;
    for (const CTransactionRef& tx : vtx) {
        if (!tx->IsPTXRollCommitTx()) continue;
        CPTXRollCommitPayload p;
        if (GetTxPayload(*tx, p)) commits[tx->GetHash()] = p;
    }
    return commits;
}

bool CheckPTXRollCommitSettlePairing(const CBlock& block,
                                     const std::map<uint256, CPTXRollCommitPayload>& confirmedParents,
                                     CValidationState& state)
{
    // Pure: block + pre-resolved confirmed parents; no chain access (BUG-034).
    const std::map<uint256, CPTXRollCommitPayload> commits = PTX_IndexSiblingCommits(block.vtx);
    for (const CTransactionRef& tx : block.vtx) {
        if (!tx->IsProbabilisticTx()) continue;   // PTXSESS = the settle/reveal
        switch (PTX_SettleParentVerdict(*tx, commits, confirmedParents)) {
        case PTXSettleParentVerdict::OK:
            break;
        case PTXSettleParentVerdict::BAD_PAYLOAD:
            return state.DoS(100, error("%s: PTXSESS payload failed to deserialize", __func__),
                             REJECT_INVALID, "ptxsess-bad-payload");
        case PTXSettleParentVerdict::NO_PARENT:
            return state.DoS(100, error("%s: PTXSESS %s has no coin-chained PTXROLLCOMMIT "
                                        "parent in this block or the confirmed chain "
                                        "(payment-before-reveal unbound)",
                                        __func__, tx->GetHash().ToString()),
                             REJECT_INVALID, "ptxsess-no-commitment-parent");
        case PTXSettleParentVerdict::QUORUM_MISMATCH:
            return state.DoS(100, error("%s: PTXSESS quorum_hash does not match its commitment "
                                        "(quorum-shop at settle)", __func__),
                             REJECT_INVALID, "ptxsess-quorum-mismatch");
        case PTXSettleParentVerdict::SEED_MISMATCH:
            return state.DoS(100, error("%s: PTXSESS round_seed does not match its commitment",
                                        __func__),
                             REJECT_INVALID, "ptxsess-seed-mismatch");
        }
    }
    return true;
}

// BUG-034 P3 — the anti-halt template filter (the E2 assembler-passes-validation
// invariant, h5065 as its repro): identify template settles the validator WOULD
// reject, so the assembler can drop them instead of shipping a template that
// fails TestBlockValidity — one poison tx must never fail every block template.
// Same collector + same verdict as validation (one-function contract). A dropped
// settle is NOT silent: the caller logs it and the settle stays in the mempool
// where the BUG-034 detector's pending-settle alert watches it.
std::vector<std::pair<uint256, PTXSettleParentVerdict>> PTX_TemplateUnpairableSettles(
        const CBlock& block, const CBlockIndex* pindexPrev, const CCoinsViewCache* view)
{
    AssertLockHeld(cs_main);
    std::vector<std::pair<uint256, PTXSettleParentVerdict>> bad;
    const auto confirmed = PTX_CollectConfirmedSettleParents(block, pindexPrev, view);
    const auto siblings  = PTX_IndexSiblingCommits(block.vtx);
    for (const CTransactionRef& tx : block.vtx) {
        if (!tx->IsProbabilisticTx()) continue;
        const PTXSettleParentVerdict v = PTX_SettleParentVerdict(*tx, siblings, confirmed);
        if (v != PTXSettleParentVerdict::OK) bad.emplace_back(tx->GetHash(), v);
    }
    return bad;
}

bool CheckPTXDKGBlockRules(const CBlock& block, CValidationState& state)
{
    AssertLockHeld(cs_main);
    int dkgCount = 0;
    for (const CTransactionRef& tx : block.vtx) {
        if (tx->IsPTXDKGTx()) ++dkgCount;
    }
    // W1.3 spec §4.4 (KDD-058): at most one PTXDKG per block.  Cross-block
    // per-formation uniqueness is ODC-030, resolved with W2 lifecycle.
    if (dkgCount > 1) {
        return state.DoS(100, error("%s: block contains %d PTXDKG txs, max 1", __func__, dkgCount),
                         REJECT_INVALID, "ptxdkg-duplicate");
    }
    return true;
}

bool CheckAndApplyPTXPayout(const CBlock& block,
                             const CBlockIndex* pindex,
                             const CDeterministicGMList& gmList,
                             const PTXPoSeTracker& poseTracker,
                             const COutPoint& effAccumOutpoint,
                             CAmount effAccumValue,
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
            // BUG-024: P11 judges the EFFECTIVE accumulator, not the raw global —
            // a boundary block that coalesces first still owes a payout.
            if (!effAccumOutpoint.IsNull() &&
                effAccumValue >= Params().PTXPayoutMinerFee()) {
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

    // P2: input must spend the EFFECTIVE accumulator UTXO (BUG-024: the
    // post-coalesce one when this block carries a coalesce — under fJustCheck
    // the global has not been advanced, and reading it here made every
    // coalesce+payout block unbuildable by its own producer).
    if (payoutTx->vin[0].prevout != effAccumOutpoint) {
        return state.DoS(100, error("%s: PTXPAYOUT input %s:%u != accumulator %s:%u",
                                    __func__,
                                    payoutTx->vin[0].prevout.hash.GetHex(),
                                    payoutTx->vin[0].prevout.n,
                                    effAccumOutpoint.hash.GetHex(),
                                    effAccumOutpoint.n),
                         REJECT_INVALID, "ptxpayout-wrong-input");
    }

    // P5: output value must equal the effective accumulator value - nPTXPayoutMinerFee.
    const CAmount minerFee    = Params().PTXPayoutMinerFee();
    const CAmount expectedOut = effAccumValue - minerFee;
    if (payoutTx->vout[0].nValue != expectedOut) {
        return state.DoS(100, error("%s: PTXPAYOUT output value %lld != expected %lld (accum=%lld fee=%lld)",
                                    __func__,
                                    (long long)payoutTx->vout[0].nValue,
                                    (long long)expectedOut,
                                    (long long)effAccumValue,
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

bool ProcessSpecialTxsInBlock(const CBlock& block, const CBlockIndex* pindex, const CCoinsViewCache* view,
                              const std::map<uint256, CPTXRollCommitPayload>& confirmedSettleParents,
                              CValidationState& state, bool fJustCheck)
{
    AssertLockHeld(cs_main);

    // BUG-026 (B): a block that FAILS to connect must leave in-memory PTX state
    // exactly as it found it.  LotteryState and the PoSe tracker are bare
    // globals mutated PART-WAY through this function; every `return false`
    // below leaves those mutations behind, because neither the throwaway coins
    // cache nor the evoDb transaction covers them.  Observed live on the h420
    // wedge: a rejected boundary block left its coalesce applied, so the live
    // accumulator read 22.00 against a true on-chain 16.00, and the producer
    // then built 48 consecutive templates spending an outpoint that does not
    // exist (bad-txns-inputs-missingorspent, one per block for 48 minutes)
    // until a restart reloaded clean persisted state.  That is what makes the
    // wedge SELF-PERPETUATING rather than a single bad block.
    //
    // Same invariant BUG-023 established for CVerifyDB's dry run, on the other
    // untransacted path: this one is the real apply, so it commits on success
    // and rolls back only on failure.  Armed only when !fJustCheck — under
    // fJustCheck nothing here mutates.  Destroyed before the caller releases
    // cs_main, so the lock requirement on GetLotteryState() holds in the dtor.
    struct PTXStateFailureSentry {
        bool armed;
        LotteryState savedLottery;
        std::map<std::string, PTXNodeRecord> savedPose;
        explicit PTXStateFailureSentry(bool arm) : armed(arm) {
            if (armed) {
                savedLottery = GetLotteryState();
                savedPose    = g_ptx_pose_tracker.GetAllRecords();
            }
        }
        void commit() { armed = false; }
        ~PTXStateFailureSentry() {
            if (armed) {
                GetLotteryState() = savedLottery;
                g_ptx_pose_tracker.RestoreRecords(std::move(savedPose));
            }
        }
    } ptxStateSentry(!fJustCheck);

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

    // PTXCOALESCE must be checked first so the payout checks see the post-coalesce
    // accumulator (§3.5 validation ordering).  BUG-024: the ordering alone only
    // worked under !fJustCheck (apply mutates the global); the effective-accumulator
    // pass-through below is what makes TestBlockValidity agree with connect.
    COutPoint effAccumOutpoint;
    CAmount effAccumValue{0};
    if (!CheckAndApplyPTXCoalesce(block, pindex, state, fJustCheck, &effAccumOutpoint, &effAccumValue)) {
        return false;
    }

    if (!CheckPTXPayoutBlockRules(block, pindex, state)) {
        return false;
    }

    if (!CheckPTXDKGBlockRules(block, state)) {
        return false;
    }

    // BUG-032 2c: every PTXSESS (reveal) must be coin-chained to a same-block
    // PTXROLLCOMMIT (payment) and reveal the same round under the same quorum.
    if (!CheckPTXRollCommitSettlePairing(block, confirmedSettleParents, state)) {
        return false;
    }

    // KDD-058-A: a connecting block that includes a PTXDKG erases it from the
    // replicated minable-commitments store (LLMQ ProcessCommitment precedent;
    // !fJustCheck only).  BOOKKEEPING-ONLY — no validation behavior lives
    // here; the erase-on-inclusion posture is unchanged from the W1.3 slot
    // (E-5: disconnect does NOT re-pend).
    if (!fJustCheck) {
        PTX_DKG_Commitments_EraseMined(block);
    }

    // Fetch DGM list and pose tracker once for both P10 and the state update.
    CDeterministicGMList gmList;
    if (pindex->pprev != nullptr) {
        gmList = deterministicGMManager->GetListForBlock(pindex->pprev);
    }
    // BUG-026 (A): the payout MUST be judged against the PRE-BLOCK pose.  The
    // pose update below used to run BEFORE this call, so the winner was selected
    // from post-block pose at connect but from pre-block pose under fJustCheck
    // (where the update is skipped) — the assembler and the validator therefore
    // selected different winners and every settlement boundary carrying a roll
    // self-rejected with ptxpayout-wrong-recipient (the h420 fleet wedge).  This
    // is BUG-024's fJustCheck asymmetry on the pose axis; BUG-024 fixed it for
    // the accumulator by exporting the effective value, and the same invariant
    // is restored here by ORDERING: selection strictly precedes the mutation, so
    // both fJustCheck paths read identical state by construction.
    // ★ It is deterministic, not incidental: PTX_SelectWinner uses total_tickets
    // as its modulus (winning_ticket = entropy % total_tickets + 1), so crediting
    // this block's own participants re-maps the entropy (measured live: 330 ->
    // 396 for a 6-roll boundary block) and additionally admits 0-ticket GMs into
    // the candidate set.  Nothing between here and the pose update reads pose.
    if (!CheckAndApplyPTXPayout(block, pindex, gmList, g_ptx_pose_tracker,
                                effAccumOutpoint, effAccumValue, state, fJustCheck)) {
        return false;
    }

    // Update pose tracker from PTXSESS quorum_members in this block.
    // ptx_roll (caller-side) calls RecordHonestParticipation for the nodes it observed
    // sign, but that only updates the caller's g_ptx_pose_tracker. Validators (staking
    // GMs) never run ptx_roll, so their pose trackers were always empty — causing P11
    // to evaluate the eligible-winners check differently on caller vs staking nodes
    // (consensus split at every settlement boundary). The fix: when a PTXSESS confirms
    // in a block, all validators apply RecordHonestParticipation for its quorum_members,
    // making the pose tracker consensus-derived from the chain.
    // BUG-026 (A): MOVED to after the payout check (see above).  Placement is
    // load-bearing, not cosmetic — do not hoist it back above the selection.
    if (!fJustCheck) {
        for (const CTransactionRef& tx : block.vtx) {
            if (!tx->IsProbabilisticTx()) continue;
            CProbabilisticTxPayload payload;
            if (!GetTxPayload(*tx, payload)) continue;
            for (const std::string& nid : payload.quorum_members) {
                g_ptx_pose_tracker.RecordHonestParticipation(nid);
            }
        }
    }

    if (!llmq::quorumBlockProcessor->ProcessBlock(block, pindex, state, fJustCheck)) {
        // pass the state returned by the function above
        return false;
    }

    if (!deterministicGMManager->ProcessBlock(block, pindex, state, fJustCheck)) {
        // pass the state returned by the function above
        return false;
    }

    // W2.1 C1: persist the accepted PTXDKG as an ACTIVE quorum record (evodb;
    // LLMQ mined-commitment pattern).  Checks run under fJustCheck; the write
    // is !fJustCheck only.  Undo mirror in UndoSpecialTxsInBlock.
    // Null tolerance is unit-test-environment-only (the SelectAtAnchor
    // precedent): production constructs the store at tiertwo/init before
    // validation.  Pre-W4-f the null-this call was latent UB that happened
    // not to crash (both methods only touched the block argument before
    // their early-returns); the W4-f hooks touch member state, surfacing it.
    if (ptxQuorumStore && !ptxQuorumStore->ProcessBlock(block, pindex, state, fJustCheck)) {
        // pass the state returned by the function above
        return false;
    }

    // BUG-027 / ODC-056(c): snapshot the POST-BLOCK pose so DisconnectBlock can
    // restore it, exactly as CheckAndApplyPTXCoalesce already does for
    // LotteryState.  Written UNCONDITIONALLY on the accept path — even when this
    // block credited no pose at all — because the undo reads the PPREV snapshot
    // and a gap makes the predecessor unrestorable (the same reason the
    // LotteryState "no PTXCOALESCE" arm still writes one).
    // Placement is load-bearing: after the RecordHonestParticipation loop, so it
    // captures the block's own credits, and before commit(), so a later failure
    // still unwinds through the sentry rather than leaving a snapshot for a block
    // that never connected.
    if (!fJustCheck) {
        WritePoseSnapshotForBlock(pindex->GetBlockHash(),
                                  g_ptx_pose_tracker.GetAllRecords());
    }

    // BUG-026 (B): the block is accepted — keep every mutation made above.
    // Reaching here is the ONLY path that disarms the rollback sentry.
    ptxStateSentry.commit();
    return true;
}

bool CheckAndApplyPTXCoalesce(const CBlock& block,
                              const CBlockIndex* pindex,
                              CValidationState& state,
                              bool fJustCheck,
                              COutPoint* pEffAccumOutpoint,
                              CAmount* pEffAccumValue)
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
        // BUG-032 2b-iii: the fee source is the PTXROLLCOMMIT, not the PTXSESS —
        // PTX_CollectRollFeeOutputs scans commits (the SAME source the assembler and
        // CheckPTXCoalesceBlockRules use, producer/validator lockstep). Do NOT key
        // this on PTXSESS presence — that proxy halted the chain at h510.
        std::vector<COutPoint> expectedVin;
        CAmount expectedValue = ls.accumulator_value;

        if (ls.HasAccumulator()) {
            expectedVin.push_back(ls.accumulator_outpoint);
        }

        std::vector<AccumInput> rollFees = PTX_CollectRollFeeOutputs(block.vtx);
        for (const AccumInput& inp : rollFees) {
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

        // BUG-024: export the effective post-coalesce accumulator under BOTH
        // fJustCheck values — the payout checks that follow must see it even
        // when the apply below is skipped, or a coalesce+payout block fails
        // its own TestBlockValidity while connecting fine.
        if (pEffAccumOutpoint) *pEffAccumOutpoint = COutPoint(coalesceTx->GetHash(), 0);
        if (pEffAccumValue)    *pEffAccumValue    = expectedValue;

        if (!fJustCheck) {
            LotteryState& mls = GetLotteryState();
            mls.accumulator_outpoint = COutPoint(coalesceTx->GetHash(), 0);
            mls.accumulator_value    = expectedValue;
            // Semantic count: rollFees is the canonical list from PTX_CollectRollFeeOutputs
            // (one per PTXROLLCOMMIT fee output — incl. an orphan commit whose settle never
            // came), not inferred from vin shape (robust to future PTXCOALESCE schema changes).
            mls.total_rolls += rollFees.size();
            WriteLotteryStateSnapshotForBlock(pindex->GetBlockHash(), mls);
        }
    } else {
        // No PTXCOALESCE: the effective accumulator is the current one.
        if (pEffAccumOutpoint) *pEffAccumOutpoint = GetLotteryState().accumulator_outpoint;
        if (pEffAccumValue)    *pEffAccumValue    = GetLotteryState().accumulator_value;
        // LotteryState unchanged.  Still write the post-block snapshot
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

    // ★ BUG-027 / ODC-056(c): restore POSE to the pre-block state too.
    //
    // THE DEFECT THIS CLOSES.  Until now this function restored LotteryState and
    // said nothing about pose, so a disconnected block's
    // RecordHonestParticipation credits were never removed — pose was MONOTONIC
    // across reorgs while the accumulator was transactional.  Measured on the
    // Phase-2 fleet at total_rolls=2 (expected 11*2 = 22 tickets): nodes read
    // 22 / 33 / 44 / 88 strictly in proportion to their chain-switch count
    // (gm01 48 tip regressions -> 88; gm03 14 -> 22).  Divergent pose selects a
    // different settlement winner, so the h300 boundary was rejected fleet-wide
    // by the contaminated nodes; and because wallet-less GMs cannot stake, the
    // stranded nodes WERE the producer set, fragmenting stake until the majority
    // chain held 0.4% of weight and stopped advancing.  A consensus defect became
    // a chain HALT.
    //
    // Mirrors the LotteryState arm above in every respect, deliberately —
    // including refusing the disconnect on a missing snapshot.  Restoring a
    // default-constructed map instead would ZERO every node's pose and silently
    // corrupt winner selection for the rest of the chain, which is strictly worse
    // than declining to disconnect.
    {
        AssertLockHeld(cs_main);
        assert(pindex->pprev != nullptr);
        std::map<std::string, PTXNodeRecord> prevPose;
        if (!ReadPoseSnapshotForBlock(pindex->pprev->GetBlockHash(), prevPose)) {
            LogPrintf("%s: missing pose snapshot for %s — evodb integrity failure\n",
                      __func__, pindex->pprev->GetBlockHash().ToString());
            return false;
        }
        g_ptx_pose_tracker.RestoreRecords(std::move(prevPose));
    }

    // W2.1 C1: reverse of connect order — the PTXDKG quorum record is written
    // last at connect, so it is erased first at disconnect.  Explicit-erase of
    // both evodb keys; NO re-pend (E-5 — re-submission is W2.2 formation's).
    if (ptxQuorumStore && !ptxQuorumStore->UndoBlock(block, pindex)) {  // null: test-env only (see connect side)
        return false;
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
