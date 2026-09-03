#!/usr/bin/env bash
#
# PTX testnet — VPS bootstrap for gamemaster operators.
#
# ★★ YOUR VPS MUST HAVE A GLOBAL IPv6 ADDRESS. Check before you buy it. IPv4 as
# well is fine -- it is IPv6 that must be there. Signing is point-to-point: the
# caller connects directly to the address you register and no relay bridges it,
# so an IPv4-only gamemaster is invisible to the network while still syncing and
# reporting Ready. install.sh refuses such a host and refuses before writing
# anything. Addresses starting `fd` are ULA and do NOT count.
#
#   wget https://raw.githubusercontent.com/vileda-hemis/Hemis-PTX-MVP1/v0.3.2-testnet/vps-install.sh
#   bash vps-install.sh
#
# ★★ BY TAG, NOT BY BRANCH, AND NOT BY `main`. This header used to say `main`.
# raw.githubusercontent serves whatever the ref points at now, so a branch there
# pins nothing -- and in THIS repository `main` holds a DIFFERENT FILE OF THE
# SAME NAME: the upstream Hemis mainnet bootstrap, which installs
# Hemis-Blockchain/Hemis mainnet and exits saying "Hemis successfully
# configured." It does not error. An operator who followed the old line got a
# working mainnet node and no indication that anything was wrong.
#
# Read it before you run it. It is short on purpose: it does NOT install anything
# itself. It fetches the pinned release and hands off to testnet/operator/install.sh,
# which is the real installer -- the one that verifies checksums, checks glibc,
# reserves the fan-out ports and writes your configs.
#
# ★ WHY A WRAPPER AND NOT A SECOND INSTALLER. A copy of the install logic here
# would be a second thing to keep in sync with the first, and the two would drift
# in the direction nobody tests. Everything below is either a prerequisite the
# installer needs before it can run, or a loop over it.
set -euo pipefail

# ★ PINNED TO A TAG, DELIBERATELY, AND THIS IS THE WHOLE POINT OF THE FILE.
# A tag is immutable: two operators running this a week apart get identical source
# and identical binaries. Branches move.
#
# ★ DO NOT "IMPROVE" THIS INTO /releases/latest/download/... The upstream Hemis
# vps-install.sh uses that form and it CANNOT work here: v0.1.0-testnet is a
# PRE-release, and GitHub's /latest ignores prereleases. Measured 2026-08-21 --
# /releases/latest/download/Hemis-Linux.zip returns 404 on this repository, and
# the API's /releases/latest says "Not Found". A testnet wants a pinned artefact
# anyway; "latest" is how two operators end up on different code.
TAG="${PTX_TAG:-v0.3.2-testnet}"
REPO="${PTX_REPO:-https://github.com/vileda-hemis/Hemis-PTX-MVP1.git}"

# ★★ ONE GM PER HOST. Run this bootstrap ONCE ON EACH of your GM machines. How
# many that is comes from the coordinator; this script does not assume a number.
#
# A quorum needs 11 members, drawn from the pool of registered, eligible,
# non-banned GMs not already in an active quorum. Below 11 nothing forms and
# every boundary is a silent skip; at exactly 11 the next GM lost stops
# formation. That spare capacity is a NETWORK total, not a per-operator figure.
#
# ★ WHY NOT SEVERAL ON ONE HOST, which this script used to do by default. The
# original reason was mechanical -- the signing fan-out dialled every member on
# one shared RPC port, so a co-hosted GM could never be reached. KDD-085 DELETED
# the fan-out; signing now arrives over P2P at each GM's own registered address
# and port, so that specific blocker is gone. One GM per host is kept anyway:
# co-hosting is untested -- no fleet run, no ceremony, no PoSe observation under
# it -- and a launch testnet is the wrong place to find out what breaks.
#
# PTX_GM_COUNT is retained for a coordinator-directed exception only. Leave it 1.
GM_COUNT="${PTX_GM_COUNT:-1}"
CLONE_DIR="${PTX_CLONE_DIR:-$HOME/Hemis-PTX-MVP1}"

