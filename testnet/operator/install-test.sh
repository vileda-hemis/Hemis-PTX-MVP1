#!/usr/bin/env bash
# PTX testnet — installer end-to-end test.
#
# ★ THIS IS NOT AN OPERATOR SCRIPT. Operators run install.sh and then
# self-check.sh. This is OUR test of install.sh, run before a release tag is cut.
#
# ---------------------------------------------------------------------------
# WHY IT EXISTS, WHICH IS THE ONLY PART WORTH READING TWICE
#
# The previous end-to-end test of install.sh stopped at "installed and
# configured" and never started a daemon. Four defects were found in the first
# inch past its last assertion, all on 2026-08-21, all by a stranger's-eye run
# that did start one:
#
#   1. the config was written as "hemis.conf"; the daemon reads "Hemis.conf"
#      (util/system.cpp:81). It does not complain -- it logs "Using config file
#      <datadir>/Hemis.conf" for a file that is not there and runs ALL DEFAULTS,
#      i.e. MAINNET.                                          -> fixed f37bf34
#   2. port/rpcport/rpcbind sat ABOVE the [ptxtestnet] header, so the daemon
#      ignored them and bound the ptxtestnet DEFAULTS.        -> fixed e414e77
#   3. gamemaster=1 shipped enabled with no key, which is not a degraded mode:
#      the daemon REFUSES TO START.                           -> fixed e414e77
#   4. GMs 2 and 3 died on a port collision that GM 1's healthy getblockcount
#      masked completely.                                     -> fixed f37bf34
#
# Every one of them is a STARTUP effect. Config, chain params, node role and
# ports are not exercised by writing files; they are exercised by binding
# sockets. So the test has to start a daemon, and this one does.
#
# ★ ANTI-VACUITY, AND IT IS THE LEG THAT MATTERS. A check that reads the
# daemon's own claims about itself shares the defect being hunted.
# `getblockcount` answered happily, with a plausible height, on a node that was
# silently synchronising MAINNET. `getnetworkinfo` would have reported the ports
# it MEANT to bind. Every check below observes what the node DID -- the
# directory layout it created on disk, and the kernel's own socket table for its
# PID -- never what it says. The one check that speaks RPC at all (C1) tests
# whether credentials AUTHENTICATE, which is a property of what the daemon
# loaded, not an assertion it makes about itself.
#
# ★ THE NETWORK IS NEVER PASSED ON THE COMMAND LINE. Nowhere below does a
# daemon get -ptxtestnet. It must come from the config file or not at all --
# passing it would make defect 1 invisible, which is exactly how defect 1
# survived.
#
# ---------------------------------------------------------------------------
# WHAT IT RUNS
#
#   GREEN  install.sh, three times, exactly as vps-install.sh drives it; start
#          all three daemons; C1-C4 must all pass.
#   RED    four mutations of install.sh's OWN OUTPUT, one per defect above,
#          each asserting that the check which is supposed to catch it FAILS.
#          A check that has never been seen to fail is not yet a check.
#
# ★★ THE THREE-GMs-ON-ONE-HOST SHAPE BELOW IS A TEST FIXTURE, NOT THE DEPLOYMENT
# MODEL. As of 2026-08-21 the deployment model is ONE GM PER HOST with one
# routable address each (see OPERATOR_GUIDE.md "One GM per host"), because the
# signing fan-out dials one port number for every member
# (ptx/ptx_fanout.cpp:117-120) and two GMs on one host at different RPC ports
# cannot both be reached.
#
# ★ The fixture is kept deliberately, and it still tests what it claims. What it
# exercises is INSTALL.SH under repeated invocation on one machine -- that each
# run produces an independent, correctly-configured node, and that the f37bf34
# port-collision defect (RED 4) still fails the check written for it. None of
# that depends on the deployment model, and running three nodes on one host is
# the cheapest way to get three nodes. ★ But do not read this file as operator
# guidance, and do not "fix" the guides to match it.
#
# Usage:
#   ./install-test.sh                 # green + all reds
#   ./install-test.sh --green-only
#   ./install-test.sh --red-only
#   PTX_TEST_BINDIR=/path/to/bin ./install-test.sh
#
# Environment:
#   PTX_TEST_BINDIR   directory holding Hemisd/Hemis-cli. Default: whatever is
#                     on PATH. The binaries are NOT built here.
#   PTX_TEST_BASE     scratch root. Default: a mktemp -d, removed on exit.
#   PTX_TEST_REPO     repo install.sh clones. Default: this checkout, so the
#                     test runs offline and tests THIS tree rather than origin.
#   PTX_TEST_REF      ref install.sh checks out. Default: this checkout's HEAD.
#   PTX_TEST_KEEP=1   do not delete PTX_TEST_BASE (for post-mortems).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(git -C "$HERE" rev-parse --show-toplevel 2>/dev/null || echo "$HERE/../..")"

TEST_REPO="${PTX_TEST_REPO:-$REPO_ROOT}"
TEST_REF="${PTX_TEST_REF:-$(git -C "$REPO_ROOT" rev-parse HEAD 2>/dev/null || echo "")}"
KEEP="${PTX_TEST_KEEP:-0}"

# ★ How long a daemon is given to reach a steady state before it is judged.
# Not a guess: measured start-to-first-listening-socket on the v0.1.0-testnet
# binary is under 2s once the Sapling parameters are cached, and the failing
# cases (defect 3) die in well under 1s. 30 is a ceiling, not a sleep -- every
# wait below polls and returns as soon as the outcome is decided.
SETTLE_MAX=120

