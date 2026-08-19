#!/bin/bash
# Launch the three published dashboard skins as detached writer jobs.
#
# Kept as a FILE, not an inline command: `pkill -f`/`ps|grep` on the script name
# matched the invoking `bash -c` line itself (its argv contains the pattern) and
# killed the launching shell mid-run — twice. A script file has a short argv, so
# both the launch and any later match-and-kill stay unambiguous.
#
#   fleet.html         standard mobile-first monitor  (Vileda's page)
#   fleet-term.html    terminal wall view, ANSI re-painted verbatim
#   fleet-ceefax.html  40-column teletext page (P100 legacy skin)
#   --teletext-dir     writes the WHOLE teletext service: P100..P888, one file
#                      per page, driven by views_extra.PAGES
#
# The last two come from ONE render() per tick, so they add no extra RPC sweep
# or 98-log grep beyond the standard page's.
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
STATIC=/mnt/pve/Node14TB/hemis-ptx/explorer/static
cd "$W2" || exit 1

# --n/--callers passed EXPLICITLY to both writers: the caller count is a
# chain-liveness figure under the wallet-less-GM topology (callers are the sole
# block producers), and it must track gen_fleet's --callers. Leaving it to a
# default is how the board silently reported "callers funded 0/4" on an
# 8-caller fleet.
# ★ w2r fleet (2026-08-15 fresh genesis): 153 GM + 8 callers, port-base 32000,
# SSD datadirs. --datadir/--jsonl/--detector-state defaults in ptx_dashboard.py
# already point at the w2r paths; passed explicitly anyway so this file IS the
# record of what the writers watch.
setsid nohup python3 -u ptx_dashboard.py --n 153 --callers 8 --port-base 32000 \
    --datadir /mnt/ptx-ssd-work/w2r-fleet/datadirs \
    --jsonl /mnt/pve/Node14TB/hemis-ptx/docker-w2r/demand-w2r153.jsonl \
    --detector-state "$W2/bug034_state_w2r153.json" \
    --html "$STATIC/fleet.html" --serve --interval 30 \
    > /tmp/dash_std.log 2>&1 < /dev/null &

setsid nohup python3 -u ptx_dashboard.py --n 153 --callers 8 --port-base 32000 \
    --datadir /mnt/ptx-ssd-work/w2r-fleet/datadirs \
    --jsonl /mnt/pve/Node14TB/hemis-ptx/docker-w2r/demand-w2r153.jsonl \
    --detector-state "$W2/bug034_state_w2r153.json" \
    --serve --interval 30 \
    --html-term "$STATIC/fleet-term.html" \
    --html-ceefax "$STATIC/fleet-ceefax.html" \
    --teletext-dir "$STATIC" \
    > /tmp/dash_term.log 2>&1 < /dev/null &

# ★ 2026-08-18: the BUG-034 detector is launched HERE, with the readers.
# It is the WRITER of bug034_state_w2r153.json; both dashboards above only READ
# it ("ONE reader shared by every view", ptx_dashboard.py:756-762). It had been
# started by hand, died in a crash, and nothing restarted it — the boot wrapper
# relaunches these writers but knew nothing about the detector. Result: the file
# froze at h2029 on 2026-08-17 14:40 while the chain ran on to h3663, and the
# dashboard panel rendered that stale snapshot as a live "pending 0, alert
# False" all-clear for ~32 hours across three crashes. Co-launching it with the
# readers makes the boot wrapper's existing dashboard-relaunch cover it too.
if ! ps -eo comm,args | awk '$1=="python3" && /bug034_detector/' | grep -q .; then
    setsid nohup python3 -u bug034_detector.py \
        >> "$W2/bug034_detector_w2r153.out" 2>&1 < /dev/null &
fi

sleep 5
echo "[dash] writers launched:"
ps -eo pid,comm,args | awk '$2=="python3" && /dashboard/ {print "   pid " $1}'
ps -eo pid,comm,args | awk '$2=="python3" && /bug034_detector/ {print "   pid " $1 " (bug034 detector)"}'
