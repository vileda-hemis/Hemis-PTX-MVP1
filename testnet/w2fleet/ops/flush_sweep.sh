#!/bin/bash
# Fleet-local chainstate flush sweep (2026-08-16, crash-tolerant posture).
# DATABASE_FLUSH_INTERVAL is 24h compile-time; on a host that resets every few
# hours that meant a ~600-block replay per crash (the BUG-037 fault surface).
# gettxoutsetinfo calls FlushStateToDisk() = FLUSH_STATE_ALWAYS, so a periodic
# sweep caps the replay window at the sweep cadence with zero code change.
# UTXO-set scan cost is trivial at dev-chain scale; nodes are flushed serially.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
LOG=/mnt/pve/Node14TB/hemis-ptx/FLUSH_SWEEP.log
LOCK=/run/ptx-flush-sweep.lock
CLI=(Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)

exec 9>"$LOCK"
flock -n 9 || exit 0   # previous sweep still running; skip this tick

ok=0; skip=0; fail=0
for c in $(seq -f 'ptx-w2r-caller%.0f' 1 8) $(seq -f 'ptx-w2r-gm%02.0f' 1 153); do
    state=$(docker inspect -f '{{.State.Running}}' "$c" 2>/dev/null)
    if [ "$state" != "true" ]; then skip=$((skip+1)); continue; fi
    if docker exec "$c" "${CLI[@]}" gettxoutsetinfo >/dev/null 2>&1; then
        ok=$((ok+1))
    else
        fail=$((fail+1))
    fi
done
echo "$(date '+%F %T') flushed=$ok skipped=$skip failed=$fail" >> "$LOG"
