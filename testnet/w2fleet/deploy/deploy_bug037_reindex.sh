#!/bin/bash
# BUG-037 fix deploy + one-shot fleet reindex (2026-08-16).
# Each node: drop REINDEX_ONCE marker -> graceful stop -> compose recreate onto
# the fixed image (entrypoint consumes the marker, appends reindex=1 for that
# start only) -> wait for reindex to re-derive the old chain (>= GATE_H) ->
# settle gate.  GMs in batches of 20, callers last (they resume staking on the
# recovered chain).  Reindex re-validates 0..972 with the pose fix: pose is
# reset (BUG-025 hook) and re-derived per block, so h360 validates and the
# pre-crash chain returns as best.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd /mnt/pve/Node14TB/hemis-ptx/docker-w2r
LOG=/mnt/pve/Node14TB/hemis-ptx/DEPLOY_BUG037_2026-08-16.log
COMPOSE="docker compose -f docker-compose.generated.yml -p ptx-w2r"
WANT_MD5=3b0d06f6fa09e9f4d9a339fd3fde8aae
DATADIRS=/mnt/ptx-ssd-work/w2r-fleet/datadirs
GATE_H=960          # old chain tip is 971; reindex must pass this to count
CLI=(Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)

say()  { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
mce_clean() { ras-mc-ctl --summary 2>/dev/null | grep -q '^No MCE errors'; }
abort() { say "ABORT: $*"; exit 1; }
mem_avail() { free -m | awk '/^Mem:/{print $7}'; }
psi_mem()   { awk -F'avg10=' '/some/{split($2,a," "); printf "%d", a[1]}' /proc/pressure/memory; }

settle() {
    for _ in $(seq 1 30); do
        mce_clean || abort "MCE recorded mid-deploy"
        local m p; m=$(mem_avail); p=$(psi_mem)
        [ "$m" -ge 20480 ] && [ "$p" -le 15 ] && { say "  settled mem=${m}Mi psi=$p"; return 0; }
        say "  waiting mem=${m}Mi psi=$p"; sleep 10
    done
    abort "settle timeout"
}

wait_reindex() {  # container names -> block until each passes GATE_H (15 min cap)
    local names=("$@")
    for c in "${names[@]}"; do
        local ok=0
        for _ in $(seq 1 90); do
            local h
            h=$(docker exec "$c" "${CLI[@]}" getblockcount 2>/dev/null || echo -1)
            if [ "${h:--1}" -ge "$GATE_H" ] 2>/dev/null; then ok=1; break; fi
            sleep 10
        done
        [ "$ok" = 1 ] || abort "$c did not reach h>=$GATE_H after reindex (stuck at ${h:-?})"
    done
    say "  reindex gate passed: ${#names[@]} node(s) >= h$GATE_H"
}

roll_batch() {
    local names=("$@") svcs=()
    for c in "${names[@]}"; do
        svcs+=("${c#ptx-w2r-}")
        touch "$DATADIRS/${c#ptx-w2r-}/REINDEX_ONCE" || abort "marker drop failed for $c"
    done
    docker stop -t 45 "${names[@]}" >/dev/null || abort "graceful stop failed: ${names[*]}"
    $COMPOSE up -d --no-deps "${svcs[@]}" >>"$LOG" 2>&1 || abort "compose up failed: ${svcs[*]}"
    sleep 15
    for c in "${names[@]}"; do
        local got
        got=$(docker exec "$c" md5sum /usr/local/bin/Hemisd 2>/dev/null | awk '{print $1}')
        [ "$got" = "$WANT_MD5" ] || abort "$c binary md5 $got != $WANT_MD5 (stale image?)"
        [ -f "$DATADIRS/${c#ptx-w2r-}/REINDEX_ONCE" ] && abort "$c did not consume REINDEX_ONCE marker"
    done
    wait_reindex "${names[@]}"
    settle
}

say "=== BUG-037 FIX DEPLOY + REINDEX start (md5 $WANT_MD5) ==="
mce_clean || abort "preflight MCE"

n=1
while [ $n -le 153 ]; do
    hi=$((n + 19)); [ $hi -gt 153 ] && hi=153
    say "--- GM batch $n..$hi ---"
    batch=()
    for i in $(seq $n $hi); do batch+=("$(printf 'ptx-w2r-gm%02d' "$i")"); done
    roll_batch "${batch[@]}"
    n=$((hi + 1))
done

for i in $(seq 1 8); do
    say "--- caller$i ---"
    roll_batch "ptx-w2r-caller$i"
done

say "--- validation ---"
sleep 45
up=$(docker ps --filter name=ptx-w2r -q | wc -l)
ready=0
for i in $(seq 1 153); do
    c=$(printf 'ptx-w2r-gm%02d' "$i")
    docker exec "$c" "${CLI[@]}" getgamemasterstatus 2>/dev/null | grep -q Ready && ready=$((ready+1))
done
say "containers=$up/161 ready=$ready/153"
TIP=$(docker exec ptx-w2r-caller1 "${CLI[@]}" getblockcount 2>/dev/null)
SETTLE_H=$((TIP - 23))
declare -A tips
for i in $(seq 1 153) c1 c2 c3 c4 c5 c6 c7 c8; do
    case $i in c*) c="ptx-w2r-caller${i#c}";; *) c=$(printf 'ptx-w2r-gm%02d' "$i");; esac
    h=$(docker exec "$c" "${CLI[@]}" getblockhash "$SETTLE_H" 2>/dev/null)
    tips[${h:-FAIL}]=$(( ${tips[${h:-FAIL}]:-0} + 1 ))
done
say "settle-depth h$SETTLE_H agreement (expect single hash x161):"
for k in "${!tips[@]}"; do say "  $k x${tips[$k]}"; done
say "pose-restore log check (callers, expect 'restored' lines on future restarts; reindex path resets instead):"
mce_clean && say "MCE clean" || say "MCE RECORDS PRESENT"
say "=== BUG-037 DEPLOY COMPLETE (tip $TIP) ==="
