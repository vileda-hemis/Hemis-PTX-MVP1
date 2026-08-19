#!/bin/bash
# Mesh-hop sweep across the thin rungs so the propagation term's scaling with RTT
# is visible (clean / d100 / d200). Same netem + convergence gate as the ladder.
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/meshhop_sweep.log
LOCK=$W2/ladder_ckpt.lock
N=${1:-8}
# Tag PREFIX (2026-08-19). The rung tags are the ONLY key the results jsonl has;
# re-running this sweep after a binary change would append post-change rolls under
# the SAME mh-* tags as the pre-change baseline and silently destroy the very
# comparison the sweep exists to make. Default preserves the historic tags.
PREFIX=${2:-mh-}
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

ping_p50() {
    "$W2/netem_mesh.sh" verify 2>/dev/null | awk -F'p50=' '/caller1/{split($2,a," ");print int(a[1]); exit}'
}

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

say "=== MESHHOP SWEEP START (n=$N per rung, tag prefix ${PREFIX}) ==="
while read -r TAG DELAY; do
    [ -z "$TAG" ] && continue
    if [ "$DELAY" -eq 0 ]; then
        say "--- rung $TAG (no netem) ---"; "$W2/netem_mesh.sh" clear >/dev/null 2>&1
        # Record the clean-rung ping too. Only the netem rungs logged theirs, so a
        # later run had no baseline to test "has the EWMA decayed?" against.
        say "clean-rung ping: caller1 p50=$(ping_p50)ms"
    else
        say "--- rung $TAG (one-way ${DELAY}ms, pair RTT $((2*DELAY))ms) ---"
        "$W2/netem_mesh.sh" apply "$DELAY" | tee -a "$LOG"; converge_pings "$DELAY"
    fi
    "$W2/meshhop_probe.py" caller1 "$TAG" "$N" 2>&1 | tee -a "$LOG"
done <<< "${PREFIX}clean 0
${PREFIX}d100 100
${PREFIX}d200 200"

say "--- clearing netem ---"; "$W2/netem_mesh.sh" clear | tee -a "$LOG"
say "=== MESHHOP SWEEP COMPLETE ==="
