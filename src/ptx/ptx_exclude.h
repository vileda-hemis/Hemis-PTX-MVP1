// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef HEMIS_PTX_EXCLUDE_H
#define HEMIS_PTX_EXCLUDE_H

#include "univalue.h"

#include <cstdint>
#include <set>
#include <string>
#include <vector>

// KDD-025: exclude param is a mixed array of integers and tx_id strings.
// Integers are added directly; tx_id strings resolve to the results[]
// of a confirmed PTX transaction.

// Returns the union of all excluded values from the exclude param.
// Integers added directly; confirmed PTX tx_ids expanded via their results[].
// Drops unconfirmed tx_ids with a LogPrintf.
std::set<int64_t> PTX_ResolveExclude(const UniValue& exclude_param);

// Splits the exclude param into raw integers and tx_id strings
// without touching the chain. Used when storing the payload.
void PTX_BuildExcludeLists(const UniValue& param,
                            std::vector<int64_t>& out_ints,
                            std::vector<std::string>& out_txids);

#endif // HEMIS_PTX_EXCLUDE_H