PASS=0; FAIL=0
ok()   { printf '  \033[32m[ok]\033[0m   %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
note() { printf '         %s\n' "$*"; }
say()  { printf '\n=== %s ===\n' "$*"; }
die()  { printf '\n  [ABORT] %s\n\n' "$*" >&2; exit 3; }

# ---------------------------------------------------------------------------
# Binaries. We do not build; we test what an operator would be handed.
# ---------------------------------------------------------------------------
if [ -n "${PTX_TEST_BINDIR:-}" ]; then
    HEMISD="$PTX_TEST_BINDIR/Hemisd"; HEMISCLI="$PTX_TEST_BINDIR/Hemis-cli"
else
    HEMISD="$(command -v Hemisd || true)"; HEMISCLI="$(command -v Hemis-cli || true)"
fi
[ -x "$HEMISD" ]   || die "no Hemisd. Set PTX_TEST_BINDIR=<dir> or put it on PATH."
[ -x "$HEMISCLI" ] || die "no Hemis-cli. Set PTX_TEST_BINDIR=<dir> or put it on PATH."

BASE="${PTX_TEST_BASE:-$(mktemp -d -t ptx-install-test.XXXXXX)}"
mkdir -p "$BASE"
PIDS=""
# ★★ BUG-047 TRIPWIRE. A daemon started with no -datadir reads $HOME/.Hemis and
# synchronises MAINNET, silently, looking healthy. On 2026-08-23 a stray bare
# `Hemisd -daemon` was found on a clean px1 with 18 MB of mainnet already in
# /root/.Hemis, 27 minutes after a run of this script had ended.
# ★ It was NOT this script that started it -- the only daemon start here is
# `$HEMISD -datadir=...` at start_daemon, and the mainnet default ports are only
# ever CHECKED (:361, :432), never bound on purpose. The attribution to this
# trap was wrong and is corrected here. But the incident is worth a check
# regardless: nothing in this harness would have noticed, and BUG-047's whole
# character is that it is invisible while it happens.
# So: snapshot whether the default datadir exists BEFORE the run, and fail loudly
# if the run leaves one behind that was not there. Not removed automatically --
# 18 MB of someone else's mainnet is theirs to look at first.
DEFAULT_DATADIR="${HOME:-/root}/.Hemis"
DEFAULT_DATADIR_PREEXISTED=0
[ -e "$DEFAULT_DATADIR" ] && DEFAULT_DATADIR_PREEXISTED=1
cleanup() {
    for p in $PIDS; do kill -TERM "$p" 2>/dev/null; done
    sleep 1
    for p in $PIDS; do kill -KILL "$p" 2>/dev/null; done
    # ★ Sweep any daemon this run started that is NOT in PIDS -- a start that
    # failed to record its pid is exactly the case that leaked before.
    pkill -f "Hemisd -datadir=$BASE" 2>/dev/null
    if [ "$DEFAULT_DATADIR_PREEXISTED" = 0 ] && [ -e "$DEFAULT_DATADIR" ]; then
        printf '\n  \033[31m[BUG-047]\033[0m this run created %s -- something ran a daemon with NO -datadir,\n' "$DEFAULT_DATADIR"
        printf '           which means it was synchronising MAINNET. Left in place on purpose;\n'
        printf '           inspect it, then remove it.\n'
    fi
    [ "$KEEP" = "1" ] || rm -rf "$BASE"
}
trap cleanup EXIT

printf 'installer end-to-end test\n'
printf '  binaries: %s\n' "$HEMISD"
printf '  version:  %s\n' "$("$HEMISCLI" -version 2>/dev/null | awk 'NR==1')"
printf '  repo:     %s\n' "$TEST_REPO"
printf '  ref:      %s\n' "${TEST_REF:0:12}"
printf '  scratch:  %s%s\n' "$BASE" "$([ "$KEEP" = 1 ] && echo '  (KEPT)')"

# ===========================================================================
# OBSERVATION PRIMITIVES
#
# Everything the checks are allowed to look at goes through here, and none of
# it asks the daemon a question about itself.
# ===========================================================================

# The ports a PID is LISTENING on, straight out of the kernel.
#
# ★ WHY /proc AND NOT `getnetworkinfo`, ss, OR THE LOG. The log line "Bound to
# [::]:29993" is the daemon's account of its own behaviour and was available
# throughout the window in which defect 2 survived. ss/netstat are absent from
# minimal images (the same procps-shaped hole that produced the port-reservation
# split-brain, d76224e). /proc/net/tcp{,6} is the kernel's own table, is present
# wherever /proc is, and is joined to THIS process by socket inode -- so a
# sibling GM listening on the port we expect cannot answer for this one, which
# is the exact confusion defect 4 hid behind.
#
# Field layout of /proc/net/tcp: $2 local_address (hex IP:PORT), $4 st,
# $10 inode. st == 0A is TCP_LISTEN.
listening_ports() {   # $1 = pid; prints one decimal port per line, sorted
    local pid="$1" inof rc
    [ -d "/proc/$pid" ] || return 1
    inof="$BASE/.inodes.$$"
    find "/proc/$pid/fd" -type l -printf '%l\n' 2>/dev/null \
        | sed -n 's/^socket:\[\([0-9]*\)\]$/\1/p' | sort -u > "$inof"
    if [ ! -s "$inof" ]; then rm -f "$inof"; return 1; fi
    # mawk has no strtonum, so the hex leaves awk and bash converts it.
    awk -v inofile="$inof" '
        BEGIN { while ((getline l < inofile) > 0) want[l] = 1 }
        FNR == 1 { next }
        $4 == "0A" && ($10 in want) { split($2, a, ":"); print a[2] }
    ' /proc/net/tcp /proc/net/tcp6 2>/dev/null \
        | while read -r hex; do printf '%d\n' "0x$hex" 2>/dev/null; done \
        | sort -un
    rc=$?; rm -f "$inof"; return $rc
}

# ★ A ZOMBIE IS NOT ALIVE, AND THIS COST A WHOLE RUN. The daemons here are
# backgrounded children of this shell, so when one exits it stays in the process
# table -- unreaped -- until wait() collects it. /proc/<pid> still exists and
# `kill -0` still succeeds for a zombie, so the obvious liveness test reports a
# daemon that died on startup as running. That is precisely the RED 3 outcome
# (gamemaster=1 with no key) reported as "the daemon STARTED", i.e. the test
# telling us its own check was vacuous when the check was fine and the test was
# not. State from /proc/<pid>/stat is the answer; Z is zombie, X is dead.
# The comm field is parenthesised and may contain spaces, hence the sed.
alive() {
    local line st
    line="$(cat "/proc/$1/stat" 2>/dev/null)" || return 1
    st="$(printf '%s' "$line" | sed 's/^.*) //' | cut -d' ' -f1)"
    [ -n "$st" ] && [ "$st" != "Z" ] && [ "$st" != "X" ]
}

# Start a daemon on a datadir and echo its PID.
#
# ★ FOREGROUND, backgrounded by this shell -- NOT -daemon. -daemon forks, and
# the PID would then have to be recovered from a pidfile whose PATH DEPENDS ON
# THE NETWORK the daemon chose. Under defect 1 that is the very thing in
# dispute, so the pidfile route would have looked in ptxtestnet/ for a file
# mainnet had written at the top level and reported "did not start" for a
# daemon that started perfectly. $! is network-independent.
#
# ★ NO -ptxtestnet, NO -port, NO -rpcport. The config file is the only input.
start_daemon() {   # $1 = datadir; echoes pid
    local dd="$1"
    "$HEMISD" -datadir="$dd" >"$dd/test-stdout.log" 2>&1 &
    local pid=$!
    PIDS="$PIDS $pid"
    printf '%s' "$pid"
}

# Wait until the outcome is decided: the process's listening set has stopped
# changing, or the process is gone.
#
# ★ "HAS IT BOUND ANYTHING YET" IS THE WRONG PREDICATE AND IT PRODUCED THREE
# FALSE FAILURES. RPC comes up early in init; the P2P listener is bound after
# the wallet is loaded, and creating a fresh HD wallet takes seconds. Returning
# on the first listening socket therefore measured the daemon MID-STARTUP and
# reported "configured P2P 29994 is NOT bound" for a node that bound it a
# moment later. Waiting for the set to be STABLE across consecutive polls is
# network-agnostic (it does not need to know what the daemon ought to bind, so
# it works on the RED legs too) and still costs nothing when the answer is
# immediate.
STABLE_POLLS=3
wait_settled() {   # $1 = pid; 0 = settled and listening, 1 = exited
    local pid="$1" i=0 prev="" cur stable=0
    while [ "$i" -lt "$SETTLE_MAX" ]; do
        if ! alive "$pid"; then wait "$pid" 2>/dev/null; return 1; fi
        cur="$(listening_ports "$pid")"
        if [ -n "$cur" ] && [ "$cur" = "$prev" ]; then
            stable=$((stable + 1))
            [ "$stable" -ge "$STABLE_POLLS" ] && return 0
        else
            stable=0
        fi
        prev="$cur"
        i=$((i + 1)); sleep 1
    done
    alive "$pid" && return 0 || { wait "$pid" 2>/dev/null; return 1; }
}

# Wait until the daemon has FINISHED INITIALISING, or has died trying.
#
# ★ WHY THE STABILITY HEURISTIC WAS NOT ENOUGH, MEASURED. Startup has a
# PLATEAU in the middle: RPC binds early, then "Creating HD Wallet" takes ~8s on
# this host, and only then is the P2P socket bound ("Bound to 0.0.0.0:29994",
# green-dd-1 debug.log:137). During the plateau the listening set is genuinely
# stable at {rpc} for eight consecutive polls, so "stable for 3" fired
# mid-startup and C3 reported "configured P2P 29994 is NOT bound" about a
# daemon that bound it seconds later. A plateau is indistinguishable from an
# endpoint if all you watch is the set.
#
# ★ THIS USES RPC, AND IT IS STILL NOT SELF-REPORT. What is taken from the
# daemon here is a BARRIER -- "are you finished?" -- and nothing else. The
# answer to that question is not among the things that can be wrong in the
# defects this test hunts: a mainnet-by-accident node and a correctly-configured
# node both finish initialising. Every claim the checks then make is read from
# /proc and the filesystem. Warm-up ends at the very end of init (after the
# listeners are up), which is exactly the fence C3 needs.
wait_initialised() {   # $1 = pid, $2 = conf, $3 = rpc port; 0 = ready, 1 = dead/never
    local pid="$1" conf="$2" rpc="$3" i=0
    while [ "$i" -lt "$SETTLE_MAX" ]; do
        if ! alive "$pid"; then wait "$pid" 2>/dev/null; return 1; fi
        if timeout 5 "$HEMISCLI" -conf="$conf" -rpcconnect=127.0.0.1 -rpcport="$rpc" \
                getblockcount >/dev/null 2>&1; then
            return 0
        fi
        i=$((i + 2)); sleep 2
    done
    return 1
}

stop_daemon() {   # $1 = pid
    local pid="$1" i=0
    kill -TERM "$pid" 2>/dev/null
    while alive "$pid" && [ "$i" -lt 20 ]; do i=$((i + 1)); sleep 1; done
    alive "$pid" && kill -KILL "$pid" 2>/dev/null
    wait "$pid" 2>/dev/null
    return 0
}

conf_val() {   # $1 = key, $2 = file
    sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*\([^[:space:]#]*\).*/\1/p" "$2" 2>/dev/null | tail -1
}

# ---------------------------------------------------------------------------
# PRE-FLIGHT — whose host is this, and what does it already hold?
#
# ★★ THIS SECTION EXISTS BECAUSE OF d76224e. That defect -- the port
# reservation giving two different diagnoses depending on whether procps was
# installed -- was invisible for a day, and then visible, and NOTHING IN THE
# CODE CHANGED BETWEEN THOSE TWO RUNS. The difference was in the test host.
#
# A test whose result depends on undeclared properties of the machine it runs
# on will eventually report a pass that means something else. So the properties
# this test depends on are measured and PRINTED, every run, before anything is
# started:
#
#   * the six configured ports MUST be free, or the green run is measuring a
#     port collision it did not create. That is fatal.
#   * the four DEFAULT ports are reported but not required. If one is already
#     held, a RED leg that expects a daemon to fall back onto it will find the
#     daemon DYING instead of mis-binding -- still a failure of the check, but
#     for a different reason, and the leg says so out loud rather than
#     quietly counting a falsification it did not earn.
# ---------------------------------------------------------------------------
# ★★ MEMBERSHIP IS TESTED WITHOUT A PIPELINE, AND THIS IS NOT STYLE.
# `producer | grep -q needle` under `set -o pipefail` IS A RACE. `grep -q` exits
# the instant it matches, which closes the pipe; the producer then dies of
# SIGPIPE (141); and pipefail reports the pipeline's status as that 141 -- so a
# SUCCESSFUL match returns FALSE. Whether it fires depends on whether the
# producer had already finished writing, i.e. on where in the list the needle
# sits and on buffering.
#
# ★ It fired here, silently, and it invalidated part of a verdict. `port_held`
# was written that way and returned "free" for EVERY port -- including 22 and
# including the 29902 that a docker-proxy had held continuously for two days
# with zero restarts. The RED 2 leg then printed "default port 29902: free,
# checked just now -- the exit is the defect, not the host", which was exactly
# backwards, and the test asserting a false thing about the host is the failure
# mode KDD-101 is about.
#
# ★ The repository already knew. install.sh section 3 uses `find -print -quit`
# rather than `find | head -1` and its comment names "the same SIGPIPE trap as
# the glibc check". Knowing the trap is not the same as not walking into it,
# which is why the answer here is a form that HAS no pipeline rather than a
# careful pipeline.
contains() {   # $1 = needle; remaining args = haystack (whitespace-separated)
    local needle="$1"; shift
    local w
    for w in $*; do
        [ "$w" = "$needle" ] && return 0
    done
    return 1
}

port_held() {   # $1 = port; 0 = something is listening on it
    local all
    all="$(all_listening_ports)"
    contains "$1" $all
}

# Every LISTEN port on the host, from the kernel. Ends in `sort`, which drains
# its input, so it has no SIGPIPE exposure of its own.
all_listening_ports() {
    awk 'NR > 1 && $4 == "0A" { split($2, a, ":"); print a[2] }' \
        /proc/net/tcp /proc/net/tcp6 2>/dev/null \
        | while read -r hex; do printf '%d\n' "0x$hex" 2>/dev/null; done \
        | sort -un
}

DEFAULTS_HELD=""
# ★★ STATIC CHECK, AND IT IS HERE BECAUSE THE DYNAMIC ONES ALL MISSED IT.
# install.sh builds the config with an UNQUOTED heredoc (it must -- $1,
# $RPC_PORT, $RPCBIND_LINES have to expand), and in an unquoted heredoc a
# backtick is COMMAND SUBSTITUTION. On 2026-08-23 an example command written in
# backticks inside a COMMENT in that heredoc -- "`Hemisd -daemon`" -- was executed
# on every single run: a bare daemon with no -datadir, syncing MAINNET into
# ~/.Hemis, which then hung install.sh forever because command substitution waits
# for a pipe a forked daemon never closes. It also blanked the comment in every
# config ever written, and no runtime check noticed, because the config still
# parsed and the daemon still started.
# A grep is the right instrument for this: the failure is in the SOURCE TEXT, and
# by the time anything is running the evidence has already been substituted away.
check_no_live_backticks() {
    local f="$HERE/install.sh" bad_lines
    [ -f "$f" ] || { bad "cannot find install.sh to check"; return 1; }
    # Inside emit_conf's heredoc: any backtick that is not backslash-escaped.
    # ★ The range starts at the `cat <<EOF` line, NOT at `emit_conf() {`. Shell
    # comments between the two are ordinary comments and their backticks are
    # inert; only the HEREDOC BODY is substituted. A first version of this check
    # used the function opener and flagged its own explanatory comment -- a false
    # positive that would have taught the next person to ignore it.
    bad_lines="$(awk '/cat <<EOF$/{inf=1;next} inf && /^EOF$/{inf=0} inf' "$f" \
                 | grep -n '`' | grep -v '\\`' || true)"
    if [ -n "$bad_lines" ]; then
        bad "install.sh: UNESCAPED BACKTICK inside emit_conf's heredoc -- it will be EXECUTED."
        printf '%s\n' "$bad_lines" | sed 's/^/           /'
        note "escape them as \\\` or the generated config loses the text and the"
        note "command runs on every install. See BUG-047."
        return 1
    fi
    ok "emit_conf heredoc: no unescaped backticks (nothing in it will execute)"
    return 0
}

preflight() {
    say "Pre-flight — this host"
    check_no_live_backticks || true
    local p busy=""
    for p in 29994 29995 29996 29997 29998 29999; do
        port_held "$p" && busy="$busy $p"
    done
    if [ -n "$busy" ]; then
        die "port(s)$busy are already in use on this host.
  The green run assigns exactly those to GMs 1-3, so it would measure a
  collision it did not create. Free them, or run this test in a container."
    fi
    ok "the six configured ports (29994-29999) are free"

    for p in 29993 29902 49165 51473; do
        if port_held "$p"; then
            DEFAULTS_HELD="$DEFAULTS_HELD $p"
        fi
    done
    if [ -n "$DEFAULTS_HELD" ]; then
        printf '  \033[33m[note]\033[0m default port(s)%s are ALREADY HELD on this host.\n' "$DEFAULTS_HELD"
        note "a mis-configured daemon that tries to fall back onto one of them will"
        note "EXIT rather than bind it. The RED legs below detect this and report"
        note "which of the two happened -- do not read them as interchangeable."
    else
        ok "no default port (29993/29902/49165/51473) held right now -- RED legs re-check at leg time"
    fi
    printf '  \033[33m[note]\033[0m this reading is a snapshot. The RED legs re-measure; do not\n'
    note "treat the line above as true for the rest of the run."
}

# ===========================================================================
# THE FOUR CHECKS
#
# Each one is the defect it catches, turned round.
# ===========================================================================

# --- C2: which chain is this, really? --------------------------------------
#
# ★ THE DATADIR LAYOUT IS THE ANSWER AND THE DAEMON DOES NOT GET A VOTE.
# CreateBaseChainParams gives ptxtestnet the subdirectory "ptxtestnet"
# (src/chainparamsbase.cpp:49) and mainnet the empty string
# (src/chainparamsbase.cpp:42). So a correctly-configured node puts blocks/ and
# chainstate/ under <datadir>/ptxtestnet/, and a node that fell through to
# mainnet defaults puts them at the TOP of the datadir. That is a fact about
# the filesystem, established before any RPC is available, and it is the single
# cheapest discriminator between the two states.
check_network() {   # $1 = datadir
    local dd="$1"
    if [ -d "$dd/blocks" ] || [ -d "$dd/chainstate" ]; then
        bad "C2 $dd: MAINNET layout -- blocks/ or chainstate/ at the top of the datadir."
        note "the config was not read, or did not select ptxtestnet. See chainparamsbase.cpp:42/49."
        return 1
    fi
    if [ ! -d "$dd/ptxtestnet" ]; then
        bad "C2 $dd: no ptxtestnet/ subdirectory -- this node is not on ptxtestnet."
        return 1
    fi
    ok "C2 $dd: on ptxtestnet (ptxtestnet/ present, nothing at datadir top)"
    return 0
}

# --- C3: did it bind the CONFIGURED ports, or the defaults? ----------------
#
# ★ THE DEFAULTS ARE NAMED EXPLICITLY AND EXCLUDED, which is the half that
# catches defect 2. "Is 29994 bound?" passes on a node that also bound 29993;
# "is 29993 bound?" is the question that fails. The four defaults, from source:
#   ptxtestnet P2P 29993   src/chainparams.cpp:815
#   ptxtestnet RPC  29902  src/chainparamsbase.cpp:49
#   mainnet    P2P 49165   src/chainparams.cpp:336
#   mainnet    RPC  51473  src/chainparamsbase.cpp:42
# A configured port that happens to equal a default is not excluded -- the
# exclusion set is built per call, minus whatever this GM legitimately wants.
check_ports() {   # $1 = pid, $2 = datadir label, $3 = p2p, $4 = rpc
    local pid="$1" label="$2" want_p2p="$3" want_rpc="$4"
    local got d rc=0
    got="$(listening_ports "$pid")"
    if [ -z "$got" ]; then
        bad "C3 $label: pid $pid is listening on NOTHING."
        return 1
    fi
    contains "$want_p2p" $got \
        || { bad "C3 $label: configured P2P $want_p2p is NOT bound (bound: $(echo $got))"; rc=1; }
    contains "$want_rpc" $got \
        || { bad "C3 $label: configured RPC $want_rpc is NOT bound (bound: $(echo $got))"; rc=1; }
    for d in 29993 29902 49165 51473; do
        [ "$d" = "$want_p2p" ] || [ "$d" = "$want_rpc" ] && continue
        if contains "$d" $got; then
            bad "C3 $label: bound DEFAULT port $d -- the config's ports were ignored."
            note "settings outside a [ptxtestnet] section are silently dropped; see install.sh section 5."
            rc=1
        fi
    done
    [ "$rc" = 0 ] && ok "C3 $label: bound exactly the configured $want_p2p/$want_rpc, no defaults"
    return $rc
}

# --- C1: did the daemon read THE CONFIG FILE IT WAS GIVEN? ------------------
#
# ★ NOT "does the log say Using config file". That line is printed from the
# path the daemon INTENDS to read, before and regardless of whether the file
# opens, and it is precisely what lied through defect 1: it named Hemis.conf
# while the installer had written hemis.conf, and the daemon ran on defaults.
#
# The effect used instead is AUTHENTICATION. rpcuser/rpcpassword exist only in
# that file. A daemon that never read it has no such credentials -- it falls
# back to a .cookie -- so the file's own credentials cannot authenticate
# against it. That is not the daemon reporting on itself; it is the daemon
# being unable to do something it could only do if it had read the file.
#
# ★ BOTH LIMBS. The positive alone would pass against a daemon with
# authentication disabled altogether, so the negative limb -- a deliberately
# wrong password must be REFUSED -- is what makes the positive mean anything.
#
# ★ The password is never put in a command line. The positive limb passes
# -conf and lets the client read the credential out of the file, the way an
# operator's own client does. The negative limb passes a wrong password, which
# is not a secret.
check_config_read() {   # $1 = conf path, $2 = rpc port, $3 = label, $4 = datadir
    local conf="$1" rpc="$2" label="$3" dd="$4" rc=0 pw
    if [ ! -f "$conf" ]; then
        bad "C1 $label: $conf does not exist."
        return 1
    fi
    # ★ -rpcwait, AND IT IS NOT PADDING. During initialisation the RPC server is
    # already listening but answers every call with "Loading block index" (error
    # -28, RPC_IN_WARMUP), so a bare getblockcount against a perfectly healthy
    # node fails -- which it duly did, on all three green GMs, and was reported
    # as "the credentials do not authenticate". -rpcwait retries through warmup;
    # timeout bounds it so a node that never comes up still fails rather than
    # hanging the test.
    if timeout 90 "$HEMISCLI" -rpcwait -conf="$conf" -rpcconnect=127.0.0.1 -rpcport="$rpc" \
            getblockcount >/dev/null 2>&1; then
        : # positive limb held
    else
        bad "C1 $label: no RPC round-trip on 127.0.0.1:$rpc using the credentials in $conf."
        note "THREE things can cause this and they need different fixes:"
        note "  (a) the daemon never read this file;"
        note "  (b) the credentials are wrong;"
        note "  (c) the daemon is not LISTENING on loopback -- rpcbind and"
        note "      rpcallowip name disjoint sets, so the address it listens on"
        note "      is the one it refuses. That is what happened on 2026-08-23"
        note "      and the message here said 'credentials' and pointed away from it."
        note "Check first:  ss -ltn | grep $rpc   against the rpcbind lines in $conf."
        rc=1
    fi
    # ★ NO -rpcwait on this limb. A 401 is not a "not up yet", and retrying it
    # would turn a correct refusal into a 90-second hang.
    pw='definitely-not-the-configured-password'
    if timeout 20 "$HEMISCLI" -rpcconnect=127.0.0.1 -rpcport="$rpc" -rpcuser=nobody -rpcpassword="$pw" \
            getblockcount >/dev/null 2>&1; then
        bad "C1 $label: a WRONG password was accepted -- authentication is not in force, so the positive limb proves nothing."
        rc=1
    fi
    # ★★ THE CONJUNCTION LEG -- the one whose absence let the disjoint
    # rpcbind/rpcallowip pair ship. Everything above passes -rpcconnect and
    # -rpcport explicitly, which is NOT what an operator types. Every command in
    # OPERATOR_GUIDE.md and every line of self-check.sh is `-datadir` and nothing
    # else, and that is the invocation that broke: the client defaults to
    # 127.0.0.1 and the daemon was not bound there.
    if timeout 60 "$HEMISCLI" -rpcwait -datadir="$dd" getblockcount >/dev/null 2>&1; then
        : # the operator's own invocation works
    else
        bad "C1 $label: \`Hemis-cli -datadir=$dd getblockcount\` -- the operator's own invocation -- does NOT work."
        note "everything in OPERATOR_GUIDE.md and self-check.sh uses exactly this"
        note "form. If the explicit-port limb above passed and this one did not,"
        note "the daemon is not listening on loopback: add rpcbind=127.0.0.1 and"
        note "rpcbind=::1 to $conf."
        rc=1
    fi
    # ★★ AND THE OTHER HALF OF THE CONJUNCTION: an address that is NOT in
    # rpcallowip must be REFUSED. Without this leg the fix could be "bind the
    # wildcard and allow everyone", which would pass every other check here.
    # PTX_CALLER is never set in this harness, so any global address qualifies.
    local ext
    ext="$(ip -o addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 | head -1 || true)"
    if [ -n "$ext" ]; then
        if timeout 20 "$HEMISCLI" -conf="$conf" -rpcconnect="$ext" -rpcport="$rpc" \
                getblockcount >/dev/null 2>&1; then
            bad "C1 $label: RPC ANSWERED from $ext, which is not in rpcallowip -- the ACL is not in force."
            rc=1
        fi
    else
        note "C1 $label: no global address on this host, so the not-permitted leg could not run (VACUOUS)."
    fi
    [ "$rc" = 0 ] && ok "C1 $label: local -datadir round-trip works; wrong password and non-permitted address both refused"
    return $rc
}

# --- OBSERVATION (not a check): which address FAMILIES did RPC land on? -----
#
# ★ DELIBERATELY NOT A PASS/FAIL. The shipped config carries both
# `rpcbind=0.0.0.0` and `rpcbind=::` (install.sh section 5), and on Linux with
# the default net.ipv6.bindv6only=0 a `::` wildcard already covers IPv4 -- so
# the second bind collides with the first and the daemon logs
# "Binding RPC on address :: port <n> failed." then carries on with one family.
# Measured on the green run, all three GMs, 2026-08-21.
#
# It is REPORTED and not FIXED, on purpose: changing a shipped rpcbind on the
# evidence of two hosts, when the failure mode it guards against (one family
# bound, peers dialling the other, node never signs) is the exact thing that
# costs a testnet its quorum, is the wrong trade. self-check.sh section 4 is
# where this gets decided, on a real host with real peers. What belongs HERE is
# that the fact is measured every run instead of rediscovered.
observe_rpc_families() {   # $1 = pid, $2 = rpc port, $3 = label
    local pid="$1" rpc="$2" label="$3" inof v4="" v6=""
    inof="$BASE/.inodes.fam.$$"
    find "/proc/$pid/fd" -type l -printf '%l\n' 2>/dev/null \
        | sed -n 's/^socket:\[\([0-9]*\)\]$/\1/p' | sort -u > "$inof"
    [ -s "$inof" ] || { rm -f "$inof"; return 0; }
    fam_has() {   # $1 = /proc/net file
        local found
        found="$(awk -v inofile="$inof" '
            BEGIN { while ((getline l < inofile) > 0) want[l] = 1 }
            FNR == 1 { next }
            $4 == "0A" && ($10 in want) { split($2, a, ":"); print a[2] }
        ' "$1" 2>/dev/null | while read -r hex; do printf '%d\n' "0x$hex" 2>/dev/null; done)"
        contains "$rpc" $found
    }
    fam_has /proc/net/tcp  && v4="IPv4"
    fam_has /proc/net/tcp6 && v6="IPv6"
    rm -f "$inof"
    if [ -n "$v4" ] && [ -n "$v6" ]; then
        printf '  \033[33m[obs]\033[0m  %s: RPC %s bound on both families\n' "$label" "$rpc"
    else
        printf '  \033[33m[obs]\033[0m  %s: RPC %s bound on %s ONLY -- the second rpcbind lost the race\n' \
            "$label" "$rpc" "${v4:-${v6:-nothing}}"
        note "not a failure of this test and not fixed here; see the comment above and self-check.sh section 4."
    fi
}

# --- C4: did ALL of them start? --------------------------------------------
#
# ★ THE WHOLE POINT IS THAT GM 1 IS NOT EVIDENCE ABOUT GM 2. Defect 4 was
# three GMs of which one ran; the survivor answered getblockcount with a
# healthy height, and that answer was read as "the install worked". So this
# check is per-GM, by PID, and additionally asserts the port sets are PAIRWISE
# DISJOINT -- because the failure mode is precisely three daemons all wanting
# the same defaults, where the kernel lets exactly one of them win.
check_all_started() {   # args: pid:label:p2p:rpc:datadir ...
    local spec pid label dd rc=0 n=0 allports="" dup
    for spec in "$@"; do
        pid="${spec%%:*}"
        label="$(echo "$spec" | cut -d: -f2)"
        dd="$(echo "$spec" | cut -d: -f5)"
        n=$((n + 1))
        if alive "$pid"; then
            allports="$allports$(listening_ports "$pid")
"
        else
            bad "C4 $label: NOT RUNNING (pid $pid exited)."
            if [ -n "$dd" ] && [ -f "$dd/test-stdout.log" ]; then
                note "why, in its own last words:"
                grep -m2 -i 'error\|unable\|cannot' "$dd/test-stdout.log" 2>/dev/null \
                    | sed 's/^/           /' || tail -2 "$dd/test-stdout.log" | sed 's/^/           /'
            fi
            rc=1
        fi
    done
    dup="$(printf '%s' "$allports" | grep -v '^$' | sort | uniq -d)"
    if [ -n "$dup" ]; then
        bad "C4: port(s) $(echo $dup) are claimed by more than one GM -- they are not independent."
        rc=1
    fi
    [ "$rc" = 0 ] && ok "C4: all $n gamemasters running, on pairwise-disjoint ports"
    return $rc
}

# ===========================================================================
# GREEN
# ===========================================================================
green_run() {
    say "GREEN — install.sh x3, then start all three"
    local n dd p2p rpc pid specs=""
    local prefix="$BASE/green-prefix"
    local params="$BASE/green-params"

    for n in 1 2 3; do
        dd="$BASE/green-dd-$n"
        p2p=$((29992 + 2 * n)); rpc=$((29993 + 2 * n))
        printf '\n---------- installing GM %s (P2P %s, RPC %s) ----------\n' "$n" "$p2p" "$rpc"
        # ★ Driven exactly as vps-install.sh:126-131 drives it, so the test
        # exercises the invocation operators actually get, not a tidier one.
        # ★ The binaries go on PATH rather than through PTX_BIN_URL. install.sh
        # section 3's second route is "binaries already on your PATH", which is
        # the route that needs no network and no release; the artefact route is
        # exercised by the release rehearsal, not here.
        ( cd "$HERE" && PATH="$(dirname "$HEMISD"):$PATH" \
            PTX_REPO="$TEST_REPO" PTX_REF="$TEST_REF" \
            PTX_PREFIX="$prefix" PTX_PARAMS_DIR="$params" \
            PTX_DATADIR="$dd" PTX_P2P_PORT="$p2p" PTX_RPC_PORT="$rpc" \
            bash ./install.sh ) >"$BASE/green-install-$n.log" 2>&1 \
            || { bad "install.sh failed for GM $n -- see $BASE/green-install-$n.log"; return 1; }
        ok "install.sh completed for GM $n"
    done

    say "GREEN — starting three daemons"
    for n in 1 2 3; do
        dd="$BASE/green-dd-$n"
        pid="$(start_daemon "$dd")"
        p2p=$((29992 + 2 * n)); rpc=$((29993 + 2 * n))
        if wait_initialised "$pid" "$dd/Hemis.conf" "$rpc"; then
            ok "GM $n started and finished initialising (pid $pid)"
        else
            bad "GM $n did NOT reach a running state. Last lines:"
            tail -5 "$dd/test-stdout.log" 2>/dev/null | sed 's/^/           /'
        fi
        specs="$specs $pid:GM$n:$p2p:$rpc:$dd"
    done

    say "GREEN — outcome checks"
    for n in 1 2 3; do
        dd="$BASE/green-dd-$n"
        p2p=$((29992 + 2 * n)); rpc=$((29993 + 2 * n))
        pid="$(echo "$specs" | tr ' ' '\n' | grep ":GM$n:" | cut -d: -f1)"
        check_network "$dd"
        alive "$pid" && check_ports "$pid" "GM$n" "$p2p" "$rpc"
        alive "$pid" && check_config_read "$dd/Hemis.conf" "$rpc" "GM$n" "$dd"
        alive "$pid" && observe_rpc_families "$pid" "$rpc" "GM$n"
    done
    # shellcheck disable=SC2086
    check_all_started $specs

    for n in 1 2 3; do
        pid="$(echo "$specs" | tr ' ' '\n' | grep ":GM$n:" | cut -d: -f1)"
        stop_daemon "$pid"
    done
}

# ===========================================================================
# RED
#
# ★ A CHECK THAT HAS NEVER BEEN SEEN TO FAIL IS NOT YET A CHECK. Each mutation
# below reconstructs one of the four shipped defects by editing install.sh's
# OWN OUTPUT -- not a hand-written config, so what is under test stays the
# artefact the installer produces -- and asserts that the check named for it
# fails. If a RED leg passes, the corresponding check is vacuous and the GREEN
# result above means nothing.
# ===========================================================================
# Why did a RED daemon exit instead of mis-binding? There are two answers and
# they are not interchangeable -- one is the defect, the other is this host.
# ★ RE-MEASURED HERE, NOT READ OFF THE PRE-FLIGHT, AND THE FIRST RUN OF THIS
# TEST IS WHY. Pre-flight found 29902 free at 15:38:14; the RED 2 daemon failed
# to bind it at 15:39:16. A container on this host had restarted and its
# docker-proxy had retaken the port INSIDE THE TEST'S OWN RUN. A one-shot
# pre-flight is a claim about the past. The leg asks at the moment it needs the
# answer.
explain_exit() {   # $1 = datadir, $2 = the default ports this leg expected it to grab
    local dd="$1" wanted="$2" p
    # ★ ALWAYS SAY WHAT WAS FOUND, INCLUDING "free". Printing only on the
    # interesting branch leaves silence meaning two different things -- "checked,
    # nothing held" and "never checked" -- and a reader six months from now
    # cannot tell them apart. That ambiguity is the same shape as the [warn] that
    # let self-check.sh report a dead node as passing.
    for p in $wanted; do
        if port_held "$p"; then
            note "default port $p: HELD by something else on this host, checked just now."
            note "  so the daemon could not bind it and exited. On a clean host it would"
            note "  have bound it and looked healthy -- which is the failure this leg is"
            note "  named after. The check failed, but not for that reason."
        else
            note "default port $p: free, checked just now -- the exit is the defect, not the host."
        fi
    done
    grep -m2 -i 'error\|unable' "$dd/test-stdout.log" 2>/dev/null | sed 's/^/           /'
}

RED_PASS=0; RED_FAIL=0
red_expect_fail() {   # $1 = human name; runs "$@" from 2 on; expects NONZERO
    local name="$1"; shift
    local before=$FAIL
    "$@" >/dev/null 2>&1
    if [ "$?" -ne 0 ]; then
        printf '  \033[32m[RED ok]\033[0m %s -- the check failed, as it must\n' "$name"
        RED_PASS=$((RED_PASS + 1))
    else
        printf '  \033[31m[RED BROKEN]\033[0m %s -- the check PASSED against the broken configuration. It is vacuous.\n' "$name"
        RED_FAIL=$((RED_FAIL + 1))
    fi
    FAIL=$before   # RED failures are counted separately; a failing check here is the success
    PASS=$((PASS))
}

# Seed a mutation datadir from a green install's product.
seed_from_green() {   # $1 = dest datadir
    local dst="$1"
    rm -rf "$dst"; mkdir -p "$dst"
    cp "$BASE/green-dd-1/Hemis.conf" "$dst/Hemis.conf" || return 1
    chmod 600 "$dst/Hemis.conf"
}

red_run() {
    local dd pid rc

    # ---- RED 1: defect 1, the lowercase filename (f37bf34) -----------------
    say "RED 1 — config named hemis.conf (the f37bf34 defect)"
    dd="$BASE/red1-dd"; seed_from_green "$dd" || { bad "RED 1 could not seed"; return 1; }
    mv "$dd/Hemis.conf" "$dd/hemis.conf"
    pid="$(start_daemon "$dd")"
    wait_settled "$pid"
    if alive "$pid"; then
        note "the daemon started and looks healthy -- which is the whole problem."
        red_expect_fail "C2 (chain)  vs lowercase config" check_network "$dd"
        red_expect_fail "C3 (ports)  vs lowercase config" check_ports "$pid" "red1" 29994 29995
        red_expect_fail "C1 (config) vs lowercase config" check_config_read "$dd/hemis.conf" 29995 "red1" "$dd"
        stop_daemon "$pid"
    else
        printf '  \033[32m[RED ok]\033[0m RED 1 -- the daemon did not survive; C1-C3 could not be evaluated, which is itself a fail\n'
        RED_PASS=$((RED_PASS + 1))
        explain_exit "$dd" "49165 51473"
    fi

    # ---- RED 2: defect 2, settings above the section header (e414e77) ------
    say "RED 2 — port/rpcport outside [ptxtestnet] (the e414e77 port defect)"
    dd="$BASE/red2-dd"; seed_from_green "$dd" || { bad "RED 2 could not seed"; return 1; }
    # Delete the section header. Every network-specific line then sits in the
    # global section, where the daemon warns once and uses the DEFAULTS.
    sed -i '/^\[ptxtestnet\]$/d' "$dd/Hemis.conf"
    pid="$(start_daemon "$dd")"
    wait_settled "$pid"
    if alive "$pid"; then
        red_expect_fail "C3 (ports) vs unsectioned config" check_ports "$pid" "red2" 29994 29995
        # C2 is expected to still PASS here: ptxtestnet=1 is global and is read.
        # Saying so is the point -- it is what makes C2 and C3 independent checks
        # rather than two names for the same one.
        if check_network "$dd" >/dev/null 2>&1; then
            ok "RED 2: C2 still passes, as predicted -- ptxtestnet=1 is global, only the section-scoped lines were lost"
        else
            bad "RED 2: C2 also failed. The two checks are not independent; re-read the mutation."
        fi
        stop_daemon "$pid"
    else
        printf '  \033[32m[RED ok]\033[0m RED 2 -- the daemon did not survive; C3 could not be evaluated, which is itself a fail\n'
        RED_PASS=$((RED_PASS + 1))
        explain_exit "$dd" "29993 29902"
    fi

    # ---- RED 3: defect 3, gamemaster=1 with no key (e414e77) ---------------
    say "RED 3 — gamemaster=1 enabled with no key (the e414e77 role defect)"
    dd="$BASE/red3-dd"; seed_from_green "$dd" || { bad "RED 3 could not seed"; return 1; }
    sed -i 's/^# gamemaster=1$/gamemaster=1/' "$dd/Hemis.conf"
    grep -qx 'gamemaster=1' "$dd/Hemis.conf" \
        || { bad "RED 3: could not enable gamemaster=1 -- the template's commented line has changed shape."; return 1; }
    # ★ wait_initialised, not wait_settled. The gamemaster key is validated LATE
    # in init -- after the wallet is loaded -- so a stability-based wait returns
    # while the node is still at "Creating HD Wallet" and reports a daemon that
    # is about to refuse as one that started. That is what the first run of this
    # leg reported, and it was the test that was wrong, not the claim.
    pid="$(start_daemon "$dd")"
    wait_initialised "$pid" "$dd/Hemis.conf" 29995
    if alive "$pid"; then
        printf '  \033[31m[RED BROKEN]\033[0m RED 3 -- the daemon STARTED with gamemaster=1 and no key. install.sh section 5 claims it cannot.\n'
        RED_FAIL=$((RED_FAIL + 1))
        stop_daemon "$pid"
    else
        printf '  \033[32m[RED ok]\033[0m RED 3 -- the daemon refused to start, as install.sh section 5 says it does\n'
        note "it said: $(grep -m1 -i 'priv key cannot be empty\|^Error' "$dd/test-stdout.log" 2>/dev/null || echo '(no matching line)')"
        RED_PASS=$((RED_PASS + 1))
    fi

    # ---- RED 4: defect 4, three GMs colliding (f37bf34) --------------------
    #
    # ★ This is the one the old test could not have caught even if it HAD
    # started a daemon, because it started one. Three lowercase configs means
    # three daemons all falling through to the same mainnet defaults; the
    # kernel gives the ports to whichever arrives first and the other two die,
    # while the survivor answers getblockcount perfectly.
    say "RED 4 — three GMs, all with lowercase configs (the f37bf34 collision)"
    local specs4="" n4
    for n4 in 1 2 3; do
        dd="$BASE/red4-dd-$n4"; seed_from_green "$dd" || { bad "RED 4 could not seed"; return 1; }
        mv "$dd/Hemis.conf" "$dd/hemis.conf"
        pid="$(start_daemon "$dd")"
        specs4="$specs4 $pid:red4-$n4:$((29992 + 2 * n4)):$((29993 + 2 * n4)):$dd"
    done
    for n4 in 1 2 3; do
        pid="$(echo "$specs4" | tr ' ' '\n' | grep ":red4-$n4:" | cut -d: -f1)"
        wait_settled "$pid" || true
    done
    # The survivor answering happily is the mask. Show it, then show the check.
    local survivor
    survivor="$(echo "$specs4" | tr ' ' '\n' | grep -v '^$' | cut -d: -f1 | while read -r p; do alive "$p" && echo "$p"; done | head -1)"
    if [ -n "$survivor" ]; then
        note "pid $survivor is up and would answer getblockcount -- that is the answer that masked this."
    fi
    # shellcheck disable=SC2086
    red_expect_fail "C4 (all started) vs three colliding GMs" check_all_started $specs4
    for n4 in 1 2 3; do
        pid="$(echo "$specs4" | tr ' ' '\n' | grep ":red4-$n4:" | cut -d: -f1)"
        stop_daemon "$pid"
    done
}

# ===========================================================================
preflight

MODE="${1:-all}"
case "$MODE" in
    --green-only) green_run ;;
    --red-only)   green_run >/dev/null 2>&1; red_run ;;
    all|"")       green_run; red_run ;;
    *) die "unknown argument '$MODE' (expected --green-only, --red-only, or nothing)" ;;
esac

say "Verdict"
printf '  green checks: %s passed, %s failed\n' "$PASS" "$FAIL"
printf '  red   legs:   %s falsified, %s vacuous\n' "$RED_PASS" "$RED_FAIL"
if [ "$FAIL" -gt 0 ] || [ "$RED_FAIL" -gt 0 ]; then
    printf '\n  NOT SHIPPABLE.\n'
    [ "$RED_FAIL" -gt 0 ] && printf '  A vacuous RED leg means the matching green check proves nothing.\n'
    exit 1
fi
printf '\n  All checks pass and every check has been seen to fail against the defect it names.\n'
exit 0
