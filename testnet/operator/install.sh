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
# ★ The operator tooling lives on feature/ptx-dkg, NOT on the default branch
# (main). Defaulting to empty would clone main, where testnet/operator/ does not
# exist -- i.e. the guide's very first step would fail. At launch this becomes
# the release TAG and this default should change with it.
REF="${PTX_REF:-feature/ptx-dkg}"
PREFIX="${PTX_PREFIX:-/opt/hemis-ptx}"
DATADIR="${PTX_DATADIR:-$HOME/.hemis-ptxtestnet}"
# ★ Overridable so a host can run several GMs. Each GM needs its OWN datadir AND
# its own port pair -- two daemons cannot share either. See OPERATOR_GUIDE.md
# "Running three GMs on one host" for the allocation table.
P2P_PORT="${PTX_P2P_PORT:-29994}"
RPC_PORT="${PTX_RPC_PORT:-29995}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ★ Root does not need sudo, and plenty of boxes (Debian minimal, container images,
# a Proxmox host) do not ship it at all. Deciding this ONCE, here, is the difference
# between a clear message in section 1 and "sudo: command not found" three sections
# later with set -e turning it into a bare exit 127.
if [ "$(id -u)" -eq 0 ]; then SUDO=""; else SUDO="sudo"; fi

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
#
# ★ Do NOT write this as `ldd --version | head -1 | awk ...`. Under `set -o pipefail`
# `head` exits after one line, ldd (a shell script on Debian) is killed by SIGPIPE,
# the pipeline reports 141 and `set -e` ends the script WITH NO OUTPUT AT ALL. It is
# a race, so it fails intermittently: measured 14 times in 20 on one host, having
# passed on that host's first run. awk does the line selection itself and reads to EOF.
GLIBC="$(ldd --version 2>/dev/null | awk 'NR==1{print $NF}')"
[ -n "$GLIBC" ] || die "could not determine glibc version ('ldd --version' produced nothing). If this is Alpine/musl, these binaries will not run."
GLIBC_MAJOR="${GLIBC%%.*}"; GLIBC_MINOR="${GLIBC#*.}"; GLIBC_MINOR="${GLIBC_MINOR%%.*}"
# ★ Where 2.31 comes from, and why it is not 2.27: the release build sets
# --enable-glibc-back-compat and contrib/devtools/symbol-check.py declares the
# floor as GLIBC 2.27 -- but the Linux job does not run `make check-symbols`, so
# 2.27 is DECLARED, not ENFORCED. 2.31 (Ubuntu 20.04) is the conservative gate we
# can actually stand behind. Turn on symbol_check for the Linux build, watch it
# pass once, and this can drop to 2.27 honestly. Not before.
if [ "$GLIBC_MAJOR" -gt 2 ] || { [ "$GLIBC_MAJOR" -eq 2 ] && [ "$GLIBC_MINOR" -ge 31 ]; }; then
    ok "glibc $GLIBC (need >= 2.31)"
else
    die "glibc $GLIBC is too old (need >= 2.31). Upgrade the OS or build from source."
fi

for tool in git curl awk sed sha256sum tar; do
    command -v "$tool" >/dev/null 2>&1 || die "missing required tool: $tool"
done
# Not fatal: section 4 falls back to writing /proc directly. Reported here rather
# than silently, so the operator is not surprised by which mechanism was used.
command -v sysctl >/dev/null 2>&1 \
    || warn "sysctl not found (package 'procps') -- the port reservation will be applied via /proc instead"
if [ -n "$SUDO" ] && ! command -v sudo >/dev/null 2>&1; then
    die "this script needs root for /opt and /etc/sysctl.d, and you are not root and have no sudo. Re-run as root, or install sudo."
fi
ok "required tools present$([ -z "$SUDO" ] && echo " (running as root; sudo not needed)")"

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
    $SUDO mkdir -p "$PREFIX"
    $SUDO chown "$(id -u):$(id -g)" "$PREFIX"
    git clone --quiet "$REPO" "$PREFIX"
    ok "cloned $REPO"
