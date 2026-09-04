#!/usr/bin/env bash
# PTX testnet — node self-check.
#
# The check that matters is NOT "is my daemon running". It is:
#
#   ★ IS MY P2P PORT REACHABLE AT THE ADDRESS I REGISTERED ON CHAIN?
#
# A node can be fully synced, correctly registered, showing ENABLED, and still
# NEVER SIGN, because signing requests arrive at the registered address. If that
# address is unreachable -- firewalled, NATted, or the wrong IP FAMILY -- you are
# selected, never contacted, and you fail silently. Nothing in the normal status
# output says so.
#
# ★★ THE RPC HALF OF THIS QUESTION NO LONGER EXISTS (KDD-085). It used to, and
# the difference matters: the PTX fan-out dialled each member's RPC directly, so
# RPC reachability was a signing requirement. Signing now arrives over P2P, RPC
# is loopback-only, and probing it would report on a property that has stopped
# meaning anything.
# ★ WHICH IS WHY THE RPC PROBE WAS REMOVED RATHER THAN LEFT PASSING. A check
# that still produces a result after the property it tested was retired is worse
# than no check: it is read as evidence. An operator whose RPC probe went green
# would conclude something about their ability to sign, and be wrong.
set -uo pipefail

# ★ Matches install.sh's default, which is now the daemon's OWN default,
# $HOME/.Hemis -- see the BUG-047 note there. A self-check that looked in a
# different directory from the one the operator installed into would report a
# perfectly healthy node as missing.
DATADIR="${PTX_DATADIR:-$HOME/.Hemis}"
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
# ---------------------------------------------------------------------------
say "0. Build identity -- are you running what you were told to run?"
# ---------------------------------------------------------------------------
# ★★ THIS CHECK DID NOT EXIST, AND ITS ABSENCE IS THE DEFECT IT FIXES.
# Nothing in install.sh, self-check.sh or the readiness criteria ever compared
# what the operator is RUNNING against what they were told to install. The
# readiness bar was `getgamemasterstatus == Ready` and `self-check exits 0` --
# neither looks at the binary. An operator on the wrong build passes both.
#
# ★ Measured cost of not having it (2026-09-01): three hosts were reported at
# one tag and running another, and reconciling that took two investigations --
# because `Hemisd -version` and `Hemis-cli -version` are DIFFERENT binaries,
# /usr/local/bin/* were SYMLINKS into /opt, and a peer's `subver` carries no
# commit at all. None of that is obvious at a prompt.
#
# ★★ COMPARED AGAINST THE REPO, NOT A HARDCODED CONSTANT. There is no value
# here to update at each cut: the binary's own string is checked against the
# checked-out source it should have been built from. That also catches the
# real-world error -- binary and source drifting apart -- which a constant
# never would.
#
# ★ ACCEPTS EITHER FORM, so it survives ODC-092's fix rather than needing to be
# undone by it. Today a release binary reports "v1.3.1.0-<commit>" because the
# release workflow's checkout fetched no tags; once that is fixed it will report
# the tag name directly. Both are correct answers to "am I on the right build",
# and this accepts whichever it is given.
SRC="${PTX_SRC:-/opt/hemis-ptx}"
VER="$(Hemisd -version 2>/dev/null | head -1)"
if [ -z "$VER" ]; then
    unk "could not read 'Hemisd -version' -- cannot confirm which build is installed"
elif [ ! -d "$SRC/.git" ]; then
    unk "no source checkout at $SRC, so the binary cannot be compared to it (set PTX_SRC=<dir>). Reported: $VER"
else
    WANT_C="$(git -c safe.directory="$SRC" -C "$SRC" rev-parse --short HEAD 2>/dev/null)"
    WANT_T="$(git -c safe.directory="$SRC" -C "$SRC" describe --tags --abbrev=0 2>/dev/null)"
    if [ -n "$WANT_C" ] && printf '%s' "$VER" | grep -q -- "$WANT_C"; then
        ok "running $VER -- matches the source at $SRC (commit $WANT_C${WANT_T:+, tag $WANT_T})"
    elif [ -n "$WANT_T" ] && printf '%s' "$VER" | grep -q -- "$WANT_T"; then
        ok "running $VER -- matches the source at $SRC (tag $WANT_T)"
    else
        bad "BUILD MISMATCH. Binary reports '$VER', but the source at $SRC is ${WANT_T:-<no tag>} / $WANT_C. You are not running what you installed -- rebuild or reinstall before trusting anything below."
    fi
fi

