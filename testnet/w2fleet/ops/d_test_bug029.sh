#!/bin/bash
# D — BUG-029 sentry on a real datadir (closure ledger entry D).
# Waits for the NEXT rotation supersede (adfacd1f cohort, ~h2607), restarts one
# successor member within the -checkblocks(6) window of the PTXDKG mine, then
# proves the share survived the startup VerifyDB walk: gm_bls_sign must SIGN.
# Pre-56c938a+BUG-029 this restart left the member UNDONE_RETAINED on disk and
# unsignable (the phase-2 disease's restart arm).
set -u
cd /mnt/pve/Node14TB/hemis-ptx/src/hemisd/testnet/w2fleet
D=/mnt/pve/Node14TB/hemis-ptx/w2-fleet/datadirs
PRED=adfacd1f
echo "[D] armed $(date -u +%FT%TZ) — waiting for MarkSuperseded of ${PRED}*"
SUP=""
until [ -n "$SUP" ]; do
  sleep 10
  SUP=$(grep -h "MarkSuperseded: quorum SUPERSEDED at height .*quorum_hash=${PRED}" \
        $D/gm01/ptxbea/debug.log 2>/dev/null | tail -1)
done
H=$(echo "$SUP" | grep -o "height [0-9]*" | grep -o "[0-9]*")
echo "[D] supersede detected: h$H  $(date -u +%FT%TZ)"
# successor quorum_hash = the rotation ceremony's quorum_hash (anchor hash)
SUCC=$(grep -h "PTX formation: ROTATION of ${PRED}" $D/gm01/ptxbea/debug.log \
       | tail -1 | grep -o "anchor [0-9a-f]*" | awk '{print $2}')
echo "[D] successor quorum_hash: $SUCC"
# pick a member that completed the successor ceremony
MEMBER=$(grep -l "ceremony DONE quorum_hash=${SUCC}" $D/gm*/ptxbea/debug.log \
         | head -1 | sed 's|.*/datadirs/\(gm[0-9]*\)/.*|\1|')
echo "[D] restarting member $MEMBER NOW (within checkblocks window of h$H)"
docker restart ptx-w2-$MEMBER
python3 - "$MEMBER" "$SUCC" <<'EOF'
import sys, time
from harness.cluster import W2Cluster
from harness.node import RPCError
member, succ = sys.argv[1], sys.argv[2]
c = W2Cluster(98)
nd = next(n for n in c.gms if n.name == member)
deadline = time.time() + 300
while time.time() < deadline:
    try:
        nd.getblockcount(); break
    except Exception:
        time.sleep(5)
print(f"[D] {member} RPC back, tip {nd.getblockcount()}")
try:
    r = nd.call("gm_bls_sign", "ab"*32, succ)
    print(f"[D] POST-RESTART SIGN PROBE: SIGNS ({r['sig_hex'][:16]}…) — "
          f"D PASS: share survived the VerifyDB walk (PTXStateSentry held)")
except RPCError as e:
    print(f"[D] POST-RESTART SIGN PROBE: REFUSED — ★ D FAIL, BUG-029-class: {e.message[:100]}")
EOF
# the walk evidence from the restarted member's log
grep -E "Verifying last|PTXStateSentry|VerifyDB" $D/$MEMBER/ptxbea/debug.log | tail -3
echo "[D] done $(date -u +%FT%TZ)"