fi
if [ -n "$REF" ]; then
    if git -C "$PREFIX" checkout --quiet "$REF" 2>/dev/null; then
        ok "checked out $REF"
    else
        die "could not check out '$REF'. If the coordinator gave you a different tag, pass it as PTX_REF=<tag>."
    fi
    # ★ `git fetch` moves the REMOTE-TRACKING ref; `git checkout <existing local branch>`
    # does NOT fast-forward it. Without the next block, re-running this installer on an
    # existing checkout prints "updating" and "checked out", exits 0, and leaves you on
    # the OLD commit -- a no-op that reports success, which is worse than an error.
    # (Tags are immutable and arrive as new refs, so they are unaffected; this is the
    # branch case, which is what PTX_REF defaults to today.)
    if git -C "$PREFIX" show-ref --verify --quiet "refs/remotes/origin/$REF"; then
        WANT_COMMIT="$(git -C "$PREFIX" rev-parse "origin/$REF")"
        if [ "$(git -C "$PREFIX" rev-parse HEAD)" != "$WANT_COMMIT" ]; then
            git -C "$PREFIX" merge --ff-only --quiet "origin/$REF" \
                || die "$PREFIX is on a branch that has diverged from origin/$REF and cannot be fast-forwarded. Move it aside and re-run: mv $PREFIX $PREFIX.old"
            ok "fast-forwarded $REF to $(git -C "$PREFIX" rev-parse --short HEAD)"
        fi
        # Assert the postcondition rather than assume the commands above achieved it.
        [ "$(git -C "$PREFIX" rev-parse HEAD)" = "$WANT_COMMIT" ] \
            || die "after checkout, $PREFIX is at $(git -C "$PREFIX" rev-parse --short HEAD) but origin/$REF is $(git -C "$PREFIX" rev-parse --short "origin/$REF"). Refusing to continue on the wrong source."
    fi
    case "$REF" in
        v*|*[0-9].[0-9]*) : ;;
        *) warn "'$REF' is a BRANCH, not a pinned tag -- it moves. For launch, pass PTX_REF=<release tag>." ;;
    esac
fi
# Sanity: the thing we came here for must actually be present.
[ -d "$PREFIX/testnet/operator" ] || die "$REF does not contain testnet/operator/ -- wrong ref. Ask the coordinator which tag to use."
echo "  commit: $(git -C "$PREFIX" rev-parse --short HEAD)"

