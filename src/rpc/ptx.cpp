// Copyright (c) 2024 The Hemis Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ptx/ptx_bls.h"
#include "ptx/ptx_commit_reveal.h"
#include "ptx/ptx_dkg.h"
#include "ptx/ptx_dkg_commitments.h"
#include "ptx/ptx_formation.h"
#include "core_io.h" // EncodeHexTx (C6 build_only mode)
#include "ptx/ptx_fanout.h"
#include "ptx/ptx_lottery_state.h"
#include "ptx/ptx_mempool.h"
#include "ptx/ptx_output_mapping.h"
#include "ptx/ptx_pose.h"
#include "ptx/ptx_quorum.h"
#include "ptx/ptx_quorum_store.h"
#include "ptx/ptx_seed.h"
#include "bls/key_io.h" // bls::DecodeSecret (W2.1 C0 operator_keys mode)
#include "crypto/sha256.h"
#include "evo/deterministicgms.h"
#include "key_io.h"
#include "logging.h"
#include "primitives/transaction.h"
#include "random.h"
#include "rpc/protocol.h"
#include "rpc/server.h"
#include "script/standard.h"
#include "sync.h"
#include "uint256.h"
#include "util/system.h"
#include "utilstrencodings.h"
#include "validation.h"

#include "ptx/ptx_wallet.h"

#ifdef ENABLE_WALLET
#include "wallet/rpcwallet.h"
#include "wallet/wallet.h"
#endif // ENABLE_WALLET

#include <univalue.h>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

RecursiveMutex cs_ptx_secrets;
std::map<std::string, std::map<std::string, uint256>> g_ptx_local_secrets;

// GM-side BLS key share — defined in ptx_bls.cpp (extern declared in ptx_bls.h).
// Accessible here via the ptx_bls.h include above. No local definition needed.

// ---------------------------------------------------------------------------
// Exclude-list helpers
// ---------------------------------------------------------------------------

static void PTX_BuildExcludeLists(const UniValue& arr,
                                   std::vector<int64_t>& exc_ints,
                                   std::vector<std::string>& exc_txids)
{
    if (arr.isNull() || !arr.isArray()) return;
    for (size_t i = 0; i < arr.size(); i++) {
        const UniValue& v = arr[i];
        if (v.isNum()) {
            exc_ints.push_back(v.get_int64());
        } else if (v.isStr()) {
            const std::string& s = v.get_str();
            // 64-char hex string is a tx_id (256-bit hash)
            if (s.size() == 64) {
                exc_txids.push_back(s);
            } else {
                exc_ints.push_back((int64_t)atoll(s.c_str()));
            }
        }
    }
}

static std::set<int64_t> PTX_ResolveExclude(const UniValue& arr)
{
    std::set<int64_t> result;
    std::vector<int64_t> exc_ints;
    std::vector<std::string> exc_txids;
    PTX_BuildExcludeLists(arr, exc_ints, exc_txids);
    for (int64_t v : exc_ints) result.insert(v);
    if (!exc_txids.empty())
        LogPrintf("PTX: tx_id exclude resolution deferred to Phase 2\n");
    return result;
}

// ---------------------------------------------------------------------------
// DKG signing material (the REPOINT) — DEALER RETIRED (KDD-069).
//
// The roll signs ONLY with DKG material: group_pk and the Lagrange x
// (share_index) are READ from the committed CPTXQuorumRecord for the ACTIVE
// quorum at this height.  The trusted dealer (its central polynomial, its
// alphabetical index basis, and gm_bls_keyset fan-out) has been removed —
// there is no fallback.  When no ACTIVE, usable quorum exists for the height,
// ptx_roll hard-errors (KDD-069); it never mints or signs with dealer key
// material.  The DKG evaluates f(share_index) with share_index in
// CalculateQuorum SCORE order (KDD-052; assigned PTX_DKG_InitSession
// ptx_dkg.cpp:261, used PTX_DKG_GenerateLocalContrib :318) — the only index
// space that now exists (the alphabetical basis is structurally gone, KDD-052).
//
// NO CONSENSUS SURFACE: group_pk and share_index are READ from already
// committed/persisted data (CPTXQuorumRecord); CheckPTXDKGTx and the
// inclusion rule are untouched.
// ---------------------------------------------------------------------------

// The store query is the only impure part; selection itself is the pure
// PTX_SelectDKGSigningCtx (ptx_quorum_store.h) so it is unit-testable.
static PTXDKGSigningCtx PTX_LoadDKGSigningCtx(int nHeight)
{
    if (!ptxQuorumStore) return PTXDKGSigningCtx();
    // §7.4 (W2.5a): the SELECTION INPUT is the block hash at nHeight — the tip
    // this roll is anchored to.  Chain-derived and unforgeable by the caller;
    // deliberately NOT round_seed (caller_salt is free-form => grindable).
    const CBlockIndex* pindexSel = WITH_LOCK(cs_main, return chainActive[nHeight]);
    if (pindexSel == nullptr) return PTXDKGSigningCtx();
    return PTX_SelectDKGSigningCtx(ptxQuorumStore->GetActiveQuorumsAtHeight(nHeight),
                                   pindexSel->GetBlockHash());
}

// ---------------------------------------------------------------------------
// RPC: ptx_roll
// ---------------------------------------------------------------------------

