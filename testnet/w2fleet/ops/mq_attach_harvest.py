#!/usr/bin/env python3
"""Per-roll in-band vs gossip split for attach-delivered commitments.

The hypothesis under test is NOT "the caller dials six and stops" -- ptx_fanout.cpp
dials ALL members concurrently and returns at the threshold-th FASTEST partial,
abandoning stragglers' dials at teardown. So the real split is
DELIVERED-BEFORE-TEARDOWN vs ABANDONED-BEFORE-DELIVERY, and a member can have
accepted the commitment in-band even though its partial was never collected.

A member that logs `PTX attach: accepted commitment <txid>` learned it FROM THE
CALLER. A member holding the tx without that line learned it FROM A PEER.
"""
import json, subprocess, sys, collections

W2 = "/mnt/pve/Node14TB/hemis-ptx/w2-fleet"
LOG = "/root/.hemis-ptxbea/ptxbea/debug.log"

def inband(container, txid):
    """True iff this member logged an attach-accept for exactly this txid."""
    r = subprocess.run(["docker", "exec", container, "sh", "-c",
                        f"grep -a -c 'PTX attach: accepted commitment {txid}' {LOG} || true"],
                       capture_output=True, text=True, timeout=60)
    try:    return int(r.stdout.strip() or 0) > 0
    except Exception: return False

def main():
    tag_prefix = sys.argv[1] if len(sys.argv) > 1 else "mq-"
    out = open(f"{W2}/mq_attach_evidence.jsonl", "a")
    seen = set()
    try:
        for l in open(f"{W2}/mq_attach_evidence.jsonl"):
            try: seen.add(json.loads(l)["commit_txid"])
            except Exception: pass
    except FileNotFoundError:
        pass

    for l in open(f"{W2}/meshhop_results.jsonl"):
        try: d = json.loads(l)
        except Exception: continue
        if not str(d.get("tag", "")).startswith(tag_prefix) or not d.get("ok"):
            continue
        # Stale-set rolls carry no usable per-member data (the probe now nulls
        # them). Counting them produced the phantom "0/9 in-band" rows.
        if not d.get("quorum_matched") or d.get("per_member_s") is None:
            continue
        txid = d.get("commit_txid")
        if not txid or txid in seen:
            continue
        members = sorted(d.get("per_member_s", {}).keys())
        ib = [m for m in members if inband(f"ptx-w2r-{m}", txid)]
        rec = {
            "commit_txid": txid, "tag": d["tag"], "i": d.get("i"),
            "quorum_hash": d.get("quorum_hash", "")[:16],
            "members": members,
            "inband": sorted(ib),
            "gossip_only": sorted(set(members) - set(ib)),
            "n_inband": len(ib), "n_members": len(members),
            "to_first_s": d.get("to_first_s"), "to_sixth_s": d.get("to_sixth_s"),
            "to_last_s": d.get("to_last_s"), "roll_total_s": d.get("roll_total_s"),
            "members_accepted": d.get("members_accepted"),
        }
        out.write(json.dumps(rec) + "\n"); out.flush()
        print(f"  {d['tag']} i={rec['i']} in-band={rec['n_inband']}/{rec['n_members']} "
              f"to_first={rec['to_first_s']}")
    out.close()

main()
