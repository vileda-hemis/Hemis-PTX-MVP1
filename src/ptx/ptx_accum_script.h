// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef Hemis_PTX_ACCUM_SCRIPT_H
#define Hemis_PTX_ACCUM_SCRIPT_H

#include "script/script.h"

/**
 * Returns the deterministic LOTTERY_ACCUM_SCRIPT used by PTXCOALESCE and PTXPAYOUT.
 *
 * Derivation (ODC-022):
 *   1. label  = "Hemis PTX Lottery Accumulator v1"
 *   2. h256   = SHA256(SHA256(label))        — 32 bytes, no known preimage as an EC point
 *   3. pubkh  = RIPEMD160(SHA256(h256))      — 20 bytes
 *   4. script = P2PKH(pubkh)
 *
 * The script is computed once and cached for the lifetime of the process.
 * Spending this output is gated by consensus rules only (no private key exists).
 */
const CScript& GetLotteryAccumScript();

/** Logs the LOTTERY_ACCUM_SCRIPT and its corresponding address at startup. */
void LogLotteryAccumScriptAtStartup();

#endif // Hemis_PTX_ACCUM_SCRIPT_H
