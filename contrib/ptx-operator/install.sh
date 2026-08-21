#!/usr/bin/env bash
#
# ★★ SUPERSEDED -- DO NOT SEND AN OPERATOR HERE.
#
# This is the FIRST-GENERATION ptxbea installer. The live one is
# testnet/operator/install.sh, and the difference is not cosmetic:
# this file writes "hemis.conf" LOWERCASE, which the daemon never opens
# (util/system.cpp:81), writes no [ptxtestnet] section, so every port line in it
# is silently dropped, and installs no Sapling parameters, without which the
# daemon exits 1 at startup. Those are the defects fixed in f37bf34 and e414e77
# -- in testnet/operator/ ONLY. Nothing here was fixed, because nothing here is
# on the path any operator should be taking.
#
# Kept for the ptxbea history it records. doc/ptx/OPERATOR_GUIDE.md, which is the
# only document that points at this file, now carries the same warning.
#
# ptxbea gamemaster installer — one command from a fresh machine to a running,
# registered, armed GM. Mirrors the familiar Hemis release-zip install, adding
# what a signing gamemaster needs: checksum verification, the standard ptxbea
# ports, ephemeral-port reservation, guided registration/arm, and a self-check.
#
# Fail EARLY and LOUDLY: the registered-but-unarmed and registered-but-unreachable
# states both look healthy on-chain and silently fail signing. We refuse to
# continue into them.
#
# Usage:
#   install.sh [--release-url URL] [--sha256 HASH] [--datadir DIR] [--yes]
#   install.sh --build            # build from source instead of fetching a release
#   install.sh --self-check-only  # just run the self-check
set -euo pipefail

# ── constants ───────────────────────────────────────────────────────────────
P2P_PORT=29994
RPC_PORT=29995                       # = P2P+1, below the ephemeral range (see OPERATOR_GUIDE §Ports)
REQUIRED_GLIBC="2.31"                # the release is built on Debian glibc; older won't run it
REQUIRED_ARCH="x86_64"
MIN_DISK_GB=20
INSTALL_DIR="${INSTALL_DIR:-/usr/local/bin}"
DATADIR="${HOME}/.hemis-ptxbea"
SYSCTL_FILE="/etc/sysctl.d/99-hemis-ptx.conf"
REPO="https://github.com/vileda-hemis/Hemis-PTX-MVP1"
# ★ No PTX release is published yet (the fleet builds its own image). Set these
# when a release with a published SHA256SUMS exists, or pass --release-url/--sha256,
# or use --build. We NEVER fetch an unverified binary.
RELEASE_URL="${RELEASE_URL:-}"
RELEASE_SHA256="${RELEASE_SHA256:-}"

DO_BUILD=0 ASSUME_YES=0 SELF_CHECK_ONLY=0
while [ $# -gt 0 ]; do case "$1" in
  --release-url) RELEASE_URL="$2"; shift 2;;
  --sha256)      RELEASE_SHA256="$2"; shift 2;;
  --datadir)     DATADIR="$2"; shift 2;;
  --build)       DO_BUILD=1; shift;;
  --yes|-y)      ASSUME_YES=1; shift;;
  --self-check-only) SELF_CHECK_ONLY=1; shift;;
  *) echo "unknown arg: $1" >&2; exit 2;;
esac; done

die()  { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; exit 1; }
info() { printf '\033[36m==>\033[0m %s\n' "$*"; }
ok()   { printf '  \033[32m✓\033[0m %s\n' "$*"; }
ask()  { [ "$ASSUME_YES" = 1 ] && return 0; read -r -p "  $1 [y/N] " a; [ "$a" = y ] || [ "$a" = Y ]; }
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ── --self-check-only shortcut ──────────────────────────────────────────────
if [ "$SELF_CHECK_ONLY" = 1 ]; then exec bash "$here/self-check.sh"; fi

# ── 1. ENVIRONMENT — fail early with useful messages ────────────────────────
info "Environment checks"

arch="$(uname -m)"
[ "$arch" = "$REQUIRED_ARCH" ] || die "architecture is '$arch', the release is $REQUIRED_ARCH only.
       If you are on ARM64, build from source (--build) — a wrong-arch binary fails with a cryptic error."
ok "architecture $arch"

# glibc by VERSION, not distro name — a newer-glibc binary won't run on older.
glibc="$(ldd --version 2>/dev/null | head -1 | grep -oE '[0-9]+\.[0-9]+' | head -1 || true)"
[ -n "$glibc" ] || die "cannot determine glibc version (ldd missing?)."
if [ "$(printf '%s\n%s\n' "$REQUIRED_GLIBC" "$glibc" | sort -V | head -1)" != "$REQUIRED_GLIBC" ]; then
  die "glibc $glibc is older than the required $REQUIRED_GLIBC. The prebuilt binary will not run
       ('version GLIBC_x.y not found'). Build from source on this host instead: --build."
