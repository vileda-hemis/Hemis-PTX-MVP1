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
        // 29902 -> 29995 (2026-08-21). ★ HISTORICAL NOTE, KEPT DELIBERATELY:
        // this number used to be load-bearing for CONSENSUS-ADJACENT behaviour,
        // not just for operators. The PTX signing fan-out dialled each member's
        // RPC at BaseParams().RPCPort(), so with 29902 here and install.sh
        // writing rpcport=29995 every gm_bls_sign request landed on a closed
        // port: quorums formed, nobody signed, and each member looked perfect on
        // chain. ptxbea never showed it because its default already equalled its
        // configured port -- a coincidence of that network's definition.
        //
        // ★★ KDD-085 REMOVED THAT COUPLING ENTIRELY (component 4). Signing is
        // delivered over P2P to the member's on-chain-advertised address, which
        // carries its own port, so no code pairs a DGM host with this value any
        // more and `-ptxfanoutport` is gone. This is now an ordinary RPC-port
        // default: getting it wrong inconveniences an operator at a prompt and
        // cannot stop a quorum signing.
        // ★ Still keep it equal to the port install.sh writes -- one number in
        // two places is one number that can disagree with itself.
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
