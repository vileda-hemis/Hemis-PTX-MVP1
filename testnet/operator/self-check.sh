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
# ★ Read the ports from THIS GM's own config, not from a constant. A host running
# three GMs has three port pairs, and probing the wrong pair reports a healthy
# node as broken (or worse, a broken one as healthy because a SIBLING answered).
# Precedence: explicit env override > the datadir's Hemis.conf > the defaults.
conf_val() { sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^[:space:]#]*\).*/\1/p" "$DATADIR/Hemis.conf" 2>/dev/null | tail -1; }
P2P_PORT="${PTX_P2P_PORT:-$(conf_val port)}";    P2P_PORT="${P2P_PORT:-29994}"
RPC_PORT="${PTX_RPC_PORT:-$(conf_val rpcport)}"; RPC_PORT="${RPC_PORT:-29995}"

PASS=0; FAIL=0; WARNS=0; UNKNOWN=0
ok()   { printf '  [ok]   %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '  [FAIL] %s\n' "$*"; FAIL=$((FAIL+1)); }
warn() { printf '  [warn] %s\n' "$*"; WARNS=$((WARNS+1)); }
# ★ THE THIRD OUTCOME, AND THE REASON THIS SCRIPT ONCE LIED.
# Sections 3-6 are the load-bearing ones. When one of them could not obtain its
# evidence -- an RPC that did not answer the way we parse, a field that was not
# there -- the old script emitted a [warn], and warns do not touch the exit code.
# The result was a node with nothing bound, no listener and a stranger's PoSe on
# screen being reported as "2 passed, 0 failed ... No failures", exit 0.
# A check that CANNOT RUN is not a check that PASSED. It gets its own outcome,
# its own line in the verdict, and its own exit code (2).
unk()  { printf '  [????] %s\n' "$*"; UNKNOWN=$((UNKNOWN+1)); }
say()  { printf '\n=== %s ===\n' "$*"; }

cli() { $CLI -datadir="$DATADIR" "$@" 2>/dev/null; }

printf 'Checking GM at %s  (P2P %s, RPC %s)\n' "$DATADIR" "$P2P_PORT" "$RPC_PORT"

# Probe a TCP port. Handles IPv6 (needs brackets) and IPv4 alike.
probe() {
    local host="$1" port="$2" target
    case "$host" in
        *:*) target="[$host]" ;;   # IPv6 literal
        *)   target="$host" ;;
    esac
    timeout 6 bash -c "exec 3<>/dev/tcp/${target}/${port}" 2>/dev/null && return 0
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
# ★ THE MESSAGE USED TO SAY "seed addresses are correct", WHICH POINTED AT
# NOTHING. This network has no DNS seeds and no fixed seeds (chainparams.cpp:887,
# :898) -- peer discovery is entirely addnode=, and if the config has none there
# is nothing to be correct or incorrect. Name the real cause.
[ "${PEERS:-0}" -gt 0 ] && ok "$PEERS peer connection(s)" \
    || bad "ZERO peers. This network has NO peer discovery: check that Hemis.conf has addnode= lines UNDER the [ptxtestnet] header (above it they are ignored), that the coordinator gave you the seed addresses, and that outbound $P2P_PORT is allowed."
# ★ Two peers, not one. The staking/tier-two sync needs GETSPORKS answered by two
# DISTINCT peers before it leaves the sporks phase (gamemaster-sync.cpp:272-284,
# GAMEMASTER_SYNC_THRESHOLD=2 in tiertwo/tiertwo_sync_state.h:22); one peer only
# gets there after the 1h fulfilled-request expiry. Not fatal for a gamemaster,
# which does not stake -- but a one-peer node is one disconnect from zero.
if [ "${PEERS:-0}" = "1" ]; then
    warn "only ONE peer. Add the coordinator's other addnode= seeds: a single peer is one disconnect away from an isolated node."
fi

# ---------------------------------------------------------------------------
say "3. Registration and the registered address"
# ---------------------------------------------------------------------------
# ★ THE FIELD NAMES HERE ARE NOT NEGOTIABLE -- they come from the daemon.
# A deterministic gamemaster's getgamemasterstatus returns CDeterministicGM::ToJson
# (src/evo/deterministicgms.cpp), which nests everything under "dgmstate" and calls
# the service address "service". There is NO "addr" key on that path at all.
# This script used to parse "addr" and therefore found NOTHING on every real node:
# section 5 then skipped, section 4's family comparison never ran, and the script
# still exited 0. "addr" DOES exist on the legacy branch -- where it is a PAYMENT
# ADDRESS, not a host -- so parsing it is worse than useless: it feeds a wallet
# address to a TCP probe.
jval() { printf '%s' "$GMSTATUS" | sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\"\([^\"]*\)\".*/\1/p" | head -1; }
jnum() { printf '%s' "$GMSTATUS" | sed -n "s/.*\"$1\"[[:space:]]*:[[:space:]]*\(-\{0,1\}[0-9][0-9]*\).*/\1/p" | head -1; }

GMSTATUS=$(cli getgamemasterstatus)
REGADDR=""
if [ -z "$GMSTATUS" ]; then
    warn "getgamemasterstatus returned nothing -- this node is not registered yet, or gamemaster=1 is missing from Hemis.conf"
else
    # "service" is the registered address (what PEERS dial); "netaddr" is what this
    # daemon believes its own service address is. They should agree, and a mismatch
    # is worth saying out loud rather than silently preferring one.
    SERVICE=$(jval service)
    NETADDR=$(jval netaddr)
    if [ -n "$SERVICE" ]; then
        REGADDR="$SERVICE"
        ok "registered service address: $REGADDR"
        if [ -n "$NETADDR" ] && [ "$NETADDR" != "$SERVICE" ]; then
            bad "the address you REGISTERED ($SERVICE) is not the one this daemon is running as ($NETADDR). Peers will dial $SERVICE."
        fi
    elif [ -n "$NETADDR" ]; then
        REGADDR="$NETADDR"
        warn "no on-chain 'service' field; falling back to this daemon's own 'netaddr' ($REGADDR). Registration may not be confirmed yet."
    else
        unk "getgamemasterstatus answered but carried neither 'service' nor 'netaddr'. Sections 4-5 cannot be evaluated. Paste its output when you report this."
    fi
    # Sanity: it must look like host:port before anything probes it.
    case "$REGADDR" in
        *:[0-9]*) : ;;
        "") : ;;
        *) unk "'$REGADDR' does not look like host:port; refusing to probe it."; REGADDR="" ;;
    esac

    STATE=$(jval status)
    [ -n "$STATE" ] && echo "  on-chain status: $STATE"
    NODEID=$(jval ptxNodeId)
    [ -n "$NODEID" ] && echo "  ptxNodeId: $NODEID"

    # ★★ The registration-time trap from the OPERATOR_GUIDE, checked rather than
    # described. ptxPaymentAddress can ONLY be set at registration; a GM without it
    # runs perfectly, signs correctly, and can never win a PTXPAYOUT lottery. It is
    # not fixable by editing a config, so finding out late is finding out too late.
    if printf '%s' "$GMSTATUS" | grep -q '"ptxPaymentAddress"'; then
        ok "ptxPaymentAddress is set -- this GM is eligible for PTXPAYOUT wins"
    else
        bad "★★ ptxPaymentAddress is NOT set. This GM can never win a PTXPAYOUT lottery, and it CANNOT be fixed in a config file -- it is registration-time state. Re-register with ptxPaymentAddress supplied."
    fi
