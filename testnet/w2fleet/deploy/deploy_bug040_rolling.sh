#!/bin/bash
# Rolling recreate onto ptx-w2:bug040-a103efe3 (BUG-040 staker SEGV fix:
# settle-parent resolver walks pprev + TestBlockValidity dummy BuildSkip).
# CALLERS FIRST — the fleet is halted at h2102 with 5/8 stakers dead; fixed
# callers un-halt the chain and mine the poison backlog immediately. GMs
# (validators, never crashed — real-pindex path) follow in batches of 20.
# Mixed fleet is safe: the fix changes no consensus semantics, only which
# index object the producer-side walk starts from. Plain recreate, NO reindex.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd /mnt/pve/Node14TB/hemis-ptx/docker-w2r
LOG=/mnt/pve/Node14TB/hemis-ptx/DEPLOY_BUG040_2026-08-17.log
COMPOSE="docker compose -f docker-compose.generated.yml -p ptx-w2r"
WANT_MD5=a103efe36cd1c9247399c155d23968c4
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

roll_batch() {
    local names=("$@") svcs=()
    for c in "${names[@]}"; do svcs+=("${c#ptx-w2r-}"); done
    docker stop -t 45 "${names[@]}" >/dev/null 2>&1   # exited callers: no-op, fine
    $COMPOSE up -d --no-deps "${svcs[@]}" >>"$LOG" 2>&1 || abort "compose up failed: ${svcs[*]}"
    sleep 15
    for c in "${names[@]}"; do
        local got
        got=$(docker exec "$c" md5sum /usr/local/bin/Hemisd 2>/dev/null | awk '{print $1}')
        [ "$got" = "$WANT_MD5" ] || abort "$c binary md5 $got != $WANT_MD5 (stale image?)"
    done
    settle
}

say "=== BUG-040 ROLLING DEPLOY start (md5 $WANT_MD5, callers-first, no reindex) ==="
mce_clean || abort "preflight MCE"

for i in $(seq 1 8); do
    say "--- caller$i ---"
    roll_batch "ptx-w2r-caller$i"
done

say "--- un-halt check: tip must move past 2102 ---"
for _ in $(seq 1 30); do
    TIP=$(docker exec ptx-w2r-caller1 "${CLI[@]}" getblockcount 2>/dev/null || echo 0)
    [ "$TIP" -gt 2102 ] && { say "chain moving again: tip $TIP"; break; }
    sleep 10
done
[ "${TIP:-0}" -gt 2102 ] || say "WARNING: tip still ${TIP:-?} after 5 min — continuing GM roll anyway"

n=1
while [ $n -le 153 ]; do
    hi=$((n + 19)); [ $hi -gt 153 ] && hi=153
    say "--- GM batch $n..$hi ---"
    batch=()
    for i in $(seq $n $hi); do batch+=("$(printf 'ptx-w2r-gm%02d' "$i")"); done
    roll_batch "${batch[@]}"
    n=$((hi + 1))
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
MEMP=$(docker exec ptx-w2r-caller1 "${CLI[@]}" getmempoolinfo 2>/dev/null | grep -o '"size": [0-9]*')
say "tip=$TIP mempool ${MEMP:-?} (backlog should be draining)"
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
say "=== BUG-040 DEPLOY COMPLETE (tip $TIP) ==="
