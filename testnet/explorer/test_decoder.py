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

    # ★★ ZERO-ARG TESTS ARE DISCOVERED, NOT LISTED -- and the list was the bug.
    # main() named its tests explicitly, so three test functions defined BELOW it
    # were never called: the two added to pin the NOT-PERFORMED-vs-FAIL rendering
    # and the footer-reachability one. The suite reported "17 passed" before and
    # after they were added, and that number was read as covering them. A test
    # that is never invoked is indistinguishable from a test that passes.
    #
    # Discovery removes the mechanism rather than repairing this instance: a new
    # zero-arg test_* is picked up by existing, and the assertion below fails the
    # run if anything is left behind.
    import inspect
    g = dict(globals())
    ran = {"test_divergence", "test_oracle", "test_replay", "test_falsification"}
    for name in sorted(g):
        if not name.startswith("test_") or name in ran:
            continue
        fn = g[name]
        if not callable(fn):
            continue
        if inspect.signature(fn).parameters:
            print("  [FAIL] %s takes arguments and is not wired into main()" % name)
            globals()["FAIL"] = FAIL + 1
            continue
        try:
            fn()
            ok("%s" % name)
        except AssertionError as e:
            bad("%s -- %s" % (name, e))
        ran.add(name)

    missed = sorted(n for n in g
                    if n.startswith("test_") and callable(g[n]) and n not in ran)
    if missed:
        bad("test functions defined but never run: %s" % ", ".join(missed))

    print("\n=== Verdict ===")
    print("  %d passed, %d failed" % (PASS, FAIL))
    return 0 if FAIL == 0 else 1




# ---------------------------------------------------------------------------
# ★★ THE THREE-STATE RENDERING. "not performed" must never read as "failed".
#
# Check D (quorum_sig verifies under the group public key) is deliberately
# ok=None: the page has no node by design, so it cannot do that check and says
# so. A two-state renderer collapses None to falsey and prints FAIL.
#
# ★ On a PUBLIC page that reads as the beacon failing verification -- the exact
# opposite of what the artefact exists to demonstrate, on the one surface whose
# whole purpose is demonstrating it. This is not a cosmetic test.
#
# ★ Caught in a console script, not here: an ad-hoc printout used a truthiness
# test and rendered D as FAIL. app.py was already correct; this test exists so
# it STAYS correct, and so the next person writing a renderer sees the case.
def test_api_always_returns_the_raw_payload():
    """★ The anti-oracle guarantee, pinned as a test rather than a comment.

    Every API response must carry payload_hex beside the decoded values. Without
    it a caller cannot re-derive our answer and has to trust it -- which is the
    privileged-oracle surface this whole architecture exists to avoid. A future
    "simplification" that drops the field must fail here."""
    import json, os, app
    here = os.path.dirname(os.path.abspath(__file__))
    fx = json.load(open(os.path.join(here, "fixtures.json")))
    ph = next(e["payload"] for e in fx if isinstance(e.get("payload"), str))
    body, code = app.api_verify(ph)
    assert code == 200, body
    assert body["payload_hex"] == ph, "payload_hex must echo the exact bytes verified"
    assert body["checks"], "a verify response with no checks proves nothing"


def test_api_not_performed_is_null_never_false():
    """★ The machine-readable half of the None-vs-FAIL distinction.

    Check D is not performed -- the group public key is not in the transaction.
    It must serialise as null with a reason, NEVER as false. A JSON false here
    would tell an integrator the signature FAILED verification, when the truth is
    that we did not attempt it."""
    import json, os, app
    here = os.path.dirname(os.path.abspath(__file__))
    fx = json.load(open(os.path.join(here, "fixtures.json")))
    settle = None
    for e in fx:
        ph = e.get("payload")
        if not isinstance(ph, str):
            continue
        d, _ = app.sniff(ph)
        if d and d["struct"] != "CPTXRollCommitPayload":
            settle = ph
            break
    assert settle, "no settle payload in fixtures to exercise check D"
    body, code = app.api_verify(settle)
    assert code == 200
    d_checks = [c for c in body["checks"] if c["id"] == "D"]
    assert d_checks, "check D must be present on a settle"
    assert d_checks[0]["ok"] is None, "check D must be null, not %r" % d_checks[0]["ok"]
    assert d_checks[0]["ok"] is not False
    # and it must say why, or null is just as opaque as false
    assert "NOT PERFORMED" in (d_checks[0]["derivation"] or "")


def test_api_malformed_input_is_400_not_a_verdict():
    """Malformed bytes must be an error, not a failing check.

    Rendering garbage as "checks failed" would tell a caller the ROLL was bad
    when the truth is that we could not read their input."""
    import app
    for bad in ("", "zzzz", "abc"):
        body, code = app.api_verify(bad)
        assert code == 400, "%r should be 400, got %s" % (bad, code)
        assert body["error"] == "malformed"
        assert "checks" not in body, "a malformed input must not produce check results"

    # ★ Well-formed hex of a struct we do not decode is NOT malformed. An LLMQ
    # commitment or a PTXDKG payload is valid data of another type; calling it
    # malformed tells an integrator their bytes are broken when they are not.
    import binascii
    not_ours = "01002e04" + "00" * 60
    body, code = app.api_verify(not_ours)
    assert code == 422, "unknown-but-valid struct should be 422, got %s" % code
    assert body["error"] == "unsupported_payload"
    assert body["payload_hex"] == not_ours, "the anti-oracle guarantee holds on errors too"


