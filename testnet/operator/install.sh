#!/usr/bin/env bash
# PTX testnet — node installer.
#
# Written to be followed LITERALLY. Where a step needs knowledge you may not
# have, it says so out loud instead of assuming you have it.
#
# This installs the NODE half only. The WALLET half (collateral, protx_register)
# runs on a DIFFERENT machine and is covered in OPERATOR_GUIDE.md.
set -euo pipefail

REPO="${PTX_REPO:-https://github.com/vileda-hemis/Hemis-PTX-MVP1.git}"
REF="${PTX_REF:-}"                       # set to a tag/commit to pin; empty = default branch
PREFIX="${PTX_PREFIX:-/opt/hemis-ptx}"
DATADIR="${PTX_DATADIR:-$HOME/.hemis-ptxtestnet}"
P2P_PORT=29994
RPC_PORT=29995

say()  { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  [ok]   %s\n' "$*"; }
warn() { printf '  [WARN] %s\n' "$*"; }
die()  { printf '\n  [FAIL] %s\n\n' "$*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 1. Environment checks.
#
# We check GLIBC VERSION and CPU ARCHITECTURE, deliberately NOT the distro name.
# "Ubuntu 22.04" tells you nothing portable -- Debian, Mint, Pop!_OS and dozens
# of others work fine, and a distro-name check rejects them for no reason while
# still passing on a distro whose glibc is too old. The binary's real
# requirement is the glibc soname and the instruction set.
# ---------------------------------------------------------------------------
say "1. Environment"

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64) ok "architecture $ARCH" ;;
    aarch64|arm64) ok "architecture $ARCH" ;;
    *) die "unsupported architecture '$ARCH' (need x86_64 or aarch64). You would have to build from source for this CPU." ;;
esac

# ldd --version prints the glibc version on its FIRST line, last field.
GLIBC="$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')"
[ -n "$GLIBC" ] || die "could not determine glibc version ('ldd --version' produced nothing). If this is Alpine/musl, these binaries will not run."
GLIBC_MAJOR="${GLIBC%%.*}"; GLIBC_MINOR="${GLIBC#*.}"; GLIBC_MINOR="${GLIBC_MINOR%%.*}"
if [ "$GLIBC_MAJOR" -gt 2 ] || { [ "$GLIBC_MAJOR" -eq 2 ] && [ "$GLIBC_MINOR" -ge 31 ]; }; then
    ok "glibc $GLIBC (need >= 2.31)"
else
    die "glibc $GLIBC is too old (need >= 2.31). Upgrade the OS or build from source."
fi

for tool in git curl awk sed; do
    command -v "$tool" >/dev/null 2>&1 || die "missing required tool: $tool"
done
ok "required tools present"

MEM_MB=$(awk '/^MemTotal:/{printf "%d", $2/1024}' /proc/meminfo)
[ "$MEM_MB" -ge 2000 ] || warn "only ${MEM_MB}MiB RAM detected; 2GiB+ recommended"
DISK_GB=$(df -BG --output=avail "$HOME" | tail -1 | tr -dc '0-9')
[ "${DISK_GB:-0}" -ge 10 ] || warn "only ${DISK_GB}G free in $HOME; 10G+ recommended"

# ---------------------------------------------------------------------------
# 2. Fetch and build.
# ---------------------------------------------------------------------------
say "2. Source"
if [ -d "$PREFIX/.git" ]; then
    ok "existing checkout at $PREFIX -- updating"
    git -C "$PREFIX" fetch --all --tags --quiet
else
    sudo mkdir -p "$PREFIX"
    sudo chown "$(id -u):$(id -g)" "$PREFIX"
    git clone --quiet "$REPO" "$PREFIX"
    ok "cloned $REPO"
fi
if [ -n "$REF" ]; then
    git -C "$PREFIX" checkout --quiet "$REF"
    ok "pinned to $REF"
else
    warn "PTX_REF not set -- using the default branch. For a launch you should pin a tag."
fi
echo "  commit: $(git -C "$PREFIX" rev-parse --short HEAD)"

