#!/usr/bin/env python3
"""PTX roll verifier — one page, no node, no index, no database.

★ WHAT THIS IS FOR.  Consensus does not check that a roll's results follow from
its beacon, nor that its beacon follows from a real threshold signature.
PTX_MapBeacon appears in src/rpc/ptx.cpp only and never in validation.cpp;
quorum_sig appears in validation.cpp zero times; the header says as much itself
at src/primitives/transaction.h:540-542.  Until W4-b lands, this page is the
only thing performing those checks -- which is why it exists before any indexer.

★ IT OWNS ITS DATA PATH.  It reaches into no fleet node, no shared credential,
no container.  Paste raw hex and it needs nothing at all.  Txid lookup is
OPTIONAL and only ever talks to a node the operator configures and owns
(PTX_NODE_RPC); with none configured the page says so rather than borrowing
someone else's.  An explorer that answers by privileged access is not an
explorer, it is a private oracle.
"""
import os
import sys
import json
import html
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer   # noqa: E402
from urllib.parse import parse_qs                                      # noqa: E402

from ptx_payload_spec import extract_spec              # noqa: E402
from ptx_decode import decode, as_dict, PayloadError   # noqa: E402
import ptx_verify as V                                 # noqa: E402

# ★ STDLIB ONLY, DELIBERATELY. The page that exists to let a stranger check our
# arithmetic should not first require them to install anything. No Flask, no
# pip, no requirements.txt: python3 app.py and it runs. That is also why the
# checks are pure Python rather than a call into libsodium or blst -- a
# verifier nobody can run is not a verifier.
SPEC = extract_spec()

# Optional, and OFF unless the operator sets it. Must be a node they own.
NODE_RPC  = os.getenv("PTX_NODE_RPC", "")
NODE_USER = os.getenv("PTX_NODE_USER", "")
NODE_PASS = os.getenv("PTX_NODE_PASS", "")
NODE_NET  = os.getenv("PTX_NODE_NET", "ptxtestnet")

CSS = """
:root{--bg:#fbfbfa;--fg:#1a1a18;--mut:#6b6b63;--line:#dcdcd4;--card:#fff;
--ok:#1a7f45;--no:#b3261e;--na:#7a5c00;--acc:#3b4cca}
@media(prefers-color-scheme:dark){:root{--bg:#16161a;--fg:#e8e8e3;--mut:#9a9a92;
--line:#2f2f36;--card:#1e1e24;--ok:#4ec98a;--no:#ff6b60;--na:#e0b341;--acc:#8ea2ff}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);
font:15px/1.55 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.wrap{max-width:980px;margin:0 auto;padding:28px 20px 80px}
.topbar{display:flex;align-items:center;gap:9px;padding:11px 20px;background:var(--card);border-bottom:1px solid var(--line);font-size:14px}.topbar .brand{color:var(--fg);text-decoration:none;font-weight:600;display:inline-flex;align-items:center;gap:8px}.topbar .mark{border-radius:5px;vertical-align:middle}.topbar .crumb{color:var(--mut)}.topbar .here{color:var(--mut)}.topbar .nav{margin-left:auto;color:var(--acc);text-decoration:none}.topbar .nav:hover{text-decoration:underline}.topbar .back{margin-left:14px;color:var(--acc);text-decoration:none}.topbar .back:hover{text-decoration:underline}h1{font-size:21px;margin:0 0 4px}.sub{color:var(--mut);font-size:13.5px;margin:0 0 22px}
textarea{width:100%;min-height:96px;padding:11px;border:1px solid var(--line);border-radius:8px;
background:var(--card);color:var(--fg);font:12.5px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;resize:vertical}
button{margin-top:10px;padding:8px 18px;border:0;border-radius:7px;background:var(--acc);
color:#fff;font-size:14px;font-weight:600;cursor:pointer}
.card{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px 18px;margin:16px 0}
h2{font-size:15px;margin:26px 0 8px}
table{width:100%;border-collapse:collapse;font:12.5px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace}
th,td{text-align:left;padding:5px 9px 5px 0;border-bottom:1px solid var(--line);vertical-align:top}
th{color:var(--mut);font-weight:600;white-space:nowrap}
td.off{color:var(--mut);white-space:nowrap}
td.val{word-break:break-all}
.raw{color:var(--mut);font-size:11.5px;word-break:break-all}
.chk{border-left:3px solid var(--line);padding:11px 0 11px 14px;margin:13px 0}
.chk.p{border-color:var(--ok)}.chk.f{border-color:var(--no)}.chk.n{border-color:var(--na)}
.badge{font-weight:700;font-size:12px;letter-spacing:.04em}
.p .badge{color:var(--ok)}.f .badge{color:var(--no)}.n .badge{color:var(--na)}
.claim{font:12.5px/1.5 ui-monospace,Menlo,monospace;margin:5px 0}
.meta{color:var(--mut);font-size:12.5px;margin:4px 0}
.cmp{font:11.5px/1.5 ui-monospace,Menlo,monospace;word-break:break-all;margin:3px 0}
.note{background:rgba(224,179,65,.11);border-left:3px solid var(--na);padding:9px 12px;
margin:8px 0;font-size:12.5px;border-radius:0 6px 6px 0}
.err{color:var(--no)}
details{margin-top:6px}summary{cursor:pointer;color:var(--mut);font-size:12.5px}
footer{margin-top:34px;padding-top:16px;border-top:1px solid var(--line);color:var(--mut);font-size:12.5px}
code{font:12px ui-monospace,Menlo,monospace;background:rgba(128,128,128,.13);padding:1px 4px;border-radius:3px}
"""

FORM = """<form method=post>
<textarea name=q placeholder="raw payload hex  —  or a txid, if a node is configured"
 spellcheck=false>{q}</textarea><br><button>Verify</button></form>"""


def esc(x):
    return html.escape(str(x))


def rpc(method, params, timeout=12):
    import urllib.request, base64
    body = json.dumps({"jsonrpc": "1.0", "id": "ptxv", "method": method, "params": params}).encode()
    req = urllib.request.Request(NODE_RPC, data=body,
                                 headers={"Content-Type": "application/json",
                                          "Authorization": "Basic " + base64.b64encode(
                                              ("%s:%s" % (NODE_USER, NODE_PASS)).encode()).decode()})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read())
    if d.get("error"):
        raise RuntimeError(d["error"].get("message", "rpc error"))
    return d["result"]