UniValue ptx_roll(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 7 || request.params.size() > 7) {
        throw std::runtime_error(
            "ptx_roll count low high unique exclude game_id caller_salt\n"
            "\nRun a PTX commit-reveal round and return verifiable random results.\n"
            "\nArguments:\n"
            "1. count       (int)  Number of values to draw (>= 1)\n"
            "2. low         (int)  Minimum value, inclusive\n"
            "3. high        (int)  Maximum value, inclusive\n"
            "4. unique      (bool) Whether draws must be distinct\n"
            "5. exclude     (arr)  Integers or 64-char tx_id strings to skip\n"
            "6. game_id     (str)  Caller-defined game identifier\n"
            "7. caller_salt (str)  Caller entropy as hex\n"
            "\nResult:\n"
            "{\n"
            "  \"results\"        : [n, ...]\n"
            "  \"round_seed\"     : \"hex\"\n"
            "  \"quorum_sig\"     : \"hex\"\n"
            "  \"quorum_members\" : [\"id\", ...]\n"
            "  \"block_height\"   : n\n"
            "  \"tx_id\"          : \"hex\"\n"
            "}\n"
            + HelpExampleCli("ptx_roll", "1 1 100 false '[]' mygame 00aabbcc")
            + HelpExampleRpc("ptx_roll", "1, 1, 100, false, [], \"mygame\", \"00aabbcc\"")
        );
    }

    if (g_ptx_my_node_id.empty())
        throw JSONRPCError(RPC_MISC_ERROR, "PTX not enabled: set ptxnodeid= in config");

    int       count            = request.params[0].get_int();
    int64_t   low              = request.params[1].get_int64();
    int64_t   high             = request.params[2].get_int64();
    bool      unique           = request.params[3].get_bool();
    const UniValue& exc_arr    = request.params[4];
    std::string game_id        = request.params[5].get_str();
    std::string caller_salt_hex = request.params[6].get_str();

    if (count < 1)
        throw JSONRPCError(RPC_INVALID_PARAMS, "count must be >= 1");
    if (low > high)
        throw JSONRPCError(RPC_INVALID_PARAMS, "low must be <= high");
    // BUG-016: cast operands to uint64 BEFORE subtracting — signed (high-low) is UB and
    // -O2 eliminates the ==0 branch after the low<=high check above.  Unsigned arithmetic
    // wraps to 0 only at INT64_MIN..INT64_MAX, matching the crash site in PTX_SampleOne.
    if ((uint64_t)high - (uint64_t)low + 1ULL == 0)
        throw JSONRPCError(RPC_INVALID_PARAMS,
            "invalid range: high - low + 1 overflows (pool size 0)");
    if (!exc_arr.isArray())
        throw JSONRPCError(RPC_INVALID_PARAMS, "exclude must be a JSON array");
    size_t exc_int_count  = 0;
    size_t exc_txid_count = 0;
    for (size_t i = 0; i < exc_arr.size(); i++) {
        const UniValue& v = exc_arr[i];
        if (!v.isNum() && !v.isStr())
            throw JSONRPCError(RPC_INVALID_PARAMS, "exclude elements must be integers or 64-char hex tx_id strings");
        if (v.isStr() && v.get_str().size() != 64)
            throw JSONRPCError(RPC_INVALID_PARAMS, "exclude string elements must be 64-char hex tx_id");
        if (v.isStr()) ++exc_txid_count; else ++exc_int_count;
    }
    if (game_id.size() > 128)
        throw JSONRPCError(RPC_INVALID_PARAMS, "game_id too long (max 128 bytes)");
    if (count > 1000)
        throw JSONRPCError(RPC_INVALID_PARAMS, strprintf("count %d exceeds maximum 1000", count));
    // BUG-017: MAX_EXCLUDE_COUNT=512 (KDD design limit) was not enforced; up to 5000 accepted.
    if (exc_int_count + exc_txid_count > 512)
        throw JSONRPCError(RPC_INVALID_PARAMS,
            strprintf("exclude list too long: %zu items exceeds maximum 512",
                      exc_int_count + exc_txid_count));
    {
        // game_id + int-excludes (8b each) + txid-excludes (65b each) + results (8b each)
        // compete for the 9,525b variable extraPayload budget; 9,000b margin below the hard limit.
        size_t var_bytes = game_id.size() + exc_int_count * 8 + exc_txid_count * 65 + (size_t)count * 8;
        if (var_bytes > 9000)
            throw JSONRPCError(RPC_INVALID_PARAMS,
                strprintf("payload budget exceeded: %zub of 9000b available "
                          "(game_id=%zub + %zux8 int-excludes"
                          " + %zux65 txid-excludes + %dx8 results)",
                          var_bytes, game_id.size(), exc_int_count, exc_txid_count, count));
    }
    if (!caller_salt_hex.empty() && !IsHex(caller_salt_hex))
        throw JSONRPCError(RPC_INVALID_PARAMS, "caller_salt must be a hex string");

    uint32_t block_height = (uint32_t)chainActive.Height();
    uint256  prev_beacon  = PTX_GetLastBeacon();

    std::vector<unsigned char> caller_salt_bytes = ParseHex(caller_salt_hex);
    uint256 nonce = PTX_BuildNonce(prev_beacon, caller_salt_bytes);

    std::vector<int64_t>    exc_ints;
    std::vector<std::string> exc_txids;
    PTX_BuildExcludeLists(exc_arr, exc_ints, exc_txids);

    uint256     params_hash = PTX_HashParams((uint32_t)count, low, high, unique, exc_ints, exc_txids);
    uint256     round_seed  = PTX_BuildRoundSeed(game_id, block_height, {}, nonce, params_hash);
    std::string round_id    = PTX_MakeRoundId(game_id, block_height, params_hash);

    int n_nodes = (int)g_ptx_nodes.size();
    if (n_nodes < 1)
        throw JSONRPCError(RPC_MISC_ERROR, "PTX: no registered nodes");

    // DKG signing material for this height (KDD-069: the trusted dealer is
    // retired — this is the ONLY signing path).
    const PTXDKGSigningCtx dkg_ctx =
            PTX_LoadDKGSigningCtx((int)block_height);

    // ★ FAIL-CLOSED (KDD-069): an ACTIVE quorum exists but its signing material
    // is unusable (group_pk not 48 bytes, or fewer than `threshold` in_qual
    // members).  There is no dealer to fall back to — hard-error.
    if (dkg_ctx.quorum_present && !dkg_ctx.active) {
        throw JSONRPCError(RPC_MISC_ERROR,
            strprintf("PTX: ACTIVE quorum %s present but signing material unusable "
                      "(group_pk size or in_qual < threshold) — cannot sign "
                      "(dealer retired, KDD-069)",
                      dkg_ctx.quorum_hash.ToString()));
    }

    // ★ NO QUORUM (KDD-069): no ACTIVE quorum for this height, and the trusted
    // dealer is retired.  There is no signing path — hard-error.
    if (!dkg_ctx.active) {
        throw JSONRPCError(RPC_MISC_ERROR,
            strprintf("PTX: no ACTIVE quorum for height %d — cannot sign "
                      "(dealer retired, KDD-069)", (int)block_height));
    }

    // The quorum-scoped threshold (dkg_ctx.threshold = majority(formed_size),
    // ODC-036) — the SINGLE source read at the sig-collection / reconstruction
    // sites below.
    const int signing_threshold = dkg_ctx.threshold;

    // Signer set: the committed effective-QUAL members of the ACTIVE quorum.
    std::vector<std::string> member_ids = dkg_ctx.member_ids;
    LogPrintf("PTX roll: DKG signing material — quorum_hash=%s signers=%d (score-order share_index)\n",
              dkg_ctx.quorum_hash.ToString(), (int)member_ids.size());

    // Sort members deterministically.  NOTE: this orders the SIGNER LIST only —
    // the Lagrange x comes from share_index (KDD-052), never from this position.
    std::sort(member_ids.begin(), member_ids.end());

    // Initialise coordinator's round entry.
    {
        LOCK(cs_ptx_rounds);
        PTXCommitRevealRound round;
        round.round_id       = round_id;
        round.round_seed     = round_seed;
        round.threshold      = signing_threshold;
        round.quorum_members = member_ids;
        round.count          = (uint32_t)count;
        round.low            = low;
        round.high           = high;
        round.unique         = unique;
        round.exclude_integers = exc_ints;
        round.exclude_txids    = exc_txids;
        round.state          = PTXRoundState::COMMIT_PHASE;
        g_ptx_rounds[round_id] = round;
    }

    // DKG members already hold their ceremony-produced share (ptx_dkg.cpp:1338);
    // there is no key fan-out (KDD-069: the dealer's gm_bls_keyset path is gone).

    // BUG-032 2b-iii COMMIT-BEFORE-SIGN: build+fund+broadcast the PTXROLLCOMMIT
    // NOW, before FanOutSign. The quorum members' gm_bls_sign gates on seeing this
    // commitment in their mempool (2b), so payment is irrevocably committed before
    // any signature — and thus any result — exists. The settle will spend this
    // commitment's chain output (the 2c coin-chain).
    CPTXRollCommitPayload commit_payload;
    commit_payload.game_id          = game_id;
    commit_payload.nSeedHeight      = block_height;
    commit_payload.nExpiryHeight    = block_height;   // same-block window (2a mandate)
    commit_payload.caller_pubkey    = caller_salt_bytes;
    commit_payload.nonce            = nonce;
    commit_payload.ptx_params_hash  = params_hash;
    commit_payload.count            = (uint32_t)count;
    commit_payload.low              = low;
    commit_payload.high             = high;
    commit_payload.unique           = unique;
    commit_payload.exclude_integers = exc_ints;
    commit_payload.exclude_txids    = exc_txids;
    commit_payload.round_seed       = round_seed;
    commit_payload.quorum_hash      = dkg_ctx.quorum_hash;
    COutPoint chain_outpoint;
    // KDD-088 direct-attach: keep the serialized commitment so every sign request
    // can carry it. `commit_raw_hex` outlives PTX_FanOutSign (same frame), which
    // holds a pointer to it for the life of the round.
    std::string commit_raw_hex;
    const std::string commit_txid = PTX_BuildRollCommitment(commit_payload, chain_outpoint,
                                                            &commit_raw_hex);
    LogPrintf("PTX roll: commitment %s broadcast BEFORE signing (round_seed=%s)\n",
              commit_txid, round_seed.ToString());

    // Collect partial BLS signatures from each quorum member.
    // stop-at-threshold: FanOutSign returns as soon as `signing_threshold` partials
    // are collected — recovery needs only t, and waiting for the rest is latency
    // (gating on the slowest members under staggered commitment propagation).
    auto partial_sigs_raw = PTX_FanOutSign(round_id, round_seed, dkg_ctx.quorum_hash,
                                           member_ids, (size_t)signing_threshold,
                                           dkg_ctx.member_protx, // A: live-DGM addr resolution
                                           commit_raw_hex);      // KDD-088: direct-attach

    // Collect blst partial signatures and 1-indexed polynomial positions.
    std::vector<std::vector<uint8_t>> bls_sigs;
    std::vector<int>                  bls_indices;
    std::vector<std::string>          signed_nodes;
    std::vector<std::string>          withheld;

    for (const auto& nid : member_ids) {
        auto it = partial_sigs_raw.find(nid);
        // ★ THE LAGRANGE x: score-order share_index (KDD-052) — the point the
        // share's polynomial was actually evaluated at.  Using the wrong space
        // yields wrong lambdas and a signature that does not verify.
        auto xit = dkg_ctx.share_index.find(nid);
        int idx = (xit == dkg_ctx.share_index.end()) ? 0 : xit->second;
        if (it != partial_sigs_raw.end() && idx > 0 &&
            (int)it->second.size() == PTX_SIG_BYTES) {
            bls_sigs.push_back(it->second);
            bls_indices.push_back(idx);
            signed_nodes.push_back(nid);
        } else {
            withheld.push_back(nid);
        }
    }

    if ((int)bls_sigs.size() < signing_threshold)
        throw JSONRPCError(RPC_MISC_ERROR,
            strprintf("PTX: BLS threshold not met: got %d/%d",
                      (int)bls_sigs.size(), signing_threshold));

    // Lagrange recovery from the first t partial sigs (signing_threshold = the
    // quorum's t on the DKG path, the registry threshold on the dealer path).
    std::vector<std::vector<uint8_t>> thresh_sigs(bls_sigs.begin(),
                                                   bls_sigs.begin() + signing_threshold);
    std::vector<int> thresh_indices(bls_indices.begin(),
                                    bls_indices.begin() + signing_threshold);

    uint8_t combined_sig[PTX_SIG_BYTES];
    if (!PTX_BLS_Recover(thresh_indices, thresh_sigs, combined_sig))
        throw JSONRPCError(RPC_MISC_ERROR, "PTX: BLS threshold signature recovery failed");

    // The verification key: the COMMITTED group_pk from the quorum record
    // (ptx_quorum_store.h) — the one a third party verifies against (KDD-049).
    uint8_t group_pk_bytes[48];
    memcpy(group_pk_bytes, dkg_ctx.group_pk.data(), 48);
    // SG-3 observability: the exact verification key, so a run can assert it is
    // byte-equal to the selected quorum's committed group_pk (predicate A) —
    // beside the "DKG signing material" line so source + key are one grep.
    LogPrintf("PTX roll: verify group_pk=%s source=dkg\n",
              HexStr(Span<const uint8_t>(group_pk_bytes, 48)));
    if (!PTX_BLS_Verify(group_pk_bytes, round_seed, combined_sig))
        throw JSONRPCError(RPC_MISC_ERROR, "PTX: BLS threshold signature verification failed");

    std::vector<uint8_t> threshold_sig_bytes(combined_sig, combined_sig + PTX_SIG_BYTES);
    uint256 beacon = PTX_BLS_SigToBeacon(combined_sig);

    // Update round state in coordinator's record.
    {
        LOCK(cs_ptx_rounds);
        auto& round = g_ptx_rounds[round_id];
        for (const auto& nid : signed_nodes)
            round.bls_partial_sigs[nid] = partial_sigs_raw[nid];
        round.threshold_sig = threshold_sig_bytes;
        round.beacon        = beacon;
        round.state         = PTXRoundState::RESOLVED;
    }

    // PoSe scoring is now applied exclusively via ProcessSpecialTxsInBlock when
    // PTXSESS transactions confirm in a block (specialtx_validation.cpp). Applying
    // RecordHonestParticipation here (on the ptx_roll caller only) would double-count
    // tickets when the PTXSESS confirms, causing the caller to have more tickets than
    // validators, making PTX_SelectWinner return different winners on different nodes
    // and causing P10 rejections at every settlement boundary. The block-processing
    // path is the sole consensus-consistent update for pose tracker state.
    // RecordWithhold is similarly deferred; proper withhold consensus handling via
    // block processing is a future iteration item.

    std::set<int64_t> exclude_set = PTX_ResolveExclude(exc_arr);
    std::vector<int64_t> results  = PTX_MapBeacon(beacon, (uint32_t)count, low, high, unique, exclude_set);

    PTX_SetLastBeacon(beacon);

    // Assemble payload.
    CProbabilisticTxPayload payload;
    payload.game_id          = game_id;
    payload.nSeedHeight      = block_height;
    payload.nExpiryHeight    = block_height;
    payload.nonce            = nonce;
    payload.ptx_params_hash  = params_hash;
    payload.count            = (uint32_t)count;
    payload.low              = low;
    payload.high             = high;
    payload.unique           = unique;
    payload.exclude_integers = exc_ints;
    payload.exclude_txids    = exc_txids;
    payload.round_seed       = round_seed;
    payload.beacon           = beacon;
    payload.results          = results;
    payload.quorum_members   = member_ids;
    payload.quorum_sig       = threshold_sig_bytes;
    payload.quorum_hash      = dkg_ctx.quorum_hash;
    // quorum_sig_hash = SHA256(threshold_sig); non-null satisfies existing validation.
    {
        CSHA256 qh;
        qh.Write(threshold_sig_bytes.data(), threshold_sig_bytes.size());
        qh.Finalize(payload.quorum_sig_hash.begin());
    }

    PTXCommitRevealRound round_copy;
    { LOCK(cs_ptx_rounds); round_copy = g_ptx_rounds[round_id]; }
    // BUG-032 2b-iii: the settle spends the commitment's chain output (coin-chain).
    std::string txid = PTX_AutoCommit(round_copy, payload, chain_outpoint);

    UniValue ret(UniValue::VOBJ);
    UniValue res_arr(UniValue::VARR);
    for (int64_t v : results) res_arr.push_back(v);
    ret.pushKV("results",        res_arr);
    ret.pushKV("round_seed",     round_seed.GetHex());
    ret.pushKV("quorum_sig",     HexStr(threshold_sig_bytes));
    ret.pushKV("quorum_sig_hash", payload.quorum_sig_hash.GetHex());
    UniValue qm_arr(UniValue::VARR);
    for (const auto& nid : member_ids) qm_arr.push_back(nid);
    ret.pushKV("quorum_members", qm_arr);
    // SG-3 observability: which quorum signed — so predicate B (selection
    // correct) is assertable in-harness from the RPC response, not by
    // log-scrape.  Post-KDD-069 the only signing path is DKG, so signing_source
    // is always "dkg" (retained for client/response-schema stability).
    ret.pushKV("signing_source", "dkg");
    ret.pushKV("quorum_hash",    dkg_ctx.quorum_hash.ToString());
    ret.pushKV("block_height",   (int64_t)block_height);
    ret.pushKV("tx_id",          txid);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: gm_commit  (coordinator → node)