# ---------------------------------------------------------------------------
# 3. Kernel port reservation.
#
# PTX uses ports in 32000-33000 for its fan-out. Without reserving them the
# kernel can hand one out as an EPHEMERAL source port for an unrelated outgoing
# connection, and the PTX listener then fails to bind -- intermittently, under
# load, which is the worst way to find out.
#
# ★ We APPEND to a dedicated file and never touch existing entries. Writing
# net.ipv4.ip_local_reserved_ports wholesale would silently discard any
# reservation another service already made.
# ---------------------------------------------------------------------------
say "3. Port reservation"
SYSCTL_FILE=/etc/sysctl.d/99-ptx-fleet-ports.conf
WANT="32000-33000"
CURRENT="$(cat /proc/sys/net/ipv4/ip_local_reserved_ports 2>/dev/null || echo "")"

if printf '%s' "$CURRENT" | grep -q "$WANT"; then
    ok "reservation $WANT already active"
elif [ -n "$CURRENT" ]; then
    # Something else reserved ports. Preserve theirs, add ours.
    MERGED="$CURRENT,$WANT"
    warn "existing reservation '$CURRENT' found -- MERGING, not replacing"
    echo "net.ipv4.ip_local_reserved_ports=$MERGED" | sudo tee "$SYSCTL_FILE" >/dev/null
    sudo sysctl -q -p "$SYSCTL_FILE"
    ok "reserved $MERGED"
else
    echo "net.ipv4.ip_local_reserved_ports=$WANT" | sudo tee "$SYSCTL_FILE" >/dev/null
    sudo sysctl -q -p "$SYSCTL_FILE"
    ok "reserved $WANT"
fi
echo "  now active: $(cat /proc/sys/net/ipv4/ip_local_reserved_ports)"

# ---------------------------------------------------------------------------
# 4. Configuration.
#
# ★ rpcbind is DUAL-STACK on purpose: 0.0.0.0 covers IPv4, :: covers IPv6.
# Binding only one family is the exact seam that makes a node look healthy
# on-chain while never receiving a sign request -- see self-check.sh.
#
# ★ RPC MUST be reachable by your quorum peers. PTX fan-out dials each member's
# RPC directly; a firewalled RPC port means you are selected, never contacted,
# and silently never sign. This is why rpcallowip is not localhost-only.
# ---------------------------------------------------------------------------
say "4. Configuration"
mkdir -p "$DATADIR"
CONF="$DATADIR/hemis.conf"
if [ -f "$CONF" ]; then
    warn "$CONF already exists -- leaving it alone. Compare it against the template printed below."
else
    RPCUSER="ptxop"
    RPCPASS="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    cat > "$CONF" <<EOF
# PTX testnet node configuration.
ptxtestnet=1

# --- RPC -------------------------------------------------------------------
rpcuser=$RPCUSER
rpcpassword=$RPCPASS
rpcport=$RPC_PORT
# Dual-stack: IPv4 and IPv6. Binding one family only is a silent failure mode.
rpcbind=0.0.0.0
rpcbind=::
# Your quorum peers must reach this RPC. Narrow these to peer addresses once
# you know them; leaving it wide open on a public IP is NOT acceptable long-term.
rpcallowip=127.0.0.1
rpcallowip=::1
# rpcallowip=<peer-address>/32     <-- add one line per peer

# --- P2P -------------------------------------------------------------------
port=$P2P_PORT
listen=1

# --- Node role -------------------------------------------------------------
gamemaster=1
# gamemasterblsprivkey=<the BLS key you generate in the OPERATOR_GUIDE>
EOF
    chmod 600 "$CONF"
    ok "wrote $CONF (mode 600)"
    warn "RPC password was generated for you; it is in $CONF. Do not paste it into chat or tickets."
fi

say "Done"
cat <<EOF
  Config:  $CONF
  Datadir: $DATADIR
  P2P:     $P2P_PORT      RPC: $RPC_PORT

  NEXT, in order:
    1. Open $P2P_PORT and $RPC_PORT in your firewall AND any NAT/cloud security group.
    2. Follow OPERATOR_GUIDE.md section "Node side" to generate your BLS key and
       send the PUBLIC half to the wallet operator.
    3. Start the daemon, then run:  ./self-check.sh
EOF
