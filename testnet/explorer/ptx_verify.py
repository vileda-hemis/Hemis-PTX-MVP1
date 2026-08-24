"""Independent re-derivation of a PTX roll, from the transaction bytes alone.

★ WHY THIS IS WORTH HAVING AT ALL.  Consensus does not check these.
`PTX_MapBeacon` appears in src/rpc/ptx.cpp only (:406, :1273) and NEVER in
validation.cpp; `quorum_sig` appears in validation.cpp zero times.  The header
says so itself (primitives/transaction.h:540-542): the quorum_hash is "NOT a
trustworthy trigger input until the consensus quorum_sig verify lands (W4-b)".

So until W4-b, a node will happily connect a block whose PTXSESS carries
results that do not follow from its beacon, or a beacon that does not follow
from its signature.  These checks are, today, the only thing looking.

Checks A, P and B need NOTHING but the payload.  C needs the payload too -- the
whole point of porting PTX_MapBeacon rather than calling a node for it.
Check D (the BLS threshold signature against the quorum's group public key) is
NOT performed here and is reported as not performed: it needs the quorum record
at quorum_hash, which only a synced indexing node has.
"""
import hashlib
import struct

UINT64_MAX = (1 << 64) - 1


def _sha256(b):
    return hashlib.sha256(b).digest()


def _sha256d(b):
    return hashlib.sha256(hashlib.sha256(b).digest()).digest()


def _u256_raw(display_hex):
    """Display hex -> the 32 raw bytes the C++ uint256 holds.

    ★ uint256 is stored little-endian and DISPLAYED big-endian.  Every hash
    input below uses .begin() -- the raw bytes -- so display hex must be
    reversed before hashing.  Skip this and you get a plausible hash that
    matches nothing, which is the worst failure mode a verifier has."""
    b = bytes.fromhex(display_hex)
    if len(b) != 32:
        raise ValueError("expected a 32-byte hash, got %d bytes" % len(b))
    return b[::-1]


def _u256_show(raw32):
    return raw32[::-1].hex()


def _compact_size(n):
    if n < 0xfd:
        return bytes([n])
    if n <= 0xffff:
        return b"\xfd" + struct.pack("<H", n)
    if n <= 0xffffffff:
        return b"\xfe" + struct.pack("<I", n)
    return b"\xff" + struct.pack("<Q", n)


# --------------------------------------------------------------------------
# P — params_hash == SHA256d(count ‖ low ‖ high ‖ unique ‖ excludes)
#     PTX_HashParams, src/ptx/ptx_seed.cpp:44-51.
#     ★ Not in the original A/B/C set, and it belongs there: check A folds
#     params_hash into the seed, so WITHOUT this an attacker could put any
#     params_hash they liked in the payload and A would still pass.  P is what
#     binds the advertised count/low/high/unique/excludes to the seed.
# --------------------------------------------------------------------------
def check_params_hash(f):
    ds = b""
    ds += struct.pack("<I", f["count"])
    ds += struct.pack("<q", f["low"])
    ds += struct.pack("<q", f["high"])
    ds += b"\x01" if f["unique"] else b"\x00"
    ds += _compact_size(len(f["exclude_integers"]))
    for v in f["exclude_integers"]:
        ds += struct.pack("<q", v)
    ds += _compact_size(len(f["exclude_txids"]))
    for s in f["exclude_txids"]:
        sb = s.encode()
        ds += _compact_size(len(sb)) + sb
    got = _u256_show(_sha256d(ds))
    return {
        "id": "P", "ok": got == f["ptx_params_hash"],
        "claim": "ptx_params_hash == SHA256d(count ‖ low ‖ high ‖ unique ‖ exclude_integers ‖ exclude_txids)",
        "source": "PTX_HashParams, src/ptx/ptx_seed.cpp:44-51",
        "derivation": "SHA256d(LE32 %d ‖ LE64 %d ‖ LE64 %d ‖ %s ‖ %d int excl ‖ %d txid excl)"
                      % (f["count"], f["low"], f["high"],
                         "01" if f["unique"] else "00",
                         len(f["exclude_integers"]), len(f["exclude_txids"])),
        "preimage_hex": ds.hex(),
        "computed": got, "claimed": f["ptx_params_hash"],
    }


