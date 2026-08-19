#!/bin/bash
# Staged restart of the fresh-genesis 153+8 fleet from intact datadirs (2026-08-15).
# Restart, NOT rebuild: bootstrap completed to tip 240 before crash #4; datadirs on SSD.
# Gentle bring-up per the pre-crash plan: batches of 20, mem floor 20 GiB, PSI ceiling 15,
# abort on any rasdaemon MCE record. Callers first (producers/seeds), then gm001..gm153.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
LOG=/mnt/pve/Node14TB/hemis-ptx/BRINGUP_2026-08-15.log
BATCH=20
# Floor scales with installed RAM (2026-08-17: box may boot with 16/32/64 GB while
# DIMMs are being swapped for the MCE hunt). Full 161-container fleet needs ~16 GiB;
# quarter-of-total keeps the old 64 GB behaviour (16 GiB floor ~= the 20 GiB intent)
# and still aborts loudly if the fleet genuinely can't fit the installed RAM.
MEM_TOTAL_MIB=$(awk '/^MemTotal:/{printf "%d", $2/1024}' /proc/meminfo)
MEM_FLOOR_MIB=$(( MEM_TOTAL_MIB / 4 ))
PSI_CEIL=15
CLI=(Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)

say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }

mce_clean() { ras-mc-ctl --summary 2>/dev/null | grep -q '^No MCE errors'; }

# PROBE GAP (proven twice: 2026-08-16, and 2026-08-18 08:02 after crash #20).
# rasdaemon only sees MCEs raised while it is RUNNING. A crash-moment record is
# replayed by the kernel at ~1.0s into the NEXT boot — before rasdaemon starts —
# so it never enters the DB and mce_clean() reports "clean" over a real fault.
# The kernel log is the authority for boot-replay; rasdaemon for live faults.
# Count is per-boot (-b 0); grep -a because crash-spanning logs carry NULs.
kmce_count() { journalctl -k -b 0 --no-pager 2>/dev/null | grep -ac 'mce: \[Hardware Error\]'; }
KMCE_BASE=0   # set at preflight, before settle() can run

abort() { say "ABORT: $*  — no further batches started; running containers left up (loud)"; exit 1; }

mem_avail() { free -m | awk '/^Mem:/{print $7}'; }
psi_mem()   { awk -F'avg10=' '/some/{split($2,a," "); printf "%d", a[1]}' /proc/pressure/memory; }

settle() {
    # Wait until mem floor + PSI ceiling are respected (max ~5 min), MCE-checked.
    for _ in $(seq 1 30); do
        mce_clean || abort "MCE recorded during bring-up (rasdaemon has a decoded record)"
        local k; k=$(kmce_count)
        [ "$k" -le "$KMCE_BASE" ] || abort "NEW kernel MCE during bring-up (kmce ${KMCE_BASE} -> ${k}) — fault under our load, not boot replay"
        local m p; m=$(mem_avail); p=$(psi_mem)
        if [ "$m" -ge "$MEM_FLOOR_MIB" ] && [ "$p" -le "$PSI_CEIL" ]; then
            say "  settled: memavail=${m}Mi psi_mem_avg10=${p}"
            return 0
        fi
        say "  waiting: memavail=${m}Mi psi_mem_avg10=${p}"
        sleep 10
    done
    abort "settle timeout: memavail=$(mem_avail)Mi psi=$(psi_mem) after 5 min"
}

rpc_ok() { docker exec "$1" "${CLI[@]}" getblockcount >/dev/null 2>&1; }

