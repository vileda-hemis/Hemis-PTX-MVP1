#!/usr/bin/env python3
"""PTXFLEET — a teletext service for the W2.5b fleet, in the 1990s Ceefax idiom.

Every page is a painting of the SAME dashboard tick that render() already
produced: no page costs an extra RPC sweep or an extra 98-log grep.  That is the
same one-measurement-many-views discipline that stops the wall view and the
teletext pages from ever disagreeing with each other.

SERVICE MAP (Ceefax convention: 100 is the index, sections by hundred, 888 is
where subtitles live)

  100  INDEX + HEADLINES        the front page, all text
  101  FLEET NEWS               headlines in full, worst-first
  102  ALERTS                   current incident list
  150  FLEET WEATHER            subsystem outlook, weather-map idiom
  160  DELAYS                   travel-news idiom for slow blocks / stalls
  199  A-Z INDEX

  201  VITALS                   block-interval ECG; a stall FLATLINES
  202  CHAIN LINES              nodes grouped by TIP HASH, not height
  203  SPLIT HISTORY            diverge -> rejoin, convergence as an event
  204  RECENT BLOCKS            special-tx marks per block

  301  QUORUM BOARD             lifecycle as a departure board
  302  CEREMONY EVENTS          FORM / DONE / ABORT counters
  303  QUORUM ROSTER            the 11 members of each active quorum

  401  POOL AND ROLLS
  402  SETTLEMENTS              recent payouts
  403  LEAGUE TABLE             wins per gamemaster

  501  GAMEMASTERS              98 nodes, carousel-paged
  502  PRODUCERS                callers + stake: who can actually mint
  503  TOPOLOGY                 peer counts, isolation

  601  WEDGE MARKERS
  602  POSE INVARIANT           tickets == 11 x rolls, per node (BUG-027)
  603  REORGS                   chain-switch counts

  701  REGISTER                 latest BUG / KDD / ODC from the tracked doc
  702  GLOSSARY

  888  PET                      the mascot, where subtitles belong

PERIOD MECHANICS that are not just decoration:
  * FASTEXT — four coloured keys are the whole navigation, as on the real thing.
  * NEWSFLASH — a critical condition pre-empts the page you are on, full width,
    which is teletext's own emergency idiom and louder than a chip in a corner.
  * CAROUSEL — 98 gamemasters do not fit on a 40x24 page, so P501 cycles
    subpages 1/5..5/5 in CSS, exactly as a real rolling page did.
  * 40 COLUMNS — every row is padded and clipped to 40, so the grid is real
    rather than suggested.

COLOUR carries the period look but never the meaning: every status also has an
icon and a word, so the pages read correctly in mono, in print, and to a
colourblind viewer.
"""

import html as _h

ST = {
    "good":     ("g", "●", "OK"),
    "warning":  ("y", "▲", "WARN"),
    "serious":  ("m", "◆", "DEGRADED"),
    "critical": ("r", "✖", "CRITICAL"),
}
RANK = {"good": 0, "warning": 1, "serious": 2, "critical": 3}
W = 40
SPARK = "▁▂▃▄▅▆▇█"

# number -> (title, slug). The registry is the single source: routes, the writer
# job and the on-page index all derive from it, so a new page cannot appear in
# one and be missing from another.
PAGES = [
    ("100", "INDEX",          "index"),
    ("101", "FLEET NEWS",     "news"),
    ("102", "ALERTS",         "alerts"),
    ("150", "FLEET WEATHER",  "weather"),
    ("160", "DELAYS",         "delays"),
    ("199", "A-Z INDEX",      "az"),
    ("200", "CHAIN INDEX",    "ix2"),
    ("300", "QUORUM INDEX",   "ix3"),
    ("400", "LOTTERY INDEX",  "ix4"),
    ("500", "NODES INDEX",    "ix5"),
    ("600", "DIAGNOSTIC INDEX", "ix6"),
    ("700", "REFERENCE INDEX", "ix7"),
    ("201", "VITALS",         "vitals"),
    ("202", "CHAIN LINES",    "tube"),
    ("203", "SPLIT HISTORY",  "splits"),
    ("204", "RECENT BLOCKS",  "blocks"),
    ("301", "QUORUM BOARD",   "board"),
    ("302", "CEREMONY EVENTS", "ceremonies"),
    ("303", "QUORUM ROSTER",  "roster"),
    ("401", "POOL AND ROLLS", "pool"),
    ("402", "SETTLEMENTS",    "settlements"),
    ("403", "LEAGUE TABLE",   "league"),
    ("501", "GAMEMASTERS",    "gms"),
    ("502", "PRODUCERS",      "producers"),
    ("503", "TOPOLOGY",       "topology"),
    ("601", "WEDGE MARKERS",  "wedge"),
    ("602", "POSE INVARIANT", "pose"),
    ("603", "REORGS",         "reorgs"),
    ("701", "REGISTER",       "register"),
    ("702", "GLOSSARY",       "glossary"),
    ("888", "FLEET PET",      "pet"),
]
SECTIONS = [("100", "NEWS"), ("200", "CHAIN"), ("300", "QUORUMS"),
            ("400", "LOTTERY"), ("500", "NODES"), ("600", "DIAGNOSTICS"),
            ("700", "REFERENCE")]

