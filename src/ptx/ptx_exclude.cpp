// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_exclude.h"

#include "logging.h"
#include "primitives/transaction.h"
#include "txdb.h"
#include "uint256.h"
#include "validation.h"

std::set<int64_t> PTX_ResolveExclude(const UniValue& exclude_param)
{
    if (exclude_param.isNull() || !exclude_param.isArray())
        return {};

    std::set<int64_t> result;

    for (size_t i = 0; i < exclude_param.size(); i++) {
        const UniValue& item = exclude_param[i];

        if (item.isNum()) {
            result.insert(item.get_int64());
        } else if (item.isStr()) {
            uint256 txid = uint256S(item.get_str());
            CTransactionRef tx;
            uint256 block_hash;
            // GetTransaction acquires cs_main internally — caller must NOT hold cs_ptx_rounds.
            if (GetTransaction(txid, tx, block_hash, true, nullptr) && !block_hash.IsNull()) {
                CProbabilisticTxPayload prev_payload;
                if (GetTxPayload(*tx, prev_payload)) {
                    for (int64_t v : prev_payload.results)
                        result.insert(v);
                }
            } else {
                LogPrintf("PTX exclude: tx %s not confirmed, skipping\n", txid.GetHex());
            }
        }
    }

    return result;
}

void PTX_BuildExcludeLists(const UniValue& param,
                            std::vector<int64_t>& out_ints,
                            std::vector<std::string>& out_txids)
{
    if (param.isNull() || !param.isArray())
        return;

    for (size_t i = 0; i < param.size(); i++) {
        const UniValue& item = param[i];
        if (item.isNum()) {
            out_ints.push_back(item.get_int64());
        } else if (item.isStr()) {
            out_txids.push_back(item.get_str());
        }
    }
}
