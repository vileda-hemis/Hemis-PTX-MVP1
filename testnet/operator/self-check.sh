#!/usr/bin/env bash
# PTX testnet — node self-check.
#
# The check that matters is NOT "is my daemon running". It is:
#
#   ★ IS MY RPC REACHABLE AT THE ADDRESS I REGISTERED ON CHAIN?
#
# A node can be fully synced, correctly registered, showing ENABLED, and still
# NEVER SIGN, because PTX fan-out dials each member's RPC directly at the
# registered address. If that address is unreachable -- firewalled, NATted, or
# the wrong IP FAMILY -- you are selected, never contacted, and you fail
# silently. Nothing in the normal status output says so.
set -uo pipefail

DATADIR="${PTX_DATADIR:-$HOME/.hemis-ptxtestnet}"
CLI="${PTX_CLI:-Hemis-cli}"
P2P_PORT=29994
RPC_PORT=29995

PASS=0; FAIL=0; WARNS=0
ok()   { printf '  [ok]   %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '  [FAIL] %s\n' "$*"; FAIL=$((FAIL+1)); }
warn() { printf '  [warn] %s\n' "$*"; WARNS=$((WARNS+1)); }
say()  { printf '\n=== %s ===\n' "$*"; }

cli() { $CLI -datadir="$DATADIR" "$@" 2>/dev/null; }

# Probe a TCP port. Handles IPv6 (needs brackets) and IPv4 alike.
probe() {
    local host="$1" port="$2" target
    case "$host" in
        *:*) target="[$host]" ;;   # IPv6 literal
        *)   target="$host" ;;
    esac
    timeout 6 bash -c "exec 3<>/dev/tcp/${host}/${port}" 2>/dev/null && return 0
    return 1
}

# ---------------------------------------------------------------------------
say "1. Daemon and RPC (local)"
# ---------------------------------------------------------------------------
if ! cli getblockcount >/dev/null; then
    bad "cannot reach the daemon's RPC locally. Is it running? Try: tail -50 $DATADIR/debug.log"
    echo; echo "  Nothing else can be checked until the daemon answers. Stopping."; exit 1
fi
HEIGHT=$(cli getblockcount)
ok "RPC answers locally; height $HEIGHT"

# ---------------------------------------------------------------------------
say "2. Chain sync"
# ---------------------------------------------------------------------------
PEERS=$(cli getconnectioncount || echo 0)
[ "${PEERS:-0}" -gt 0 ] && ok "$PEERS peer connection(s)" \
    || bad "ZERO peers. Check that outbound $P2P_PORT is allowed and that seed addresses are correct."

# ---------------------------------------------------------------------------
say "3. Registration and the registered address"
# ---------------------------------------------------------------------------
GMSTATUS=$(cli getgamemasterstatus)
if [ -z "$GMSTATUS" ]; then
    warn "getgamemasterstatus returned nothing -- this node is not registered yet, or gamemaster=1 is missing from hemis.conf"
    REGADDR=""
else
    # The registered service address is what PEERS will dial. Extract host and port.
    REGADDR=$(printf '%s' "$GMSTATUS" | sed -n 's/.*"addr"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
    [ -n "$REGADDR" ] && ok "registered service address: $REGADDR" \
        || warn "could not parse a registered address out of getgamemasterstatus"
    STATE=$(printf '%s' "$GMSTATUS" | sed -n 's/.*"status"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' | head -1)
    [ -n "$STATE" ] && echo "  on-chain status: $STATE"
fi

# ---------------------------------------------------------------------------
say "4. Bind coverage -- the IPv6 seam"
# ---------------------------------------------------------------------------
# ★ THIS IS THE SILENT KILLER. If you registered an IPv6 address but the daemon
# only bound IPv4 (or vice-versa), everything local looks perfect and no peer
# can ever reach you.
BINDS=$(ss -Hltn 2>/dev/null | awk '{print $4}')
V4_RPC=$(printf '%s\n' "$BINDS" | grep -cE "^(0\.0\.0\.0|127\.0\.0\.1|\*):$RPC_PORT$")
V6_RPC=$(printf '%s\n' "$BINDS" | grep -cE "^\[?(::|::1)\]?:$RPC_PORT$")
[ "$V4_RPC" -gt 0 ] && ok "RPC $RPC_PORT bound on IPv4" || warn "RPC $RPC_PORT NOT bound on IPv4"
[ "$V6_RPC" -gt 0 ] && ok "RPC $RPC_PORT bound on IPv6" || warn "RPC $RPC_PORT NOT bound on IPv6"

if [ -n "${REGADDR:-}" ]; then
    REGHOST="${REGADDR%:*}"; REGHOST="${REGHOST#[}"; REGHOST="${REGHOST%]}"
    case "$REGADDR" in
        \[*) FAMILY=6 ;;
        *:*:*) FAMILY=6 ;;
        *) FAMILY=4 ;;
    esac
    echo "  registered address family: IPv$FAMILY (host $REGHOST)"
    if [ "$FAMILY" = 6 ] && [ "$V6_RPC" -eq 0 ]; then
        bad "★ IPv6 SEAM: you registered an IPv6 address but RPC is not bound on IPv6. Peers CANNOT reach you. Add 'rpcbind=::' to hemis.conf and restart."
    elif [ "$FAMILY" = 4 ] && [ "$V4_RPC" -eq 0 ]; then
        bad "★ IPv4 SEAM: you registered an IPv4 address but RPC is not bound on IPv4. Add 'rpcbind=0.0.0.0' to hemis.conf and restart."
    else
        ok "registered family matches a bound family"
    fi
