// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_wallet.h"

#include "script/ismine.h"

std::vector<LastSettlement> PTX_FilterWalletSettlements(
    const CKeyStore&                    ks,
    const std::vector<LastSettlement>&  history)
{
    std::vector<LastSettlement> result;
    for (const auto& s : history) {
        if (s.winner_script.empty())
            continue;
        if ((IsMine(ks, s.winner_script) & ISMINE_SPENDABLE) != 0)
            result.push_back(s);
    }
    return result;
}

std::vector<WalletGMInfo> PTX_FilterWalletGMs(
    const CKeyStore&            ks,
    const CDeterministicGMList& gmList,
    const PTXPoSeTracker&       tracker)
{
    std::vector<WalletGMInfo> result;

    gmList.ForEachGM(/*onlyValid=*/false, [&](const CDeterministicGMCPtr& dgm) {
        const CScript& payScript = dgm->pdgmState->scriptPTXPayment;
        if (payScript.empty())
            return;
        if ((IsMine(ks, payScript) & ISMINE_SPENDABLE) == 0)
            return;

        WalletGMInfo info;
        info.node_id        = dgm->pdgmState->node_id;
        info.payment_script = payScript;

        const std::string& nid = dgm->pdgmState->node_id;
        if (!nid.empty()) {
            PTXNodeRecord rec = tracker.GetRecord(nid);
            info.tickets    = rec.lottery_tickets;
            info.eligible   = rec.quorum_eligible;
            info.pose_score = rec.pose_score;
        }

        result.push_back(std::move(info));
    });

    return result;
}
