#!/bin/bash
# Concurrent-load latency ladder (2026-08-17, operator spec: 20 rolls/block
# x 10 blocks per RTT setting, concurrent across the 8 callers).
# Same rung set as latency_ladder.sh for comparability; netem cleared at end.
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/load_ladder_run.log
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

say "=== LOAD LADDER START (tip $(docker exec ptx-w2r-caller1 Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026 getblockcount 2>/dev/null)) ==="

say "--- rung load-clean (no netem) ---"
"$W2/netem_mesh.sh" clear >/dev/null 2>&1
"$W2/load_battery.py" load-clean 20 10 2>&1 | tee -a "$LOG" | tail -4

for D in 25 50 100 200; do
    say "--- rung load-d${D} (one-way ${D}ms, pair RTT $((2*D))ms) ---"
    "$W2/netem_mesh.sh" apply "$D" | tee -a "$LOG"
    sleep 30
    "$W2/netem_mesh.sh" verify 2>&1 | tee -a "$LOG"
    "$W2/load_battery.py" "load-d${D}" 20 10 2>&1 | tee -a "$LOG" | tail -4
done

say "--- clearing netem ---"
"$W2/netem_mesh.sh" clear | tee -a "$LOG"
say "=== LOAD LADDER COMPLETE ==="