// ---------------------------------------------------------------------------

UniValue gm_commit(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 4) {
        throw std::runtime_error(
            "gm_commit round_id round_seed_hex members_json commitment_hex\n"
            "\nRecord a quorum commitment for this node (called by the coordinator).\n"
            "\nArguments:\n"
            "1. round_id       (str)   Round identifier\n"
            "2. round_seed_hex (str)   Hex round seed\n"
            "3. members_json   (array) Sorted quorum member node_id list\n"
            "4. commitment_hex (str)   Hex commitment for this node\n"
            + HelpExampleRpc("gm_commit", "\"rid\", \"aabb...\", [\"n1\"], \"ccdd...\"")
        );
    }

    std::string        round_id    = request.params[0].get_str();
    uint256            round_seed  = uint256S(request.params[1].get_str());
    const UniValue&    marr        = request.params[2].get_array();
    uint256            commitment  = uint256S(request.params[3].get_str());
    const std::string& committer   = g_ptx_my_node_id;

    bool ok = false;
    {
        LOCK(cs_ptx_rounds);
        if (g_ptx_rounds.count(round_id) == 0) {
            PTXCommitRevealRound r;
            r.round_id   = round_id;
            r.round_seed = round_seed;
            r.threshold  = 3;
            for (size_t i = 0; i < marr.size(); i++)
                r.quorum_members.push_back(marr[i].get_str());
            std::sort(r.quorum_members.begin(), r.quorum_members.end());
            g_ptx_rounds[round_id] = r;
        }
        ok = PTX_SubmitCommit(g_ptx_rounds[round_id], committer, commitment);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("accepted", ok);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: gm_reveal  (coordinator → node)
// ---------------------------------------------------------------------------

UniValue gm_reveal(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2) {
        throw std::runtime_error(
            "gm_reveal round_id secret_hex\n"
            "\nRecord a quorum reveal for this node (called by the coordinator).\n"
            "\nArguments:\n"
            "1. round_id   (str) Round identifier\n"
            "2. secret_hex (str) Hex secret\n"
            + HelpExampleRpc("gm_reveal", "\"rid\", \"aabb...\"")
        );
    }

    std::string round_id = request.params[0].get_str();
    uint256     secret   = uint256S(request.params[1].get_str());

    bool ok = false;
    {
        LOCK(cs_ptx_rounds);
        if (g_ptx_rounds.count(round_id) == 0) {
            UniValue ret(UniValue::VOBJ);
            ret.pushKV("accepted", false);
            ret.pushKV("resolved", false);
            return ret;
        }
        ok = PTX_SubmitReveal(g_ptx_rounds[round_id], g_ptx_my_node_id, secret);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("accepted", ok);
    ret.pushKV("resolved", false);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: gm_bls_sign  (coordinator → GM)
// ---------------------------------------------------------------------------

UniValue gm_bls_sign(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2) {
        throw std::runtime_error(
            "gm_bls_sign round_seed_hex quorum_hash\n"
            "\nSign round_seed with this node's CURRENT BLS key share for the named\n"
            "quorum and return the partial signature (KDD-070 P1: keyed selection).\n"
            "\nArguments:\n"
            "1. round_seed_hex (str) 64-char hex round seed\n"
            "2. quorum_hash    (str) 64-char hex quorum_hash — selects which share signs\n"
            "3. commit_hex     (str, optional) raw PTXROLLCOMMIT for this round (KDD-088\n"
            "                  direct-attach). Accepted into the local mempool via the\n"
            "                  normal path if not already present, then the unchanged\n"
            "                  BUG-032 gate decides. Omitted => gossip-only delivery.\n"
            "\nResult:\n"
            "{\n"
            "  \"sig_hex\" : \"hex\"  96-byte BLS partial signature\n"
            "}\n"
            + HelpExampleRpc("gm_bls_sign", "\"aabb...\", \"ccdd...\"")
        );
    }

    std::string seed_hex = request.params[0].get_str();
    if (!IsHex(seed_hex))
        throw JSONRPCError(RPC_INVALID_PARAMS, "round_seed_hex must be a hex string");
    std::string qh_hex = request.params[1].get_str();
    if (!IsHex(qh_hex))
        throw JSONRPCError(RPC_INVALID_PARAMS, "quorum_hash must be a hex string");

    uint256 round_seed  = uint256S(seed_hex);
    uint256 quorum_hash = uint256S(qh_hex);

    // ── KDD-088 DIRECT-ATTACH ────────────────────────────────────────────────
    // Optional third argument: the round's PTXROLLCOMMIT itself. Hand it to the
    // mempool module, which accepts it via the NORMAL path if we have not already
    // seen it via gossip; the gate below is UNCHANGED and still decides. The
    // helper is transport-agnostic and never throws — a bad attachment must not
    // fail louder than no attachment at all, so we fall through to the gate's
    // ordinary retryable refusal. Full rationale (why accept-into-mempool rather
    // than verify-only IS the security property) lives on the declaration.
    if (request.params.size() > 2 && !request.params[2].isNull()) {
        std::string attach_err;
        if (!PTX_AcceptAttachedCommitment(request.params[2].get_str(),
                                          round_seed, quorum_hash, attach_err)) {
            LogPrintf("PTX gm_bls_sign: attachment not usable (%s) — deferring to gate\n",
                      attach_err);
        }
    }

    // BUG-032 2b: route signing through the fund-then-sign gate. It refuses to
    // reveal (sign) unless a REAL PTXROLLCOMMIT for this exact (round_seed,
    // quorum_hash) is present in the local mempool — payment-before-reveal, and
    // by binding quorum_hash, the signing-path close of the quorum-shop (BUG-033).
    // KDD-070 P1 CURRENT-share selection now lives inside the gate. It also
    // reports whether a refusal is RETRYABLE (commitment not seen yet —
    // propagation delay) versus terminal (no share / sign failure).
    uint8_t sig_buf[PTX_SIG_BYTES];
    std::string sign_err;
    bool retryable = false;
    if (!PTX_SignRoundIfCommitted(round_seed, quorum_hash, sig_buf, sign_err, &retryable)) {
        // ODC-064: give every refusal branch a voice, naming the quorum + reason.
        LogPrintf("PTX gm_bls_sign: %s node=%s quorum=%s reason=%s\n",
                  retryable ? "RETRY" : "FAILED",
                  g_ptx_my_node_id, quorum_hash.ToString(), sign_err);
        // wait-not-reject: a not-yet-propagated commitment is RETRYABLE, not
        // terminal — the coordinator's retry budget bounds the wait, so a
        // legitimate roll is not failed by ordinary network delay.
        throw JSONRPCError(retryable ? RPC_PTX_COMMITMENT_NOT_SEEN : RPC_MISC_ERROR,
                           sign_err);
    }

    LogPrintf("PTX gm_bls_sign: node=%s sig[0..3]=%02x%02x%02x%02x\n",
              g_ptx_my_node_id, sig_buf[0],sig_buf[1],sig_buf[2],sig_buf[3]);

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("sig_hex", HexStr(Span<const uint8_t>(sig_buf, PTX_SIG_BYTES)));
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: ptx_debug_setnodefailmode
// ---------------------------------------------------------------------------

UniValue ptx_debug_setnodefailmode(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 2) {
        throw std::runtime_error(
            "ptx_debug_setnodefailmode target_node_id mode\n"
            "\nSimulate a node fail mode for testing (coordinator side only).\n"
            "\nTEST-GATED: regtest and ptxbea ONLY -- hard error on every other network.\n"
            "\nArguments:\n"
            "1. target_node_id (str) Node to target\n"
            "2. mode           (str) abstain | withhold | invalid_commit | clear\n"
            + HelpExampleRpc("ptx_debug_setnodefailmode", "\"node1\", \"withhold\"")
        );
    }

    // ★ NETWORK GATE ADDED 2026-08-21. This RPC had NO gate at all -- it was
    // callable on every network including MAINNET. It perturbs ceremony messaging
    // (abstain / withhold / invalid_commit), so an authenticated caller could
    // degrade their own node's participation and their own PoSe score. It is a
    // debug-injection tool and belongs behind the same gate as its two siblings,
    // ptx_debug_ptxdkgpopulate (:901-903) and ptx_debug_selectquorum (:1711-1713).
    if (!Params().IsRegTestNet() && !Params().IsPTXBeaTestNet()) {
        throw JSONRPCError(RPC_MISC_ERROR,
            "ptx_debug_setnodefailmode is only available on regtest or the ptxbea test network");
    }

    std::string target = request.params[0].get_str();
    std::string mode   = request.params[1].get_str();

    {
        LOCK(cs_ptx_failmodes);
        if (mode == "clear")
            g_ptx_node_failmodes.erase(target);
        else
            g_ptx_node_failmodes[target] = mode;
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("target", target);
    ret.pushKV("mode",   mode);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: ptx_debug_ptxdkgpopulate (W1.3 Package 3 C5 — test-gated injection driver)
// ---------------------------------------------------------------------------

// Server-side PTXDKG builder for the debug RPC.  Premit sigs are REAL BLS
// signatures from THROWAWAY keys: CBLSSignature must hold a valid G2 element
// to survive (de)serialisation and the structural null-sig check, so "dummy"
// cannot mean junk bytes.  Structurally valid, never operator-key valid —
// sufficient for every bad-anchor battery row, because V1/V2/V3 fire in
// CheckPTXDKGTx before the V6–V8 premit-sig checks.  The real-operator-key
// mode (accept path) lands at C6.
static CMutableTransaction PTX_Debug_BuildPTXDKGTxFromSpec(const uint256& quorum_hash,
                                                           int formation_height,
                                                           const uint8_t group_pk[48],
                                                           const uint256& vvec_hash,
                                                           int n_members,
                                                           int n_premits)
{
    PTXDKGPayload pl;
    pl.quorum_hash      = quorum_hash;
    memcpy(pl.group_pk_bytes, group_pk, 48);
    pl.vvec_hash        = vvec_hash;
    pl.formation_height = formation_height;
    for (int i = 0; i < n_members; i++) {
        pl.member_node_ids.push_back(strprintf("dbggm%d:8080", i));
    }
    for (int i = 0; i < n_premits; i++) {
        PTXDKGPhase4Msg p;
        p.quorum_hash = quorum_hash;
        std::vector<unsigned char> pb(32, 0);
        pb[0] = (unsigned char)(i + 1);
        pb[1] = 0xDB;
        p.proTxHash = uint256(pb);
        memcpy(p.group_pk_bytes, group_pk, 48);
        p.vvec_hash = vvec_hash;
        CBLSSecretKey throwaway;
        throwaway.MakeNewKey();
        // KDD-072 P-b2: sign over the payload's predecessor view (zero for the
        // v1 debug shape) — keeps this builder lockstep with the validator's
        // preimage if a rotation arm is ever added here (KDD-073 site #3).
        p.sig = throwaway.Sign(p.GetSignHash(pl.predecessor_quorum_hash));
        pl.premit_commitments[p.proTxHash] = p;
    }
    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType    = CTransaction::TxType::PTXDKG;
    SetTxPayload(tx, pl);
    return tx;
}

// Real-operator-key builder (W2.1 C0 — the C6 "operator_keys" mode).  Builds a
// CONNECT-VALID payload with no ceremony and no transport: members come from the
// canonical selection at the anchor (PTX_DKG_SelectQuorumFromList — the same
// KDD-060 core CheckPTXDKGTx V5 runs, so construction and validation agree by
// construction) and premits are signed with the SUPPLIED operator secrets,
// matched to selected members by registered pubKeyOperator.  `exclude` drops
// members from the committed list (under-strength payloads, KDD-061);
// `members_override`, when present, replaces the committed member list verbatim
// (containment falsification rows) — premits still come from real survivors.
static CMutableTransaction PTX_Debug_BuildPTXDKGTxFromChain(const uint256& quorum_hash,
                                                            int formation_height,
                                                            const uint8_t group_pk[48],
                                                            const uint256& vvec_hash,
                                                            const std::vector<CBLSSecretKey>& operator_sks,
                                                            const std::set<std::string>& exclude,
                                                            const std::vector<std::string>* members_override,
                                                            int n_premits,
                                                            const uint256& predecessor)
{
    CDeterministicGMList dgmList;
    // SG-1a: the builder mirrors V5's CONSENSUS pool (eligible minus active,
    // PTX_Formation_BuildPool) — with the V5 swap, a raw-list selection could
    // never build an acceptable PTXDKG once any ACTIVE quorum exists (its
    // premit signers would include excluded members).
    // KDD-072 P-b3b rotation arm: with a predecessor, the members come from
    // P-b3a's SHARED resolver (the exact function V12 and the store guard
    // run) — the drill tests the real path, not a debug facsimile.
    std::vector<CPTXQuorumRecord> activeAtAnchor;
    CDeterministicGMList listForm;
    CPTXQuorumRecord predRec;
    {
        LOCK(cs_main);
        const CBlockIndex* pindexQuorum = LookupBlockIndex(quorum_hash);
        if (pindexQuorum == nullptr) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "operator_keys mode requires a real anchor: quorum_hash not found");
        }
        dgmList = deterministicGMManager->GetListForBlock(pindexQuorum);
        if (ptxQuorumStore) {
            activeAtAnchor =
                ptxQuorumStore->GetActiveQuorumsAtHeight(pindexQuorum->nHeight);
        }
        if (!predecessor.IsNull()) {
            if (ptxQuorumStore == nullptr ||
                !ptxQuorumStore->GetQuorumRecord(predecessor, predRec)) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    "predecessor_quorum_hash: no quorum record found");
            }
            const CBlockIndex* pindexPred = LookupBlockIndex(predRec.quorum_hash);
            if (pindexPred == nullptr) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    "predecessor_quorum_hash: formation anchor not found");
            }
            listForm = deterministicGMManager->GetListForBlock(pindexPred);
        }
    }
    std::vector<CDeterministicGMCPtr> quorum11;
    if (!predecessor.IsNull()) {
        std::string err;
        if (!PTX_DKG_ResolveRotationQuorum(predRec, dgmList, listForm, quorum11, err)) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                strprintf("rotation members unresolvable: %s", err));
        }
    } else {
        const CDeterministicGMList formationPool =
            PTX_Formation_BuildPool(dgmList, activeAtAnchor);
        quorum11 = PTX_DKG_SelectQuorumFromList(formationPool, quorum_hash);
    }
    if (quorum11.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "no eligible GMs at anchor");
    }

    std::map<std::vector<uint8_t>, CBLSSecretKey> sk_by_pk;
    for (const CBLSSecretKey& sk : operator_sks) {
        sk_by_pk[sk.GetPublicKey().ToByteVector()] = sk;
    }

    PTXDKGPayload pl;
    // KDD-072 P-b3b: a rotation is v2 and names its predecessor; the premit
    // signing below flips to the P-b2 predecessor preimage AUTOMATICALLY
    // (both sites already sign GetSignHash(pl.predecessor_quorum_hash)).
    pl.nVersion = predecessor.IsNull() ? PTXDKGPayload::CURRENT_VERSION
                                       : PTXDKGPayload::ROTATION_VERSION;
    pl.predecessor_quorum_hash = predecessor;
    pl.quorum_hash      = quorum_hash;
    memcpy(pl.group_pk_bytes, group_pk, 48);
    pl.vvec_hash        = vvec_hash;
    pl.formation_height = formation_height;

    // Survivors: selection order minus excluded — committed list keeps the
    // KDD-052/060 score order; exclusion leaves GAPPED share indices (KDD-061).
    std::vector<CDeterministicGMCPtr> survivors;
    for (const auto& dgm : quorum11) {
        if (exclude.count(dgm->pdgmState->node_id)) continue;
        survivors.push_back(dgm);
    }
    if (members_override != nullptr) {
        pl.member_node_ids = *members_override;
    } else {
        for (const auto& dgm : survivors) {
            pl.member_node_ids.push_back(dgm->pdgmState->node_id);
        }
    }

    int built = 0;
    for (const auto& dgm : survivors) {
        if (built >= n_premits) break;
        const CBLSPublicKey op_pk = dgm->pdgmState->pubKeyOperator.Get();
        auto it = sk_by_pk.find(op_pk.ToByteVector());
        if (it == sk_by_pk.end()) continue; // no secret supplied for this member
        PTXDKGPhase4Msg p;
        p.quorum_hash = quorum_hash;
        p.proTxHash   = dgm->proTxHash;
        memcpy(p.group_pk_bytes, group_pk, 48);
        p.vvec_hash   = vvec_hash;
        p.sig         = it->second.Sign(p.GetSignHash(pl.predecessor_quorum_hash)); // KDD-072 P-b2: payload view (zero = v1)
        pl.premit_commitments[p.proTxHash] = p;
        built++;
    }
    if (built < n_premits) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            strprintf("only %d of %d premits buildable: supplied operator_keys match "
                      "%d selected survivors", built, n_premits, built));
    }

    CMutableTransaction tx;
    tx.nVersion = CTransaction::TxVersion::SAPLING;
    tx.nType    = CTransaction::TxType::PTXDKG;
    SetTxPayload(tx, pl);
    return tx;
}

