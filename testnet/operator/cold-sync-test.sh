#!/usr/bin/env bash
# PTX testnet — COLD-SYNC harness.
#
# ★ THIS IS NOT AN OPERATOR SCRIPT. It is OUR gate, run before a release tag is
# cut and again after any consensus change. Operators run install.sh.
#
# ---------------------------------------------------------------------------
# WHY IT EXISTS
#
# A running fleet NEVER RE-VALIDATES the chain it already connected. So "the
# fleet is healthy" says nothing about whether TODAY's binary can validate that
# chain FROM BLOCK 0 -- which is exactly what every joining operator does on
# day one. Those two properties differ precisely where h385 lived: an ungated
# consensus rule (BUG-032's C8 re-keying) applied to all history, so the live
# fleet ran on happily while every fresh node wedged at 384.
#
# ★ -reindex IS NOT A FALLBACK, IT IS A WEAKER TEST. At the h385 arc, reindex
# reached tip 876 CLEAN while network IBD wedged at 384 -- same binary, same
# block -- because reindex SKIPS C8 on stored blocks. The standup recorded that
# result as "VACUOUS as a validity diagnostic". This harness refuses to
# substitute it and asserts afterwards that it did not happen.
#
# ★ "EMPTY DATADIR" MEANS FRESHLY CREATED, NOT CLEANED. gm121's h297
# ptxdkg-duplicate at the same arc was a WIPE ARTIFACT -- leftover evodb/ and
# llmq/ -- i.e. a harness manufacturing its own failure. This one refuses to
# start against a directory that already has contents.
#
# ★★ ANTI-VACUITY, AND IT IS THE WHOLE REASON THIS MEANS ANYTHING.
# A node that syncs NOTHING reports zero bad blocks. Measured 2026-08-24 with
# -connect at a non-existent peer: after 60s, height 0, peers 0, and `bad-*`
# lines: 0. So "no errors in the log" PASSES on a node that never left genesis.
# It is not an assertion. The discriminating assertions are:
#       tip != genesis   AND   height == reference   AND   tip == reference tip
# and the two RED legs below exist to prove this harness can tell the two
# failure modes apart:
#   RED 1  unreachable peer   -> must report a CONNECTIVITY failure (height 0)
#   RED 2  divergent binary   -> must report a VALIDITY failure (bad-* at N>0)
# ★ RED 2 is the one that matters. A genesis/magic mismatch was REJECTED as a
# fixture: ptxbea is "PTX3" and ptxtestnet is "PTXT" (chainparams.cpp), so a
# cross-chain probe never completes the handshake and fails at the NETWORK
# layer -- indistinguishable from RED 1, and it would prove the connectivity
# path while appearing to prove the validity one.
#
# Usage:
#   PTX_TEST_BINDIR=/path/to/bin PTX_COLDSYNC_PEER=<host:port> \
#   PTX_REF_RPC=127.0.0.1:32000 PTX_REF_USER=u PTX_REF_PASS=p \
#   [PTX_MUTANT_BINDIR=/path/to/mutant] ./cold-sync-test.sh
set -u
PATH=/usr/sbin:/usr/bin:/sbin:/bin