# ---------------------------------------------------------------------------
# 3. Binaries.
#
# ★ Cloning the source does NOT give you a runnable node, and the guide's very
# next step calls Hemis-cli. Before this section existed the operator got
# "Hemis-cli: command not found" two steps later, with nothing to connect it to.
#
# Order of preference:
#   1. the release artefact pinned in RELEASE.env (written by the release cut),
#   2. binaries already on PATH,
#   3. stop, and say which of the two you are missing.
# Building from source is deliberately NOT attempted here: it needs Boost, BDB
# 4.8 and a toolchain to line up, it takes tens of minutes, and a failure half an
# hour in is the worst possible first experience. If you must, the instructions
# are printed rather than run.
# ---------------------------------------------------------------------------
# ---------------------------------------------------------------------------
# 3b. Build from source -- the route that actually exists before a tag is cut.
#
# ★ PROVEN on a fresh debian:bookworm-slim, 2026-08-19, not assumed:
#   * Section 3 dies on ANY branch checkout. RELEASE.env is absent on a branch,
#     a branch ref derives no artefact URL BY DESIGN, and a fresh box has nothing
#     on PATH. Observed: "[FAIL] no PTX binaries, and no release artefact".
#   * The route (b) this script used to PRINT failed at its FIRST command:
#     "./autogen.sh: configuration failed, please install autoconf first"
#     -- because it named no build dependencies at all.
#   * With the dependencies installed it STILL failed:
#     "configure: error: Found Berkeley DB other than 4.8 ... --with-incompatible-bdb"
#
# So there was no route to a working binary, and the project was circular: the
# release tag cannot be cut until an end-to-end run passes, and that run cannot
# start without a binary. ★ Printing instructions is not a route if the
# instructions do not work -- and nobody finds out until a stranger runs them.
#
# The recipe below is NOT invented here. It is the one baked into the builder
# image that produced the 161-node fleet, so it has more evidence behind it than
# anything else in this repository.
#
# ★ --with-incompatible-bdb is load-bearing AND has a consequence the operator
# must be told: wallets created by this binary use the SYSTEM Berkeley DB (5.3 on
# Debian 12), not the 4.8 that official release artefacts are built against. A
# wallet.dat created here is NOT readable by a stock release binary. On a fresh
# testnet where every wallet is made by this same build that is harmless -- it is
# what the whole fleet already runs -- but it only bites LATER, when someone
# swaps binaries, which is exactly why it is said here and not in a footnote.
# ---------------------------------------------------------------------------
build_from_source() {
    say "3b. Build from source"
    command -v apt-get >/dev/null 2>&1 \
        || die "PTX_BUILD_FROM_SOURCE is only wired up for apt-based distros (Debian/Ubuntu).
  On anything else, install the equivalents of: build-essential autoconf automake
  libtool pkg-config libssl-dev libevent-dev libboost-all-dev libdb5.3++-dev
  libzmq3-dev libgmp-dev libsodium-dev, plus a Rust toolchain, then run:
    cd $PREFIX && ./autogen.sh && ./configure --without-gui --disable-tests \\
      --disable-bench --with-incompatible-bdb --without-miniupnpc --prefix=/usr/local \\
      && make -j<N> && make install"

    # ★ Parallelism derived from RAM, NOT from core count. `make -j$(nproc)` sizes
    # the job count by a quantity with no relation to the binding constraint: each
    # g++ here holds on the order of 1 GiB, so -j12 on a 4 GiB VPS does not build
    # faster, it gets OOM-killed two-thirds of the way through -- after half an
    # hour, which is the worst possible moment. Same shape as the fleet memory
    # floor registered as KDD-095: a limit derived from the wrong quantity.
    local cores mem_mib jobs
    cores=$(nproc)
    mem_mib=$(awk '/^MemAvailable:/{printf "%d", $2/1024}' /proc/meminfo)
    jobs=$(( mem_mib / 1024 )); [ "$jobs" -lt 1 ] && jobs=1
    [ "$jobs" -gt "$cores" ] && jobs="$cores"
    if [ "$jobs" -lt "$cores" ]; then
        warn "using -j$jobs, not -j$cores: ${mem_mib}MiB available and each compiler job needs ~1GiB"
    else
        ok "building with -j$jobs (${cores} cores, ${mem_mib}MiB available)"
    fi

    # A build that dies on ENOSPC at 90% wastes the whole wait. Check first.
    local free_gb
    free_gb=$(df -BG --output=avail "$PREFIX" | tail -1 | tr -dc '0-9')
    [ "${free_gb:-0}" -ge 8 ] \
        || die "only ${free_gb}G free at $PREFIX; a source build needs ~8G. Free some space and re-run."

    say "3b.1 Build dependencies (this is an apt-get install -- it changes your system)"
    $SUDO apt-get -qq update
    # --no-install-recommends keeps this to the packages the build genuinely needs.
    $SUDO apt-get -qq install -y --no-install-recommends \
        build-essential autoconf automake libtool pkg-config \
        libssl-dev libevent-dev libboost-all-dev libdb5.3++-dev \
        libzmq3-dev libgmp-dev libsodium-dev curl ca-certificates \
        || die "could not install the build dependencies -- see the apt output above"
    ok "build dependencies installed"

    # configure.ac does AC_PATH_PROG for rustc and cargo and src/Makefile.am has
    # ENABLE_ONLINE_RUST blocks, so a Rust toolchain is genuinely required. Debian
    # bookworm's rustc is old enough to be a coin flip, so use rustup like the
    # builder image does -- but only if cargo is not already there.
    if command -v cargo >/dev/null 2>&1; then
        ok "rust toolchain already present: $(cargo --version 2>/dev/null)"
    else
        say "3b.2 Rust toolchain"
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
            | sh -s -- -y --default-toolchain stable --profile minimal \
            || die "rustup install failed"
        export PATH="$HOME/.cargo/bin:$PATH"
        command -v cargo >/dev/null 2>&1 || die "rustup ran but cargo is still not on PATH"
        ok "rust installed: $(cargo --version)"
    fi

    say "3b.3 Compile (tens of minutes -- this is the slow part)"
    local blog="${TMPDIR:-/tmp}/ptx-build.log"
    echo "  full output: $blog"
    (
        cd "$PREFIX"
        ./autogen.sh
        ./configure --without-gui --disable-tests --disable-bench \
                    --with-incompatible-bdb --without-miniupnpc --prefix=/usr/local
        make -j"$jobs"
        $SUDO make install
    ) >"$blog" 2>&1 || {
        # ★ Show the tail AND keep the log. A masked failure is how a build gets
        # reported as "it just stopped" with nothing to act on.
        echo "  --- last 25 lines of $blog ---"
        tail -25 "$blog" | sed 's/^/  /'
        die "the build failed. Full log: $blog"
    }

    # ★ JUDGE THE BUILD BY ITS CONTENT, NOT BY make's EXIT CODE. A green make that
    # produced nothing runnable is the failure mode this project has hit before.
    local missing=""
    for b in Hemisd Hemis-cli Hemis-tx; do
        if [ -s "/usr/local/bin/$b" ]; then :; else missing="$missing $b"; fi
    done
    [ -z "$missing" ] || die "make exited 0 but these binaries are missing or empty:$missing. Log: $blog"
    /usr/local/bin/Hemis-cli -version >/dev/null 2>&1 \
        || die "Hemis-cli was built but will not execute on this machine. Log: $blog"
    # The builder image that produced the fleet strips, and it matters: unstripped,
    # Hemisd is ~291MB against ~30MB stripped. On a single-GM VPS that is real disk.
    # Done AFTER the run-check above, so a strip that damages a binary cannot be
    # mistaken for a build that never worked.
    $SUDO strip /usr/local/bin/Hemisd /usr/local/bin/Hemis-cli /usr/local/bin/Hemis-tx 2>/dev/null \
        || warn "could not strip the binaries (harmless -- they just stay large)"
    /usr/local/bin/Hemis-cli -version >/dev/null 2>&1 \
        || die "Hemis-cli stopped working after strip. Log: $blog"
    ok "built and installed: $(/usr/local/bin/Hemis-cli -version 2>/dev/null | awk 'NR==1')"
    # ★ The version says "-dirty" and that is NOT a warning about your checkout.
    # autogen.sh regenerates Makefile.in, aclocal.m4, build-aux/ and autom4te.cache/,
    # and this repository TRACKS those generated files -- so building from a
    # pristine clone necessarily dirties the tree. Said out loud because a binary
    # that calls itself dirty otherwise looks like it was built from modified source.
    echo "         (\"-dirty\" is expected: autogen.sh regenerates tracked autotools files)"
    warn "this build uses the SYSTEM Berkeley DB (--with-incompatible-bdb), not 4.8:"
    echo "         wallets it creates are NOT readable by a stock release binary."
    echo "         Fine for this testnet; do not mix this build with release binaries."
}

