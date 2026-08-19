#!/bin/bash
# Parallel fan-out (fanpar) fleet validation — run AFTER the rolling deploy,
# BEFORE any ladder (a broken build burns ~120 stakes across a full run).
#   1. probe roll     — capture the quorum (FULL member ids incl. :suffix —
#                       a bare "gmNN" failmode key never matches) from the
#                       roll's own quorum_members output
#   2. RED (failmode) — withhold 6/11 members => roll must FAIL CLEAN (<30s,
#                       "threshold not met"), proving collection + clean-fail
#                       on the parallel pass (reuses the abandon-gate hooks)
#   3. slow member    — 2000ms netem on ONE member's veth => a roll whose
#                       quorum CONTAINS that member must stay ~clean-fast:
#                       stop-at-sixth-FASTEST, never gated on the slowest
#   4. smoke          — one roll on each of the 8 callers
set -u
W2=/mnt/pve/Node14TB/hemis-ptx/w2-fleet
LOG=$W2/fanpar_validation.log
CLI=(Hemis-cli -ptxbea -rpcuser=ptxw2rpc -rpcpassword=ptxw2pass2026)
say() { echo "$(date '+%F %T') $*" | tee -a "$LOG"; }
fail() { say "FAIL: $*"; cleanup; exit 1; }

SLOW_VETH=""
MEMBERS=()
cleanup() {
    for gm in "${MEMBERS[@]:-}"; do
        docker exec ptx-w2r-caller1 "${CLI[@]}" ptx_debug_setnodefailmode "$gm" clear >/dev/null 2>&1
    done
    [ -n "$SLOW_VETH" ] && tc qdisc del dev "$SLOW_VETH" root >/dev/null 2>&1
}
trap cleanup EXIT

roll1() {  # roll1 <caller> <game_id>; sets LAT, ROLL_OUT; rc = roll rc
    local c=$1 gid=$2 t0 t1 rc salt
    salt=$(head -c16 /dev/urandom | md5sum | cut -c1-12)
    t0=$(date +%s.%N)
    ROLL_OUT=$(docker exec "$c" "${CLI[@]}" ptx_roll 1 1 100 false "[]" "$gid" "$salt" 2>&1); rc=$?
    t1=$(date +%s.%N)
    LAT=$(echo "$t1 $t0" | awk '{printf "%.2f", $1-$2}')
    return $rc
}
members_of() {  # parse quorum_members (full ids) from $ROLL_OUT
    echo "$ROLL_OUT" | python3 -c '
import json,sys
try: print("\n".join(json.load(sys.stdin)["quorum_members"]))
except Exception: pass'
}

say "=== FANPAR VALIDATION start ==="

# --- 1. probe roll: capture the quorum this caller's rolls use ---
roll1 ptx-w2r-caller1 fanpar_probe || fail "probe roll failed: $ROLL_OUT"
mapfile -t MEMBERS < <(members_of)
[ "${#MEMBERS[@]}" -eq 11 ] || fail "expected 11 quorum_members, got ${#MEMBERS[@]}"
QHASH=$(echo "$ROLL_OUT" | python3 -c 'import json,sys; print(json.load(sys.stdin)["quorum_hash"][:16])')
say "probe roll ok in ${LAT}s — quorum ${QHASH} members: ${MEMBERS[*]}"

# --- 2. RED: withhold 6 members -> clean sub-30s threshold failure ---
for gm in "${MEMBERS[@]:0:6}"; do
    docker exec ptx-w2r-caller1 "${CLI[@]}" ptx_debug_setnodefailmode "$gm" withhold >/dev/null 2>&1 \
        || fail "setnodefailmode $gm failed"
done
say "RED: withheld ${MEMBERS[*]:0:6}"
if roll1 ptx-w2r-caller1 fanpar_red; then
    RQ=$(echo "$ROLL_OUT" | python3 -c 'import json,sys; print(json.load(sys.stdin)["quorum_hash"][:16])' 2>/dev/null)
    fail "RED roll SUCCEEDED (${LAT}s, quorum ${RQ:-?} vs withheld ${QHASH}) — quorum rotated or failmodes ineffective"