UniValue ptx_debug_ptxdkgpopulate(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() < 1 || request.params.size() > 2) {
        throw std::runtime_error(
            "ptx_debug_ptxdkgpopulate payload ( force )\n"
            "\nTEST-GATED: regtest and ptxbea ONLY — hard error on every other network.\n"
            "Build a PTXDKG special transaction server-side from an explicit payload\n"
            "description and populate the pending block-inject slot (W1.3 P3 C5 driver).\n"
            "Without operator_keys: premit signatures are throwaway-key BLS sigs —\n"
            "structurally valid, never operator-key valid (bad-anchor battery rows).\n"
            "With operator_keys (W2.1 C0): members = the canonical selection at the\n"
            "anchor (KDD-060 core), premits signed by the supplied operator secrets —\n"
            "a CONNECT-VALID payload, no ceremony needed.\n"
            "\nArguments:\n"
            "1. payload   (object, required)\n"
            "     {\n"
            "       \"quorum_hash\": \"hex\",       (string, required) formation anchor block hash\n"
            "       \"formation_height\": n,      (numeric, required) anchor height cross-check\n"
            "       \"group_pk\": \"hex|generate\", (string, optional, default \"generate\") 48-byte\n"
            "                                     compressed G1, or generate a throwaway key\n"
            "       \"vvec_hash\": \"hex\",         (string, optional, default 5e..5e) 32-byte hash\n"
            "       \"premits\": n,               (numeric, optional, default 6) premit count\n"
            "       \"members\": n,               (numeric, optional, default 11) member count (fake mode only)\n"
            "       \"operator_keys\": [\"bls-sk-..\", ..]  (array, optional) REAL mode: members from the\n"
            "                                     canonical selection at the anchor; premits signed by\n"
            "                                     these operator secrets (bech32, bls::DecodeSecret)\n"
            "       \"predecessor_quorum_hash\": \"hex\" (string, optional, real mode) KDD-072 P-b3b:\n"
            "                                     build a v2 ROTATION of this quorum — members from the\n"
            "                                     P-b3a shared resolver, premits over the predecessor\n"
            "                                     sign-hash, nVersion=2\n"
            "       \"exclude\": [\"node_id\", ..]  (array, optional, real mode) drop these members from\n"
            "                                     the committed list (under-strength / gapped-index rows)\n"
            "       \"members_override\": [..]     (array, optional, real mode) replace the committed\n"
            "                                     member list verbatim (containment falsification rows)\n"
            "       \"build_only\": bool          (optional) build + return tx_hex, do NOT touch the slot\n"
            "     }\n"
            "2. force     (boolean, optional, default false) E-1: bypass BOTH populate-time\n"
            "             guards (refuse-while-set + validate-before-inject) and seat the tx\n"
            "             directly, so a bad payload reaches the assembler and the\n"
            "             generate-time reject is observable. The production populate path\n"
            "             stays unconditionally guarded.\n"
            "\nResult:\n"
            "{ \"txid\": \"hex\", \"tx_hex\": \"hex\", \"force\": bool, \"populated\": true }\n"
            + HelpExampleRpc("ptx_debug_ptxdkgpopulate",
                             "{\"quorum_hash\":\"00..\",\"formation_height\":100}, false")
        );
    }

    // NET-GATE — the load-bearing safety property, checked before any
    // parameter read or state touch.  This RPC feeds BLOCK PRODUCTION (the
    // pending slot is read by CreateNewBlock), so it must be impossible to
    // run on a network whose blocks matter.  HARD allowlist on the chainparams
    // predicate — fails CLOSED for main, public testnet, ptxtestnet and any
    // future network.  Deliberately stronger than the ptx_debug_setnodefailmode
    // precedent (which has no gate at all): failmode only perturbs ceremony
    // messaging; this touches consensus-facing block assembly.
    if (!Params().IsRegTestNet() && !Params().IsPTXBeaTestNet()) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND,
            "ptx_debug_ptxdkgpopulate is only available on regtest or the ptxbea test network");
    }

    const UniValue& spec = request.params[0].get_obj();
    const bool fForce = request.params.size() > 1 && !request.params[1].isNull()
                        && request.params[1].get_bool();

    // W2.1 C0: real-operator-key premit mode (the mode C5 stubbed as "lands at C6").
    std::vector<CBLSSecretKey> operator_sks;
    const UniValue& v_ok = find_value(spec, "operator_keys");
    if (!v_ok.isNull()) {
        for (size_t i = 0; i < v_ok.size(); i++) {
            auto opKey = bls::DecodeSecret(Params(), v_ok[i].get_str());
            if (!opKey) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                    strprintf("operator_keys[%u] is not a valid BLS secret", (unsigned)i));
            }
            operator_sks.push_back(*opKey);
        }
        if (operator_sks.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "operator_keys must be non-empty when present");
        }
    }
    std::set<std::string> exclude;
    const UniValue& v_ex = find_value(spec, "exclude");
    if (!v_ex.isNull()) {
        if (operator_sks.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "exclude requires operator_keys mode");
        }
        for (size_t i = 0; i < v_ex.size(); i++) exclude.insert(v_ex[i].get_str());
    }
    // KDD-072 P-b3b rotation arm: optional predecessor (real mode only).
    uint256 predecessor_qh;
    const UniValue& v_pred = find_value(spec, "predecessor_quorum_hash");
    if (!v_pred.isNull()) {
        if (operator_sks.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                "predecessor_quorum_hash requires operator_keys mode");
        }
        predecessor_qh = uint256S(v_pred.get_str());
        if (predecessor_qh.IsNull()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "predecessor_quorum_hash must be non-zero");
        }
    }
    std::vector<std::string> members_override;
    bool fMembersOverride = false;
    const UniValue& v_mo = find_value(spec, "members_override");
    if (!v_mo.isNull()) {
        if (operator_sks.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "members_override requires operator_keys mode");
        }
        fMembersOverride = true;
        for (size_t i = 0; i < v_mo.size(); i++) members_override.push_back(v_mo[i].get_str());
    }

    const UniValue& v_qh = find_value(spec, "quorum_hash");
    const UniValue& v_fh = find_value(spec, "formation_height");
    if (v_qh.isNull() || v_fh.isNull()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
            "payload requires quorum_hash and formation_height");
    }
    const uint256 quorum_hash = uint256S(v_qh.get_str());
    const int formation_height = v_fh.get_int();

    // vvec_hash: explicit hex or the fixed default.
    uint256 vvec_hash = uint256S("5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e5e");
    const UniValue& v_vh = find_value(spec, "vvec_hash");
    if (!v_vh.isNull()) vvec_hash = uint256S(v_vh.get_str());

    // group_pk: "generate" (throwaway key — a valid compressed G1, which the
    // structural decompress check requires) or explicit 96-hex-char bytes.
    uint8_t group_pk[48];
    std::string strGpk = "generate";
    const UniValue& v_gpk = find_value(spec, "group_pk");
    if (!v_gpk.isNull()) strGpk = v_gpk.get_str();
    if (strGpk == "generate") {
        CBLSSecretKey throwaway;
        throwaway.MakeNewKey();
        const std::vector<uint8_t> pk = throwaway.GetPublicKey().ToByteVector();
        if (pk.size() != 48) {
            throw JSONRPCError(RPC_INTERNAL_ERROR, "generated group_pk is not 48 bytes");
        }
        memcpy(group_pk, pk.data(), 48);
    } else {
        if (!IsHex(strGpk) || strGpk.size() != 96) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "group_pk must be \"generate\" or 96 hex chars");
        }
        const std::vector<unsigned char> raw = ParseHex(strGpk);
        memcpy(group_pk, raw.data(), 48);
    }

    const UniValue& v_pre = find_value(spec, "premits");
    const UniValue& v_mem = find_value(spec, "members");
    const int n_premits = v_pre.isNull() ? 6 : v_pre.get_int();
    const int n_members = v_mem.isNull() ? 11 : v_mem.get_int();

    const CMutableTransaction mtx = operator_sks.empty()
        ? PTX_Debug_BuildPTXDKGTxFromSpec(
              quorum_hash, formation_height, group_pk, vvec_hash, n_members, n_premits)
        : PTX_Debug_BuildPTXDKGTxFromChain(
              quorum_hash, formation_height, group_pk, vvec_hash, operator_sks,
              exclude, fMembersOverride ? &members_override : nullptr, n_premits,
              predecessor_qh);
    const CTransactionRef tx = MakeTransactionRef(mtx);

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("txid",   tx->GetHash().GetHex());
    ret.pushKV("tx_hex", EncodeHexTx(*tx));
    // Real mode: surface the committed member list (selection order) so
    // battery rows can learn the canonical selection without decoding tx_hex.
    if (!operator_sks.empty()) {
        PTXDKGPayload built;
        if (GetTxPayload(*tx, built)) {
            UniValue mv(UniValue::VARR);
            for (const std::string& nid : built.member_node_ids) mv.push_back(nid);
            ret.pushKV("member_node_ids", mv);
        }
    }

    // BUILD_ONLY (C6): return the serialized tx WITHOUT touching the pending
    // slot — the mempool-rejection (F-5) and two-per-block (C3-invocation) rows
    // need a raw PTXDKG to feed sendrawtransaction / a crafted block, not a
    // populated slot.
    const UniValue& v_bo = find_value(spec, "build_only");
    if (!v_bo.isNull() && v_bo.get_bool()) {
        ret.pushKV("build_only", true);
        ret.pushKV("populated",  false);
        return ret;
    }

    if (fForce) {
        PTX_DKG_Commitments_ForceAdd(tx);
    } else {
        CValidationState state;
        if (!PTX_DKG_Commitments_AddAndRelay(tx, state)) {
            throw JSONRPCError(RPC_VERIFY_REJECTED,
                strprintf("populate refused: %s", state.GetRejectReason()));
        }
    }

    ret.pushKV("force",     fForce);
    ret.pushKV("populated", true);
    return ret;
}