say "3. Binaries"
# RELEASE.env is generated when a release is cut. It is not edited by hand and it
# is absent on a plain branch checkout -- which is exactly the case this handles.
if [ -f "$HERE/RELEASE.env" ]; then
    # shellcheck disable=SC1091
    . "$HERE/RELEASE.env"
fi
BIN_URL="${PTX_BIN_URL:-${RELEASE_BIN_URL:-}}"
BIN_SHA="${PTX_BIN_SHA256:-${RELEASE_BIN_SHA256:-}}"
BINDIR="$PREFIX/bin"
ASSET="${PTX_BIN_ASSET:-Hemis-Linux.tar.gz}"

# ★ If you were given a TAG, the artefact for that tag is derivable -- you should
# not also have to be given a URL. Branch refs get nothing here on purpose: a
# branch has no release, and guessing one would produce a confusing 404 instead of
# the accurate "this checkout is source only" message below.
case "$REF" in
    v*|*[0-9].[0-9]*)
        if [ -z "$BIN_URL" ]; then
            BIN_URL="${REPO%.git}/releases/download/$REF/$ASSET"
            ok "release artefact for $REF: $BIN_URL"
        fi
        # The checksum list published with the release. This proves the download
        # was not corrupted and matches what the release job produced. It does NOT
        # prove authenticity -- it comes from the same host as the artefact. The
        # authenticity anchor is the coordinator telling you the hash directly, so
        # if PTX_BIN_SHA256 is set it is checked against this list as well.
        if [ -z "$BIN_SHA" ]; then
            SUMS="$(curl -fsSL "${REPO%.git}/releases/download/$REF/SHA256SUMS" 2>/dev/null || true)"
            if [ -n "$SUMS" ]; then
                BIN_SHA="$(printf '%s\n' "$SUMS" | awk -v a="$ASSET" '$2 == a || $2 == "*"a {print $1; exit}')"
                [ -n "$BIN_SHA" ] && ok "sha256 for $ASSET taken from the release SHA256SUMS"
            fi
            [ -n "$BIN_SHA" ] || warn "no SHA256SUMS published for $REF -- ask the coordinator for the sha256 and pass PTX_BIN_SHA256=<hash>"
        fi
        ;;