# --------------------------------------------------------------------------
# A — round_seed == SHA256(game_id ‖ LE32(height) ‖ caller_pubkey ‖ nonce ‖ params_hash)
#     PTX_BuildRoundSeed, src/ptx/ptx_seed.cpp:24-40 (KDD-002/003).
#     ★ game_id is hashed RAW, with no length prefix (h.Write(game_id.data(),
#     game_id.size())) -- unlike its serialisation, which IS length-prefixed.
# --------------------------------------------------------------------------
def check_round_seed(f):
    """★ THE caller_pubkey FIELD IS NOT A SEED INPUT, AND ON A COMMITMENT IT IS
    NOT A PUBKEY EITHER.  Measured, not assumed: across 141 real ptxbea
    PTXROLLCOMMIT payloads the round_seed reproduces 141/141 with an EMPTY
    caller_pubkey and 0/141 with the value the payload actually carries.

    The cause is two lines apart in one function.  src/rpc/ptx.cpp:225 derives
    the seed passing `{}` for caller_pubkey, while src/rpc/ptx.cpp:300 stores
    `caller_salt_bytes` into commit_payload.caller_pubkey.  The salt DOES reach
    the seed, but indirectly, through the nonce
    (PTX_BuildNonce(prev_beacon, caller_salt_bytes), src/rpc/ptx.cpp:218) --
    never as a direct hash input.  The settle payload leaves the field empty,
    which is why A passes there and would fail here.

    So we try the payload's own value first and fall back to the empty vector,
    and we SAY which one reproduced.  Hard-coding empty would hide a real
    future change; trusting the field would fail on every commitment today.
    """
    def build(pubkey_hex):
        pre = f["game_id"].encode()
        pre += struct.pack("<I", f["nSeedHeight"])
        pre += bytes.fromhex(pubkey_hex)
        pre += _u256_raw(f["nonce"])
        pre += _u256_raw(f["ptx_params_hash"])
        return pre

    claimed = f["round_seed"]
    field_hex = f["caller_pubkey"]

    pre = build(field_hex)
    got = _u256_show(_sha256(pre))
    variant = "the payload's caller_pubkey field (%d byte(s))" % (len(field_hex) // 2)
    note = None

    if got != claimed and field_hex:
        pre_alt = build("")
        got_alt = _u256_show(_sha256(pre_alt))
        if got_alt == claimed:
            pre, got = pre_alt, got_alt
            variant = "an EMPTY caller_pubkey"
            note = ("the payload carries %d byte(s) in caller_pubkey but the seed was NOT "
                    "derived with them. src/rpc/ptx.cpp:225 passes {} for this input; "
                    "src/rpc/ptx.cpp:300 stores the caller SALT in the field. The salt "
                    "reaches the seed only through the nonce "
                    "(PTX_BuildNonce, src/rpc/ptx.cpp:218), never directly."
                    % (len(field_hex) // 2))

    return {
        "id": "A", "ok": got == claimed,
        "claim": "round_seed == SHA256(game_id \u2016 LE32(nSeedHeight) \u2016 caller_pubkey \u2016 nonce \u2016 ptx_params_hash)",
        "source": "PTX_BuildRoundSeed, src/ptx/ptx_seed.cpp:24-40 (KDD-002/003)",
        "derivation": "SHA256(%r \u2016 LE32 %d \u2016 %s \u2016 nonce \u2016 params_hash)"
                      % (f["game_id"], f["nSeedHeight"], variant),
        "preimage_hex": pre.hex(),
        "computed": got, "claimed": claimed,
        "note": note,
    }


# --------------------------------------------------------------------------
# B — beacon == SHA256(quorum_sig)
#     PTX_BLS_SigToBeacon, src/ptx/ptx_bls.cpp:777-782.
# --------------------------------------------------------------------------
def check_beacon(f):
    sig = bytes.fromhex(f["quorum_sig"])
    got = _u256_show(_sha256(sig)) if sig else None
    return {
        "id": "B", "ok": bool(sig) and got == f["beacon"],
        "claim": "beacon == SHA256(quorum_sig)",
        "source": "PTX_BLS_SigToBeacon, src/ptx/ptx_bls.cpp:777-782",
        "derivation": "SHA256(%d-byte signature)" % len(sig),
        "preimage_hex": sig.hex(),
        "computed": got, "claimed": f["beacon"],
        "note": None if sig else "payload carries no quorum_sig -- nothing to hash",
    }


# --------------------------------------------------------------------------
# C — results == PTX_MapBeacon(beacon, count, low, high, unique, excludes)
#     A FAITHFUL PORT of src/ptx/ptx_output_mapping.cpp:12-100.
#
#     ★ PORTED, NOT DELEGATED TO A NODE -- and that was a real choice.  Driving
#     C through a node's RPC would be safer against porting error, but it would
#     cost the property that makes this page worth having: that it verifies a
#     roll with no node, no index and no chain, from hex alone.  The port is
#     justified by evidence rather than by confidence: test_decoder.py replays
#     it against every settled PTXSESS harvested from ptxbea and requires an
#     EXACT match on the results field, across the unique and non-unique paths
#     and with non-empty exclusion lists.  A subtly-wrong reimplementation
#     produces confident wrong verdicts, which is worse than no verifier -- so
#     the test is the licence to ship this, not a formality.
# --------------------------------------------------------------------------
def expand_beacon(beacon_raw32, needed_bytes):
    """PTX_ExpandBeacon, ptx_output_mapping.cpp:12-30."""
    out = bytearray()
    prev = beacon_raw32
    idx = 0
    while len(out) < needed_bytes:
        blk = _sha256(prev + struct.pack("<I", idx))
        idx += 1
        out += blk
        prev = blk
    return bytes(out[:needed_bytes])


def sample_one(exp, offset, low, high):
    """PTX_SampleOne, ptx_output_mapping.cpp:32-48.
    -> (ok, value, new_offset). Rejection sampling to remove modulo bias."""
    pool = high - low + 1
    threshold = (UINT64_MAX // pool) * pool
    while offset + 8 <= len(exp):
        chunk = struct.unpack_from("<Q", exp, offset)[0]
        offset += 8
        if chunk < threshold:
            return True, low + (chunk % pool), offset
    return False, None, offset


def map_beacon(beacon_raw32, count, low, high, unique, exclude_set):
    """PTX_MapBeacon, ptx_output_mapping.cpp:50-100."""
    if not unique:
        expand_mult = 128
        exp = expand_beacon(beacon_raw32, count * expand_mult)
        off = 0
        res = []
        stall = 0
        while len(res) < count:
            ok, v, off = sample_one(exp, off, low, high)
            if not ok:
                expand_mult *= 2
                exp = expand_beacon(beacon_raw32, count * expand_mult)
                off = 0
                continue
            if v not in exclude_set:
                res.append(v)
                stall = 0
            else:
                stall += 1
                if stall > 100000:
                    raise ValueError("exclude set covers entire range")
        return res

    pool = [v for v in range(low, high + 1) if v not in exclude_set]
    if len(pool) < count:
        raise ValueError("pool too small for unique draw (%d < %d)" % (len(pool), count))
    exp = expand_beacon(beacon_raw32, len(pool) * 32)
    off = 0
    for i in range(count):
        # ★ j DEFAULTS TO i and the return value is DELIBERATELY IGNORED --
        # ptx_output_mapping.cpp:95 does exactly this. On exhausted material the
        # element stays in place rather than the draw failing. Ported, not fixed:
        # a "better" version here would disagree with the chain.
        j = i
        ok, v, off = sample_one(exp, off, i, len(pool) - 1)
        if ok:
            j = v
        pool[i], pool[j] = pool[j], pool[i]
    return pool[:count]


def check_results(f):
    exc = set(f["exclude_integers"])
    err = None
    got = None
    try:
        got = map_beacon(_u256_raw(f["beacon"]), f["count"], f["low"], f["high"],
                         f["unique"], exc)
    except Exception as e:                                   # noqa: BLE001
        err = str(e)
    return {
        "id": "C", "ok": err is None and got == f["results"],
        "claim": "results == PTX_MapBeacon(beacon, count, low, high, unique, exclude_integers)",
        "source": "PTX_MapBeacon, src/ptx/ptx_output_mapping.cpp:50-100 (KDD-009/024/025/026)",
        "derivation": "%s over [%d, %d]%s, %d draw(s)"
                      % ("Fisher-Yates on the eligible pool" if f["unique"]
                         else "independent rejection-sampled draws",
                         f["low"], f["high"],
                         (", excluding %d value(s)" % len(exc)) if exc else "",
                         f["count"]),
        "computed": got, "claimed": f["results"], "error": err,
    }


def check_signature_not_performed(f):
    """D — stated plainly as NOT performed, with the reason."""
    return {
        "id": "D", "ok": None,
        "claim": "quorum_sig verifies under the quorum's group public key at quorum_hash, "
                 "and that quorum was the canonical one at nSeedHeight",
        "source": "PTX_BLS_Verify src/ptx/ptx_bls.cpp:788; group_pk_bytes in "
                  "CPTXQuorumRecord, src/ptx/ptx_quorum_store.h:137",
        "derivation": "NOT PERFORMED. The group public key is not in the transaction -- it "
                      "lives in the quorum record keyed by quorum_hash, which is built from "
                      "chain data by a synced node. This page has no node by design, so it "
                      "cannot do this check and does not pretend to.",
        "computed": None, "claimed": f.get("quorum_hash"),
    }


def verify(fields, struct_name):
    """-> list of check results, in reading order."""
    if struct_name == "CPTXRollCommitPayload":
        # Sig-less and results-less by design (BUG-032 fund-then-sign): the
        # commitment exists BEFORE the quorum signs, so B/C have nothing to
        # check and saying "pass" would be a lie about what was verified.
        return [check_params_hash(fields), check_round_seed(fields)]
    return [check_params_hash(fields), check_round_seed(fields),
            check_beacon(fields), check_results(fields),
            check_signature_not_performed(fields)]