def test_footer_reports_reachability_not_configuration():
    """The footer must describe what the node IS DOING, not what was configured.

    ★ The regression this pins is a PUBLIC one. The footer used to say "Txid
    lookup is enabled against <node>" whenever PTX_NODE_RPC was set -- a static
    claim that a reboot silently falsified. A visitor pasting a txid then got an
    error from a page that had just promised it would work. Every other
    false-green in this project misreported to US; this one misreported to a
    stranger, on the page whose entire subject is that claims can be checked.

    Three states, and the middle one is the one that was missing."""
    import os, importlib, app

    os.environ.pop("PTX_NODE_RPC", None)
    importlib.reload(app)
    off = app.footer()
    assert "off" in off, "with no node configured the footer must say lookup is off"

    # Port 1 is not listening; this is 'configured but unreachable'.
    os.environ["PTX_NODE_RPC"] = "http://127.0.0.1:1/"
    os.environ["PTX_NODE_USER"] = "x"
    os.environ["PTX_NODE_PASS"] = "y"
    importlib.reload(app)
    down = app.footer()
    assert "unavailable" in down, "an unreachable node must NOT be described as enabled"
    assert "enabled" not in down, "the word 'enabled' must not survive when the node is down"
    # ...and it must say the checks still work, because they do.
    assert "extraPayload" in down and "checks still run" in down

    # The three states must be textually distinct, or the distinction is cosmetic.
    assert off != down

    os.environ.pop("PTX_NODE_RPC", None)
    importlib.reload(app)


def test_render_check_distinguishes_not_performed_from_failed():
    import app

    base = {"id": "D", "claim": "c", "source": "s", "derivation": "d",
            "computed": None, "claimed": "abc"}

    not_performed = app.render_check(dict(base, ok=None))
    failed        = app.render_check(dict(base, ok=False))
    passed        = app.render_check(dict(base, ok=True))

    # The three states must be textually distinct...
    assert "NOT PERFORMED" in not_performed
    assert "FAIL" in failed
    assert "PASS" in passed

    # ...and "not performed" must NOT claim failure, in any casing.
    assert "FAIL" not in not_performed, (
        "check D rendered as FAIL when it was merely not performed -- on a public "
        "page this reads as the beacon failing verification")
    assert "PASS" not in not_performed

    # ...and visually distinct, or the badge text is the only signal.
    assert 'class="chk n"' in not_performed
    assert 'class="chk f"' in failed
    assert 'class="chk p"' in passed

    # A not-performed check must not show a computed-vs-claimed comparison:
    # there is no computed value, and printing one invites reading it as a
    # mismatch.
    assert "computed" not in not_performed


def test_check_D_is_none_not_false_end_to_end():
    """On a REAL recorded payload, check D must be ok=None and RENDER as
    not-performed.

    ★ Uses app.sniff() rather than a hand-built fields dict or a guessed nType:
    the first attempt built the dict by hand and was rejected by the hash
    helpers, which proved nothing about D at all."""
    import json, os, app, ptx_verify
    here = os.path.dirname(os.path.abspath(__file__))
    fx = json.load(open(os.path.join(here, "fixtures.json")))
    found = None
    for e in fx:
        try:
            dec, _tried = app.sniff(e["payload"])
        except Exception:
            continue
        if not dec:
            continue
        fields = app.ptx_decode.as_dict(dec) if hasattr(app, "ptx_decode") \
                 else __import__("ptx_decode").as_dict(dec)
        d = [c for c in ptx_verify.verify(fields, dec["struct"]) if c["id"] == "D"]
        if d:
            found = d[0]; break
    assert found is not None, "no fixture produced a check D"
    assert found["ok"] is None, "check D must be None (not performed), never False"
    html = app.render_check(found)
    assert "NOT PERFORMED" in html
    assert "FAIL" not in html, (
        "check D rendered as FAIL when merely not performed -- on a public page "
        "that reads as the beacon failing verification")


def test_register_page():
    """The /register walkthrough EMITS a correct protx_register command.

    Delegated to node because the walkthrough's logic is JavaScript. Asserting the
    Python renders the page would be ODC-098 all over again: a check on the
    instrument rather than on the thing itself. The JS is executed against a DOM
    shim and the resulting argv is compared to `Hemis-cli help protx_register`
    from the released v0.3.1-testnet binary.
    """
    import shutil, subprocess
    js = os.path.join(HERE, "test_register_page.js")
    assert os.path.exists(js), "register page test missing"
    node = shutil.which("node")
    # ★ Three-state, not two. No node means NOT PERFORMED -- and a run that did not
    # perform it must not report a pass, per this suite's standing rule.
    assert node, ("NOT PERFORMED: node is not installed, so the /register walkthrough "
                  "was not executed. This is not a pass -- install node or run the "
                  "suite where it exists.")
    r = subprocess.run([node, js], capture_output=True, text=True)
    assert r.returncode == 0, "register walkthrough failed:\n" + r.stdout + r.stderr


# ★★ THE ENTRY POINT LIVES AT THE BOTTOM, AND THAT IS LOAD-BEARING.
# It used to sit ABOVE these test definitions, so when main() ran, the module
# body had only executed as far as that line and the functions below did not
# exist yet. Discovery over globals() therefore found nothing, and the explicit
# list in main() could not name them either. IMPORTING the module ran the whole
# file and made them appear -- so a probe that imported disagreed with the real
# run, which is how this survived being looked at.
if __name__ == "__main__":
    sys.exit(main())
