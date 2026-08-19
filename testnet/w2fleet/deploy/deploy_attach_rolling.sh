#!/bin/bash
# KDD-088 DIRECT-ATTACH rolling recreate onto ptx-w2:attach-5be4ceda.
# The commitment now rides the gm_bls_sign request; a member that has not seen it
# via gossip accepts it locally through the NORMAL mempool path (accept-into-
# mempool IS the security mechanism -- the BUG-032 gate's predicate was always
# "is it in MY mempool", never "was it broadcast").
#
# GMs FIRST, callers LAST. Order is not load-bearing here -- old members ignore a
# third RPC arg (the guard is params.size() < 2, not != 2), and callers keep
# broadcasting anyway -- so every intermediate state is a working fleet. GMs
# first simply means the benefit is live the moment the callers roll.
# No consensus semantics change: nothing a validator checks depends on how a
# member learned of a transaction, so there is no activation gate and no reindex.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd /mnt/pve/Node14TB/hemis-ptx/docker-w2r
LOG=/mnt/pve/Node14TB/hemis-ptx/DEPLOY_ATTACH_2026-08-19.log
COMPOSE="docker compose -f docker-compose.generated.yml -p ptx-w2r"
WANT_MD5=5be4cedae926ebd1024c96c27db19a2b
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

# --- RESUME CHECKPOINT (2026-08-19) --------------------------------------
# The host's most recent hold was 16 minutes; this deploy is estimated at
# 15-20. Without a checkpoint an interrupted run restarts at gm01 and can never
# finish. The checkpoint is deliberately NOT a state file: the script already
# reads each container's binary md5, so THE LIVE FLEET IS THE CHECKPOINT. That
# cannot drift out of sync with reality the way a written ledger can, and it
# needs no cleanup after a partial run.
# Fail-safe direction: a container that is down, or whose md5 cannot be read,
# reads as NOT done and gets rolled. Unknown always means work, never skip.
already_deployed() {
    [ "$(docker exec "$1" md5sum /usr/local/bin/Hemisd 2>/dev/null | awk '{print $1}')" = "$WANT_MD5" ]
}

roll_batch() {
    local names=("$@") svcs=() todo=() skipped=0
    for c in "${names[@]}"; do
        if already_deployed "$c"; then skipped=$((skipped + 1)); else todo+=("$c"); fi
    done
    if [ ${#todo[@]} -eq 0 ]; then
        say "  SKIP: checkpoint — all ${#names[@]} already at $WANT_MD5"
        return 0
    fi
    [ "$skipped" -gt 0 ] && say "  resume: $skipped of ${#names[@]} already at md5, rolling ${#todo[@]}"
    for c in "${todo[@]}"; do svcs+=("${c#ptx-w2r-}"); done
    docker stop -t 45 "${todo[@]}" >/dev/null 2>&1
    $COMPOSE up -d --no-deps "${svcs[@]}" >>"$LOG" 2>&1 || abort "compose up failed: ${svcs[*]}"
    sleep 15
    for c in "${todo[@]}"; do
        local got
        got=$(docker exec "$c" md5sum /usr/local/bin/Hemisd 2>/dev/null | awk '{print $1}')
        [ "$got" = "$WANT_MD5" ] || abort "$c binary md5 $got != $WANT_MD5 (stale image?)"
    done
    settle
}

say "=== KDD-088 DIRECT-ATTACH ROLLING DEPLOY start (md5 $WANT_MD5, floor ${FLOOR}Mi, GMs first, callers last, no reindex, resumable) ==="
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
say "=== KDD-088 DIRECT-ATTACH DEPLOY COMPLETE (tip $TIP) ==="