_CSS = """
html,body{margin:0;background:#000;color:#fff}
body{display:flex;justify-content:center;padding:10px 6px}
.tv{position:relative;background:#000;padding:8px 10px;border-radius:4px}
.r{font:700 clamp(11px,3.15vw,19px)/1.30 "Cascadia Mono",ui-monospace,Menlo,
Consolas,monospace;white-space:pre;letter-spacing:.055em}
.dh{font-size:clamp(18px,5.9vw,31px);line-height:1.13;letter-spacing:.04em}
i{font-style:normal}a{text-decoration:none}
.t-w{color:#fff}.t-y{color:#ff0}.t-c{color:#0ff}.t-g{color:#0f0}
.t-m{color:#f0f}.t-r{color:#f00}.t-b{color:#5a7fff}.t-k{color:#4a4a5a}
.t-gb{color:#000;background:#0f0}.t-rb{color:#fff;background:#f00}
.t-yb{color:#000;background:#ff0}.t-cb{color:#000;background:#0ff}
.t-mb{color:#000;background:#f0f}
.ft{display:flex;gap:0;margin-top:2px}
.ft a{flex:1;text-align:center;font-size:clamp(10px,2.7vw,14px);padding:4px 0;
font-weight:700;letter-spacing:.04em}
.idx{display:flex;gap:2px;flex-wrap:wrap;margin-top:5px}
.idx a{flex:1;min-width:64px;text-align:center;font-size:clamp(9px,2.2vw,12px);
padding:3px 2px;font-weight:700;color:#000;opacity:.8}
.idx a.on{opacity:1;outline:2px solid #fff}
.scan{position:absolute;inset:0;pointer-events:none;border-radius:4px;
background:repeating-linear-gradient(180deg,rgba(255,255,255,.05) 0 1px,
transparent 1px 3px)}
/* ── WALL MODE — a real 4:3 teletext picture ───────────────────────────
   Default sizing is phone-first and caps at 19px, so above ~600px the page is
   a ~530px column floating in black — the opposite of what teletext was.
   Wall mode reproduces the PICTURE, not just bigger text, and that means
   getting the geometry right rather than only the scale:

     40 columns x 25 rows filling 4:3 => cell aspect w/h = (4/40)/(3/25) = 0.833

   The phone metrics give 0.504 (advance .655em over line-height 1.30), which
   is why the first attempt came out 1387x1926 — PORTRAIT, not a TV page. Wall
   mode therefore sets line-height to 1.0 and widens tracking to .233em so the
   advance is .833em: cell 0.833 wide x 1.0 tall, page 33.3 x 25.0 = 4:3 exact.
   line-height 1.0 is the authentic value anyway — it is what makes the mosaic
   block rules (▄) join vertically instead of showing hairline gaps.

   Sizing takes the smaller of the two constraints, so the 4:3 page is whole on
   any screen and PILLARBOXED on 16:9 — exactly how a teletext page looked on a
   widescreen TV:
     width  fs <= vw / 33.3  (~3.0vw)
     height fs <= vh / 25.0  (~4.0vh)
   Opt-in via ?wall so the phone view and a laptop reader are untouched. */
html.wall,html.wall body{height:100%;overflow:hidden}
html.wall body{padding:0;align-items:center;justify-content:center}
html.wall .tv{padding:0;border-radius:0}
html.wall .scan{border-radius:0}
html.wall .r{font-size:min(2.95vw,3.93vh);line-height:1.0;letter-spacing:.233em}
/* double height = exactly two rows, as on the real service */
html.wall .dh{font-size:min(5.9vw,7.86vh);line-height:1.0;letter-spacing:.21em}
html.wall .ft{margin-top:0}
html.wall .ft a{font-size:min(2.0vw,2.7vh);padding:0;line-height:1.0}
html.wall .flash{padding:0;line-height:1.0;font-size:min(2.95vw,3.93vh)}
.flash{background:#f00;color:#fff;font-weight:700;padding:3px 0;text-align:center;
letter-spacing:.08em}
/* CAROUSEL — real rolling subpages, CSS only, no JS */
.car{position:relative}
.car>div{animation:cyc 25s steps(1,end) infinite;opacity:0}
@keyframes cyc{0%,18%{opacity:1}20%,100%{opacity:0}}
.car>div:nth-child(1){animation-delay:0s}
.car>div:nth-child(2){animation-delay:5s}
.car>div:nth-child(3){animation-delay:10s}
.car>div:nth-child(4){animation-delay:15s}
.car>div:nth-child(5){animation-delay:20s}
.car>div:not(:first-child){position:absolute;top:0;left:0;right:0}
"""


def _row(cells):
    """cells: (text, colour) or (text, colour, page_no).

    ★ The three-tuple makes the cell CLICKABLE. On the real service you keyed
    the number in on a remote; in a browser there is no keypad, so every page
    reference has to be a link or it is decoration. Padding is computed on the
    text, not the markup, so the 40-column grid survives the anchors."""
    out, used = [], 0
    for cell in cells:
        if used >= W:
            break
        txt, cls = cell[0], cell[1]
        page = cell[2] if len(cell) > 2 else None
        txt = str(txt)[:W - used]
        used += len(txt)
        piece = '<i class="t-%s">%s</i>' % (cls, _h.escape(txt))
        out.append('<a href="%s">%s</a>' % (_href(page), piece) if page else piece)
    if used < W:
        out.append(" " * (W - used))
    return '<div class="r">%s</div>' % "".join(out)


def _ref(label, no, dots=True):
    """An inline 'see also' — label, leader dots, linked page number."""
    title = next((t for n, t, _sl in PAGES if n == no), "")
    lead = label.ljust(28, "." if dots else " ")[:28]
    return _row([("  ", "w"), (lead, "b"), ("P%s" % no, "c", no)])


def _kv(label, val, cls="w", pad=12):
    return _row([("  ", "w"), (str(label).ljust(pad), "c"), (val, cls)])


def _rule(cls="b"):
    return _row([("▄" * W, cls)])


def _slug(no):
    for n, _t, sl in PAGES:
        if n == no:
            return sl
    return "index"


def _href(no):
    return "/p%s" % no


def _tt(page_no, rows, s, minrows=19, flash=None):
    title = next((t for n, t, _s in PAGES if n == page_no), "PTXFLEET")
    L = []
    if flash:
        minrows -= 1          # the newsflash occupies one of the 25
        # NEWSFLASH — teletext's own emergency idiom: it takes the whole width
        # and pre-empts whatever page you are reading.
        L.append('<div class="r flash">%s</div>' % _h.escape(flash[:W]))
    L += [_row([("P%s" % page_no, "w"), (" PTXFLEET", "c"),
                ("  %s %s" % (s.get("ts", ""), s.get("clock", "")), "w")]),
          '<div class="r dh"><i class="t-y">%s</i></div>' % _h.escape(title),
          _rule()]
    # ★ THE 25-ROW BUDGET IS HARD, as it was on the real service — a page was
    # written to fit, not scrolled. Enforcing it here is what makes the wall
    # geometry true: 4:3 is computed from 40x25, so a page that renders 29 rows
    # is not a teletext page, it is a 29-row page that would overflow (and, with
    # overflow:hidden in wall mode, silently lose its bottom rows).
    # Double-height rows cost TWO of the budget, so the count is by rendered
    # height and not by list length.
    def _h_of(entry):
        # A carousel is the whole body — every subpage inside it is padded to
        # the budget and only one is ever in flow, so it costs the full budget
        # exactly once. Counting it as a single row (its list length) is what
        # let P501 render 93 rows and blow the 4:3 geometry.
        if 'class="car"' in entry:
            return minrows
        return 2 if 'class="r dh"' in entry else 1

    used = 0
    for entry in rows:
        c = _h_of(entry)
        if used + c > minrows:
            break
        L.append(entry)
        used += c
    while used < minrows:
        L.append(_row([]))
        used += 1
    L.append(_rule())
    # 25 rendered rows total (the title is double-height, so it counts as two)
    # — the real page geometry, and what makes the 4:3 arithmetic come out.
    # ★ FASTEXT is the WHOLE navigation, as on the real service — there is no
    # page-list strip along the bottom. Keys are CONTEXTUAL: red always returns
    # to the front page, green goes to the index for the section you are in,
    # yellow steps to the next page of that section, cyan is the A-Z. Browsing
    # lives on index pages (P100, P199, P200..P700), not on every page's footer.
    sec = page_no[0] + "00"
    insec = [n for n, _t, _sl in PAGES if n[0] == page_no[0] and n != sec]
    nxt = sec
    if insec:
        nxt = insec[(insec.index(page_no) + 1) % len(insec)] \
            if page_no in insec else insec[0]
    sec_title = next((t for n, t, _sl in PAGES if n == sec), "INDEX")
    nxt_title = next((t for n, t, _sl in PAGES if n == nxt), "NEXT")
    L.append('<div class="r ft">'
             '<a class="t-r" href="/p100">100 INDEX</a>'
             '<a class="t-g" href="%s">%s %s</a>'
             '<a class="t-y" href="%s">%s %s</a>'
             '<a class="t-c" href="/p199">199 A-Z</a></div>'
             % (_href(sec), sec, sec_title[:9],
                _href(nxt), nxt, nxt_title[:9]))
    # ?wall=1 turns it on, ?wall=0 off; the choice is remembered so the 30s
    # meta-refresh cannot silently drop a wall display back to phone sizing.
    boot = ("(function(){try{var q=location.search,w=null;"
            "if(/[?&]wall=0/.test(q))w='0';else if(/[?&]wall/.test(q))w='1';"
            "if(w!==null)localStorage.setItem('ptxWall',w);"
            "if((localStorage.getItem('ptxWall')||'0')==='1')"
            "document.documentElement.className='wall';}catch(e){}})();")
    return ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<meta http-equiv="refresh" content="30">'
            '<title>P%s %s</title><script>%s</script>'
            '<style>%s</style></head><body>'
            '<div class="tv">%s<div class="scan"></div></div></body></html>'
            % (page_no, _h.escape(title), boot, _CSS, "".join(L)))