// ---------------------------------------------------------------------------
// RPC: ptx_quorum_info (W2.1 C1 — read-only registry observation port)
// ---------------------------------------------------------------------------

static UniValue QuorumRecordToJson(const CPTXQuorumRecord& rec)
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("version",           (int)rec.nVersion);
    ret.pushKV("quorum_hash",       rec.quorum_hash.ToString());
    ret.pushKV("formation_height",  rec.formation_height);
    ret.pushKV("group_pk",          HexStr(rec.group_pk_bytes));
    ret.pushKV("vvec_hash",         rec.vvec_hash.ToString());
    ret.pushKV("formed_size",       (int)rec.formed_size);
    ret.pushKV("completed_size",    (int)rec.completed_size);
    switch (static_cast<PTXQuorumState>(rec.state)) {
        case PTXQuorumState::FORMING:   ret.pushKV("state", "forming");   break;
        case PTXQuorumState::ACTIVE:    ret.pushKV("state", "active");    break;
        case PTXQuorumState::SUPERSEDED: ret.pushKV("state", "superseded"); break; // KDD-063 repurpose (P-b4)
        case PTXQuorumState::DISBANDED: ret.pushKV("state", "disbanded"); break;
        default:                        ret.pushKV("state", strprintf("unknown(%d)", rec.state));
    }
    ret.pushKV("provenance",        (int)rec.provenance);
    ret.pushKV("accepted_txid",     rec.accepted_txid.ToString());
    ret.pushKV("mined_block_hash",  rec.mined_block_hash.ToString());
    ret.pushKV("mined_height",      rec.mined_height);
    ret.pushKV("last_rotation_height", rec.last_rotation_height);
    ret.pushKV("drift_offset",      rec.drift_offset);
    ret.pushKV("consecutive_inquorate_blocks", rec.consecutive_inquorate_blocks);
    // ODC-043: the record-v2 lifecycle stamps (KDD-072 P-b4) — they answer WHEN
    // a state transition happened, where `state` above answers only WHAT. Emitted
    // RAW INCLUDING THE -1 SENTINEL, matching last_rotation_height / drift_offset
    // directly above (this object mirrors the record's declaration order and has
    // exactly one not-applicable convention: show the sentinel, never omit, never
    // null). An ACTIVE record shows -1; a SUPERSEDED record shows its height.
    // v1 records deserialize with both sentinels, so this is safe for every record.
    ret.pushKV("superseded_height", rec.superseded_height);
    ret.pushKV("disbanded_height",  rec.disbanded_height);
    UniValue members(UniValue::VARR);
    for (const PTXQuorumMemberRecord& m : rec.members) {
        UniValue mv(UniValue::VOBJ);
        mv.pushKV("node_id",     m.node_id);
        mv.pushKV("pro_tx_hash", m.proTxHash.ToString());
        mv.pushKV("share_index", (int)m.share_index);
        mv.pushKV("in_qual",     m.in_qual);
        members.push_back(mv);
    }
    ret.pushKV("members", members);
    return ret;
}

