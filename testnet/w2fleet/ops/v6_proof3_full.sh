#!/bin/bash
# ODC-066 proof 3 (self-closing, user-authorized fallback): gm50 does a DKG over
# v6 and its quorum signs a roll. If the ramp reaches 8/8 with gm50 still
# unselected, PAUSE demand -> idle-retirement (KDD-074) reforms quorums,
# re-selecting fresh from the pool (verified: not same-set) -> gm50 gets fresh
# chances -> then RESUME demand for the signed roll.
cd /mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
D=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs
J=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/demand-N98.jsonl
CALLERS=127.0.0.1:31000,127.0.0.1:31099,127.0.0.1:31100,127.0.0.1:31101,127.0.0.1:31102,127.0.0.1:31103,127.0.0.1:31104,127.0.0.1:31105
echo "[proof3] waiter LIVE $(date -u +%T) — watching gm50 DKG-over-v6 (fallback: pause+reform at 8/8)"

nactive() {
  python3 -c "from harness.cluster import W2Cluster;gm=W2Cluster(98).gms[0];ql=gm.call('ptx_quorum_list').get('quorums',[]);print(sum(1 for q in ql if q.get('state')=='active'))" 2>/dev/null || echo 0
}

paused=0
QH=""
for _ in $(seq 1 720); do   # up to ~12h
  L=$(grep -h "ceremony DONE" "$D"/gm50/ptxbea/debug.log 2>/dev/null | tail -1)
  if [ -n "$L" ]; then
    QH=$(echo "$L" | grep -o 'quorum_hash=[0-9a-f]*' | head -1 | cut -d= -f2)
    echo "[proof3] ceremony DONE on gm50: ${L:0:110}"
    break
  fi
  A=$(nactive)
  if [ "$paused" = 0 ] && [ "$A" -ge 8 ] 2>/dev/null; then
    echo "[proof3] ramp reached 8/8 with gm50 unselected — PAUSING demand to force idle-retirement reforms (user-authorized)"
    for p in $(ps -eo pid,args | awk '/[d]emand_driver.py/{print $1}'); do kill "$p" 2>/dev/null; done
    paused=1
  fi
  sleep 60
done

if [ -z "$QH" ]; then echo "[proof3] gm50 never selected within window"; exit 1; fi

if [ "$paused" = 1 ]; then
  echo "[proof3] gm50 selected via reform — RESUMING demand for the signed roll"
  setsid nohup python3 -u demand_driver.py --callers "$CALLERS" --jsonl "$J" \
    > /mnt/pve/Node14TB/hemis-ptx/w2-fleet/logs/demand_driver.out 2>&1 </dev/null &
fi

echo "[proof3] waiting for a signed roll routed to gm50's quorum ${QH:0:16}…"
for _ in $(seq 1 240); do
  HIT=$(python3 - "$QH" "$J" <<'PY'
import json, sys
qh, jf = sys.argv[1], sys.argv[2]
try:
    r = [json.loads(l) for l in open(jf)]
except Exception:
    r = []
m = [x for x in r if x.get('ok') and (x.get('quorum_hash') or '').startswith(qh[:16])]
if m:
    x = m[-1]
    print(f"OK h{x.get('h')} seq{x.get('seq')} quorum {qh[:16]} result {x.get('results')}")
PY
)
  if [ -n "$HIT" ]; then
    echo "[proof3] PTX-OVER-v6 PROVEN — SIGNED ROLL: $HIT"
    exit 0
  fi
  sleep 30
done
echo "[proof3] gm50 in a quorum (DKG-over-v6 done) but no roll routed within window"