# ── shared health roll-up ─────────────────────────────────────────────────
def _stats(s):
    """Per-stat rows. The pet's table and _health()'s headline read the SAME
    list, so the headline is literally max(rows) — they cannot disagree in
    direction or severity."""
    tgt = s.get("target_block_s", 60)
    stall = s.get("stall_s")
    tips = len(s.get("tipgroups", {}) or {})
    nq, nact = s.get("nq") or 0, s.get("nact") or 0
    wl, wt = s.get("wedge_live"), s.get("wedge_tot", 0) or 0
    dead = s.get("dead", 0) or 0
    rows = []
    if stall is None:
        rows.append(("HEARTBEAT", "--", "warning", "no block time yet"))
    else:
        r = ("good" if stall <= 1.5 * tgt else "warning" if stall <= 3 * tgt
             else "serious" if stall <= 6 * tgt else "critical")
        rows.append(("HEARTBEAT", "%ds since blk" % stall, r,
                     "CHAIN STALLED %dm" % (stall // 60) if r == "critical"
                     else "block overdue %ds" % stall))
    rows.append(("CHAINS", str(max(tips, 1)),
                 "good" if tips <= 1 else "serious",
                 "chain split across %d tips" % tips))
    # ODC-069: active vs live capacity (⌊pool/11⌋), SAME representation as the
    # terminal/html — never active/nq or active/8 (nSupportedQuorums is a
    # declaration, not a cap; the pool-driven count is unbounded).
    _cap = s.get("capacity")
    rows.append(("QUORUMS", "%d active ~%s cap" % (nact, _cap if _cap is not None else "?"),
                 "good" if nact else "serious",
                 "" if nact else "no ACTIVE quorum"))
    rows.append(("WEDGE", str(wt), "critical" if wl else
                 ("warning" if wt else "good"),
                 "LIVE payout/DKG rejection" if wl
                 else "%d historical marker(s)" % wt))
    rows.append(("NODES", "%s/%s" % (s.get("gm_ok", "?"), s.get("n", "?")),
                 "good" if dead == 0 else "warning",
                 "%d node(s) unreachable" % dead))
    return rows


def _health(s):
    worst, why = "good", "FLEET NOMINAL"
    for _l, _v, role, reason in _stats(s):
        if role and RANK[role] > RANK[worst]:
            worst, why = role, reason
    return worst, why


def _flash(s):
    role, why = _health(s)
    return ("NEWSFLASH  %s" % why) if role == "critical" else None


def _status(s):
    role, why = _health(s)
    cls, icon, lab = ST[role]
    txt = (" %s %s — %s " % (icon, lab, why))[:W - 2].ljust(W - 2)
    return [_row([(" STATUS", "g")]),
            _row([("  ", "w"), (txt, "gb" if role == "good" else "rb")])]


# ══ 100s — NEWS ═══════════════════════════════════════════════════════════
def index(s):
    role, why = _health(s)
    cls, icon, lab = ST[role]
    heads = _headlines(s)[:3]
    rows = [_row([("  ", "w"), ((" %s %s — %s " % (icon, lab, why))[:W - 2]
                                .ljust(W - 2), "gb" if role == "good" else "rb")]),
            _row([]), _row([(" HEADLINES", "g")])]
    for i, (col, head, lines) in enumerate(heads, 1):
        rows.append(_row([(" %d " % i, "b"), (head[:34], col)]))
        if lines:
            rows.append(_row([("   ", "w"), (lines[0][:36], "w")]))
    rows += [_row([]), _row([(" SECTIONS", "g")])]
    for sec, name in SECTIONS:
        first = next((n for n, _t, _sl in PAGES if n[0] == sec[0]), sec)
        titles = ", ".join(t.title() for n, t, _sl in PAGES
                           if n[0] == sec[0])[:26]
        rows.append(_row([("  ", "w"), (sec, "y", first), (" ", "w"),
                          (name.ljust(12), "c", first), (titles, "b")]))
    rows += [_row([]),
             _row([("  888 PET", "y", "888"), ("   ", "w"),
                   ("199 A-Z INDEX", "c", "199")]),
             '<div class="r">  <a href="?wall=1"><i class="t-g">'
             'WALL MODE</i></a><i class="t-b"> fills the screen'
             '</i></div>',
             '<div class="r">  <a href="?wall=0"><i class="t-b">'
             'normal size</i></a></div>',
             _row([]),
             _kv("TIP", "h%s" % s.get("tip", "?"), "y"),
             _kv("MAJORITY", "%s/%s" % (s.get("maj_n", "?"),
                                        s.get("live_n", "?")), "w")]
    return _tt("100", rows, s, flash=_flash(s))


def _headlines(s):
    out = []
    tgt = s.get("target_block_s", 60)
    stall, tips = s.get("stall_s"), len(s.get("tipgroups", {}) or {})
    c = s.get("cnt", {}) or {}
    if stall is not None and stall > 6 * tgt:
        out.append(("r", "CHAIN STALLED",
                    ["No block for %dm. Target %ss." % (stall // 60, tgt),
                     "A HALT, not a quiet spell."]))
    if s.get("wedge_live"):
        out.append(("r", "LIVE REJECTION",
                    ["Payout/DKG rejection in progress.",
                     "%d marker(s) fleet-wide. See P601." % s.get("wedge_tot", 0)]))
    if tips > 1:
        out.append(("y", "CHAIN SPLIT",
                    ["%d distinct tips. P202 shows which" % tips,
                     "nodes; P203 whether it rejoins."]))
    if s.get("dead"):
        out.append(("y", "NODES UNREACHABLE",
                    ["%d node(s) not answering RPC." % s["dead"]]))
    if s.get("nact"):
        out.append(("g", "QUORUMS ACTIVE",
                    ["%d active, ~%s capacity. Done %s, abort %s."
                     % (s["nact"], s.get("capacity", "?"), c.get("DONE", 0),
                        c.get("ABORT", 0))]))
    else:
        out.append(("c", "AWAITING FORMATION",
                    ["No ACTIVE quorum yet. Rolls need",
                     "one (KDD-069 retired the dealer)."]))
    if s.get("last_settle_h"):
        out.append(("g", "LAST SETTLEMENT",
                    ["%s to %s" % (s.get("last_amount", "?"),
                                   str(s.get("winner", "?"))[:20]),
                     "at h%s. See P402." % s["last_settle_h"]]))
    out.append(("w", "CHAIN",
                ["Tip h%s. Majority %s/%s." % (s.get("tip", "?"),
                                               s.get("maj_n", "?"),
                                               s.get("live_n", "?")),
                 "Rate %.1fs/blk. Boundary h%s."
                 % (s.get("rate", 0) or 0, s.get("nb", "?"))]))
    return out


def news(s):
    rows = []
    for i, (col, head, lines) in enumerate(_headlines(s)[:6], 1):
        rows.append(_row([(" %02d " % i, "b"), (head, col)]))
        for ln in lines:
            rows.append(_row([("    ", "w"), (ln, "w")]))
        rows.append(_row([]))
    rows += _status(s)
    return _tt("101", rows, s, flash=_flash(s))


def alerts(s):
    al = s.get("alerts", []) or []
    rows = [_row([(" CURRENT ALERTS", "g")]), _rule()]
    if not al:
        rows += [_row([("  ", "w"), ("NO ALERTS", "g")]),
                 _row([("  ", "w"), ("all monitored conditions clear", "b")])]
    for a in al[:8]:
        rows.append(_row([("  ! ", "r"), (str(a)[:36], "y")]))
    rows += [_row([]), _row([(" ALERT SOURCES", "c")]),
             _row([("  wedge markers .......... P601", "b")]),
             _row([("  chain splits ........... P202", "b")]),
             _row([("  liveness / stalls ...... P201", "b")]),
             _ref("  pose invariant", "602"), _row([])]
    rows += _status(s)
    return _tt("102", rows, s, flash=_flash(s))


def weather(s):
    """Subsystem outlook in the weather-map idiom — a period-correct way to show
    several independent健 states at once without a table."""
    sym = {"good": ("☀", "g", "FINE"), "warning": ("☁", "y", "CLOUDY"),
           "serious": ("☂", "m", "RAIN"), "critical": ("⚡", "r", "STORM")}
    rows = [_row([(" OUTLOOK BY SUBSYSTEM", "g")]), _rule()]
    for label, val, role, why in _stats(s):
        r = role or "good"
        ic, cls, word = sym[r]
        rows.append(_row([("  ", "w"), (ic + " ", cls),
                          (label.ljust(11), "c"), (word.ljust(8), cls),
                          (str(val)[:14], "w")]))
    rows.append(_row([]))
    role, why = _health(s)
    ic, cls, word = sym[role]
    rows += [_row([(" GENERAL OUTLOOK", "c")]),
             '<div class="r dh"><i class="t-%s">  %s  %s</i></div>' % (cls, ic, word),
             _row([("  ", "w"), (why[:36], "w")]), _row([]),
             _row([("  ☀ fine  ☁ cloudy  ☂ rain  ⚡ storm", "b")]),
             _row([("  symbol AND word — never colour", "b")]),
             _row([("  alone", "b")]), _row([])]
    return _tt("150", rows, s, flash=_flash(s))


def delays(s):
    """Block production in the travel-news idiom. 'Delays on the main line' is a
    more natural reading of a slow chain than a seconds counter."""
    tgt = s.get("target_block_s", 60)
    stall = s.get("stall_s")
    iv = s.get("intervals", [])[-10:]
    rows = [_row([(" SERVICE STATUS  MAIN LINE", "g")]), _rule()]
    if stall is None:
        rows.append(_row([("  NO SERVICE INFORMATION", "y")]))
    elif stall > 6 * tgt:
        rows += [_row([("  ", "w"), (" SERVICE SUSPENDED ".ljust(W - 2), "rb")]),
                 _row([("  no block for %dm %ds" % (stall // 60, stall % 60), "r")]),
                 _row([("  scheduled every %ss" % tgt, "b")])]
    elif stall > 2 * tgt:
        rows += [_row([("  ", "w"), (" SEVERE DELAYS ".ljust(W - 2), "yb")]),
                 _row([("  running %ds behind schedule" % (stall - tgt), "y")])]
    elif stall > tgt:
        rows += [_row([("  MINOR DELAYS", "y")]),
                 _row([("  %ds since last arrival" % stall, "w")])]
    else:
        rows += [_row([("  ", "w"), (" GOOD SERVICE ".ljust(W - 2), "gb")]),
                 _row([("  running to schedule", "g")])]
    rows.append(_row([]))
    rows.append(_row([(" RECENT ARRIVALS", "c")]))
    for b in reversed(iv):
        late = b["s"] - tgt
        cls = "g" if abs(late) <= 15 else ("y" if late < 60 else "r")
        word = "on time" if abs(late) <= 15 else ("%+ds" % late)
        rows.append(_row([("  h", "b"), (str(b["h"]).ljust(7), "w"),
                          ("%3ds" % b["s"], "w"), ("   ", "w"), (word, cls)]))
    rows.append(_row([]))
    return _tt("160", rows, s, flash=_flash(s))


def _section_index(s, sec, blurb):
    """A section index page — where browsing belongs, instead of a strip glued
    to the bottom of every page."""
    rows = [_row([(" PAGES IN THIS SECTION", "g")]), _rule()]
    for no, title, _sl in PAGES:
        if no[0] == sec[0] and no != sec:
            rows.append(_row([("  ", "w"), (no, "y", no), ("  ", "w"),
                              (title.ljust(20), "w", no)]))
    rows.append(_row([]))
    for ln in blurb:
        rows.append(_row([("  ", "w"), (ln, "b")]))
    rows.append(_row([]))
    rows.append(_row([(" OTHER SECTIONS", "c")]))
    for sn, nm in SECTIONS:
        if sn[0] != sec[0]:
            tgt = next((n for n, _t, _sl in PAGES if n == sn), sn)
            rows.append(_row([("  ", "w"), (sn, "y", tgt), ("  ", "w"),
                              (nm, "w", tgt)]))
    rows.append(_row([("  888  ", "w", "888"), ("FLEET PET", "w", "888")]))
    return _tt(sec, rows, s, flash=_flash(s))


def ix2(s):
    return _section_index(s, "200", [
        "chain health, forks and liveness.",
        "P201 is the liveness page: a stall",
        "flatlines rather than sitting still."])


def ix3(s):
    return _section_index(s, "300", [
        "DKG quorum formation and lifecycle.",
        "P303 lists the 11 members holding",
        "each active group key."])


def ix4(s):
    return _section_index(s, "400", [
        "the accumulator, rolls and payouts.",
        "settlement runs every 60 blocks."])


def ix5(s):
    return _section_index(s, "500", [
        "the fleet roster. note P502: the",
        "callers are the ENTIRE producer set,",
        "since gamemasters cannot stake."])


def ix6(s):
    return _section_index(s, "600", [
        "rejection markers, the pose",
        "invariant and reorg counts — the",
        "pages that catch consensus faults."])


def ix7(s):
    return _section_index(s, "700", [
        "register and glossary. the design",
        "doc stays the source of truth; P701",
        "only reads it."])


def az(s):
    rows = [_row([(" ALL PAGES A-Z", "g")]), _rule(),
            '<div class="r">  <a href="?wall=1"><i class="t-g">WALL MODE</i>'
            '</a><i class="t-b">  /  </i><a href="?wall=0">'
            '<i class="t-b">normal</i></a></div>', _row([])]
    for no, title, _sl in sorted(PAGES, key=lambda p: p[1]):
        rows.append(_row([("  ", "w"), (title.ljust(20), "w", no),
                          (".. P%s" % no, "c", no)]))
    rows.append(_row([]))
    return _tt("199", rows, s, flash=_flash(s))


# ══ 200s — CHAIN ══════════════════════════════════════════════════════════
def vitals(s):
    tgt = s.get("target_block_s", 60)
    stall = s.get("stall_s")
    iv = s.get("intervals", [])[-36:]
    trace = []
    for b in iv:
        ratio = min(b["s"] / tgt, 3.0) if tgt else 1.0
        trace.append(SPARK[int(max(0, min(7, round((1.0 / max(ratio, .2)) * 3.5))))])
    flat = stall is not None and stall > 2 * tgt
    if flat:
        trace += ["▁"] * max(2, (W - 4) - len(trace))
    line = "".join(trace)[-(W - 4):] or "▁" * (W - 4)
    bpm = round(3600 / (sum(b["s"] for b in iv) / len(iv)), 1) if iv else 0
    rows = [_row([(" HEARTBEAT", "g")]),
            _row([("  ", "w"), (line, "r" if flat else "g")]), _row([]),
            _kv("SINCE BLK", ("%ds" % stall) if stall is not None else "--",
                "r" if flat else "y"),
            _kv("BLK/HOUR", str(bpm), "w"),
            _kv("TARGET", "%ss/blk" % tgt, "w"),
            _kv("OVERDUE", ("%d blk" % (stall // tgt))
                if stall and stall > tgt else "0", "r" if flat else "g"),
            _row([]),
            _row([("  FLATLINE = HALT, NOT QUIET", "r")]) if flat else
            _row([("  steady beat — chain advancing", "w")]),
            _row([("  judged vs wall-clock, never vs", "b")]),
            _row([("  the previous sample", "b")]),
            _row([("  delays in travel form ... P160", "b")]), _row([])]
    rows += _status(s)
    return _tt("201", rows, s, flash=_flash(s))


def tube(s):
    groups = s.get("tipgroups", {}) or {}
    heights = s.get("heights", {}) or {}
    order = sorted(groups.items(), key=lambda kv: -len(kv[1]))
    LC = ["g", "y", "m", "c", "r", "w"]
    rows = [_row([(" GROUPED BY TIP HASH", "g")]), _rule()]
    for i, (tip, nodes) in enumerate(order[:6]):
        cls = LC[i % len(LC)]
        hs = [heights.get(n) for n in nodes if heights.get(n) is not None]
        lo, hi = (min(hs), max(hs)) if hs else (0, 0)
        rows.append(_row([(" ", "w"), ("━" * 6, cls), ("● ", cls),
                          (str(tip)[:8], "w"),
                          ("  %d node%s" % (len(nodes),
                                            "" if len(nodes) == 1 else "s"), "c")]))
        rows.append(_row([("        h%s%s%s" % (lo, "-%s" % hi if hi != lo else "",
                                                "  MAIN" if i == 0 else ""), "b")]))
        if len(nodes) <= 3:
            rows.append(_row([("        ", "w"), (", ".join(nodes[:3]), "b")]))
    rows += [_row([]),
             _row([("  ahead-on-same-line = latency", "b")]),
             _row([("  a different line   = SPLIT", "b")]),
             _ref("  did it rejoin?", "203"), _row([])]
    rows += _status(s)
    return _tt("202", rows, s, flash=_flash(s))


def splits(s):
    eps = list(s.get("split_log", []))[-7:]
    openep = s.get("split_open")
    rows = [_row([(" DIVERGE ┳    REJOIN ┻", "g")]), _rule()]
    if not eps and not openep:
        rows += [_row([("  no split recorded", "g")]),
                 _row([("  one chain for the whole", "b")]),
                 _row([("  observed window", "b")])]
    for e in eps:
        rows.append(_row([("  h", "b"), (str(e.get("from_h")), "y"),
                          (" ┳", "r"), ("━" * 7, "r"), ("┻ h", "g"),
                          (str(e.get("to_h")), "g"),
                          ("  %sblk" % e.get("blocks", "?"), "b")]))
        rows.append(_row([("      %d tips, %s"
                           % (e.get("max_tips", 2),
                              ", ".join(e.get("nodes", [])[:3])), "b")]))
    if openep:
        rows.append(_row([("  h", "b"), (str(openep.get("from_h")), "y"),
                          (" ┳", "r"), ("╌" * 7, "r"), ("▶ OPEN", "r")]))
    rows += [_row([]),
             _row([("  ┻ = recorded CONVERGENCE, not", "b")]),
             _row([("  merely the absence of alarm", "b")]),
             _row([("  persists across restarts", "b")]), _row([])]
    return _tt("203", rows, s, flash=_flash(s))


def blocks(s):
    bl = s.get("blocks", []) or []
    rows = [_row([(" S=SESS R=COMMIT C=COALESCE", "c")]),
            _row([(" P=PAYOUT D=DKG ·=plain", "c")]),
            _row([(" C/R WITHOUT S = FORFEIT", "r")]), _rule()]
    # Per-block colouring instead of one flat strip: C/R with no S in the same
    # block is a roll that committed and never settled (forfeit). A wall of
    # those rendered identically to healthy activity is how a forfeit run went
    # unread; red is the point of this view.
    marks = [str(x) for x in bl]
    budget = (W - 4) * 4
    while marks and sum(len(m) for m in marks) > budget:
        marks.pop(0)
    cur, cw = [], 0
    for m in marks:
        colr = "r" if (m != "·" and "S" not in m and ("C" in m or "R" in m)) else "y"
        if cur and cw + len(m) > W - 4:
            rows.append(_row([("  ", "w")] + cur))
            cur, cw = [], 0
        cur.append((m, colr))
        cw += len(m)
    if cur:
        rows.append(_row([("  ", "w")] + cur))
    if not marks:
        rows.append(_row([("  no blocks observed yet", "b")]))
    rows += [_row([]),
             _kv("TIP", "h%s" % s.get("tip", "?"), "y"),
             _kv("BOUNDARY", "h%s" % s.get("nb", "?"), "w"),
             _kv("SETTLE AT", "h%s" % s.get("ns", "?"), "w"), _row([])]
    return _tt("204", rows, s, flash=_flash(s))


# ══ 300s — QUORUMS ════════════════════════════════════════════════════════
def board(s):
    # ODC-069: quorum count is pool-driven and unbounded — show ALL active, not a
    # frozen row cap. A teletext page holds ~16 rows; beyond that, indicate the
    # overflow HONESTLY (+N more) rather than silently dropping quorums.
    qs = [q for q in (s.get("quorums", []) or []) if (q.get("state") or "").lower() == "active"]
    _PAGE = 16
    overflow = max(0, len(qs) - _PAGE)
    rows = [_row([(" QUORUM    STATE  MINED  v6/v4  (%d active)" % len(qs), "c")]), _rule()]
    if not qs:
        rows += [_row([("  no quorums yet — awaiting", "y")]),
                 _row([("  formation", "y")])]
    for q in qs[:_PAGE]:
        st = (q.get("state") or "?").upper()[:7]
        cls = {"ACTIVE": "g", "FORMING": "y"}.get(st, "m")
        mined = q.get("mined")
        v6, v4 = q.get("v6"), q.get("v4")
        fam = ("%d/%d" % (v6, v4)) if v6 is not None else "--"
        # mixed families (both present) glow cyan; single-family dim
        fcls = "c" if (v6 and v4) else "b"
        rows.append(_row([(" ", "w"), (str(q["qh"])[:9].ljust(10), "w"),
                          (st.ljust(7), cls),
                          ((str(mined) if mined is not None else "PEND").ljust(6),
                           "w" if mined is not None else "y"),
                          (fam.rjust(5), fcls)]))
    if overflow:
        rows.append(_row([("  +%d more active (page cap; see term)" % overflow, "y")]))
    rows += [_row([]),
             _row([("  PEND = relayed, not yet mined", "b")]),
             _row([("  v6/v4 = member address family", "b")]),
             _row([("  (cyan = mixed-family quorum)", "b")]),
             _ref("  members", "303"), _row([])]
    rows += _status(s)
    return _tt("301", rows, s, flash=_flash(s))


def ceremonies(s):
    c = s.get("cnt", {}) or {}
    rows = [_row([(" CEREMONY COUNTERS", "g")]), _rule(),
            _kv("STARTED", str(c.get("FORM+", 0)), "w"),
            _kv("DONE", str(c.get("DONE", 0)), "g"),
            _kv("ABORTED", str(c.get("ABORT", 0)),
                "r" if c.get("ABORT") else "g"),
            _row([]),
            _row([(" LIFECYCLE", "c")]),
            _kv("REFORMED", str(c.get("REFORM", 0)), "w"),
            _kv("ROTATED", str(c.get("ROTATE", 0)), "w"),
            _kv("YIELDED", str(c.get("YIELD", 0)), "w"),
            _kv("GUARD2", str(c.get("GUARD2", 0)), "y" if c.get("GUARD2") else "w"),
            _row([]),
            _row([(" TRANSPORT", "c")]),
            _kv("ACCEPTED", str(c.get("accepted", 0)), "w"),
            _kv("SEM-REJECT", str(c.get("sem-rej", 0)),
                "y" if c.get("sem-rej") else "w"),
            _kv("COMMITTED", str(c.get("commit", 0)), "w"),
            _kv("PERSISTED", str(c.get("persisted", 0)), "w"), _row([])]
    return _tt("302", rows, s, flash=_flash(s))


def roster(s):
    qm = s.get("quorum_members", {}) or {}
    fam = {q.get("qh"): (q.get("v6"), q.get("v4"))
           for q in (s.get("quorums", []) or [])}
    rows = [_row([(" MEMBERS PER ACTIVE QUORUM", "g")]), _rule()]
    if not qm:
        rows.append(_row([("  no quorums formed yet", "y")]))
    for qh, members in list(qm.items())[:4]:
        v6, v4 = fam.get(qh, (None, None))
        famtag = ("  v6:%d v4:%d" % (v6, v4)) if v6 is not None else ""
        fcls = "c" if (v6 and v4) else "b"
        rows.append(_row([(" ", "w"), (str(qh)[:12], "y"),
                          ("  %d mbr" % len(members), "c"),
                          (famtag, fcls)]))
        line = ""
        for m in members:
            short = str(m).split(":")[0]
            if len(line) + len(short) + 1 > W - 4:
                rows.append(_row([("   ", "w"), (line, "w")]))
                line = ""
            line += short + " "
        if line:
            rows.append(_row([("   ", "w"), (line, "w")]))
        rows.append(_row([]))
    return _tt("303", rows, s, flash=_flash(s))


# ══ 400s — LOTTERY ════════════════════════════════════════════════════════
def pool(s):
    rows = [_row([(" ACCUMULATOR", "g")]), _rule(),
            _kv("POOL", "%.8f" % ((s.get("pool", 0) or 0) / 1e8), "y"),
            _kv("ROLLS", str(s.get("rolls", 0)), "w"),
            _kv("ELIGIBLE", str(s.get("eligible", 0)), "w"),
            _kv("WINDOW", str(s.get("window", "?")), "w"),
            _kv("NEXT SETTLE", "h%s" % s.get("ns", "?"), "c"),
            _row([]),
            _row([(" LAST RESULT", "c")]),
            _kv("WINNER", str(s.get("winner", "--"))[:22], "y"),
            _kv("PAID", str(s.get("last_amount", "--")), "w"),
            _kv("AT HEIGHT", "h%s" % s.get("last_settle_h", "--"), "w"),
            _row([])]
    cr = s.get("chain_rolls", []) or []
    rows.append(_row([(" ON-CHAIN ROLLS  h  req  =res  lat", "c")]))
    if not cr:
        rows.append(_row([("  none decoded yet", "b")]))
    for r in cr[:4]:
        res = r.get("results")
        resx = str(res[0]) if (res and len(res) == 1) else (
            ",".join(str(x) for x in res)[:6] if res else "?")
        p = r.get("passes")
        lat = ("%dp" % p) if p else "-"      # p propagation passes ~ p*150ms
        rows.append(_row([("  h", "w"), (str(r.get("h")).ljust(5), "y"),
                          (str(r.get("req", "?")).ljust(5), "w"),
                          ("=", "b"), (resx.ljust(5), "g"),
                          (lat.rjust(4), "c" if p else "b")]))
    rows += [_row([("  lat = fan-out passes (FLOOR;", "b")]),
             _row([("  single-host under-measures)", "b")]),
             _row([]),
             _row([(" DEMAND", "c")]),
             _kv("ROLLS OK", "%s/%s" % (s.get("roll_ok", 0), s.get("roll_n", 0)), "w"),
             _kv("MEAN IVL", "%.1f blk" % (s.get("ivl", 0) or 0),
                 "g" if (s.get("ivl") or 0) < 25 else "r"),
             _row([("  ODC-052 bound: under 25 blocks", "b")]), _row([])]
    return _tt("401", rows, s, flash=_flash(s))


def settlements(s):
    hist = s.get("settlements", []) or []
    rows = [_row([(" HEIGHT   AMOUNT        WINNER", "c")]), _rule()]
    if not hist:
        rows.append(_row([("  no settlements yet", "y")]))
    for e in hist[:10]:
        rows.append(_row([(" h", "b"), (str(e.get("height", "?")).ljust(7), "w"),
                          (str(e.get("amount", "?")).ljust(14), "y"),
                          (str(e.get("gm", e.get("winner_protx", "?")))[:12], "w")]))
    rows += [_row([]), _row([("  league table ........... P403", "b")]), _row([])]
    return _tt("402", rows, s, flash=_flash(s))


def league(s):
    hist = s.get("settlements", []) or []
    tally = {}
    for e in hist:
        k = str(e.get("gm", e.get("winner_protx", "?")))[:14]
        tally[k] = tally.get(k, 0) + 1
    rows = [_row([(" WINS PER GAMEMASTER", "g")]), _rule()]
    if not tally:
        rows.append(_row([("  no settlements yet", "y")]))
    for i, (k, v) in enumerate(sorted(tally.items(), key=lambda kv: -kv[1])[:12], 1):
        rows.append(_row([(" %2d " % i, "b"), (k.ljust(18), "w"),
                          ("█" * min(v, 12), "y"), (" %d" % v, "c")]))
    rows += [_row([]),
             _row([("  from the retained settlement", "b")]),
             _row([("  history (last %d)" % len(hist), "b")]), _row([])]
    return _tt("403", rows, s, flash=_flash(s))


# ══ 500s — NODES ══════════════════════════════════════════════════════════
def gms(s):
    """CAROUSEL — 98 nodes do not fit a 40x25 page, so it rolls 1/5..5/5 like a
    real rolling page rather than truncating and pretending. Each subpage is
    padded to the SAME row budget so the page height never changes as it
    cycles — a page that grew and shrank on rotation would break the 4:3
    picture every five seconds."""
    BODY = 19                      # the wall body budget, matching _tt
    heights = s.get("heights", {}) or {}
    names = sorted(k for k in heights if k.startswith("gm"))
    maj = s.get("tip", 0)
    per = 20
    chunks = [names[i:i + per] for i in range(0, len(names), per)] or [[]]
    chunks = chunks[:5]
    subs = []
    for si, chunk in enumerate(chunks, 1):
        rr = [_row([(" NODE   HEIGHT  NODE   HEIGHT", "c")]), _rule()]
        for a in range(0, len(chunk), 2):
            cells = [("  ", "w")]
            for nm in chunk[a:a + 2]:
                h = heights.get(nm)
                cls = "g" if h == maj else ("y" if h else "r")
                cells += [(nm.ljust(7), "w"), (str(h if h else "--").ljust(8), cls)]
            rr.append(_row(cells))
        rr.append(_row([]))
        rr.append(_row([("  GM RPC-OK %s/%s   LAGGARDS %s"
                         % (s.get("gm_ok", "?"), s.get("n", "?"),
                            s.get("lag", 0)), "c")]))
        rr.append(_row([("  green = on the majority tip", "b")]))
        rr.append(_row([("  subpage %d/%d — rolls every 5s"
                         % (si, len(chunks)), "b")]))
        while len(rr) < BODY:
            rr.insert(len(rr) - 3, _row([]))
        subs.append("".join(rr[:BODY]))
    car = '<div class="car">%s</div>' % "".join("<div>%s</div>" % x for x in subs)
    return _tt("501", [car], s, flash=_flash(s))


def producers(s):
    """Under the wallet-less topology the callers ARE the producer set, so this
    is a liveness page, not an inventory: 98 GMs cannot mint a block."""
    prod = s.get("producers", {}) or {}
    heights = s.get("heights", {}) or {}
    maj = s.get("tip", 0)
    tot = sum(v.get("stake") or 0 for v in prod.values())
    onmaj = sum(v.get("stake") or 0 for k, v in prod.items()
                if heights.get(k) == maj)
    rows = [_row([(" CALLER   STAKE      HEIGHT", "c")]), _rule()]
    for k in sorted(prod):
        v = prod[k]
        h = heights.get(k)
        cls = "g" if h == maj else "r"
        rows.append(_row([(" ", "w"), (k.ljust(9), "w"),
                          (("%.0f" % v["stake"] if v.get("stake") is not None
                            else "--").ljust(11), "y"),
                          (str(h or "--"), cls)]))
    pct = (100.0 * onmaj / tot) if tot else 0
    rows += [_row([]),
             _kv("TOTAL STAKE", "%.0f" % tot, "w"),
             _kv("ON MAJ TIP", "%.0f (%.1f%%)" % (onmaj, pct),
                 "g" if pct > 50 else "r"),
             _row([]),
             _row([("  %d GMs are WALLET-LESS and can" % s.get("n", 0), "b")]),
             _row([("  NEVER mint — stake off the", "b")]),
             _row([("  majority tip cannot advance it", "b")]), _row([])]
    return _tt("502", rows, s, flash=_flash(s))


def topology(s):
    rows = [_row([(" CONNECTIVITY", "g")]), _rule(),
            _kv("NODES", str(s.get("live_n", "?")), "w"),
            _kv("UNREACHABLE", str(s.get("dead", 0)),
                "r" if s.get("dead") else "g"),
            _kv("MAJORITY", "%s/%s" % (s.get("maj_n", "?"),
                                       s.get("live_n", "?")), "w"),
            _kv("SPREAD", str(s.get("spread", 0)),
                "y" if s.get("spread") else "g"),
            _kv("AHEAD", str(s.get("ahead", 0)), "y" if s.get("ahead") else "g"),
            _kv("BEHIND", str(s.get("behind", 0)),
                "y" if s.get("behind") else "g"),
            _kv("CONTAINERS", str(s.get("conts", "?")), "w"),
            _row([]),
            _row([(" READING", "c")]),
            _row([("  AHEAD on the same tip is just", "b")]),
            _row([("  latency; a different tip is a", "b")]),
            _row([("  split. P202 tells them apart.", "b")]), _row([])]
    rows += _status(s)
    return _tt("503", rows, s, flash=_flash(s))


# ══ 600s — DIAGNOSTICS ════════════════════════════════════════════════════
def wedge(s):
    c = s.get("cnt", {}) or {}
    keys = [("ptxpayout-wrong-recipient", "WRONG RECIP"),
            ("ptxpayout-wrong-input", "WRONG INPUT"),
            ("ptxpayout-missing-at-boundary", "MISSING@BND"),
            ("bad-txns-inputs-missingorspent", "MISSING/SPENT"),
            ("ptxdkg-quorum-underfull", "UNDERFULL")]
    rows = [_row([(" REJECTION MARKERS", "g")]), _rule()]
    for k, lab in keys:
        v = c.get(k, 0)
        rows.append(_kv(lab, str(v), "r" if v else "g", pad=14))
    rows += [_row([]),
             _kv("TOTAL", str(s.get("wedge_tot", 0)),
                 "r" if s.get("wedge_tot") else "g"),
             _kv("LIVE NOW", "YES" if s.get("wedge_live") else "no",
                 "r" if s.get("wedge_live") else "g"),
             _row([]),
             _row([("  a marker is a block REFUSED by", "b")]),
             _row([("  a node — the BUG-026 signature", "b")]),
             _ref("  pose invariant", "602"), _row([])]
    rows += _status(s)
    return _tt("601", rows, s, flash=_flash(s))


def pose(s):
    """The BUG-027 invariant, per node. tickets must equal 11 x rolls; anything
    higher is reorg contamination and the ratio is roughly how many times the
    node re-connected the same PTXSESS bodies."""
    pb = s.get("pose_by_node", {}) or {}
    bad = []
    good = 0
    for k, v in pb.items():
        if not v:
            continue
        t, r = v
        if t == 11 * r:
            good += 1
        else:
            bad.append((k, t, 11 * r))
    rows = [_row([(" tickets == 11 x rolls", "g")]), _rule(),
            _kv("CLEAN", str(good), "g", pad=10),
            _kv("VIOLATIONS", str(len(bad)), "r" if bad else "g", pad=10),
            _row([])]
    if bad:
        rows.append(_row([(" NODE     TICKETS  EXPECTED", "c")]))
        for k, t, e in sorted(bad, key=lambda x: -x[1])[:10]:
            rows.append(_row([(" ", "w"), (k.ljust(9), "w"),
                              (str(t).ljust(9), "r"), (str(e), "b")]))
    else:
        rows.append(_row([("  every node holds the invariant", "g")]))
    rows += [_row([]),
             _row([("  excess = reorg contamination", "b")]),
             _row([("  (ODC-056 leg 2 / BUG-027):", "b")]),
             _row([("  pose credits were not removed", "b")]),
             _ref("  on disconnect. reorgs", "603"), _row([])]
    return _tt("602", rows, s, flash=_flash(s))


def reorgs(s):
    ro = s.get("reorgs", {}) or {}
    rows = [_row([(" CHAIN SWITCHES BY NODE", "g")]), _rule()]
    if not ro:
        rows += [_row([("  not sampled this tick", "b")]),
                 _row([("  (tip-height regressions are a", "b")]),
                 _row([("  log scan, run periodically)", "b")])]
    for k, v in sorted(ro.items(), key=lambda kv: -(kv[1] or 0))[:10]:
        rows.append(_row([("  ", "w"), (k.ljust(10), "w"),
                          ("█" * min(int(v or 0), 16), "y"), (" %s" % v, "c")]))
    rows += [_row([]),
             _row([("  a switch disconnects blocks;", "b")]),
             _row([("  before BUG-027 pose credits", "b")]),
             _row([("  survived that, so tickets grew", "b")]),
             _ref("  with switch count", "602"), _row([])]
    return _tt("603", rows, s, flash=_flash(s))


# ══ 700s — REFERENCE ══════════════════════════════════════════════════════
def register(s):
    reg = s.get("register", []) or []
    rows = [_row([(" LATEST REGISTER ENTRIES", "g")]), _rule()]
    if not reg:
        rows.append(_row([("  register unavailable", "y")]))
    for r in reg[-8:]:
        cls = "r" if r.startswith("BUG") else ("y" if r.startswith("ODC") else "c")
        rows.append(_row([("  ", "w"), (str(r)[:36], cls)]))
    rows += [_row([]),
             _kv("NEXT FREE", str(s.get("next_free", "?"))[:24], "w", pad=10),
             _row([]),
             _row([("  source: doc/ptx/", "b")]),
             _row([("  DKG_DESIGN_DOC_v1.md — the doc", "b")]),
             _row([("  stays the single source, this", "b")]),
             _row([("  page only reads it", "b")]), _row([])]
    return _tt("701", rows, s, flash=_flash(s))


GLOSSARY = [
    ("GM", "gamemaster; wallet-less, cannot"),
    ("", "stake under this topology"),
    ("CALLER", "funded node that calls ptx_roll;"),
    ("", "the whole producer set here"),
    ("TREASURY", "the caller holding collateral"),
    ("PTXSESS", "a roll, on chain (type 9)"),
    ("PTXDKG", "quorum commitment (type 11)"),
    ("POSE", "participation record; drives"),
    ("", "settlement winner selection"),
    ("QUORUM", "11 GMs holding a group key"),
    ("BOUNDARY", "every 60 blocks; settlement"),
    ("WEDGE", "a block a node refuses"),
]


def glossary(s):
    rows = [_row([(" TERMS", "g")]), _rule()]
    for term, desc in GLOSSARY:
        rows.append(_row([("  ", "w"), (term.ljust(9), "y"), (desc, "w")]))
    rows.append(_row([]))
    return _tt("702", rows, s, flash=_flash(s))


# ══ 888 — PET ═════════════════════════════════════════════════════════════
PET_FACE = {
    "good":     [r"  (\_/) ", r"  (o.o) ", r'  (")_(")'],
    "warning":  [r"  (\_/) ", r"  (o.O) ", r'  (")_(")'],
    "serious":  [r"  (\_/) ", r"  (-.-) ", r'  (")_(")'],
    "critical": [r"  (\_/) ", r"  (x.x) ", r'  (")_(")'],
}
MOOD = {"good": "HAPPY", "warning": "TWITCHY",
        "serious": "UNWELL", "critical": "UNRESPONSIVE"}


def pet(s):
    role, why = _health(s)
    cls, icon, lab = ST[role]
    rows = ['<div class="r dh"><i class="t-%s">%s</i></div>'
            % (cls, _h.escape(ln)) for ln in PET_FACE[role]]
    rows += [_row([]),
             _row([("   ", "w"), (MOOD[role], cls), ("  %s %s" % (icon, lab), cls)]),
             _row([]), _row([(" VITALS", "g")]), _rule()]
    for label, val, r, _why in _stats(s):
        if r is None:
            rows.append(_row([("  ", "w"), (label.ljust(11), "c"),
                              ("— %s" % val, "b")]))
        else:
            c, ic, _lab = ST[r]
            rows.append(_row([("  ", "w"), (label.ljust(11), "c"),
                              ("%s %s" % (ic, val), c)]))
    rows += [_row([]),
             _row([("  mood = worst of the rows above", "b")]),
             _row([("  same roll-up P201 uses, so the", "b")]),
             _row([("  two can never disagree", "b")]), _row([])]
    return _tt("888", rows, s, flash=_flash(s))


# registry: slug -> renderer, consumed by the writer and the routes
RENDER = {
    "index": index, "news": news, "alerts": alerts, "weather": weather,
    "delays": delays, "az": az, "vitals": vitals, "tube": tube,
    "splits": splits, "blocks": blocks, "board": board,
    "ceremonies": ceremonies, "roster": roster, "pool": pool,
    "settlements": settlements, "league": league, "gms": gms,
    "producers": producers, "topology": topology, "wedge": wedge,
    "pose": pose, "reorgs": reorgs, "register": register,
    "glossary": glossary, "pet": pet,
    "ix2": ix2, "ix3": ix3, "ix4": ix4, "ix5": ix5, "ix6": ix6, "ix7": ix7,
}
