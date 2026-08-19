#!/bin/bash
# MULTI-QUORUM to_first experiment (2026-08-19, rev 2).
#
# §12 recorded the cost of characterising a class from ONE quorum. This samples
# several quorums at the SAME rung (d200) to test whether to_first is
# quorum-dependent.
#
# rev 2 fixes two defects found on the first window:
#  1. The checkpoint keyed on ptx_quorum_list's newest ACTIVE quorum, but the
#     probe's own discovery roll can select a DIFFERENT one -- which is exactly
#     how a stale member list arises. Now it keys on the quorum the probe
#     ACTUALLY used, read back from the banked records.
#  2. A window is only checkpointed once it has MIN_VALID rolls with
#     quorum_matched=True. Otherwise a window that rotated mid-run banks as
#     "done" while carrying no usable propagation data at all.
#
# CHECKPOINTED because the host holds 16min-3h and this spans ~2h.
# Netem is applied ONLY during measurement and cleared between windows.
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/mq_tofirst.log
CKPT=$W2/mq_tofirst.ckpt
TARGET=${1:-5}
ROLLS=${2:-8}
MIN_VALID=${3:-4}
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
touch "$CKPT"
grep -q '^ALL-DONE' "$CKPT" && { say "SKIP: ALL-DONE"; exit 0; }

exec 9>"$W2/ladder_ckpt.lock"
flock -n 9 || { say "SKIP: ladder lock held"; exit 0; }

# NOT `grep -c ... || echo 0`: grep -c PRINTS 0 and EXITS 1 on no match, so the
# fallback fires too and the function returns "0\n0" -- which fails every numeric
# test silently and made rev 1 write ALL-DONE without measuring anything.
done_count() { awk 'BEGIN{n=0} /^q /{n++} END{print n}' "$CKPT"; }

# Read back what the probe actually signed with, and how many rolls were valid.
summarise_tag() {
    python3 - "$1" <<'PYEOF'
import json,sys,collections
tag=sys.argv[1]; q=collections.Counter(); valid=0; tf=[]
for l in open('/mnt/pve/Node14TB/hemis-ptx/w2-fleet/meshhop_results.jsonl'):
    try: d=json.loads(l)
    except Exception: continue
    if d.get('tag')!=tag: continue
    if d.get('quorum_matched') and d.get('to_first_s') is not None:
        valid+=1; tf.append(d['to_first_s']); q[d.get('quorum_hash','')[:16]]+=1
print(f"{q.most_common(1)[0][0] if q else 'NONE'} {valid} {min(tf) if tf else 0}")
PYEOF
}

say "=== MULTI-QUORUM to_first START (target $TARGET, $ROLLS rolls, need $MIN_VALID valid) ==="
say "already done: $(done_count)/$TARGET"

iter=0
while [ "$(done_count)" -lt "$TARGET" ]; do
    iter=$((iter+1))
    TAG="mqx-$(date +%H%M%S)"
    say "--- window $iter (tag $TAG): applying d200 ---"
    "$W2/netem_mesh.sh" apply 200 >>"$LOG" 2>&1
    t0=$(date +%s)
    while :; do
        p50=$("$W2/netem_mesh.sh" verify 2>/dev/null | awk -F'p50=' '/caller1/{split($2,a," ");print int(a[1]); exit}')
        [ -n "$p50" ] && [ "$p50" -ge 360 ] && { say "convergence p50=${p50}ms"; break; }
        [ $(( $(date +%s) - t0 )) -ge 240 ] && { say "convergence TIMEOUT p50=${p50:-?}"; break; }
        sleep 15
    done
    "$W2/meshhop_probe.py" caller1 "$TAG" "$ROLLS" 2>&1 | tee -a "$LOG"
    "$W2/netem_mesh.sh" clear >>"$LOG" 2>&1

    read -r AQ NVALID MINTF <<< "$(summarise_tag "$TAG")"
    say "window $iter: actual quorum=$AQ valid=$NVALID/$ROLLS min_to_first=$MINTF"

    if [ "$AQ" = "NONE" ] || [ "$NVALID" -lt "$MIN_VALID" ]; then
        say "window $iter REJECTED (only $NVALID valid) — not checkpointing, retrying after rotation"
        sleep 180; continue
    fi
    if grep -q "^q $AQ\$" "$CKPT"; then
        say "quorum $AQ already banked — waiting for rotation (30 blocks ~23min)"
        sleep 180; continue
    fi
    say "harvesting in-band/gossip split for $AQ"
    "$W2/mq_attach_harvest.py" "$TAG" 2>&1 | tee -a "$LOG"
    echo "q $AQ" >> "$CKPT"
    say "CHECKPOINT $AQ  ($(done_count)/$TARGET)"
done
echo "ALL-DONE $(date '+%F %T')" >> "$CKPT"
say "=== MULTI-QUORUM to_first COMPLETE ($(done_count) quorums) ==="