if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

say()  { printf '\n=== %s ===\n' "$*"; }
ok()   { printf '  [ok]   %s\n' "$*"; }
warn() { printf '  [WARN] %s\n' "$*"; }
die()  { printf '\n  [FAIL] %s\n\n' "$*" >&2; exit 1; }

say "PTX testnet bootstrap — $TAG"
echo "  repo:      $REPO"
echo "  clone:     $CLONE_DIR"
echo "  gamemasters to install: $GM_COUNT"

# ---------------------------------------------------------------------------
# 1. Prerequisites.
#
# ★ git and curl are what install.sh needs before it can tell you anything useful.
# Without them it stops at its own tool check with "missing required tool: git",
# which is correct but is a worse first experience than just installing them.
#
# ★ NO unzip. The upstream mainnet script needs it because it ships a .zip; the
# PTX release ships .tar.gz and install.sh uses tar, which is on every base image.
# ---------------------------------------------------------------------------
say "1. Prerequisites"
if command -v apt-get >/dev/null 2>&1; then
    # ★ "$SUDO env VAR=..." and NOT "$SUDO VAR=... ". As root $SUDO is empty, and a
    # variable assignment is only an assignment PREFIX if the parser sees it in that
    # position -- after $SUDO expands to nothing the next word becomes the COMMAND,
    # and you get "DEBIAN_FRONTEND=noninteractive: command not found". Caught on the
    # first container run of this script, as root, which is how most VPSes arrive.
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get update -qq
    # --no-install-recommends: this is somebody's VPS, not a workstation.
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends \
        git curl ca-certificates >/dev/null
    ok "git, curl, ca-certificates installed"
else
    # ★ Not fatal. install.sh checks by glibc and architecture, not distro name,
    # so a non-apt box is expected to work -- it just cannot be prepared for you.
    warn "no apt-get here; install these yourself if missing: git curl ca-certificates"
fi
for t in git curl; do
    command -v "$t" >/dev/null 2>&1 || die "missing required tool: $t (install it and re-run)"
done

# ---------------------------------------------------------------------------
# 2. Fetch the pinned source.
# ---------------------------------------------------------------------------
say "2. Source"
if [ -d "$CLONE_DIR/.git" ]; then
    # ★ Re-runs are expected -- an operator who hits a firewall problem will run
    # this again. Fetch and hard-reset to the tag rather than pulling: pull on a
    # detached tag checkout is either a no-op or a merge, and neither is what is
    # wanted here.
    git -C "$CLONE_DIR" fetch --tags --quiet origin
    git -C "$CLONE_DIR" checkout --quiet --detach "$TAG"
    ok "existing clone updated to $TAG"
else
    git clone --quiet --branch "$TAG" --depth 1 "$REPO" "$CLONE_DIR"
    ok "cloned $TAG into $CLONE_DIR"
fi

# ★ Assert what we came for rather than assuming the clone achieved it. If the
# coordinator ever points PTX_TAG at a branch on which the operator tooling does
# not exist -- main, for one -- this is where it should stop, not four steps later.
INSTALLER="$CLONE_DIR/testnet/operator/install.sh"
[ -x "$INSTALLER" ] || [ -f "$INSTALLER" ] \
    || die "$INSTALLER is not in this checkout.
  '$TAG' does not carry testnet/operator/. Check the tag name with the coordinator."
ok "installer present: $INSTALLER"
echo "  commit: $(git -C "$CLONE_DIR" rev-parse --short HEAD)"