def sniff(payload_hex):
    """Which payload is this? Decide by which struct consumes the bytes EXACTLY.

    ★ Trailing bytes disqualify a candidate rather than being ignored. A
    payload that no struct consumes exactly is reported as unrecognised --
    most likely a newer version carrying a field this build does not know --
    instead of being force-fitted to the nearest match and rendered as fact."""
    tried = []
    for name in ("CProbabilisticTxPayload", "CPTXRollCommitPayload"):
        try:
            d = decode(payload_hex, name, SPEC)
        except PayloadError as e:
            tried.append((name, str(e)))
            continue
        if not d["trailing"]:
            return d, tried
        tried.append((name, "decoded but left %d trailing byte(s)" % (len(d["trailing"]) // 2)))
    return None, tried


def render_fields(d):
    rows = []
    for f in d["fields"]:
        v = f["value"]
        v = ", ".join(str(x) for x in v) if isinstance(v, list) else str(v)
        if v == "":
            v = "<span class=raw>(empty)</span>"
        else:
            v = esc(v)
        raw = f["raw"]
        if len(raw) > 96:
            raw = raw[:96] + "…"
        rows.append(
            "<tr><td class=off>[%d:%d]</td><td>%s</td><td class=off>%s</td>"
            "<td class=val>%s<div class=raw>%s</div></td></tr>"
            % (f["start"], f["end"], esc(f["name"]), esc(f["cpp"]), v, esc(raw)))
    return ("<table><tr><th>bytes</th><th>field</th><th>type</th><th>value / raw</th></tr>"
            + "".join(rows) + "</table>")


def render_check(c):
    if c["ok"] is None:
        cls, badge = "n", "NOT PERFORMED"
    elif c["ok"]:
        cls, badge = "p", "PASS"
    else:
        cls, badge = "f", "FAIL"
    h = ['<div class="chk %s"><span class=badge>%s &nbsp;check %s</span>' % (cls, badge, c["id"])]
    h.append('<div class=claim>%s</div>' % esc(c["claim"]))
    h.append('<div class=meta>%s</div>' % esc(c["derivation"]))
    h.append('<div class=meta>source: <code>%s</code></div>' % esc(c["source"]))
    if c.get("computed") is not None or c.get("claimed") is not None:
        comp = c.get("computed")
        clm = c.get("claimed")
        fmt = lambda x: ", ".join(str(i) for i in x) if isinstance(x, list) else str(x)
        if c["ok"] is not None:
            h.append('<div class=cmp>computed &nbsp;%s</div>' % esc(fmt(comp)))
            h.append('<div class=cmp>in payload %s</div>' % esc(fmt(clm)))
    if c.get("error"):
        h.append('<div class="cmp err">error: %s</div>' % esc(c["error"]))
    if c.get("note"):
        h.append('<div class=note>★ %s</div>' % esc(c["note"]))
    if c.get("preimage_hex"):
        h.append('<details><summary>hash preimage (%d bytes) — check the arithmetic yourself</summary>'
                 '<div class=raw>%s</div></details>' % (len(c["preimage_hex"]) // 2, esc(c["preimage_hex"])))
    h.append('</div>')
    return "".join(h)


# ★★ A HEADER THAT MATCHES THE EXPLORER, ON A PAGE THAT STAYS SEPARATE.
# The explorer links here as real navigation rather than embedding this page in
# an iframe, and that is deliberate: this page's claim is that it is small,
# dependency-free and reproducible by anyone. Wrapped in another application's
# chrome a visitor cannot see where that boundary is -- which is precisely the
# privileged-oracle confusion the split architecture exists to avoid. So: shared
# visual language, its own URL, its own stylesheet, no shared runtime.
# ★ The mark is served from the explorer at /img/page-title-img.png -- ONE
# asset, same origin through nginx, so the two pages cannot drift apart.
HEADER = ("<div class=topbar><a class=brand href='/'>"
          "<img class=mark src='/img/page-title-img.png' alt='' width=26 height=26>"
          "<span>Hemis PTX testnet</span></a>"
          "<span class=crumb>&rsaquo;</span><span class=here>Roll verifier</span>"
          "<a class=nav href='/v2/register'>Register a gamemaster</a>"
          "<a class=back href='/'>&larr; Block explorer</a></div>")


def page(body, q=""):
    return ("<!doctype html><meta charset=utf-8><meta name=viewport "
            "content='width=device-width,initial-scale=1'>"
            "<title>Roll verifier - Hemis PTX testnet</title>"
            "<style>%s</style>%s<div class=wrap><h1>PTX roll verifier</h1>"
            "<p class=sub>Re-derives a roll from the transaction bytes. Checks P, A and B and C "
            "need no node, no index and no chain — paste hex and they run.</p>%s%s%s</div>"
            % (CSS, HEADER, FORM.format(q=esc(q)), body, footer()))


# ★★ REACHABILITY, NOT CONFIGURATION.  This footer used to say "Txid lookup is
# enabled against <node>" whenever PTX_NODE_RPC was SET -- a static assertion
# about a capability that a reboot silently removed.  With the node down the
# sentence was simply false, and a visitor who pasted a txid got an error from a
# page that had just told them it would work.
#
# ★ That is the false-green shape aimed at a STRANGER rather than at us, on the
# one artefact whose whole purpose is demonstrating that claims can be checked.
# A page about verifiability must not assert its own capabilities unverified.
#
# Probed, not assumed; cached briefly so a reload does not hammer the node, and
# short-timeout so a hung node degrades the sentence rather than the page.
_node_cache = {"t": 0.0, "up": None, "height": None}

def node_status():
    """-> (up, height).  up is None when no node is configured at all."""
    if not NODE_RPC:
        return None, None
    now = time.time()
    if now - _node_cache["t"] < 15:
        return _node_cache["up"], _node_cache["height"]
    up, height = False, None
    try:
        height = rpc("getblockcount", [], timeout=4)
        up = True
    except Exception:
        up = False
    _node_cache.update(t=now, up=up, height=height)
    return up, height


def footer():
    up, height = node_status()
    if up is True:
        node = ("Txid lookup is <b>available</b> against <code>%s</code> — a node this page's "
                "operator owns, currently at height %s. It is never used for the checks "
                "themselves." % (esc(NODE_RPC), esc(height)))
    elif up is False:
        node = ("Txid lookup is <b>unavailable</b> right now: the node at <code>%s</code> is not "
                "answering. <b>Paste raw <code>extraPayload</code> hex and the checks still run</b> "
                "— they need no node, no index and no chain, which is the point. Only the "
                "convenience of looking a payload up by txid is affected." % esc(NODE_RPC))
    else:
        node = ("Txid lookup is <b>off</b>: no node is configured (<code>PTX_NODE_RPC</code>). "
                "Paste raw payload hex — the checks need nothing else. This page deliberately does "
                "not borrow another operator's node: an answer that depends on privileged access "
                "is not verification.")
    return ("<footer><p>%s</p><p>Field layout is read from "
            "<code>SERIALIZE_METHODS</code> in <code>src/primitives/transaction.h</code>, not "
            "hand-written here, and <code>test_decoder.py</code> fails if the two ever diverge.</p>"
            "</footer>" % node)


NOT_DERIVABLE = """<div class=note>★ <b>What this page will never show, and why.</b>
Per-round gamemaster participation — who was asked to sign and stayed silent — and the timing
between commitment and response are <b>not derivable from chain data by anyone</b>, including us.
They live in <code>g_ptx_rounds</code>, an in-memory map (<code>src/ptx/ptx_commit_reveal.cpp:12</code>)
filled by a node's own fan-out participation, lost on restart and empty on any node that did not
take part. The honest on-chain proxy is <code>quorum_members</code> above: it names the members
whose shares composed this signature, which answers <i>who signed</i>. A participation column
sourced from a fleet node would be a privileged answer wearing a public label.</div>"""


def handle(q):
    """q is the submitted text (empty for a plain GET). -> full HTML page."""
    q = (q or "").strip()
    if not q:
        return page("")

    payload_hex, prov = q, "pasted raw payload"
    compact = "".join(q.split())
    if len(compact) == 64 and all(ch in "0123456789abcdefABCDEF" for ch in compact):
        if not NODE_RPC:
            return page('<div class="card err">That looks like a txid, but txid lookup is off — '
                        'no node is configured. Paste the raw <code>extraPayload</code> hex instead; '
                        'the checks do not need a node.</div>', q)
        # ★ Same correction as the footer: say the node is down BEFORE trying, so
        # the message names the cause rather than surfacing a transport error.
        if node_status()[0] is False:
            return page('<div class="card err">That looks like a txid, but the node this page '
                        'uses for lookups is <b>not answering</b>, so it cannot be resolved right '
                        'now. Paste the raw <code>extraPayload</code> hex instead — the A/B/C '
                        'checks do not use the node and are unaffected.</div>', q)
        try:
            tx = rpc("getrawtransaction", [compact, 1])
            payload_hex = tx.get("extraPayload") or ""
            if not payload_hex:
                return page('<div class="card err">That transaction carries no '
                            '<code>extraPayload</code> — it is not a PTX special transaction.</div>', q)
            prov = "txid %s (type %s)" % (compact[:16] + "…", tx.get("type"))
        except Exception as e:                                # noqa: BLE001
            return page('<div class="card err">node lookup failed: %s</div>' % esc(e), q)

    d, tried = sniff(payload_hex)
    if d is None:
        rows = "".join("<li><code>%s</code>: %s</li>" % (esc(n), esc(m)) for n, m in tried)
        return page('<div class="card err"><b>Not a payload this build recognises.</b>'
                    '<ul>%s</ul><p>A payload that no struct consumes exactly is most likely a newer '
                    'version carrying a field this build does not know. It is reported rather than '
                    'force-fitted to the nearest match.</p></div>' % rows, q)

    fl = as_dict(d)
    checks = V.verify(fl, d["struct"])
    body = ['<div class=card><b>%s</b> — %d bytes, fully consumed, no trailing data'
            '<div class=meta>%s</div></div>' % (esc(d["struct"]), d["size"], esc(prov))]
    body.append("<h2>Decoded, with byte boundaries</h2>" + render_fields(d))
    body.append("<h2>Independent verification</h2>")
    if d["struct"] == "CPTXRollCommitPayload":
        body.append('<div class=note>★ This is the roll <b>commitment</b>, which is sig-less and '
                    'results-less by design (BUG-032, fund-then-sign: a roll\'s threshold signature '
                    '<i>is</i> its result, so the commitment must be funded and broadcast before the '
                    'quorum signs). Checks B and C have nothing to verify here — its settle sibling, '
                    'joined by <code>round_seed</code> in the same block, carries the reveal.</div>')
    body.extend(render_check(c) for c in checks)
    body.append(NOT_DERIVABLE)
    return page("".join(body), q)


# ===========================================================================
# JSON API — the VERIFICATION half (ODC-099). Discovery endpoints are
# deliberately absent; see the register.
#
# ★★ EVERY RESPONSE CARRIES payload_hex, AND THAT IS NOT REDUNDANT.
# Returning decoded values without the bytes they came from would make this an
# ORACLE: a caller would have to trust our answer because they could not
# reproduce it. With the payload present, any caller can re-derive every check
# independently -- which is the entire architecture, and the reason proxy.py was
# retired. DO NOT REMOVE IT TO "SIMPLIFY" THE RESPONSE.
#
# ★ Three-state honesty, same as the HTML: a check that was not performed is
# "ok": null with a reason, NEVER false. A JSON false where the answer is "we
# did not check" is the machine-readable version of rendering None as FAIL.
# ===========================================================================
API_PREFIX = "/api/v1"


def _api_checks(fields, struct_name):
    out = []
    for c in V.verify(fields, struct_name):
        out.append({
            "id": c["id"],
            "ok": c["ok"],                      # true / false / null(not performed)
            "claim": c.get("claim"),
            "source": c.get("source"),
            "derivation": c.get("derivation"),
            "computed": c.get("computed"),
            "claimed": c.get("claimed"),
        })
    return out


def _api_decode(payload_hex):
    """-> (body, status). Raises nothing; malformed input is a 400 with a locus."""
    d, tried = sniff(payload_hex)
    if d is None:
        # ★ THREE STATES HERE TOO, and the distinction is not cosmetic.
        # "malformed" means YOUR BYTES ARE BROKEN. A PTXDKG or an LLMQ
        # commitment is neither broken nor ours: it is a valid special tx whose
        # payload this decoder does not model. Calling that malformed tells an
        # integrator their input is bad when the truth is that we do not decode
        # this type -- the same error as rendering "not performed" as FAIL.
        return ({"error": "unsupported_payload",
                 "message": "these bytes are not a PTX roll payload this service decodes; "
                            "they may belong to another special-transaction type",
                 "decodes": ["CProbabilisticTxPayload", "CPTXRollCommitPayload"],
                 "tried": [{"struct": n, "why": w} for n, w in tried],
                 "payload_hex": payload_hex}, 422)
    fields = {f["name"]: f["value"] for f in d["fields"]}
    return ({
        "struct": d["struct"],
        "payload_hex": payload_hex,            # ★ see the block comment above
        "size": d["size"],
        "fields": [{"name": f["name"], "type": f.get("type"), "value": f["value"],
                    "byte_start": f.get("start"), "byte_end": f.get("end")}
                   for f in d["fields"]],
        "checks": _api_checks(fields, d["struct"]),
    }, 200)


def api_verify(payload_hex):
    h = "".join((payload_hex or "").split())
    if not h:
        return ({"error": "malformed", "message": "empty payload"}, 400)
    if len(h) % 2 or any(c not in "0123456789abcdefABCDEF" for c in h):
        return ({"error": "malformed", "message": "not hex"}, 400)
    return _api_decode(h)


def api_tx(txid):
    if not NODE_RPC:
        return ({"error": "not_performed",
                 "message": "txid lookup is not configured on this instance; "
                            "POST the raw payload to /api/v1/verify instead"}, 501)
    try:
        tx = rpc("getrawtransaction", [txid, 1], timeout=8)
    except Exception as e:
        return ({"error": "not_found", "message": str(e), "txid": txid}, 404)
    ph = tx.get("extraPayload") or ""
    if not ph:
        return ({"error": "not_found",
                 "message": "transaction carries no extraPayload; it is not a PTX special tx",
                 "txid": txid, "tx_type": tx.get("type")}, 404)
    body, code = _api_decode(ph)
    body["txid"] = txid
    body["tx_type"] = tx.get("type")
    body["confirmations"] = tx.get("confirmations")
    return (body, code)


def api_commitment(txid):
    """Settled, or not. ★ The chain records exactly ONE failure mode.

    rpc/ptx.cpp:292 broadcasts the commitment BEFORE the fan-out, so a quorum
    that misses threshold leaves a PTXROLLCOMMIT with no settle and the fee
    forfeit. Funding failures (-32050, ptx_mempool.cpp:98) and no-quorum errors
    never reach the chain at all, so no API can show them.

    ★★ AND "past its window" IS NOT A CONSENSUS FACT. specialtx_validation.cpp
    :950 enforces only nExpiryHeight >= nSeedHeight; BUG-034 retired the
    same-block mandate and the comment is explicit that no upper bound is
    enforced. nExpiryHeight is declared by the COMMITTER. So this endpoint
    reports settled-or-not (a hard fact, from the UTXO set) and the arithmetic
    against the declared expiry, LABELLED as declared -- it never returns a
    verdict of "failed", which the chain does not reach.
    """
    if not NODE_RPC:
        return ({"error": "not_performed",
                 "message": "commitment status needs a node; not configured here"}, 501)
    try:
        tx = rpc("getrawtransaction", [txid, 1], timeout=8)
    except Exception as e:
        return ({"error": "not_found", "message": str(e), "txid": txid}, 404)
    ph = tx.get("extraPayload") or ""
    d, _ = sniff(ph) if ph else (None, None)
    if d is None or d["struct"] != "CPTXRollCommitPayload":
        return ({"error": "not_found",
                 "message": "not a PTXROLLCOMMIT",
                 "txid": txid, "struct": (d or {}).get("struct")}, 404)
    fields = {f["name"]: f["value"] for f in d["fields"]}

    # ★ The settle SPENDS an output of the commitment (BUG-032 2c coin-chain),
    # so an UNSPENT output means no settle exists. O(1) against the UTXO set --
    # no scanning, no index beyond what the node already keeps.
    unspent = []
    for n in range(len(tx.get("vout") or [])):
        try:
            if rpc("gettxout", [txid, n, True], timeout=8) is not None:
                unspent.append(n)
        except Exception:
            pass
    settled = (len(unspent) == 0)

    try:
        height = rpc("getblockcount", [], timeout=6)
    except Exception:
        height = None
    expiry = fields.get("nExpiryHeight")
    past = None
    if height is not None and isinstance(expiry, int):
        past = height > expiry

    return ({
        "txid": txid,
        "struct": d["struct"],
        "payload_hex": ph,                      # ★ see the block comment above
        "settled": settled,
        "unspent_vouts": unspent,
        "declared_expiry_height": expiry,
        "nSeedHeight": fields.get("nSeedHeight"),
        "chain_height": height,
        "past_declared_expiry": past,
        "note": ("nExpiryHeight is declared by the committer. Consensus enforces no upper "
                 "bound (specialtx_validation.cpp:950, BUG-034), so an unsettled commitment "
                 "past it is NOT a consensus-confirmed failure -- it is the caller's own "
                 "declared window having elapsed. settled=false is a hard fact from the UTXO "
                 "set; past_declared_expiry is arithmetic on a self-declared value."),
        "checks": _api_checks(fields, d["struct"]),
    }, 200)


def api_route(method, path, body_hex):
    """-> (obj, status) or None when the path is not an API path."""
    if not path.startswith(API_PREFIX):
        return None
    rest = path[len(API_PREFIX):]
    if method == "POST" and rest == "/verify":
        return api_verify(body_hex)
    if method == "GET" and rest.startswith("/tx/"):
        return api_tx(rest[4:].strip("/"))
    if method == "GET" and rest.startswith("/commitment/"):
        return api_commitment(rest[len("/commitment/"):].strip("/"))
    return ({"error": "not_found", "message": "no such endpoint",
             "endpoints": ["POST %s/verify" % API_PREFIX,
                           "GET %s/tx/<txid>" % API_PREFIX,
                           "GET %s/commitment/<txid>" % API_PREFIX]}, 404)


# ===========================================================================
# /register — a GUIDED WALKTHROUGH for protx_register. Not a form.
#
# ★★ A form assumes you already have the values. This asks for a name, tells you
# which commands to run and ON WHICH MACHINE, takes the results back, and only
# then emits the registration command. Step 5 (the compound ptxnodeid) stays
# hidden until step 4 is done, because that is the step every prior account of
# this command missed -- including both of ours.
#
# ★★ CLIENT-SIDE ONLY, AND ENFORCED BY CSP RATHER THAN BY DISCIPLINE.
# This page ships connect-src 'none', so the BROWSER blocks any fetch/XHR even
# if someone later adds one. It composes strings; it never talks to a node. The
# moment it did, it would become an endpoint on a surface deliberately kept
# read-only and wallet-less -- the property retiring proxy.py established.
# ===========================================================================
REGISTER_CSP = ("default-src 'none'; style-src 'unsafe-inline'; "
                "script-src 'unsafe-inline'; connect-src 'none'; form-action 'none'")

REGISTER_HTML = r"""<!doctype html><meta charset=utf-8>
<meta name=viewport content='width=device-width,initial-scale=1'>
<title>Register a gamemaster - Hemis PTX testnet</title>
<style>
:root{--bg:#fbfbfa;--fg:#1a1a18;--mut:#6b6b63;--line:#dcdcd4;--card:#fff;
--ok:#1a7f45;--no:#b3261e;--acc:#3b4cca;--wal:#2563eb;--gm:#c2410c}
@media(prefers-color-scheme:dark){:root{--bg:#16161a;--fg:#e8e8e3;--mut:#9a9a92;
--line:#2f2f36;--card:#1e1e24;--ok:#4ec98a;--no:#ff6b60;--acc:#8ea2ff;--wal:#7ba6ff;--gm:#e2762c}}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);
font:15px/1.55 ui-sans-serif,system-ui,-apple-system,Segoe UI,Roboto,sans-serif}
.topbar{display:flex;align-items:center;gap:9px;padding:11px 20px;background:var(--card);
border-bottom:1px solid var(--line);font-size:14px}
.topbar .brand{color:var(--fg);text-decoration:none;font-weight:600;display:inline-flex;align-items:center;gap:8px}
.topbar .mark{border-radius:5px}.topbar .crumb,.topbar .here{color:var(--mut)}
.topbar .back{margin-left:auto;color:var(--acc);text-decoration:none}
.wrap{max-width:860px;margin:0 auto;padding:26px 20px 90px}
h1{font-size:21px;margin:0 0 4px}.sub{color:var(--mut);font-size:13.5px;margin:0 0 20px}
.step{background:var(--card);border:1px solid var(--line);border-radius:10px;padding:16px 18px;margin:0 0 16px}
.step.off{opacity:.45}
.step h2{font-size:15px;margin:0 0 10px;display:flex;align-items:center;gap:9px;flex-wrap:wrap}
.n{display:inline-flex;width:23px;height:23px;border-radius:50%;background:var(--acc);color:#fff;
align-items:center;justify-content:center;font-size:12.5px;font-weight:700;flex:0 0 auto}
label{display:block;font-size:13px;color:var(--mut);margin:10px 0 4px}
input[type=text],input:not([type]){width:100%;padding:8px 10px;border:1px solid var(--line);border-radius:7px;background:var(--bg);
color:var(--fg);font:13px ui-monospace,SFMono-Regular,Menlo,monospace}
.chk{display:flex;align-items:flex-start;gap:8px;margin:12px 0 2px;font-size:13px;color:var(--fg)}
.chk input{margin-top:3px;flex:0 0 auto}
.host{display:inline-block;font-size:11px;font-weight:700;letter-spacing:.04em;padding:2px 8px;
border-radius:20px;text-transform:uppercase}
.host.w{background:color-mix(in srgb,var(--wal) 16%,transparent);color:var(--wal);border:1px solid var(--wal)}
.host.g{background:color-mix(in srgb,var(--gm) 16%,transparent);color:var(--gm);border:1px solid var(--gm)}
.cmd{position:relative;margin:8px 0}
.cmd pre{margin:0;padding:11px 74px 11px 12px;background:var(--bg);border:1px solid var(--line);
border-radius:7px;overflow-x:auto;font:12.5px/1.5 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:pre-wrap;word-break:break-all}
.cmd button{position:absolute;top:7px;right:7px;padding:4px 10px;border:1px solid var(--line);
border-radius:6px;background:var(--card);color:var(--fg);font-size:12px;cursor:pointer}
.err{color:var(--no);font-size:12.5px;margin:5px 0 0;font-weight:600}
.good{color:var(--ok);font-size:12.5px;margin:5px 0 0}
.warn{background:color-mix(in srgb,var(--no) 9%,transparent);border-left:3px solid var(--no);
padding:9px 12px;border-radius:5px;font-size:13px;margin:10px 0}
.info{background:color-mix(in srgb,var(--acc) 9%,transparent);border-left:3px solid var(--acc);
padding:9px 12px;border-radius:5px;font-size:13px;margin:10px 0}
.wait{background:color-mix(in srgb,var(--gm) 10%,transparent);border-left:3px solid var(--gm);
padding:9px 12px;border-radius:5px;font-size:13px;margin:10px 0}
.note{color:var(--mut);font-size:12.5px;margin:6px 0 0}
footer{margin-top:26px;color:var(--mut);font-size:12.5px;border-top:1px solid var(--line);padding-top:14px}
</style>
<div class=topbar><a class=brand href='/'><img class=mark src='/img/page-title-img.png' alt='' width=26 height=26><span>Hemis PTX testnet</span></a><span class=crumb>&rsaquo;</span><span class=here>Register a gamemaster</span><a class=back href='/v2'>&larr; Roll verifier</a></div>
<div class=wrap>
<h1>Register a gamemaster</h1>
<p class=sub>Walks the <code>protx_register</code> sequence in the order the chain requires, and composes
the exact commands. Two machines are involved and the page says which is which at every step.</p>

<div class=warn><b>This page composes text. It checks nothing on chain.</b>
It cannot tell whether an address exists, whether a BLS key is yours, whether the collateral confirmed,
or whether a registration succeeded. A completed walkthrough is a correctly-<i>shaped</i> command,
not a validated registration. Nothing you type here leaves your browser.</div>

<div class=step id=s0>
  <h2><span class=n>0</span> Before you start</h2>
  <p class=note><b>You need two machines already installed</b> &mdash; a wallet host and this gamemaster
  host &mdash; and <b>coins in the wallet</b>. This page composes the registration; it does not install
  anything and cannot get you funded.</p>
  <ul class=note style="margin:6px 0 0 18px">
    <li><b>Not installed yet?</b> Follow
      <a href="https://github.com/vileda-hemis/Hemis-PTX-MVP1/blob/v0.3.4-testnet/testnet/operator/OPERATOR_ONEPAGER.md">OPERATOR_ONEPAGER.md</a>
      first. It is the authority on prerequisites; this page deliberately does not restate them.</li>
    <li><b>No coins yet?</b> Ask the coordinator in <b>#testnet</b> for
      <b>(N&nbsp;&times;&nbsp;100)&nbsp;+&nbsp;500&nbsp;HMS</b> and post your funding address. Wait for
      <code>Hemis-cli getbalance</code> to show it &mdash; it counts confirmed coins only.</li>
    <li><b>Already have both?</b> Carry on to step 1.</li>
  </ul>
</div>

<div class=step id=s1>
  <h2><span class=n>1</span> Name this gamemaster</h2>
  <label>A short label. Every address below is named after it, so registering four gamemasters cannot
  reuse an address by accident. This label also becomes the gamemaster's PTX identity on chain.</label>
  <input id=name placeholder="gm01" autocomplete=off spellcheck=false>
  <div id=namemsg></div>
</div>

<div class="step off" id=s2>
  <h2><span class=n>2</span> Send the collateral first <span class="host w">wallet host</span></h2>
  <p class=note>This comes first because <code>protx_register</code> needs a collateral output that is
  already <b>confirmed</b>. Start it now and it matures while you do steps 3 and 4.</p>
  <div id=cmds2a></div>
  <label>Paste the collateral address that returned</label>
  <input id=coladdr placeholder="y..." autocomplete=off spellcheck=false>
  <div id=cmds2b></div>
</div>

<div class="step off" id=s3>
  <h2><span class=n>3</span> Make the keys while it confirms</h2>
  <p class=note>Nothing here touches the collateral, so do it during the wait.</p>
  <div id=cmds3></div>
</div>

<div class="step off" id=s4>
  <h2><span class=n>4</span> Collect the values <span class="host w">wallet host</span></h2>
  <p class=note>Run this once the send has at least one confirmation. If it returns an empty list
  <code>[]</code>, the block has not arrived yet &mdash; wait and run it again.</p>
  <div id=cmds4></div>
  <label>Collateral <code>txid</code> &mdash; from the entry showing exactly <code>100.00000000</code></label>
  <input id=ctxid placeholder="0123...cdef" autocomplete=off spellcheck=false>
  <label>Collateral output index (<code>vout</code>)</label>
  <input id=cvout placeholder="0" autocomplete=off spellcheck=false>
  <label>Owner address &mdash; must differ from the collateral address</label>
  <input id=owner autocomplete=off spellcheck=false>
  <label>Payout address &mdash; where gamemaster block rewards go</label>
  <input id=payout autocomplete=off spellcheck=false>
  <div class=chk><input type=checkbox id=samepay checked>
    <span>Use the payout address for PTX lottery rewards too <b>(recommended)</b></span></div>
  <div id=ptxwrap></div>
  <label>BLS <b>public</b> key <span class="host g">gamemaster host</span> &mdash; the <code>public</code>
  half only. The secret never leaves that machine.</label>
  <input id=blspub placeholder="bls-pk-ptx1..." autocomplete=off spellcheck=false>
  <label>The gamemaster's address and port, as peers will reach it <span class="host g">gamemaster host</span>
  &mdash; a <b>global IPv6</b> address, <b>bracketed</b>, then the port. IPv4 as well on the host is
  fine; what you register must be IPv6, because signing goes directly to this address and no relay
  bridges address families. Addresses starting <code>fd</code> are ULA and do not count.</label>
  <input id=ipport placeholder="[2a07:...:9400]:29994" autocomplete=off spellcheck=false>
  <div id=s4msg></div>
</div>

<div class="step off" id=s5>
  <h2><span class=n>5</span> Register <span class="host w">wallet host</span></h2>
  <div id=cmd5></div>
  <div id=cmd5note></div>
</div>

<div class="step off" id=s6>
  <h2><span class=n>6</span> Finish the gamemaster's config <span class="host g">gamemaster host</span></h2>
  <p class=note>Registration returns a <b>compound</b> identifier &mdash; your label plus a suffix the chain
  derives from the collateral outpoint. The gamemaster needs that exact value, not the bare label.</p>
  <label>Paste the <code>ptxNodeId</code> from the registration response</label>
  <input id=compound placeholder="gm01:d7b70a85" autocomplete=off spellcheck=false>
  <div id=cmd6></div>
</div>

<footer>Composed locally. This page has no network access &mdash; its content-security policy sets
<code>connect-src 'none'</code>, so the browser blocks any call it might try to make.
Argument order and rules are taken from <code>Hemis-cli help protx_register</code> in the released
<code>v0.3.1-testnet</code> binary.</footer>
</div>
<script>
var RESERVED = ["admin","system","null","none","gm","gamemaster","node","default","test"];
function $(id){return document.getElementById(id)}
function esc(t){return String(t).replace(/[&<>"]/g,function(c){return {"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]})}

// Name the rule that failed. "bad protx id suffix" told an operator nothing.
function labelError(v){
  if(!v) return "Enter a name.";
  if(v.length<3||v.length>24) return "Must be 3-24 characters — yours is "+v.length+".";
  var bad=v.match(/[^a-zA-Z0-9_-]/);
  if(bad) return "Contains “"+bad[0]+"”. Only letters, digits, hyphen and underscore are allowed.";
  if(/^[-_]/.test(v)) return "Cannot start with “"+v[0]+"”.";
  if(/[-_]$/.test(v)) return "Cannot end with “"+v[v.length-1]+"”.";
  if(/^[0-9]+$/.test(v)) return "Cannot be all digits.";
  if(RESERVED.indexOf(v.toLowerCase())>=0) return "“"+v+"” is a reserved word ("+RESERVED.join(", ")+").";
  return null;
}
function cmdBlock(host,cmd){
  var cls = host==="gm" ? "g" : "w";
  var who = host==="gm" ? "gamemaster host" : "wallet host";
  return '<div class=cmd><span class="host '+cls+'">'+who+'</span>'+
         '<pre>'+esc(cmd)+'</pre><button onclick="cp(this)">copy</button></div>';
}
function cp(b){
  var t=b.parentNode.querySelector("pre").textContent;
  navigator.clipboard.writeText(t).then(function(){b.textContent="copied";
    setTimeout(function(){b.textContent="copy"},1200)});
}
function off(){for(var i=0;i<arguments.length;i++){$(arguments[i]).className="step off"}}
function render(){
  var n=$("name").value.trim();
  var e=labelError(n);
  $("namemsg").innerHTML = n===""? "" : (e? '<p class=err>'+esc(e)+'</p>' : '<p class=good>Valid label.</p>');
  if(n===""||e){ off("s2","s3","s4","s5","s6"); $("cmds2a").innerHTML=""; return; }

  // --- step 2: collateral first ---
  $("s2").className="step";
  $("cmds2a").innerHTML = cmdBlock("wal",'Hemis-cli getnewaddress "'+n+'-collateral"');
  var ca=$("coladdr").value.trim();
  if(!ca){ $("cmds2b").innerHTML=""; off("s3","s4","s5","s6"); return; }
  $("cmds2b").innerHTML =
    cmdBlock("wal",'Hemis-cli sendtoaddress "'+ca+'" 100') +
    '<p class=note>Exactly 100. If the amount is wrong the registration fails with '+
    '<code>invalid value</code> followed by a satoshi figure &mdash; and the number you needed is not in the message.</p>'+
    '<div class=wait><b>Now wait for one confirmation.</b> Blocks are about a minute apart. '+
    'Do step 3 while you wait.</div>';

  // --- step 3: keys, during the wait ---
  $("s3").className="step";
  $("cmds3").innerHTML =
    cmdBlock("wal",'Hemis-cli getnewaddress "'+n+'-owner"') +
    cmdBlock("wal",'Hemis-cli getnewaddress "'+n+'-payout"') +
    cmdBlock("gm", 'Hemis-cli generateblskeypair') +
    '<p class=note>Keep the BLS <b>secret</b> on the gamemaster host and put it in that host’s '+
    '<code>Hemis.conf</code>. Only the public half is used below.</p>';

  // --- step 4: collect ---
  $("s4").className="step";
  $("cmds4").innerHTML = cmdBlock("wal",'Hemis-cli listunspent 1 9999999 "[\\"'+ca+'\\"]"');
  var same=$("samepay").checked;
  $("ptxwrap").innerHTML = same ? '' :
    '<label>PTX payment address &mdash; leave empty to opt out of the lottery</label>'+
    '<input id=ptxpay autocomplete=off spellcheck=false value="'+esc(window._pp||"")+'">';
  if(!same){ var pe=$("ptxpay"); if(pe){ pe.addEventListener("input",function(){window._pp=pe.value;render()}); } }

  var v={ctxid:$("ctxid").value.trim(),cvout:$("cvout").value.trim(),owner:$("owner").value.trim(),
         payout:$("payout").value.trim(),blspub:$("blspub").value.trim(),ipport:$("ipport").value.trim()};
  var ptxpay = same ? v.payout : (window._pp||"").trim();
  var msgs=[];
  if(v.ctxid && !/^[0-9a-fA-F]{64}$/.test(v.ctxid)) msgs.push("The txid should be 64 hex characters — yours is "+v.ctxid.length+".");
  if(v.cvout && !/^[0-9]+$/.test(v.cvout)) msgs.push("The output index is a plain number, with no quotes or brackets.");
  if(v.blspub && v.blspub.indexOf("bls-sk")===0) msgs.push("That is the SECRET key. Use the public half — it starts bls-pk.");
  if(v.owner && v.owner===ca) msgs.push("The owner address must differ from the collateral address.");
  // ★★ THE CHECK HERE USED TO BE VACUOUS. It asked whether the string contained
  // ANY colon -- and a bracketed IPv6 address is made of colons, so it could never
  // fire on the only address family this page accepts. A registration with no port
  // is ACCEPTED: the chain stores the chainparams default :29993, which nothing
  // listens on. The gamemaster then installs cleanly, syncs, reports Ready, and
  // refuses to arm with "Local address ... does not match the address from ProTx".
  // ★ The remedy is protx_update_service from the WALLET host, which requires the
  // BLS SECRET to travel there -- the one movement this architecture otherwise
  // avoids. Found live on ptxtestnet-03, 2026-09-04.
  if(v.ipport && /^\[[^\]]+\]$/.test(v.ipport))
    msgs.push("Missing the port. This does not fail at registration \u2014 the chain stores the "+
              "default :29993, which nothing listens on, and the gamemaster then refuses to "+
              "arm. Write it as \u201c[\u2026]:29994\u201d.");
  // ★ Gamemasters must register a global IPv6 address: signing is point-to-point
  // and no relay bridges address families, so an IPv4 registration is invisible.
  if(/^\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}:\d+$/.test(v.ipport))
    msgs.push("That is an IPv4 address. Gamemasters must register a global IPv6 address — signing "+
              "goes directly to it and no relay bridges address families, so an IPv4 gamemaster is "+
              "invisible to the network.");
  if(/^\[?[Ff][CcDd]/.test(v.ipport))
    msgs.push("That is a ULA (fc00::/7, the addresses starting fd). Linux calls its scope “global” "+
              "but it is not routable — no peer outside your own network can reach it.");
  if(v.ipport && v.ipport.indexOf(":")>=0 && v.ipport.indexOf("[")<0 &&
     (v.ipport.match(/:/g)||[]).length>1)
    msgs.push("Bracket the address, then the port: […]:29994. Without brackets the port colon is "+
              "ambiguous with the address’s own colons.");
  // ★ A non-29994 port is a WARNING and must NOT block: vps-install.sh legitimately
  // assigns 29996, 29998, ... to the 2nd and later gamemasters sharing one host, so
  // requiring 29994 exactly would reject configurations this project's own bootstrap
  // creates. Errors gate step 5; warnings do not.
  var warns=[];
  var pm=/^\[[^\]]+\]:(\d+)$/.exec(v.ipport||"");
  if(pm && pm[1]!=="29994")
    warns.push("Port "+pm[1]+" is not the documented 29994. That is correct only for a second "+
               "or later gamemaster sharing ONE host, where vps-install.sh assigns 29996, 29998, \u2026 "+
               "One gamemaster per host uses 29994 \u2014 check port= in that host\u2019s Hemis.conf. "+
               "This value must equal it exactly or the node will not arm.");
  $("s4msg").innerHTML = (msgs.length? '<p class=err>'+msgs.map(esc).join("<br>")+'</p>':"")
                       + (warns.length? '<p class=note>'+warns.map(esc).join("<br>")+'</p>':"");
  var filled = v.ctxid&&v.cvout&&v.owner&&v.payout&&v.blspub&&v.ipport;
  if(!filled || msgs.length){ off("s5","s6"); $("cmd5").innerHTML=""; return; }

  // --- step 5: register ---
  $("s5").className="step";
  var c='Hemis-cli protx_register \\\n'+
    '  "'+v.ctxid+'" \\\n'+'  '+v.cvout+' \\\n'+'  "'+v.ipport+'" \\\n'+'  "'+v.owner+'" \\\n'+
    '  "'+v.blspub+'" \\\n'+'  "" \\\n'+'  "'+v.payout+'" \\\n'+'  0 \\\n'+'  "" \\\n'+
    '  "'+ptxpay+'" \\\n'+'  "'+n+'"';
  var J='{"jsonrpc":"1.0","id":"ptx","method":"protx_register","params":['+
    '"'+v.ctxid+'",'+v.cvout+',"'+v.ipport+'","'+v.owner+'","'+v.blspub+'","","'+v.payout+'",0,"","'+
    ptxpay+'","'+n+'"]}';
  var curl="curl -s --user <rpcuser>:<rpcpassword> \\\n"+
    "  --data-binary '"+J+"' \\\n"+
    "  -H 'content-type: text/plain;' http://127.0.0.1:29995/";
  $("cmd5").innerHTML=cmdBlock("wal",c)+
    '<div class=info><b>BUG-059 — fixed in v0.3.2-testnet.</b> '+
    '<code>Hemis-cli</code> sends every argument as a string, and <code>protx_register</code> is '+
    'missing from the table that converts them back, so the collateral index arrives as '+
    '<code>"0"</code> and the node answers <b>“JSON value is not an integer as expected”</b>. '+
    'On <b>v0.3.1-testnet and earlier</b> the command above fails with '+
    '<b>“JSON value is not an integer as expected”</b>: <code>Hemis-cli</code> sent every argument '+
    'as a string and <code>protx_register</code> was missing from the table that converts them back, '+
    'so the collateral index arrived as <code>"0"</code>. <b>Check your version with '+
    '<code>Hemisd -version</code>.</b> If you are on v0.3.2 or later, ignore the command below. '+
    'If you are on an older build, it does the same job by sending real JSON types.</div>'+
    cmdBlock("wal",curl)+
    '<p class=note>Substitute the <code>rpcuser</code> and <code>rpcpassword</code> from your '+
    'wallet host’s <code>Hemis.conf</code>. This page never sees them — it composes text and has '+
    'no network access. Note <code>'+v.cvout+'</code> appears unquoted: that is the whole point.</p>';
  $("cmd5note").innerHTML =
    (ptxpay ? '' :
      '<div class=info><b>Registering without a PTX payment address.</b> Argument 10 is empty, so this '+
      'gamemaster earns block rewards normally but is not eligible for PTX lottery payouts. '+
      'It cannot be changed later &mdash; re-registering is the only fix. Tick the box in step 4 if that '+
      'was not deliberate.</div>') +
    '<p class=note>Eleven arguments. The empty strings and the <code>0</code> are positional &mdash; drop one '+
    'and every argument after it shifts, which is how the PTX payment address and the node id get silently '+
    'lost. Argument 6 is empty on purpose: the voting address defaults to the owner address.</p>';

  // --- step 6: config line ---
  $("s6").className="step";
  var comp=$("compound").value.trim();
  if(!comp){ $("cmd6").innerHTML=""; return; }
  if(comp.indexOf(":")<0){
    $("cmd6").innerHTML='<p class=err>That looks like the bare label. The response returns '+
      '<code>label:suffix</code> — paste the whole thing, suffix included.</p>'; return; }
  if(comp.split(":")[0]!==n){
    $("cmd6").innerHTML='<p class=err>That starts with “'+esc(comp.split(":")[0])+
      '” but this walkthrough registered “'+esc(n)+'”. Paste the value from this gamemaster’s response.</p>'; return; }
  $("cmd6").innerHTML='<p class=note>Put <b>all three</b> lines under <code>[ptxtestnet]</code> in the '+
    'gamemaster’s <code>~/.Hemis/Hemis.conf</code>, then restart. Two of them are already in the file, '+
    'commented out by <code>install.sh</code> &mdash; uncomment those rather than adding duplicates.</p>'+
    '<div class=warn><b>All three go in together, then one restart.</b> <code>gamemaster=1</code> with no '+
    'key makes the daemon <b>refuse to start</b>, so do not add it on its own. And a gamemaster that is '+
    'registered but holds no key registers fine, syncs fine, reports <code>Ready</code> &mdash; and cannot '+
    'sign anything.</div>'+
    cmdBlock("gm","gmoperatorprivatekey=<the BLS SECRET from step 3>\ngamemaster=1\nptxnodeid="+comp)+
    '<p class=note>★ The secret is the <b>secret</b> half from step 3, the one that never left this machine. '+
    'This page does not ask for it and never sees it &mdash; paste it in the config yourself.</p>'+
    '<p class=note>★ Also check <code>externalip=</code> is present and <b>not</b> commented. '+
    '<code>install.sh</code> writes it for you from this host’s global IPv6 address; without it the '+
    'gamemaster never reaches <code>Ready</code>. Do not add a second copy &mdash; if it is missing, the '+
    'install did not complete.</p>'+
    cmdBlock("gm","sudo systemctl restart hemis-ptx")+
    cmdBlock("gm","Hemis-cli getgamemasterstatus   # expect: \"status\": \"Ready\"");
}
["name","coladdr","ctxid","cvout","owner","payout","blspub","ipport","compound"].forEach(function(id){
  $(id).addEventListener("input",render);
});
$("samepay").addEventListener("change",render);
render();
</script>
"""


def register_page():
    return REGISTER_HTML


class Handler(BaseHTTPRequestHandler):
    server_version = "ptx-verifier"

    def _send(self, body, code=200):
        b = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        # No third-party anything, so lock the page down to itself.
        self.send_header("Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; form-action 'self'")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self._body(b)

    def _send_json(self, obj, code=200):
        b = json.dumps(obj, indent=1, sort_keys=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(b)))
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self._body(b)

    def do_HEAD(self):
        """HEAD is GET without a body.

        ★ Without this, BaseHTTPRequestHandler answers 501, and an uptime monitor
        -- which defaults to HEAD -- reports the verifier as DOWN while it is
        serving perfectly. eIquidus at / answers HEAD, so a split check would
        disagree with itself about the same host.
        """
        self._head_only = True
        try:
            self.do_GET()
        finally:
            self._head_only = False

    def _body(self, b):
        """Write a body unless this is a HEAD request.

        ★ This is the ONE place that may touch self.wfile directly -- everything
        else goes through here, which is what makes HEAD correct everywhere at
        once rather than route by route.
        """
        if not getattr(self, "_head_only", False):
            self.wfile.write(b)

    def do_GET(self):
        path = self.path.split("?")[0]
        r = api_route("GET", path, None)
        if r is not None:
            return self._send_json(r[0], r[1])
        # ★ Both spellings: nginx strips the /v2 prefix in production, so the app
        # sees "/register" -- but a link written "/v2/register" must also work when
        # the app is hit directly (dev, or the port behind nginx). One route, two
        # names, so the SAME href is correct in both places.
        if path.rstrip("/") in ("/register", "/v2/register"):
            # ★ its own CSP: inline script is needed, connect-src 'none' is the
            # guarantee that the page cannot call anything even if edited later.
            b = register_page().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.send_header("Content-Security-Policy", REGISTER_CSP)
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self._body(b)
            return
        self._send(handle(""))

    def do_POST(self):
        try:
            n = int(self.headers.get("Content-Length") or 0)
        except ValueError:
            n = 0
        if n > 1 << 20:                       # a payload is ~500 bytes; 1 MiB is generous
            self._send(page('<div class="card err">input too large</div>'), 413)
            return
        raw = self.rfile.read(n).decode("utf-8", "replace")
        apath = self.path.split("?")[0]
        if apath.startswith(API_PREFIX):
            # accept either a bare hex body or q=<hex>
            hx = (parse_qs(raw).get("q") or [raw])[0]
            r = api_route("POST", apath, hx)
            return self._send_json(r[0], r[1])
        q = (parse_qs(raw).get("q") or [""])[0]
        self._send(handle(q))

    def log_message(self, fmt, *a):           # keep stdout for real output
        pass


if __name__ == "__main__":
    bind = os.getenv("PTX_VERIFIER_BIND", "127.0.0.1")
    port = int(os.getenv("PTX_VERIFIER_PORT", "8710"))
    print("PTX roll verifier on http://%s:%d  (no node configured)" % (bind, port)
          if not NODE_RPC else
          "PTX roll verifier on http://%s:%d  (txid lookup via %s)" % (bind, port, NODE_RPC))
    ThreadingHTTPServer((bind, port), Handler).serve_forever()
