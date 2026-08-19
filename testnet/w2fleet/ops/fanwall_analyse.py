#!/usr/bin/env python3
"""§9.5 read-out: did raising the attempt budget clear the thin-lane failures,
or do they persist against the 30s wall?

The decisive column is NOT the pass count alone — it is WHERE the failures land.
Budget-clipped failures under the old constant all sat at 9.09s (60 x 150ms).
If failures persist here they must sit at ~30s (the wall); if they vanish, the
old cutoff was manufacturing them."""
import json, sys, statistics

RF = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet/latency_ladder_results.jsonl"

def load():
    out = []
    for ln in open(RF, errors="replace").read().splitlines():
        s = ln.replace("\0", "").strip()
        if not s: continue
        if '{"tag"' in s: s = s[s.rfind('{"tag"'):]
        try: r = json.loads(s)
        except ValueError: continue
        out.append(r)
    return out

recs = load()
def last(tag):
    m = [r for r in recs if r.get("tag") == tag]
    return m[-1] if m else None

print("=" * 74)
print("§9.5 EXPERIMENT READ-OUT — attempt budget 60 (9.0s) -> 200 (30s wall)")
print("=" * 74)
for base, exp in (("d100", "w200-d100"), ("d200", "w200-d200")):
    b, e = last(base), last(exp)
    print("\n--- thin %s ---" % base)
    for label, r in (("baseline (60 att, 9.0s)", b), ("experiment (200 att, 30s)", e)):
        if not r:
            print("  %-26s MISSING" % label); continue
        print("  %-26s %2d/%-2d ok   p50=%-5s p95=%-5s max=%-5s   %s"
              % (label, r.get("ok", 0), r.get("total", 0), r.get("p50_s"),
                 r.get("p95_s"), r.get("max_s"), r.get("when")))
    if not (b and e): continue
    # Where did the failures land?
    for label, r in (("baseline", b), ("experiment", e)):
        fails = [x for x in r.get("runs", []) if not x.get("ok")]
        if fails:
            print("  %s failure latencies: %s" % (label,
                  ", ".join("%.2fs (%s: %s)" % (f.get("latency_s", 0), f.get("shape"),
                            (f.get("err") or "")[:60]) for f in fails)))
        else:
            print("  %s failures: NONE" % label)

# Combined verdict
b100, b200 = last("d100"), last("d200")
e100, e200 = last("w200-d100"), last("w200-d200")
if all((b100, b200, e100, e200)):
    bo = b100["ok"] + b200["ok"]; bt = b100["total"] + b200["total"]
    eo = e100["ok"] + e200["ok"]; et = e100["total"] + e200["total"]
    ef = [x for r in (e100, e200) for x in r.get("runs", []) if not x.get("ok")]
    print("\n" + "=" * 74)
    print("COMBINED thin d100+d200:  baseline %d/%d   ->   experiment %d/%d" % (bo, bt, eo, et))
    if not ef:
        print("VERDICT: failures VANISHED at a 30s budget.")
        print("  => the residual thin-lane tail was OUR OWN cutoff, not lost gossip.")
        print("  => §9.5 close condition MET: direct-attach CLOSES on evidence.")
    else:
        lat = [f.get("latency_s", 0) for f in ef]
        print("VERDICT: %d failure(s) PERSIST. latencies: %s"
              % (len(ef), ", ".join("%.2fs" % x for x in lat)))
        if lat and min(lat) > 20:
            print("  => clipped at the 30s WALL, not the old 9.0s budget.")
            print("  => gossip genuinely not delivering: direct-attach PROVEN necessary.")
        else:
            print("  => failures BELOW 20s = not budget-clipped at all; these are")
            print("     real protocol/BLS outcomes. Read fail_reasons before concluding.")
    print("=" * 74)
