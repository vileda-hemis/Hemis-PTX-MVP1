// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_quorum.h"

#include "logging.h"
#include "ptx/ptx_output_mapping.h"
#include "ptx/ptx_pose.h"
#include "util/system.h"

#include <algorithm>
#include <climits>
#include <stdexcept>

std::vector<PTXNodeInfo> g_ptx_nodes;
std::string              g_ptx_my_node_id;
uint256                  g_ptx_last_beacon;
RecursiveMutex           cs_ptx_last_beacon;

void PTX_LoadNodesFromArgs()
{
    g_ptx_nodes.clear();
    // Accept -ptxnodeid (canonical) or legacy -ptxmynodeid from docker-compose.
    g_ptx_my_node_id = gArgs.GetArg("-ptxnodeid", gArgs.GetArg("-ptxmynodeid", ""));

    for (const std::string& spec : gArgs.GetArgs("-ptxnode")) {
        // Expected format: id:host:port:user:password (5 colon-separated fields)
        std::vector<std::string> parts;
        std::stringstream ss(spec);
        std::string token;
        while (std::getline(ss, token, ':')) parts.push_back(token);
        if (parts.size() < 3) {
            LogPrintf("PTX: invalid -ptxnode (expected id:host:port[:user:pass]): %s\n", spec);
            continue;
        }
        int port = 0;
        try {
            port = std::stoi(parts[2]);
        } catch (...) {
            LogPrintf("PTX: invalid -ptxnode (bad port): %s\n", spec);
            continue;
        }
        if (port <= 0 || port > 65535) {
            LogPrintf("PTX: invalid -ptxnode (port out of range): %s\n", spec);
            continue;
        }
        PTXNodeInfo ni;
        ni.node_id = parts[0];
        ni.host    = parts[1];
        ni.port    = (uint16_t)port;
        g_ptx_nodes.push_back(std::move(ni));
        LogPrintf("PTX: loaded node %s at %s:%d\n", g_ptx_nodes.back().node_id,
                  g_ptx_nodes.back().host, (int)g_ptx_nodes.back().port);
    }
    LogPrintf("PTX: %d node(s) loaded; my_node_id=%s\n",
              (int)g_ptx_nodes.size(),
              g_ptx_my_node_id.empty() ? "(none)" : g_ptx_my_node_id);
}

int PTX_NodeScore(const std::string& node_id)
{
    if (!g_ptx_pose_tracker.IsEligible(node_id)) return INT_MAX;
    return g_ptx_pose_tracker.GetRecord(node_id).pose_score;
}

PTXQuorumAssignment PTX_AssignQuorum(const std::string& round_id,
                                     const uint256& round_seed,
                                     int quorum_size,
                                     int threshold)
{
    // Build eligible pool sorted stably by (score, node_id) for determinism across nodes.
    std::vector<std::pair<int, std::string>> scored;
    scored.reserve(g_ptx_nodes.size());
    for (const auto& ni : g_ptx_nodes) {
        int s = PTX_NodeScore(ni.node_id);
        if (s != INT_MAX) {
            scored.emplace_back(s, ni.node_id);
        }
    }
    std::sort(scored.begin(), scored.end());

    if ((int)scored.size() < quorum_size) {
        throw std::runtime_error("PTX: not enough eligible nodes for quorum assignment");
    }

    // Deterministic Fisher-Yates over the eligible pool using round_seed as entropy.
    std::vector<std::string> pool;
    pool.reserve(scored.size());
    for (const auto& p : scored) pool.push_back(p.second);

    auto exp = PTX_ExpandBeacon(round_seed, pool.size() * 32);
    size_t off = 0;
    for (int i = 0; i < quorum_size; i++) {
        int64_t j = (int64_t)i;
        PTX_SampleOne(exp, off, (int64_t)i, (int64_t)pool.size() - 1, j);
        std::swap(pool[(size_t)i], pool[(size_t)j]);
    }

    PTXQuorumAssignment qa;
    qa.round_id  = round_id;
    qa.threshold = threshold;
    qa.members   = std::vector<std::string>(pool.begin(), pool.begin() + quorum_size);

    LogPrintf("PTX: quorum assigned round=%s size=%d threshold=%d\n",
              round_id, quorum_size, threshold);
    return qa;
}

uint256 PTX_GetLastBeacon()
{
    LOCK(cs_ptx_last_beacon);
    return g_ptx_last_beacon;
}

void PTX_SetLastBeacon(const uint256& beacon)
{
    LOCK(cs_ptx_last_beacon);
    g_ptx_last_beacon = beacon;
    LogPrintf("PTX: last beacon updated %s\n", beacon.GetHex());
}