# ---------------------------------------------------------------------------
say "0b. Role -- is this machine configured as the thing you meant to build?"
# ★★ THE GAP THIS CLOSES is the same one section 0 closes for the binary: a
# config CONTAINING a line is not the same as the daemon BEHAVING that way, and
# a wrong role installs perfectly cleanly. A gamemaster host built as a wallet
# registers, syncs, reports ENABLED and SILENTLY NEVER SIGNS -- there is no
# error anywhere, which is precisely why it needs an assertion rather than a
# reader's attention.
ROLE_CONF="$DATADIR/Hemis.conf"
if [ ! -f "$ROLE_CONF" ]; then
    unk "no $ROLE_CONF, so the role cannot be checked"
else
    _listen="$(grep -cE '^listen=1' "$ROLE_CONF" 2>/dev/null)"
    _extip="$(grep -cE '^externalip=' "$ROLE_CONF" 2>/dev/null)"
    _declared="$(grep -oE '^# ROLE: [a-z]+' "$ROLE_CONF" 2>/dev/null | awk '{print $3}')"
    if [ -z "$_declared" ]; then
        unk "this config predates the PTX_ROLE toggle (no '# ROLE:' line) -- re-run install.sh to stamp it, or check listen/externalip by hand"
    elif [ "$_declared" = "gamemaster" ]; then
        # ★★ TWO externalip LINES PASS A "-ge 1" TEST, AND THAT IS HOW THIS WAS MISSED.
        # v0.3.4's one-pager told operators to ADD `externalip=<address, bare, no
        # brackets>` to a config where install.sh had already written one. The result
        # is two lines: the daemon advertises one of them, and if it is not the one you
        # registered, the gamemaster syncs, reports Ready, and then refuses to arm with
        # "Local address ... does not match the address from ProTx" -- the same failure
        # family as a transposed port. A count check that accepts "1 or more" cannot
        # see it, which is why it did not.
        if [ "$_extip" -gt 1 ]; then
            bad "★ $_extip 'externalip=' lines in $ROLE_CONF, and there must be exactly one. install.sh writes it for you; a second added by hand is a contradiction, and the daemon advertises only one of the two. Delete the line you added, keep the installer's, restart, and re-check."
        elif [ "$_listen" -ge 1 ] && [ "$_extip" -ge 1 ]; then
            ok "role gamemaster, and the config matches it (listen=1, externalip set)"
        else
            bad "role says GAMEMASTER but listen=1 is $( [ "$_listen" -ge 1 ] && echo present || echo MISSING ) and externalip is $( [ "$_extip" -ge 1 ] && echo set || echo MISSING ). A gamemaster without both registers, shows ENABLED and never receives a signing request."
        fi
    elif [ "$_declared" = "wallet" ]; then
        # listen is NOT a discriminator: BOTH roles listen, deliberately, so that
        # a wallet host returns connectivity to a network with no DNS seed.
        # externalip is the discriminator -- it advertises an address for
        # REGISTRATION, and a wallet host registers nothing.
        if [ "$_extip" = "0" ]; then
            ok "role wallet, and the config matches it (no externalip; listen=1 is correct for both roles)"
        else
            bad "role says WALLET but this config sets externalip= -- that advertises an address for a machine that registers nothing. Re-run install.sh with PTX_ROLE=wallet."
        fi
    else
        bad "unrecognised role '\''$_declared'\'' in $ROLE_CONF"
    fi
fi

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

    # ★★ THE ONE-LINE VERDICT, AND IT WAS ONLY BEING ECHOED.
    # "Ready" is returned by CActiveDeterministicGamemasterManager::GetStatus()
    # (src/activegamemaster.cpp:60-71) and ONLY after every gate in Init() passes:
    # DIP3 active, listen=1, the ProTx on-chain, not PoSe-banned, an external
    # address discoverable in the registered family (:152-157), that address equal
    # to the ProTx address (:161-167), and a successful self-connect to the
    # registered service (:169-186). Nothing else this script checks covers the
    # last three. Printing it and moving on was the difference between "the node
    # is configured" and "the node is armed".
    STATE=$(jval status)
    if [ -z "$STATE" ]; then
        unk "getgamemasterstatus carried no 'status'. Whether this gamemaster is ARMED is UNKNOWN -- this is not a pass."
    elif [ "$STATE" = "Ready" ]; then
        ok "status: Ready -- this gamemaster is armed and the daemon agrees with its own ProTx"
    else
        case "$STATE" in
            "Waiting for ProTx"*)
                warn "status: $STATE. Normal for the first minutes after protx_register; re-run when it confirms." ;;
            "Gamemaster was PoSe banned")
                bad "status: $STATE. You are OUT of the eligible set and it does NOT clear by itself -- it needs an on-chain protx_update_service. See OPERATOR_GUIDE.md 'If your GM is PoSe-banned'." ;;
            "Error."*|*"external address"*)
                bad "status: $STATE. The daemon cannot agree with its own registration -- almost always a missing or wrong externalip= in Hemis.conf. See OPERATOR_GUIDE.md A3." ;;
            *)
                bad "status: $STATE. Not 'Ready', so this gamemaster is NOT armed and will not sign." ;;
        esac
    fi
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

    # ★★ THE ADVERTISED PORT MUST MATCH THE REGISTERED ONE, and this is the only
    # way externalip's FORM can be wrong. All three forms parse and store
    # identically -- bare, [bracketed], and [bracketed]:PORT -- because the port
    # defaults to `port=` (init.cpp:1418 looks it up with GetListenPort()). So a
    # check on the form would enforce a rule that does not exist. What CAN be
    # wrong is an explicit port that disagrees: [addr]:29996 advertises 29996.
    # ★ The daemon does catch that, at arming, comparing CService address AND
    # port (activegamemaster.cpp:159). This catches it BEFORE arming -- which
    # matters, because a node that has not registered yet never reaches that gate.
    if command -v python3 >/dev/null 2>&1; then
        ADV_PORT="$(cli getnetworkinfo | python3 -c "
