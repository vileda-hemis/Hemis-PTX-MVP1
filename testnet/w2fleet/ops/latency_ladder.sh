#!/bin/bash
# Latency ladder driver (fan-out budget arc, 2026-08-16).
# Rung 0 = clean substrate (battery-recreation comparability check vs the
# standup baseline p50 1.2s / p95 1.5s), then one-way delays 25/50/100/200 ms
# (pair RTT 50/100/200/400 ms). Battery per rung; netem cleared at the end
# and between nothing — each apply REPLACES the qdisc.
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/latency_ladder_run.log
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

say "=== LATENCY LADDER START (tip $(docker exec ptx-w2r-caller1 Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026 getblockcount 2>/dev/null)) ==="

say "--- rung clean (no netem) ---"
"$W2/netem_mesh.sh" clear >/dev/null 2>&1
"$W2/latency_battery.py" clean 3 2>&1 | tee -a "$LOG" | tail -1

for D in 25 50 100 200; do
    say "--- rung d${D} (one-way ${D}ms, pair RTT $((2*D))ms) ---"
    "$W2/netem_mesh.sh" apply "$D" | tee -a "$LOG"
    sleep 30   # let P2P pings re-measure
    "$W2/netem_mesh.sh" verify 2>&1 | tee -a "$LOG"
    "$W2/latency_battery.py" "d${D}" 3 2>&1 | tee -a "$LOG" | tail -1
done

say "--- clearing netem ---"
"$W2/netem_mesh.sh" clear | tee -a "$LOG"
say "=== LATENCY LADDER COMPLETE ==="
