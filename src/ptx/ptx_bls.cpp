// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_bls.h"

#include "crypto/sha256.h"
#include "logging.h"

PTXBLSState g_ptx_bls;
RecursiveMutex cs_ptx_bls;

bool PTX_BLS_Init(const std::vector<std::string>& node_ids, int threshold)
{
    if (node_ids.empty() || threshold < 1 || threshold > (int)node_ids.size())
        return false;

    PTXBLSState state;
    state.threshold = threshold;

    // Generate master polynomial: degree = t-1, so t coefficients.
    state.msk.resize(threshold);
    state.mpk.resize(threshold);
    for (int i = 0; i < threshold; i++) {
        state.msk[i].MakeNewKey();
        state.mpk[i] = state.msk[i].GetPublicKey();
    }

    // Compute per-GM secret key share and BLS ID.
    for (const auto& node_id : node_ids) {
        CBLSId id = PTX_BLS_NodeId(node_id);
        state.ids[node_id] = id;

        CBLSSecretKey share;
        if (!share.SecretKeyShare(state.msk, id)) {
            LogPrintf("PTX BLS: SecretKeyShare failed node=%s\n", node_id);
            return false;
        }
        state.shares[node_id] = share;
    }

    state.initialized = true;

    LOCK(cs_ptx_bls);
    g_ptx_bls = std::move(state);
    LogPrintf("PTX BLS: initialized n=%d t=%d\n",
              (int)g_ptx_bls.shares.size(), g_ptx_bls.threshold);
    return true;
}

CBLSPublicKey PTX_BLS_GetMasterPubKey()
{
    LOCK(cs_ptx_bls);
    return g_ptx_bls.mpk.empty() ? CBLSPublicKey{} : g_ptx_bls.mpk[0];
}

CBLSId PTX_BLS_NodeId(const std::string& node_id)
{
    CSHA256 h;
    h.Write((const unsigned char*)node_id.data(), node_id.size());
    uint256 hash;
    h.Finalize(hash.begin());
    return CBLSId(hash);
}

CBLSSignature PTX_BLS_Recover(const std::vector<CBLSSignature>& partial_sigs,
                               const std::vector<CBLSId>& ids)
{
    CBLSSignature result;
    if (!result.Recover(partial_sigs, ids)) {
        return CBLSSignature{};
    }
    return result;
}

uint256 PTX_BLS_SigToBeacon(const CBLSSignature& sig)
{
    if (!sig.IsValid())
        return uint256{};
    std::vector<uint8_t> sig_bytes = sig.ToByteVector();
    CSHA256 h;
    h.Write(sig_bytes.data(), sig_bytes.size());
    uint256 beacon;
    h.Finalize(beacon.begin());
    return beacon;
}
