// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-2014 The Bitcoin developers
// Copyright (c) 2016-2021 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "chainparamsbase.h"

#include "tinyformat.h"
#include "util/system.h"

#include <assert.h>

const std::string CBaseChainParams::MAIN = "main";
const std::string CBaseChainParams::TESTNET = "test";
const std::string CBaseChainParams::REGTEST = "regtest";
const std::string CBaseChainParams::PTXTESTNET = "ptxtestnet";
const std::string CBaseChainParams::PTXBEATESTNET = "ptxbea";

void AppendParamsHelpMessages(std::string& strUsage, bool debugHelp)
{
    strUsage += HelpMessageGroup("Chain selection options:");
    strUsage += HelpMessageOpt("-testnet", "Use the test chain");
    strUsage += HelpMessageOpt("-ptxtestnet", "Use the PTX closed testnet chain");
    strUsage += HelpMessageOpt("-ptxbea", "Use the PTX bea testnet chain (ODC-022 Solution 1)");
    if (debugHelp) {
        strUsage += HelpMessageOpt("-regtest", "Enter regression test mode, which uses a special chain in which blocks can be solved instantly. "
                                               "This is intended for regression testing tools and app development.");
    }
}

static std::unique_ptr<CBaseChainParams> globalChainBaseParams;

const CBaseChainParams& BaseParams()
{
    assert(globalChainBaseParams);
    return *globalChainBaseParams;
}

std::unique_ptr<CBaseChainParams> CreateBaseChainParams(const std::string& chain)
{
    if (chain == CBaseChainParams::MAIN)
        return std::make_unique<CBaseChainParams>("", 51473);
    else if (chain == CBaseChainParams::TESTNET)
        return std::make_unique<CBaseChainParams>("testnet5", 51475);
    else if (chain == CBaseChainParams::REGTEST)
        return std::make_unique<CBaseChainParams>("regtest", 51477);
    else if (chain == CBaseChainParams::PTXTESTNET)
        // ★★ 29902 -> 29995 (2026-08-21). THIS NUMBER IS NOT COSMETIC: it is the
        // port the PTX signing fan-out dials. PTX_FanoutRpcPort()
        // (ptx/ptx_fanout.cpp:117-120) returns
        //     gArgs.GetArg("-ptxfanoutport", BaseParams().RPCPort())
        // and BaseParams().RPCPort() is THIS value -- it never consults -rpcport
        // (chainparamsbase.h:30 is a bare `return nRPCPort`; the daemon's actual
        // bound port comes from httpserver.cpp:297 instead). The DGM record carries
        // only the P2P endpoint, so PTX_ResolveMemberAddr takes the on-chain HOST
        // and pairs it with THIS port (ptx_fanout.cpp:126-131).
        //
        // With 29902 here and install.sh writing rpcport=29995, the fan-out dialled
        // a port nothing listened on: quorums form, every gm_bls_sign request lands
        // on a closed port, and the member looks perfect on-chain while never
        // signing. ptxbea never showed this because its default (29995, :56) already
        // equalled its configured port -- a coincidence of that network's
        // definition. ★ Verified live 2026-08-21: a ptxbea roll opened 60-80
        // concurrent outbound sockets to remote port 29995 and none to 29994.
        //
        // ★ KEEP THIS EQUAL TO THE PORT install.sh WRITES. See the convention at
        // ptx_fanout.cpp:104-116 and ptxbea-known-limitations.md §13.
        return std::make_unique<CBaseChainParams>("ptxtestnet", 29995);
    else if (chain == CBaseChainParams::PTXBEATESTNET)
        // ptxbea testnet RPC = P2P+1 (P2P 29994, chainparams.cpp), restoring the
        // PIVX-lineage adjacency (cf. testnet 51474/51475) while staying BELOW the
        // kernel ephemeral range (32768-60999) — the whole 5147x Hemis family sits
        // INSIDE that range, so following the lineage block literally would
        // reintroduce the caller7 ephemeral-port race on every operator host.
        return std::make_unique<CBaseChainParams>("ptxbea", 29995);
    else
        throw std::runtime_error(strprintf("%s: Unknown chain %s.", __func__, chain));
}

void SelectBaseParams(const std::string& chain)
{
    globalChainBaseParams = CreateBaseChainParams(chain);
    gArgs.SelectConfigNetwork(chain);
}