import json,sys
try:
    d=json.load(sys.stdin)
    la=[a for a in d.get('localaddresses',[]) if ':' in str(a.get('address',''))]
    print(la[0]['port'] if la else '')
except Exception:
    print('')
" 2>/dev/null)"
        REG_PORT="${REGADDR##*:}"
        if [ -z "${ADV_PORT:-}" ]; then
            unk "the daemon advertises no IPv6 local address yet, so the advertised port could NOT be compared with the registered one. This is not a pass."
        elif [ "$ADV_PORT" = "$REG_PORT" ]; then
            ok "advertised port $ADV_PORT matches the registered address's port"
        else
            bad "★ you advertise port $ADV_PORT but registered port $REG_PORT. The gamemaster will refuse to arm (\"Local address ... does not match the address from ProTx\"). Check for an explicit port in externalip= that disagrees with port=."
        fi
    fi

    # ★★ THE GAP NEITHER SECTION 4 NOR 5 COULD SEE: "reachable, but only by half
    # the network". Both of those probe the LOCAL socket or this host's own view
    # of itself, so a node that binds correctly and answers its own probe still
    # passes while being invisible to every peer on the other address family.
    # Signing is point-to-point -- the caller dials the registered address and no
    # relay bridges it -- so the family you registered is the family you exist on.
    # See KDD-110.
    if ! command -v ip >/dev/null 2>&1; then
        unk "iproute2 absent -- could NOT verify this host owns the address you registered. This is not a pass."
    else
        # Routable global unicast only. ULA (fc00::/7) reports scope=global on
        # Linux and is NOT routable -- registering one produces a node nobody can
        # reach, which is exactly the failure this leg exists to name.
        HOST_V6=$(ip -o -6 addr show scope global 2>/dev/null \
                    | grep -viE '[[:space:]](temporary|deprecated|tentative)([[:space:]]|$)' \
                    | awk '{print $4}' | cut -d/ -f1 \
                    | grep -viE '^(fc|fd|fe80)' | grep -vxE '::1' | sort -u || true)
        HOST_V4=$(ip -o -4 addr show scope global 2>/dev/null \
                    | awk '{print $4}' | cut -d/ -f1 | sort -u || true)

        case "$REGHOST" in
            [Ff][CcDd]*) bad "★ You registered $REGHOST, which is a ULA (fc00::/7). Linux calls its scope 'global' but it is NOT routable -- no peer outside your own network can reach it. Register a real global IPv6 address." ;;
            [Ff][Ee]80*) bad "★ You registered $REGHOST, a link-local address. It is not reachable off this link." ;;
        esac

        if [ "$FAMILY" = 6 ]; then
            if [ -z "$HOST_V6" ]; then
                bad "★ FAMILY GAP: you registered an IPv6 address but this host has NO routable global IPv6 address. Peers cannot reach you and nothing local will show it -- the daemon binds and answers happily. Fix IPv6 on this host, or re-register at an address you actually have."
            elif printf '%s\n' "$HOST_V6" | grep -qxF "$REGHOST"; then
                ok "registered address $REGHOST is a routable IPv6 address on this host"
            else
                warn "registered $REGHOST is not among this host's addresses ($(printf '%s' "$HOST_V6" | tr '\n' ' '))."
                echo "         That is CORRECT behind NAT -- you register the router's address --"
                echo "         and WRONG if you meant to register this machine. Check which it is."
            fi
        else
            # Policy, not preference: signing is point-to-point and this network
            # routes it over IPv6. An IPv4 registration is invisible to it.
            bad "★ You registered an IPv4 address ($REGHOST). PTX gamemasters must register a global IPv6 address -- signing is point-to-point and no relay bridges address families, so an IPv4-registered gamemaster is invisible to the network while syncing and reporting Ready. See OPERATOR_GUIDE.md."
            [ -n "$HOST_V6" ] && echo "         This host HAS routable IPv6: $(printf '%s' "$HOST_V6" | tr '\n' ' ')-- re-register with it."
        fi
    fi
