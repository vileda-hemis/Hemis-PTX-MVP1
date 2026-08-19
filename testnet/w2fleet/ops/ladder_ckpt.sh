#!/bin/bash
# Checkpointed ladder driver (2026-08-17, crash-resume arc).
# Covers BOTH rung sets: latency (latency_battery.py, 3 samples/shape) and
# concurrent-load (load_battery.py, 20 rolls/block x 10 blocks, operator spec).
# The host hard-resets on a 20min-2.5h cadence (silent data-fabric class, box
# demoted local-dev), so a monolithic run keeps getting invalidated mid-flight.
#
# Checkpoint = one line per completed rung in ladder_ckpt.done, appended ONLY
# after the battery exits 0 AND its record is verified banked in the results
# jsonl — a crash costs at most the in-flight rung. "ALL-DONE" sentinel marks
# a finished run. crash_bringup_boot.sh auto-resumes an unfinished run after
# a successful staged bringup.
#
# Usage: ladder_ckpt.sh [resume|fresh]   (resume is the default)
#   resume: skip rungs already in ladder_ckpt.done
#   fresh:  truncate the done-file and run every rung
#
# Vacuity guard (the load-d25 0/200 "caller_salt" lesson): a rung whose record
# banks with ok==0 is only accepted when the dominant failure is a protocol
# outcome (BLS/threshold/timeout); a harness-shaped failure aborts the run
# WITHOUT checkpointing so a broken battery can't burn through the rung table.
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/ladder_ckpt_run.log
DONE=$W2/ladder_ckpt.done
LOCK=$W2/ladder_ckpt.lock
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

# Ping-convergence gate (replaces the fixed 30s settle): the two thin-rung
# misses of the 2026-08-17 run both sat in the FIRST battery round after netem
# apply — post-apply transients, not propagation misses (FANOUT_BUDGET_ANALYSIS
# §2 note 1). Gate the battery start on caller1's measured P2P p50 reaching the
# injected RTT floor (0.9 x 2 x one-way) instead of hoping 30s was enough.
# Bitcoin pings re-measure on a ~2min cycle, hence the 240s timeout; on timeout
# we proceed loudly rather than abort (the battery itself is the arbiter).
converge_pings() {
    local delay_ms=$1 rtt_floor t0 now p50
    rtt_floor=$(( (2*delay_ms*9)/10 ))
    t0=$(date +%s)
    while :; do
        p50=$("$W2/netem_mesh.sh" verify 2>/dev/null | awk -F'p50=' '/caller1/{split($2,a," ");print int(a[1]); exit}')
        if [ -n "$p50" ] && [ "$p50" -ge "$rtt_floor" ]; then
            say "ping convergence: caller1 p50=${p50}ms >= floor ${rtt_floor}ms"
            return 0
        fi
        now=$(date +%s)
        if [ $((now-t0)) -ge 240 ]; then
            say "ping convergence TIMEOUT (caller1 p50=${p50:-?}ms < floor ${rtt_floor}ms after 240s) — proceeding"
            return 0
        fi
        sleep 15
    done
}

exec 9>"$LOCK"
if ! flock -n 9; then
    say "SKIP: another ladder_ckpt.sh holds $LOCK"
    exit 0
fi

MODE="${1:-resume}"
if [ "$MODE" = fresh ]; then
    : > "$DONE"
fi
touch "$DONE"

# NUL-tolerant done-file reads (2026-08-18): a crash mid-append leaves a NUL
# hole glued to the next entry (proven on both the results jsonl and this
# done-file), which silently defeats exact-line greps.
donefile() { tr -d '\0' < "$DONE"; }

if donefile | grep -q '^ALL-DONE'; then
    say "SKIP: done-file carries ALL-DONE — nothing to resume (use 'fresh' to re-run)"
    exit 0
fi

# tag | battery | one-way delay ms (0 = clean, netem cleared)
RUNGS='clean lat 0
d25 lat 25
d50 lat 50
d100 lat 100
d200 lat 200
load-clean load 0
load-d25 load 25
load-d50 load 50
load-d100 load 100
load-d200 load 200'

results_file() { [ "$1" = lat ] && echo "$W2/latency_ladder_results.jsonl" || echo "$W2/load_ladder_results.jsonl"; }