fi
ok "glibc $glibc (>= $REQUIRED_GLIBC)"

avail_gb="$(df -Pk "$HOME" | awk 'NR==2{print int($4/1024/1024)}')"
[ "${avail_gb:-0}" -ge "$MIN_DISK_GB" ] || die "only ${avail_gb}GB free under $HOME; need >= ${MIN_DISK_GB}GB (chain + shares grow)."
ok "disk ${avail_gb}GB free"

for t in python3 curl awk sort ss; do command -v "$t" >/dev/null 2>&1 || die "missing prerequisite: $t"; done
ok "prerequisites present (python3, curl, ss, …)"

# Docker alternative — a much simpler question than distro/glibc.
if command -v docker >/dev/null 2>&1; then
  info "Docker is available on this host."
  echo "     If native install gives you trouble, the project ships container images that avoid"
  echo "     the glibc/arch question entirely. See OPERATOR_GUIDE.md §Container. Continuing native."
fi

# ── 2. IDEMPOTENCY — detect an existing install ─────────────────────────────
if [ -f "$DATADIR/ptxbea/wallet.dat" ] || [ -f "$DATADIR/wallet.dat" ]; then
  info "Existing datadir found at $DATADIR"
  echo "     Re-running is safe: this will NOT touch your wallet.dat or ptx_shares.dat."
  ask "Continue and re-verify config + reservation + self-check?" || die "aborted by operator (nothing changed)."
fi

# ── 3. FETCH THE BINARY (checksum-verified) OR BUILD ────────────────────────
if [ "$DO_BUILD" = 1 ]; then
  info "Building from source ($REPO)"
  command -v git >/dev/null || die "git required for --build"
  workdir="$(mktemp -d)"; git clone --depth 1 "$REPO" "$workdir/src"
  echo "     Build deps + steps: see $REPO/doc/build-unix.md. This can take 20–40 min."
  ask "Run the autogen+configure+make now?" || die "aborted before build."
  ( cd "$workdir/src/src/hemisd" && ./autogen.sh && ./configure --without-gui && make -j"$(nproc)" )
  sudo install -m0755 "$workdir/src/src/hemisd/src/Hemisd" "$workdir/src/src/hemisd/src/Hemis-cli" "$INSTALL_DIR/"
  ok "built and installed Hemisd, Hemis-cli to $INSTALL_DIR"
elif command -v Hemisd >/dev/null 2>&1 && command -v Hemis-cli >/dev/null 2>&1; then
  ok "Hemisd/Hemis-cli already on PATH — skipping fetch (use --build to rebuild)"
else
  [ -n "$RELEASE_URL" ] || die "no binary on PATH and no --release-url given.
       No PTX release is published yet. Either:
         • build on this host:            install.sh --build
         • or pass a verified release:    install.sh --release-url URL --sha256 HASH
       We refuse to fetch an unverified binary."
  [ -n "$RELEASE_SHA256" ] || die "--release-url requires --sha256 (we never install an unverified binary)."
  info "Fetching release from $RELEASE_URL"
  tmp="$(mktemp -d)"; curl -fSL "$RELEASE_URL" -o "$tmp/hemis-ptx.zip"
  got="$(sha256sum "$tmp/hemis-ptx.zip" | awk '{print $1}')"
  [ "$got" = "$RELEASE_SHA256" ] || die "SHA256 MISMATCH — refusing to install.
       expected $RELEASE_SHA256
       got      $got"
  ok "checksum verified"
  command -v unzip >/dev/null || { info "installing unzip"; sudo apt-get update -qq && sudo apt-get install -y unzip; }
  sudo unzip -o "$tmp/hemis-ptx.zip" -d "$INSTALL_DIR" >/dev/null
  ok "installed to $INSTALL_DIR"
fi
command -v Hemisd >/dev/null || die "Hemisd not found on PATH after install ($INSTALL_DIR on PATH?)."

# ── 4. HOST CONFIG — reserve ports against ephemeral allocation (append-safe) ─
info "Reserving ports $P2P_PORT,$RPC_PORT against ephemeral allocation"
# net.ipv4.* here is a historical name — it governs BOTH IPv4 and IPv6. One line
# covers both stacks. We reserve even though 29994/5 are below the default range:
# the range is configurable and hardened hosts widen it. Costs nothing.
cur="$(sysctl -n net.ipv4.ip_local_reserved_ports 2>/dev/null | tr -d '[:space:]' || true)"
need="$P2P_PORT,$RPC_PORT"
add=""
for p in $P2P_PORT $RPC_PORT; do
  case ",$cur," in *",$p,"*) : ;; *) add="${add:+$add,}$p";; esac
done
if [ -z "$add" ]; then
  ok "already reserved (ip_local_reserved_ports contains $need)"