fi

# ---------------------------------------------------------------------------
say "5. ★ EXTERNAL reachability at the REGISTERED address (P2P)"
# ---------------------------------------------------------------------------
if [ -z "${REGADDR:-}" ]; then
    # ★ Not a pass. This is THE check the guide calls the silent killer, and it did
    # not run. Scoring it as a warning is how this script came to report a dead node
    # as healthy.
    unk "no registered address -- section 5 DID NOT RUN. This is not a pass. Re-run after protx_register, and fix section 3 first if it reported a problem."
else
    # ★ P2P ONLY. The RPC probe that used to sit beside this was deleted by
    # KDD-085 -- RPC is loopback-only now and has no remote caller, so a green
    # RPC probe would have been a true statement about an irrelevant port,
    # presented next to the one check the guide calls the silent killer.
    if probe "$REGHOST" "$P2P_PORT"; then
        ok "P2P port $P2P_PORT accepted a connection at $REGHOST"
    else
        bad "P2P port $P2P_PORT is NOT reachable at $REGHOST -- open it in your firewall AND your NAT / cloud security group. This is the port signing requests arrive on."
    fi
    # ★ And the inverse check, which is new and is the one that would catch a
    # config left over from the old model: RPC must NOT be reachable remotely.
    if probe "$REGHOST" "$RPC_PORT"; then
        bad "RPC port $RPC_PORT is REACHABLE at $REGHOST. It should be loopback-only (KDD-085). Your rpcbind lines still name a global address -- your credentials are exposed to anything that can route to you, for no benefit, since nothing dials RPC any more."
    else
        ok "RPC port $RPC_PORT is not reachable remotely -- correct, it is a local admin interface"
    fi
    cat <<'NOTE'

  ★ WHAT THIS TEST DOES AND DOES NOT PROVE.
    It connects FROM THIS MACHINE to your own registered address. On many NAT
    setups that succeeds via hairpin routing even when NOBODY OUTSIDE can reach
    you, and on a few it fails even when outsiders can. So:
      - a FAIL here is a reason to CHECK FROM OUTSIDE before changing anything.
        Some setups fail this self-probe while outsiders reach you perfectly
        well. Ask another operator to connect, or use an external port checker,
        BEFORE you edit config or firewall rules -- a coordinator host has
        failed this test while two other hosts reached its P2P port fine.
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
# ★★ AND WHAT A ZERO CANNOT PROVE. PoSe penalty has exactly ONE increment site:
# evo/deterministicgms.cpp:828, for a member marked invalid in a SUCCESSFUL LLMQ
# final commitment. A commitment needs minSize=2 valid members
# (llmq/quorums_commitment.cpp:71) and a session below minSize aborts outright
# (llmq/quorums_dkgsession.cpp:98), with null commitments skipped entirely
# (evo/deterministicgms.cpp:769). So below THREE registered gamemasters the
# penalty is structurally incapable of moving, and "0" is a guaranteed pass that
# proves nothing at all. Say so, rather than letting an early operator read it as
# evidence that peers can reach them.
# protx_list takes POSITIONAL booleans (detailed wallet_only valid_only height,
# rpc/rpcevo.cpp:874-876) -- "protx_list valid_only" throws on get_bool. Ask for
# the undetailed, valid-only list and count the txids.
GMCOUNT=$(cli protx_list false false true 2>/dev/null | grep -c '"[0-9a-f]\{64\}"' || echo 0)
PENALTY=$(jnum PoSePenalty)
BANHEIGHT=$(jnum PoSeBanHeight)
if [ -n "$PENALTY" ]; then
    if [ "$PENALTY" -eq 0 ]; then
        if [ "${GMCOUNT:-0}" -lt 3 ] 2>/dev/null; then
            unk "PoSe penalty 0, but there are only ${GMCOUNT:-?} registered gamemasters. Below three, this number CANNOT move (it only changes on a failed LLMQ session, which needs two other members), so it proves nothing yet. It is not evidence that peers can reach you -- section 5 and another operator connecting to you are."
        else
            ok "PoSe penalty 0 across ${GMCOUNT} registered gamemasters -- the network is reaching you"
        fi
    else
        bad "PoSe penalty $PENALTY (non-zero). The network is FAILING to reach you. Re-read section 5; trust this over anything section 5 said."
    fi
    if [ -n "$BANHEIGHT" ] && [ "$BANHEIGHT" -ge 0 ]; then
        bad "PoSe BANNED at height $BANHEIGHT. This does NOT decay back on its own: the ban is cleared only by an on-chain protx_update_service (evo/deterministicgms.cpp:693-700). See OPERATOR_GUIDE.md 'If your GM is PoSe-banned'."
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
