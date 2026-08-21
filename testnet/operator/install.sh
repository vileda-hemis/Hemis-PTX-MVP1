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
# ★ PINNED TO THE RELEASE TAG. A tag is immutable: two operators running this on
# different days get the same source and the same binaries, which a branch cannot
# promise. It is also what makes the artefact route in section 3 work at all --
# a branch ref derives no release URL, by design.
#
# Defaulting to empty would clone the default branch (main), where
# testnet/operator/ does not exist -- i.e. the guide's very first step would fail.
#
# To build a later fix before it is tagged, pass the branch explicitly:
#   PTX_REF=feature/ptx-dkg PTX_BUILD_FROM_SOURCE=1 ./install.sh
REF="${PTX_REF:-v0.1.0-testnet}"
PREFIX="${PTX_PREFIX:-/opt/hemis-ptx}"
DATADIR="${PTX_DATADIR:-$HOME/.hemis-ptxtestnet}"
# ★ Overridable, but the DOCUMENTED DEPLOYMENT IS ONE GM PER HOST, so the defaults
# are what an operator should use. These remain for unusual deployments and for
# install-test.sh's fixture. ★ RPC_PORT in particular must stay 29995: the signing
# fan-out dials ONE port number for EVERY member (ptx/ptx_fanout.cpp:117-120 --
# PTX_FanoutRpcPort() takes no per-member argument), so a GM on a non-standard RPC
# port is never contacted and silently never signs. See OPERATOR_GUIDE.md
# "One GM per host, one routable address per GM".
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
    # ★ DEBIAN_FRONTEND=noninteractive, AND VIA `$SUDO env`, FOR TWO SEPARATE REASONS.
    #
    # The frontend: without it, any package in this list that pulls in a
    # configuration prompt -- tzdata is the usual one, and libboost-all-dev's
    # dependency closure is wide -- stops and WAITS FOR INPUT. In a bootstrap an
    # operator started and walked away from, that is not a failure, it is a hang,
    # and the ten-minute build never begins. -y answers apt's own questions; it
    # does not answer debconf's.
    #
    # The `env`: as root $SUDO is empty, and a variable assignment is only an
    # assignment PREFIX if the parser sees it in that position. After $SUDO
    # expands to nothing the next word becomes the COMMAND, and you get
    # "DEBIAN_FRONTEND=noninteractive: command not found". vps-install.sh:71-76
    # was caught by exactly this, on the first container run, as root -- which is
    # how most VPSes arrive. It was fixed there and the same form is used here.
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get -qq update
    # --no-install-recommends keeps this to the packages the build genuinely needs.
    $SUDO env DEBIAN_FRONTEND=noninteractive apt-get -qq install -y --no-install-recommends \
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
    # ★ "-dirty" USED to be expected here and no longer is. This repository tracked
    # its own autotools output, so every autogen.sh run rewrote 29 tracked files and
    # a build from a pristine clone was dirty before it compiled anything. Fixed in
    # 93b44d4 by untracking them. It mattered because share/genbuild.sh:32-34 uses
    # the TAG name only when `git diff-index --quiet HEAD` passes -- a dirty tree
    # falls through to <shorthash>-dirty, so a tagged release could not have named
    # its own tag. Verified clean 2026-08-19: fresh clone + full build, 0 files.
    #
    # So if you see "-dirty" now it means what it says, and it is worth saying.
    case "$(/usr/local/bin/Hemis-cli -version 2>/dev/null | awk 'NR==1')" in
        *-dirty)
            warn "the version string says \"-dirty\": $PREFIX has local modifications."
            echo "         A build from a clean checkout should not say this. Check: git -C $PREFIX status"
            ;;
    esac
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
# 3c. Sapling zkSNARK parameters.
#
# ★ WITHOUT THESE THE DAEMON DOES NOT START. It is not a warning and not a
# degraded mode: init.cpp:1268 calls LoadSaplingParams(), which on failure calls
# StartShutdown() and the process exits 1. Proven both legs against the
# v0.1.0-testnet release binary on 2026-08-21:
#   RED   no ~/.Hemis-params      -> "Shutdown requested. Exiting.", exit 1,
#                                    immediately after the cache-configuration lines
#   GREEN params in place         -> "Loaded Sapling parameters in 0.199504s seconds."
#                                    then "init message: Done loading", stays up
# The RED failure names nothing useful in the log tail, which is why an operator
# hitting it reads it as "the daemon just dies".
#
# ★ NOTHING NEEDS DOWNLOADING. The two files are tracked in this repository and
# the clone in section 2 already put them at $PREFIX/params. The release tarball
# does NOT carry them -- it is three binaries -- so fetching them from the release
# is not an option and never was.
#
# The daemon looks in $HOME/.Hemis-params (util/system.cpp:567-594, the Unix
# branch). That is the HOME of whoever RUNS Hemisd. If you later run the daemon as
# a different user or under a systemd unit with its own HOME, either repeat this
# for that user or pass -paramsdir=<dir> explicitly.
# ---------------------------------------------------------------------------
say "3c. Sapling parameters"
PARAMS_SRC="$PREFIX/params"
PARAMS_DST="${PTX_PARAMS_DIR:-$HOME/.Hemis-params}"
mkdir -p "$PARAMS_DST"
for pf in sapling-spend.params sapling-output.params; do
    if [ -s "$PARAMS_DST/$pf" ]; then
        ok "$pf already present"
        continue
    fi
    [ -s "$PARAMS_SRC/$pf" ] \
        || die "$PARAMS_SRC/$pf is missing from the checkout. The clone in section 2 is incomplete -- delete $PREFIX and re-run."
    # Copy to a temp name and move into place, so an interrupted copy cannot leave
    # a short file that the next run then reports as "already present".
    cp "$PARAMS_SRC/$pf" "$PARAMS_DST/.$pf.part" \
        || die "could not write to $PARAMS_DST (need ~52MB free)"
    mv -f "$PARAMS_DST/.$pf.part" "$PARAMS_DST/$pf"
    ok "installed $pf ($(du -h "$PARAMS_DST/$pf" | cut -f1))"