# Verify the battery banked a fresh record for $tag; enforce the vacuity guard.
# Prints nothing; returns 0 = accept, 1 = banked-but-vacuous (abort), 2 = not banked.
check_banked() {  # tag results_file lines_before
    python3 - "$1" "$2" "$3" <<'PYEOF'
import json, sys
tag, path, before = sys.argv[1], sys.argv[2], int(sys.argv[3])
# 2026-08-18 hardening (the load-d25 "ok=0" phantom): a host crash mid-append
# leaves a NUL hole at the file tail WITHOUT a newline, so the next battery
# glues its clean record onto the same line — json.loads(lines[-1]) then dies
# on the leading NULs and the traceback's exit status 1 masqueraded as the
# vacuity verdict (guard claimed "banked ok=0" while the jsonl held 197/200).
# Parse defensively: strip NULs, salvage the last {"tag" object on a line,
# scan the WHOLE fresh region (not just the final line), and reserve exit 1
# for a genuinely-parsed ok==0 harness-shaped record.
try:
    lines = open(path, errors="replace").read().splitlines()
except OSError:
    sys.exit(2)
rec = None
for ln in lines[min(before, len(lines)):]:
    s = ln.replace("\0", "").replace("�", "").strip()
    if not s:
        continue
    for cand in (s, s[s.rfind('{"tag"'):] if '{"tag"' in s else None):
        if cand is None:
            continue
        try:
            r = json.loads(cand)
        except ValueError:
            continue
        if isinstance(r, dict) and r.get("tag") == tag:
            rec = r
        break   # first successful parse settles this line
if rec is None:
    sys.exit(2)
if rec.get("ok", 0) == 0:
    reasons = " ".join(rec.get("fail_reasons", {}).keys()).lower()
    if not any(w in reasons for w in ("bls", "threshold", "timeout")):
        sys.exit(1)   # harness-shaped total failure: do not checkpoint
sys.exit(0)
PYEOF
}

say "=== LADDER CKPT START mode=$MODE (tip $(docker exec ptx-w2r-caller1 Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026 getblockcount 2>/dev/null)) ==="

while read -r TAG BATTERY DELAY; do
    [ -z "$TAG" ] && continue
    if donefile | grep -qx "$TAG"; then
        say "rung $TAG: checkpointed — skip"
        continue
    fi
    RF=$(results_file "$BATTERY")
    BEFORE=$(wc -l < "$RF" 2>/dev/null || echo 0)
    if [ "$DELAY" -eq 0 ]; then
        say "--- rung $TAG ($BATTERY battery, no netem) ---"
        "$W2/netem_mesh.sh" clear >/dev/null 2>&1
    else
        say "--- rung $TAG ($BATTERY battery, one-way ${DELAY}ms, pair RTT $((2*DELAY))ms) ---"
        "$W2/netem_mesh.sh" apply "$DELAY" | tee -a "$LOG"
        converge_pings "$DELAY"
        "$W2/netem_mesh.sh" verify 2>&1 | tee -a "$LOG"
    fi
    if [ "$BATTERY" = lat ]; then
        "$W2/latency_battery.py" "$TAG" 3 2>&1 | tee -a "$LOG" | tail -1
    else
        "$W2/load_battery.py" "$TAG" 20 10 2>&1 | tee -a "$LOG" | tail -4
    fi
    check_banked "$TAG" "$RF" "$BEFORE"; RC=$?
    if [ "$RC" -eq 0 ]; then
        echo "$TAG" >> "$DONE"
        say "rung $TAG: banked + checkpointed"
    elif [ "$RC" -eq 1 ]; then
        say "ABORT: rung $TAG banked ok=0 with harness-shaped failures (vacuity guard) — NOT checkpointed, fix the battery before resuming"
        "$W2/netem_mesh.sh" clear >/dev/null 2>&1
        exit 1
    else
        say "ABORT: rung $TAG produced no banked record (battery died?) — NOT checkpointed"
        "$W2/netem_mesh.sh" clear >/dev/null 2>&1
        exit 1
    fi
done <<< "$RUNGS"

say "--- clearing netem ---"
"$W2/netem_mesh.sh" clear | tee -a "$LOG"
echo "ALL-DONE $(date '+%F %T')" >> "$DONE"
say "=== LADDER CKPT COMPLETE (all rungs banked) ==="
