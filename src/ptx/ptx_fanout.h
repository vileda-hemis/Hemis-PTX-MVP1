// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_FANOUT_H
#define HEMIS_PTX_FANOUT_H

#include "ptx/ptx_quorum.h"
#include "sync.h"
#include "uint256.h"

#include <univalue.h>

#include <map>
#include <string>
#include <vector>

// Response from a single PTX RPC fan-out call to a remote node.
struct PTXRpcResponse {
    bool success{false};
    std::string body;
};

// Coordinator-side fail-mode map for testing node misbehavior.
// Modes: "abstain" | "withhold" | "invalid_commit"
extern RecursiveMutex cs_ptx_failmodes;
extern std::map<std::string, std::string> g_ptx_node_failmodes;

// Send a JSON-RPC 1.0 POST to node.host:node.port. Returns success=true when
// the response body contains {"result":{"accepted":true}}.
PTXRpcResponse PTX_CallNodeRpc(const PTXNodeInfo& node,
                               const std::string& method,
                               const UniValue& params);

// Compute SHA256(secret||round_seed) commitments for all member_ids and
// fan out gm_commit via HTTP. Fail modes from g_ptx_node_failmodes are applied
// coordinator-side. cs_ptx_rounds is NOT held during HTTP calls.
void PTX_FanOutCommit(const std::string& round_id,
                      const std::map<std::string, uint256>& secrets,
                      const uint256& round_seed,
                      const std::vector<std::string>& member_ids);

// Fan out gm_reveal to all committed quorum members. cs_ptx_rounds is NOT
// held during HTTP calls.
void PTX_FanOutReveal(const std::string& round_id,
                      const std::map<std::string, uint256>& secrets);

// --- BLS threshold signing (Phase 2) ---

// Send each GM in member_ids its BLS key share via gm_bls_keyset.
// Skips nodes that already have a keyset flagged in g_ptx_bls.keyset_sent.
// cs_ptx_bls must NOT be held during HTTP calls.
void PTX_FanOutKeySet(const std::vector<std::string>& member_ids);

// Ask each GM in member_ids to sign round_seed via gm_bls_sign.
// Returns node_id -> 96-byte partial signature for every node that responds.
// cs_ptx_rounds is NOT held during HTTP calls.
std::map<std::string, std::vector<uint8_t>> PTX_FanOutSign(
    const std::string& round_id,
    const uint256& round_seed,
    const std::vector<std::string>& member_ids);

#endif // HEMIS_PTX_FANOUT_H