UniValue ptx_quorum_list(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() > 1) {
        throw std::runtime_error(
            "ptx_quorum_list ( height )\n"
            "\nList ACTIVE quorums at a height (default: chain tip) — the router's\n"
            "active-set-at-height query (W2.1 registry). Read-only; all networks.\n"
            "\nArguments:\n"
            "1. height (numeric, optional) query height; default current tip\n"
            "\nResult: { \"height\": n, \"quorums\": [ {summary}, ... ] } most-recent first\n"
            + HelpExampleRpc("ptx_quorum_list", "")
        );
    }
    int nHeight;
    {
        LOCK(cs_main);
        nHeight = chainActive.Height();
    }
    if (request.params.size() == 1 && !request.params[0].isNull()) {
        nHeight = request.params[0].get_int();
        if (nHeight < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "height must be >= 0");
    }
    if (ptxQuorumStore == nullptr) {
        throw JSONRPCError(RPC_MISC_ERROR, "quorum store unavailable");
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("height", nHeight);
    UniValue arr(UniValue::VARR);
    for (const CPTXQuorumRecord& rec : ptxQuorumStore->GetActiveQuorumsAtHeight(nHeight)) {
        UniValue q(UniValue::VOBJ);
        q.pushKV("quorum_hash",      rec.quorum_hash.ToString());
        q.pushKV("formation_height", rec.formation_height);
        q.pushKV("mined_height",     rec.mined_height);
        q.pushKV("formed_size",      (int)rec.formed_size);
        q.pushKV("completed_size",   (int)rec.completed_size);
        q.pushKV("state",            rec.state == (uint8_t)PTXQuorumState::ACTIVE
                                         ? "active" : strprintf("state(%d)", rec.state));
        arr.push_back(q);
    }
    ret.pushKV("quorums", arr);
    return ret;
}

UniValue ptx_quorum_health(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 0) {
        throw std::runtime_error(
            "ptx_quorum_health\n"
            "\nThis node's margin contribution to each ACTIVE quorum (ODC-070 erosion\n"
            "watch).  Threshold is t-of-11, so members can silently lose shares while\n"
            "the quorum keeps signing — until the loss that fails it.  For every active\n"
            "quorum: is this node an in_qual member, and does it hold a CURRENT-role\n"
            "share (the only role that signs, BUG-028)?  Own view only — a node cannot\n"
            "see other members' share state; fleet capacity is aggregated across nodes\n"
            "by the operator/harness.  Read-only; all networks.\n"
            "\nResult:\n"
            "{ \"node_id\": s, \"height\": n, \"member_of\": n, \"capable\": n,\n"
            "  \"degraded\": n, \"quorums\": [ { \"quorum_hash\": s, \"mined_height\": n,\n"
            "  \"member\": b, \"share_current\": b, \"memory_only\": b }, ... ] }\n"
            + HelpExampleRpc("ptx_quorum_health", ""));
    }
    if (ptxQuorumStore == nullptr) {
        throw JSONRPCError(RPC_MISC_ERROR, "quorum store unavailable");
    }
    int nHeight;
    {
        LOCK(cs_main);
        nHeight = chainActive.Height();
    }
    int member = 0, capable = 0;
    UniValue arr(UniValue::VARR);
    for (const PTXShareHealth& h : PTX_ShareHealthReport(nHeight, g_ptx_my_node_id)) {
        UniValue q(UniValue::VOBJ);
        q.pushKV("quorum_hash",   h.quorum_hash.ToString());
        q.pushKV("mined_height",  h.mined_height);
        q.pushKV("member",        h.am_member);
        q.pushKV("share_current", h.share_current);
        q.pushKV("memory_only",   h.memory_only);
        arr.push_back(q);
        if (h.am_member) {
            ++member;
            if (h.share_current) ++capable;
        }
    }
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("node_id",   g_ptx_my_node_id);
    ret.pushKV("height",    nHeight);
    ret.pushKV("member_of", member);
    ret.pushKV("capable",   capable);
    ret.pushKV("degraded",  member - capable);
    ret.pushKV("quorums",   arr);
    return ret;
}

UniValue ptx_quorum_info(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "ptx_quorum_info \"quorum_hash\"\n"
            "\nReturn the persisted quorum record for a formation anchor (W2.1 registry).\n"
            "Read-only; all networks.\n"
            "\nArguments:\n"
            "1. quorum_hash (string, required) formation anchor block hash\n"
            "\nResult: the full record incl. per-member materialized share_index (KDD-061)\n"
            + HelpExampleRpc("ptx_quorum_info", "\"00..\"")
        );
    }
    const uint256 quorum_hash = uint256S(request.params[0].get_str());
    CPTXQuorumRecord rec;
    if (ptxQuorumStore == nullptr || !ptxQuorumStore->GetQuorumRecord(quorum_hash, rec)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "no quorum record for that quorum_hash");
    }
    return QuorumRecordToJson(rec);
}

// Forward declaration: defined after ptx_pose_status (Step 13).
static UniValue PTX_BuildPoseJson(const PTXNodeRecord& rec);

// ---------------------------------------------------------------------------
// RPC: ptx_getroundstatus
// ---------------------------------------------------------------------------

UniValue ptx_getroundstatus(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_getroundstatus ( round_id )\n"
            "\nReturn PTX round status and PoSe records.\n"
            "\nArguments:\n"
            "1. round_id (str, optional) Return status for a specific round only\n"
            + HelpExampleRpc("ptx_getroundstatus", "")
        );
    }

    UniValue ret(UniValue::VOBJ);

    // Helper lambda to serialise one round.
    auto round_to_uv = [](const PTXCommitRevealRound& r) {
        UniValue ro(UniValue::VOBJ);
        ro.pushKV("round_id",   r.round_id);
        ro.pushKV("round_seed", r.round_seed.GetHex());
        ro.pushKV("beacon",     r.beacon.GetHex());
        ro.pushKV("threshold",  r.threshold);
        ro.pushKV("state",      (int)r.state);
        UniValue cm(UniValue::VARR);
        for (const auto& n : r.committed_nodes) cm.push_back(n);
        ro.pushKV("committed",  cm);
        UniValue wh(UniValue::VARR);
        for (const auto& n : r.withheld_nodes) wh.push_back(n);
        ro.pushKV("withheld",   wh);
        UniValue ab(UniValue::VARR);
        for (const auto& n : r.abstained_nodes) ab.push_back(n);
        ro.pushKV("abstained",  ab);
        ro.pushKV("count",      (int64_t)r.count);
        ro.pushKV("low",        r.low);
        ro.pushKV("high",       r.high);
        ro.pushKV("unique",     r.unique);
        UniValue exc(UniValue::VARR);
        for (int64_t v : r.exclude_integers) exc.push_back(v);
        for (const auto& s : r.exclude_txids) exc.push_back(s);
        ro.pushKV("exclude",    exc);
        if (r.state == PTXRoundState::RESOLVED) {
            std::set<int64_t> exc_set(r.exclude_integers.begin(), r.exclude_integers.end());
            std::vector<int64_t> derived = PTX_MapBeacon(r.beacon, r.count, r.low, r.high, r.unique, exc_set);
            UniValue res(UniValue::VARR);
            for (int64_t v : derived) res.push_back(v);
            ro.pushKV("results", res);
        }
        return ro;
    };

    UniValue rounds_arr(UniValue::VARR);
    {
        LOCK(cs_ptx_rounds);
        if (!request.params.empty() && !request.params[0].isNull()) {
            std::string rid = request.params[0].get_str();
            auto it = g_ptx_rounds.find(rid);
            if (it != g_ptx_rounds.end())
                rounds_arr.push_back(round_to_uv(it->second));
        } else {
            // Return up to the last 10 rounds.
            std::vector<const PTXCommitRevealRound*> all;
            for (const auto& kv : g_ptx_rounds) all.push_back(&kv.second);
            size_t start = all.size() > 10 ? all.size() - 10 : 0;
            for (size_t i = start; i < all.size(); i++)
                rounds_arr.push_back(round_to_uv(*all[i]));
        }
    }
    ret.pushKV("rounds", rounds_arr);

    // PoSe records.
    UniValue pose_arr(UniValue::VARR);
    for (const auto& kv : g_ptx_pose_tracker.GetAllRecords()) {
        pose_arr.push_back(PTX_BuildPoseJson(kv.second));
    }
    ret.pushKV("pose_records", pose_arr);

    return ret;
}

// ---------------------------------------------------------------------------
// Shared pose-record JSON builder (Step 13)
// Used by ptx_pose_status, ptx_lottery_status.eligible_nodes,
// ptx_gm_pose, and ptx_wallet_operated_gms.
// Field order: node_id → pose_score → eligible → tickets → penalized_this_window
// (penalized_this_window appended last to preserve stable ordering for existing consumers).
// ---------------------------------------------------------------------------

static UniValue PTX_BuildPoseJson(const PTXNodeRecord& rec)
{
    UniValue po(UniValue::VOBJ);
    po.pushKV("node_id",               rec.node_id);
    po.pushKV("pose_score",            rec.pose_score);
    po.pushKV("eligible",              rec.quorum_eligible);
    po.pushKV("tickets",               rec.lottery_tickets);
    po.pushKV("penalized_this_window", rec.window_zeroed);
    return po;
}

// ---------------------------------------------------------------------------
// RPC: ptx_pose_status
// ---------------------------------------------------------------------------

UniValue ptx_pose_status(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_pose_status\n"
            "\nReturn PoSe scores, lottery tickets, and eligibility for all known GMs.\n"
            "\nResult:\n"
            "[\n"
            "  {\n"
            "    \"node_id\"               : \"str\",  compound label:suffix from ProRegPL v3\n"
            "    \"pose_score\"            : n,        cumulative penalty score (0 = healthy)\n"
            "    \"eligible\"              : bool,     false when pose_score >= 100\n"
            "    \"tickets\"               : n,        honest-participation count this window\n"
            "    \"penalized_this_window\" : bool      true if GM was penalized this window (tickets were reset)\n"
            "  }, ...\n"
            "]\n"
            + HelpExampleCli("ptx_pose_status", "")
            + HelpExampleRpc("ptx_pose_status", "")
        );
    }

    UniValue arr(UniValue::VARR);
    for (const auto& kv : g_ptx_pose_tracker.GetAllRecords()) {
        arr.push_back(PTX_BuildPoseJson(kv.second));
    }
    return arr;
}

