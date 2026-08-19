#!/usr/bin/env python3
"""PTX W2.5b fleet dashboard — single-screen, auto-refreshing, run from node1.

    python3 /mnt/pve/Node14TB/hemis-ptx/w2-fleet/ptx_dashboard.py
    python3 ptx_dashboard.py --interval 5 --once

DESIGN NOTE (why this does not repeat fleet_watch.py's bug): event counts here
are CUMULATIVE TOTALS from a direct grep over the fresh datadirs, plus a delta
against a baseline snapshot taken at dashboard start.  There is NO byte-offset
tailing.  fleet_watch.py stores f.tell() from a TEXT-mode file -- an opaque
cookie, not a byte position -- and compares it to os.path.getsize(), so when the
cookie exceeds the size it re-reads the whole file and re-counts history into a
never-reset accumulator.  Totals-by-grep cannot drift that way.

Fits ~150x48.  Degrades gracefully on narrower terminals.
"""
import argparse
import collections
import json
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor

W2 = "/mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet"
sys.path.insert(0, W2)
try:
    import views_extra
    from harness.node import Node
except Exception as e:                                    # pragma: no cover
    sys.exit("cannot import harness.node from %s: %s" % (W2, e))

DD = "/mnt/ptx-ssd-work/w2r-fleet/datadirs"
JSONL = "/mnt/pve/Node14TB/hemis-ptx/docker-w2r/demand-w2r153.jsonl"
PROOFDIR = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet"   # dir holding bug0NN-proof.log; --proof-dir re-points
# ★ the CURRENT fleet's detector instance — the old bug034_state.json froze at
# the 311-fleet's death (tip 6223) and would render as live; --detector-state re-points
DET_STATE = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/bug034_state_w2r153.json"
# ODC-075: per-quorum "blocks below signing threshold" timer state — the height
# each quorum was FIRST observed incapable (capable<6), so the age survives a
# dashboard restart (the writers get relaunched after every host reset — a reset
# is exactly when quorums go incapable, so an in-memory-only clock would zero the
# very number this exists to show). Shared by both writer processes; atomic write.
INCAP_STATE = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/incap_timer_w2r153.json"

# ── RECENT ROLLS from the CHAIN (2026-08-10): decode a PTXSESS payload enough to
# show the roll, and classify quorum members by address family. ──
def _rcompact(b, o):
    n = b[o]; o += 1
    if n < 0xfd:  return n, o
    if n == 0xfd: return int.from_bytes(b[o:o+2], 'little'), o + 2
    if n == 0xfe: return int.from_bytes(b[o:o+4], 'little'), o + 4
    return int.from_bytes(b[o:o+8], 'little'), o + 8

def decode_ptxsess(hexstr):
    """CProbabilisticTxPayload -> {game_id, count, low, high, results, round_seed}."""
    try:
        b = bytes.fromhex(hexstr); o = 0
        gl, o = _rcompact(b, o); game_id = b[o:o+gl].decode('latin1', 'replace'); o += gl
        o += 4 + 4                                    # nSeedHeight + nExpiryHeight
        cl, o = _rcompact(b, o); o += cl              # caller_pubkey
        o += 32 + 32                                  # nonce + params_hash
        count = int.from_bytes(b[o:o+4], 'little'); o += 4
        low   = int.from_bytes(b[o:o+8], 'little', signed=True); o += 8
        high  = int.from_bytes(b[o:o+8], 'little', signed=True); o += 8
        o += 1                                        # unique
        ei, o = _rcompact(b, o); o += ei * 8          # exclude_integers
        et, o = _rcompact(b, o)                       # exclude_txids
        for _ in range(et):
            tl, o = _rcompact(b, o); o += tl
        round_seed = b[o:o+32][::-1].hex(); o += 32
        o += 32                                       # beacon
        rn, o = _rcompact(b, o)
        results = [int.from_bytes(b[o+i*8:o+i*8+8], 'little', signed=True) for i in range(rn)]
        o += rn * 8
        quorum_hash = ""
        try:                                          # quorum_hash is the last field
            o += 32                                   # quorum_sig_hash
            qm, o = _rcompact(b, o)                    # quorum_members (vec<string>)
            for _ in range(qm):
                ml, o = _rcompact(b, o); o += ml
            qs, o = _rcompact(b, o); o += qs          # quorum_sig (vec<uint8>)
            quorum_hash = b[o:o+32][::-1].hex()
        except Exception:
            pass
        return {"game_id": game_id, "count": count, "low": low, "high": high,
                "results": results, "round_seed": round_seed, "quorum_hash": quorum_hash}
    except Exception:
        return None

PTX_DECLARED_L = 8    # nSupportedQuorums (ptxbea, chainparams ptxFormation). ODC-069: a
                      # Guard-1 DECLARATION, not a formation cap — shown labelled, never
                      # as a denominator. Active quorums are pool-driven ~⌊eligible/11⌋.

# ★ FAMILY FROM GROUND TRUTH (2026-08-15): the family split is whatever family
# each GM's -externalip actually is, read from the fleet's compose file — NOT a
# modulus constant kept "in lockstep" with gen_fleet (the old %7 rule silently
# went stale when the w2r fleet moved to %5), and NOT the DGM service field
# (v4-for-everyone: it's the registration/RPC endpoint, tonight's finding).
COMPOSE = "/mnt/pve/Node14TB/hemis-ptx/docker-w2r/docker-compose.generated.yml"
_FAMILY_MAP = None
def _family_map():
    global _FAMILY_MAP
    if _FAMILY_MAP is not None:
        return _FAMILY_MAP
    import re
    m = {}
    try:
        blocks = re.split(r'\n  (?=[a-z])', open(COMPOSE).read())
        for b in blocks:
            svc = re.match(r'(gm\d+|caller\d+):', b)
            ext = re.search(r'-externalip=(\S+)', b)
            if svc and ext:
                m[svc.group(1)] = "v6" if ext.group(1).startswith("[") else "v4"
    except OSError:
        pass
    _FAMILY_MAP = m
    return m

def node_family(node_id):
    import re
    mm = re.match(r'(gm0*\d+|caller\d+)', str(node_id or ""))
    if not mm: return None
    name = mm.group(1)
    fam = _family_map()
    if fam:
        # datadir/compose names are gm05-style; node_ids may zero-pad differently
        mnum = re.match(r'gm0*(\d+)', name)
        if mnum:
            return fam.get("gm%02d" % int(mnum.group(1)))
        return fam.get(name)
    # fallback (compose unreadable): the CURRENT fleet's generator rule, i%5==0 -> v4
    mnum = re.match(r'gm0*(\d+)', name)
    if not mnum: return None
    return "v4" if int(mnum.group(1)) % 5 == 0 else "v6"
RPC_USER = os.environ.get("PTX_RPCUSER", "ptxw2rpc")
def _env_rpcpass() -> str:
    """Single source of truth for the fleet RPC password: the .env gen_fleet writes.

    ★ WHY (2026-08-06): this was a hardcoded fixture literal. When the fleet's
    credentials were rotated at rebuild, the dashboard kept sending the OLD
    password and published auth failures as though they were fleet state — the
    same defect that made 106 healthy nodes look "non-responsive" to the harness.
    Env var still wins so an operator can override; the literal is now only a
    last resort.
    """
    v = os.environ.get("PTX_RPCPASS")
    if v:
        return v
    # w2r (current fleet) first; old w2 kept as fallback for archaeology runs
    for envf in ("/mnt/pve/Node14TB/hemis-ptx/docker-w2r/.env",
                 "/mnt/pve/Node14TB/hemis-ptx/docker-w2/.env"):
        try:
            with open(envf) as f:
                for line in f:
                    line = line.strip()
                    if line.startswith("RPCPASSWORD="):
                        val = line.split("=", 1)[1].strip()
                        if val:
                            return val
        except OSError:
            continue
    return "ptxw2pass2026"


RPC_PASS = _env_rpcpass()

# type ids -> single-char marks for the recent-blocks strip
# 12 = PTXROLLCOMMIT (fund-then-sign commitment; pairs 1:1 with S in every
# roll-bearing block since BUG-034 P2) — was unmarked, hiding half the pair.
MARKS = {6: "S", 9: "C", 10: "P", 11: "D", 12: "R"}

# markers counted in ONE grep pass; label -> distinctive substring
MARKERS = [
    ("FORM+",     "ceremony session STARTED"),
    ("DONE",      "ceremony DONE"),
    ("ABORT",     "ceremony ABORTED"),
    ("EXIT",      "ceremony session EXITED"),
    ("GUARD2",    "fairness floor overrides"),
    ("REFORM",    "quorum REFORMED at height"),
    ("YIELD",     "rotation YIELDED"),
    ("ROTATE",    "PTX formation: ROTATION of"),
    # ── succession events (W2.3/W2.4 rotation gate, first observation ~h2220).
    # PTX_BLS_Promote itself logs NOTHING on success (flagged in the closure
    # ledger); SUPERSEDE fires at the same connect and is its store-side proxy.
    # PENDEXP firing near a rotation = promotion FAILED (TTL reaped the share
    # before its PTXDKG connected) — a finding, never noise.
    ("SUPERSEDE", "quorum SUPERSEDED at height"),
    ("PENDEXP",   "expiring stale PENDING share"),
    ("DISCARD",   "discarding SUPERSEDED share"),
    ("accepted",  "transport accepted"),
    ("sem-rej",   "transport semantic-reject"),
    ("catchup",   "ceremony catch-up relay"),
    ("commit",    "minable commitment stored"),
    ("persisted", "persisted PTXDKG quorum record"),
    ("poolskip",  "pool below threshold"),
    ("posereset", "PTX PoSe: reindex in progress"),
    ("verifydb",  "Verifying last"),
]
REJECTS = [
    ("ptxpayout-wrong-input",          "ptxpayout-wrong-input"),
    ("ptxpayout-missing-at-boundary",  "ptxpayout-missing-at-boundary"),
    ("ptxpayout-wrong-recipient",      "ptxpayout-wrong-recipient"),
    ("bad-txns-inputs-missingorspent", "bad-txns-inputs-missingorspent"),
    ("ptxdkg-quorum-underfull",        "ptxdkg-quorum-underfull"),
]

R, G, Y, RD, C, B, DIM = ("\033[0m", "\033[32m", "\033[33m", "\033[31m",
                          "\033[36m", "\033[1m", "\033[2m")


def col(s, c):
    return "%s%s%s" % (c, s, R)


def okbad(v, good=0):
    return col(str(v), G if v == good else RD)


# ── ANSI → HTML ───────────────────────────────────────────────────────────
# The /fleet-term.html page is the TERMINAL frame verbatim, not a re-layout of
# the same numbers.  render() stays the single source of truth for the wall
# view; this only re-paints its escape codes.  A second hand-built copy of that
# layout is exactly how a fix lands in one view and silently misses the other
# (the same drift the consensus()/build_alerts() helpers exist to prevent).
_ANSI_CLASS = {"1": "b", "2": "d", "31": "r", "32": "g", "33": "y", "36": "c"}


def ansi_to_html(text):
    """Escape-safe ANSI SGR -> nested <span class="a-*">. Unknown codes drop."""
    import html as _h
    import re
    parts = re.split(r"\x1b\[([0-9;]*)m", text)
    out, active = [], []
    for i, p in enumerate(parts):
        if i % 2 == 0:
            out.append(_h.escape(p))
            continue
        codes = [c for c in p.split(";") if c] or ["0"]
        for c in codes:
            if c == "0":
                out.append("</span>" * len(active))
                active = []
            elif c in _ANSI_CLASS:
                active.append(c)
                out.append('<span class="a-%s">' % _ANSI_CLASS[c])
    out.append("</span>" * len(active))
    return "".join(out)


def roll_label(count, low, high, unique):
    """Human label for the roll request (d20 / coin flip / draw N of M)."""
    span = high - low + 1
    if count == 1:
        if span == 2:
            return "coin flip"
        if low == 1 and span in (3, 4, 6, 8, 10, 12, 16, 20, 50, 52, 100):
            return "d%d" % span
        return "1x[%d-%d]" % (low, high)
    if unique:
        return "draw %d of %d" % (count, span)
    return "%dx[%d-%d]" % (count, low, high)


def host_mem():
    """(MemAvailable GiB, swap-used GiB, PSI-memory some avg10) or (-1,-1,-1).

    ★ 2026-08-15: the 45-day fleet's death-by-memory was VISIBLE on the Proxmox
    graphs for 24h and unwatched — the cliff (94 GiB wall, 09:52 pin + crash)
    needed an alert, not a graph. This surfaces the two real cliff signals on
    the dashboard everyone actually watches."""
    try:
        avail = swap_t = swap_f = 0
        with open("/proc/meminfo") as f:
            for ln in f:
                if ln.startswith("MemAvailable:"):
                    avail = int(ln.split()[1]) / (1024 * 1024)
                elif ln.startswith("SwapTotal:"):
                    swap_t = int(ln.split()[1]) / (1024 * 1024)
                elif ln.startswith("SwapFree:"):
                    swap_f = int(ln.split()[1]) / (1024 * 1024)
        psi = 0.0
        try:
            with open("/proc/pressure/memory") as f:
                for ln in f:
                    if ln.startswith("some"):
                        psi = float(ln.split()[1].split("=")[1])
                        break
        except OSError:
            pass
        return avail, swap_t - swap_f, psi
    except Exception:
        return -1.0, -1.0, -1.0


def host_mem_alerts(avail, swap_used, psi):
    """Shared by terminal + HTML so the two views cannot disagree."""
    a = []
    if avail < 0:
        return a
    if avail < 20:
        a.append("★ HOST MEMORY CRITICAL — %.1f GiB available (cliff killed the "
                 "45-day fleet); shed load NOW" % avail)
    elif avail < 30:
        a.append("HOST MEMORY LOW — %.1f GiB available (< 30 GiB headroom floor)" % avail)
    if swap_used > 1.0:
        a.append("HOST SWAP IN USE — %.1f GiB (pre-crash signature was swap fill)" % swap_used)
    if psi > 5.0:
        a.append("HOST MEMORY PRESSURE — PSI some avg10=%.1f (stalls happening)" % psi)
    return a


def host_hw():
    """(uptime_hours, mce_count) from /proc/uptime + the rasdaemon sqlite db.
    ★ 2026-08-15: the hardware question is OPEN (4 UC fabric sync-flood crashes,
    mixed DIMMs never swapped). While it is, host uptime + MCE count belong on
    the page everyone watches — a crash-reboot resets uptime and any MCE >0 is
    an immediate red, not a graph to notice later. Returns (-1, -1) if unreadable;
    mce=-1 renders as '?' (rasdaemon absent is NOT the same as zero MCEs)."""
    try:
        up_h = float(open("/proc/uptime").read().split()[0]) / 3600.0
    except Exception:
        up_h = -1.0
    mce = -1
    try:
        out = subprocess.run(
            ["sqlite3", "/var/lib/rasdaemon/ras-mc_event.db",
             "SELECT count(*) FROM mce_record"],
            capture_output=True, text=True, timeout=10)
        if out.returncode == 0 and out.stdout.strip().isdigit():
            mce = int(out.stdout.strip())
    except Exception:
        pass
    return up_h, mce


# ── BUG-034 detector staleness — ONE definition shared by every view ──────────
# (same anti-drift rule as consensus()/build_alerts(): the terminal and HTML
# panels must never disagree about whether the detector's state is current.)
DET_STALE_SECS   = 120   # writer died: `t` stops advancing
DET_STALE_BLOCKS = 10    # writer alive but not keeping up with the chain

