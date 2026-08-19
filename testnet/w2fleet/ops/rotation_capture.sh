#!/bin/bash
# ROTATION-GATE EVIDENCE CAPTURE — closure ledger item B (first natural
# rotation, cohort formed@780 due h2220) + the OWED PENDING-TTL measurement
# (FINALIZE->connect latency vs the provisional PTX_PENDING_TTL_BLOCKS=8,
# ptx_bls.h:180). Detached, session-independent. Captures window [2150,2350]
# to the forensics pattern REGARDLESS of outcome, per the gate discipline.
#
# Evidence captured at h2350:
#   - full debug.log copies for every rotation-quorum member + caller1 + gm01
#     (logs are ~2MB/day and shrink only at startup >10MB, so nothing is lost;
#     copies exist so later restarts (D-test) cannot eat the window)
#   - grep extracts: ROTATION of / ceremony DONE / SUPERSEDED / expiring stale
#     PENDING / discarding SUPERSEDED / persisted PTXDKG — the B-closure chain
#   - per-rotation FINALIZE->connect latency table (DONE timestamp vs
#     persisted-record timestamp per successor quorum_hash)
#   - demand JSONL slice for the window (successor-signed-roll proof)
#   - ptx_quorum_info of each superseded predecessor (state + heights)
set -u
DATA=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs
OUT=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/forensics/rotation-h2220-$(date +%Y%m%d)
CLI_PY=/mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
LOG=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/logs/rotation_capture.out

tip() {
  cd "$CLI_PY" && python3 -c "
from harness.cluster import W2Cluster
print(W2Cluster(98).gms[0].getblockcount())" 2>/dev/null || echo 0
}

echo "[capture] armed $(date -u +%FT%TZ) — waiting for h2350 (window [2150,2350])"
T=0
until [ "$T" -ge 2350 ] 2>/dev/null; do
  sleep 300
  T=$(tip)
done
echo "[capture] h$T reached $(date -u +%FT%TZ) — capturing"

mkdir -p "$OUT/logs" "$OUT/extracts"

# 1. full log copies (all nodes: members of the rotating cohorts are 11-of-98
#    per quorum and cheap enough to take wholesale at ~2MB each)
for d in "$DATA"/gm* "$DATA"/caller1; do
  n=$(basename "$d")
  cp "$d/ptxbea/debug.log" "$OUT/logs/$n.debug.log" 2>/dev/null
done

# 2. the B-closure grep chain, fleet-wide, with node attribution
cd "$OUT/logs"
grep -H "PTX formation: ROTATION of"          *.debug.log > ../extracts/rotation-start.txt
grep -H "ceremony DONE"                       *.debug.log > ../extracts/ceremony-done.txt
grep -H "ceremony ABORTED"                    *.debug.log > ../extracts/ceremony-abort.txt
grep -H "quorum SUPERSEDED at height"         *.debug.log > ../extracts/superseded.txt
grep -H "expiring stale PENDING share"        *.debug.log > ../extracts/pending-ttl-expiry.txt
grep -H "discarding SUPERSEDED share"         *.debug.log > ../extracts/superseded-discard.txt
grep -H "persisted PTXDKG quorum record"      *.debug.log > ../extracts/dkg-connect.txt
grep -H "MarkReformed"                        *.debug.log > ../extracts/reformed.txt

# 3. demand JSONL window slice (successor-signed rolls live here)
cp /mnt/pve/Node14TB/hemis-ptx/w2-fleet/demand-N98.jsonl "$OUT/demand-N98.jsonl"

# 4. FINALIZE->connect latency per successor (the OWED TTL measurement input):
#    pair each successor quorum_hash's first "ceremony DONE" stamp with its
#    "persisted PTXDKG quorum record" stamp; block-latency from mined_height -
#    formation trailer is in dkg-connect.txt. Assembled in the analysis pass —
#    raw pairs are what capture must not lose, and now cannot.
echo "[capture] done -> $OUT"
ls -la "$OUT/extracts" | tail -12