// ---------------------------------------------------------------------------
// RPC: ptx_gm_pose
// ---------------------------------------------------------------------------

UniValue ptx_gm_pose(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1) {
        throw std::runtime_error(
            "ptx_gm_pose \"node_id\"\n"
            "\nReturn pose-tracker detail for a single registered GM.\n"
            "Errors with RPC_INVALID_PARAMETER if node_id is not found in the DGM list.\n"
            "For GMs participating in rolls but not yet registered via protx_register*,\n"
            "use ptx_pose_status instead.\n"
            "\nArguments:\n"
            "1. \"node_id\"  (string, required) compound label:suffix from protx_register* response\n"
            "\nResult:\n"
            "{\n"
            "  \"node_id\"               : \"str\",\n"
            "  \"pose_score\"            : n,        cumulative penalty score (0 = healthy)\n"
            "  \"eligible\"              : bool,     false when pose_score >= 100\n"
            "  \"tickets\"               : n,        honest-participation count this window\n"
            "  \"penalized_this_window\" : bool,     true if GM was penalized this window\n"
            "  \"payment_configured\"    : bool      true if scriptPTXPayment is set (GM can win payouts)\n"
            "}\n"
            + HelpExampleCli("ptx_gm_pose", "\"gm01:aabbccdd\"")
            + HelpExampleRpc("ptx_gm_pose", "\"gm01:aabbccdd\"")
        );
    }

    const std::string node_id = request.params[0].get_str();
    CDeterministicGMList gmList = deterministicGMManager->GetListAtChainTip();

    GMPoseDetail detail;
    if (!PTX_GetGMPoseDetail(node_id, gmList, g_ptx_pose_tracker, detail)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "GM not found: " + node_id);
    }

    UniValue result = PTX_BuildPoseJson(detail.pose);
    result.pushKV("payment_configured", detail.payment_configured);
    return result;
}

// ---------------------------------------------------------------------------
// RPC: ptx_lottery_status
// ---------------------------------------------------------------------------

// Build one settlement JSON entry for last_settle or settlement_history.
// "gm" is omitted when winner_script is empty or yields no standard destination —
// this cannot happen in normal operation (PTX_SelectWinner skips empty scripts) but
// the omit-not-empty-string contract makes any anomaly clearly visible to consumers.
// include_amount_sat: true for last_settle (flat monitoring field), false for history entries.
static UniValue PTX_MakeSettlementJson(const LastSettlement& s, bool include_amount_sat)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("height",       (int64_t)s.height);
    obj.pushKV("winner_protx", s.winner_protx.GetHex());
    if (include_amount_sat) obj.pushKV("amount_sat", s.amount);
    obj.pushKV("amount",       strprintf("%.8f", (double)s.amount / COIN));
    obj.pushKV("txid",         s.payout_txid.GetHex());
    if (!s.winner_script.empty()) {
        CTxDestination dest;
        if (ExtractDestination(s.winner_script, dest)) {
            obj.pushKV("gm", EncodeDestination(dest));
        }
    }
    return obj;
}

UniValue ptx_lottery_status(const JSONRPCRequest& request)
{
    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_lottery_status\n"
            "\nReturn current PTX lottery state for explorer and monitoring consumption.\n"
            "\nResult:\n"
            "{\n"
            "  \"pool_balance_sat\"   : n,        (numeric) accumulator UTXO value in satoshis\n"
            "  \"settlement_window\"  : n,        (numeric) blocks per settlement window\n"
            "  \"current_height\"     : n,        (numeric) current chain tip height\n"
            "  \"next_settlement_at\" : n,        (numeric) height of next settlement boundary\n"
            "  \"total_rolls\"        : n,        (numeric) cumulative PTX sessions since genesis\n"
            "  \"eligible_nodes\"     : [         (array) all pose-tracker nodes\n"
            "    {\n"
            "      \"node_id\"               : \"str\",\n"
            "      \"pose_score\"            : n,\n"
            "      \"eligible\"              : bool,\n"
            "      \"tickets\"               : n,\n"
            "      \"penalized_this_window\" : bool\n"
            "    }, ...\n"
            "  ],\n"
            "  \"last_settle\"        : {         (object) most recent settlement;\n"
            "                                     height=0 and winner_protx all-zero indicate no settlement yet\n"
            "    \"height\"       : n,\n"
            "    \"gm\"           : \"str\",   Base58Check payment address (field absent if script empty)\n"
            "    \"winner_protx\" : \"hex\",\n"
            "    \"amount_sat\"   : n,        (extra field on last_settle only)\n"
            "    \"amount\"       : \"str\",   HMS, 8 decimal places\n"
            "    \"txid\"         : \"hex\"\n"
            "  },\n"
            "  \"settlement_history\" : [         (array) recent settlements, newest first, cap=20\n"
            "    {\n"
            "      \"height\"       : n,\n"
            "      \"gm\"           : \"str\",   Base58Check payment address (field absent if script empty)\n"
            "      \"winner_protx\" : \"hex\",\n"
            "      \"amount\"       : \"str\",   HMS, 8 decimal places\n"
            "      \"txid\"         : \"hex\"\n"
            "    }, ...\n"
            "  ]\n"
            "}\n"
            + HelpExampleCli("ptx_lottery_status", "")
            + HelpExampleRpc("ptx_lottery_status", "")
        );
    }

    const int window  = Params().PTXSettlementWindow();
    const int height  = chainActive.Height();
    const int next_at = height + (window - (height % window));

    // Snapshot the full LotteryState under a single cs_main acquisition so all
    // fields are consistent with each other.
    LotteryState snapshot;
    {
        LOCK(cs_main);
        snapshot = GetLotteryState();
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("pool_balance_sat",   snapshot.accumulator_value);
    ret.pushKV("settlement_window",  window);
    ret.pushKV("current_height",     (int64_t)height);
    ret.pushKV("next_settlement_at", (int64_t)next_at);
    ret.pushKV("total_rolls",        (int64_t)snapshot.total_rolls);

    UniValue nodes_arr(UniValue::VARR);
    for (const auto& kv : g_ptx_pose_tracker.GetAllRecords()) {
        nodes_arr.push_back(PTX_BuildPoseJson(kv.second));
    }
    ret.pushKV("eligible_nodes", nodes_arr);

    // last_settle: sentinel when height==0 and winner_protx all-zero (no settlement yet).
    ret.pushKV("last_settle", PTX_MakeSettlementJson(snapshot.last_settle, /*include_amount_sat=*/true));

    // settlement_history: newest first (reverse-iterate the ring buffer).
    UniValue hist_arr(UniValue::VARR);
    const std::vector<LastSettlement>& hist = snapshot.settlement_history;
    for (int i = (int)hist.size() - 1; i >= 0; --i) {
        hist_arr.push_back(PTX_MakeSettlementJson(hist[i], /*include_amount_sat=*/false));
    }
    ret.pushKV("settlement_history", hist_arr);

    return ret;
}

// ---------------------------------------------------------------------------
// Wallet-scoped RPCs (Step 12)
// ---------------------------------------------------------------------------

#ifdef ENABLE_WALLET

UniValue ptx_wallet_lottery_status(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_wallet_lottery_status\n"
            "\nReturn wallet-scoped PTX lottery state: GMs whose scriptPTXPayment is spendable\n"
            "by keys in this wallet, and their current lottery participation status.\n"
            "\nNote: my_gms lists GMs where this wallet controls the payout address. It does NOT\n"
            "necessarily represent GMs operated by this wallet — in the cold/hot operator pattern\n"
            "the payout key can be on a different wallet from the operator/collateral keys.\n"
            "For operational ownership, see protx_list wallet_only=true.\n"
            "(KDD-035)\n"
            "\nResult:\n"
            "{\n"
            "  \"my_gms\": [               (array) GMs whose scriptPTXPayment this wallet controls\n"
            "    {\n"
            "      \"node_id\"    : \"str\",  compound label:suffix from ProRegPL v3\n"
            "      \"address\"    : \"str\",  Base58Check of scriptPTXPayment\n"
            "      \"tickets\"    : n,       current lottery_tickets in pose tracker\n"
            "      \"eligible\"   : bool,    quorum_eligible from pose tracker\n"
            "      \"pose_score\" : n        pose_score from pose tracker\n"
            "    }, ...\n"
            "  ],\n"
            "  \"my_eligible_count\" : n    count of my_gms where eligible==true and tickets>0\n"
            "}\n"
            + HelpExampleCli("ptx_wallet_lottery_status", "")
            + HelpExampleRpc("ptx_wallet_lottery_status", "")
        );
    }

    CDeterministicGMList gmList = deterministicGMManager->GetListAtChainTip();

    std::vector<WalletGMInfo> myGMs;
    {
        LOCK(pwallet->cs_wallet);
        myGMs = PTX_FilterWalletGMs(*pwallet, gmList, g_ptx_pose_tracker);
    }

    UniValue gmsArr(UniValue::VARR);
    int eligibleCount = 0;
    for (const auto& info : myGMs) {
        UniValue gobj(UniValue::VOBJ);
        gobj.pushKV("node_id",    info.node_id);
        CTxDestination dest;
        if (ExtractDestination(info.payment_script, dest)) {
            gobj.pushKV("address", EncodeDestination(dest));
        }
        gobj.pushKV("tickets",    info.tickets);
        gobj.pushKV("eligible",   info.eligible);
        gobj.pushKV("pose_score", info.pose_score);
        if (info.eligible && info.tickets > 0) ++eligibleCount;
        gmsArr.push_back(gobj);
    }

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("my_gms",           gmsArr);
    ret.pushKV("my_eligible_count", eligibleCount);
    return ret;
}