def detector_staleness(d34, maj_h):
    """Is the detector's state actually CURRENT?  Two independent deaths, and
    they are NOT interchangeable:

      * wall-clock  -- the writer process died, so `t` freezes.  This is the one
        that fired for 32h on 2026-08-17/18 (the panel DID render "stale"; the
        defect was that nothing restarted the detector, not that the panel lied).
      * tip-divergence -- the writer is alive and stamping `t`, but its tip has
        fallen behind the chain: a wedged loop or a stuck RPC.  Wall-clock
        CANNOT see this failure, which is why it is worth having both.

    Returns (stale, why).  `why` is short enough to render inline."""
    if not d34:
        return (True, "absent")
    age = time.time() - d34.get("t", 0)
    if age > DET_STALE_SECS:
        return (True, ("%.0fm old" % (age / 60)) if age >= 60 else ("%.0fs old" % age))
    dtip = d34.get("tip")
    if maj_h and dtip is not None and (maj_h - dtip) > DET_STALE_BLOCKS:
        return (True, "%d blk behind" % (maj_h - dtip))
    return (False, None)

def host_hw_alerts(up_h, mce):
    a = []
    if mce > 0:
        a.append("★ HOST MCE RECORDED — %d event(s) in rasdaemon (open HW case); "
                 "harvest ras-mc-ctl --errors NOW" % mce)
    if 0 <= up_h < 0.5:
        a.append("HOST UPTIME %.0f min — unplanned reboot? (crash pattern: "
                 "host dies under fleet load)" % (up_h * 60))
    return a


def sh(cmd, timeout=60):
    try:
        return subprocess.run(["bash", "-c", cmd], capture_output=True,
                              text=True, timeout=timeout).stdout.strip()
    except Exception:
        return ""


def register_tail(n=6):
    """Latest register entries for P701. Reads the tracked design doc rather
    than duplicating its content — the doc stays the single source of truth."""
    doc = ("/mnt/pve/Node14TB/hemis-ptx/src/hemisd/doc/ptx/DKG_DESIGN_DOC_v1.md")
    out = []
    try:
        txt = open(doc, encoding="utf-8", errors="replace").read()
        import re as _re
        for m in _re.finditer(r"\*\*(BUG-\d+|KDD-\d+|ODC-\d+)[^*]{0,80}\*\*", txt):
            out.append(m.group(0).strip("*")[:38])
        nf = _re.findall(r"Next-free:\s*([^*\n]+)", txt)
        return out[-n:], (nf[-1].strip() if nf else "?")
    except Exception:
        return [], "?"


def fleet_epoch():
    """mtime of the current fleet's datadir root. Files older than this belong
    to a PREVIOUS fleet and must not be shown as live (the stale-panel trap:
    after a teardown the demand jsonl / proof logs / event baseline all still
    exist and silently describe a fleet that no longer exists)."""
    try:
        return os.path.getmtime(os.path.join(DD, "gm01"))
    except OSError:
        return 0.0


def grep_counts():
    """ONE pass over all debug.logs -> {label: count}. Ground truth, no offsets."""
    pats = "|".join(m[1] for m in MARKERS + REJECTS)
    # -a (text mode) is LOAD-BEARING: a daemon killed by a host crash leaves NUL
    # bytes in debug.log, GNU grep then declares the file binary and silently
    # suppresses every -o match — the counters read 0 while the events are in
    # the file (2026-08-15 w2r repoint finding; ugrep fails the same way).
    out = sh("cd %s 2>/dev/null && grep -rhoaE %s */ptxbea/debug.log 2>/dev/null "
             "| sort | uniq -c" % (DD, repr(pats)), timeout=120)
    raw = {}
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        n, _, txt = line.partition(" ")
        try:
            raw[txt.strip()] = int(n)
        except ValueError:
            pass
    res = {}
    for label, pat in MARKERS + REJECTS:
        res[label] = raw.get(pat, 0)
    return res


