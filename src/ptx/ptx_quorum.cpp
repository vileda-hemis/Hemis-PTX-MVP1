// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_quorum.h"

#include "logging.h"
#include "netbase.h"
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
        // Format: id@host:port  (documented in ptx_quorum.h)
        // id is a label or compound label:suffix (KDD-033); both are @-free.
        // host:port is delegated to SplitHostPort, which handles IPv4 and bracketed IPv6
        // ([fe80::1]:29903) identically to the rest of the codebase.
        size_t at = spec.find('@');
        if (at == std::string::npos || at == 0) {
            LogPrintf("PTX: invalid -ptxnode (expected id@host:port): %s\n", spec);
            continue;
        }
        std::string id        = spec.substr(0, at);
        std::string host_port = spec.substr(at + 1);

        int         port = 0;
        std::string host;
        SplitHostPort(host_port, port, host);
        if (host.empty() || port <= 0 || port > 65535) {
            LogPrintf("PTX: invalid -ptxnode (bad host:port '%s'): %s\n", host_port, spec);
            continue;
        }

        PTXNodeInfo ni;
        ni.node_id = id;
        ni.host    = host;
        ni.port    = (uint16_t)port;
        LogPrintf("PTX: loaded node %s at %s:%d\n", ni.node_id, ni.host, (int)ni.port);
        g_ptx_nodes.push_back(std::move(ni));
    }
    LogPrintf("PTX: %d node(s) loaded; my_node_id=%s\n",
              (int)g_ptx_nodes.size(),
              g_ptx_my_node_id.empty() ? "(none)" : g_ptx_my_node_id);
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