NET="${PTX_COLDSYNC_NET:--ptxbea}"
NETDIR="${PTX_COLDSYNC_NETDIR:-ptxbea}"
PEER="${PTX_COLDSYNC_PEER:?set PTX_COLDSYNC_PEER=<host:port> (one peer, the reference)}"
REF_RPC="${PTX_REF_RPC:?set PTX_REF_RPC=<host:port> for the reference peer RPC}"
REF_USER="${PTX_REF_USER:?set PTX_REF_USER}"
REF_PASS="${PTX_REF_PASS:?set PTX_REF_PASS}"
BASE="${PTX_COLDSYNC_BASE:-/tmp/ptx-coldsync-$$}"
RPCPORT="${PTX_COLDSYNC_RPCPORT:-31975}"
P2PPORT="${PTX_COLDSYNC_P2PPORT:-31974}"
TIMEOUT="${PTX_COLDSYNC_TIMEOUT:-1800}"
STALL="${PTX_COLDSYNC_STALL:-120}"
# ★ RED 1's PEER MUST BE UNREACHABLE FROM WHEREVER THIS RUNS, AND THAT IS A
# PROPERTY OF THE ADDRESS, NOT OF THIS NETWORK. It was 172.32.0.250 -- an
# address on the fleet's Docker bridge, which is unreachable from most hosts by
# accident rather than by rule. ★ 172.32/12 is NOT RFC1918: private space stops
# at 172.31.255.255, so 172.32.0.250 is globally-routable address space that
# happens to be borrowed for a bridge here. Shipped to operators, RED 1 would
# have been dialling a stranger's host and calling the result a controlled
# falsification. 192.0.2.0/24 is TEST-NET-1 (RFC 5737), reserved for exactly
# this and guaranteed never to route anywhere. Same port as the real peer, so
# the address is the only variable between the green run and this leg.
RED1_PEER="${PTX_COLDSYNC_RED1_PEER:-192.0.2.1:${PEER##*:}}"

if [ -n "${PTX_TEST_BINDIR:-}" ]; then
    HEMISD="$PTX_TEST_BINDIR/Hemisd"; HEMISCLI="$PTX_TEST_BINDIR/Hemis-cli"
else
    HEMISD="$(command -v Hemisd || true)"; HEMISCLI="$(command -v Hemis-cli || true)"
fi

PASS=0; FAIL=0; RED_PASS=0; RED_FAIL=0
ok()   { printf '  \033[32m[ok]\033[0m   %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
redok(){ printf '  \033[32m[RED ok]\033[0m %s\n' "$*"; RED_PASS=$((RED_PASS+1)); }
redno(){ printf '  \033[31m[RED VACUOUS]\033[0m %s\n' "$*"; RED_FAIL=$((RED_FAIL+1)); }
note() { printf '         %s\n' "$*"; }
say()  { printf '\n=== %s ===\n' "$*"; }
die()  { printf '\n  [ABORT] %s\n\n' "$*" >&2; cleanup; exit 3; }

PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    sleep 2
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
}
trap cleanup EXIT INT TERM

refcli() { "$HEMISCLI" "$NET" -rpcconnect="${REF_RPC%:*}" -rpcport="${REF_RPC##*:}" \
                       -rpcuser="$REF_USER" -rpcpassword="$REF_PASS" "$@" 2>/dev/null; }