done
# ★ Assert the outcome rather than trusting the copies. A truncated params file
# fails at daemon start, far from here, with the same unhelpful message as no file.
for pf in sapling-spend.params sapling-output.params; do
    a="$(sha256sum "$PARAMS_SRC/$pf" | awk '{print $1}')"
    b="$(sha256sum "$PARAMS_DST/$pf" | awk '{print $1}')"
    [ "$a" = "$b" ] || die "$PARAMS_DST/$pf does not match $PARAMS_SRC/$pf (sha256 $b vs $a). Delete it and re-run."
done
ok "sapling parameters verified in $PARAMS_DST"

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
    # ★★ DECIDE read-only-vs-malformed BEFORE asking sysctl, because sysctl cannot
    # tell you which one it hit. Without this, an unprivileged container that HAS
    # procps installed died right here with "the file is malformed" -- a wrong
    # diagnosis, and fatal, three lines above the handler written for exactly this
    # case. The no-sysctl branch below has always returned 1 and let the caller
    # explain properly; the sysctl branch called die() instead, so which message an
    # operator got depended on whether procps happened to be installed. Caught
    # 2026-08-21 by running the bootstrap in a container that had procps, having
    # passed the day before in one that did not.
    #
    # test -w is the right probe and was checked, not assumed: in an unprivileged
    # container running AS ROOT, /proc/sys is mounted ro and `test -w` correctly
    # reports not-writable (access(2) accounts for a read-only mount; the file's
    # permission bits alone would say root may write it).
    $SUDO test -w /proc/sys/net/ipv4/ip_local_reserved_ports 2>/dev/null || return 1
    if command -v sysctl >/dev/null 2>&1; then
        # Now a sysctl failure really does mean what the message says: we know the
        # kernel interface is writable, so the file we just wrote must be at fault.
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
# ★★ "Hemis.conf", CAPITAL H, AND THE CASE IS THE WHOLE THING. The daemon's config
# filename is Hemis_CONF_FILENAME = "Hemis.conf" (util/system.cpp:81). This script
# used to write "hemis.conf", which on a case-sensitive filesystem -- i.e. Linux --
# the daemon never opens. It does not complain: it logs "Using config file
# <datadir>/Hemis.conf" for a file that is not there and starts with ALL DEFAULTS.
#
# What that cost, measured in a fresh container running this installer as an
# operator would (2026-08-21): the node came up on MAINNET, not ptxtestnet -- blocks/
# and chainstate/ at the top of the datadir instead of under ptxtestnet/, "Bound to
# [::]:49165" instead of the P2P port asked for, no gamemaster=1, no rpcuser, and any
# gamemasterblsprivkey the operator had added silently unread. The second and third
# GMs on the host then failed outright, binding mainnet RPC 51473 which the first had
# already taken -- "Unable to bind any endpoint for RPC server".
#
# ★ It survived because this installer's end-to-end test stops at "installed and
# configured" and never starts a daemon. The first thing past the last assertion was
# the first thing broken -- the same reason 3c's missing parameters survived.
CONF="$DATADIR/Hemis.conf"
TEMPLATE="$DATADIR/Hemis.conf.ptx-template"

