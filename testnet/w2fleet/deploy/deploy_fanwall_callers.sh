#!/bin/bash
# §9.5 experiment deploy — FANOUT_MAX_ATTEMPTS derived from FANOUT_WALL_MS/
# FANOUT_RETRY_MS (60 -> 200), making the 30s wall reachable for the first time.
# CALLERS ONLY, by construction: PTX_FanOutSign has exactly one call site
# (rpc/ptx.cpp:320, the ptx_roll RPC) and only callers run ptx_roll. GMs are
# responders and never execute the dial loop, so rolling 153 GMs would change
# nothing measurable and would add 25 min of exposure. Minimal intervention =
# the cleanest experiment: the ONLY delta vs the gen-B ladder is this constant.
# No consensus semantics change; mixed-binary fleet safe.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd /mnt/pve/Node14TB/hemis-ptx/docker-w2r
LOG=/mnt/pve/Node14TB/hemis-ptx/DEPLOY_FANWALL_2026-08-18.log
COMPOSE="docker compose -f docker-compose.generated.yml -p ptx-w2r"
WANT_MD5=f0e5458f2088879b662ff8a4522eb8fb
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

say "=== FANWALL CALLER DEPLOY start (md5 $WANT_MD5, floor ${FLOOR}Mi, callers only) ==="
mce_clean || abort "preflight MCE"

for i in $(seq 1 8); do
    c="ptx-w2r-caller$i"
    say "--- caller$i ---"
    docker stop -t 45 "$c" >/dev/null 2>&1
    $COMPOSE up -d --no-deps "caller$i" >>"$LOG" 2>&1 || abort "compose up failed: caller$i"
    sleep 15
    got=$(docker exec "$c" md5sum /usr/local/bin/Hemisd 2>/dev/null | awk '{print $1}')
    [ "$got" = "$WANT_MD5" ] || abort "$c binary md5 $got != $WANT_MD5 (stale image?)"
    say "  $c md5 OK"
    settle
done

say "=== FANWALL CALLER DEPLOY complete ==="