fi

# ---------------------------------------------------------------------------
say "4. Bind coverage -- the IPv6 seam"
# ---------------------------------------------------------------------------
# ★ THIS IS THE SILENT KILLER. If you registered an IPv6 address but the daemon
# only bound IPv4 (or vice-versa), everything local looks perfect and no peer
# can ever reach you.
# ★ `ss` spells the three cases differently and they are easy to misread:
#   0.0.0.0:P  IPv4 only        [::]:P  IPv6 only (v6only=1)
#   *:P        a DUAL-STACK IPv6 socket -- it serves BOTH families, so it must
#              count for both. Counting it as IPv4 only reports "NOT bound on
#              IPv6" for a node that answers perfectly well on IPv6.
BINDS=$(ss -Hltn 2>/dev/null | awk '{print $4}')
V4_RPC=$(printf '%s\n' "$BINDS" | grep -cE "^(0\.0\.0\.0|127\.0\.0\.1|\*):$RPC_PORT$")
V6_RPC=$(printf '%s\n' "$BINDS" | grep -cE "^(\[(::|::1)\]|\*):$RPC_PORT$")
[ "$V4_RPC" -gt 0 ] && ok "RPC $RPC_PORT bound on IPv4" || warn "RPC $RPC_PORT NOT bound on IPv4"
[ "$V6_RPC" -gt 0 ] && ok "RPC $RPC_PORT bound on IPv6" || warn "RPC $RPC_PORT NOT bound on IPv6"
# ★ One family missing is a warning; NEITHER family bound means nothing is
# listening on the RPC port at all, so no peer can ever reach you. That is not two
# warnings, it is one failure -- and it was scored as two warnings.
if [ "$V4_RPC" -eq 0 ] && [ "$V6_RPC" -eq 0 ]; then
    bad "RPC $RPC_PORT is NOT LISTENING on any address family. No peer can contact this GM. Is the daemon running with this datadir, and does its rpcport match $RPC_PORT?"
fi

