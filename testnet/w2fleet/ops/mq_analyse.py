#!/usr/bin/env python3
"""Answer the three to_first questions across every banked quorum.

VALIDITY IS RE-DERIVED FROM meshhop_results.jsonl, NOT trusted from
mq_attach_evidence.jsonl. The evidence file is append-only and its EARLIEST rows
were written by a harvest that predated the quorum_matched filter, so it contains
stale-set rolls that a naive reader scores as real (they read as "0/9 in-band,
to_first 2.9s" -- a phantom blackout class). KDD-089: derive from the authority.
"""
import json, collections, statistics as st

W2 = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet"

# authority: txid -> quorum_matched
truth = {}
for l in open(f"{W2}/meshhop_results.jsonl"):
    try: d = json.loads(l)
    except Exception: continue
    if d.get("commit_txid"):
        truth[d["commit_txid"]] = bool(d.get("quorum_matched"))

rows, dropped = [], []
for l in open(f"{W2}/mq_attach_evidence.jsonl"):
    try: r = json.loads(l)
    except Exception: continue
    (rows if truth.get(r.get("commit_txid")) else dropped).append(r)

print(f"evidence rows: {len(rows)+len(dropped)}  valid: {len(rows)}  DROPPED as stale-set: {len(dropped)}")
for r in dropped:
    print(f"   dropped {r['tag']} i={r['i']} in-band={r['n_inband']}/{r['n_members']} to_first={r['to_first_s']}")

byq = collections.defaultdict(list)
for r in rows: byq[r["quorum_hash"][:8]].append(r)

print(f"\n=== Q1: in-band vs from-peer, per quorum ({len(byq)} distinct quorums) ===")
tot_ib = tot_m = 0
for q, rs in sorted(byq.items()):
    ib = sum(r["n_inband"] for r in rs); m = sum(r["n_members"] for r in rs)
    tot_ib += ib; tot_m += m
    gossip = sorted({g for r in rs for g in r["gossip_only"]})
    print(f"  {q}  rolls={len(rs):2d}  members={rs[0]['n_members']:2d}  in-band={ib}/{m} "
          f"({100*ib/m:.0f}%)  gossip_only={gossip if gossip else 'NONE'}")
print(f"  TOTAL in-band {tot_ib}/{tot_m} = {100*tot_ib/tot_m:.1f}%")

print("\n=== Q2: does to_first track quorum composition? ===")
meds = {}
for q, rs in sorted(byq.items()):
    tf = sorted(r["to_first_s"] for r in rs if r["to_first_s"] is not None)
    if not tf: continue
    meds[q] = st.median(tf)
    print(f"  {q}  n={len(tf):2d}  members={rs[0]['n_members']:2d}  to_first "
          f"min={min(tf):.3f} med={st.median(tf):.3f} max={max(tf):.3f}")
if len(meds) >= 2:
    lo, hi = min(meds.values()), max(meds.values())
    allv = [r["to_first_s"] for r in rows if r["to_first_s"] is not None]
    print(f"  spread of per-quorum MEDIANS: {lo:.3f}-{hi:.3f}s (range {hi-lo:.3f}s)")
    print(f"  spread WITHIN the pooled sample: {min(allv):.3f}-{max(allv):.3f}s (range {max(allv)-min(allv):.3f}s)")
    print(f"  -> between-quorum range is {100*(hi-lo)/(max(allv)-min(allv)):.0f}% of within-sample range")