# ── one cold-sync run ───────────────────────────────────────────────────────
# $1 label   $2 bindir   $3 peer   -> sets CS_HEIGHT CS_TIP CS_REASON CS_BADHASH CS_BAND
run_cold_sync() {
    local label="$1" bindir="$2" peer="$3"
    local dd="$BASE/$label" d="$bindir/Hemisd" c="$bindir/Hemis-cli"
    CS_HEIGHT=0; CS_TIP=""; CS_REASON=""; CS_BADHASH=""; CS_BAND=""

    # ★ freshly created, never cleaned — a reused datadir manufactures failures
    rm -rf "$dd"; mkdir -p "$dd"
    [ "$(ls -A "$dd" | wc -l)" -eq 0 ] || die "$label: datadir not empty at start"

    "$d" "$NET" -datadir="$dd" -addnode="$peer" -connect="$peer" -dnsseed=0 \
         -listen=0 -port="$P2PPORT" -rpcport="$RPCPORT" -rpcbind=127.0.0.1 \
         -rpcallowip=127.0.0.1 -rpcuser=cs -rpcpassword=cs -daemon >/dev/null 2>&1
    local t0 last=-1 stalled=0 h ref
    t0=$(date +%s)
    # verify by outcome: -daemon exits 0 even when startup then fails
    local up=0
    for _ in $(seq 1 40); do
        sleep 3
        if "$c" "$NET" -datadir="$dd" -rpcport="$RPCPORT" -rpcuser=cs -rpcpassword=cs \
                getblockcount >/dev/null 2>&1; then up=1; break; fi
    done
    [ "$up" -eq 1 ] || { CS_REASON="daemon never answered RPC"; return 1; }
    PIDS="$PIDS $(cat "$dd/$NETDIR/Hemisd.pid" 2>/dev/null)"

    while :; do
        h=$("$c" "$NET" -datadir="$dd" -rpcport="$RPCPORT" -rpcuser=cs -rpcpassword=cs \
              getblockcount 2>/dev/null || echo "")
        [ -n "$h" ] || h=$CS_HEIGHT
        ref=$(refcli getblockcount); [ -n "$ref" ] || ref=0
        local el=$(( $(date +%s) - t0 ))
        CS_BAND="${CS_BAND}${el}s:${h} "        # ★ the per-height band-map — what made h385 findable
        CS_HEIGHT=$h
        [ "$h" -ge "$ref" ] && [ "$h" -gt 0 ] && break
        if [ "$h" = "$last" ]; then stalled=$((stalled+5)); else stalled=0; fi
        last=$h
        [ "$stalled" -ge "$STALL" ] && { CS_REASON="stalled ${STALL}s"; break; }
        [ "$el" -ge "$TIMEOUT" ] && { CS_REASON="timeout ${TIMEOUT}s"; break; }
        sleep 5
    done
    CS_TIP=$("$c" "$NET" -datadir="$dd" -rpcport="$RPCPORT" -rpcuser=cs -rpcpassword=cs \
               getbestblockhash 2>/dev/null || echo "")
    CS_GENESIS=$("$c" "$NET" -datadir="$dd" -rpcport="$RPCPORT" -rpcuser=cs -rpcpassword=cs \
               getblockhash 0 2>/dev/null || echo "")
    CS_PEERS=$("$c" "$NET" -datadir="$dd" -rpcport="$RPCPORT" -rpcuser=cs -rpcpassword=cs \
               getconnectioncount 2>/dev/null || echo 0)
    local log="$dd/$NETDIR/debug.log"
    CS_BADLINE=$(grep -oiE "bad-[a-z0-9-]+" "$log" 2>/dev/null | head -1)
    # ★ NO `|| echo 0` HERE. grep -c already prints 0 on no-match but EXITS 1,
    # so the fallback appended a SECOND line and the numeric test then saw
    # "0\n0" and errored -- which this harness reported as "the weak path was
    # used", a FALSE FAIL from its own instrument. Caught on the first real run.
    CS_REINDEX=$(grep -ci "reindex" "$log" 2>/dev/null | head -1)
    CS_REINDEX=${CS_REINDEX:-0}
    CS_BADHASH=$(grep -iE "InvalidChainFound|bad-" "$log" 2>/dev/null | grep -oE "[0-9a-f]{64}" | head -1)
    "$c" "$NET" -datadir="$dd" -rpcport="$RPCPORT" -rpcuser=cs -rpcpassword=cs stop >/dev/null 2>&1
    sleep 4
    return 0
}

report_run() {   # $1 label
    note "band-map: $CS_BAND"
    note "height=$CS_HEIGHT peers=$CS_PEERS tip=${CS_TIP:0:16}… genesis=${CS_GENESIS:0:16}…"
    [ -n "$CS_BADLINE" ] && note "reject reason: $CS_BADLINE  block: ${CS_BADHASH:0:16}…"
    [ -n "$CS_REASON" ]  && note "stopped because: $CS_REASON"
}

say "PREFLIGHT"
[ -x "$HEMISD" ]   || die "no Hemisd. Set PTX_TEST_BINDIR=<dir>."
[ -x "$HEMISCLI" ] || die "no Hemis-cli. Set PTX_TEST_BINDIR=<dir>."
REF_H=$(refcli getblockcount); REF_TIP=$(refcli getbestblockhash)
[ -n "$REF_H" ] || die "reference peer RPC at $REF_RPC did not answer"
ok "reference peer: height $REF_H tip ${REF_TIP:0:16}…"
mkdir -p "$BASE"

# ── GREEN ───────────────────────────────────────────────────────────────────
say "GREEN — cold sync from block 0 over the network"
T0=$(date +%s)
run_cold_sync green "$PTX_TEST_BINDIR" "$PEER"
ELAPSED=$(( $(date +%s) - T0 ))
report_run green
REF_H2=$(refcli getblockcount); REF_TIP2=$(refcli getbestblockhash)