# ★ Anyone who ran the broken version has a lowercase hemis.conf that is now inert,
# and a datadir carrying a partial MAINNET download. Say both out loud: a stale file
# that looks like configuration is worse than no file, and a mainnet chainstate under
# a ptxtestnet datadir will not become a testnet one by fixing the config.
if [ -f "$DATADIR/hemis.conf" ] && [ ! -e "$CONF" ]; then
    # ★ RENAME rather than tell the operator to. They may have added
    # gamemasterblsprivkey= by hand -- the one line in there that is expensive to
    # recreate and dangerous to mistype -- and it is the whole content of the file
    # that was being ignored. Only when the correct name is free, so this can never
    # overwrite a good config with a stale one.
    mv "$DATADIR/hemis.conf" "$CONF"
    warn "found $DATADIR/hemis.conf -- LOWERCASE, AND THE DAEMON NEVER READ IT."
    echo "         An older version of this script wrote that name. Everything in it,"
    echo "         including any gamemasterblsprivkey you added, was being ignored."
    echo "         It has been renamed to $CONF, which the daemon does read."
    echo "         Check it before starting: the ports and rpcbind lines in it are"
    echo "         whatever that older run wrote, not necessarily this run's."
fi
if [ -d "$DATADIR/blocks" ] && [ ! -d "$DATADIR/ptxtestnet" ]; then
    warn "$DATADIR holds a MAINNET chain (blocks/ at the top level, no ptxtestnet/)."
    echo "         That is what the lowercase-config bug produced: the daemon ignored"
    echo "         your config and synced mainnet. It is not convertible. Stop that"
    echo "         daemon and clear the mainnet data before starting on ptxtestnet:"
    echo "           rm -rf $DATADIR/blocks $DATADIR/chainstate $DATADIR/database"
    echo "         Your wallet.dat is NOT touched by that command, and on this testnet"
    echo "         it holds nothing you need -- your collateral lives on the wallet machine."
fi

# ★ The template is written EVERY run, next to the config, and is the thing the
# already-exists branch tells you to compare against. Previously that branch said
# "compare it against the template printed below" and nothing was printed below --
# an instruction pointing at something that did not exist.
# ★ Build the rpcbind lines from THIS host's own global addresses. One GM per
# host means one address per GM, so binding explicitly is free and removes both
# the wildcard exposure and the ODC-076 double-bind collision. If no global
# address is found -- a NATted box, or a host addressed only after boot -- fall
# back to the dual-stack wildcard and say so, because refusing to write a config
# is worse than writing a working one with a warning.
RPCBIND_LINES="$(ip -o addr show scope global 2>/dev/null \
    | awk '{print $4}' | cut -d/ -f1 | sort -u \
    | sed 's/^/rpcbind=/')"
if [ -z "$RPCBIND_LINES" ]; then
    RPCBIND_LINES="rpcbind=0.0.0.0
rpcbind=::"
    warn "no global address found on this host -- RPC is bound to the WILDCARD."
    echo "         That listens on every interface; only your firewall stops the"
    echo "         internet reaching it, and the rpcpassword is in $CONF."
    echo "         Once this host has its public address, replace the two rpcbind"
    echo "         lines with that address and restart."
else
    ok "RPC will bind this host's own address(es): $(printf '%s' "$RPCBIND_LINES" | sed 's/^rpcbind=//' | tr '\n' ' ')"
    if [ "$(printf '%s\n' "$RPCBIND_LINES" | grep -c .)" -gt 2 ]; then
        warn "this host has several global addresses, so several rpcbind lines were written."
        echo "         That is safe but wider than needed. A gamemaster should bind the"
        echo "         address it REGISTERS; delete the other rpcbind lines from $CONF"
        echo "         once you know which one that is, then restart."
    fi
fi

