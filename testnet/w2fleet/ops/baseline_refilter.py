#!/usr/bin/env python3
"""Retrospective stale-member-set filter for records predating quorum_matched.

FIRST METHOD FAILED ITS POSITIVE CONTROL and is discarded: "did the ACTIVE set
change across the run" called att-d200 clean when quorum_matched already proved 5
of its 8 rolls stale. Active-set stability is not the criterion -- SELECTION is.

THIS method uses direct evidence instead of inference. For each roll the caller's
own debug.log names the members that actually answered the sign fan-out
(`PTX: FanOutSign: <gm>:<id> got partial sig`). Those members are, by
construction, in the ACTUAL signing quorum. If any of them is absent from the set
the probe polled, the polled set was stale -- proven, not inferred.

Direction of proof: finding a logged signer outside the polled set PROVES stale.
Finding none is strong evidence of a match (the answering subset is ~6 of 11), so
a run with zero violations across every roll is reported as consistent-with-clean,
which is the honest strength of the claim.
"""
import json, subprocess, collections, re

CL = "/root/.hemis-ptxbea/ptxbea/debug.log"

def caller_log(container):
    r = subprocess.run(["docker", "exec", container, "sh", "-c", f"cat -v {CL}"],
                       capture_output=True, text=True, timeout=300)
    return r.stdout.splitlines()

SIGN = re.compile(r"FanOutSign: ([A-Za-z0-9]+):[0-9a-f]+ got partial sig")
NOTSEEN = re.compile(r"FanOutSign: ([A-Za-z0-9]+):[0-9a-f]+ .*(not seen|-32051)")

def signers_for(lines, idx_by_txid, txid, window=400):
    i = idx_by_txid.get(txid)
    if i is None: return None
    out = set()
    for l in lines[i:i+window]:
        m = SIGN.search(l) or NOTSEEN.search(l)
        if m: out.add(m.group(1))
        if "threshold" in l and "reached" in l: break
    return out

def main():
    recs = collections.defaultdict(list)
    for l in open("meshhop_results.jsonl"):
        try: d = json.loads(l)
        except Exception: continue
        t = str(d.get("tag", ""))
        if t.startswith(("mh-", "att-", "att2-")) and d.get("per_member_s"):
            recs[t].append(d)

    lines = caller_log("ptx-w2r-caller1")
    idx = {}
    for n, l in enumerate(lines):
        if "broadcast BEFORE signing" in l:
            m = re.search(r"commitment ([0-9a-f]{64})", l)
            if m: idx[m.group(1)] = n

    print(f"{'run':12s} {'rolls':>5s} {'resolved':>8s} {'STALE':>6s} {'clean':>6s}   verdict")
    summary = {}
    for tag in sorted(recs):
        stale = clean = unres = 0
        stale_rolls = []
        for d in recs[tag]:
            polled = set(d["per_member_s"].keys())
            sg = signers_for(lines, idx, d["commit_txid"])
            if not sg: unres += 1; continue
            outside = sg - polled
            if outside:
                stale += 1; stale_rolls.append((d.get("i"), sorted(outside)[:3]))
            else:
                clean += 1
        res = stale + clean
        v = "★ CONTAMINATED" if stale else ("consistent with CLEAN" if res else "unresolved")
        print(f"{tag:12s} {len(recs[tag]):5d} {res:8d} {stale:6d} {clean:6d}   {v}")
        if stale_rolls:
            print(f"             stale rolls i={[r[0] for r in stale_rolls]}  e.g. signer outside polled set: {stale_rolls[0][1]}")
        summary[tag] = (stale, clean, unres)
    return summary

S = main()
print()
print("=== POSITIVE CONTROL ===")
st, cl, _ = S.get("att-d200", (0, 0, 0))
print(f"att-d200 via this method: stale={st} clean={cl}")
print("quorum_matched ground truth: stale=5 clean=3")
print("METHOD VALIDATED" if (st, cl) == (5, 3) else "★ METHOD DISAGREES WITH GROUND TRUTH — do not trust its mh-* verdicts")
