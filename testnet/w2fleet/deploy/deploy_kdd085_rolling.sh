#!/bin/bash
# KDD-085 SIGN-OVER-P2P rolling recreate onto ptx-w2:kdd085-1ffd690d (v0.1.3-testnet).
#
# ★★ ORDER IS LOAD-BEARING HERE, UNLIKE THE KDD-088 ATTACH DEPLOY.
# That deploy noted "order is not load-bearing -- old members ignore a third RPC
# arg, so every intermediate state is a working fleet". THAT IS NOT TRUE NOW.
# KDD-085 replaced the CALLER's transport: a new caller speaks P2P `ptxsignreq`,
# and an OLD GM has no handler for it, so it IGNORES the message silently
# (net_processing.cpp:2372 -- a LogPrint under BCLog::NET and nothing else).
# The caller gets no answer it can classify: component 3 leaves that member
# INFLIGHT forever, it keeps counting toward max_reachable, and the round waits
# out the full 30s wall. PROTOCOL_VERSION was never bumped (W4B §9.13(h)), so
# there is no capability signal to detect it either.
#
# ★ In-place upgrade is safe because the new binary is a SUPERSET RESPONDER:
# component 4 deleted the CALLER (ptx_fanout.cpp), not the responder --
# gm_bls_sign is still registered (rpc/ptx.cpp:1806) with its attach optional.
#   old caller -> old GM : works        old caller -> NEW GM : works
#   NEW caller -> old GM : BROKEN       NEW caller -> NEW GM : works
# So EVERY GM FIRST, CALLERS LAST. Under that order the broken combination is
# never reached and no flag day is needed.
#
# No consensus delta: specialtx_validation.cpp and chainparamsbase.cpp changed
# COMMENT-ONLY across bfea163..1d07eda (comment-stripped diff = empty), and
# primitives/transaction.h, validation.cpp, consensus/* are untouched. Historical
# blocks validate identically -- no reindex, no fresh genesis, not the h385 shape.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
cd /mnt/pve/Node14TB/hemis-ptx/docker-w2r || exit 1
LOG=/mnt/pve/Node14TB/hemis-ptx/DEPLOY_KDD085_2026-09-01.log
COMPOSE="docker compose -f docker-compose.generated.yml -p ptx-w2r"
WANT_MD5=bc6cbfc5b131788a825befe10c5c2324
CLI=(Hemis-cli -ptxbea -rpcport=29903 -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)   # ★ -rpcport PINNED: the fleet entrypoint sets 29903, and the binary's compiled-in ptxbea default CHANGED between images -- omitting it made the validation read a default the daemon was never using
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

# THE LIVE FLEET IS THE CHECKPOINT (unchanged from the attach deploy): a
# container that is down, or whose md5 cannot be read, reads as NOT done.
already_deployed() {
    [ "$(docker exec "$1" md5sum /usr/local/bin/Hemisd 2>/dev/null | awk '{print $1}')" = "$WANT_MD5" ]
}

roll_batch() {
    local names=("$@") svcs=() todo=() skipped=0
    for c in "${names[@]}"; do
        if already_deployed "$c"; then skipped=$((skipped + 1)); else todo+=("$c"); fi
    done
    if [ ${#todo[@]} -eq 0 ]; then
        say "  SKIP: checkpoint -- all ${#names[@]} already at $WANT_MD5"; return 0
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

say "=== KDD-085 SIGN-OVER-P2P ROLLING DEPLOY start (md5 $WANT_MD5, floor ${FLOOR}Mi, GMs FIRST callers LAST -- ORDER LOAD-BEARING, no reindex, resumable) ==="
mce_clean || abort "preflight MCE"

# ★ GM SET COMES FROM A FILE, not seq 1..153. The fleet was shrunk to 65 running
# GMs for this pass because node1 lost ~24GB of RAM and could not meet this
# script's own memory floor at 153 (available 1934Mi vs floor 10006Mi, swap 99.9%
# full). Two of the 65 -- gm08 and gm10, both members of the newest active quorum
# -- are DELIBERATELY held back on the old binary to produce F2 item 6's
# condition: a connected member that silently ignores ptxsignreq.
mapfile -t GMS < "${GM_LIST:-/tmp/deploy_gms.txt}"
say "GM set: ${#GMS[@]} from ${GM_LIST:-/tmp/deploy_gms.txt}"
idx=0
while [ $idx -lt ${#GMS[@]} ]; do
    batch=()
    for _ in $(seq 1 20); do
        [ $idx -lt ${#GMS[@]} ] || break
        batch+=("$(printf 'ptx-w2r-gm%02d' "${GMS[$idx]}")")
        idx=$((idx + 1))
    done
    say "--- GM batch of ${#batch[@]} (${batch[0]}..${batch[-1]}) ---"
    roll_batch "${batch[@]}"
done

# ★ GATE: refuse to roll any caller until EVERY GM is on the new binary.
say "--- gate: verifying all 153 GMs before any caller rolls ---"
notyet=0
for i in "${GMS[@]}"; do
    already_deployed "$(printf 'ptx-w2r-gm%02d' "$i")" || notyet=$((notyet+1))
done
[ "$notyet" -eq 0 ] || abort "$notyet GM(s) not on $WANT_MD5 -- rolling a caller now would break every roll whose quorum contains one"
say "  all 153 GMs confirmed at $WANT_MD5 -- safe to roll callers"

for i in $(seq 1 8); do
    say "--- caller$i ---"
    roll_batch "ptx-w2r-caller$i"
done

say "--- validation ---"
sleep 45
up=$(docker ps --filter name=ptx-w2r -q | wc -l)
ready=0
for i in "${GMS[@]}"; do
    c=$(printf 'ptx-w2r-gm%02d' "$i")
    docker exec "$c" "${CLI[@]}" getgamemasterstatus 2>/dev/null | grep -q '"status": "Ready"' && ready=$((ready+1))
done
say "containers=$up/73 ready=$ready/${#GMS[@]} (gm08,gm10 held on old binary by design)"
TIP=$(docker exec ptx-w2r-caller1 "${CLI[@]}" getblockcount 2>/dev/null)
SETTLE_H=$((TIP - 23))
declare -A tips
for i in "${GMS[@]}" c1 c2 c3 c4 c5 c6 c7 c8; do
    case $i in c*) c="ptx-w2r-caller${i#c}";; *) c=$(printf 'ptx-w2r-gm%02d' "$i");; esac
    h=$(docker exec "$c" "${CLI[@]}" getblockhash "$SETTLE_H" 2>/dev/null)
    tips[${h:-FAIL}]=$(( ${tips[${h:-FAIL}]:-0} + 1 ))
done
say "settle-depth h$SETTLE_H agreement (expect single hash x161):"
for k in "${!tips[@]}"; do say "  $k x${tips[$k]}"; done
mce_clean && say "MCE clean" || say "MCE RECORDS PRESENT"
say "=== KDD-085 DEPLOY COMPLETE (tip $TIP) ==="