[ "$CS_HEIGHT" -gt 0 ] \
  && ok "G1 height > 0 ($CS_HEIGHT) — it actually synced something" \
  || bad "G1 height is 0 — synced nothing"
[ -n "$CS_TIP" ] && [ "$CS_TIP" != "$CS_GENESIS" ] \
  && ok "G2 tip != genesis — left block 0" \
  || bad "G2 tip == genesis — never left block 0"
[ "$CS_HEIGHT" -ge "$REF_H" ] \
  && ok "G3 reached the reference height ($CS_HEIGHT >= $REF_H)" \
  || bad "G3 stuck at $CS_HEIGHT, reference was $REF_H (peer now $REF_H2)"
if [ "$CS_TIP" = "$REF_TIP2" ] || [ "$CS_TIP" = "$REF_TIP" ]; then
    ok "G4 tip hash matches the reference peer"
else
    bad "G4 tip mismatch: probe ${CS_TIP:0:16}… vs reference ${REF_TIP2:0:16}…"
fi
[ -z "$CS_BADLINE" ] \
  && ok "G5 no bad-* reject in debug.log" \
  || bad "G5 reject present: $CS_BADLINE at block ${CS_BADHASH:0:16}…"
[ "${CS_REINDEX:-0}" -eq 0 ] \
  && ok "G6 no reindex — this was network IBD, the strong path" \
  || bad "G6 debug.log mentions reindex ${CS_REINDEX}x — the weak path was used"
note "runtime: ${ELAPSED}s for $CS_HEIGHT blocks"

# ── RED 1 — connectivity failure ────────────────────────────────────────────
say "RED 1 — unreachable peer (must report CONNECTIVITY failure, not pass)"
run_cold_sync red1 "$PTX_TEST_BINDIR" "$RED1_PEER"
report_run red1
if [ "$CS_HEIGHT" -eq 0 ] && [ "$CS_TIP" = "$CS_GENESIS" ]; then
    redok "RED 1 — height 0 and tip == genesis, so G1/G2 would FAIL as they must"
    [ -z "$CS_BADLINE" ] && note "★ and bad-* lines: NONE — a log-absence check would have PASSED here"
else
    redno "RED 1 did not produce a wedged node — the leg proves nothing"
fi

# ── RED 2 — validity failure ────────────────────────────────────────────────
if [ -n "${PTX_MUTANT_BINDIR:-}" ] && [ -x "${PTX_MUTANT_BINDIR}/Hemisd" ]; then
    say "RED 2 — divergent binary (must report VALIDITY failure, distinct from RED 1)"
    run_cold_sync red2 "$PTX_MUTANT_BINDIR" "$PEER"
    report_run red2
    if [ -n "$CS_BADLINE" ] && [ "$CS_HEIGHT" -gt 0 ] && [ "$CS_HEIGHT" -lt "$REF_H" ]; then
        redok "RED 2 — refused a block the fleet accepted: stuck at $CS_HEIGHT with $CS_BADLINE"
        note "★ DISTINCT from RED 1: height > 0 and peers connected, so this is"
        note "  'reached the chain and refused it', not 'could not reach it'."
    else
        redno "RED 2 did not produce a validity rejection (height=$CS_HEIGHT reason=${CS_BADLINE:-none})"
    fi
else
    say "RED 2 — SKIPPED (no PTX_MUTANT_BINDIR)"
    note "★ Without it the harness has NOT been shown to detect a validity failure."
    RED_FAIL=$((RED_FAIL+1))
fi

say "Verdict"
printf '  green checks: %d passed, %d failed\n' "$PASS" "$FAIL"
printf '  red   legs:   %d falsified, %d vacuous\n' "$RED_PASS" "$RED_FAIL"
if [ "$FAIL" -eq 0 ] && [ "$RED_FAIL" -eq 0 ]; then
    printf '\n  COLD SYNC OK — and every check has been seen to fail against the\n'
    printf '  defect it names, including a VALIDITY failure distinct from a\n'
    printf '  connectivity one.\n\n'; exit 0
fi
printf '\n  NOT OK\n\n'; exit 1
