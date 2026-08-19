#!/bin/bash
# Boot-time fleet recovery (installed 2026-08-17 after crash #9).
# The host crashes silently (data-fabric sync flood class, box demoted to
# local-dev); fleet containers are restart=no BY DESIGN, so every crash left
# 161 containers exited until an operator ran bringup_staged.sh by hand.
# This wrapper runs at every boot via ptx-fleet-bringup.service and hands off
# to the proven staged bringup ONLY when the fleet is actually down.
#
# To hold the fleet down deliberately across a reboot (deploy, ladder run,
# migration): touch /mnt/pve/Node14TB/hemis-ptx/docker-w2r/BRINGUP_DISABLE
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
BASE=/mnt/pve/Node14TB/hemis-ptx
LOG=$BASE/CRASH_BRINGUP.log
DISABLE=$BASE/docker-w2r/BRINGUP_DISABLE

say() { echo "$(date '+%F %T') $*" >> "$LOG"; }

say "=== boot wrapper start (uptime $(awk '{printf "%d", $1}' /proc/uptime)s) ==="

if [ -e "$DISABLE" ]; then
    say "SKIP: $DISABLE present — operator holds fleet down"
    exit 0
fi

# Wait for the docker daemon (up to 5 min).
for _ in $(seq 1 60); do docker info >/dev/null 2>&1 && break; sleep 5; done
if ! docker info >/dev/null 2>&1; then
    say "ABORT: docker daemon not responsive after 5 min"
    exit 1
fi

total=$(docker ps -a --filter name=ptx-w2r -q | wc -l)
up=$(docker ps --filter name=ptx-w2r -q | wc -l)
say "fleet containers: $up up / $total total"

if [ "$total" -eq 0 ]; then
    say "SKIP: no ptx-w2r containers exist (fleet retired/migrated)"
    exit 0
fi
if [ "$up" -ge $((total / 2)) ]; then
    say "SKIP: fleet already majority-up — not a crash-down boot"
    exit 0
fi

# Let the box settle (infra containers, filesystems, thermal) before load.
say "fleet is down — settling 60s, then staged bringup"
sleep 60

# bringup_staged.sh carries its own guards (MCE preflight+per-batch, mem floor,
# PSI ceiling) and aborts loudly into BRINGUP_2026-08-15.log on any violation.
if "$BASE/docker-w2r/bringup_staged.sh"; then
    say "staged bringup COMPLETE (see BRINGUP_2026-08-15.log for validation)"
else
    say "staged bringup ABORTED (rc=$?) — see BRINGUP_2026-08-15.log; NOT retrying"
    exit 1
fi

# Re-arm the dashboard writers if none are running (idempotence guard: the
# launcher itself does not check, and doubled writers double the RPC sweep).
# comm-anchored match, NOT pgrep -f: a bare pgrep -f self-matches the invoking
# shell's argv when run interactively (the trap in start_dash_writers.sh header).
if ! ps -eo comm,args | awk '$1=="python3" && /ptx_dashboard/' | grep -q .; then
    say "relaunching dashboard writers"
    "$BASE/w2-fleet/start_dash_writers.sh" >> "$LOG" 2>&1
else
    say "dashboard writers already running — leaving as-is"
fi

# Ladder auto-resume (2026-08-17): an unfinished checkpointed ladder run
# (done-file present without the ALL-DONE sentinel) continues from the next
# rung. Detached so it survives this unit; ladder_ckpt.sh holds its own flock.
W2=$BASE/w2-fleet
if [ -f "$W2/ladder_ckpt.done" ] && ! grep -q '^ALL-DONE' "$W2/ladder_ckpt.done"; then
    say "ladder run in progress — resuming from checkpoint (ladder_ckpt_run.log)"
    # >/dev/null, NOT the run log: ladder_ckpt.sh tees its own output to the
    # log already — the old redirect double-wrote every say()/battery line.
    setsid nohup "$W2/ladder_ckpt.sh" resume >/dev/null 2>&1 &
fi

# Blackout-hunt auto-resume (2026-08-19): the hunt is a ~1-in-36 event search at
# d200, i.e. LONGER than this box's mean time between crashes, so it has to
# resume itself the same way the ladder does. Its checkpoint is written
# atomically (never appended), so an interrupted run re-enters at the caller and
# roll it stopped on. `finished: true` (a blackout was caught) makes both this
# hook and the script itself a no-op — it must not keep burning stakes after the
# question is answered. It takes the same ladder flock, so it can never run
# concurrently with a ladder/replication pass.
if [ -f "$W2/blackout_hunt.ckpt.json" ] \
   && ! grep -q '"finished": *true' "$W2/blackout_hunt.ckpt.json"; then
    say "blackout hunt unfinished — resuming from checkpoint (blackout_hunt.log)"
    setsid nohup "$W2/blackout_hunt.sh" >/dev/null 2>&1 &
fi

say "=== boot wrapper done ==="