esac

if [ -n "$BIN_URL" ]; then
    # ★ A URL without a checksum is not a pin. Refuse rather than install something
    # we cannot identify -- the whole point of shipping artefacts is that you can
    # tell whether you got what the coordinator meant you to get.
    [ -n "$BIN_SHA" ] || die "PTX_BIN_URL is set but PTX_BIN_SHA256 is not. Refusing to install an unverified binary."
    TMPD="$(mktemp -d)"
    trap 'rm -rf "$TMPD"' EXIT
    curl -fsSL --retry 3 -o "$TMPD/hemis.tar.gz" "$BIN_URL" || die "could not download $BIN_URL"
    echo "$BIN_SHA  $TMPD/hemis.tar.gz" | sha256sum -c --status \
        || die "SHA256 MISMATCH on the downloaded archive. Do NOT use it. Expected $BIN_SHA; got $(sha256sum "$TMPD/hemis.tar.gz" | awk '{print $1}')."
    ok "downloaded and verified sha256 $BIN_SHA"
    mkdir -p "$TMPD/x" && tar -xzf "$TMPD/hemis.tar.gz" -C "$TMPD/x"
    mkdir -p "$BINDIR"
    for b in Hemisd Hemis-cli Hemis-tx; do
        # -print -quit rather than `| head -1`: same SIGPIPE trap as the glibc check.
        src="$(find "$TMPD/x" -type f -name "$b" -print -quit)"
        [ -n "$src" ] || die "the archive does not contain $b. Wrong artefact for this platform?"
        install -m 0755 "$src" "$BINDIR/$b"
    done
    ok "installed Hemisd, Hemis-cli, Hemis-tx into $BINDIR"
    for b in Hemisd Hemis-cli; do
        $SUDO ln -sf "$BINDIR/$b" "/usr/local/bin/$b" 2>/dev/null \
            || warn "could not symlink $b into /usr/local/bin -- add $BINDIR to your PATH yourself"
    done
    # Assert the outcome instead of assuming the install achieved it.
    "$BINDIR/Hemis-cli" -version >/dev/null 2>&1 \
        || die "$BINDIR/Hemis-cli will not run on this machine. Wrong architecture, or a glibc older than the build host's."
    ok "Hemis-cli runs: $("$BINDIR/Hemis-cli" -version 2>/dev/null | awk 'NR==1')"