if [ -n "${REGADDR:-}" ]; then
    REGHOST="${REGADDR%:*}"; REGHOST="${REGHOST#[}"; REGHOST="${REGHOST%]}"
    case "$REGADDR" in
        \[*) FAMILY=6 ;;
        *:*:*) FAMILY=6 ;;
        *) FAMILY=4 ;;
    esac
    echo "  registered address family: IPv$FAMILY (host $REGHOST)"
    if [ "$FAMILY" = 6 ] && [ "$V6_RPC" -eq 0 ]; then
        bad "★ IPv6 SEAM: you registered an IPv6 address but RPC is not bound on IPv6. Peers CANNOT reach you. Add 'rpcbind=::' to Hemis.conf and restart."
    elif [ "$FAMILY" = 4 ] && [ "$V4_RPC" -eq 0 ]; then
        bad "★ IPv4 SEAM: you registered an IPv4 address but RPC is not bound on IPv4. Add 'rpcbind=0.0.0.0' to Hemis.conf and restart."
    else
        ok "registered family matches a bound family"
    fi
fi

# ---------------------------------------------------------------------------
say "5. ★ EXTERNAL reachability at the REGISTERED address"
# ---------------------------------------------------------------------------
if [ -z "${REGADDR:-}" ]; then
    # ★ Not a pass. This is THE check the guide calls the silent killer, and it did
    # not run. Scoring it as a warning is how this script came to report a dead node
    # as healthy.
    unk "no registered address -- section 5 DID NOT RUN. This is not a pass. Re-run after protx_register, and fix section 3 first if it reported a problem."
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
# ★ THREE THINGS WERE WRONG HERE AND THE THIRD MADE THE FIRST TWO INVISIBLE.
#   1. `ptx_gm_pose` takes a REQUIRED node_id (src/rpc/ptx.cpp: params.size() != 1
#      throws). Called bare it always errored, stderr was swallowed, and it always
#      fell through to the fallback.
#   2. `ptx_pose_status` returns EVERY GM on the network. Reading "the first
#      pose_score" reads SOMEONE ELSE'S SCORE and presents it as your verdict.
#   3. The regex was "pose_?score" in a BRE, where `?` is a literal character. It
#      matched nothing, so the wrong answer from (2) never surfaced -- and the
#      whole section quietly degraded to a warning that cost nothing.
#
# The score for THIS node is already in the section-3 output: dgmstate.PoSePenalty
# is the chain's own count for this gamemaster. Use that as the verdict, and use
# ptx_gm_pose -- with the node_id it actually wants -- only for the extra detail.
PENALTY=$(jnum PoSePenalty)
BANHEIGHT=$(jnum PoSeBanHeight)
if [ -n "$PENALTY" ]; then
    if [ "$PENALTY" -eq 0 ]; then
        ok "PoSe penalty 0 -- the network is reaching you"
    else
        bad "PoSe penalty $PENALTY (non-zero). The network is FAILING to reach you. Re-read section 5; trust this over anything section 5 said."
    fi
    if [ -n "$BANHEIGHT" ] && [ "$BANHEIGHT" -ge 0 ]; then
        bad "PoSe BANNED at height $BANHEIGHT. You are out of the eligible set until revived."
    fi
elif [ -z "$GMSTATUS" ]; then
    warn "not registered yet, so the network has no verdict on you (normal before protx_register)"
else
    unk "getgamemasterstatus carried no PoSePenalty. The network's verdict on this node is UNKNOWN -- this is not a pass."
fi

# Detail, and the one thing PoSePenalty does not tell you: tickets this window.
if [ -n "${NODEID:-}" ]; then
    DETAIL=$(cli ptx_gm_pose "$NODEID")
    if [ -n "$DETAIL" ]; then
        TICKETS=$(printf '%s' "$DETAIL" | sed -n 's/.*"tickets"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' | head -1)
        [ -n "$TICKETS" ] && echo "  honest-participation tickets this window: $TICKETS"
    else
        warn "ptx_gm_pose '$NODEID' returned nothing (the pose tracker may not have seen this GM in a roll yet)"
    fi
else
    warn "no ptxNodeId on chain for this GM -- registered without one. Not fatal; it is the label used in quorum output."
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
printf '\n=== RESULT: %d passed, %d failed, %d could-not-run, %d warnings ===\n' \
    "$PASS" "$FAIL" "$UNKNOWN" "$WARNS"
if [ "$FAIL" -gt 0 ]; then
    echo "  Fix the [FAIL] lines above before reporting your node as ready."
    exit 1
fi
if [ "$UNKNOWN" -gt 0 ]; then
    # ★ Exit 2, deliberately not 0. "Nothing failed" and "the checks ran" are
    # different claims, and only the second one means your node is ready.
    echo "  Nothing FAILED, but $UNKNOWN load-bearing check(s) COULD NOT RUN -- see the [????] lines."
    echo "  Do NOT report this node as ready. An unrun check is not a passed check."
    exit 2
fi
echo "  All checks ran and none failed. If section 5 only PASSED by hairpin, ask another operator to connect to you."
