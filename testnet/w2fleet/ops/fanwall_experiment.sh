#!/bin/bash
# §9.5 experiment — the decidable gate for direct-attach.
# Re-runs the THIN d100 + d200 rungs with FANOUT_MAX_ATTEMPTS derived from the
# wall (60 -> 200 ticks, 9.0s -> 30s), so a roll that would have completed
# between 9s and 30s can now do so instead of being budget-clipped.
#
# Reads out as pre-registered in FANOUT_BUDGET_ANALYSIS.md §9.5:
#   failures vanish        -> residual was OUR cutoff; direct-attach CLOSES
#   failures persist ~28s  -> gossip genuinely not delivering; direct-attach PROVEN
# Distinct tags (w200-*) so records never collide with the gen-B d100/d200 rows.
# Structure lifted from ladder_ckpt.sh: same netem apply, same ping-convergence
# gate, same NUL-tolerant banked check, same vacuity guard, same lock.
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/fanwall_experiment.log
DONE=$W2/fanwall_experiment.done
LOCK=$W2/ladder_ckpt.lock          # SAME lock as the ladder: never run both
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

converge_pings() {
    local delay_ms=$1 rtt_floor t0 now p50
    rtt_floor=$(( (2*delay_ms*9)/10 ))
    t0=$(date +%s)
    while :; do
        p50=$("$W2/netem_mesh.sh" verify 2>/dev/null | awk -F'p50=' '/caller1/{split($2,a," ");print int(a[1]); exit}')
        if [ -n "$p50" ] && [ "$p50" -ge "$rtt_floor" ]; then
            say "ping convergence: caller1 p50=${p50}ms >= floor ${rtt_floor}ms"; return 0
        fi
        now=$(date +%s)
        if [ $((now-t0)) -ge 240 ]; then
            say "ping convergence TIMEOUT (caller1 p50=${p50:-?}ms < floor ${rtt_floor}ms after 240s) — proceeding"; return 0
        fi
        sleep 15
    done
}

exec 9>"$LOCK"
if ! flock -n 9; then say "SKIP: ladder lock held"; exit 0; fi
touch "$DONE"
donefile() { tr -d '\0' < "$DONE"; }

RF=$W2/latency_ladder_results.jsonl
RUNGS='w200-d100 100
w200-d200 200'

check_banked() {
    python3 - "$1" "$2" "$3" <<'PYEOF'
import json, sys
tag, path, before = sys.argv[1], sys.argv[2], int(sys.argv[3])
try:
    lines = open(path, errors="replace").read().splitlines()
except OSError:
    sys.exit(2)
rec = None
for ln in lines[min(before, len(lines)):]:
    s = ln.replace("\0", "").replace("�", "").strip()
    if not s: continue
    for cand in (s, s[s.rfind('{"tag"'):] if '{"tag"' in s else None):
        if cand is None: continue
        try: r = json.loads(cand)
        except ValueError: continue
        if isinstance(r, dict) and r.get("tag") == tag: rec = r
        break
if rec is None: sys.exit(2)
if rec.get("ok", 0) == 0:
    reasons = " ".join(rec.get("fail_reasons", {}).keys()).lower()
    if not any(w in reasons for w in ("bls", "threshold", "timeout")):
        sys.exit(1)
sys.exit(0)
PYEOF
}

say "=== FANWALL EXPERIMENT START (binary f0e5458f, callers only, tip $(docker exec ptx-w2r-caller1 Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026 getblockcount 2>/dev/null)) ==="

while read -r TAG DELAY; do
    [ -z "$TAG" ] && continue
    if donefile | grep -qx "$TAG"; then say "rung $TAG: checkpointed — skip"; continue; fi
    BEFORE=$(wc -l < "$RF" 2>/dev/null || echo 0)
    say "--- rung $TAG (lat battery, one-way ${DELAY}ms, pair RTT $((2*DELAY))ms) ---"
    "$W2/netem_mesh.sh" apply "$DELAY" | tee -a "$LOG"
    converge_pings "$DELAY"
    "$W2/netem_mesh.sh" verify 2>&1 | tee -a "$LOG"
    "$W2/latency_battery.py" "$TAG" 3 2>&1 | tee -a "$LOG" | tail -2
    check_banked "$TAG" "$RF" "$BEFORE"; RC=$?
    if [ "$RC" -eq 0 ]; then
        echo "$TAG" >> "$DONE"; say "rung $TAG: banked + checkpointed"
    elif [ "$RC" -eq 1 ]; then
        say "ABORT: rung $TAG banked ok=0, harness-shaped (vacuity guard) — NOT checkpointed"
        "$W2/netem_mesh.sh" clear >/dev/null 2>&1; exit 1
    else
        say "ABORT: rung $TAG produced no banked record (battery died?)"
        "$W2/netem_mesh.sh" clear >/dev/null 2>&1; exit 1
    fi
done <<< "$RUNGS"

say "--- clearing netem ---"
"$W2/netem_mesh.sh" clear | tee -a "$LOG"
echo "ALL-DONE $(date '+%F %T')" >> "$DONE"
say "=== FANWALL EXPERIMENT COMPLETE ==="