# ---------------------------------------------------------------------------
# 3. Install each gamemaster.
#
# ★ The port pairs are the allocation from OPERATOR_GUIDE.md, and they are not
# arbitrary: GM n uses P2P 29992+2n and RPC 29993+2n. Keeping to the table means
# the coordinator can read your self-check output without a translation step.
# ---------------------------------------------------------------------------
say "3. Gamemasters"
for n in $(seq 1 "$GM_COUNT"); do
    # ★ At the documented GM_COUNT=1 these are install.sh's own defaults and no
    # override is passed at all -- the ports are the SAME on every host, because
    # the hosts differ rather than the ports. The n>1 arithmetic survives only for
    # the coordinator-directed exception above, and it is NOT the supported shape:
    # a second GM here would sit on RPC 29997, which the fan-out never dials.
    if [ "$n" = "1" ]; then
        # ★ GM 1 goes in the DAEMON'S OWN DEFAULT datadir, matching install.sh.
        # That is BUG-047's fix: a bare `Hemisd` on this host then finds a real
        # config and comes up on ptxtestnet instead of silently syncing mainnet.
        # GMs 2+ below keep suffixed directories -- they cannot all be the
        # default -- and they already carry the warning that a second GM on one
        # host is the wrong deployment anyway.
        p2p=29994; rpc=29995; datadir="$HOME/.Hemis"
    else
        p2p=$((29992 + 2 * n)); rpc=$((29993 + 2 * n))
        datadir="$HOME/.hemis-ptxtestnet-$n"
        warn "GM $n is a SECOND gamemaster on this host, on RPC $rpc."
        warn "  The signing fan-out dials one port for every member; this GM will"
        warn "  register and never be contacted. Use a separate host instead."
    fi
    printf '\n---------- GM %s of %s: datadir %s, P2P %s, RPC %s ----------\n' \
        "$n" "$GM_COUNT" "$datadir" "$p2p" "$rpc"
    # ★ Run it from its own directory. install.sh resolves siblings (RELEASE.env)
    # relative to BASH_SOURCE, but self-check.sh is documented as being run from
    # there and an operator who copies the pattern from here will get it right.
    ( cd "$CLONE_DIR/testnet/operator" \
      && PTX_DATADIR="$datadir" PTX_P2P_PORT="$p2p" PTX_RPC_PORT="$rpc" \
         bash ./install.sh ) \
      || die "install.sh failed for GM $n. Nothing after this point ran. The output
  above says which section stopped; fix that and re-run this script -- it is safe
  to run again and will skip what is already done."
done

# ---------------------------------------------------------------------------
# 4. What is deliberately NOT done here.
# ---------------------------------------------------------------------------
say "Done — and here is what is still yours to do"
cat <<EOF
  Installed $GM_COUNT gamemaster(s) from $TAG.
  Binaries:  /opt/hemis-ptx/bin  (symlinked into /usr/local/bin)
  Your copy of the scripts: $CLONE_DIR/testnet/operator

  ★ NOTHING HAS BEEN STARTED. The upstream mainnet script starts the daemon to
    generate a config and stops it again. This one does not, because a PTX node
    needs its BLS key in Hemis.conf before there is any point in it running, and
    starting it first would only teach you to ignore a daemon that is not doing
    its job.

  NEXT, in order — the full text for each step is in
  $CLONE_DIR/testnet/operator/OPERATOR_GUIDE.md:

   1. FIREWALL. Open each GM's P2P port in BOTH the host firewall and any
      NAT router or cloud security group. For GM 1 that is 29994 ONLY.
      ★ Do NOT open 29995 (RPC). It is loopback-only: nothing dials it, and
        exposing it publishes the credentials in your config for no benefit.
      ★ P2P closed is the silent killer: the node still syncs (it dials out),
        shows as registered and enabled, and never signs anything, because
        signing requests arrive over P2P at the address you registered.
        Nothing in the ordinary status output tells you.

   2. BLS KEY, per GM. Generate it on THIS machine, put the SECRET half in that
      GM's Hemis.conf as gmoperatorprivatekey=, and send only the PUBLIC half to
      the wallet operator. See OPERATOR_GUIDE.md, "Node side".

   3. COLLATERAL, on your OTHER machine. Your collateral and wallet keys never
      go on this box. See OPERATOR_GUIDE.md, "Wallet side" -- one exact 100 HMS
      output per gamemaster.

   4. START, then verify:
        Hemisd -datadir=\$HOME/.Hemis -daemon
        cd $CLONE_DIR/testnet/operator && ./self-check.sh
EOF