else
  # Merge — NEVER clobber an existing reservation (it is a comma-list others use).
  merged="${cur:+$cur,}$add"
  if [ -n "$cur" ]; then
    info "existing reservation present: '$cur' — appending $add (not overwriting)"
  fi
  printf '# Hemis ptxbea gamemaster — reserve P2P/RPC so an outbound connection\n' | sudo tee "$SYSCTL_FILE" >/dev/null
  printf '# cannot grab them as a source port before the daemon binds (the caller7 race).\n' | sudo tee -a "$SYSCTL_FILE" >/dev/null
  printf 'net.ipv4.ip_local_reserved_ports = %s\n' "$merged" | sudo tee -a "$SYSCTL_FILE" >/dev/null
  sudo sysctl -p "$SYSCTL_FILE" >/dev/null
  ok "reserved $merged (persisted in $SYSCTL_FILE — survives reboot, which is when the race bites)"
fi

# ── 5. CONFIG — standard ports, dual-stack RPC bind ─────────────────────────
info "Writing ptxbea config"
mkdir -p "$DATADIR"
CONF="$DATADIR/hemis.conf"
if [ -f "$CONF" ] && ! ask "config exists at $CONF — overwrite (keeps wallet/shares)?"; then
  ok "kept existing config"
else
  # rpcauth: generate a credential if none set. rpcbind=[::] is dual-stack; the
  # fan-out reaches this node's RPC at its REGISTERED address on $RPC_PORT.
  rpcpw="$(head -c18 /dev/urandom | base64 | tr -dc 'a-zA-Z0-9')"
  cat > "$CONF" <<EOF
# Hemis ptxbea gamemaster config — generated by install.sh
daemon=1
listen=1
server=1
port=$P2P_PORT
rpcport=$RPC_PORT
# RPC MUST be reachable by the signing fan-out at your REGISTERED address, not
# just localhost — otherwise the node looks healthy on-chain and never signs.
rpcbind=[::]
rpcbind=0.0.0.0
rpcallowip=127.0.0.1
# ▲ RESTRICT this to your peer/fleet range for production; 0.0.0.0/0 is unsafe.
rpcuser=ptxop
rpcpassword=$rpcpw
# externalip: set to the PUBLIC address you register (uncomment + fill):
# externalip=YOUR.PUBLIC.IP
# gamemasterblsprivkey: the operator BLS key that ARMS this node (see §Arm):
# gamemasterblsprivkey=YOUR_OPERATOR_BLS_PRIVKEY
EOF
  chmod 600 "$CONF"
  ok "wrote $CONF (ports $P2P_PORT/$RPC_PORT, dual-stack RPC, mode 600)"
  echo "     ▲ EDIT $CONF: set externalip to your public IP, tighten rpcallowip,"
  echo "       and add gamemasterblsprivkey once you generate the operator key (§Arm)."
fi

# ── 6. COLLATERAL, KEYS, REGISTER, ARM — guided (needs operator inputs) ──────
cat <<EOF

$(info "Next: collateral, register, arm  — these need YOUR inputs; see OPERATOR_GUIDE.md")
  1. Fund the COLLATERAL: send exactly the collateral amount to an address in this
     wallet; note its txid and output index (protx_register needs them).
  2. Generate the OPERATOR BLS key:   Hemis-cli -ptxbea bls generate
     put the SECRET half in $CONF as gamemasterblsprivkey, keep the PUBLIC half for step 3.
  3. REGISTER (ProRegTx):             Hemis-cli -ptxbea protx_register \\
        <collateralHash> <collateralIndex> <YOUR.PUBLIC.IP:$P2P_PORT> \\
        <ownerAddr> <operatorPubKey> <votingAddr> <payoutAddr>
  4. ARM: ensure gamemasterblsprivkey is in $CONF, restart the daemon.

  ★ SHARES: after you are selected into a quorum, ptx_shares.dat appears in the
    datadir. Treat it like wallet.dat — it is SECRET and NOT recoverable. A datadir
    backup taken BEFORE a ceremony forfeits those shares permanently (ODC-071).
EOF

# ── 7. START + SELF-CHECK ───────────────────────────────────────────────────
if ask "Start the daemon now?"; then
  Hemisd -ptxbea -datadir="$DATADIR" >/dev/null 2>&1 || true
  info "daemon starting; waiting for RPC…"
  for _ in $(seq 1 30); do Hemis-cli -ptxbea -datadir="$DATADIR" getblockcount >/dev/null 2>&1 && break; sleep 2; done
fi

info "Running self-check"
HEMIS_CLI="Hemis-cli" bash "$here/self-check.sh" || {
  echo
  echo "Self-check reported failures above. Fix them before relying on this node —"
  echo "a GM that is registered but unarmed or unreachable looks healthy and never signs."
  exit 1
}
info "Install complete. Re-run the self-check any time:  bash $here/self-check.sh"
