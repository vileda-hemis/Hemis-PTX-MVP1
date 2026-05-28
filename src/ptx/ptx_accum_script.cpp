// Copyright (c) 2026 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx_accum_script.h"

#include "hash.h"
#include "key_io.h"
#include "script/standard.h"
#include "uint256.h"
#include "util/system.h"

#include <cstring>
#include <string>

static const char* ACCUM_LABEL = "Hemis PTX Lottery Accumulator v1";

static CScript ComputeLotteryAccumScript()
{
    const unsigned char* label = reinterpret_cast<const unsigned char*>(ACCUM_LABEL);
    const size_t labelLen = strlen(ACCUM_LABEL);

    // Step 1: SHA256(SHA256(label)) — produces 32 bytes with no known EC preimage
    uint256 h256;
    CHash256().Write(label, labelLen).Finalize(h256.begin());

    // Step 2: RIPEMD160(SHA256(h256)) — produces the 20-byte pubKeyHash
    uint160 pubkh;
    CHash160().Write(h256.begin(), 32).Finalize(pubkh.begin());

    // Step 3: P2PKH script
    return CScript() << OP_DUP << OP_HASH160 << ToByteVector(pubkh) << OP_EQUALVERIFY << OP_CHECKSIG;
}

const CScript& GetLotteryAccumScript()
{
    static const CScript script = ComputeLotteryAccumScript();
    return script;
}

void LogLotteryAccumScriptAtStartup()
{
    const CScript& script = GetLotteryAccumScript();
    CTxDestination dest;
    if (ExtractDestination(script, dest)) {
        LogPrintf("PTX: LOTTERY_ACCUM_SCRIPT address = %s\n", EncodeDestination(dest));
    } else {
        LogPrintf("PTX: LOTTERY_ACCUM_SCRIPT computed (address encoding unavailable at this init stage)\n");
    }
}