UniValue ptx_lottery_history(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_lottery_history\n"
            "\nReturn this wallet's recent PTX lottery winnings — the subset of the chain-side\n"
            "settlement_history where the payout went to a key controlled by this wallet.\n"
            "Results are newest first. Bounded by the chain-side history cap (max 20 entries).\n"
            "For an audit-grade all-time history, scan wallet transaction history for PTXPAYOUT\n"
            "receipts.\n"
            "\nResult:\n"
            "[                            (array) recent settlements won by this wallet, newest first\n"
            "  {\n"
            "    \"height\"       : n,\n"
            "    \"gm\"           : \"str\",  Base58Check payment address (field absent if non-standard)\n"
            "    \"winner_protx\" : \"hex\",\n"
            "    \"amount\"       : \"str\",  HMS, 8 decimal places\n"
            "    \"amount_sat\"   : n,\n"
            "    \"txid\"         : \"hex\"\n"
            "  }, ...\n"
            "]\n"
            + HelpExampleCli("ptx_lottery_history", "")
            + HelpExampleRpc("ptx_lottery_history", "")
        );
    }

    std::vector<LastSettlement> history;
    {
        LOCK(cs_main);
        history = GetLotteryState().settlement_history;
    }

    std::vector<LastSettlement> mine;
    {
        LOCK(pwallet->cs_wallet);
        mine = PTX_FilterWalletSettlements(*pwallet, history);
    }

    // mine is in ring-buffer order (oldest first); reverse to newest-first for output.
    UniValue ret(UniValue::VARR);
    for (int i = (int)mine.size() - 1; i >= 0; --i) {
        ret.push_back(PTX_MakeSettlementJson(mine[i], /*include_amount_sat=*/true));
    }
    return ret;
}

UniValue ptx_wallet_operated_gms(const JSONRPCRequest& request)
{
    CWallet* const pwallet = GetWalletForJSONRPCRequest(request);
    if (!EnsureWalletIsAvailable(pwallet, request.fHelp))
        return NullUniValue;

    if (request.fHelp) {
        throw std::runtime_error(
            "ptx_wallet_operated_gms\n"
            "\nReturn GMs where this wallet holds the owner or voting key, annotated with\n"
            "current pose-tracker state.\n"
            "\nNote: predicate is ks.HaveKey(keyIDOwner) || ks.HaveKey(keyIDVoting). This covers\n"
            "EC keys only. The BLS operator key (used for quorum signing on hot nodes) is NOT\n"
            "checked — a hot node holding only the BLS operator key will see empty results.\n"
            "For full GM association including collateral and BLS keys, use protx_list\n"
            "wallet_only=true. For GMs paying to keys in this wallet, see\n"
            "ptx_wallet_lottery_status. (KDD-036)\n"
            "\nResult:\n"
            "[                              (array) GMs where this wallet holds owner or voting key\n"
            "  {\n"
            "    \"node_id\"               : \"str\",  compound label:suffix from ProRegPL v3\n"
            "    \"proTxHash\"             : \"hex\",\n"
            "    \"payment_address\"       : \"str\",  Base58Check of scriptPTXPayment (field absent if not set)\n"
            "    \"has_payment_address\"   : bool,    true if scriptPTXPayment is configured\n"
            "    \"pose_score\"            : n,\n"
            "    \"eligible\"              : bool,\n"
            "    \"tickets\"               : n,\n"
            "    \"penalized_this_window\" : bool\n"
            "  }, ...\n"
            "]\n"
            + HelpExampleCli("ptx_wallet_operated_gms", "")
            + HelpExampleRpc("ptx_wallet_operated_gms", "")
        );
    }

    CDeterministicGMList gmList = deterministicGMManager->GetListAtChainTip();

    std::vector<OperatedGMInfo> myGMs;
    {
        LOCK(pwallet->cs_wallet);
        myGMs = PTX_FilterOperatedGMs(*pwallet, gmList, g_ptx_pose_tracker);
    }

    UniValue ret(UniValue::VARR);
    for (const auto& info : myGMs) {
        UniValue gobj(UniValue::VOBJ);
        gobj.pushKV("node_id",    info.node_id);
        gobj.pushKV("proTxHash",  info.proTxHash.GetHex());
        if (!info.payment_script.empty()) {
            CTxDestination dest;
            if (ExtractDestination(info.payment_script, dest)) {
                gobj.pushKV("payment_address", EncodeDestination(dest));
            }
        }
        gobj.pushKV("has_payment_address",   info.has_payment_address);
        gobj.pushKV("pose_score",            info.pose_score);
        gobj.pushKV("eligible",              info.eligible);
        gobj.pushKV("tickets",               info.tickets);
        gobj.pushKV("penalized_this_window", info.penalized_this_window);
        ret.push_back(gobj);
    }
    return ret;
}
#endif // ENABLE_WALLET

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ptx_debug_selectquorum — SG-1a probe: run the pure formation caller at an
// explicit anchor and return the score-ordered selection. READ-ONLY (no
// pending slot, no session, no store write). Net-gated like
// ptx_debug_ptxdkgpopulate: fails CLOSED off regtest/ptxbea. This is the
// surface the all-22 same-anchor identity row (c) and the on-fleet
// score-order fixture query per node.
// ---------------------------------------------------------------------------
static UniValue ptx_debug_selectquorum(const JSONRPCRequest& request)
{
    if (request.fHelp || request.params.size() != 1)
        throw std::runtime_error(
            "ptx_debug_selectquorum \"anchor_hash\"\n"
            "\nSG-1a debug probe: the pure formation selection at an anchor.\n"
            "Read-only; regtest/ptxbea only.\n"
            "\nArguments:\n"
            "1. anchor_hash   (string, required) block hash of the formation anchor\n"
            "\nResult: {anchor, height, eligible, active_excluded, pool,\n"
            "          selected: [{node_id, proTxHash, share_index}] (score order)}\n");

    if (!Params().IsRegTestNet() && !Params().IsPTXBeaTestNet()) {
        throw JSONRPCError(RPC_METHOD_NOT_FOUND,
            "ptx_debug_selectquorum is only available on regtest or the ptxbea test network");
    }

    const uint256 anchor_hash = ParseHashV(request.params[0], "anchor_hash");

    UniValue ret(UniValue::VOBJ);
    LOCK(cs_main);

    const CBlockIndex* pindexAnchor = LookupBlockIndex(anchor_hash);
    if (pindexAnchor == nullptr)
        throw JSONRPCError(RPC_INVALID_PARAMETER, "anchor_hash not found");
    // DETERMINISM: the anchor must be on the active chain — a stale-branch
    // anchor would let two nodes answer from different histories.
    if (!chainActive.Contains(pindexAnchor))
        throw JSONRPCError(RPC_INVALID_PARAMETER, "anchor not on the active chain");

    // Diagnostics mirror the caller's own pool build (same functions).
    const CDeterministicGMList listAtAnchor =
        deterministicGMManager->GetListForBlock(pindexAnchor);
    std::vector<CPTXQuorumRecord> activeAtAnchor;
    if (ptxQuorumStore) {
        activeAtAnchor = ptxQuorumStore->GetActiveQuorumsAtHeight(pindexAnchor->nHeight);
    }
    size_t eligible = 0;
    listAtAnchor.ForEachGM(true, [&](const CDeterministicGMCPtr& dgm) {
        if (PTX_DKG_IsGMPTXEligible(dgm)) eligible++;
    });
    const CDeterministicGMList pool =
        PTX_Formation_BuildPool(listAtAnchor, activeAtAnchor);

    std::vector<PTXDKGMember> members;
    const bool formed = PTX_Formation_SelectAtAnchor(pindexAnchor, members);

    ret.pushKV("anchor", anchor_hash.ToString());
    ret.pushKV("height", pindexAnchor->nHeight);
    ret.pushKV("eligible", (uint64_t)eligible);
    ret.pushKV("active_excluded", (uint64_t)(eligible - pool.GetValidGMsCount()));
    ret.pushKV("pool", (uint64_t)pool.GetValidGMsCount());
    ret.pushKV("formed", formed);
    UniValue arr(UniValue::VARR);
    for (size_t i = 0; i < members.size(); i++) {
        UniValue m(UniValue::VOBJ);
        m.pushKV("node_id", members[i].node_id);
        m.pushKV("proTxHash", members[i].proTxHash.ToString());
        m.pushKV("share_index", (uint64_t)(i + 1));
        arr.push_back(m);
    }
    ret.pushKV("selected", arr);
    return ret;
}

// clang-format off
static const CRPCCommand commands[] = {
//  category  name                         handler                       okSafe  argNames
    { "ptx",  "ptx_roll",                  &ptx_roll,                   true,   {"count","low","high","unique","exclude","game_id","caller_salt"} },
    { "ptx",  "gm_commit",                 &gm_commit,                  true,   {"round_id","round_seed_hex","members_json","commitment_hex"} },
    { "ptx",  "gm_reveal",                 &gm_reveal,                  true,   {"round_id","secret_hex"} },
    { "ptx",  "gm_bls_sign",               &gm_bls_sign,                true,   {"round_seed_hex","quorum_hash","commit_hex"} },
    { "ptx",  "ptx_debug_setnodefailmode", &ptx_debug_setnodefailmode,  true,   {"target_node_id","mode"} },
    { "ptx",  "ptx_debug_ptxdkgpopulate",  &ptx_debug_ptxdkgpopulate,   true,   {"payload","force"} },
    { "ptx",  "ptx_debug_selectquorum",    &ptx_debug_selectquorum,     true,   {"anchor_hash"} },
    { "ptx",  "ptx_quorum_info",           &ptx_quorum_info,            true,   {"quorum_hash"} },
    { "ptx",  "ptx_quorum_health",         &ptx_quorum_health,          true,   {} },
    { "ptx",  "ptx_quorum_list",           &ptx_quorum_list,            true,   {"height"} },
    { "ptx",  "ptx_getroundstatus",        &ptx_getroundstatus,         true,   {"round_id"} },
    { "ptx",  "ptx_pose_status",           &ptx_pose_status,            true,   {} },
    { "ptx",  "ptx_gm_pose",               &ptx_gm_pose,                true,   {"node_id"} },
    { "ptx",  "ptx_lottery_status",        &ptx_lottery_status,         true,   {} },
#ifdef ENABLE_WALLET
    { "ptx",  "ptx_wallet_lottery_status", &ptx_wallet_lottery_status,  true,   {} },
    { "ptx",  "ptx_lottery_history",       &ptx_lottery_history,        true,   {} },
    { "ptx",  "ptx_wallet_operated_gms",   &ptx_wallet_operated_gms,    true,   {} },
#endif // ENABLE_WALLET
};
// clang-format on

void RegisterPTXRPCCommands(CRPCTable& tableRPC)
{
    for (unsigned int vcidx = 0; vcidx < ARRAYLEN(commands); vcidx++)
        tableRPC.appendCommand(commands[vcidx].name, &commands[vcidx]);
}
