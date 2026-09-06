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

# ★★ SELF-REPORTED BUILD IDENTITY. This file is a DEPLOYED artefact: it is copied
# onto the explorer host and served from there, so no repo-side gate can see what
# is actually running -- pin-check.sh validates the file in git while this page
# may serve something two releases older. Measured 2026-09-05: the live page was
# still on v0.3.5-testnet, having missed v0.3.6 entirely, and carried a port
# validation that could never fire.
#
# ★ The hash is computed from THIS FILE at import, so it cannot be edited to lie
# without changing the value it reports. ★★ And it is emitted into the page
# FOOTER rather than a log: anyone can read it in a browser without a shell,
# which is the same property the register page's offline verification rests on.
DEPLOY_TAG = "v0.4.1-testnet"
try:
    import hashlib as _hl
    with open(os.path.abspath(__file__), "rb") as _f:
        DEPLOY_HASH = _hl.sha256(_f.read()).hexdigest()[:12]
except Exception:
    DEPLOY_HASH = "unknown"

from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer   # noqa: E402
import threading
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
margin:8px 0;font-size:12.5px;border-radius:0 6px 6px 0}.stats{display:flex;flex-wrap:wrap;gap:14px;margin:10px 0}.stat{min-width:150px;flex:1 1 150px}.stat .k{font-size:11px;text-transform:uppercase;letter-spacing:.06em;opacity:.6}.stat .v{font-size:19px;font-weight:600;margin:2px 0}.why{font-size:12px;opacity:.72;line-height:1.45;margin-top:3px}.empty{border-left:3px solid #b58900;padding:8px 12px;margin:8px 0;font-size:13px;line-height:1.5}.empty ul{margin:6px 0 0 18px;padding:0}.na{opacity:.55;font-style:italic}table{border-collapse:collapse;width:100%;margin:8px 0;font-size:13px}th,td{text-align:left;padding:5px 9px;border-bottom:1px solid rgba(128,128,128,.25);vertical-align:top}th{font-size:11px;text-transform:uppercase;letter-spacing:.05em;opacity:.6}
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
    # ★ Build identity on EVERY page, not just the register form. This file is a
    # deployed artefact with no repo-side gate able to see it, so the page states
    # what it is and anyone can check it from a browser without a shell.
    ident = ("<p class=deployid>serving %s &middot; app %s</p>" % (DEPLOY_TAG, DEPLOY_HASH))
    return (ident + "<footer><p>%s</p><p>Field layout is read from "
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
      <a href="https://github.com/vileda-hemis/Hemis-PTX-MVP1/blob/v0.4.1-testnet/testnet/operator/OPERATOR_ONEPAGER.md">OPERATOR_ONEPAGER.md</a>
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
<code>v0.3.1-testnet</code> binary.
<br><span class=deployid>serving """ + DEPLOY_TAG + """ &middot; app """ + DEPLOY_HASH + """</span></footer>
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


def api_docs_page():
    """★ THE PTX API, DOCUMENTED WHERE AN INTEGRATOR LOOKS.

    eIquidus's own /info page documents its inherited endpoints and says nothing
    about these three, so a developer reading the API surface of this chain sees a
    generic block explorer and no sign the chain does anything unusual -- on the
    one surface whose whole purpose is demonstrating that it does.

    ★★ This page lives in THIS file rather than in eIquidus's info.pug because
    info.pug is vendored third-party software with no source in this repository:
    editing it creates an unversioned deployed artefact that the next upstream
    update silently reverts. Here it is covered by pin-check and carries the
    footer build identity, so its own drift is visible.
    """
    return ("<!doctype html><meta charset=utf-8><meta name=viewport "
            "content='width=device-width,initial-scale=1'>"
            "<title>PTX API - Hemis PTX testnet</title>"
            "<style>%s</style>%s<div class=wrap><h1>PTX API</h1>"
            "<p class=sub>Three endpoints. Everything the chain does that a generic "
            "block explorer does not.</p>"

            "<div class=card><h2>POST /v2/api/v1/verify</h2>"
            "<p>Raw payload hex in, decoded roll and re-derived checks out. "
            "<b>Pure computation</b> &mdash; no node, no index, no chain. "
            "It runs if the node is down.</p>"
            "<pre>curl -X POST https://ptx-explorer.lnky.uk/v2/api/v1/verify \\\n"
            "     -H 'Content-Type: application/json' -d '{\"hex\":\"&lt;payload hex&gt;\"}'</pre></div>"

            "<div class=card><h2>GET /v2/api/v1/tx/&lt;txid&gt;</h2>"
            "<p>Fetches the transaction, decodes its extra payload, and runs the same checks.</p>"
            "<pre>curl https://ptx-explorer.lnky.uk/v2/api/v1/tx/&lt;txid&gt;</pre></div>"

            "<div class=card><h2>GET /v2/api/v1/commitment/&lt;txid&gt;</h2>"
            "<p>Reports whether a roll commitment is <code>settled</code>, plus "
            "<code>past_declared_expiry</code>.</p>"
            "<p>★ <b>Those two are not the same kind of fact.</b> <code>settled</code> is a chain "
            "fact. <code>past_declared_expiry</code> is arithmetic on a value the payload declares "
            "about itself &mdash; there is <b>no consensus-enforced settlement window</b> on this "
            "chain, so nothing rejects a commitment for being late. The API reports both and lets "
            "you judge; it does not invent a verdict the chain does not have.</p>"
            "<pre>curl https://ptx-explorer.lnky.uk/v2/api/v1/commitment/&lt;txid&gt;</pre></div>"

            "<div class=card><h2>Every response carries <code>payload_hex</code></h2>"
            "<p>★ <b>Returning decoded values without the bytes they came from would make this an "
            "oracle.</b> You would be trusting our arithmetic. With the payload bytes in the same "
            "response you can re-derive every check independently &mdash; against your own node, "
            "your own decoder, or by hand. That is the point of the API, and it is why the field is "
            "on responses that failed as well as ones that succeeded.</p></div>"

            "<div class=card><h2>Four states, and never <code>false</code></h2>"
            "<p><code>200</code> + <code>\"ok\": true|false</code> &mdash; checked, and this is the answer.<br>"
            "<code>200</code> + <code>\"ok\": null</code> + a reason &mdash; <b>not performed</b>. "
            "Never <code>false</code>: a <code>false</code> where the answer is &ldquo;we did not "
            "check&rdquo; is a machine-readable lie.<br>"
            "<code>400 malformed</code> &mdash; the bytes are wrong, with the offset.<br>"
            "<code>404 not_found</code> &mdash; no such transaction, or it carries no extra payload.<br>"
            "<code>422 unsupported_payload</code> &mdash; valid bytes this service does not decode.</p>"
            "<p>★ <b>The 422 is the one worth understanding.</b> A ProRegTx is a perfectly valid "
            "special transaction; it simply is not a roll payload. Reporting it as "
            "<code>malformed</code> would tell you your bytes are broken when they are not. The "
            "response names every structure that was tried and why each was rejected:</p>"
            "<pre>{ \"error\": \"unsupported_payload\",\n"
            "  \"tried\": [ {\"struct\": \"CProbabilisticTxPayload\",\n"
            "               \"why\": \"unique: bool byte is 0x08, not 0 or 1\"},\n"
            "             {\"struct\": \"CPTXRollCommitPayload\",\n"
            "               \"why\": \"unique: bool byte is 0x08, not 0 or 1\"} ] }</pre></div>"

            "<div class=card><h2>Rate limits</h2>"
            "<p><code>/verify</code> is <b>not limited</b> &mdash; it makes the node do nothing.<br>"
            "<code>/tx/</code> and <code>/commitment/</code> are limited to <b>30 requests per "
            "minute per IP</b>, burst 10, returning <code>429</code>. They make this node do I/O "
            "for a stranger, which is the reason for the difference.</p></div>"

            "%s</div>" % (CSS, HEADER, footer()))


def register_page():
    return REGISTER_HTML


# ─────────────────────────────────────────────────────────────────────────────
# ROLL FEED
#
# ★ There is NO RPC that lists rolls. `ptx_lottery_history` is wallet-scoped and
# `settlement_history` is a 20-entry ring of SETTLEMENTS, not of rolls. The only
# complete source is the chain, so this scans blocks for type 12 (PTXROLLCOMMIT)
# and type 6 (PTXSESS). Measured 1 ms/block, ~8 s for the whole chain, so the
# scan is incremental and cached rather than per-request.
#
# ★★ THE FEED SHOWS ROLLS THAT PRODUCED NO RESULT. Eight of the first ten paid
# commitments never settled. A feed listing only settled rolls would present a
# chain that works flawlessly -- the same misrepresentation `total_rolls` makes
# by counting commitments and being read as draws.
#
# ★★★ AND IT DOES NOT CALL THEM FAILED. There is no consensus-enforced
# settlement window (BUG-034 retired it), so a commitment that will never settle
# is indistinguishable from one that still might. "No settle observed" is a fact
# this page can support; "failed" and "pending" are both verdicts the chain does
# not reach.
# ─────────────────────────────────────────────────────────────────────────────
_feed_lock = threading.Lock()
_feed = {"from": None, "to": None, "commits": {}, "settles": {}, "dkg": [], "err": None}

TYPE_COMMIT, TYPE_SETTLE, TYPE_DKG = 12, 6, 11


def _feed_scan(budget_blocks=20000):
    """Incremental block scan. Returns (commits, settles, scanned_to, err)."""
    with _feed_lock:
        try:
            tip = rpc("getblockcount", [])
        except Exception as e:                                # noqa: BLE001
            _feed["err"] = str(e)
            return _feed
        start = (_feed["to"] + 1) if _feed["to"] is not None else max(1, tip - budget_blocks)
        if _feed["from"] is None:
            _feed["from"] = start
        for h in range(start, tip + 1):
            try:
                blk = rpc("getblock", [rpc("getblockhash", [h]), 2])
            except Exception as e:                            # noqa: BLE001
                _feed["err"] = "scan stopped at %d: %s" % (h, e)
                break
            for t in blk.get("tx", []):
                ty = t.get("type", 0)
                if ty == TYPE_COMMIT:
                    _feed["commits"][t["txid"]] = {"height": h, "time": blk.get("time")}
                elif ty == TYPE_SETTLE:
                    _feed["settles"][t["txid"]] = {
                        "height": h, "time": blk.get("time"),
                        "spends": [i.get("txid") for i in t.get("vin", []) if i.get("txid")]}
                elif ty == TYPE_DKG:
                    # ★ One scan, two pages. A PTXDKG transaction is a ceremony
                    # RESULT landing on chain, so its height is where a quorum
                    # record was written -- the anchor for quorum history.
                    _feed["dkg"].append({"height": h, "txid": t["txid"]})
            _feed["to"] = h
        else:
            _feed["err"] = None
        return _feed


def _payload_of(txid):
    """extraPayload -> decoded fields, or (None, reason)."""
    try:
        tx = rpc("getrawtransaction", [txid, 1])
    except Exception as e:                                    # noqa: BLE001
        return None, "lookup failed: %s" % e
    hx = tx.get("extraPayload") or ""
    if not hx:
        return None, "no extraPayload"
    d, tried = sniff(hx)
    if d is None:
        return None, "payload did not decode"
    # ★ sniff() returns a DECODED OBJECT; the flat field map comes from as_dict().
    # Using the object directly made every field read None -- and because the
    # settled/unsettled test keyed off d.get("results"), a genuinely settled roll
    # rendered as "no settle observed" while the counter above it said otherwise.
    # The page disagreeing with its own counter is what exposed it.
    return as_dict(d), None


def feed_page():
    st = _feed_scan()
    rows = []
    settle_for = {}
    for stxid, s in st["settles"].items():
        for parent in s["spends"]:
            if parent in st["commits"]:
                settle_for[parent] = stxid
    order = sorted(st["commits"].items(), key=lambda kv: -kv[1]["height"])[:60]

    for ctxid, c in order:
        stxid = settle_for.get(ctxid)
        src = stxid or ctxid
        d, why = _payload_of(src)
        gid = esc(d.get("game_id")) if d else "<span class=na>—</span>"
        if d:
            params = "%s from %s–%s%s%s" % (
                d.get("count"), d.get("low"), d.get("high"),
                ", distinct" if d.get("unique") else "",
                (", excluding %s" % ", ".join(str(x) for x in d["exclude_integers"]))
                if d.get("exclude_integers") else "")
        else:
            params = '<span class=na>%s</span>' % esc(why or "unavailable")
        if stxid:
            # ★ KDD-118: "a settle exists" and "its payload decoded" are DIFFERENT
            # facts. The state is decided by the coin-chain -- a settle spending
            # this commitment -- and never by whether this page could read the
            # results. Conflating them made a settled roll read as unsettled.
            state = '<span class=good>settled</span>'
            note = ""
            link = ('<a href="/v2?q=%s">verify &rarr;</a>' % esc(stxid))
            if d and d.get("results"):
                res = " ".join('<b>%s</b>' % esc(v) for v in d["results"])
            else:
                res = ('<span class=na>settled, but this page could not read the results '
                       '(%s)</span>' % esc(why or "unknown"))
        else:
            res = '<span class=na>none</span>'
            state = '<span class=warn>no settle observed</span>'
            note = ('<div class=why>The commitment is on chain and its fee is paid, but no '
                    'settle spending it has been seen. There is no consensus-enforced '
                    'settlement window, so this is not "failed" and not "pending" — only '
                    'that no result was published.</div>')
            link = ('<a href="/v2?q=%s">verify commitment &rarr;</a>' % esc(ctxid))
        rows.append(
            "<tr><td>%s</td><td><code>%s</code></td><td>%s</td><td>%s</td>"
            "<td>%s%s</td><td>%s</td></tr>"
            % (esc(c["height"]), esc(ctxid[:12]) + "…", gid, params, state, note, link))

    scanned = ('blocks %s–%s' % (esc(st["from"]), esc(st["to"]))
               if st["to"] is not None else 'not yet scanned')
    n_c, n_s = len(st["commits"]), len(settle_for)
    head = ('<div class=card><h2>Rolls</h2><div class=stats>'
            '<div class=stat><div class=k>paid commitments</div><div class=v>%d</div>'
            '<div class=why>every roll that paid its fee</div></div>'
            '<div class=stat><div class=k>produced a result</div><div class=v>%d</div>'
            '<div class=why>a settle spending that commitment was found</div></div>'
            '<div class=stat><div class=k>no settle observed</div><div class=v>%d</div>'
            '<div class=why>fee paid, no result published</div></div></div>'
            '<div class=why>Scanned %s. A roll is two transactions: a commitment that pays the '
            'fee, and a settle that publishes the result and spends it. Both are listed because '
            'a feed of settles alone would show a chain that never fails.</div>'
            % (n_c, n_s, n_c - n_s, scanned))
    if st["err"]:
        head += '<div class=empty><b>Scan incomplete:</b> %s — the list below is partial and the ' \
                'counts above understate.</div>' % esc(st["err"])
    if not rows:
        head += ('<div class=empty><b>No rolls found in the scanned range.</b> Empty, not zero: '
                 'this is what the scan saw, not a statement that none exist outside it.</div>')
        return page(head + "</div>")
    return page(head + "<table><tr><th>height</th><th>commitment</th><th>game_id</th>"
                "<th>draw</th><th>state</th><th></th></tr>" + "".join(rows) + "</table></div>")


# ─────────────────────────────────────────────────────────────────────────────
# OBSERVATION API  —  /v2/api/v1/observe/*
#
# ★ A DIFFERENT API FROM /verify, /tx, /commitment, AND IT SAYS SO. Those are
# VERIFICATION: pure, chain-derived, every response carries payload_hex so the
# caller can re-derive the answer without trusting this service. These are
# OBSERVATION: derived from a block scan, cached, and incomplete by nature. An
# observation endpoint cannot make the verification claim, and mixing the two
# without saying so weakens the ones that can.
#
# ★★ THE HONESTY SURVIVES INTO JSON OR IT DOES NOT COUNT. A machine cannot read
# the caveat paragraph an HTML page prints beside a number, so:
#   - state is a STRING ENUM, never a boolean. `settled: false` would rebuild
#     KDD-118 in a format machines consume, because false reads as "did not
#     settle" when the truth is "no settle observed, and no consensus-enforced
#     window exists to make that final".
#   - every response carries `scan`, so a consumer can tell "we did not see it"
#     from "it is not there". Counts are explicitly scoped to that range.
#   - a -1 sentinel is emitted as null PLUS a companion boolean, so a consumer
#     never has to know that -1 means unset.
# ─────────────────────────────────────────────────────────────────────────────
OBSERVE_PREFIXES = ("/v2/api/v1/observe/", "/api/v1/observe/")

_STATE_SEMANTICS = {
    "settled": "a settle transaction spending this commitment's output was found in the scanned "
               "range; the pairing is structural (coin-chain), not heuristic",
    "no_settle_observed": "the commitment is on chain and its fee is paid, and no settle spending "
                          "it was found in the scanned range. This is NOT 'failed' and NOT "
                          "'pending': BUG-034 retired the settlement window, so there is no "
                          "consensus-enforced deadline and a roll that will never settle is "
                          "indistinguishable from one that still might.",
}


def _scan_block():
    st = _feed_scan()
    return {
        "from": st.get("from"), "to": st.get("to"),
        "complete": st.get("err") is None,
        "error": st.get("err"),
        "note": ("counts and lists below describe ONLY blocks from..to. Absence here means "
                 "'not seen in this range', never 'does not exist'."),
    }


def _sentinel(v):
    """-1 / missing -> (None, False); a real height -> (h, True)."""
    return (None, False) if v is None or v < 0 else (v, True)


def observe_rolls():
    st = _feed_scan()
    settle_for = {}
    for stxid, s in st["settles"].items():
        for parent in s["spends"]:
            if parent in st["commits"]:
                settle_for[parent] = stxid
    rolls = []
    for ctxid, c in sorted(st["commits"].items(), key=lambda kv: -kv[1]["height"]):
        stxid = settle_for.get(ctxid)
        d, why = _payload_of(stxid or ctxid)
        rolls.append({
            "commitment_txid": ctxid,
            "commitment_height": c["height"],
            "settle_txid": stxid,
            "settle_height": st["settles"][stxid]["height"] if stxid else None,
            "state": "settled" if stxid else "no_settle_observed",
            "game_id": (d or {}).get("game_id"),
            "params": None if not d else {
                "count": d.get("count"), "low": d.get("low"), "high": d.get("high"),
                "unique": d.get("unique"),
                "exclude_integers": d.get("exclude_integers"),
            },
            "results": (d or {}).get("results"),
            "payload_read": d is not None,
            "payload_error": None if d is not None else why,
            "verify_url": "/v2?q=%s" % (stxid or ctxid),
        })
    n_s = sum(1 for r in rolls if r["state"] == "settled")
    return {
        "kind": "observation",
        "not_verification": "Derived from a cached block scan. Unlike /v2/api/v1/tx, this cannot "
                            "be independently re-derived from the response; use the verify_url or "
                            "the verification endpoints for that.",
        "scan": _scan_block(),
        "state_semantics": _STATE_SEMANTICS,
        "counts": {"paid_commitments": len(rolls), "settled": n_s,
                   "no_settle_observed": len(rolls) - n_s,
                   "scoped_to_scan_range": True},
        "rolls": rolls,
    }


def observe_quorums():
    st = _feed_scan()
    seen = {}
    for d in sorted(st.get("dkg", []), key=lambda x: x["height"]):
        try:
            lst = rpc("ptx_quorum_list", [d["height"] + 1])
        except Exception:                                     # noqa: BLE001
            continue
        for q in (lst.get("quorums") if isinstance(lst, dict) else lst) or []:
            if q.get("quorum_hash"):
                seen.setdefault(q["quorum_hash"], d["height"])
    out = []
    for qh in seen:
        try:
            r = rpc("ptx_quorum_info", [qh])
        except Exception as e:                                # noqa: BLE001
            out.append({"quorum_hash": qh, "record_read": False, "error": str(e)})
            continue
        sup_h, sup = _sentinel(r.get("superseded_height"))
        dis_h, dis = _sentinel(r.get("disbanded_height"))
        mem = r.get("members") or []
        out.append({
            "quorum_hash": qh, "record_read": True,
            "state": r.get("state"),
            "formation_height": r.get("formation_height"),
            "mined_height": r.get("mined_height"),
            "formed_size": r.get("formed_size"),
            "completed_size": r.get("completed_size"),
            "superseded_height": sup_h, "superseded": sup,
            "disbanded_height": dis_h, "disbanded": dis,
            # ★ ODC-116: the record HAS reformed_height and ptx_quorum_info does
            # not emit it, so a consumer can see THAT a quorum was reformed and
            # not WHEN. Named rather than omitted.
            "reformed_height": None,
            "reformed_height_available": False,
            "reformed_height_note": "recorded on chain (CPTXQuorumRecord.reformed_height) but not "
                                    "emitted by ptx_quorum_info — see ODC-116",
            "members": [{"node_id": m.get("node_id"), "share_index": m.get("share_index"),
                         "in_qual": m.get("in_qual")} for m in mem],
            "not_qualified": [m.get("node_id") for m in mem if not m.get("in_qual")],
        })
    out.sort(key=lambda r: -(r.get("mined_height") or 0))
    return {
        "kind": "observation",
        "scan": _scan_block(),
        "unavailable": {
            "per_ceremony_progression": "qual/bad at each phase transition exists only in each "
                                        "member's debug log and in no RPC. Not shown rather than "
                                        "guessed; it would need a new RPC field, not a scraper.",
        },
        "quorums": out,
    }


def observe_health():
    def q(m, p=None):
        try:
            return rpc(m, p or []), None
        except Exception as e:                                # noqa: BLE001
            return None, str(e)
    h, e_h = q("getblockcount")
    gmc, e_g = q("getgamemastercount")
    qh, e_q = q("ptx_quorum_health")
    lot, e_l = q("ptx_lottery_status")
    body = {
        "kind": "observation",
        "height": h, "height_error": e_h,
        "gamemasters": gmc, "gamemasters_error": e_g,
        # ★ measured vs unknown, never conflated: quorums==[] means measured-and-none;
        # quorums==null with an error means we could not ask.
        "active_quorums": (qh or {}).get("quorums") if qh is not None else None,
        "active_quorums_measured": qh is not None,
        "active_quorums_error": e_q,
        "lottery": lot, "lottery_error": e_l,
        "lottery_notes": {
            "total_rolls": "counts COMMITMENTS (every roll that paid its fee), not completed "
                           "draws. Compare against observe/rolls counts.settled.",
            "settlement_history": "a 20-entry ring buffer, not an all-time list",
            "eligible_nodes_empty": "empty means every gamemaster holds 0 tickets; tickets are "
                                    "credited only when a roll's settle CONFIRMS, and reset to 0 "
                                    "at every settlement. Empty is not zero-forever.",
        },
    }
    return body


def quorums_page():
    """★ QUORUM HISTORY, AND THE COLUMN THAT DOES NOT EXIST (KDD-118).

    `ptx_quorum_list` is an AS-OF-HEIGHT query, not a history: it answers "what
    was active at height H". So the set of quorums that have ever existed is
    recovered from the chain -- every PTXDKG transaction (type 11) is a ceremony
    result landing, and its height is where a record was written.

    ★★ The per-ceremony progression (`qual=N bad=M` at each phase transition) is
    LOG-ONLY and in no RPC. It is not omitted silently here: the page says the
    column does not exist and why. Scraping eleven machines' logs would make this
    page depend on their log retention, which is a worse property than a missing
    column -- if that progression matters, it is a case for a new RPC field, not
    a scraper.
    """
    st = _feed_scan()
    seen, records = {}, []
    for d in sorted(st.get("dkg", []), key=lambda x: x["height"]):
        try:
            lst = rpc("ptx_quorum_list", [d["height"] + 1])
        except Exception:                                     # noqa: BLE001
            continue
        for q in (lst.get("quorums") if isinstance(lst, dict) else lst) or []:
            qh = q.get("quorum_hash")
            if qh and qh not in seen:
                seen[qh] = d["height"]
    for qh in seen:
        try:
            records.append(rpc("ptx_quorum_info", [qh]))
        except Exception:                                     # noqa: BLE001
            records.append({"quorum_hash": qh, "_err": True})
    records.sort(key=lambda r: -(r.get("mined_height") or 0))

    out = ['<div class=card><h2>Quorums</h2>']
    if st.get("err"):
        out.append('<div class=empty><b>Scan incomplete:</b> %s — quorums formed outside the '
                   'scanned range are missing from this list.</div>' % esc(st["err"]))
    if not records:
        out.append('<div class=empty><b>No quorum records found.</b> Empty, not zero — this is '
                   'what the scan of blocks %s–%s found, not a statement that none have ever '
                   'existed.</div>' % (esc(st.get("from")), esc(st.get("to"))))
        return page("".join(out) + "</div>")

    out.append('<div class=why>Recovered by scanning for PTXDKG transactions (type 11), because '
               '<code>ptx_quorum_list</code> answers "what was active at height H" rather than '
               'listing history. Ceremony detail below is what the chain records; see the note '
               'at the foot for what it does not.</div>')
    for r in records:
        qh = r.get("quorum_hash", "")
        if r.get("_err"):
            out.append('<div class=empty><code>%s…</code> — record could not be read.</div>'
                       % esc(qh[:24]))
            continue
        fs, cs = r.get("formed_size", 0), r.get("completed_size", 0)
        mem = r.get("members") or []
        nq = [m.get("node_id", "?") for m in mem if not m.get("in_qual")]
        sup, dis = r.get("superseded_height", -1), r.get("disbanded_height", -1)
        if sup and sup > 0:
            life = "superseded at %s" % esc(sup)
        elif dis and dis > 0:
            life = "disbanded at %s" % esc(dis)
        elif r.get("state") == "active":
            life = "<span class=good>active</span>"
        else:
            life = esc(r.get("state", "?"))
        out.append('<div class=stats>'
                   '<div class=stat><div class=k>quorum</div><div class=v><code>%s…</code></div></div>'
                   '<div class=stat><div class=k>formed at</div><div class=v>%s</div>'
                   '<div class=why>anchor boundary</div></div>'
                   '<div class=stat><div class=k>mined at</div><div class=v>%s</div>'
                   '<div class=why>ceremony result on chain</div></div>'
                   '<div class=stat><div class=k>members</div><div class=v>%d of %d</div>'
                   '<div class=why>qualified of formed</div></div>'
                   '<div class=stat><div class=k>state</div><div class=v>%s</div></div>'
                   '</div>'
                   % (esc(qh[:24]), esc(r.get("formation_height")), esc(r.get("mined_height")),
                      cs, fs, life))
        if nq:
            out.append('<div class=note><b>Did not qualify:</b> %s — named rather than counted, '
                       'because "%d of %d" cannot be acted on.</div>' % (esc(", ".join(nq)), cs, fs))
        elif fs:
            out.append('<div class=why>All %d members qualified.</div>' % fs)
    out.append('<div class=empty><b>Not available here: per-ceremony progression.</b> The '
               '<code>qual</code> and <code>bad</code> counts at each phase transition '
               '(HASH_COMMIT&rarr;CONTRIB&rarr;COMPLAINT&rarr;JUSTIFY&rarr;PREMIT&rarr;FINALIZE) '
               'exist only in each member\'s debug log and in no RPC. They are not shown rather '
               'than being guessed. Reading them would mean scraping eleven machines\' logs, '
               'which would make this page depend on their log retention — a worse property than '
               'a missing column. If that progression matters, it is a case for a new RPC field.'
               '</div>')
    return page("".join(out) + "</div>")


def health_page():
    """★ NETWORK HEALTH, WITH THE REASON FOR EVERY STATE (KDD-118).

    The rule this page is built to: a surface that reports a state must report
    the predicate that produced it, or name what it could not determine.
    `eligible_nodes: []` sent three different wrong hypotheses chasing an empty
    array; the fix is not a prettier empty table, it is saying WHY it is empty.

    ★★ "Empty" is not "zero". Zero implies the thing was measured and came out
    nought; empty says nothing has been recorded. They demand different actions
    from the reader, so they are rendered differently here.
    """
    def _rpc(m, p=None):
        try:
            return rpc(m, p or []), None
        except Exception as e:                                # noqa: BLE001
            return None, str(e)

    up, _ = node_status()
    if up is False:
        return page('<div class="card err">The node this page reads is <b>not answering</b>, so '
                    'nothing below can be measured. This is NOT a statement about the network — '
                    'it is a statement about this page\'s node.</div>')

    height, e_h   = _rpc("getblockcount")
    gmc,   e_gmc  = _rpc("getgamemastercount")
    qh,    e_qh   = _rpc("ptx_quorum_health")
    lot,   e_lot  = _rpc("ptx_lottery_status")
    roster,e_ros  = _rpc("protx_list", [True, False, True])
    peers, e_pi   = _rpc("getpeerinfo")

    def nd(label, value, why=""):
        w = ('<div class=why>%s</div>' % esc(why)) if why else ""
        return '<div class=stat><div class=k>%s</div><div class=v>%s</div>%s</div>' % (
            esc(label), value, w)

    out = []

    # ── chain ────────────────────────────────────────────────────────────────
    out.append("<div class=card><h2>Chain</h2><div class=stats>")
    out.append(nd("height", esc(height) if height is not None else
                  '<span class=na>unavailable</span>', e_h or ""))
    if gmc:
        out.append(nd("gamemasters", "%d enabled of %d" % (gmc.get("enabled", 0), gmc.get("total", 0))))
    out.append("</div></div>")

    # ── quorum ───────────────────────────────────────────────────────────────
    out.append("<div class=card><h2>Quorum</h2>")
    quorums = (qh or {}).get("quorums") or []
    if qh is None:
        # ★ KDD-118 applied to this page's OWN failure: could-not-measure is not
        # the same claim as none-exist, and rendering the first as the second is
        # the defect this page was written to avoid.
        out.append('<div class=empty><b>Could not determine.</b> The quorum RPC did not answer '
                   '(%s), so this says nothing about whether a quorum exists — only that this '
                   'page could not ask.</div>' % esc(e_qh or "no reason given"))
    elif not quorums:
        out.append('<div class=empty><b>No ACTIVE quorum.</b> This is not the same as '
                   '"no gamemasters". A quorum forms at a boundary and is retired after '
                   '<b>200 blocks with no settle confirming</b> — that is a deliberate health '
                   'check, not a fault. With only one quorum alive at a time, its retirement '
                   'leaves a gap of roughly one ceremony (~27 blocks) during which '
                   '<code>ptx_roll</code> returns "no ACTIVE quorum for height N". '
                   'The gap disappears once the network supports two quorums.</div>')
    else:
        for q in quorums:
            qhash = q.get("quorum_hash", "")
            info, e_qi = _rpc("ptx_quorum_info", [qhash])
            out.append("<div class=stats>")
            out.append(nd("quorum", "<code>%s…</code>" % esc(qhash[:24])))
            if info:
                fs, cs = info.get("formed_size", 0), info.get("completed_size", 0)
                thr = fs // 2 + 1
                out.append(nd("members", "%d formed, %d qualified" % (fs, cs),
                              "qualified = completed the ceremony and holds a share; "
                              "a member may be formed but not qualified if it was still "
                              "verifying when the contribution phase closed"))
                out.append(nd("threshold", "%d of %d" % (thr, cs),
                              "signatures needed to reconstruct the group signature"))
                out.append(nd("mined at", esc(info.get("mined_height"))))
            elif e_qi:
                out.append(nd("detail", '<span class=na>unavailable</span>', e_qi))
            out.append("</div>")
            if info:
                mem = info.get("members") or []
                nq = [m for m in mem if not m.get("in_qual")]
                if nq:
                    out.append('<div class=note><b>Not qualified:</b> %s. Named rather than '
                               'counted — a bare "qualified 10 of 11" cannot be acted on.</div>'
                               % esc(", ".join(m.get("node_id", "?") for m in nq)))
    out.append("</div>")

    # ── lottery ──────────────────────────────────────────────────────────────
    out.append("<div class=card><h2>Lottery</h2>")
    if lot is None:
        out.append('<div class=empty><b>Could not determine.</b> The lottery RPC did not answer '
                   '(%s). Nothing below is a claim about the pool.</div>'
                   % esc(e_lot or "no reason given"))
    else:
        pool = lot.get("pool_balance_sat", 0)
        hist = lot.get("settlement_history") or []
        ls   = lot.get("last_settle") or {}
        out.append("<div class=stats>")
        out.append(nd("pool", "%.8f tHMS" % (pool / 1e8),
                      "accumulates the service fee from every commitment; paid out whole to one "
                      "gamemaster at a settlement"))
        out.append(nd("paid rolls", esc(lot.get("total_rolls", 0)),
                      "counts COMMITMENTS — every roll that paid its fee, including rolls that "
                      "never settled. It is not the number of completed draws."))
        out.append(nd("settlements recorded", esc(len(hist)) if hist else
                      '<span class=na>none yet</span>',
                      "kept as a 20-entry ring buffer, so this is recent history and not an "
                      "all-time count"))
        out.append(nd("next settlement", esc(lot.get("next_settlement_at")),
                      "every %s blocks" % esc(lot.get("settlement_window"))))
        out.append("</div>")
        if ls and ls.get("height"):
            out.append('<div class=note><b>Last settlement:</b> %s tHMS at height %s to '
                       '<code>%s…</code></div>' % (esc(ls.get("amount")), esc(ls.get("height")),
                                                   esc(str(ls.get("winner_protx", ""))[:20])))
        else:
            out.append('<div class=empty><b>No settlement recorded yet.</b> Not "zero won" — '
                       'nothing has been written. A settlement needs at least one gamemaster '
                       'holding a ticket, and tickets are credited only when a roll\'s settle '
                       'transaction confirms.</div>')

        # eligibility, WITH REASONS
        elig = lot.get("eligible_nodes")
        out.append("<h3>Eligibility</h3>")
        if elig:
            rows = "".join(
                "<tr><td><code>%s</code></td><td>%s</td><td>%s</td></tr>" % (
                    esc(n.get("node_id")), esc(n.get("tickets", 0)),
                    "eligible" if n.get("eligible") else "not eligible")
                for n in elig)
            out.append("<table><tr><th>gamemaster</th><th>tickets</th><th>state</th></tr>%s</table>"
                       % rows)
            out.append('<div class=why>Tickets are <b>per settlement window</b> — they reset to 0 '
                       'at every settlement. A row showing 0 immediately after a settlement is '
                       'correct and expected, not a failure. Eleven gamemasters with tickets and '
                       'no wins is also correct: one winner is drawn per settlement.</div>')
        else:
            reasons = []
            if roster:
                no_id  = sum(1 for d in roster if not (d.get("dgmstate") or {}).get("ptxNodeId"))
                no_pay = sum(1 for d in roster
                             if not (d.get("dgmstate") or {}).get("ptxPaymentAddress"))
                if no_id:
                    reasons.append("%d gamemaster(s) have no ptxNodeId, which makes them "
                                   "permanently ineligible" % no_id)
                if no_pay:
                    reasons.append("%d have no ptxPaymentAddress" % no_pay)
            reasons.append("every remaining gamemaster holds 0 tickets, and tickets are credited "
                           "only when a roll's settle transaction confirms")
            out.append('<div class=empty><b>No eligible gamemasters.</b> Empty, not zero — the '
                       'list is built by filtering, and here is what filtered it: <ul>%s</ul>'
                       'This is the normal state immediately after a settlement.</div>'
                       % "".join("<li>%s</li>" % esc(r) for r in reasons))
    out.append("</div>")

    # ── reachability ─────────────────────────────────────────────────────────
    out.append("<div class=card><h2>Member reachability</h2>")
    out.append('<div class=why>Measured <b>from this page\'s node</b>, which is not the node that '
               'calls <code>ptx_roll</code>. A caller\'s own view can differ, and its view is the '
               'one that decides whether a roll reaches threshold. Shown because a member that '
               'nothing can connect to cannot sign, and because "unreachable" in a caller\'s log '
               'does not distinguish "down" from "never dialled".</div>')
    if not peers or not quorums:
        out.append('<div class=empty>Not measurable: %s</div>'
                   % esc(e_pi or "no active quorum to measure against"))
    else:
        byip = {}
        for p in peers:
            a = p.get("addr", "")
            ip = a.rsplit(":", 1)[0].strip("[]") if a else ""
            byip.setdefault(ip, []).append(a)
        info, _ = _rpc("ptx_quorum_info", [quorums[0].get("quorum_hash", "")])
        members = (info or {}).get("members") or []
        addr_of = {}
        if roster:
            for d in roster:
                st = d.get("dgmstate") or {}
                if st.get("ptxNodeId"):
                    svc = st.get("service", "")
                    addr_of[st["ptxNodeId"]] = svc.rsplit(":", 1)[0].strip("[]") if svc else ""
        rows, reach = [], 0
        for m in members:
            nid = m.get("node_id", "?")
            ip = addr_of.get(nid, "")
            conns = byip.get(ip, [])
            if not conns:
                state, why = "no connection", "this node holds no connection to it"
            elif any(c.endswith(":29994") for c in conns):
                state, why = "connected", "outbound to its advertised port"; reach += 1
            else:
                state, why = "inbound only", ("it connected to us on an ephemeral port; "
                                              "reachable, but a caller must look it up by "
                                              "address rather than address+port"); reach += 1
            rows.append("<tr><td><code>%s</code></td><td>%s</td><td class=why>%s</td></tr>"
                        % (esc(nid), esc(state), esc(why)))
        thr = (info or {}).get("formed_size", 0) // 2 + 1
        out.append("<div class=stats>%s</div>" % nd(
            "reachable", "%d of %d" % (reach, len(members)),
            "threshold is %d — a roll needs that many members to answer" % thr))
        out.append("<table><tr><th>member</th><th>state</th><th>meaning</th></tr>%s</table>"
                   % "".join(rows))
    out.append("</div>")

    return page("".join(out))


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
        # ★ ?q= ON GET. The page was built as a POST paste-box and do_GET threw
        # the query string away, ending at handle("") -- while the explorer's PTX
        # panel has always linked to /v2?q=<txid>. The link was right and the
        # handler was missing, so every such link presented an empty form and the
        # operator had to copy the txid off the page they had just left.
        # This is what makes a roll and its proof ONE url: the proof travels with
        # the claim instead of being something you have to be told to do.
        # handle() already resolves a 64-hex txid via the node, so nothing
        # downstream changes.
        qs_raw = self.path.split("?", 1)[1] if "?" in self.path else ""
        get_q = (parse_qs(qs_raw).get("q") or [""])[0][:8192]
        for _pfx in OBSERVE_PREFIXES:
            if path.startswith(_pfx):
                _what = path[len(_pfx):].strip("/")
                _fn = {"rolls": observe_rolls, "quorums": observe_quorums,
                       "health": observe_health}.get(_what)
                if _fn is None:
                    return self._send_json(
                        {"error": "not_found", "message": "unknown observation endpoint",
                         "available": ["health", "rolls", "quorums"]}, 404)
                try:
                    return self._send_json(_fn(), 200)
                except Exception as _e:                       # noqa: BLE001
                    return self._send_json(
                        {"error": "observation_failed", "message": str(_e),
                         "note": "this is a failure of the observation service, not a statement "
                                 "about the chain"}, 503)
        r = api_route("GET", path, None)
        if r is not None:
            return self._send_json(r[0], r[1])
        # ★ Both spellings: nginx strips the /v2 prefix in production, so the app
        # sees "/register" -- but a link written "/v2/register" must also work when
        # the app is hit directly (dev, or the port behind nginx). One route, two
        # names, so the SAME href is correct in both places.
        if path.rstrip("/") in ("/api", "/v2/api"):
            b = api_docs_page().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self._body(b)
            return
        if path.rstrip("/") in ("/quorums", "/v2/quorums"):
            b = quorums_page().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self._body(b)
            return
        if path.rstrip("/") in ("/feed", "/v2/feed", "/rolls", "/v2/rolls"):
            b = feed_page().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self._body(b)
            return
        if path.rstrip("/") in ("/health", "/v2/health"):
            b = health_page().encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(b)))
            self.send_header("X-Content-Type-Options", "nosniff")
            self.end_headers()
            self._body(b)
            return
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
        self._send(handle(get_q))

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