elif command -v Hemisd >/dev/null 2>&1 && command -v Hemis-cli >/dev/null 2>&1; then
    ok "using the binaries already on your PATH: $(command -v Hemisd)"
    echo "  version: $(Hemis-cli -version 2>/dev/null | awk 'NR==1')"
elif [ "${PTX_BUILD_FROM_SOURCE:-0}" = "1" ]; then
    # The operator asked for it explicitly. Section 3b does the whole thing and
    # verifies the result, rather than printing a recipe and hoping.
    build_from_source
else
    die "no PTX binaries, and no release artefact to fetch.

  This checkout is source only. You need Hemisd and Hemis-cli, and there are two ways:

    (a) Use the release the coordinator points you at -- the supported route once
        a tag exists:
          PTX_BIN_URL=<url> PTX_BIN_SHA256=<sha256> ./install.sh
        (or re-run from a release tag, where RELEASE.env supplies both.)

    (b) Build from source. THIS SCRIPT WILL DO IT FOR YOU -- it installs the build
        dependencies, compiles, and verifies the binaries actually run:
          PTX_BUILD_FROM_SOURCE=1 ./install.sh
        Expect tens of minutes, ~8G of disk, and an apt-get install that changes
        your system. See section 3b for exactly what it does and why.

  ★ Before a release tag exists, (b) is the ONLY route -- (a) has nothing to point
    at. Ask the coordinator whether a tag has been cut yet."
fi

# ---------------------------------------------------------------------------
# 4. Kernel port reservation.
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
say "4. Port reservation"
SYSCTL_FILE=/etc/sysctl.d/99-ptx-fleet-ports.conf
# ★ /etc/sysctl.d is NOT guaranteed to exist. Proven 2026-08-19: on
# debian:bookworm-slim -- and on the minimal LXC/container templates an operator
# is most likely to spin up for a single GM -- the directory is absent, and this
# section died with a bare "tee: ...: No such file or directory" AFTER a
# ten-minute build had already succeeded. Losing a good build to a missing
# directory is the worst kind of late failure.
$SUDO mkdir -p "$(dirname "$SYSCTL_FILE")" \
    || die "cannot create $(dirname "$SYSCTL_FILE") -- run as root, or as a user with sudo"
WANT="32000-33000"
CURRENT="$(cat /proc/sys/net/ipv4/ip_local_reserved_ports 2>/dev/null || echo "")"

# ★ `sysctl` is NOT guaranteed to be installed. Proven 2026-08-19 on
# debian:bookworm-slim: /sbin and /usr/sbin are on PATH but the procps package is
# absent, so `sysctl -q -p` died as a bare "command not found", exit 127, with
# set -e giving no explanation -- three lines after a ten-minute build succeeded.
# Section 1 checks for git/curl/awk/sed/sha256sum/tar and never checked for this
# one, which is the pattern: the tool you forgot to declare is the one that is
# missing. Same shape as the `sudo: command not found` finding.
#
# Rather than add a dependency, apply the setting the way this script ALREADY
# READS it -- through /proc -- and use sysctl only when it is there, because
# `sysctl -p FILE` additionally validates that the file we just wrote parses.
apply_reservation() {
    local value="$1"
    echo "net.ipv4.ip_local_reserved_ports=$value" | $SUDO tee "$SYSCTL_FILE" >/dev/null \
        || die "could not write $SYSCTL_FILE"
    if command -v sysctl >/dev/null 2>&1; then
        $SUDO sysctl -q -p "$SYSCTL_FILE" \
            || die "$SYSCTL_FILE was written but sysctl refused to load it -- the file is malformed"
    else
        # No procps. The kernel interface is the same one sysctl writes.
        # stderr suppressed: the caller prints a far better diagnosis than
        # "Read-only file system", and a raw tee error above it just muddies it.
        printf '%s' "$value" | $SUDO tee /proc/sys/net/ipv4/ip_local_reserved_ports >/dev/null 2>&1 \
            || return 1
    fi
    # ★ ASSERT, do not assume. Neither branch's exit code proves the kernel took
    # the value -- and the whole point of this section is that a reservation which
    # is not actually in force fails LATER, intermittently, under load.
    local active
    active="$(cat /proc/sys/net/ipv4/ip_local_reserved_ports 2>/dev/null || echo "")"
    printf '%s' "$active" | grep -q "$WANT" || return 1
    return 0
}

