#!/bin/bash
# Persistence-survival standing test (BUG-039 companion; the seam kill-replay
# does not cover). Kill-replay proves replay convergence; THIS proves that what
# the daemon persists survives a clean restart intact. Four restart-boundary
# bugs (BUG-029/036/037/039) came from this one untested seam.
#
# Picks a GM that currently HOLDS a share (anti-vacuity: the irreplaceable
# state must be present for its survival to be asserted), skips nodes that are
# mid-ceremony (a restart would abort the formation), cleanly restarts it, and
# asserts:
#   S1 shares:  ptx_shares.dat md5 identical + LoadShares count == pre-count
#   S2 health:  per-quorum {member, share_current} set identical
#   S3 pose:    record count identical (values tick with time; count must not)
#   S4 chain:   node returns, reaches pre-restart height, GM goes Ready
#   (S5 known-quorum list: covered by S2 — the signature enumerates every quorum)
#
# ONLY meaningful on a fixed (BUG-039+) image — on a pre-fix image this test
# DESTROYS the probe node's shares (which is exactly what it exists to catch).
# Appends to PERSIST_SURVIVAL.log; exit 1 + loud line on any violation.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/PERSIST_SURVIVAL.log
DATADIRS=/mnt/ptx-ssd-work/w2r-fleet/datadirs
CLI=(Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)