class Dash:
    def __init__(self, n, interval, callers=8, port_base=31000):
        self.n = n
        self.port_base = port_base
        self.interval = interval
        self.gms = [Node("gm%02d" % i, "127.0.0.1", port_base + i, RPC_USER, RPC_PASS)
                    for i in range(1, n + 1)]
        # ★ Caller set is DERIVED, not hardcoded (was pinned at 4 and silently
        # under-reported an 8-caller fleet as "0/4" with 5..8 invisible).
        # Mirrors gen_fleet.py's allocator exactly: caller1 takes port_base,
        # caller_k>1 takes port_base + n + (k-1).  Keep the two in step — the
        # caller count is now a CHAIN-LIVENESS parameter (callers are the sole
        # block producers under the wallet-less-GM topology), so a dashboard
        # that miscounts them is misreporting producer capacity.
        # ★ STABLE caller ports (offset 900, above the GM range) — matches
        # gen_fleet.CALLER_PORT_OFFSET / cluster.py. The old port_base+n+(k-1)
        # SHIFTED with n and, on the 150->180 grow, pointed at the empty pre-grow
        # slots (reporting caller2-8 unreachable) while the callers actually moved
        # to port_base+900+(k-1). Keep the three in step.
        CALLER_PORT_OFFSET = 900
        self.callers = [
            Node("caller%d" % k, "127.0.0.1",
                 port_base if k == 1 else port_base + CALLER_PORT_OFFSET + (k - 1),
                 RPC_USER, RPC_PASS)
            for k in range(1, callers + 1)]
        self.gm01 = self.gms[0]
        # ★ TREASURY = caller1: the wallet-holder. Every wallet-gated probe must
        # target this, NOT gm01 — GMs run -disablewallet=1, so wallet RPCs are
        # unregistered there and answer "Method not found (disabled)", which the
        # collateral-lock probe reported as the meaningless "-1/98".
        self.treasury = self.callers[0]
        self.byname = {n.name: n for n in self.gms + self.callers}
        self.blockcache = {}          # height -> (block_hash, mark string)  [hash-aware: reorg/fresh-genesis safe]
        self.timecache = {}           # height -> block unix time (vitals view)
        # ★ SPLIT EPISODES — a tube map that only shows the CURRENT tips answers
        # "are we split now?" but not "has this been happening?", which is the
        # question that actually mattered during BUG-027 (a fleet can look
        # converged between boundaries and re-split at every one). An episode
        # opens when tips>1 and CLOSES when the fleet returns to a single tip,
        # so convergences are recorded as first-class events, not inferred from
        # a gap. Persisted because the writer job restarts far more often than
        # the fleet does.
        self.split_log_path = os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "split-history.jsonl")
        self.split_open = None
        self.split_log = []
        try:
            with open(self.split_log_path) as f:
                self.split_log = [json.loads(l) for l in f if l.strip()][-40:]
        except Exception:
            self.split_log = []
        self.t0 = time.time()
        self.base = grep_counts()     # baseline for deltas
        # ★ per-consumer previous-tick ledgers. render() (term/ceefax/teletext
        # family) and emit_html() (the standard page) BOTH compute
        # movement-since-last-tick from these counts and then advance the
        # marker. When one instance renders both families per tick (the
        # unified writer loop), a single shared ledger would hand each
        # consumer half-window deltas — a live wedge could flag LIVE on one
        # page and read 0/hist on the other, depending on which half-tick the
        # counter moved in. One ledger per consumer keeps the LIVE-vs-hist
        # judgement identical to the old one-process-per-view layout.
        self.prev_cnt_by = {"term": dict(self.base), "std": dict(self.base)}
        self.prev_tip = None
        self.rate = 0.0
        self._dp = None

    # ── proTxHash -> GM label ─────────────────────────────────────────────
    # last_settle.winner_protx is the winning gamemaster's REGISTRATION txid,
    # which says nothing to a human reading the wall. protx_list carries the
    # ptxNodeId for every registration, so resolve it to "gm75" and keep the
    # hash prefix alongside (the hash is what the chain/RPC speak, so dropping
    # it entirely would make the panel harder to cross-check, not easier).
    # Cached: registrations are static for a fleet's life; refetch only on a
    # miss, so a rotation or a re-bootstrap still resolves without a restart.
    def gm_label(self, protx, width=12):
        h = str(protx or "")
        if not h or h.strip("0") == "":
            return "-"
        nid = self._protx.get(h) if getattr(self, "_protx", None) else None
        if nid is None:
            try:
                lst = self.gm01.call("protx_list")
                self._protx = {e["proTxHash"]: e.get("dgmstate", {}).get("ptxNodeId", "")
                               for e in lst}
                nid = self._protx.get(h)
            except Exception:
                self._protx = getattr(self, "_protx", {}) or {}
        if not nid:
            return h[:width]
        return "%s %s" % (nid.split(":")[0], h[:8])

    def driver_params(self):
        """Roll request shape, read from the live demand_driver cmdline."""
        if self._dp is not None:
            return self._dp
        out = sh("ps -eo args | grep -m1 '[d]emand_driver.py'")
        c, lo, hi, uq = 1, 1, 100, False
        t = out.split()
        for i, tok in enumerate(t):
            try:
                if tok == "--count":
                    c = int(t[i + 1])
                elif tok == "--low":
                    lo = int(t[i + 1])
                elif tok == "--high":
                    hi = int(t[i + 1])
                elif tok == "--unique":
                    uq = True
            except (IndexError, ValueError):
                pass
        self._dp = (c, lo, hi, uq)
        return self._dp


    # ── shared correctness-critical logic (ONE copy, used by BOTH the terminal
    # and HTML renders — a second copy is how the majority/fork fix would drift
    # out of one view, which is the exact class of bug this session kept hitting)
    @staticmethod
    def consensus(live):
        """(majority_height, majority_count, ahead, behind, laggards)."""
        hc = collections.Counter(live.values())
        maj_h, maj_n = (hc.most_common(1)[0] if hc else (0, 0))
        ahead = {k: v for k, v in live.items() if v > maj_h}
        behind = {k: v for k, v in live.items() if v < maj_h}
        lag = [k for k, v in live.items() if maj_h - v > 6]
        return maj_h, maj_n, ahead, behind, lag

    @staticmethod
    def build_alerts(dead, lag, ahead, maj_h, cnt, moved, ivl, spread):
        a = []
        if any(v > 0 for v in moved.values()):
            a.append("LIVE PAYOUT/DKG REJECTION — wedge in progress")
        if ahead:
            a.append("FORK — %d node(s) AHEAD of majority by %d (consensus split)"
                     % (len(ahead), max(ahead.values()) - maj_h))
        # ★ ABORT alerts on MOVEMENT only (same live-vs-hist rule as the wedge
        # counters): a historical abort — e.g. the 2 from the 2026-08-15 crash
        # morning on this chain — stays visible in EVENTS (red count) without
        # alarming forever.
        if moved.get("ABORT", 0) > 0:
            a.append("ceremony ABORT LIVE (+%d this tick)" % moved["ABORT"])
        if dead:
            a.append("%d node(s) unreachable" % len(dead))
        if lag:
            a.append("%d laggard(s) >6 behind" % len(lag))
        if spread > 6:
            a.append("height spread %d" % spread)
        if 0 < ivl >= 25:
            a.append("roll interval %.1f >= ODC-052 bound 25" % ivl)
        return a

    # ── collectors ────────────────────────────────────────────────────
    def heights(self):
        """One parallel sweep: height + peer count per node."""
        def one(nd):
            try:
                h = nd.getblockcount()
            except Exception:
                return nd.name, (None, None, None)
            try:
                c = nd.call("getconnectioncount")
            except Exception:
                c = None
            # ★ tip hash: the tube map groups nodes by CHAIN, not by height —
            # same height on different tips is a fork, and a height-only view
            # cannot tell those apart (it read as "converged" once already).
            try:
                tp = nd.getbestblockhash()
            except Exception:
                tp = None
            return nd.name, (h, c, tp)
        with ThreadPoolExecutor(max_workers=40) as ex:
            return dict(ex.map(one, self.gms + self.callers))

    def pose_sweep(self):
        """Per-node (tickets, rolls) — the BUG-027 invariant tickets == 11*rolls.
        One extra pair of calls per node; at a 30s tick that is the same cost the
        BUG-027 proof capture already pays, and it is the single measurement that
        would have shown the pose divergence the moment it started."""
        def one(nd):
            try:
                ls = nd.call("ptx_lottery_status")
                ps = nd.call("ptx_pose_status")
                return nd.name, (sum(r["tickets"] for r in ps),
                                 ls.get("total_rolls", 0))
            except Exception:
                return nd.name, None
        try:
            with ThreadPoolExecutor(max_workers=40) as ex:
                return dict(ex.map(one, self.gms + self.callers))
        except Exception:
            return {}

    def share_health_sweep(self):
        """ODC-070 margin-erosion panel data: aggregate each GM's OWN share
        health (ptx_quorum_health) into per-quorum signing capacity.  A node
        only knows its own share state — there is no P2P health beacon yet —
        so the fleet-level capacity view exists ONLY here, where the harness
        can ask all 153.  A quorum at exactly threshold (6) capable looks
        healthy to every consumer until the next loss fails it: that is the
        number this panel exists to surface.  RPC absent on pre-ODC-070
        binaries: nodes that error are counted in 'unreported'."""
        per_q, unreported = {}, 0

        def one(nd):
            try:
                return nd.call("ptx_quorum_health")
            except Exception:
                return None
        try:
            with ThreadPoolExecutor(max_workers=40) as ex:
                rows = list(ex.map(one, self.gms))
        except Exception:
            return {}, len(self.gms)
        for h in rows:
            if not h:
                unreported += 1
                continue
            for q in h.get("quorums", []):
                if not q.get("member"):
                    continue
                e = per_q.setdefault(q["quorum_hash"],
                                     {"members": 0, "capable": 0, "volatile": 0,
                                      "mined": q.get("mined_height")})
                e["members"] += 1
                if q.get("share_current"):
                    e["capable"] += 1
                    # BUG-039 lesson: capacity backed by a memory-only share is
                    # real NOW but evaporates at that node's next restart — a
                    # different health state than persisted capacity, and the
                    # panel must not conflate them (the 15:25 illusion).
                    if q.get("memory_only"):
                        e["volatile"] += 1
        return per_q, unreported

    def capacity_timer(self, cap, tip):
        """ODC-075: per-quorum 'blocks below signing threshold' timer.

        Idle-retirement and incapable-retirement are DIFFERENT concerns
        (registered ODC-075): a share-dead quorum today retires only because it
        LOOKS idle (no attributed roll in nRetireWindow=200 blocks — inevitable,
        it cannot sign), so the system waits ~200 blocks to reclaim capacity a
        node knew was gone at startup. This timer makes the real condition —
        capable<6 — observable directly. `cap` is the share_health_sweep() map;
        `tip` the current height. Returns {quorum_hash: blocks_below_threshold}.

        State is the FIRST height each quorum was seen incapable; recovery to
        capable>=6 CLEARS it (the clock resets — a recovered quorum starts
        fresh), and a quorum that leaves the active set (retired/reformed) is
        dropped. Persisted atomically (two writer processes share the file; both
        derive the same values from the same chain state, so last-writer-wins
        converges). tip < recorded-first (a reorg) also resets, never negative."""
        st = getattr(self, "_incap_state", None)
        if st is None:
            try:
                with open(INCAP_STATE) as f:
                    st = json.load(f)
            except Exception:
                st = {}
            self._incap_state = st
        out, live = {}, set()
        for qh, ent in cap.items():
            live.add(qh)
            if ent.get("capable", 0) < 6:
                first = st.get(qh)
                if first is None or first > tip:
                    first = tip
                    st[qh] = first
                out[qh] = max(0, tip - first)
            else:
                st.pop(qh, None)                 # recovered: clock resets
        for qh in list(st):
            if qh not in live:                   # left the active set
                st.pop(qh, None)
        try:
            tmp = INCAP_STATE + ".tmp"
            with open(tmp, "w") as f:
                json.dump(st, f)
            os.replace(tmp, INCAP_STATE)
        except Exception:
            pass
        return out

    @staticmethod
    def capacity_state(ent):
        """Three-state signing classification for a quorum's capacity entry:
        CAN-SIGN (capable>=6), DEAD (0<capable<6, cannot sign), EMPTY
        (capable==0, nothing left). Returns (label, css_class/short)."""
        c = ent.get("capable", 0)
        if c >= 6:
            return ("CAN-SIGN", "g")
        if c == 0:
            return ("EMPTY", "r")
        return ("DEAD", "r")

    def template_fail_scan(self, fresh_secs=180):
        """E2-c: surface 'block-template construction FAILED' (miner.cpp:189) from
        the PRODUCER (caller) logs — the h5065 halt signature (BUG-024/034). One
        poison tx makes EVERY template fail, so producers skip every candidate and
        the chain HALTS; the log is loud but unwatched, so the h5065 halt took ~18
        min to notice. This makes it an at-a-glance marker: the next halt is caught
        in seconds. Freshness-gated on the daemon's own UTC timestamp so an OLD,
        since-recovered failure sitting in the log tail does not raise a permanent
        false alarm. Returns None (never failed / only stale) or a dict with the
        live flag, age, latest line, and hit count."""
        import re as _re, glob, datetime
        latest_ts, latest_line, hits = None, "", 0
        for log in sorted(glob.glob(os.path.join(DD, "caller*", "ptxbea", "debug.log"))):
            txt = sh("grep -ha 'block-template construction FAILED' %s 2>/dev/null "
                     "| tail -20" % log) or ""
            for ln in txt.splitlines():
                if not ln.strip():
                    continue
                hits += 1
                m = _re.match(r'(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})', ln)
                if not m:
                    continue
                try:
                    ts = datetime.datetime.strptime(m.group(1), "%Y-%m-%dT%H:%M:%S")
                except Exception:
                    continue
                if latest_ts is None or ts > latest_ts:
                    latest_ts, latest_line = ts, ln
        if latest_ts is None:
            return None
        age = (datetime.datetime.utcnow() - latest_ts).total_seconds()
        return {"live": age <= fresh_secs, "age_s": int(age),
                "line": latest_line, "hits": hits}

    def producer_sweep(self):
        """Callers are the whole producer set under the wallet-less topology, so
        'who can actually mint' is a liveness figure, not trivia."""
        def one(nd):
            try:
                us = nd.call("listunspent", 20, 9999999)
                return nd.name, {"stake": round(sum(u["amount"] for u in us), 2),
                                 "utxos": len(us)}
            except Exception:
                return nd.name, {"stake": None, "utxos": None}
        try:
            with ThreadPoolExecutor(max_workers=12) as ex:
                return dict(ex.map(one, self.callers))
        except Exception:
            return {}

    def detector_state(self):
        """BUG-034 settle-detector state JSON (ONE reader shared by every view).
        None if absent/unreadable; the caller renders the panel inert."""
        try:
            with open(DET_STATE) as f:
                return json.load(f)
        except Exception:
            return None

    def producer_attribution(self, window=240):
        """{caller_name|'UNATTRIBUTED': blocks} over the last `window` blocks,
        from the coinstake payees chain_perq already collected (no extra block
        reads). Address→caller classification is ismine-probed ONCE per new
        address and cached — registrations of new stake addresses trickle in
        slowly, so steady-state cost is ~zero.
        ★ Gate-5/BUG-036 visibility: production concentration is the fleet's
        largest structural fragility; it gets a panel, not a script."""
        if not hasattr(self, "_addr_owner"):
            self._addr_owner = {}
        stake_addr = getattr(self, "_stake_addr", {})
        if not stake_addr:
            return {}, 0
        hi = max(stake_addr)
        lo = max(1, hi - window + 1)
        addrs = {a for h, a in stake_addr.items() if h >= lo and a}
        for a in addrs - set(self._addr_owner):
            owner = "UNATTRIBUTED"
            all_answered = True
            for nd in self.callers:
                try:
                    info = nd.call("getaddressinfo", a)
                except Exception:
                    try:
                        info = nd.call("validateaddress", a)
                    except Exception:
                        # this caller never answered — a negative reached past
                        # it is NOT a verdict on the address
                        all_answered = False
                        continue
                if info.get("ismine"):
                    owner = nd.name
                    break
            # Cache a positive always; cache the NEGATIVE only when every
            # caller actually answered. A negative probed past an unreachable
            # caller poisoned the cache permanently (caller7, 2026-08-16: its
            # addresses froze as UNATTRIBUTED after it came back — the
            # read-through-cache-poisoning class, BUG-030/036, ops edition).
            if owner != "UNATTRIBUTED" or all_answered:
                self._addr_owner[a] = owner
        out = collections.Counter()
        for h in range(lo, hi + 1):
            a = stake_addr.get(h)
            out[self._addr_owner.get(a, "UNATTRIBUTED") if a else "no-coinstake"] += 1
        return dict(out), hi - lo + 1

    def quorum_members(self, ql):
        # ★ NO [:8] cap (2026-08-10): the old cap resolved members for only the
        # first 8 quorums in ql order, so every quorum past 8 — and any sorted-
        # position quorum that happened to be ql[8:] — fell through to (0,0) in
        # quorum_family_counts, rendering a real 11-member quorum as 0/0. Quorum
        # count is pool-driven and unbounded (~floor(N/11)); resolve EVERY active
        # quorum's members. One ptx_quorum_info RPC each (~13 now, ~19 at N=216).
        out = {}
        for q in ql:
            if q.get("state") != "active":
                continue
            qh = q.get("quorum_hash")
            try:
                qi = self.gm01.call("ptx_quorum_info", qh)
                ms = qi.get("members") or qi.get("member_node_ids") or []
                out[qh] = [m if isinstance(m, str) else m.get("node_id") for m in ms]
            except Exception:
                out[qh] = []
        return out

    def quorum_family_counts(self, ql):
        """{quorum_hash: (n_v6, n_v4)} — the mixed-family composition per quorum."""
        mem = self.quorum_members(ql)
        out = {}
        for qh, ids in mem.items():
            v6 = sum(1 for nid in ids if node_family(nid) == "v6")
            v4 = sum(1 for nid in ids if node_family(nid) == "v4")
            out[qh] = (v6, v4)
        return out

    def refresh_fleet(self):
        """LIVE-DERIVED fleet size — the ground-truth GM count, NOT the frozen --n.
        (2026-08-10) The batch grow to 150 exposed '150/120'-style stale denominators:
        the numerator tracked the fleet, the /120 denominator was --n frozen at launch.
        Every 'X/N' denominator must derive from current registered state so it auto-
        tracks each grow (150 now, 180/216 next) with no restart. Also EXTENDS the GM
        node set so the dashboard actually queries gm121-150 (else the numerator is
        blind to the new nodes). Returns the live expected GM count."""
        try:
            n_live = len(self.treasury.protx_list(detailed=False, valid_only=True))
        except Exception:
            n_live = len(self.gms)
        if n_live > len(self.gms):
            for i in range(len(self.gms) + 1, n_live + 1):
                nd = Node("gm%02d" % i, "127.0.0.1", self.port_base + i, RPC_USER, RPC_PASS)
                self.gms.append(nd)
                self.byname[nd.name] = nd
        return n_live

    def quorum_capacity(self, tip):
        """(eligible, pool, capacity=⌊eligible/11⌋) from the ONE ground-truth source
        (ptx_debug_selectquorum) every view shares — so the quorum count can't read
        one way in the terminal and another in the html/ceefax (bug-5 consolidation).
        Capacity is the pool-driven ceiling (ODC-069), not nSupportedQuorums."""
        try:
            sel = self.gm01.call("ptx_debug_selectquorum",
                                 self.gm01.call("getblockhash", tip // 30 * 30))
            elig, pool = sel.get("eligible"), sel.get("pool")
            return elig, pool, (elig // 11 if elig else None)
        except Exception:
            return None, None, None

    def roll_latencies(self):
        """From the caller log, {round_seed: (passes, quorum16)}: the fan-out
        'threshold N reached ... (P attempt(s))' gives the propagation passes
        (a latency proxy, ~P*150ms), and the preceding 'DKG signing material
        quorum_hash=' + 'round_seed=' tie it to the roll."""
        import re, glob
        # Read ALL caller logs (caller1..8), not a single "caller" dir — rolls fire
        # round-robin across callers, so a single-log read missed most rolls' passes.
        # Per-caller state (cur_q/cur_seed) so interleaving across callers can't mismatch.
        lat = {}
        for log in sorted(glob.glob(os.path.join(DD, "caller*", "ptxbea", "debug.log"))):
            txt = sh("grep -haE 'signing material quorum_hash=|broadcast BEFORE signing|"
                     "threshold [0-9]+ reached' %s 2>/dev/null | tail -200" % log) or ""
            cur_q = None; cur_seed = None
            for ln in txt.splitlines():
                mq = re.search(r'quorum_hash=([0-9a-f]{16})', ln)
                if mq and 'signing material' in ln:
                    cur_q = mq.group(1)
                ms = re.search(r'round_seed=([0-9a-f]+)', ln)
                if ms:
                    cur_seed = ms.group(1)
                # log line: "threshold 6 reached — returning (6 partials, 1 attempt(s))"
                # the attempt count follows ", " not "(" — the old \( anchor never matched.
                mp = re.search(r'threshold \d+ reached.*?(\d+) attempt', ln)
                if mp and cur_seed:
                    lat[cur_seed] = (int(mp.group(1)), cur_q)
        return lat

    def _jsonl_latency_map(self):
        """{round_seed: caller-measured latency_ms} from the demand jsonl — the
        REAL request→signed-answer wall-clock the driver records, vs the fan-out
        'passes' proxy. Keyed on round_seed (the roll's unique id, in both the
        ptx_roll result and the PTXSESS payload)."""
        m = {}
        try:
            with open(JSONL) as f:
                for line in f:
                    try:
                        d = json.loads(line)
                    except Exception:
                        continue
                    rs, lm = d.get("round_seed"), d.get("latency_ms")
                    if rs and lm is not None:
                        m[rs] = lm
        except OSError:
            pass
        return m

    def chain_rolls(self, ref, tip, limit=7):
        """RECENT ROLLS from the CHAIN (not the demand JSONL): decode PTXSESS
        settles + latency. Prefer the CALLER-MEASURED latency_ms (demand jsonl,
        joined by round_seed); fall back to the fan-out 'passes' proxy from the
        caller log when a roll has no jsonl record (e.g. a manual ptx_roll)."""
        lat = self.roll_latencies()
        jlat = self._jsonl_latency_map()
        out = []; h = tip
        while h > max(tip - 300, 0) and len(out) < limit:
            try:
                blk = ref.call("getblock", ref.call("getblockhash", h))
            except Exception:
                break
            for txid in blk.get("tx", []):
                try:
                    rt = ref.call("getrawtransaction", txid, True, blk["hash"])
                except Exception:
                    continue
                if rt.get("type") != 6:      # PTXSESS
                    continue
                d = decode_ptxsess(rt.get("extraPayload", "")) or {}
                rs = d.get("round_seed", "")
                passes, _ = lat.get(rs, (None, None))
                qh = (d.get("quorum_hash") or "")[:16] or "-"
                out.append({
                    "h": h, "game": d.get("game_id", "?"),
                    "req": ("d%d" % d.get("high", 0)) if d.get("low") == 1
                           else ("%d-%d" % (d.get("low", 0), d.get("high", 0))),
                    "results": d.get("results"), "quorum": qh, "passes": passes,
                    "latency_ms": jlat.get(rs)})
            h -= 1
        return out

    def chain_perq(self, ref, tip):
        """Cumulative mined-roll count PER QUORUM, from the CHAIN — NOT the
        demand JSONL. AUDIT 2026-08-10: the QUORUMS `rolls` column used to read
        demand()'s per-quorum Counter, which tallies FIRED rolls from the driver
        log and therefore (a) misses any roll fired outside the driver (a manual
        ptx_roll) and (b) misses fired-but-not-mined rolls — same divergence
        class as the RECENT ROLLS→JSONL bug. Measured drift at audit: demand
        showed 17 across 5 quorums; the chain had 20 across 6 (quorum 1119f497's
        mined roll was absent entirely). This counts actual mined PTXSESS
        (type 6), decoding quorum_hash from the settle payload — getblock v2
        carries extraPayload, so no per-tx RPC. Incremental: only blocks newer
        than the last scan are read; the count survives across ticks."""
        if not hasattr(self, "_perq_chain"):
            self._perq_chain = collections.Counter()
            self._perq_scanned_to = 0
            self._stake_addr = {}     # height -> coinstake payee (producer panel)
        # a shallow reorg could leave a stale count; rescan the last few blocks
        # each tick by rewinding the watermark a little below tip.
        start = max(self._perq_scanned_to + 1, 1)
        if self._perq_scanned_to > tip:              # reorg shortened the chain
            self._perq_chain = collections.Counter(); start = 1
            self._stake_addr = {}
        for h in range(start, tip + 1):
            try:
                b = ref.call("getblock", ref.call("getblockhash", h), 2)
            except Exception:
                break                                # stop; resume next tick
            for t in b.get("tx", []):
                if t.get("type") == 6:               # PTXSESS
                    d = decode_ptxsess(t.get("extraPayload", "")) or {}
                    self._perq_chain[(d.get("quorum_hash") or "?")[:16]] += 1
            # coinstake payee (piggybacked — the block is already fetched):
            # vout[0] is the empty marker; the payee is the next addressed vout.
            for t in b.get("tx", [])[1:2]:
                vo = t.get("vout") or []
                if vo and vo[0].get("scriptPubKey", {}).get("type") == "nonstandard":
                    for v in vo[1:]:
                        a = v.get("scriptPubKey", {}).get("addresses")
                        if a:
                            self._stake_addr[h] = a[0]
                            break
            self._stake_addr.setdefault(h, None)
            self._perq_scanned_to = h
        for h in list(self._stake_addr):
            if h < tip - 2000:
                del self._stake_addr[h]
        return self._perq_chain

    def caller_funds(self):
        def one(nd):
            try:
                return nd.name, len(nd.listunspent(1))
            except Exception:
                return nd.name, -1
        with ThreadPoolExecutor(max_workers=4) as ex:
            return dict(ex.map(one, self.callers))

    def blocks(self, tip, span=26, ref=None):
        ref = ref or self.gm01
        lo = max(1, tip - span + 1)
        for h in range(lo, tip + 1):
            # ★ HASH-AWARE cache (fresh-genesis/reorg safety): the block at a
            # height can CHANGE (chain rebuilt, reorg) while this long-running
            # process keeps its cache. Key validity on the block HASH, not the
            # bare height — else stale marks from a destroyed chain paint phantom
            # C/S/P/D on the new chain's blocks at the same heights.
            try:
                hh = ref.call("getblockhash", h)
            except Exception:
                self.blockcache[h] = (None, "?")
                continue
            cached = self.blockcache.get(h)
            if cached and cached[0] == hh:       # same block still at h → mark valid
                continue
            try:
                b = ref.call("getblock", hh, 2)
                m = "".join(sorted({MARKS[t.get("type", 0)] for t in b["tx"]
                                    if t.get("type", 0) in MARKS}))
                self.blockcache[h] = (hh, m or "·")
                # ★ block TIME cached here for free — the vitals view needs
                # intervals, and getblock is already being paid for.
                self.timecache[h] = b.get("time")
            except Exception:
                self.blockcache[h] = (hh, "?")
        for h in list(self.blockcache):
            if h < lo - 200:
                del self.blockcache[h]
        return [(h, self.blockcache[h][1]) for h in range(lo, tip + 1)]

    def demand(self):
        rows = []
        try:
            if os.path.getmtime(JSONL) < fleet_epoch():
                raise OSError("stale jsonl (previous fleet)")
            with open(JSONL) as f:
                for line in f:
                    try:
                        rows.append(json.loads(line))
                    except Exception:
                        pass
        except Exception:
            pass
        perq = collections.Counter()
        perc = collections.Counter()
        ok = 0
        for r in rows:
            if r.get("ok"):
                ok += 1
                perq[r.get("quorum_hash", "?")[:16]] += 1
            perc[r.get("caller", "?").split("@")[0]] += 1
        interval = 0.0
        if len(rows) >= 2:
            hs = [r["h"] for r in rows if "h" in r]
            if len(hs) >= 2:
                interval = (max(hs) - min(hs)) / max(1, len(hs) - 1)
        return rows, ok, perq, perc, interval


    # ── HTML render (Vileda's remote monitor) ─────────────────────────────
    # Mobile-first: the "is the fleet OK" answer lives ENTIRELY in the header
    # (status band + tip/majority + fork callout + chips + alerts), readable on a
    # phone with NO horizontal scroll. Detail tables sit below and scroll
    # individually. Same fixed logic as the terminal view via the shared
    # consensus()/build_alerts() helpers, and the same fleet-epoch staleness
    # discipline — a dead fleet's rolls/proof-logs never render as live.
    def emit_html(self):
        import html as _h
        e = _h.escape
        n_live = self.refresh_fleet()   # shared live-derived GM count (bug-1/5 fix)
        sweep = self.heights()
        live = {k: v[0] for k, v in sweep.items() if v[0] is not None}
        conns = [v[1] for v in sweep.values() if v[1] is not None]
        dead = [k for k, v in sweep.items() if v[0] is None]
        maj_h, maj_n, ahead, behind, lag = self.consensus(live)
        spread = (max(live.values()) - min(live.values())) if live else 0
        ref = self.gm01
        for nm, h in live.items():
            if h == maj_h:
                ref = self.byname.get(nm, self.gm01)
                break
        try:    st = ref.call("ptx_lottery_status")
        except Exception: st = {}
        try:    ql = ref.call("ptx_quorum_list").get("quorums", [])
        except Exception: ql = []
        cnt = grep_counts()
        if any(cnt.get(k, 0) < self.base.get(k, 0) for k in cnt):
            # log rotation: rebase EVERY consumer's ledger, not just ours —
            # a shrunk count against a stale ledger reads as negative motion.
            self.base = dict(cnt)
            for k in self.prev_cnt_by:
                self.prev_cnt_by[k] = dict(cnt)
        moved = {l: cnt.get(l, 0) - self.prev_cnt_by["std"].get(l, 0)
                 for l, _ in REJECTS}
        moved["ABORT"] = cnt.get("ABORT", 0) - self.prev_cnt_by["std"].get("ABORT", 0)
        self.prev_cnt_by["std"] = dict(cnt)
        rows, ok, perq, perc, ivl = self.demand()
        try:    locks = len(self.treasury.call("listlockunspent")["transparent"])
        except Exception: locks = -1
        nact = sum(1 for q in ql if q.get("state") == "active")
        alerts = self.build_alerts(dead, lag, ahead, maj_h, cnt, moved, ivl, spread)
        hm_avail, hm_swap, hm_psi = host_mem()
        alerts += host_mem_alerts(hm_avail, hm_swap, hm_psi)
        hw_up, hw_mce = host_hw()
        alerts += host_hw_alerts(hw_up, hw_mce)
        wedge_tot = sum(cnt.get(l, 0) for l, _ in REJECTS)
        wedge_live = any(v > 0 for v in moved.values())

        # E2-c: block-template-construction-FAILED marker (the h5065 halt signature).
        # Alert strings are PLAIN TEXT — the render escapes each with e(a) — so use a
        # literal em-dash and do not pre-escape the log line.
        tf = self.template_fail_scan()
        tf_live = bool(tf and tf["live"])
        if tf_live:
            alerts.append('E2-c HALT RISK: a producer logged "block-template construction '
                          'FAILED" %ds ago — one poison tx fails every template (BUG-024/034); '
                          'chain halts until it clears. %s'
                          % (tf["age_s"], tf["line"][:160]))

        state = "bad" if (alerts and (wedge_live or ahead or dead or tf_live)) else ("warn" if alerts else "ok")
        P = ['<!doctype html><html lang="en"><head><meta charset="utf-8">',
             '<meta name="viewport" content="width=device-width,initial-scale=1">',
             '<meta http-equiv="refresh" content="30">',
             '<title>PTX W2.5b Fleet</title>',
             # Staleness banner: the browser happily re-fetches a FROZEN file
             # forever when the writers are down — a dashboard showing a chain
             # that no longer exists sent someone down the wrong path at 3am.
             # Client-side age check against the embedded render epoch; fires
             # at 3 min (6 missed 30s ticks).
             '<script>var PTX_RENDERED=%d;window.addEventListener("load",function(){'
             'var age=Math.floor(Date.now()/1000-PTX_RENDERED);'
             'if(age>180){var b=document.createElement("div");'
             'b.style.cssText="position:fixed;top:0;left:0;right:0;z-index:999;'
             'background:#a02020;color:#fff;font:bold 14px monospace;'
             'padding:6px 12px;text-align:center";'
             'b.textContent="STALE PAGE — writers down for "+Math.floor(age/60)+'
             '" min; this is NOT the live chain";'
             'document.body.prepend(b);}});</script>' % int(time.time()),
             '<script>(function(){try{document.documentElement.setAttribute("data-theme",'
             'localStorage.getItem("ptxTheme")||"dark")}catch(e){}})();</script>',
             '<style>',
             ':root{--bg:#0d1117;--fg:#c9d1d9;--dim:#6e7681;--ok:#3fb950;--warn:#d29922;--bad:#f85149;--cy:#39c5cf;--pan:#161b22;--br:#30363d;--bok:#0d2b16;--bwarn:#2b230d;--bbad:#2b0f0d;--balert:#3d0f0c;--falert:#ffb4ae;--btn:#21262d}',
             ':root[data-theme="light"]{--bg:#f6f8fa;--fg:#1f2328;--dim:#656d76;--ok:#1a7f37;--warn:#9a6700;--bad:#cf222e;--cy:#0969da;--pan:#ffffff;--br:#d0d7de;--bok:#dafbe1;--bwarn:#fff8c5;--bbad:#ffebe9;--balert:#ffebe9;--falert:#82071e;--btn:#eaeef2}',
             '*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);',
             'font:15px/1.45 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;padding:8px}',
             '.band{border-radius:10px;padding:10px 12px;margin-bottom:10px;border:1px solid var(--br)}',
             '.band.ok{background:var(--bok);border-color:var(--ok)}.band.warn{background:var(--bwarn);border-color:var(--warn)}',
             '.band.bad{background:var(--bbad);border-color:var(--bad)}',
             '.tip{font-size:28px;font-weight:700;letter-spacing:-.5px}',
             '.sub{font-size:14px;color:var(--dim);font-weight:400}',
             '.chips{display:flex;flex-wrap:wrap;gap:6px;margin-top:8px}',
             '.chip{padding:4px 9px;border-radius:999px;border:1px solid var(--br);background:var(--pan);font-size:14px;white-space:nowrap}',
             '.chip.ok{color:var(--ok);border-color:var(--ok)}.chip.bad{color:var(--bad);border-color:var(--bad)}',
             '.chip.warn{color:var(--warn);border-color:var(--warn)}',
             '.alert{margin-top:8px;padding:8px 10px;border-radius:8px;background:var(--balert);border:1px solid var(--bad);color:var(--falert);font-weight:700}',
             '.ctl{display:flex;gap:6px;align-items:center;margin-bottom:8px;flex-wrap:wrap}',
             '.ctl button{background:var(--btn);color:var(--fg);border:1px solid var(--br);border-radius:8px;padding:6px 12px;font:inherit;cursor:pointer;min-width:44px;min-height:34px}',
             # nav links styled as the buttons beside them: same 44x34 touch
             # target, so they stay tappable on a phone rather than becoming
             # hairline text links.
             '.ctl a.nav{background:var(--btn);color:var(--cy);border:1px solid var(--br);border-radius:8px;padding:6px 12px;font:inherit;text-decoration:none;min-height:34px;display:inline-flex;align-items:center}',
             '.ctl a.nav:hover{text-decoration:underline}',
             '.ctl button:active{opacity:.6}',
             '.alert div{margin:2px 0}.nom{margin-top:8px;color:var(--ok);font-weight:700}',
             'section{background:var(--pan);border:1px solid var(--br);border-radius:10px;padding:10px 12px;margin-bottom:10px}',
             'h2{font-size:14px;text-transform:uppercase;letter-spacing:.08em;color:var(--dim);margin:0 0 8px}',
             '.scroll{overflow-x:auto;-webkit-overflow-scrolling:touch}',
             'table{border-collapse:collapse;width:100%;font-size:14px;min-width:420px}',
             'th{text-align:left;color:var(--dim);font-weight:400;padding:3px 8px 3px 0;border-bottom:1px solid var(--br)}',
             'td{padding:3px 8px 3px 0;white-space:nowrap}',
             '.g{color:var(--ok)}.r{color:var(--bad)}.y{color:var(--warn)}.c{color:var(--cy)}.d{color:var(--dim)}',
             '.blk{font-size:13px;letter-spacing:1px;word-break:break-all}',
             'footer{color:var(--dim);font-size:13px;text-align:center;padding:4px 0 10px}',
             '</style></head><body>',
             '<div class="ctl">',
             # ★ VIEW NAV — this view had NO links to its siblings at all, so the
             # terminal and ceefax skins were reachable only by typing the URL.
             # All three views now expose all three, extensionless.
             '<a class="nav" href="/fleet">standard</a>',
             '<a class="nav" href="/fleet-term">terminal</a>',
             '<a class="nav" href="/fleet-ceefax">ceefax</a>',
             '<a class="nav" href="/fleet-news">news</a>',
             '<a class="nav" href="/fleet-vitals">vitals</a>',
             '<a class="nav" href="/fleet-board">board</a>',
             '<a class="nav" href="/fleet-tube">tube</a>',
             '<a class="nav" href="/fleet-pet">pet</a>',
             '<button onclick="ptxTheme()" title="light / dark">&#9681; theme</button>',
             '<button onclick="ptxZoom(-1)" title="smaller">A&minus;</button>',
             '<button onclick="ptxZoom(1)" title="larger">A+</button>',
             '<span class="d" id="ptxz"></span>',
             '</div>']

        # ── header: everything needed to answer "is it OK" ──
        P.append('<div class="band %s">' % state)
        forkhtml = ''
        if ahead:
            forkhtml = ('<div class="alert">FORK: %d node(s) +%d &mdash; %s</div>'
                        % (len(ahead), max(ahead.values()) - maj_h,
                           e(", ".join(sorted(ahead)[:5]))))
        P.append('<div class="tip">h%d <span class="sub">majority %d/%d</span></div>'
                 % (maj_h, maj_n, len(live) or 0))
        P.append('<div class="chips">')
        # ODC-069: active vs live capacity + declared-L label, SAME representation
        # as the terminal (shared quorum_capacity) — never active/8.
        _elig, _pool, _cap = self.quorum_capacity(maj_h)
        P.append('<span class="chip %s">quorums %d active · ~%s cap · L=%d decl</span>'
                 % ("ok" if nact else "warn", nact,
                    _cap if _cap is not None else "?", PTX_DECLARED_L))
        P.append('<span class="chip %s">wedge %d%s</span>'
                 % ("bad" if wedge_live else ("warn" if wedge_tot else "ok"),
                    wedge_tot, " LIVE" if wedge_live else (" hist" if wedge_tot else "")))
        P.append('<span class="chip %s">nodes %d/%d</span>'
                 % ("ok" if not dead else "bad",
                    len([k for k in live if k.startswith("gm")]), n_live))
        P.append('<span class="chip %s">locks %d/%d</span>'
                 % ("ok" if locks == n_live else "bad", locks, n_live))
        P.append('<span class="chip %s">spread %d</span>' % ("ok" if spread <= 6 else "bad", spread))
        if hm_avail >= 0:
            _hm_cls = "bad" if hm_avail < 20 else ("warn" if (hm_avail < 30 or hm_swap > 1) else "ok")
            P.append('<span class="chip %s">host %.0fG free · swap %.1fG</span>'
                     % (_hm_cls, hm_avail, hm_swap))
        P.append('<span class="chip %s">up %s · MCE %s</span>'
                 % ("bad" if hw_mce > 0 else ("warn" if hw_mce < 0 else "ok"),
                    ("%.1fh" % hw_up) if hw_up >= 0 else "?",
                    str(hw_mce) if hw_mce >= 0 else "?"))
        P.append('<span class="chip">pool %.4f</span>' % (st.get("pool_balance_sat", 0) / 1e8))
        P.append('<span class="chip">rolls %d</span>' % st.get("total_rolls", 0))
        P.append('</div>')
        P.append(forkhtml)
        if alerts:
            P.append('<div class="alert">' + "".join("<div>%s</div>" % e(a) for a in alerts) + '</div>')
        else:
            P.append('<div class="nom">nominal &mdash; no alerts</div>')
        P.append('</div>')

        # ── quorums ──
        perq_chain = self.chain_perq(ref, maj_h)   # mined rolls/quorum (ground truth, not demand JSONL)
        # ★ live capacity, never a frozen /8 — the LAST holdout of the frozen-
        # denominator family (the terminal view was fixed under ODC-069, this
        # header wasn't; caught in the 2026-08-15 w2r repoint).
        P.append('<section><h2>Quorums %d active &middot; ~%s cap &middot; L=%d decl</h2><div class="scroll"><table>'
                 % (nact, _cap if _cap is not None else "?", PTX_DECLARED_L))
        P.append('<tr><th>#</th><th>hash</th><th>state</th><th>f/c</th><th>formed@</th><th>mined@</th><th>rolls</th></tr>')
        for i, q in enumerate(sorted([q for q in ql if q.get("state") == "active"],
                                     key=lambda x: x.get("formation_height", 0)), 1):
            qh = str(q.get("quorum_hash", ""))[:16]
            conv = q.get("formed_size") == q.get("completed_size") == 11
            P.append('<tr><td class="d">%d</td><td class="c">%s</td><td class="%s">%s</td>'
                     '<td class="%s">%s/%s</td><td>%s</td><td>%s</td><td>%d</td></tr>'
                     % (i, e(qh), "g" if q.get("state") == "active" else "y",
                        e(str(q.get("state", "?"))), "g" if conv else "r",
                        q.get("formed_size", "?"), q.get("completed_size", "?"),
                        q.get("formation_height", "?"), q.get("mined_height", "?"),
                        perq_chain.get(qh, 0)))
        if not ql:
            P.append('<tr><td colspan="7" class="d">none formed yet</td></tr>')
        P.append('</table></div></section>')

        # ── lottery (between quorums and rolls, mirroring the terminal view) ──
        ls = st.get("last_settle", {}) or {}
        hist = st.get("settlement_history", []) or []
        nxt = st.get("next_settlement_at", 0)
        P.append('<section><h2>Lottery</h2><div class="scroll"><table>')
        P.append('<tr><th>pool</th><th>rolls</th><th>window</th><th>eligible</th><th>next settle</th></tr>')
        P.append('<tr><td class="%s">%.8f</td><td class="c">%s</td><td>%s</td><td class="c">%s</td>'
                 '<td class="c">h%s%s</td></tr>'
                 % ("g" if st.get("pool_balance_sat") else "d",
                    st.get("pool_balance_sat", 0) / 1e8,
                    st.get("total_rolls", 0), st.get("settlement_window", "?"),
                    len(st.get("eligible_nodes", [])), nxt,
                    (" (%d)" % (nxt - maj_h)) if nxt and maj_h else ""))
        P.append('</table></div>')
        if ls.get("height"):
            P.append('<div>last settle <span class="c">h%s</span> &rarr; <span class="g">%s</span>'
                     ' &middot; <span class="d">win %s &middot; tx %s</span></div>'
                     % (ls.get("height"), e(str(ls.get("amount", "0"))),
                        e(self.gm_label(ls.get("winner_protx"))),
                        e(str(ls.get("txid", ""))[:12])))
        else:
            P.append('<div class="d">last settle: none yet</div>')
        P.append('<div class="d">history: %s</div></section>'
                 % (" &middot; ".join("h%s&rarr;%s" % (x.get("height", "?"), e(str(x.get("amount", "?"))))
                                      for x in hist[-8:]) if hist else "(none yet)"))

        # ── settle detector (BUG-034 P1 — same reader as the terminal panel) ──
        d34 = self.detector_state()
        P.append('<section><h2>Settle detector (BUG-034)</h2>')
        if d34:
            stale34, why34 = detector_staleness(d34, maj_h)
            pend = d34.get("pending_count", 0)
            rate34 = d34.get("baseline_unsettled_rate_mixed")
            P.append('<div class="scroll"><table><tr><th>pending</th><th>max age</th>'
                     '<th>&Delta;c&rarr;s p50/p95</th><th>n</th><th>unsettled(mixed)</th><th>window</th><th></th></tr>')
            P.append('<tr><td class="%s">%s</td><td>%s blk</td><td class="c">%s/%s blk</td><td>%s</td>'
                     '<td class="%s">%s</td><td>%s blk</td><td class="%s">%s</td></tr></table></div>'
                     % ("r" if d34.get("alert") else ("d" if stale34 else ("y" if pend else "g")), pend,
                        d34.get("pending_max_age_blocks", 0),
                        d34.get("delta_p50", "-"), d34.get("delta_p95", "-"),
                        d34.get("delta_n", 0),
                        "y" if (rate34 or 0) > 0.25 else "d",
                        "-" if rate34 is None else "%.1f%%" % (rate34 * 100),
                        d34.get("baseline_window", "?"),
                        "r" if d34.get("alert") else ("y" if stale34 else "g"),
                        "ALERT" if d34.get("alert")
                        else ("STALE (%s)" % why34 if stale34 else "live")))
        else:
            P.append('<div class="d">detector not running &mdash; panel inert '
                     '(NOT a clean pass: pending/&Delta; unmeasured)</div>')
        P.append('</section>')

        # ── quorum signing capacity (ODC-070 margin-erosion watch) ──
        cap, unrep = self.share_health_sweep()
        P.append('<section><h2>Quorum signing capacity (ODC-070)</h2>')
        if not cap:
            P.append('<div class="d">no capacity reports%s &mdash; panel inert '
                     '(RPC ships with the share-health binary)</div>'
                     % (' (%d/%d nodes unreported)' % (unrep, len(self.gms)) if unrep else ''))
        else:
            # ODC-075: 'blocks below threshold' timer, keyed on capable<6.
            incap = self.capacity_timer(cap, maj_h)
            P.append('<div class="scroll"><table><tr><th>quorum</th><th>mined</th>'
                     '<th>state</th><th>capable</th><th>durable</th>'
                     '<th>margin over t=6</th><th>blocks below</th></tr>')
            # loop var must not be named `e` — it leaks past the loop and
            # shadows the escape helper (broke the producers table below the
            # first time cap was non-empty, i.e. first full-fleet ODC-070 tick)
            for qh, ent in sorted(cap.items(), key=lambda kv: kv[1]["capable"]):
                margin = ent["capable"] - 6
                vol = ent.get("volatile", 0)
                # durable margin: what survives every member restarting.
                dur_margin = (ent["capable"] - vol) - 6
                cls = 'r' if margin < 0 else ('y' if margin <= 1 else 'g')
                if vol and dur_margin < margin:
                    cls = 'r' if dur_margin < 0 else 'y'   # volatile-backed: never green
                voltxt = ' <span class="y">(%d volatile)</span>' % vol if vol else ''
                slabel, scls = self.capacity_state(ent)
                # a quorum one restart from incapable: durable already < 6 while
                # live capable is not — surface it BEFORE the restart, not after.
                durtxt = '%d' % (ent["capable"] - vol)
                durcls = 'r' if (ent["capable"] - vol) < 6 else ('y' if (ent["capable"] - vol) <= 7 else 'g')
                below = incap.get(qh)
                belowtxt = ('<span class="r">%d blk</span>' % below) if below is not None else '&mdash;'
                P.append('<tr><td>%s&hellip;</td><td>%s</td>'
                         '<td class="%s">%s</td><td class="%s">%d/%d%s</td>'
                         '<td class="%s">%s</td><td class="%s">%+d</td><td>%s</td></tr>'
                         % (qh[:12], ent.get("mined"), scls, slabel, cls, ent["capable"],
                            ent["members"], voltxt, durcls, durtxt, cls, margin, belowtxt))
            P.append('</table></div>')
            worst = min(e["capable"] for e in cap.values())
            tot_vol = sum(e.get("volatile", 0) for e in cap.values())
            if worst < 6:
                P.append('<div class="alert">quorum(s) BELOW THRESHOLD &mdash; unsignable '
                         'until reform/rotation replaces them</div>')
            elif worst <= 7:
                P.append('<div class="y">margin thin: worst quorum at %d/11 capable '
                         '(threshold 6) &mdash; silent erosion is the failure mode</div>' % worst)
            if tot_vol:
                P.append('<div class="y">%d share(s) MEMORY-ONLY (persist failed, ODC-035) '
                         '&mdash; that capacity evaporates at the holder&rsquo;s next restart; '
                         'durable margin shown by row colour (BUG-039 lesson)</div>' % tot_vol)
            if unrep:
                P.append('<div class="d">%d node(s) unreported (old binary or down) &mdash; '
                         'capacity figures are LOWER BOUNDS</div>' % unrep)
        P.append('</section>')

        # ── producers (Gate-5/BUG-036 concentration visibility) ──
        prod = getattr(self, "_last_prod", None) or self.producer_sweep()
        attr, attr_win = self.producer_attribution()
        tot_stake = sum((v.get("stake") or 0) for v in prod.values())
        P.append('<section><h2>Producers &middot; stake (mature) &amp; blocks over last %d</h2>'
                 '<div class="scroll"><table>' % (attr_win or 0))
        P.append('<tr><th>caller</th><th>stake</th><th>share</th><th>utxos</th><th>blocks</th><th>share</th></tr>')
        for k in sorted(prod):
            v = prod[k]; stk = v.get("stake")
            share = 100.0 * (stk or 0) / tot_stake if tot_stake else 0
            blk = attr.get(k, 0)
            bshare = 100.0 * blk / attr_win if attr_win else 0
            cls = "r" if (share > 40 or bshare > 40) else ("g" if stk else "d")
            P.append('<tr><td>%s</td><td class="%s">%s</td><td class="%s">%.0f%%</td>'
                     '<td>%s</td><td>%d</td><td class="%s">%.0f%%</td></tr>'
                     % (e(k), cls, ("%.0f" % stk) if stk is not None else "?",
                        cls, share, v.get("utxos", "?"), blk, cls, bshare))
        una = attr.get("UNATTRIBUTED", 0)
        if una:
            P.append('<tr><td colspan="6" class="y">UNATTRIBUTED coinstakes: %d</td></tr>' % una)
        P.append('</table></div></section>')

        # ── recent rolls — from the CHAIN (decoded PTXSESS), the same source as
        # the terminal view. The demand JSONL is a divergeable driver-side log
        # (misses manual rolls, counts unmined fires); it now feeds only the
        # driver-throughput footer below, epoch-guarded. ──
        crolls = self.chain_rolls(ref, maj_h, 10)
        P.append('<section><h2>Recent rolls (newest first, from chain)</h2><div class="scroll"><table>')
        P.append('<tr><th>height</th><th>game</th><th>quorum</th><th>request</th><th>result</th><th>latency</th></tr>')
        for r in crolls:
            res = r.get("results")
            resx = ", ".join(str(x) for x in res) if isinstance(res, list) and res else "-"
            lm, p = r.get("latency_ms"), r.get("passes")
            lat = ("%dms" % round(lm)) if lm is not None else ("~%dms (%dp)" % (p * 150, p) if p else "-")
            P.append('<tr><td class="c">h%s</td><td>%s</td><td class="g">%s</td><td class="d">%s</td>'
                     '<td class="g">%s</td><td class="d">%s</td></tr>'
                     % (r.get("h", "?"), e((r.get("game") or "?")[:12]),
                        e(str(r.get("quorum", "-"))[:16]), e((r.get("req") or "-")[:10]),
                        e(resx[:24]), e(lat)))
        if not crolls:
            P.append('<tr><td colspan="6" class="d">no rolls on chain yet</td></tr>')
        P.append('</table></div>')
        if rows:
            P.append('<div class="d">driver: ok %d/%d &middot; callers %s &middot; mean interval %.1f blk (ODC-052 bound &lt;25)</div>'
                     % (ok, len(rows), e(" ".join("%s=%d" % kv for kv in sorted(perc.items()))) or "-", ivl))
        else:
            P.append('<div class="d">no demand driver on this fleet (jsonl absent/stale)</div>')
        P.append('</section>')

        # ── blocks + events ──
        bl = self.blocks(maj_h, span=40, ref=ref) if maj_h else []
        P.append('<section><h2>Recent blocks &middot; S=sess R=commit C=coalesce P=payout D=dkg'
                 ' &middot; <span class="r">C/R without S = forfeit run (commit mined, no settle)</span></h2>')
        # C/R-without-S is the block-strip forfeit signature: a wall of "CR" means
        # every roll commits, fails to settle, and forfeits — rendered identically
        # to healthy activity before this colouring, which is how a forfeit run
        # went unread pre-capacity-panel. Chain-data signal, independent of node
        # self-reporting.
        def _blkcls(m):
            if m == "·":
                return "d"
            if ("C" in m or "R" in m) and "S" not in m:
                return "r"
            return "g"
        P.append('<div class="blk">%s</div>' % " ".join(
            '<span class="%s">%s</span>' % (_blkcls(m), e(m)) for _, m in bl))
        P.append('<div class="d blk">h%s &rarr; h%s</div></section>'
                 % (bl[0][0] if bl else "-", bl[-1][0] if bl else "-"))

        P.append('<section><h2>Events (grep ground-truth)</h2><div class="scroll"><table><tr>')
        for k in ("FORM+", "DONE", "ABORT", "GUARD2", "REFORM", "YIELD", "ROTATE",
                      "SUPERSEDE", "PENDEXP", "DISCARD"):
            P.append('<th>%s</th>' % e(k))
        P.append('</tr><tr>')
        for k in ("FORM+", "DONE", "ABORT", "GUARD2", "REFORM", "YIELD", "ROTATE",
                      "SUPERSEDE", "PENDEXP", "DISCARD"):
            P.append('<td class="%s">%d</td>' % ("r" if (k == "ABORT" and cnt.get(k)) else "", cnt.get(k, 0)))
        P.append('</tr></table></div>')
        P.append('<div class="scroll"><table><tr>')
        for l, _ in REJECTS:
            P.append('<th>%s</th>' % e(l.replace("ptxpayout-", "pay-")))
        P.append('</tr><tr>')
        for l, _ in REJECTS:
            v, mv = cnt.get(l, 0), moved[l]
            P.append('<td class="%s">%d%s</td>' % ("r" if mv > 0 else ("y" if v else "g"), v,
                                                   " LIVE" if mv > 0 else (" hist" if v else "")))
        P.append('</tr></table></div></section>')

        P.append('<footer>rendered %s &middot; auto-refresh 30s &middot; peers %s &middot; majority-sourced</footer>'
                 % (time.strftime("%Y-%m-%d %H:%M:%S"),
                    ("%d-%d" % (min(conns), max(conns))) if conns else "?"))
        P.append(
            '<script>'
            'function ptxApply(z){document.body.style.zoom=z;'
            'var e=document.getElementById("ptxz");if(e)e.textContent=Math.round(z*100)+"%";}'
            'function ptxTheme(){var d=document.documentElement,'
            'n=d.getAttribute("data-theme")==="light"?"dark":"light";'
            'd.setAttribute("data-theme",n);try{localStorage.setItem("ptxTheme",n)}catch(e){}}'
            'function ptxZoom(dir){var z=1;try{z=parseFloat(localStorage.getItem("ptxZoom"))||1}catch(e){}'
            'z=Math.min(2.2,Math.max(0.7,Math.round((z+dir*0.1)*10)/10));'
            'try{localStorage.setItem("ptxZoom",z)}catch(e){}ptxApply(z);}'
            '(function(){var z=1;try{z=parseFloat(localStorage.getItem("ptxZoom"))||1}catch(e){}ptxApply(z);})();'
            '</script>')
        P.append('</body></html>')
        return "".join(P)
    # ── render ────────────────────────────────────────────────────────
    # ── terminal-verbatim HTML (the wall view, published) ─────────────────
    def emit_html_term(self):
        """The TERMINAL frame, re-painted for the browser. Layout logic is
        render()'s and only render()'s. COLUMNS is pinned so the published
        frame does not reflow with whatever tty the writer job inherited."""
        os.environ["COLUMNS"] = "150"
        frame = ansi_to_html(self.render())
        s = self.snap
        dot, glow = (("#f85149", "#f8514966") if s["alerts"]
                     else ("#3fb950", "#3fb95066"))
        _st = s.get("stall_s")
        if _st is None:
            age_html = ""
        elif _st >= 180:      # ~3x target spacing — the stall signature
            age_html = ' <b style="color:#f85149">(%ds ago)</b>' % _st
        elif _st >= 120:
            age_html = ' <span style="color:#d29922">(%ds ago)</span>' % _st
        else:
            age_html = " (%ds ago)" % _st
        return (
            '<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<meta http-equiv="refresh" content="30">'
            '<title>PTX Fleet — terminal</title>'
            # Same staleness banner as the main view (frozen-file trap).
            + ('<script>var PTX_RENDERED=%d;window.addEventListener("load",function(){'
               'var age=Math.floor(Date.now()/1000-PTX_RENDERED);'
               'if(age>180){var b=document.createElement("div");'
               'b.style.cssText="position:fixed;top:0;left:0;right:0;z-index:999;'
               'background:#a02020;color:#fff;font:bold 14px monospace;'
               'padding:6px 12px;text-align:center";'
               'b.textContent="STALE PAGE — writers down for "+Math.floor(age/60)+'
               '" min; this is NOT the live chain";'
               'document.body.prepend(b);}});</script>' % int(time.time())) +
            '<style>'
            'html,body{margin:0;background:#0b0e13;color:#c9d1d9}'
            'body{font:13px/1.28 ui-monospace,SFMono-Regular,Menlo,Consolas,monospace;padding:10px}'
            '.bar{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin-bottom:8px;'
            'font-size:13px;color:#6e7681}'
            '.dot{width:9px;height:9px;border-radius:50%%;display:inline-block;'
            'background:%s;box-shadow:0 0 8px %s}'
            'a{color:#39c5cf;text-decoration:none}a:hover{text-decoration:underline}'
            '.wrap{overflow-x:auto;-webkit-overflow-scrolling:touch;'
            'border:1px solid #21262d;border-radius:8px;background:#0d1117;padding:10px}'
            'pre{margin:0;white-space:pre;font:inherit}'
            '.a-b{font-weight:700;color:#e6edf3}.a-d{color:#6e7681}'
            '.a-g{color:#3fb950}.a-y{color:#d29922}.a-r{color:#f85149}.a-c{color:#39c5cf}'
            '</style></head><body>'
            '<div class="bar"><span class="dot"></span>'
            '<b style="color:#c9d1d9">PTX fleet — terminal view</b>'
            '<span>tip h%d%s · %d/%d majority · refreshes 30s</span>'
            '<a href="/fleet">standard</a><a href="/fleet-term">terminal</a>'
            '<a href="/fleet-ceefax">ceefax</a><a href="/fleet-news">news</a>'
            '<a href="/fleet-vitals">vitals</a><a href="/fleet-board">board</a>'
            '<a href="/fleet-tube">tube</a><a href="/fleet-pet">pet</a></div>'
            '<div class="wrap"><pre>%s</pre></div></body></html>'
        ) % (dot, glow, s["tip"], age_html, s["maj_n"], s["live_n"], frame)

    # ── teletext (Ceefax) render ──────────────────────────────────────────
    def emit_html_ceefax(self):
        """40-column teletext page built from render()'s snapshot — no second
        measurement pass, so it can never disagree with the wall view."""
        import html as _h
        if getattr(self, "snap", None) is None:
            self.render()
        s = self.snap
        W = 40

        def row(cells):
            """cells: list of (text, colour-class). Padded/clipped to 40 cols."""
            out, used = [], 0
            for txt, cls in cells:
                if used >= W:
                    break
                txt = txt[:W - used]
                used += len(txt)
                out.append('<i class="t-%s">%s</i>' % (cls, _h.escape(txt)))
            if used < W:
                out.append(" " * (W - used))
            return '<div class="r">%s</div>' % "".join(out)

        def kv(label, val, cls="w", pad=11):
            return row([("  ", "w"), (label.ljust(pad), "c"), (val, cls)])

        ok = not s["alerts"]
        rate_c = "g" if 45 < s["rate"] < 90 else "y"
        maj_c = "g" if s["maj_n"] == s["live_n"] else "y"
        L = []
        # header — page number, service, clock (the Ceefax signature row)
        L.append(row([("P100", "w"), (" PTXFLEET", "c"),
                      ("  " + s["ts"] + "  " + s["clock"], "w")]))
        L.append('<div class="r dh"><i class="t-y">PTX W2.5b FLEET</i></div>')
        L.append(row([("▄" * W, "b")]))

        L.append(row([(" CHAIN", "g")]))
        L.append(kv("TIP", "h%d" % s["tip"], "y"))
        L.append(kv("MAJORITY", "%d/%d" % (s["maj_n"], s["live_n"]), maj_c))
        L.append(kv("RATE", "%.1fs/blk" % s["rate"], rate_c))
        L.append(kv("BOUNDARY", "h%d (%d)" % (s["nb"], s["nb"] - s["tip"]), "w"))
        L.append(kv("NODES", "%d/%d rpc-ok" % (s["gm_ok"], s["n"]),
                    "g" if s["gm_ok"] == s["n"] else "r"))
        L.append(row([]))

        L.append(row([(" QUORUMS", "g")]))
        L.append(kv("ACTIVE", "%d/8" % s["nact"], "g" if s["nact"] else "y"))
        L.append(kv("IN-FLIGHT", str(s["inflight"]), "y" if s["inflight"] else "w"))
        L.append(kv("POOL LEFT", "?" if s["pool_left"] is None else str(s["pool_left"]), "w"))
        L.append(kv("8/8 ETA", ("h%d" % s["proj"]) if s["need"] or s["inflight"]
                    else "REACHED", "c"))
        L.append(row([]))

        L.append(row([(" LOTTERY", "g")]))
        L.append(kv("POOL", "%.8f" % (s["pool"] / 1e8), "w"))
        L.append(kv("ROLLS", "%d   ELIGIBLE %d" % (s["rolls"], s["eligible"]), "w"))
        if s.get("last_settle_h"):
            L.append(kv("LAST WIN", str(s["winner"]), "y"))
            L.append(kv("PAID", "%s @ h%s" % (s["last_amount"], s["last_settle_h"]), "w"))
        L.append(row([]))

        c = s["cnt"]
        L.append(row([(" EVENTS", "g")]))
        L.append(row([("  ", "w"), ("FORM+ ", "c"), (str(c.get("FORM+", 0)), "w"),
                      ("  DONE ", "c"), (str(c.get("DONE", 0)), "w"),
                      ("  ABORT ", "c"),
                      (str(c.get("ABORT", 0)), "r" if c.get("ABORT") else "g")]))
        L.append(row([("  ", "w"), ("WEDGE ", "c"),
                      ("LIVE" if s["wedge_live"] else "%d hist" % s["wedge_tot"]
                       if s["wedge_tot"] else "CLEAR",
                       "r" if s["wedge_live"] else "y" if s["wedge_tot"] else "g")]))
        L.append(row([]))

        # status band — the "is the fleet OK" answer, teletext-style
        L.append(row([(" STATUS", "g")]))
        if ok:
            L.append(row([("  ", "w"), (" FLEET NOMINAL ".ljust(W - 2), "gb")]))
        else:
            for a in s["alerts"][:3]:
                L.append(row([("  ", "w"), (a[:W - 2].ljust(W - 2), "rb")]))
        while len(L) < 22:
            L.append(row([]))
        # FASTEXT footer — the four coloured link keys
        L.append(row([("▄" * W, "b")]))
        # FASTEXT keys point ONLY at live routes. /lottery.html and the other
        # PoC pages are served 410 Gone (proxy.py _RETIRED) — linking one here
        # would put a dead key on the monitor. The cyan slot is the page-number
        # label, not a link, so the bar keeps its four-key shape without one.
        # ★ FASTEXT, proper four-key colour bar: RED is the service index
        # (PTXFLEET), BLUE is OTHERS -> P101, which carries the news and links
        # on to pet / board / vitals / tube. Every key is a live route.
        L.append('<div class="r ft">'
                 '<a class="t-r" href="/fleet">PTXFLEET</a>'
                 '<a class="t-g" href="/fleet-term">TERMINAL</a>'
                 '<a class="t-y" href="/fleet-news">NEWS</a>'
                 '<a class="t-b" style="color:#5a7fff" href="/fleet-news">'
                 'OTHERS</a></div>')

        return (
            '<!doctype html><html lang="en"><head><meta charset="utf-8">'
            '<meta name="viewport" content="width=device-width,initial-scale=1">'
            '<meta http-equiv="refresh" content="30">'
            '<title>P100 PTXFLEET</title><style>'
            'html,body{margin:0;background:#000;color:#fff}'
            'body{display:flex;justify-content:center;padding:12px 6px}'
            '.tv{position:relative;background:#000;padding:10px 12px;border-radius:4px}'
            '.r{font:700 clamp(13px,3.6vw,21px)/1.32 "Cascadia Mono",ui-monospace,'
            'Menlo,Consolas,monospace;white-space:pre;letter-spacing:.06em}'
            '.dh{font-size:clamp(20px,6.6vw,34px);line-height:1.14;letter-spacing:.05em}'
            '.ft{display:flex;gap:0}.ft a{flex:1;text-align:center;text-decoration:none;'
            'font-size:clamp(11px,2.8vw,15px);padding:3px 0}'
            'i{font-style:normal}'
            '.t-w{color:#fff}.t-y{color:#ff0}.t-c{color:#0ff}.t-g{color:#0f0}'
            '.t-m{color:#f0f}.t-r{color:#f00}.t-b{color:#00a}'
            '.t-gb{color:#000;background:#0f0}.t-rb{color:#fff;background:#f00}'
            '.scan{position:absolute;inset:0;pointer-events:none;border-radius:4px;'
            'background:repeating-linear-gradient(180deg,rgba(0,0,0,.30) 0 1px,'
            'rgba(0,0,0,0) 1px 3px)}'
            '</style></head><body><div class="tv">%s<div class="scan"></div></div>'
            '</body></html>'
        ) % "".join(L)

    def render(self):
        width = min(shutil.get_terminal_size((150, 48)).columns, 200)
        L = []
        add = L.append

        n_live = self.refresh_fleet()   # live GM count + extend node set BEFORE the sweep
        sweep = self.heights()
        live = {k: v[0] for k, v in sweep.items() if v[0] is not None}
        conns = [v[1] for v in sweep.values() if v[1] is not None]
        dead = [k for k, v in sweep.items() if v[0] is None]

        # ★ CONSENSUS-FIRST (the h480 lesson). tip used to be max(), so a forked
        # node reads as a healthy tip while the truth hides in `spread`. That is
        # exactly how the h480 partition stayed invisible: gm01 alone had
        # advanced, and every measurement was taken from gm01. Report the
        # MAJORITY height, and source all chain data (blocks, quorums, lottery)
        # from a node ON the majority chain — never from whichever node happens
        # to be furthest ahead.
        hcount = collections.Counter(live.values())
        maj_h, maj_n = (hcount.most_common(1)[0] if hcount else (0, 0))
        ahead = {k: v for k, v in live.items() if v > maj_h}
        behind = {k: v for k, v in live.items() if v < maj_h}
        tip = maj_h
        lo = min(live.values()) if live else 0
        lag = [k for k, v in live.items() if maj_h - v > 6]
        ref = self.gm01
        for name, h in live.items():
            if h == maj_h:
                ref = self.byname.get(name, self.gm01)
                break

        # ★ WINDOWED rate from ON-CHAIN timestamps (last 20 blocks), recomputed
        # only when the tip advances. The previous estimator (wall-clock since
        # last tip-advance / blocks-advanced, one sample per 30s tick) was
        # quantized to k x tick-interval and read 123.9s/blk against a chain
        # measured at 61s spacing (2026-08-07 cadence verdict — the a/b/c
        # question that reading provoked). Block timestamps cannot lie about
        # spacing; two header lookups per new block is the whole cost.
        if tip > 21 and tip != self.prev_tip:
            try:
                t_hi = ref.call("getblock", ref.call("getblockhash", tip))["time"]
                t_lo = ref.call("getblock", ref.call("getblockhash", tip - 20))["time"]
                self.timecache[tip] = t_hi   # header block-age reads this pre-blocks()
                if t_hi > t_lo:
                    self.rate = (t_hi - t_lo) / 20.0
            except Exception:
                pass
            self.prev_tip = tip

        # ★ sourced from a MAJORITY-chain node (see CONSENSUS-FIRST above),
        # never from gm01-by-default: on a fork gm01 may be the minority.
        try:
            st = ref.call("ptx_lottery_status")
        except Exception:
            st = {}
        try:
            ql = ref.call("ptx_quorum_list").get("quorums", [])
        except Exception:
            ql = []

        cnt = grep_counts()
        # A teardown resets every count to 0; a baseline from the previous fleet
        # then yields NEGATIVE deltas ("0(+-44)"). Detect and re-baseline —
        # every consumer's ledger, not just ours.
        if any(cnt.get(k, 0) < self.base.get(k, 0) for k in cnt):
            self.base = dict(cnt)
            for k in self.prev_cnt_by:
                self.prev_cnt_by[k] = dict(cnt)
        d = {k: cnt.get(k, 0) - self.base.get(k, 0) for k in cnt}
        rows, ok, perq, perc, ivl = self.demand()
        funds = self.caller_funds()
        # ★ project label DERIVED from the caller's actual container (not the
        # hardcoded 'ptx-w2', which pointed at the RETIRED fleet → counted 0).
        # The container publishing the caller's RPC port names the live project.
        proj = (sh("docker ps --filter publish=%d "
                   "--format '{{.Label \"com.docker.compose.project\"}}' | head -1"
                   % self.port_base) or "").strip()
        conts = (sh("docker ps --filter label=com.docker.compose.project=%s "
                    "--format '{{.Names}}' | wc -l" % proj) if proj else "?") or "?"

        up = int(time.time() - self.t0)
        hdr = (" PTX W2.5b FLEET DASHBOARD ".center(width - 30, "═"))
        add(col("╔" + hdr + " %s ═ up %dm ═" % (time.strftime("%H:%M:%S"), up // 60), B + C))

        nb = ((tip // 30) + 1) * 30
        ns = st.get("next_settlement_at", ((tip // 60) + 1) * 60)
        fork = ""
        if ahead:
            fork = col("  ⚠ FORK: %d node(s) +%d [%s]" % (
                len(ahead), max(ahead.values()) - maj_h,
                ", ".join(sorted(ahead)[:4])), RD)
        elif len(hcount) > 1 and behind:
            fork = col("  ⚠ %d node(s) behind (max −%d)" % (
                len(behind), maj_h - min(behind.values())), Y)
        # ★ block AGE next to the tip: a growing number is the EARLIEST visible
        # sign of a stall (the h5065 halt's shape before anyone noticed). Red at
        # ~3x the 60s target spacing. Timestamp comes from caches the tick
        # already fills (rate estimator / blocks()); one fallback fetch at most.
        _tt = self.timecache.get(tip)
        if _tt is None and tip:
            try:
                _tt = ref.call("getblock", ref.call("getblockhash", tip)).get("time")
                self.timecache[tip] = _tt
            except Exception:
                _tt = None
        tip_age = int(time.time() - _tt) if _tt else None
        age_s = ("(%ds ago)" % tip_age) if tip_age is not None else "(age ?)"
        age_c = (RD if tip_age >= 180 else (Y if tip_age >= 120 else G)) \
            if tip_age is not None else DIM
        add(" %s tip %s %s %s  spread %s  rate %s  │ boundary %s (%s)  settle %s (%s)%s" % (
            col("CHAIN ", B),
            col("h%d" % tip, G if not ahead else Y),
            col(age_s, age_c),
            col("(majority %d/%d)" % (maj_n, len(live)), G if maj_n == len(live) else Y),
            okbad(max(live.values()) - lo if live else 0),
            col("%.1fs/blk" % self.rate, G if 45 < self.rate < 90 else Y),
            col("h%d" % nb, C), nb - tip, col("h%d" % ns, C), ns - tip, fork))

        nfund = sum(1 for k, v in funds.items() if v > 0)
        add(" %s %s GM rpc-ok  │ callers funded %s  │ containers %s  │ unreachable %s  │ laggards>6 %s" % (
            col("NODES ", B),
            col("%d/%d" % (len([k for k in live if k.startswith('gm')]), n_live),
                G if len([k for k in live if k.startswith('gm')]) == n_live else RD),
            # ★ Denominators DERIVED, not literal. Both were pinned to the old
            # 4-caller / 102-container fleet and would have kept reporting a
            # healthy-looking "1/4" and an amber container count on the 8-caller
            # rebuild — a dashboard quietly grading against a fleet that no
            # longer exists. Green needs a MAJORITY of callers funded, because
            # callers are the sole block producers here.
            col("%d/%d" % (nfund, len(self.callers)),
                G if nfund > len(self.callers) // 2 else
                (Y if nfund else RD)),
            col(conts, G if conts.strip() == str(n_live + len(self.callers)) else Y),
            okbad(len(dead)), okbad(len(lag))))
        if dead:
            add("   " + col("UNREACHABLE: " + ", ".join(sorted(dead)[:14]), RD))

        # health: BUG-019 lock invariant, mempool, peer spread, disk
        try:
            locks = len(self.treasury.call("listlockunspent")["transparent"])
        except Exception:
            locks = -1
        try:
            mem = len(self.gm01.call("getrawmempool"))
        except Exception:
            mem = -1
        dfroot = sh("df -h / | tail -1 | awk '{print $4\" free (\"$5\")\"}'")
        dfdata = sh("df -h /mnt/pve/Node14TB | tail -1 | awk '{print $4\" free\"}'")
        add(" %s collateral locks %s  │ mempool %s  │ peers %s  │ / %s  │ Node14TB %s" % (
            col("HEALTH", B),
            col("%d/%d" % (locks, n_live), G if locks == n_live else RD),
            col(str(mem), G if mem >= 0 else RD),
            col("%d-%d" % (min(conns), max(conns)) if conns else "?", C),
            col(dfroot or "?", Y if dfroot.endswith(("9%)", "0%)")) else G),
            col(dfdata or "?", G)))

        # one blank line above every section header keeps the wall scannable
        def sect(title):
            add("")
            add(col("─ %s " % title, B).ljust(width + 8, "─"))

        # quorums
        nact = sum(1 for q in ql if q.get("state") == "active")
        # ★ ODC-069: the active-quorum count is POOL-DRIVEN (~floor(eligible/11)),
        # NOT capped by nSupportedQuorums. Show ACTIVE vs LIVE capacity + the
        # declared L as a labelled declaration — never as a denominator that makes
        # the actual count (already 9>8) read as over-limit.
        eligible, pool_left, capacity = self.quorum_capacity(tip)
        sect("QUORUMS %d active · ~%s capacity (⌊pool/11⌋) · declared L=%d (a declaration, not a cap — ODC-069)"
             % (nact, capacity if capacity is not None else "?", PTX_DECLARED_L))
        fam = self.quorum_family_counts(ql)
        perq_chain = self.chain_perq(ref, tip)   # mined rolls/quorum (ground truth)
        # ★ ALIGNMENT RULE: pad the RAW cell to its column width FIRST, colour
        # second — %-Ns applied to a coloured string counts the invisible ANSI
        # bytes and the column drifts as values grow a digit (age 2→4, rolls
        # 1→2, v6/v4 10/1 vs 7/4). Header and rows share the same widths.
        # cap = LIVE signing capacity (ODC-070 sweep: members holding a CURRENT
        # share). f/c is the ceremony's HISTORY; cap is what can sign NOW —
        # the two diverge exactly when shares die after formation (the
        # BUG-039 outage read 11/11 f/c against 0/11 cap fleet-wide).
        # '*' = capacity partly memory-only (evaporates at that holder's
        # restart); colours use the DURABLE margin over t=6.
        cap_sweep, _capunrep = self.share_health_sweep()
        incap = self.capacity_timer(cap_sweep, tip)   # ODC-075 blocks-below-threshold
        _QW = (4, 18, 9, 7, 7, 6, 9, 8, 7, 6, 6)   # #,hash,state,f/c,cap,below,formed@,mined@,cerem,age,rolls
        _qfmt = "  " + "".join("%%-%ds" % w for w in _QW) + "%s"
        add(col(_qfmt % ("#", "quorum_hash", "state", "f/c", "cap", "below", "formed@",
                         "mined@", "cerem", "age", "rolls", "v6/v4"), DIM))
        for i, q in enumerate(sorted([q for q in ql if q.get("state") == "active"],
                                     key=lambda x: x.get("formation_height", 0)), 1):
            qh_full = str(q.get("quorum_hash", ""))
            qh = qh_full[:16]
            stt = str(q.get("state", "?"))
            fs, cs = q.get("formed_size", "?"), q.get("completed_size", "?")
            fh = q.get("formation_height", 0) or 0
            mh = q.get("mined_height", 0) or 0
            conv = (fs == cs == 11)
            v6n, v4n = fam.get(q.get("quorum_hash"), (0, 0))
            ent = cap_sweep.get(qh_full)
            if ent:
                vol = ent.get("volatile", 0)
                durable = ent["capable"] - vol
                cap_txt = "%d/%d%s" % (ent["capable"], ent["members"], "*" if vol else "")
                cap_clr = RD if durable < 6 else (Y if (durable <= 7 or vol) else G)
                # ODC-075: blocks the quorum has been below threshold (capable<6).
                _b = incap.get(qh_full)
                below_txt = ("%dblk" % _b) if _b is not None else "-"
                below_clr = RD if _b is not None else DIM
            else:
                cap_txt, cap_clr = "?", DIM
                below_txt, below_clr = "-", DIM
            add("  %s%s%s%s%s%s%-9s%-8s%-7s%-6s%-6s%s" % (
                ("%-4d" % i),
                col(qh.ljust(18), C),
                col(stt.upper().ljust(9), G if stt == "active" else Y),
                col(("%s/%s" % (fs, cs)).ljust(7), G if conv else RD),
                col(cap_txt.ljust(7), cap_clr),
                col(below_txt.ljust(6), below_clr),
                fh, mh,
                ("%dblk" % (mh - fh)) if (mh and fh) else "-",
                (tip - mh) if mh else "-",
                perq_chain.get(qh, 0),
                col("%d/%d" % (v6n, v4n), C if (v6n and v4n) else DIM)))

        # lottery
        pool = st.get("pool_balance_sat", 0)
        ls = st.get("last_settle", {}) or {}
        sect("LOTTERY")
        add("  pool %s HMS   rolls %s   window %s   eligible %s   last settle %s → %s  %s" % (
            col("%.8f" % (pool / 1e8), G if pool else DIM),
            col(str(st.get("total_rolls", 0)), C), st.get("settlement_window", "?"),
            col(str(len(st.get("eligible_nodes", []))), C),
            col("h%s" % ls.get("height", 0), C), col(str(ls.get("amount", "0")), G),
            col("win %s" % self.gm_label(ls.get("winner_protx")), DIM)))
        hist = st.get("settlement_history", []) or []
        add("  history: " + ("   ".join(
            "%s→%s" % (col("h%s" % e.get("height", "?"), C), e.get("amount", "?"))
            for e in hist[-7:]) if hist else col("(none yet)", DIM)))

        # BUG-034 settle detector — operational signal (long-pending settles =
        # relay/assembler health; delta = settle-after-commit baseline). Reads
        # the external detector's state JSON; panel is inert if it's not running.
        d34 = self.detector_state()
        if d34:
            stale34, why34 = detector_staleness(d34, maj_h)
            pend = d34.get("pending_count", 0)
            age = d34.get("pending_max_age_blocks", 0)
            rate = d34.get("baseline_unsettled_rate_mixed")
            add("  settle-detector: pending %s (max age %s blk)  Δcommit→settle p50/p95 %s/%s blk (n=%s)  unsettled %s%%/%sblk %s" % (
                col(str(pend), RD if d34.get("alert") else (DIM if stale34 else (Y if pend else G))),
                col(str(age), RD if d34.get("alert") else DIM),
                col(str(d34.get("delta_p50", "-")), C), col(str(d34.get("delta_p95", "-")), C),
                d34.get("delta_n", 0),
                col("-" if rate is None else "%.1f" % (rate * 100), Y if (rate or 0) > 0.25 else DIM),
                d34.get("baseline_window", "?"),
                col("[ALERT]", RD) if d34.get("alert")
                else (col("[STALE %s]" % why34, Y) if stale34 else "")))

        # ★ PRODUCERS (Gate-5/BUG-036 visibility, 2026-08-15): per-caller mature
        # stake + blocks-produced share. One producer holding most production is
        # the partition topology that made BUG-036 dangerous — it stays on the
        # wall, not in a script. Blocks share is chain-attributed (coinstake
        # payee → ismine, cached); stake is listunspent minconf=20 (mature only).
        prod = self.producer_sweep()
        self._last_prod = prod
        attr, attr_win = self.producer_attribution()
        sect("PRODUCERS  (stake/share · blocks/share over last %d)" % (attr_win or 0))
        tot_stake = sum((v.get("stake") or 0) for v in prod.values())
        cells = []
        for k in sorted(prod):
            v = prod[k]
            stk = v.get("stake")
            share = 100.0 * (stk or 0) / tot_stake if tot_stake else 0
            blk = attr.get(k, 0)
            bshare = 100.0 * blk / attr_win if attr_win else 0
            # red at >40% of either: no single caller may dominate post-rebalance
            c = RD if (share > 40 or bshare > 40) else (G if stk else DIM)
            # fixed cell widths (pad raw, colour after) so c1..c4 align over c5..c8
            cells.append(col("%-4s%-9s%-9s" % (
                k.replace("caller", "c"),
                ("%.0fk/%d%%" % (stk / 1000, round(share))) if stk is not None else "?",
                "%db/%.0f%%" % (blk, bshare)), c))
        for rowc in (cells[:4], cells[4:]):
            if rowc:
                add("  " + " │ ".join(rowc))
        una = attr.get("UNATTRIBUTED", 0)
        if una:
            add("  " + col("UNATTRIBUTED coinstakes in window: %d (investigate — "
                           "producer outside the caller set?)" % una, Y))

        # formation forecast — one 11-set per %30 boundary until the pool
        # (eligible minus active) drops below 11, which is what caps L at 8.
        sect("FORECAST")
        # ★ READ-AUTHORITATIVE (2026-08-03) — the pool is NOT derived any more.
        #
        # This block has now produced a phantom TWICE, and both times the cause
        # was the same shape: DERIVING pool occupancy from ceremony-event
        # arithmetic that silently assumed a lifecycle invariant which the next
        # lifecycle feature broke.
        #   1st: started-minus-ACTIVE assumed ceremonies are monotonic — false
        #        once the W4-f idle arm retires quorums (REFORM returns 11 to
        #        the pool while cumulative FORM+ keeps climbing) → "in-flight 1",
        #        pool -1.
        #   2nd: started-minus-DONE assumed every ceremony emits 11 DONE lines —
        #        false once completions DEGRADE (ODC-059): d7d212e5 emitted 10,
        #        389e32a emitted 8, so done//11 undercounts → the phantom came
        #        back as "in-flight 1", pool -1, while the chain said pool=10.
        # A third variant would break a third derivation.  So: the POOL is read
        # from the same computation the daemon itself forms with —
        # ptx_debug_selectquorum at the current cycle anchor (eligible,
        # active_excluded, pool) — and IN-FLIGHT uses STARTED-minus-EXITED,
        # because "ceremony session EXITED" is logged in the single teardown
        # epilogue every exit path funnels through (DONE, ABORT and interrupt
        # alike), so it survives aborts and degraded completions by design.
        started = cnt.get("FORM+", 0) // 11
        exited = cnt.get("EXIT", 0) // 11
        inflight = max(0, started - exited)
        # pool_left/capacity already resolved once in the QUORUMS panel — reuse
        # (one ptx_debug_selectquorum per tick). "need" = live capacity minus
        # what's committed (active + in-flight), NOT the stale 8.
        committed = nact + inflight
        target = capacity if capacity is not None else nact
        need = max(0, target - committed)
        nxt = [((tip // 30) + 1 + k) * 30 for k in range(need)]
        last_form = nxt[-1] if nxt else (tip if not inflight else tip)
        proj = (last_form + 27) if nxt else (tip + 27 if inflight else tip)
        eta = max(0, proj - tip) * self.rate / 60 if self.rate else 0
        add("  ceremonies %s (cum)  active %s  in-flight %s  │ pool %s  │ need %s: %s  │ full ~%s %s" % (
            col("%d/%d exited" % (exited, started), C), col(str(nact), G),
            col(str(inflight), Y if inflight else G),
            col("?" if pool_left is None else str(pool_left),
                DIM if pool_left is None else (G if pool_left >= 11 else Y)),
            col(str(need), C), col(" ".join("h%d" % x for x in nxt[:5]) or "none", C),
            col("h%d" % proj if (nxt or inflight) else "reached", C),
            col("ETA ~%dm" % eta if eta else "", DIM)))

        # events
        sect("EVENTS  (cumulative │ Δ since start │ green 0 = tested-clean; 0ᵖ = pending)")
        # ★ bug-6 HONEST ZEROS: a green 0 must MEAN "tested here, clean". Counters
        # that CANNOT have fired yet on this fleet are PENDING, not a pass —
        # rotation-gated events before any quorum reaches R=1440; GUARD2 latent
        # until L≈R/B under competition; the W4-e yield/reform needs an idle-or-
        # impossible quorum. Marked 0ᵖ (yellow) + footnoted, never a clean green 0.
        oldest_age = (tip - min((q.get("formation_height") or tip)
                                for q in ql if q.get("state") == "active")) if nact else 0
        R = 1440  # nRotationInterval (ptxbea) — the rotation age gate
        pending_note = {}

        def _ev(k):
            v = cnt.get(k, 0)
            dt = col("(+%d)" % d[k], G) if d.get(k) else ""
            if v > 0:
                return "%s %s%s" % (col(k, DIM), v, dt)
            # reasons are NAME-FREE and shared verbatim so identical ones GROUP
            # into one clause on the ᵖ line (it wrapped at 1080p when four
            # counters each restated the same rotation-gate sentence).
            reason = None
            if k in ("ROTATE", "SUPERSEDE", "DISCARD", "PENDEXP") and oldest_age < R:
                reason = "none aged to R=%d (~%d)" % (R, oldest_age)
            elif k == "GUARD2":
                reason = "needs contention"
            elif k == "YIELD":
                # KDD-075: yield = rotation due-selection SKIPPING a quorum that
                # is ALSO terminal-eligible — needs the R=1440 age gate AND the
                # idle overlap. Idle-only reforms (they happen every 60 blk)
                # correctly never yield; below the age gate it groups with the
                # rotation family.
                reason = ("none aged to R=%d (~%d)" % (R, oldest_age)) \
                    if oldest_age < R else "needs a due quorum that is ALSO idle"
            elif k == "REFORM":
                reason = "needs idle quorum"
            if reason:
                pending_note[k] = reason
                return "%s %s" % (col(k, DIM), col("0ᵖ", Y))
            return "%s %s" % (col(k, DIM), col("0", G))
        add("  " + "  ".join(_ev(k) for k in
            ("FORM+", "DONE", "ABORT", "GUARD2", "REFORM", "YIELD", "ROTATE",
             "SUPERSEDE", "PENDEXP", "DISCARD")))
        add("  " + "  ".join("%s %s" % (col(k, DIM), cnt.get(k, 0))
                             for k in ("accepted", "sem-rej", "catchup", "commit",
                                       "persisted", "poolskip", "verifydb", "posereset")))
        if pending_note:
            # group counters sharing a reason: NAMES — reason │ NAMES — reason
            grouped = {}
            for k, reason in pending_note.items():   # insertion order = display order
                grouped.setdefault(reason, []).append(k)
            clauses = " │ ".join("%s — %s" % ("/".join(ks), r)
                                 for r, ks in grouped.items())
            add("  " + col("ᵖ ", Y) + col("0 ≠ tested-clean: ", DIM)
                + col(clauses, Y))

        # wedge watch — folded into EVENTS to save a header row.
        # A non-zero CUMULATIVE count is history (e.g. an already-diagnosed and
        # recovered wedge); only MOVEMENT since the previous tick means live
        # trouble. Distinguishing the two is what stops this false-alarming
        # forever after a single past incident.
        moved = {lbl: cnt.get(lbl, 0) - self.prev_cnt_by["term"].get(lbl, 0)
                 for lbl, _ in REJECTS}
        wedge_live = any(v > 0 for v in moved.values())
        abort_mv = cnt.get("ABORT", 0) - self.prev_cnt_by["term"].get("ABORT", 0)
        parts = []
        for lbl, _ in REJECTS:
            v, mv = cnt.get(lbl, 0), moved[lbl]
            if mv > 0:
                s = col("%d(+%d LIVE)" % (v, mv), RD)
            elif v > 0:
                s = col("%d hist" % v, Y)
            else:
                s = col("0", G)
            parts.append("%s %s" % (col(lbl.replace("ptxpayout-", "pay-"), DIM), s))
        add("  " + col("wedge▸ ", B) + "   ".join(parts))

        # blocks strip
        sect("RECENT BLOCKS   S=PTXSESS  R=ROLLCOMMIT  C=COALESCE  P=PAYOUT  D=PTXDKG  ·=plain")
        # span chosen from terminal width so the strip never needs truncating
        # (slicing a coloured string mid-escape-sequence corrupts the render).
        span = max(8, (width - 4) // 4)
        bl = self.blocks(tip, span=span, ref=ref)
        add("  " + " ".join(col(m.ljust(3), G if m != "·" else DIM) for _, m in bl))
        add("  " + " ".join(col(str(h)[-3:].ljust(3), DIM) for h, _ in bl))

        # recent rolls — the per-roll beacon view. Aggregate per-quorum counts
        # already live in the QUORUMS table's `rolls` column, so the bar chart
        # was redundant; this shows §7.4 routing roll-by-roll instead.
        sect("RECENT ROLLS  (newest first, from chain)")
        add(col("  %-9s%-11s%-18s%-9s%-17s%s" % ("height", "game", "quorum",
                                                 "request", "result", "latency"), DIM))
        crolls = self.chain_rolls(ref, tip, 7)
        for r in crolls:
            res = r.get("results")
            resx = ", ".join(str(x) for x in res) if isinstance(res, list) and res else "-"
            p = r.get("passes")
            lm = r.get("latency_ms")
            if lm is not None:                    # caller-measured req→answer (real)
                lat = "%dms" % round(lm)
                latcol = Y if lm > 3000 else G
            elif p:                               # fan-out passes proxy (fallback)
                lat = "~%dms (%dp)" % (p * 150, p)
                latcol = Y if p > 4 else G
            else:
                lat = "-"; latcol = DIM
            add("  %s%-11s%s%-9s%s%s" % (
                col(("h%s" % r.get("h", "?")).ljust(9), C),
                (r.get("game", "?") or "?")[:10],
                col(str(r.get("quorum", "-"))[:16].ljust(18), G),
                (r.get("req", "-") or "-")[:8],
                col(resx[:16].ljust(17), G),
                col(lat, latcol)))
        if not crolls:
            add(col("  (no rolls on chain yet)", DIM))
        add("  %s from on-chain PTXSESS  │ latency = caller-measured req→answer "
            "(jsonl); ~Np = fan-out passes proxy" % col("§rolls▸", B))

        # composition-proof legs (step 10) — read from the proof logs
        sect("COMPOSITION PROOF (BUG-024 boundary×demand │ BUG-023 restart │ BUG-025 reindex)")
        for tag, path, want in (
                ("BUG-024", os.path.join(PROOFDIR, "bug024-proof.log"), "VERDICT"),
                ("BUG-023", os.path.join(PROOFDIR, "bug023-proof.log"), "VERDICT"),
                ("BUG-025", os.path.join(PROOFDIR, "bug025-proof.log"), "VERDICT")):
            if not os.path.exists(path) or os.path.getmtime(path) < fleet_epoch():
                add("  %s %s" % (col(tag, DIM), col("not started", DIM)))
                continue
            v = sh("grep -a '%s' %s 2>/dev/null | tail -1" % (want, path))
            last = sh("tail -1 %s 2>/dev/null" % path)
            if v:
                add("  %s %s" % (col(tag, B), col(v[:width - 12],
                                                  G if "PASS" in v else RD)))
            else:
                add("  %s %s" % (col(tag, B), col(("running: " + last)[:width - 12], Y)))

        # alerts
        alerts = []
        if len(dead):
            alerts.append("%d node(s) unreachable" % len(dead))
        if ahead:
            alerts.append("★ FORK — %d node(s) AHEAD of the majority by %d (consensus split)"
                          % (len(ahead), max(ahead.values()) - maj_h))
        if len(lag):
            alerts.append("%d laggard(s) >6 behind" % len(lag))
        if wedge_live:
            alerts.append("LIVE PAYOUT/DKG REJECTION — wedge in progress")
        # movement-only (hist aborts stay a red EVENTS count, not a siren)
        if abort_mv > 0:
            alerts.append("ceremony ABORT LIVE (+%d this tick)" % abort_mv)
        if tip - lo > 6:
            alerts.append("height spread %d" % (tip - lo))
        if 0 < ivl >= 25:
            alerts.append("roll interval %.1f >= ODC-052 bound 25" % ivl)
        _hma, _hms, _hmp = host_mem()
        alerts += host_mem_alerts(_hma, _hms, _hmp)
        _hwu, _hwm = host_hw()
        alerts += host_hw_alerts(_hwu, _hwm)
        _tf = self.template_fail_scan()          # E2-c halt-signature marker
        if _tf and _tf["live"]:
            alerts.append("E2-c HALT RISK: block-template FAILED %ds ago (BUG-024/034 poison-tx)"
                          % _tf["age_s"])
        sect("ALERTS")
        if _hma >= 0:
            add("  " + col("host mem: %.1f GiB free · swap %.1f GiB · PSI %.1f"
                           % (_hma, _hms, _hmp),
                           RD if _hma < 20 else (Y if (_hma < 30 or _hms > 1) else DIM)))
        add("  " + col("host hw:  up %s · MCE %s (rasdaemon)"
                       % (("%.1fh" % _hwu) if _hwu >= 0 else "?",
                          str(_hwm) if _hwm >= 0 else "?"),
                       RD if _hwm > 0 else (Y if _hwm < 0 else DIM)))
        add("  " + (col("  ".join(alerts), RD) if alerts else col("none — fleet nominal", G)))
        hist = [l for l, _ in REJECTS if cnt.get(l, 0) and moved[l] == 0]
        if hist and not wedge_live:
            add("  " + col("history (static, not live): " + ", ".join(hist), DIM))
        self.prev_cnt_by["term"] = dict(cnt)
        # ── snapshot for the teletext render ──────────────────────────────
        # Stashed from values render() ALREADY computed, so /fleet-ceefax.html
        # costs no extra RPC sweep and no second grep pass over 98 debug.logs —
        # it cannot disagree with the wall view because it never re-measures.
        # ── derived fields for the alternate views ────────────────────────
        # ★ STALL is computed from the TIP BLOCK'S OWN TIMESTAMP against
        # wall-clock — never from "did the number change since last tick".
        # That distinction is the BUG-027 lesson: a chain static for 48 minutes
        # read as healthy because every derived field was stable.
        _now = time.time()
        _tiptime = self.timecache.get(tip)
        stall_s = int(_now - _tiptime) if _tiptime else None
        # recent inter-block intervals (seconds) — the vitals waveform
        _hs = sorted(h for h in self.timecache if h <= tip and h > tip - 60)
        intervals = []
        for a, b in zip(_hs, _hs[1:]):
            d = (self.timecache[b] or 0) - (self.timecache[a] or 0)
            if 0 < d < 3600:
                intervals.append({"h": b, "s": d})
        # nodes grouped by TIP HASH — chain identity, not height
        tipgroups = {}
        for k, v in sweep.items():
            if v[0] is not None and len(v) > 2 and v[2]:
                tipgroups.setdefault(v[2], []).append(k)

        # ── split-episode bookkeeping (diverge -> converge) ────────────────
        _ntips = len(tipgroups)
        if _ntips > 1:
            _minor = sorted((sorted(v) for k, v in tipgroups.items()),
                            key=len)[:-1]           # every group but the largest
            _stray = sorted({n for grp in _minor for n in grp})
            if self.split_open is None:
                self.split_open = {"from_h": tip, "to_h": None, "max_tips": _ntips,
                                   "nodes": _stray, "t0": time.strftime("%H:%M:%S")}
            else:
                self.split_open["max_tips"] = max(self.split_open["max_tips"], _ntips)
                self.split_open["nodes"] = sorted(
                    set(self.split_open["nodes"]) | set(_stray))
        elif self.split_open is not None:
            ep = self.split_open
            ep["to_h"] = tip
            ep["t1"] = time.strftime("%H:%M:%S")
            ep["blocks"] = max(0, tip - ep["from_h"])
            self.split_open = None
            self.split_log.append(ep)
            self.split_log = self.split_log[-40:]
            try:
                with open(self.split_log_path, "a") as f:
                    f.write(json.dumps(ep) + "\n")
            except Exception:
                pass

        try:
            _pose = self.pose_sweep()
        except Exception:
            _pose = {}
        _prod = getattr(self, "_last_prod", None) or {}
        try:
            _qmem = self.quorum_members(ql)
        except Exception:
            _qmem = {}
        try:
            _fam = self.quorum_family_counts(ql)
        except Exception:
            _fam = {}
        try:
            _crolls = self.chain_rolls(ref, tip, limit=6)
        except Exception:
            _crolls = []
        _reg, _nextfree = register_tail()

        self.snap = {
            "pose_by_node": _pose, "producers": _prod, "quorum_members": _qmem,
            "register": _reg, "next_free": _nextfree,
            "settlements": (st.get("settlement_history") or [])[:12],
            "stall_s": stall_s, "intervals": intervals,
            "split_log": list(self.split_log[-8:]),
            "split_open": dict(self.split_open) if self.split_open else None,
            "tipgroups": {k: sorted(v) for k, v in tipgroups.items()},
            "heights": dict(live),
            "quorums": [{"qh": q.get("quorum_hash", "?"),
                         "state": q.get("state", "?"),
                         "formed": q.get("formation_height"),
                         "mined": q.get("mined_height"),
                         "size": q.get("formed_size"),
                         "compl": q.get("completed_size"),
                         "v6": _fam.get(q.get("quorum_hash", "?"), (None, None))[0],
                         "v4": _fam.get(q.get("quorum_hash", "?"), (None, None))[1]}
                        for q in ql],
            "chain_rolls": _crolls,
            "target_block_s": 60,
            "tip": tip, "maj_n": maj_n, "live_n": len(live),
            "spread": (max(live.values()) - lo) if live else 0,
            "rate": self.rate, "nb": nb, "ns": ns,
            "gm_ok": len([k for k in live if k.startswith("gm")]), "n": n_live,
            "nfund": nfund, "conts": conts, "dead": len(dead), "lag": len(lag),
            "ahead": len(ahead), "behind": len(behind),
            "locks": locks, "mem": mem, "nact": nact, "nq": len(ql),
            "capacity": capacity, "declared_L": PTX_DECLARED_L,
            "pool": pool, "rolls": st.get("total_rolls", 0),
            "winner": self.gm_label(ls.get("winner_protx")),
            "last_settle_h": ls.get("height", 0), "last_amount": ls.get("amount", "0"),
            "window": st.get("settlement_window", "?"),
            "eligible": len(st.get("eligible_nodes", []) or []),
            "started": started, "inflight": inflight, "pool_left": pool_left,
            "need": need, "nxt": list(nxt), "proj": proj, "eta": eta,
            "cnt": dict(cnt), "wedge_live": wedge_live,
            "wedge_tot": sum(cnt.get(l, 0) for l, _ in REJECTS),
            "alerts": list(alerts), "blocks": list(bl),
            "roll_ok": ok, "roll_n": len(rows), "ivl": ivl,
            "ts": time.strftime("%a %d %b"), "clock": time.strftime("%H:%M:%S"),
        }
        return "\n".join(L)


def main():
    ap = argparse.ArgumentParser()
    # defaults track the CURRENT fleet (w2r 153+8, port 32000, 2026-08-15
    # fresh genesis) — refresh_fleet() live-derives the GM count each tick
    # anyway; --n only seeds the initial node set.
    ap.add_argument("--n", type=int, default=153)
    ap.add_argument("--callers", type=int, default=8,
                    help="caller count — MUST match gen_fleet's --callers. Under "
                         "the wallet-less-GM topology callers are the sole block "
                         "producers, so this is a liveness figure, not cosmetic.")
    ap.add_argument("--port-base", type=int, default=32000)
    ap.add_argument("--detector-state", default=None,
                    help="BUG-034 settle-detector state JSON (default = the w2r153 "
                         "instance; the old bug034_state.json froze at the 311-fleet's "
                         "death and must not render as live)")
    ap.add_argument("--compose", default=None,
                    help="fleet compose file for the ground-truth -externalip "
                         "family map (default = docker-w2r generated compose)")
    ap.add_argument("--datadir", default=None,
                    help="datadir root for the EVENT/ceremony log grep. "
                         "Defaults to the soak path; the fresh fleet needs the SSD "
                         "path (else the RPC lines show the new fleet but the "
                         "event/roll lines show the SOAK — a Frankenstein view).")
    ap.add_argument("--proof-dir", default=None,
                    help="dir holding bug0NN-proof.log for the COMPOSITION PROOF panel "
                         "(default = retired w2-fleet tree; re-point at the permanent fleet)")
    ap.add_argument("--jsonl", default=None,
                    help="demand-driver JSONL for RECENT ROLLS (default = soak's "
                         "demand-N98.jsonl). Point at the fresh fleet's file so it "
                         "does not show the soak's rolls (h2882...).")
    ap.add_argument("--interval", type=float, default=10.0)
    ap.add_argument("--once", action="store_true")
    ap.add_argument("--html", default=None,
                    help="render HTML to this path instead of the terminal")
    ap.add_argument("--html-term", default=None,
                    help="render the TERMINAL frame verbatim as HTML to this path")
    ap.add_argument("--html-ceefax", default=None,
                    help="render the teletext (Ceefax) page to this path")
    # ★ Four alternate skins of the SAME tick — no extra RPC sweep, no extra
    # 98-log grep; they consume the snapshot render() already produced.
    ap.add_argument("--teletext-dir", default=None,
                    help="write EVERY teletext page (P100..P888) into this dir; "
                         "pages come from views_extra.PAGES so routes, writer and "
                         "the on-page index can never drift apart")
    ap.add_argument("--serve", action="store_true",
                    help="writer-job mode: re-render every requested view "
                         "each --interval seconds")
    a = ap.parse_args()
    global DD, JSONL, PROOFDIR, DET_STATE, COMPOSE
    if a.datadir:
        DD = a.datadir     # re-point the EVENT/ceremony grep at the fleet under view
    if a.jsonl:
        JSONL = a.jsonl    # re-point the DEMAND panel (driver throughput) at the fleet's demand log
    if a.proof_dir:
        PROOFDIR = a.proof_dir   # re-point COMPOSITION PROOF logs off the retired w2-fleet tree
    if a.detector_state:
        DET_STATE = a.detector_state
    if a.compose:
        COMPOSE = a.compose
    d = Dash(a.n, a.interval, callers=a.callers, port_base=a.port_base)

    # ── unified writer job ────────────────────────────────────────────────
    # ONE instance renders every requested view per tick. The term, ceefax
    # and teletext pages all ride a single render() (the teletext family
    # reads the snapshot render() stashes — no re-measure, they cannot
    # disagree with the wall view). The standard page (emit_html) still runs
    # its own measurement pass, so a combined instance costs the same two
    # sweeps the historical two-process layout did — the win is one process
    # to manage and one credential import, not fewer sweeps. The two page
    # families keep separate movement ledgers (prev_cnt_by) so LIVE-vs-hist
    # wedge judgement stays full-interval on both; see __init__.
    # Atomic replace throughout so the Flask origin never serves a torn
    # file; never abort the loop on a transient RPC/IO failure — a remote
    # monitor that dies on one bad tick is worse than a stale tick. Each
    # family gets its own try so one failing page cannot freeze the others.
    if a.html or a.html_term or a.html_ceefax or a.teletext_dir:
        def write(path, text):
            tmp = path + ".tmp"
            with open(tmp, "w") as f:
                f.write(text)
            os.replace(tmp, path)

        while True:
            if a.html_term or a.html_ceefax or a.teletext_dir:
                try:
                    term = d.emit_html_term()    # calls render(); fills d.snap
                    if a.html_term:
                        write(a.html_term, term)
                    if a.html_ceefax:
                        write(a.html_ceefax, d.emit_html_ceefax())
                    if a.teletext_dir:
                        for _no, _title, _slug in views_extra.PAGES:
                            fn = views_extra.RENDER.get(_slug)
                            if fn:
                                write(os.path.join(a.teletext_dir,
                                                   "fleet-%s.html" % _slug),
                                      fn(d.snap))
                except Exception as exc:
                    sys.stderr.write("[dash] term/ceefax render failed: %s\n" % exc)
                    sys.stderr.flush()
            if a.html:
                try:
                    write(a.html, d.emit_html())
                except Exception as exc:
                    import traceback
                    sys.stderr.write("[dash] render failed: %s\n" % exc)
                    traceback.print_exc(file=sys.stderr)
                    sys.stderr.flush()
            if a.once or not a.serve:
                return
            time.sleep(a.interval)

    try:
        while True:
            frame = d.render()
            sys.stdout.write("\033[H\033[J" + frame + "\n")
            sys.stdout.flush()
            if a.once:
                return
            time.sleep(a.interval)
    except KeyboardInterrupt:
        sys.stdout.write("\033[0m\n")


if __name__ == "__main__":
    main()
