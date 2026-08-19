#!/bin/bash
# px1 idle-soak heartbeat — ported from node1's soak_watch.sh (2026-08-19).
# One line every 5 min; the last line before a gap bounds any crash to a 5-min window.
#
# kmce counts kernel 'mce: [Hardware Error]' lines THIS boot, INCLUDING the boot-time
# replay of crash-moment records, which never enter rasdaemon's DB. That gap is the
# 2026-08-16 probe finding: mce_clean=1 while the kernel had logged Bank-5 residue.
# The pattern deliberately does NOT match 'MCE: In-kernel MCE decoding enabled' — that
# banner is printed on every clean boot and a naive `grep -i mce` counts it as a fault.
PATH=/usr/sbin:/usr/bin:/sbin:/bin
WORK=/mnt/ptx-ssd-work
LOG=$WORK/SOAK_IDLE_px1.log
BASE=$WORK/soak_px1.baseline
DQ=$WORK/SOAK_DISQUALIFIED

[ -f "$BASE" ] || exit 0
KMCE_BASE=$(awk -F= '/^kmce_base=/{print $2}' "$BASE")
BOOT_ID_BASE=$(awk -F= '/^boot_id=/{print $2}' "$BASE")

TCTL=$(sensors k10temp-pci-00c3 2>/dev/null | awk '/Tctl/{print $2}')
MEMA=$(free -m | awk '/^Mem:/{print $7}')
UP=$(awk '{printf "%d", $1}' /proc/uptime)
LOAD=$(cut -d' ' -f1-3 /proc/loadavg)
MCE=$(ras-mc-ctl --summary 2>/dev/null | grep -c '^No MCE errors')
KMCE=$(journalctl -k -b 0 --no-pager 2>/dev/null | grep -ac 'mce: \[Hardware Error\]')
BOOT_ID=$(cat /proc/sys/kernel/random/boot_id)

STATUS=ok
# ANY MCE disqualifies. Three independent ways to fail:
#  1. kernel logged a Hardware Error line beyond the baseline
#  2. rasdaemon stopped reporting "No MCE errors"
#  3. the boot_id changed => the box REBOOTED under us, i.e. it crashed
[ "${KMCE:-0}" -gt "${KMCE_BASE:-0}" ] && STATUS=DISQUALIFIED-KMCE
[ "${MCE:-0}" -ne 1 ] && STATUS=DISQUALIFIED-RASDAEMON
[ "$BOOT_ID" != "$BOOT_ID_BASE" ] && STATUS=DISQUALIFIED-REBOOT

echo "$(date '+%F %T') up=${UP}s Tctl=${TCTL} memavail=${MEMA}Mi load=${LOAD} mce_clean=${MCE} kmce=${KMCE}/${KMCE_BASE} status=${STATUS}" >> "$LOG"

if [ "$STATUS" != ok ] && [ ! -f "$DQ" ]; then
    { echo "$(date '+%F %T') *** SOAK DISQUALIFIED: $STATUS ***"
      echo "kmce=$KMCE base=$KMCE_BASE mce_clean=$MCE boot_id=$BOOT_ID base_boot=$BOOT_ID_BASE"
      journalctl -k -b 0 --no-pager 2>/dev/null | grep -a 'mce: \[Hardware Error\]' | tail -20
    } > "$DQ"
    echo "$(date '+%F %T') *** DISQUALIFIED — see $DQ ***" >> "$LOG"
fi
