#!/bin/bash
# STANDING TEST — unclean-shutdown replay consistency (BUG-029/036/037 class).
# SIGKILLs a node mid-run, restarts it, and asserts its crash-rollback replay
# reaches the SAME chain as the rest of the fleet.  The class's three known
# instances all needed a condition no clean test produces: state judged during
# replay against stores that did not roll back with the chainstate.
#
# Usage: kill_replay_check.sh <gmNN|callerN> [ref_node]
#   victim   — node to SIGKILL+restart (e.g. gm77)
#   ref_node — agreement reference (default caller1)
#
# PASS = victim tip-hash == reference tip-hash at settle depth (23), zero new
# InvalidChainFound after the kill, and the replay report printed.
# ANTI-VACUITY: prints how many blocks were replayed and how many carried a
# PTXPAYOUT (type 10) — a run that replays 0 payout blocks does NOT exercise
# the BUG-037 verdict path and says so loudly.
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin
VICTIM="${1:?usage: kill_replay_check.sh <gmNN|callerN> [ref]}"
REF="${2:-caller1}"
C="ptx-w2r-$VICTIM"; R="ptx-w2r-$REF"
DATADIR="/mnt/ptx-ssd-work/w2r-fleet/datadirs/$VICTIM/ptxbea"
CLI=(Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)
say() { echo "$(date '+%F %T') $*"; }
fail() { say "FAIL: $*"; exit 1; }

pre_kill_lines=$(/usr/bin/grep -ac . "$DATADIR/debug.log" 2>/dev/null || echo 0)
pre_tip=$(docker exec "$C" "${CLI[@]}" getblockcount) || fail "victim RPC dead pre-kill"
say "victim=$VICTIM pre-kill tip=$pre_tip — SIGKILL now"

docker kill "$C" >/dev/null || fail "docker kill failed"
sleep 2
docker start "$C" >/dev/null || fail "docker start failed"

# Wait for RPC (reindex never happens here — REINDEX_ONCE only; replay is fast)
up=0
for _ in $(seq 1 60); do
    docker exec "$C" "${CLI[@]}" getblockcount >/dev/null 2>&1 && { up=1; break; }
    sleep 5
done
[ "$up" = 1 ] || fail "victim RPC not back after 5 min"

# Replay report: first UpdateTip after restart = rollback base.
base_h=$(tail -n +"$pre_kill_lines" "$DATADIR/debug.log" 2>/dev/null | /usr/bin/grep -a -m1 -oE "UpdateTip: new best=[0-9a-f]+  height=[0-9]+" | /usr/bin/grep -oE "height=[0-9]+" | cut -d= -f2)
[ -n "${base_h:-}" ] || base_h=$pre_tip

# Give it time to catch the fleet tip
sleep 30
ref_tip=$(docker exec "$R" "${CLI[@]}" getblockcount)
settle_h=$((ref_tip - 23))
for _ in $(seq 1 30); do
    v=$(docker exec "$C" "${CLI[@]}" getblockcount 2>/dev/null || echo 0)
    [ "$v" -ge "$settle_h" ] && break
    sleep 10
done

vh=$(docker exec "$C" "${CLI[@]}" getblockhash "$settle_h" 2>/dev/null) || fail "victim has no block at settle depth h$settle_h"
rh=$(docker exec "$R" "${CLI[@]}" getblockhash "$settle_h") || fail "ref getblockhash failed"
[ "$vh" = "$rh" ] || fail "DIVERGENCE at h$settle_h: victim $vh != ref $rh (BUG-037 class)"

inv=$(tail -n +"$pre_kill_lines" "$DATADIR/debug.log" | /usr/bin/grep -ac "InvalidChainFound" || true)
[ "${inv:-0}" -eq 0 ] || fail "victim marked $inv chain(s) invalid after restart — replay rejected history"

# Anti-vacuity: count payout blocks in the replayed range [base_h, pre_tip]
payouts=0
if [ "$base_h" -lt "$pre_tip" ]; then
    for h in $(seq "$base_h" "$pre_tip"); do
        bh=$(docker exec "$R" "${CLI[@]}" getblockhash "$h" 2>/dev/null) || continue
        docker exec "$R" "${CLI[@]}" getblock "$bh" 2 2>/dev/null | /usr/bin/grep -q '"type": 10' && payouts=$((payouts+1))
    done
fi
restore_line=$(tail -n +"$pre_kill_lines" "$DATADIR/debug.log" | /usr/bin/grep -a -m1 "PTX PoSe: restored" || true)
say "replayed h$base_h..h$pre_tip ($((pre_tip - base_h)) blocks), payout blocks crossed: $payouts"
[ -n "$restore_line" ] && say "pose restore: $restore_line" || say "pose restore line ABSENT (pre-fix binary or no snapshot?)"
if [ "$payouts" -eq 0 ]; then
    say "WARNING: VACUOUS for the BUG-037 verdict path — no PTXPAYOUT in the replayed range; drive rolls across a settlement boundary and re-run"
fi
say "PASS: victim rejoined the same chain (h$settle_h $vh), no invalid marks"
