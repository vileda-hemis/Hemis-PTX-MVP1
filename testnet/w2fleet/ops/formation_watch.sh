#!/bin/bash
# Watch the NEXT formation with net+gamemaster logging on. Capture, per selected
# member: engaged (STARTED) vs silent (selected but no membership verdict), and
# for silent members the net/driver log around the boundary — to prove whether
# the driver was busy / had no connections / never evaluated membership.
cd /mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
D=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs
N=30
tipnow() { python3 -c "from harness.cluster import W2Cluster;print(W2Cluster(98).gms[0].getblockcount())" 2>/dev/null || echo 0; }

T=$(tipnow); NEXT=$(( (T/N + 1) * N ))
echo "[fw] tip $T — waiting for next boundary h$NEXT"
until [ "$(tipnow)" -ge "$NEXT" ] 2>/dev/null; do sleep 20; done
echo "[fw] boundary h$NEXT reached $(date -u +%T) — capturing selection"
python3 - "$NEXT" <<'PY'
import sys, time, subprocess
from harness.cluster import W2Cluster
H=int(sys.argv[1]); c=W2Cluster(98); gm=c.gms[0]
anchor=gm.call('getblockhash',H)
sel=gm.call('ptx_debug_selectquorum',anchor)
members=[m['node_id'].split(':')[0] for m in sel['selected']]
qh=sel.get('anchor',anchor)[:16]
print(f"[fw] h{H} anchor {anchor[:16]} selected 11: {members}")
# wait ~30 blocks for the ceremony to resolve
deadline=time.time()+2400
while gm.getblockcount() < H+30 and time.time()<deadline: time.sleep(20)
D="/mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs"
def g(node,pat,net=False):
    f=f"{D}/{node}/ptxbea/debug.log"
    return subprocess.run(["grep","-h",pat,f],capture_output=True,text=True).stdout
engaged=[]; silent=[]
for n in members:
    started = f"quorum_hash={anchor[:20]}" in g(n,"ceremony session STARTED")
    notmem  = g(n,f"not a member at anchor {anchor[:16]}").strip()!=""
    if started: engaged.append(n)
    elif not notmem: silent.append(n)
print(f"[fw] ENGAGED (STARTED): {len(engaged)} {engaged}")
print(f"[fw] SILENT (selected, no verdict): {len(silent)} {silent}")
# for silent members, show net/driver activity around the boundary
for n in silent[:4]:
    print(f"[fw] --- {n} log around h{H} boundary (net+gamemaster) ---")
    out=subprocess.run(["bash","-c",
      f"grep -hE 'formation boundary: height={H}|ceremony|prior ceremony|OpenGamemaster|verified member|socket|connection' {D}/{n}/ptxbea/debug.log | tail -8 | cut -c1-130"],
      capture_output=True,text=True).stdout
    print(out or "  (no ceremony/net lines)")
# outcome
done=int(subprocess.run(["bash","-c",f"grep -hc 'ceremony DONE quorum_hash={anchor[:20]}' {D}/gm*/ptxbea/debug.log | paste -sd+ | bc"],capture_output=True,text=True).stdout or 0)
ab=int(subprocess.run(["bash","-c",f"grep -hc 'ceremony ABORTED quorum_hash={anchor[:20]}' {D}/gm*/ptxbea/debug.log | paste -sd+ | bc"],capture_output=True,text=True).stdout or 0)
print(f"[fw] OUTCOME h{H}: DONE={done} ABORTED={ab} — {'FORMED' if done>=6 else 'FAILED sub-threshold' if ab else 'pending'}")
PY
echo "[fw] done"