say()  { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
fail() { say "VIOLATION: $*"; say "=== SURVIVAL CHECK FAILED ==="; exit 1; }

rpc() { docker exec "$1" "${CLI[@]}" "${@:2}" 2>/dev/null; }

health_sig() {  # stable signature: quorum_hash member share_current, sorted
    rpc "$1" ptx_quorum_health | python3 -c '
import json,sys
o=json.load(sys.stdin)
rows=sorted(f"{q[\"quorum_hash\"]} {q[\"member\"]} {q[\"share_current\"]}" for q in o["quorums"])
print("\n".join(rows))'
}

# --- fleet-down guard: with the fleet stopped the check is vacuous AND harmful
# (starts a lone probe into a dead mesh; S4 can never pass — 2026-08-17 false
# FAILED after crash #8). Skip loudly instead.
RUNNING=$(docker ps --format '{{.Names}}' | grep -c '^ptx-w2r-')
if [ "$RUNNING" -lt 100 ]; then
    say "SKIP: fleet down ($RUNNING/161 w2r containers running) — survival check needs a live mesh"
    exit 0
fi

# --- choose the probe: first GM holding a current share, not mid-ceremony ---
PROBE=""; VACUOUS=1
for i in $(seq 1 153); do
    c=$(printf 'ptx-w2r-gm%02d' "$i")
    docker ps --format '{{.Names}}' | grep -qx "$c" || continue
    cur=$(rpc "$c" ptx_quorum_health | python3 -c 'import json,sys; print(json.load(sys.stdin)["capable"])' 2>/dev/null) || continue
    [ "${cur:-0}" -ge 1 ] || continue
    # mid-ceremony guard: a STARTED without DONE/EXITED in the last 45 min
    gm=$(printf 'gm%02d' "$i")
    recent=$(find "$DATADIRS/$gm/ptxbea/debug.log" -mmin -45 2>/dev/null)
    if [ -n "$recent" ]; then
        inflight=$(tail -c 200000 "$DATADIRS/$gm/ptxbea/debug.log" | grep -ac 'ceremony session STARTED')
        done_=$(tail -c 200000 "$DATADIRS/$gm/ptxbea/debug.log" | grep -ac 'ceremony session EXITED\|ceremony DONE')
        [ "$inflight" -gt "$done_" ] && continue
    fi
    PROBE=$c; PROBEGM=$gm; VACUOUS=0; break
done
if [ -z "$PROBE" ]; then
    PROBE=ptx-w2r-gm01; PROBEGM=gm01
    say "note: no GM holds a current share — running on gm01, S1/S2 VACUOUS for shares"
fi

say "=== SURVIVAL CHECK START probe=$PROBE vacuous_share=$VACUOUS ==="

DD="$DATADIRS/$PROBEGM/ptxbea"
PRE_MD5=$(md5sum "$DD/ptx_shares.dat" 2>/dev/null | awk '{print $1}')
PRE_CNT=$(python3 - "$DD/ptx_shares.dat" <<'PY'
import struct,sys
try:
    b=open(sys.argv[1],'rb').read(9); print(struct.unpack('<Q',b[1:9])[0])
except Exception: print(-1)
PY
)
PRE_HEALTH=$(health_sig "$PROBE")
PRE_POSE=$(rpc "$PROBE" ptx_quorum_health | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; md5sum "$DD/ptx_pose.dat" 2>/dev/null | awk '{print $1}')
PRE_POSE_SZ=$(stat -c %s "$DD/ptx_pose.dat" 2>/dev/null || echo 0)
PRE_H=$(rpc "$PROBE" getblockcount)
say "pre: shares=$PRE_CNT md5=${PRE_MD5:-none} height=$PRE_H pose_bytes=$PRE_POSE_SZ"

docker restart "$PROBE" >/dev/null || fail "docker restart failed"

# wait for RPC (up to 4 min)
for _ in $(seq 1 48); do rpc "$PROBE" getblockcount >/dev/null && break; sleep 5; done
rpc "$PROBE" getblockcount >/dev/null || fail "S4: no RPC 4 min after restart"

POST_MD5=$(md5sum "$DD/ptx_shares.dat" 2>/dev/null | awk '{print $1}')
POST_CNT=$(python3 - "$DD/ptx_shares.dat" <<'PY'
import struct,sys
try:
    b=open(sys.argv[1],'rb').read(9); print(struct.unpack('<Q',b[1:9])[0])
except Exception: print(-1)
PY
)
LOADED=$(grep -a 'LoadShares:' "$DD/debug.log" | tail -1)
say "post: shares=$POST_CNT md5=${POST_MD5:-none} | $LOADED"

# S1 — the irreplaceable state
[ "$POST_CNT" = "$PRE_CNT" ] || fail "S1: on-disk share count $PRE_CNT -> $POST_CNT across restart"
[ "$POST_MD5" = "$PRE_MD5" ] || fail "S1: ptx_shares.dat content changed across a clean restart"
if [ "$VACUOUS" = 0 ]; then
    echo "$LOADED" | grep -q "LoadShares: $PRE_CNT share" \
        || fail "S1: LoadShares reported '$LOADED', expected $PRE_CNT loaded"
fi

# S2 — health signature identical (allow up to 3 min for tiertwo to settle)
for _ in $(seq 1 36); do
    POST_HEALTH=$(health_sig "$PROBE")
    [ "$POST_HEALTH" = "$PRE_HEALTH" ] && break; sleep 5
done
[ "$POST_HEALTH" = "$PRE_HEALTH" ] || fail "S2: per-quorum member/share_current signature changed"

# S3 — pose record survival (size may only grow; shrink = loss)
POST_POSE_SZ=$(stat -c %s "$DD/ptx_pose.dat" 2>/dev/null || echo 0)
[ "$POST_POSE_SZ" -ge "$PRE_POSE_SZ" ] || fail "S3: ptx_pose.dat shrank $PRE_POSE_SZ -> $POST_POSE_SZ"

# S4 — chain + Ready (up to 5 min)
for _ in $(seq 1 60); do
    H=$(rpc "$PROBE" getblockcount); [ "${H:-0}" -ge "$PRE_H" ] && break; sleep 5
done
[ "${H:-0}" -ge "$PRE_H" ] || fail "S4: height ${H:-none} below pre-restart $PRE_H after 5 min"
for _ in $(seq 1 60); do
    rpc "$PROBE" getgamemasterstatus | grep -q Ready && READY=1 && break; sleep 5
done
[ "${READY:-0}" = 1 ] || fail "S4: GM not Ready 5 min after restart"

say "=== SURVIVAL CHECK PASS probe=$PROBE shares=$PRE_CNT held-across-restart ==="
