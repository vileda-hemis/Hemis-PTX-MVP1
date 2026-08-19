#!/usr/bin/env python3
"""§12 decomposition — harvest fan-out WALL-HITS from the caller logs and say
exactly which members failed to deliver, so a repeat failure can be attributed.

The caller log carries only four FanOutSign shapes (verified by enumeration):
  gmXX HTTP 500 RETRY(not-seen) body={... round_seed <hex> ...}
  gmXX got partial sig (n/6)
  threshold n reached — returning (...)            <- round terminator, success
  wall-clock ceiling 30000ms hit after n tick(s)
      (a collected, b pending, c inflight) — returning   <- terminator, failure

So a round = the lines up to a terminator, and a member that appears only with
RETRY(not-seen) and never "got partial sig" is a NON-DELIVERER for that round.

Per the §12 question: `pending` vs `inflight` at the wall separates
  pending  -- no response outstanding (transport / unreachable side)
  inflight -- a request was outstanding when the wall fired (timing side)
and the non-deliverer identities answer the one that matters most: is it the
SAME members every time (specific nodes) or a rotating set (systemic)?

Dedupes on (caller, timestamp) so re-running never double-counts.
"""
import re, sys, json, os, glob, collections

W2 = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet"
OUT = f"{W2}/meshrep_walls.jsonl"
DD = "/mnt/ptx-ssd-work/w2r-fleet/datadirs"

RE_LINE  = re.compile(r"^(\S+Z) PTX: FanOutSign: (.*)$")
RE_NOSEE = re.compile(r"^(gm\d+):\w+ HTTP \d+ RETRY\(not-seen\)")
RE_PART  = re.compile(r"^(gm\d+):\w+ got partial sig")
RE_SEED  = re.compile(r"round_seed ([0-9a-f]{64})")
RE_WALL  = re.compile(r"wall-clock ceiling (\d+)ms hit after (\d+) tick\(s\) "
                      r"\((\d+) collected, (\d+) pending, (\d+) inflight\)")
RE_THR   = re.compile(r"threshold \d+ reached")

def scan(caller, path):
    """Yield one dict per wall-hit round in this caller's log."""
    seen, part, seed, out = set(), set(), None, []
    try:
        fh = open(path, errors="replace")
    except OSError:
        return out
    with fh:
        for raw in fh:
            m = RE_LINE.match(raw.replace("\0", ""))
            if not m:
                continue
            ts, rest = m.group(1), m.group(2)
            g = RE_NOSEE.match(rest)
            if g:
                seen.add(g.group(1))
                if seed is None:
                    s = RE_SEED.search(rest)
                    if s: seed = s.group(1)
                continue
            g = RE_PART.match(rest)
            if g:
                part.add(g.group(1)); seen.add(g.group(1)); continue
            w = RE_WALL.search(rest)
            if w:
                out.append({
                    "caller": caller, "ts": ts, "round_seed": seed,
                    "wall_ms": int(w.group(1)), "ticks": int(w.group(2)),
                    "collected": int(w.group(3)), "pending": int(w.group(4)),
                    "inflight": int(w.group(5)),
                    "nondelivering": sorted(seen - part),
                    "delivered": sorted(part),
                })
                seen, part, seed = set(), set(), None
                continue
            if RE_THR.search(rest):
                seen, part, seed = set(), set(), None

    return out

def main():
    tag = sys.argv[1] if len(sys.argv) > 1 else "adhoc"
    known = set()
    if os.path.exists(OUT):
        for l in open(OUT, errors="replace"):
            try:
                r = json.loads(l)
                known.add((r["caller"], r["ts"]))
            except Exception:
                pass
    fresh = []
    for p in sorted(glob.glob(f"{DD}/caller*/ptxbea/debug.log")):
        caller = p.split("/")[-3]
        for rec in scan(caller, p):
            if (rec["caller"], rec["ts"]) in known:
                continue
            rec["tag"] = tag
            fresh.append(rec)
    with open(OUT, "a") as f:
        for r in fresh:
            f.write(json.dumps(r) + "\n")
    print(f"[{tag}] wall-hits harvested: {len(fresh)} new")
    for r in fresh:
        print(f"    {r['caller']} {r['ts']} collected={r['collected']} "
              f"pending={r['pending']} inflight={r['inflight']} "
              f"nondelivering={','.join(r['nondelivering']) or '-'}")
    if fresh:
        c = collections.Counter(m for r in fresh for m in r["nondelivering"])
        print(f"    non-deliverer tally this rung: {dict(c)}")

main()