# ★ An UNPRIVILEGED container cannot do this at all, and that must not be reported
# as "need root" -- proven 2026-08-19: in an unprivileged container running AS
# ROOT, /proc/sys is a read-only mount, so the old message sent the operator to
# fix a permission they already had. A wrong diagnosis is worse than none.
#
# It must also not be FATAL. A single GM in an unprivileged LXC is a perfectly
# reasonable deployment and everything else in this installer works there; the
# reservation is set on the HOST, because ip_local_reserved_ports is per network
# namespace and an unprivileged guest does not own its own writable copy.
port_reservation_unavailable() {
    warn "COULD NOT RESERVE PORTS $WANT -- and this is not a permission you can grant yourself."
    echo "         /proc/sys is read-only here, which means this is an UNPRIVILEGED container."
    echo "         (You are $( [ "$(id -u)" -eq 0 ] && echo "already root" || echo "not root" ); root is not the issue.)"
    echo
    echo "         WHAT IT COSTS: the kernel may hand out a port in $WANT as an"
    echo "         ephemeral source port for some unrelated outgoing connection, and the"
    echo "         PTX listener then fails to bind -- intermittently, under load, which is"
    echo "         the worst way to find out. The node works; this is a latent fault."
    echo
    echo "         FIX IT ON THE HOST, not in here -- run on the Proxmox/LXC host:"
    echo "           echo 'net.ipv4.ip_local_reserved_ports=$WANT' > $SYSCTL_FILE"
    echo "           sysctl -p $SYSCTL_FILE"
    echo "         or run this guest privileged. Then re-run this installer to confirm."
}

if printf '%s' "$CURRENT" | grep -q "$WANT"; then
    ok "reservation $WANT already active"
elif [ -n "$CURRENT" ]; then
    # Something else reserved ports. Preserve theirs, add ours.
    MERGED="$CURRENT,$WANT"
    warn "existing reservation '$CURRENT' found -- MERGING, not replacing"
    if apply_reservation "$MERGED"; then ok "reserved $MERGED"; else port_reservation_unavailable; fi
else
    if apply_reservation "$WANT"; then ok "reserved $WANT"; else port_reservation_unavailable; fi
fi
echo "  now active: $(cat /proc/sys/net/ipv4/ip_local_reserved_ports 2>/dev/null || echo '(unreadable)')"

