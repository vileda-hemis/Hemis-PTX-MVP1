// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_QUORUM_H
#define HEMIS_PTX_QUORUM_H

#include "sync.h"
#include "uint256.h"

#include <cstdint>
#include <string>
#include <vector>

struct PTXNodeInfo {
    std::string          node_id;
    std::string          host;
    uint16_t             port{0};
    std::vector<uint8_t> pubkey; // optional; used in seed construction
};

extern std::vector<PTXNodeInfo> g_ptx_nodes;
extern std::string              g_ptx_my_node_id;
extern uint256                  g_ptx_last_beacon;
extern RecursiveMutex           cs_ptx_last_beacon;

// Parse -ptxnode=id@host:port entries and -ptxmynodeid=id from gArgs.
void PTX_LoadNodesFromArgs();

uint256 PTX_GetLastBeacon();
void    PTX_SetLastBeacon(const uint256& beacon);

#endif // HEMIS_PTX_QUORUM_H
