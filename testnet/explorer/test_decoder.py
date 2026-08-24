#!/usr/bin/env python3
"""The licence to ship the decoder and the PTX_MapBeacon port.

Four things are proven here, and the fourth is the one that makes the other
three mean anything:

  1. DIVERGENCE. The spec extracted from src/primitives/transaction.h still
     matches payload_spec.golden.  ★ This is the whole point of generating the
     decoder rather than hand-writing it: when someone adds a payload field,
     this fails LOUDLY instead of the decoder going quietly wrong.  The
     previous hand-rolled decoder (explorer/proxy.py:162-195) had no such test
     and silently dropped quorum_hash.
  2. ORACLE. The generated decoder agrees with that hand-rolled decoder on
     every field it covered.  It was correct as far as it went, so it is a fair
     oracle -- and this pins the fields it DID cover while we replace it.
  3. REPLAY. Checks P/A/B/C reproduce real settled ptxbea payloads exactly,
     across the unique and non-unique paths and with non-empty exclusions.
  4. ★ FALSIFICATION. Each check is shown FAILING against a deliberately
     corrupted payload.  Without this, "281/281 pass" is consistent with checks
     that cannot fail -- the vacuity trap the cold-sync harness was built to
     expose, pointed at this verifier.

Run:  python3 test_decoder.py [fixtures.json]
Fixtures are real transactions; harvest with harvest_fixtures.py.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from ptx_payload_spec import extract_spec, spec_fingerprint   # noqa: E402
from ptx_decode import decode, as_dict, PayloadError          # noqa: E402
import ptx_verify as V                                        # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
GOLDEN = os.path.join(HERE, "payload_spec.golden")

PASS = FAIL = 0


def ok(msg):
    global PASS
    PASS += 1
    print("  [ok]   %s" % msg)


def bad(msg):
    global FAIL
    FAIL += 1
    print("  [FAIL] %s" % msg)


# ---------------------------------------------------------------- 1. divergence
def test_divergence():
    print("\n=== 1. Divergence: header vs golden spec ===")
    live = spec_fingerprint(extract_spec())
    if not os.path.exists(GOLDEN):
        bad("payload_spec.golden is missing -- regenerate: python3 ptx_payload_spec.py > payload_spec.golden")
        return
    with open(GOLDEN, encoding="utf-8") as fh:
        golden = fh.read().strip()
    if live.strip() == golden:
        n = sum(len(v) for v in extract_spec().values())
        ok("wire spec matches the header exactly (%d fields across 2 payloads)" % n)
    else:
        bad("THE PAYLOAD WIRE FORMAT HAS CHANGED.\n"
            "         src/primitives/transaction.h no longer matches payload_spec.golden.\n"
            "         This is the test doing its job -- the decoder is now stale.\n"
            "         Review the diff, then regenerate:\n"
            "           python3 ptx_payload_spec.py > payload_spec.golden")
        gl, ll = golden.splitlines(), live.splitlines()
        for i in range(max(len(gl), len(ll))):
            g = gl[i] if i < len(gl) else "<missing>"
            l = ll[i] if i < len(ll) else "<missing>"
            if g != l:
                print("           golden: %s\n           header: %s" % (g, l))


# ------------------------------------------------------------------- 2. oracle
def _oracle_decode(payload_hex):
    """The pre-existing hand-rolled decoder, explorer/proxy.py:162-195,
    reproduced verbatim in behaviour. Correct on every field it reaches; it
    simply stops before quorum_hash."""
    p = bytes.fromhex(payload_hex)
    o = 0

    def cs(off):
        b = p[off]
        if b < 0xfd:
            return b, off + 1
        if b == 0xfd:
            return struct.unpack_from("<H", p, off + 1)[0], off + 3
        if b == 0xfe:
            return struct.unpack_from("<I", p, off + 1)[0], off + 5
        return struct.unpack_from("<Q", p, off + 1)[0], off + 9

    def rs(off):
        n, off = cs(off)
        return p[off:off + n].decode(), off + n

    r = {}
    r["game_id"], o = rs(o)
    r["nSeedHeight"] = struct.unpack_from("<I", p, o)[0]; o += 4
    r["nExpiryHeight"] = struct.unpack_from("<I", p, o)[0]; o += 4
    n, o = cs(o); o += n                                   # caller_pubkey skipped
    r["nonce"] = p[o:o + 32].hex(); o += 32
    r["ptx_params_hash"] = p[o:o + 32].hex(); o += 32
    r["count"] = struct.unpack_from("<I", p, o)[0]; o += 4
    r["low"] = struct.unpack_from("<q", p, o)[0]; o += 8
    r["high"] = struct.unpack_from("<q", p, o)[0]; o += 8
    r["unique"] = bool(p[o]); o += 1
    n, o = cs(o)
    r["exclude_integers"] = [struct.unpack_from("<q", p, o + i * 8)[0] for i in range(n)]; o += n * 8
    n, o = cs(o)
    r["exclude_txids"] = []
    for _ in range(n):
        s, o = rs(o); r["exclude_txids"].append(s)
    r["round_seed"] = p[o:o + 32].hex(); o += 32
    r["beacon"] = p[o:o + 32].hex(); o += 32
    n, o = cs(o)
    r["results"] = [struct.unpack_from("<q", p, o + i * 8)[0] for i in range(n)]; o += n * 8
    r["quorum_sig_hash"] = p[o:o + 32].hex(); o += 32
    n, o = cs(o)
    r["quorum_members"] = []
    for _ in range(n):
        s, o = rs(o); r["quorum_members"].append(s)
    n, o = cs(o)
    r["quorum_sig"] = p[o:o + n].hex(); o += n
    return r


def test_oracle(fx):
    print("\n=== 2. Oracle: generated decoder vs the hand-rolled one it replaces ===")
    sess = [f for f in fx if f["type"] == 6]
    if not sess:
        bad("no PTXSESS fixtures")
        return
    mismatches = []
    for f in sess:
        new = as_dict(decode(f["payload"], "CProbabilisticTxPayload"))
        old = _oracle_decode(f["payload"])
        for k, ov in old.items():
            nv = new[k]
            # the oracle keeps uint256 in wire order; ours displays big-endian
            if k in ("nonce", "ptx_params_hash", "round_seed", "beacon", "quorum_sig_hash"):
                ov = bytes.fromhex(ov)[::-1].hex()
            if nv != ov:
                mismatches.append((f["txid"][:16], k, ov, nv))
    if mismatches:
        bad("%d field mismatch(es) vs the oracle" % len(mismatches))
        for m in mismatches[:5]:
            print("         %s %s: oracle=%r new=%r" % m)
    else:
        ok("agrees with the oracle on all %d shared fields, across %d payloads"
           % (len(_oracle_decode(sess[0]["payload"])), len(sess)))
    # and the field the oracle could not see
    if all("quorum_hash" in as_dict(decode(f["payload"], "CProbabilisticTxPayload")) for f in sess):
        ok("decodes quorum_hash, which the oracle silently dropped (KDD-074/076)")


# ------------------------------------------------------------------- 3. replay
def test_replay(fx):
    print("\n=== 3. Replay: P/A/B/C against real settled ptxbea payloads ===")
    tally = {}
    shapes = set()
    for f in fx:
        sn = "CProbabilisticTxPayload" if f["type"] == 6 else "CPTXRollCommitPayload"
        d = decode(f["payload"], sn)
        if d["trailing"]:
            bad("%s: %d trailing byte(s) after the last field -- payload is not this struct"
                % (f["txid"][:16], len(d["trailing"]) // 2))
            return
        fl = as_dict(d)
        shapes.add((sn == "CProbabilisticTxPayload", fl["unique"],
                    bool(fl["exclude_integers"]), fl["count"] > 1))
        for c in V.verify(fl, sn):
            if c["ok"] is None:
                continue
            key = c["id"]
            tally.setdefault(key, [0, 0])
            tally[key][0 if c["ok"] else 1] += 1
    for cid in sorted(tally):
        p, fl_ = tally[cid]
        if fl_:
            bad("check %s: %d passed, %d FAILED" % (cid, p, fl_))
        else:
            ok("check %s reproduced %d/%d real payloads exactly" % (cid, p, p))
    # coverage: the port must have met both MapBeacon paths and real exclusions
    uniq = any(s[1] for s in shapes)
    nonuniq = any(not s[1] for s in shapes)
    excl = any(s[2] for s in shapes)
    multi = any(s[3] for s in shapes)
    if uniq and nonuniq and excl and multi:
        ok("shape coverage: unique AND non-unique paths, non-empty exclusions, count>1 (%d shapes)"
           % len(shapes))
    else:
        bad("shape coverage INCOMPLETE -- unique=%s non-unique=%s exclusions=%s count>1=%s. "
            "A green run that never exercised a path proves nothing about it."
            % (uniq, nonuniq, excl, multi))


# ------------------------------------------------------------- 4. falsification
def test_falsification(fx):
    print("\n=== 4. ★ Falsification: each check must FAIL on corrupted data ===")
    sess = [f for f in fx if f["type"] == 6]
    base = as_dict(decode(sess[0]["payload"], "CProbabilisticTxPayload"))

    def mutate(**kw):
        d = dict(base)
        d.update(kw)
        return d

    def flip(h):
        b = bytearray(bytes.fromhex(h))
        b[0] ^= 0xff
        return bytes(b).hex()

    cases = [
        ("P", "ptx_params_hash flipped", mutate(ptx_params_hash=flip(base["ptx_params_hash"]))),
        ("P", "count altered",           mutate(count=base["count"] + 1)),
        ("A", "nonce flipped",           mutate(nonce=flip(base["nonce"]))),
        ("A", "nSeedHeight altered",     mutate(nSeedHeight=base["nSeedHeight"] + 1)),
        ("B", "quorum_sig flipped",      mutate(quorum_sig=flip(base["quorum_sig"]))),
        ("C", "beacon flipped",          mutate(beacon=flip(base["beacon"]))),
        ("C", "results altered",         mutate(results=[r + 1 for r in base["results"]])),
    ]
    for cid, what, fields in cases:
        res = {c["id"]: c for c in V.verify(fields, "CProbabilisticTxPayload")}
        c = res[cid]
        if c["ok"] is False:
            ok("check %s FAILS on %s -- it discriminates" % (cid, what))
        else:
            bad("check %s still reports ok=%r with %s -- THE CHECK IS VACUOUS"
                % (cid, c["ok"], what))

    # the decoder itself must refuse malformed input rather than invent fields
    for what, blob in (("truncated payload", sess[0]["payload"][:40]),
                       ("not hex", "zzzz")):
        try:
            decode(blob, "CProbabilisticTxPayload")
            bad("decoder accepted %s instead of raising" % what)
        except PayloadError:
            ok("decoder refuses %s with a located error" % what)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "fixtures.json")
    test_divergence()
    if not os.path.exists(path):
        print("\n  no fixtures at %s -- replay/oracle/falsification SKIPPED." % path)
        print("  ★ A run that skips them is not a pass. Harvest first.")
        return 1
    fx = json.load(open(path))
    print("\n  fixtures: %d real transactions" % len(fx))
    test_oracle(fx)
    test_replay(fx)
    test_falsification(fx)
    print("\n=== Verdict ===")
    print("  %d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