# ★ Being active NOW is not the same as surviving a reboot. sysctl.d files are
# applied in filename order and the LAST writer of a key wins, so a foreign file
# that sorts after ours silently drops our range at the next boot -- the machine
# comes back looking fine and the PTX listener starts failing to bind under load.
# Proven both ways: a foreign 50-*.conf is harmless, a foreign 99-zz-*.conf wipes us.
OURS="$(basename "$SYSCTL_FILE")"
for f in /etc/sysctl.d/*.conf /etc/sysctl.conf; do
    [ -f "$f" ] || continue
    [ "$(basename "$f")" = "$OURS" ] && continue
    grep -q 'ip_local_reserved_ports' "$f" 2>/dev/null || continue
    if [ "$f" = /etc/sysctl.conf ] || [ "$(basename "$f")" \> "$OURS" ]; then
        warn "$f also sets ip_local_reserved_ports and is applied AFTER $SYSCTL_FILE."
        warn "  At the next reboot IT WINS and the $WANT reservation is lost. Merge our range into that file, or rename ours to sort last."
    else
        echo "  note: $f also sets ip_local_reserved_ports, but is applied before ours (harmless)"
    fi
done

# ---------------------------------------------------------------------------
# 5. Configuration.
#
# ★ rpcbind is DUAL-STACK on purpose: 0.0.0.0 covers IPv4, :: covers IPv6.
# Binding only one family is the exact seam that makes a node look healthy
# on-chain while never receiving a sign request -- see self-check.sh.
#
# ★ RPC MUST be reachable by your quorum peers. PTX fan-out dials each member's
# RPC directly; a firewalled RPC port means you are selected, never contacted,
# and silently never sign. This is why rpcallowip is not localhost-only.
# ---------------------------------------------------------------------------
say "5. Configuration"
mkdir -p "$DATADIR"
CONF="$DATADIR/hemis.conf"
TEMPLATE="$DATADIR/hemis.conf.ptx-template"

# ★ The template is written EVERY run, next to the config, and is the thing the
# already-exists branch tells you to compare against. Previously that branch said
# "compare it against the template printed below" and nothing was printed below --
# an instruction pointing at something that did not exist.
emit_conf() {   # $1 = value to put in rpcpassword
    cat <<EOF
# PTX testnet node configuration.
ptxtestnet=1

# --- RPC -------------------------------------------------------------------
rpcuser=${RPCUSER:-ptxop}
rpcpassword=$1
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
}

# The template carries a PLACEHOLDER password, never the generated one -- it is a
# reference document, and it must stay safe to read, diff and paste.
emit_conf '<your rpcpassword -- keep the one already in hemis.conf>' > "$TEMPLATE"
chmod 600 "$TEMPLATE"

if [ -f "$CONF" ]; then
    warn "$CONF already exists -- leaving it alone."
    echo "  A reference config for THIS GM's ports has been written to:"
    echo "    $TEMPLATE"
    echo "  Compare them (the rpcpassword line is expected to differ):"
    echo "    diff -u $TEMPLATE $CONF"
    if ! grep -qE '^[[:space:]]*rpcbind[[:space:]]*=[[:space:]]*::' "$CONF"; then
        warn "  your existing config has no 'rpcbind=::' -- IPv6 is NOT bound. See self-check.sh section 4."
    fi
    if ! grep -qE "^[[:space:]]*rpcport[[:space:]]*=[[:space:]]*$RPC_PORT" "$CONF"; then
        warn "  your existing config's rpcport does not match the $RPC_PORT this run was told to use."
    fi
else
    RPCUSER="ptxop"
    RPCPASS="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')"
    emit_conf "$RPCPASS" > "$CONF"
    chmod 600 "$CONF"
    ok "wrote $CONF (mode 600)"
    ok "reference template at $TEMPLATE"
    warn "RPC password was generated for you; it is in $CONF. Do not paste it into chat or tickets."
fi

say "Done"
cat <<EOF
  Config:  $CONF
  Datadir: $DATADIR
  P2P:     $P2P_PORT      RPC: $RPC_PORT

  Running more than one GM on this host? Repeat with BOTH overridden, e.g.:
    PTX_DATADIR=~/.hemis-ptxtestnet-2 PTX_P2P_PORT=29996 PTX_RPC_PORT=29997 ./install.sh
  The 32000-33000 kernel reservation is host-wide and is set once; re-running is safe.

  NEXT, in order:
    1. Open $P2P_PORT and $RPC_PORT in your firewall AND any NAT/cloud security group.
    2. Follow OPERATOR_GUIDE.md section "Node side" to generate your BLS key and
       send the PUBLIC half to the wallet operator.
    3. Start the daemon, then run:  ./self-check.sh
EOF
