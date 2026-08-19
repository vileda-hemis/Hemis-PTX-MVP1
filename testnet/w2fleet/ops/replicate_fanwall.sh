#!/bin/bash
# §12 replication — thin d100 + d200, 3 rounds each = 144 rolls on the Gen D
# binary (FANOUT_MAX_ATTEMPTS derived, 30s wall). Gen D was n=1 on the surviving
# failure; this is the sample that decides whether that failure is a class.
# Every rung's wall-hits are harvested straight after the rung, while the caller
# logs still hold the round (see meshrep_harvest.py for the decomposition).
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/replicate_fanwall.log
DONE=$W2/replicate_fanwall.done
LOCK=$W2/ladder_ckpt.lock
RF=$W2/latency_ladder_results.jsonl
ROUNDS=${1:-3}
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

converge_pings() {
    local d=$1 floor t0 p50; floor=$(( (2*d*9)/10 )); t0=$(date +%s)
    while :; do
        p50=$("$W2/netem_mesh.sh" verify 2>/dev/null | awk -F'p50=' '/caller1/{split($2,a," ");print int(a[1]); exit}')
        [ -n "$p50" ] && [ "$p50" -ge "$floor" ] && { say "convergence: caller1 p50=${p50}ms >= ${floor}ms"; return 0; }
        [ $(( $(date +%s) - t0 )) -ge 240 ] && { say "convergence TIMEOUT (p50=${p50:-?}) — proceeding"; return 0; }
        sleep 15
    done
}

exec 9>"$LOCK"
flock -n 9 || { say "SKIP: ladder lock held"; exit 0; }
touch "$DONE"
donefile() { tr -d '\0' < "$DONE"; }

say "=== REPLICATION START: $ROUNDS rounds x {d100,d200} x 24 rolls (binary f0e5458f) ==="
for r in $(seq 1 "$ROUNDS"); do
  for D in 100 200; do
    TAG="rep${r}-d${D}"
    if donefile | grep -qx "$TAG"; then say "rung $TAG: checkpointed — skip"; continue; fi
    say "--- rung $TAG (one-way ${D}ms) ---"
    "$W2/netem_mesh.sh" apply "$D" >>"$LOG" 2>&1
    converge_pings "$D"
    "$W2/latency_battery.py" "$TAG" 3 2>&1 | tee -a "$LOG" | tail -1
    # harvest wall-hits for THIS rung while the caller logs still hold the rounds
    "$W2/meshrep_harvest.py" "$TAG" 2>&1 | tee -a "$LOG" | tail -3
    echo "$TAG" >> "$DONE"
    say "rung $TAG: done"
  done
done
say "--- clearing netem ---"; "$W2/netem_mesh.sh" clear >>"$LOG" 2>&1
echo "ALL-DONE $(date '+%F %T')" >> "$DONE"
say "=== REPLICATION COMPLETE ==="