fi
echo "$ROLL_OUT" | grep -q "threshold not met" || fail "RED failed for the wrong reason: $ROLL_OUT"
awk -v l="$LAT" 'BEGIN{exit !(l<30)}' || fail "RED took ${LAT}s (>=30s wall ceiling breached)"
say "RED ok: clean threshold failure in ${LAT}s (<30s): $(echo "$ROLL_OUT" | grep -o 'threshold not met[^\"]*' | head -1)"
for gm in "${MEMBERS[@]:0:6}"; do
    docker exec ptx-w2r-caller1 "${CLI[@]}" ptx_debug_setnodefailmode "$gm" clear >/dev/null 2>&1
done

# --- 3. single slow member: 2000ms on one member's veth ---
SLOW_ID=${MEMBERS[0]}
SLOW_CONT="ptx-w2r-${SLOW_ID%%:*}"
SLOW_VETH=$(awk -v c="$SLOW_CONT" '$1==c{print $2}' "$W2/netem_veth.map")
[ -n "$SLOW_VETH" ] || fail "no veth mapping for $SLOW_CONT (run netem_mesh.sh map)"
tc qdisc replace dev "$SLOW_VETH" root netem delay 2000ms || fail "tc apply failed on $SLOW_VETH"
say "slow-member: 2000ms netem on $SLOW_ID ($SLOW_CONT / $SLOW_VETH)"
# Bounds: a BELOW-threshold pass must await every dial to classify retryables,
# so ONE cold roll may pay one slow-member completion (~4s at 2000ms netem;
# capped by the 5s member timeout). An answered member is never re-dialed, so
# the cost is once-per-roll at most — the fatal (sequential) behavior was
# EVERY pass paying it. Proof of stop-at-sixth-fastest = warm in-quorum rolls
# staying ~clean-fast: require every in-quorum roll <8s AND the majority <4s.
HIT=0 FASTHIT=0
for i in 1 2 3 4 5 6; do
    roll1 ptx-w2r-caller1 "fanpar_slow$i" || fail "slow-member roll $i failed: $ROLL_OUT"
    if members_of | grep -qx "$SLOW_ID"; then
        HIT=$((HIT+1))
        say "slow-member roll $i ok in ${LAT}s (quorum CONTAINS $SLOW_ID)"
        awk -v l="$LAT" 'BEGIN{exit !(l<8)}' || fail "roll $i with slow member in-quorum took ${LAT}s (>=8s — gated on the slow member beyond the one pass-tail wait)"
        awk -v l="$LAT" 'BEGIN{exit !(l<4)}' && FASTHIT=$((FASTHIT+1))
    else
        say "slow-member roll $i ok in ${LAT}s (quorum without slow member — not probative)"
    fi
    [ "$HIT" -ge 3 ] && break
done
[ "$HIT" -ge 1 ] || fail "no roll picked a quorum containing $SLOW_ID — case vacuous, re-run"
[ "$FASTHIT" -ge $(( (HIT/2)+1 )) ] || fail "only $FASTHIT/$HIT in-quorum rolls <4s — slow member gating warm rolls (stop-at-sixth-fastest broken)"
tc qdisc del dev "$SLOW_VETH" root >/dev/null 2>&1; SLOW_VETH=""
say "slow-member ok: $HIT in-quorum roll(s) unaffected by a 2000ms member (stop-at-sixth-fastest proven)"

# --- 4. smoke: one roll per caller ---
OKC=0
for i in $(seq 1 8); do
    if roll1 "ptx-w2r-caller$i" fanpar_smoke; then
        say "smoke caller$i ok ${LAT}s"; OKC=$((OKC+1))
    else
        say "smoke caller$i FAILED: $ROLL_OUT"
    fi
done
[ "$OKC" -eq 8 ] || fail "smoke $OKC/8"
say "=== FANPAR VALIDATION COMPLETE: probe+RED+slow-member($HIT in-quorum)+smoke 8/8 green ==="
