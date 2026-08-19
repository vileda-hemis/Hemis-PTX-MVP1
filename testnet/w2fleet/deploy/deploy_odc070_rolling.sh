#!/bin/bash
# Rolling recreate onto ptx-w2:roll-odc070-24106753 (snapshot retention 370fca0
# + share-health 3332a05 + BUG-038 12d356c + ODC-070 share store efbbb27).
# Plain recreate, NO reindex — a reindex is what destroyed the shares; avoiding
# one in the deploy that fixes share durability is the point.  Graceful stops,
# md5 gate, settle gate.  GMs in batches of 20, callers last.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd /mnt/pve/Node14TB/hemis-ptx/docker-w2r
LOG=/mnt/pve/Node14TB/hemis-ptx/DEPLOY_ODC070_2026-08-16.log
COMPOSE="docker compose -f docker-compose.generated.yml -p ptx-w2r"
WANT_MD5=241067532516e7e853366b8365e77caf
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
    docker stop -t 45 "${names[@]}" >/dev/null || abort "graceful stop failed: ${names[*]}"
    $COMPOSE up -d --no-deps "${svcs[@]}" >>"$LOG" 2>&1 || abort "compose up failed: ${svcs[*]}"
    sleep 15
    for c in "${names[@]}"; do
        local got
        got=$(docker exec "$c" md5sum /usr/local/bin/Hemisd 2>/dev/null | awk '{print $1}')
        [ "$got" = "$WANT_MD5" ] || abort "$c binary md5 $got != $WANT_MD5 (stale image?)"
    done
    settle
}

say "=== ODC-070 ROLLING DEPLOY start (md5 $WANT_MD5, no reindex) ==="
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
say "=== ODC-070 DEPLOY COMPLETE (tip $TIP) ==="
