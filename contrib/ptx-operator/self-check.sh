#!/usr/bin/env bash
# ptxbea gamemaster self-check — runs at the end of install and standalone after.
#
# Verifies the states that look HEALTHY from some angles but silently fail:
# registered-but-unreachable (the IPv6 seam — RPC bound to localhost/firewalled,
# node registers and looks Ready on-chain, yet fails every signing request) and
# registered-but-unarmed. Plain PASS/FAIL/WARN per check with a one-line remedy.
#
# The single most valuable check here is EXTERNAL RPC REACHABILITY — see §RPC.
#
# Exit: 0 if no FAILs (WARN allowed), 1 if any FAIL.
set -u

CLI="${HEMIS_CLI:-hemis-cli} -ptxbea"
P2P_PORT=29994
RPC_PORT=29995

pass=0 fail=0 warn=0
ok()   { printf '  \033[32mPASS\033[0m  %s\n' "$1"; pass=$((pass+1)); }
bad()  { printf '  \033[31mFAIL\033[0m  %s\n         ↳ remedy: %s\n' "$1" "$2"; fail=$((fail+1)); }
warns(){ printf '  \033[33mWARN\033[0m  %s\n         ↳ %s\n' "$1" "$2"; warn=$((warn+1)); }

# JSON helper — python3 is a hard prerequisite (checked by install.sh env step).
jget() { python3 -c 'import sys,json
try: d=json.load(sys.stdin)
except Exception: sys.exit(2)
k=sys.argv[1]
for p in k.split("."):
    if isinstance(d,dict): d=d.get(p)
    else: d=None
print("" if d is None else d)' "$1" 2>/dev/null; }

echo "=== ptxbea gamemaster self-check ==="

# ── 1. daemon responding ────────────────────────────────────────────────────
if ! $CLI getblockcount >/dev/null 2>&1; then
  bad "daemon not responding to RPC on this host" \
      "start it: 'Hemisd -ptxbea -daemon', then wait for load and re-run this check"
  echo; echo "cannot continue without a responding daemon."; exit 1
fi
ok "daemon responding to local RPC"

# ── 2. synced to the network ────────────────────────────────────────────────
CHAIN=$($CLI getblockchaininfo 2>/dev/null)
# PIVX-lineage field name: initial_block_downloading (NOT bitcoin's
# initialblockdownload — probing the wrong key WARNs on every healthy node;
# caught live against gm25 during the first run of this script).
IBD=$(printf '%s' "$CHAIN" | jget initial_block_downloading)
PROG=$(printf '%s' "$CHAIN" | jget verificationprogress)
HEIGHT=$(printf '%s' "$CHAIN" | jget blocks)
if [ "$IBD" = "False" ] || [ "$IBD" = "false" ]; then
  ok "chain synced (height ${HEIGHT:-?}, progress ${PROG:-?})"
else
  warns "still syncing (IBD, height ${HEIGHT:-?})" \
        "wait for sync to finish; a syncing node cannot sign or be judged Ready"
fi

# ── 3. peer connectivity ────────────────────────────────────────────────────
PEERS=$($CLI getconnectioncount 2>/dev/null)
if [ "${PEERS:-0}" -ge 1 ] 2>/dev/null; then
  ok "connected to ${PEERS} peer(s)"
else
  bad "no peers connected" \
      "check outbound network / -addnode seeds; a partitioned node drifts and cannot sign"
fi

# ── 4. registered on-chain + the DGM entry matches this node ─────────────────
GMS=$($CLI getgamemasterstatus 2>/dev/null)
if [ -z "$GMS" ]; then
  bad "this node is not registered as a gamemaster (getgamemasterstatus empty/errored)" \
      "complete registration: protx_register (see OPERATOR_GUIDE.md §Register)"
else
  REG_ADDR=$(printf '%s' "$GMS" | jget netaddr)   # host:port the DGM advertises
  ok "registered on-chain — DGM advertises ${REG_ADDR:-?}"
  # the advertised P2P port must be the standard one, or peers/fan-out miss it
  case "$REG_ADDR" in
    *:$P2P_PORT) : ;;
    "" ) : ;;
    * ) warns "DGM-advertised port is not :$P2P_PORT (${REG_ADDR})" \
              "re-register with the standard P2P port $P2P_PORT, or peers may not reach you" ;;
  esac
fi

# ── 5. armed (operator key loaded, Ready) ───────────────────────────────────
STATUS=$(printf '%s' "$GMS" | jget status)
if [ "$STATUS" = "Ready" ]; then
  ok "armed and Ready (operator key loaded)"
elif [ -n "$GMS" ]; then
  bad "registered but NOT Ready (status: ${STATUS:-unknown})" \
      "load the operator BLS key: set gamemasterblsprivkey in the config and restart (see §Arm)"
fi

# ── RPC: EXTERNAL REACHABILITY — the single most valuable check ──────────────
# A GM whose RPC is bound to localhost, firewalled, or on the wrong address
# family REGISTERS FINE and looks Ready, then fails every signing request. The
# fan-out reaches you at your REGISTERED address on RPC $RPC_PORT.
REG_HOST=""
[ -n "${REG_ADDR:-}" ] && REG_HOST="${REG_ADDR%:*}"

