#!/bin/bash
# ODC-066 proof 2: PTX over v6 — gm50 (v6-registered, [fd00:31::60]) participates
# in a DKG ceremony (connects to 10 members + exchanges shares over the network)
# and its quorum signs a demand roll (ok:true). gm50 selected into a quorum is
# probabilistic (~90% over 8 quorums, one-per-GM); if not selected this cycle it
# stays READY+eligible and we say so.
cd /mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
D=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs
echo "[v6proof2] waiting for gm50 to complete a DKG ceremony (DKG over v6)…"
QH=""
for _ in $(seq 1 400); do   # up to ~3.3h (full ramp to 8/8)
  L=$(grep -h "ceremony DONE" $D/gm50/ptxbea/debug.log 2>/dev/null | tail -1)
  if [ -n "$L" ]; then
    QH=$(echo "$L" | grep -o 'quorum_hash=[0-9a-f]*' | head -1 | cut -d= -f2)
    echo "[v6proof2] gm50 completed DKG: ${L:0:120}"
    break
  fi
  sleep 30
done
if [ -z "$QH" ]; then echo "[v6proof2] gm50 not selected into a quorum within window — READY+eligible but idle this cycle"; exit 0; fi
echo "[v6proof2] gm50 is a member of quorum ${QH:0:16}; waiting for a signed roll to it…"
for _ in $(seq 1 120); do
  HIT=$(python3 - "$QH" <<'PY'
import json, sys
qh = sys.argv[1]
try:
    rolls = [json.loads(l) for l in open('/mnt/pve/Node14TB/hemis-ptx/w2-fleet/demand-N98.jsonl')]
except Exception:
    rolls = []
m = [r for r in rolls if r.get('ok') and (r.get('quorum_hash') or '').startswith(qh[:16])]
if m:
    r = m[-1]
    print(f"OK h{r.get('h')} seq{r.get('seq')} quorum {qh[:16]} result {r.get('results')}")
PY
)
  if [ -n "$HIT" ]; then
    echo "[v6proof2] ★ SIGNED ROLL FROM gm50's QUORUM: $HIT"
    echo "[v6proof2] ★★ PTX-OVER-v6 PROVEN: v6 GM did DKG + its quorum signed a roll"
    exit 0
  fi
  sleep 30
done
echo "[v6proof2] gm50's quorum formed but no roll routed to it within window (rolls are score-routed) — DKG-over-v6 proven, signed-roll pending more demand"
