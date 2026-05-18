// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_BLS_H
#define HEMIS_PTX_BLS_H

#include "bls/bls_wrapper.h"
#include "sync.h"
#include "uint256.h"

#include <map>
#include <set>
#include <string>
#include <vector>

// Trusted-dealer threshold BLS state (coordinator side).
// Coordinator generates the master polynomial and distributes key shares to GMs.
// Any t-of-n GMs can produce partial signatures that reconstruct the same
// deterministic threshold signature — the basis for the PTX beacon.
struct PTXBLSState {
    // Master polynomial coefficients: msk[0] is the master secret; msk[1..t-1] are random.
    std::vector<CBLSSecretKey> msk;
    // Verification vector: mpk[i] = msk[i].GetPublicKey(); threshold sig verifies vs mpk[0].
    std::vector<CBLSPublicKey> mpk;
    // Per-GM key shares derived from the polynomial: node_id -> share
    std::map<std::string, CBLSSecretKey> shares;
    // Per-GM BLS IDs (SHA256 of node_id_string as uint256): node_id -> CBLSId
    std::map<std::string, CBLSId> ids;
    // Tracks which GMs have received their key share this session.
    std::set<std::string> keyset_sent;

    int threshold{0};
    bool initialized{false};
};

extern PTXBLSState g_ptx_bls;
extern RecursiveMutex cs_ptx_bls;

// Initialize BLS state with all registered GM node_ids and threshold t.
// Generates a fresh master polynomial of degree t-1; computes per-GM key shares.
// Must be called after g_ptx_nodes is populated (PTX_LoadNodesFromArgs).
bool PTX_BLS_Init(const std::vector<std::string>& node_ids, int threshold);

// Return master public key (mpk[0]) — threshold signatures verify against this.
CBLSPublicKey PTX_BLS_GetMasterPubKey();

// Derive a deterministic CBLSId for a node: CBLSId(SHA256(node_id_bytes)).
CBLSId PTX_BLS_NodeId(const std::string& node_id);

// Recover a threshold signature from t partial signatures and their node CBLSIds.
// Returns an invalid CBLSSignature on failure (< t sigs or Lagrange error).
CBLSSignature PTX_BLS_Recover(const std::vector<CBLSSignature>& partial_sigs,
                               const std::vector<CBLSId>& ids);

// Derive the PTX beacon from a threshold signature: SHA256(96-byte sig serialization).
uint256 PTX_BLS_SigToBeacon(const CBLSSignature& sig);

#endif // HEMIS_PTX_BLS_H