# 5a. LOCAL, DEFINITIVE: is the RPC listener bound beyond loopback?
BINDS=$(ss -ltnH "sport = :$RPC_PORT" 2>/dev/null | awk '{print $4}')
if [ -z "$BINDS" ]; then
  bad "nothing is listening on RPC port $RPC_PORT" \
      "set rpcport=$RPC_PORT and rpcbind=[::] in the config and restart"
elif printf '%s\n' "$BINDS" | grep -qvE '^(127\.0\.0\.1|\[?::1\]?):'; then
  ok "RPC listener bound beyond loopback ($(printf '%s' "$BINDS" | tr '\n' ' '))"
else
  bad "RPC is bound to LOCALHOST ONLY ($BINDS) — the signing fan-out cannot reach it" \
      "set rpcbind=[::] (or your registered address) + rpcallowip for the peer range, restart"
fi

# 5b. BEST-EFFORT from this host (may hairpin — not definitive on its own).
if [ -n "$REG_HOST" ]; then
  if timeout 5 bash -c "exec 3<>/dev/tcp/${REG_HOST}/${RPC_PORT}" 2>/dev/null; then
    ok "TCP connect to registered RPC ${REG_HOST}:${RPC_PORT} succeeded (best-effort — see §RPC)"
    exec 3>&- 2>/dev/null || true
  else
    bad "cannot TCP-connect to your registered RPC ${REG_HOST}:${RPC_PORT}" \
        "open $RPC_PORT in the firewall to the peer range and bind RPC to $REG_HOST / [::]"
  fi
fi

# 5c. DEFINITIVE backstop — the operator MUST confirm from another host, because
# a same-host probe can hairpin-NAT back to loopback and false-pass.
echo "  ---- verify RPC reachability FROM ANOTHER HOST (definitive) ----"
echo "       run this on a DIFFERENT machine (needs your rpcuser/rpcpassword):"
echo "         hemis-cli -ptxbea -rpcconnect=${REG_HOST:-<your-registered-ip>} \\"
echo "                   -rpcport=${RPC_PORT} -rpcuser=<u> -rpcpassword=<p> getblockcount"
echo "       a timeout/refusal here = the silent-signing-failure state, even though"
echo "       this node looks healthy on-chain. (Peer-assisted auto-probe: owed, KDD-085."
echo "        Until then this manual from-another-host check is the definitive one.)"

# ── 6. P2P reachability (best-effort) ───────────────────────────────────────
if [ -n "$REG_HOST" ]; then
  if timeout 5 bash -c "exec 3<>/dev/tcp/${REG_HOST}/${P2P_PORT}" 2>/dev/null; then
    ok "TCP connect to registered P2P ${REG_HOST}:${P2P_PORT} succeeded"
    exec 3>&- 2>/dev/null || true
  else
    warns "cannot TCP-connect to registered P2P ${REG_HOST}:${P2P_PORT} from here" \
          "open $P2P_PORT in the firewall; confirm from another host (P2P is definitionally public)"
  fi
fi

# ── 7. shares held for every quorum this node is a member of ────────────────
QH=$($CLI ptx_quorum_health 2>/dev/null)
if [ -z "$QH" ]; then
  warns "ptx_quorum_health returned nothing" \
        "expected on a pre-share-health binary; upgrade to surface share health"
else
  MEMBER_OF=$(printf '%s' "$QH" | jget member_of)
  CAPABLE=$(printf '%s' "$QH" | jget capable)
  if [ "${MEMBER_OF:-0}" = "0" ]; then
    ok "not currently a member of any quorum (nothing to hold — normal until selected)"
  elif [ "${CAPABLE:-0}" = "${MEMBER_OF:-0}" ]; then
    ok "holding a current share for all ${MEMBER_OF} quorum(s) this node is a member of"
  else
    bad "member of ${MEMBER_OF} quorum(s) but only ${CAPABLE} have a current share" \
        "a lost share is NOT recoverable (ODC-071); keep the node Ready — it re-shares at next rotation"
  fi
fi

# ── 8. ports reserved against ephemeral allocation ──────────────────────────
RES=$(sysctl -n net.ipv4.ip_local_reserved_ports 2>/dev/null || true)
if printf ',%s,' "$RES" | grep -q ",$RPC_PORT," && printf ',%s,' "$RES" | grep -q ",$P2P_PORT,"; then
  ok "ports $P2P_PORT,$RPC_PORT reserved against ephemeral allocation"
else
  warns "ports $P2P_PORT,$RPC_PORT NOT in ip_local_reserved_ports" \
        "run install.sh (or write /etc/sysctl.d/99-hemis-ptx.conf) — a reboot can hand your port to an outbound connection first"
fi

echo
echo "=== $pass passed, $warn warning(s), $fail failure(s) ==="
[ "$fail" -eq 0 ] || exit 1