emit_conf() {   # $1 = value to put in rpcpassword
    cat <<EOF
# PTX testnet node configuration.

# ★ SELECTS THE NETWORK, and must stay HERE -- above the section header, not in it.
ptxtestnet=1

# ★★ EVERYTHING BELOW SITS UNDER [ptxtestnet], AND THAT IS NOT COSMETIC. port,
# rpcport and rpcbind are network-specific settings: outside a network section the
# daemon IGNORES them and says so only in a startup warning --
#   "Config setting for -port only applied on ptxtestnet network when in
#    [ptxtestnet] section."
# -- and then binds the ptxtestnet DEFAULTS instead. Measured 2026-08-21 with these
# same lines above the header: "Bound to [::]:29993" rather than the P2P port asked
# for, and RPC attempted on 29902 rather than the one asked for. A node that is
# listening on the wrong ports looks entirely healthy from the inside.
[ptxtestnet]

# --- RPC -------------------------------------------------------------------
rpcuser=${RPCUSER:-ptxop}
rpcpassword=$1
rpcport=$RPC_PORT
# ★★ BOUND TO THIS HOST'S OWN GLOBAL ADDRESSES, NOT THE WILDCARD.
# The wildcard pair (0.0.0.0 + ::) leaves RPC listening on every interface with
# only the firewall between it and the internet, and the credentials above are in
# this file -- one rule away from exposure. With one GM per host there is no
# reason to bind anything but this host's own address.
# ★ It also sidesteps ODC-076: on Linux with the default net.ipv6.bindv6only=0 a
# "::" wildcard already covers IPv4, so the second of the two wildcard binds
# collided with the first and failed ("Binding RPC on address :: port N failed"),
# leaving one family unbound. Explicit per-address binds cannot collide.
$RPCBIND_LINES
# ★★ WHO MAY CALL THIS RPC, AND THIS LIST AS SHIPPED CANNOT SIGN.
# PTX fan-out dials each member's RPC directly to request a signature, so your
# quorum peers MUST be allowed here. localhost-only is a safe firewall posture and
# a node that never signs. Add one line per peer address from the coordinator at
# onboarding, then restart:
#   rpcallowip=<peer-address>/128    (IPv6 host)   or  /32 (IPv4 host)
# Keep it to peer addresses. Do NOT open it to 0.0.0.0/0 or ::/0.
rpcallowip=127.0.0.1
rpcallowip=::1
# rpcallowip=<peer-address>/128    <-- add one line per peer, from the coordinator

# --- P2P -------------------------------------------------------------------
port=$P2P_PORT
listen=1

# --- Node role -------------------------------------------------------------
# ★★ THESE TWO LINES GO IN TOGETHER, AND NOT BEFORE YOU HAVE THE KEY.
# gamemaster=1 with an empty gamemasterblsprivkey does not start a limited node --
# it REFUSES TO START AT ALL: "Error: ERROR: Gamemaster priv key cannot be empty."
# Since generateblskeypair is an RPC call, a config carrying gamemaster=1 up front
# locks you out of the very daemon you need in order to produce the key. Verified
# 2026-08-21 as a single-variable change against an otherwise working node.
#
# So: start the daemon as it is, generate the key (OPERATOR_GUIDE.md A4), then
# uncomment BOTH of these and restart.
# gamemaster=1
# gamemasterblsprivkey=<the BLS key you generate in the OPERATOR_GUIDE>
EOF
}

# The template carries a PLACEHOLDER password, never the generated one -- it is a
# reference document, and it must stay safe to read, diff and paste.
emit_conf '<your rpcpassword -- keep the one already in Hemis.conf>' > "$TEMPLATE"
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
  Params:  $PARAMS_DST
  P2P:     $P2P_PORT      RPC: $RPC_PORT

  ★ ONE GM PER HOST. Do not run a second gamemaster on this machine. The signing
  fan-out dials every member on the SAME port number (ptx/ptx_fanout.cpp:117-120),
  so a second GM here on a different RPC port would register, be selected, and
  silently never receive a signing request. Four GMs means four hosts, each with
  its own internet-routable address and this same port pair.
  The 32000-33000 kernel reservation is host-wide and is set once; re-running is safe.

  NEXT, in order:
    1. Open $P2P_PORT and $RPC_PORT in your firewall AND any NAT/cloud security group.
    2. Follow OPERATOR_GUIDE.md section "Node side" to generate your BLS key and
       send the PUBLIC half to the wallet operator.
    3. Start the daemon, then run:  ./self-check.sh
EOF
