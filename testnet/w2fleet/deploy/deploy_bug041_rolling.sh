#!/bin/bash
# Rolling recreate onto ptx-w2:bug041-<md5> — BUG-041 fix: PoS template
# parallel-array off-by-one (one sentinel vs two SolveProofOfStake txs) made
# the BUG-034 P3 exclusion read vTxFees/vTxSigOps out-of-bounds and call
# erase(end()) on a tail-index drop — gcc-12 -O2 heap corruption, three
# caller cores, three random downstream death sites, one cause.
# L1 aligns the arrays at creation; L2 bounds-checks the P3 drop loop (loud).
# GMs first in batches of 20 (validators never crashed — producer-side fix),
# callers LAST (they stake, i.e. carry the fixed CreateNewBlock path; a
# caller recreated mid-roll just fails that one roll). Mixed fleet safe: no
# consensus semantics change. Plain recreate, NO reindex.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd /mnt/pve/Node14TB/hemis-ptx/docker-w2r
LOG=/mnt/pve/Node14TB/hemis-ptx/DEPLOY_BUG041_2026-08-18.log
COMPOSE="docker compose -f docker-compose.generated.yml -p ptx-w2r"
WANT_MD5=8c77d2287c9aad27d07bf7d2344eb102
CLI=(Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)
# Adaptive settle floor (the bringup_staged.sh 2026-08-18 lesson: a hard-coded
# 64GB-calibrated floor aborts unattended runs when the box changes RAM).
FLOOR=$(( $(awk '/MemTotal/{print int($2/1024)}' /proc/meminfo) / 4 ))

say()  { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
mce_clean() { ras-mc-ctl --summary 2>/dev/null | grep -q '^No MCE errors'; }
abort() { say "ABORT: $*"; exit 1; }
mem_avail() { free -m | awk '/^Mem:/{print $7}'; }
psi_mem()   { awk -F'avg10=' '/some/{split($2,a," "); printf "%d", a[1]}' /proc/pressure/memory; }

settle() {
    for _ in $(seq 1 30); do
        mce_clean || abort "MCE recorded mid-deploy"
        local m p; m=$(mem_avail); p=$(psi_mem)
        [ "$m" -ge "$FLOOR" ] && [ "$p" -le 15 ] && { say "  settled mem=${m}Mi psi=$p"; return 0; }
        say "  waiting mem=${m}Mi psi=$p"; sleep 10
    done
    abort "settle timeout"
}

roll_batch() {
    local names=("$@") svcs=()
    for c in "${names[@]}"; do svcs+=("${c#ptx-w2r-}"); done
    docker stop -t 45 "${names[@]}" >/dev/null 2>&1
    $COMPOSE up -d --no-deps "${svcs[@]}" >>"$LOG" 2>&1 || abort "compose up failed: ${svcs[*]}"
    sleep 15
    for c in "${names[@]}"; do
        local got
        got=$(docker exec "$c" md5sum /usr/local/bin/Hemisd 2>/dev/null | awk '{print $1}')
        [ "$got" = "$WANT_MD5" ] || abort "$c binary md5 $got != $WANT_MD5 (stale image?)"
    done
    settle
}

say "=== BUG-041 ROLLING DEPLOY start (md5 $WANT_MD5, floor ${FLOOR}Mi, GMs first, callers last, no reindex) ==="
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
    docker exec "$c" "${CLI[@]}" getgamemasterstatus 2>/dev/null | grep -q '"status": "Ready"' && ready=$((ready+1))
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
mce_clean && say "MCE clean" || say "MCE RECORDS PRESENT"
say "=== BUG-041 DEPLOY COMPLETE (tip $TIP) ==="