say "=== STAGED BRING-UP START (batch=$BATCH, floor=${MEM_FLOOR_MIB}Mi, psi<=$PSI_CEIL) ==="
mce_clean || abort "preflight: rasdaemon already has MCE records — verdict is FAIL, do not bring up"
# Boot-replay residue is a record of the PREVIOUS crash, not a fault under our load.
# It must be reported loudly (the ledger under-counted crashes without it) but must NOT
# abort: unattended auto-bringup has to keep recovering the fleet after a crash reboot.
KMCE_BASE=$(kmce_count)
if [ "$KMCE_BASE" -gt 0 ]; then
    say "preflight: *** BOOT-REPLAY MCE RESIDUE: ${KMCE_BASE} kernel MCE line(s) this boot ***"
    say "preflight: *** previous boot ended in a machine check — rasdaemon DB is empty by design (replay predates it) ***"
    journalctl -k -b 0 --no-pager 2>/dev/null | grep -a 'mce: \[Hardware Error\]' \
        | sed 's/^/    /' | tee -a "$LOG"
    say "preflight: proceeding anyway (residue is prior-boot); NEW MCEs from here abort the run"
else
    say "preflight: kernel MCE log clean this boot (kmce=0)"
fi
say "preflight: mce clean (rasdaemon), kmce_base=${KMCE_BASE}, memavail=$(mem_avail)Mi, psi=$(psi_mem)"

say "--- callers 1-8 ---"
for i in $(seq 1 8); do docker start "ptx-w2r-caller$i" >/dev/null || abort "docker start caller$i failed"; done
sleep 15
for i in $(seq 1 8); do
    for _ in $(seq 1 24); do rpc_ok "ptx-w2r-caller$i" && break; sleep 5; done
    rpc_ok "ptx-w2r-caller$i" || abort "caller$i RPC not responsive after 2 min"
done
say "callers 8/8 RPC-responsive"
settle

n=1
while [ $n -le 153 ]; do
    hi=$((n + BATCH - 1)); [ $hi -gt 153 ] && hi=153
    say "--- GM batch $n..$hi ---"
    for i in $(seq $n $hi); do
        docker start "$(printf 'ptx-w2r-gm%02d' "$i")" >/dev/null \
            || abort "docker start gm$(printf '%03d' "$i") failed"
    done
    sleep 20
    settle
    n=$((hi + 1))
done

say "--- validation ---"
for _ in $(seq 1 60); do
    up=$(docker ps --filter name=ptx-w2r --format '{{.Names}}' | wc -l)
    [ "$up" -eq 161 ] && break; sleep 5
done
say "containers running: $(docker ps --filter name=ptx-w2r -q | wc -l)/161"

resp=0; ready=0; declare -A tips
for i in $(seq 1 153) c1 c2 c3 c4 c5 c6 c7 c8; do
    case $i in c*) c="ptx-w2r-caller${i#c}";; *) c=$(printf 'ptx-w2r-gm%02d' "$i");; esac
    h=$(docker exec "$c" "${CLI[@]}" getbestblockhash 2>/dev/null)
    if [ -n "$h" ]; then resp=$((resp+1)); tips[$h]=$(( ${tips[$h]:-0} + 1 )); fi
    case $c in *gm*)
        docker exec "$c" "${CLI[@]}" getgamemasterstatus 2>/dev/null | grep -q Ready && ready=$((ready+1));;
    esac
done
say "RPC-responsive: $resp/161; GM Ready: $ready/153; distinct tips: ${#tips[@]}"
for h in "${!tips[@]}"; do say "  tip $h x${tips[$h]}"; done
say "tip height (caller1): $(docker exec ptx-w2r-caller1 "${CLI[@]}" getblockcount 2>/dev/null)"
KMCE_END=$(kmce_count)
if ! mce_clean; then
    say "MCE: rasdaemon RECORDS PRESENT — check ras-mc-ctl --errors"
elif [ "$KMCE_END" -gt "$KMCE_BASE" ]; then
    say "MCE: NEW kernel MCE lines during bring-up (kmce ${KMCE_BASE} -> ${KMCE_END}) — check journalctl -k -b 0"
elif [ "$KMCE_BASE" -gt 0 ]; then
    say "MCE: no NEW faults (kmce steady at ${KMCE_BASE}) — but this boot carries ${KMCE_BASE} boot-replay line(s) from the previous crash"
else
    say "MCE: still clean (rasdaemon empty AND kmce=0 this boot)"
fi
say "memavail=$(mem_avail)Mi psi=$(psi_mem)"
say "=== BRING-UP COMPLETE ==="
