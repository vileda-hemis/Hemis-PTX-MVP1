// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_seed.h"

#include "crypto/common.h"
#include "crypto/sha256.h"
#include "hash.h"
#include "random.h"
#include "streams.h"
#include "version.h"

uint256 PTX_BuildNonce(const uint256& prev_beacon, const std::vector<uint8_t>& caller_salt)
{
    CSHA256 h;
    h.Write(prev_beacon.begin(), 32);
    h.Write(caller_salt.data(), caller_salt.size());
    uint256 r;
    h.Finalize(r.begin());
    return r;
}

uint256 PTX_BuildRoundSeed(const std::string& game_id, uint32_t block_height,
                            const std::vector<uint8_t>& caller_pubkey,
                            const uint256& nonce, const uint256& params_hash)
{
    CSHA256 h;
    h.Write(reinterpret_cast<const uint8_t*>(game_id.data()), game_id.size());
    uint8_t bh[4];
    WriteLE32(bh, block_height); // KDD-003: anchors seed to block, prevents re-roll within block
    h.Write(bh, 4);
    if (!caller_pubkey.empty()) {
        h.Write(caller_pubkey.data(), caller_pubkey.size());
    }
    h.Write(nonce.begin(), 32);
    h.Write(params_hash.begin(), 32);
    uint256 r;
    h.Finalize(r.begin());
    return r;
}

uint256 PTX_HashParams(uint32_t count, int64_t low, int64_t high, bool unique,
                        const std::vector<int64_t>& exc_ints,
                        const std::vector<std::string>& exc_txids)
{
    CDataStream ds(SER_GETHASH, PROTOCOL_VERSION);
    ds << count << low << high << unique << exc_ints << exc_txids;
    return Hash(ds.begin(), ds.end());
}

std::string PTX_MakeRoundId(const std::string& game_id, uint32_t block_height,
                              const uint256& params_hash)
{
    CSHA256 h;
    h.Write(reinterpret_cast<const uint8_t*>(game_id.data()), game_id.size());
    uint8_t bh[4];
    WriteLE32(bh, block_height);
    h.Write(bh, 4);
    h.Write(params_hash.begin(), 32);
    uint8_t rand8[8];
    GetRandBytes(rand8, 8); // ensures uniqueness for concurrent same-block same-game_id calls
    h.Write(rand8, 8);
    uint256 r;
    h.Finalize(r.begin());
    return r.GetHex().substr(0, 32);
}