fi

# ---------------------------------------------------------------------------
say "5. ★ EXTERNAL reachability at the REGISTERED address"
# ---------------------------------------------------------------------------
if [ -z "${REGADDR:-}" ]; then
    warn "no registered address -- skipping. Re-run this after protx_register."
else
    for spec in "P2P:$P2P_PORT" "RPC:$RPC_PORT"; do
        NAME="${spec%%:*}"; PORT="${spec##*:}"
        if probe "$REGHOST" "$PORT"; then
            ok "$NAME port $PORT accepted a connection at $REGHOST"
        else
            bad "$NAME port $PORT is NOT reachable at $REGHOST -- open it in your firewall AND your NAT / cloud security group"
        fi
    done
    cat <<'NOTE'

  ★ WHAT THIS TEST DOES AND DOES NOT PROVE.
    It connects FROM THIS MACHINE to your own registered address. On many NAT
    setups that succeeds via hairpin routing even when NOBODY OUTSIDE can reach
    you, and on a few it fails even when outsiders can. So:
      - a FAIL here is real: fix it.
      - a PASS here is encouraging, NOT proof.
    The only definitive tests are (a) another operator connecting to you, and
    (b) your PoSe score staying at zero -- checked next, and it is the network's
    own verdict rather than your machine's opinion of itself.
NOTE
fi

# ---------------------------------------------------------------------------
say "6. PoSe -- the network's verdict on your reachability"
# ---------------------------------------------------------------------------
POSE=$(cli ptx_gm_pose 2>/dev/null || cli ptx_pose_status 2>/dev/null)
if [ -z "$POSE" ]; then
    warn "no PoSe data available yet (normal on a brand-new node)"
else
    SCORE=$(printf '%s' "$POSE" | sed -n 's/.*"pose_?score"[[:space:]]*:[[:space:]]*\([0-9]*\).*/\1/p' | head -1)
    if [ -n "$SCORE" ]; then
        [ "$SCORE" -eq 0 ] && ok "PoSe score 0 -- the network is reaching you" \
                           || bad "PoSe score $SCORE (non-zero). The network is FAILING to reach you. Re-read section 5."
    else
        echo "$POSE" | head -5
        warn "could not parse a PoSe score; inspect the output above"
    fi
fi

# ---------------------------------------------------------------------------
say "7. ptx_shares.dat -- custody"
# ---------------------------------------------------------------------------
SHARES="$DATADIR/ptx_shares.dat"
if [ -f "$SHARES" ]; then
    ok "ptx_shares.dat present ($(stat -c%s "$SHARES") bytes, modified $(stat -c%y "$SHARES" | cut -d. -f1))"
    cat <<'NOTE'
  ★ READ THIS EVEN IF EVERYTHING ELSE PASSED (ODC-071):
    ptx_shares.dat lives on the NODE. It is NOT in your wallet backup, and no
    amount of wallet recovery reproduces it. It is rewritten at every DKG
    ceremony. Restoring a datadir snapshot taken BEFORE the newest ceremony
    PERMANENTLY FORFEITS those shares -- the quorum cannot re-issue them.
    Back up this file AFTER every ceremony, or accept that a restore loses them.
NOTE
else
    warn "no ptx_shares.dat yet -- expected until you have taken part in a DKG ceremony"
fi

# ---------------------------------------------------------------------------
say "8. Quorum membership (informational)"
# ---------------------------------------------------------------------------
QL=$(cli ptx_quorum_list 2>/dev/null)
[ -n "$QL" ] && echo "$QL" | head -12 || warn "ptx_quorum_list returned nothing (no quorums formed yet)"
cat <<'NOTE'
  Note: quorum SELECTION is advisory, not consensus-enforced. Do not build
  tooling that assumes the selection you see here is binding on other nodes.
NOTE

# ---------------------------------------------------------------------------
printf '\n=== RESULT: %d passed, %d failed, %d warnings ===\n' "$PASS" "$FAIL" "$WARNS"
[ "$FAIL" -eq 0 ] || { echo "  Fix the [FAIL] lines above before reporting your node as ready."; exit 1; }
echo "  No failures. If section 5 only PASSED by hairpin, ask another operator to connect to you."
