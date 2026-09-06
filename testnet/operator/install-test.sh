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
# routable address each (see OPERATOR_GUIDE.md "One GM per host"). ★ The reason
# has CHANGED: it used to be that the signing fan-out dialled one shared RPC port
# so co-hosted GMs could not both be reached. KDD-085 deleted the fan-out. One GM
# per host now stands on co-hosting being untested, not on it being impossible --
# which does not weaken the note below: this fixture is still not the model.
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
# ★★ A THIRD OUTCOME, because two are not enough and this file just proved it.
# The role/unit assertion cannot run without systemd, and a container has none.
# Scored as a pass it would be a lie; scored as a fail it would block a tag on a
# property that is fine. It is NOT PERFORMED -- the same three-state discipline
# self-check.sh uses, and the same one the explorer's check D needs, arrived at
# here by writing a check that genuinely could not run.
UNKNOWN=0
ok()   { printf '  \033[32m[ok]\033[0m   %s\n' "$*"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31m[FAIL]\033[0m %s\n' "$*"; FAIL=$((FAIL+1)); }
note() { printf '         %s\n' "$*"; }
say()  { printf '\n=== %s ===\n' "$*"; }
unk()  { printf '  \033[33m[????]\033[0m %s\n' "$*"; UNKNOWN=$((UNKNOWN + 1)); }
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

# ★★ THIS SUITE CANNOT BE RUN TWICE AT ONCE, and the failure is silent-ish.
# The legs bind FIXED ports (29902/29993/29994/29995 and 29992+2n), so a second
# concurrent run steals them; daemons then die of "Unable to bind to" and their
# RED legs are scored VACUOUS -- a leg that proved nothing, reported as a
# coverage LOSS rather than an error. Measured: two overlapping runs turned
# "8 falsified, 0 vacuous" into "6 falsified, 2 vacuous", which reads exactly
# like a regression in the code under test and cost a full investigation to
# attribute. The "0 vacuous" guard did its job -- this warning names the most
# likely cause so the next reader does not go looking in the wrong place.
if pgrep -x Hemisd >/dev/null 2>&1; then
    printf '\n  \033[33m[WARN]\033[0m a Hemisd is ALREADY RUNNING on this host.\n'
    printf '         This suite binds fixed ports. If that daemon (or another copy of\n'
    printf '         this script) holds them, RED legs will die of the environment and be\n'
    printf '         scored VACUOUS -- which looks like lost coverage, not a port clash.\n'
    printf '         Stop it first, or expect "N vacuous" and read this note again.\n\n'
fi
# ★★ ONE params dir, shared by install.sh (which WRITES it) and start_daemon
# (which must be TOLD it). This was a live defect: green_run passed
# PTX_PARAMS_DIR outside the daemon's HOME, and start_daemon then launched with
# neither -paramsdir nor a redirected HOME, so the daemon looked in the real
# $HOME/.Hemis-params, found nothing, and exited during init with
# "Cannot find the Sapling parameters". ★ The comment above bare_invocation_run
# diagnoses EXACTLY this and says "AND NO PTX_PARAMS_DIR HERE, DELIBERATELY" --
# the fix was applied to that leg and not to its sibling, so the lesson was
# written down next to one instance while the other kept the bug. It only stayed
# hidden because a tester whose own $HOME already had .Hemis-params never saw it.
PARAMS_DIR="$BASE/params"
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
    # ★★★ REMOVE ANY UNIT THIS RUN ENABLED. Since KDD-109 the WALLET role does
    # `systemctl enable --now hemis-ptx`, and the ROLE leg exercises that role --
    # so running this suite ENABLES A ROOT UNIT ON THE TEST HOST, pointed at a
    # scratch datadir this function then deletes. Observed on node1: the unit
    # crash-looped 240 times against a deleted directory, held 29994/29995, and
    # aborted the NEXT run of this very suite with exit 3. The harness made itself
    # unrunnable. ★ Only ever removes a unit whose ExecStart names $BASE -- a real
    # node's unit on a real host must survive this untouched.
    if [ -f /etc/systemd/system/hemis-ptx.service ] \
       && grep -q -- "-datadir=$BASE" /etc/systemd/system/hemis-ptx.service 2>/dev/null; then
        systemctl disable --now hemis-ptx >/dev/null 2>&1
        rm -f /etc/systemd/system/hemis-ptx.service
        systemctl daemon-reload >/dev/null 2>&1
        systemctl reset-failed hemis-ptx >/dev/null 2>&1
        printf '  cleanup: removed the hemis-ptx unit this run enabled (scratch datadir)\n'
    fi
    if [ "$DEFAULT_DATADIR_PREEXISTED" = 0 ] && [ -e "$DEFAULT_DATADIR" ]; then
        # ★ States what it OBSERVED, not what it assumes. The first version said
        # "something ran a daemon ... which means it was synchronising MAINNET".
        # On its first real firing that was wrong twice over: it was Hemis-cli, not
        # Hemisd, and the directory held an empty wallets/ and nothing else. A
        # tripwire that overstates its evidence gets disbelieved the first time it
        # is right.
        printf '\n  \033[31m[BUG-047]\033[0m this run created %s -- something ran WITHOUT -datadir.\n' "$DEFAULT_DATADIR"
        if [ -s "$DEFAULT_DATADIR/debug.log" ] || [ -d "$DEFAULT_DATADIR/blocks" ]; then
            printf '           It has a chain in it, so a DAEMON ran there: that is MAINNET.\n'
        else
            printf '           No chain in it (%s), so this is most likely Hemis-cli, which\n' "$(du -sh "$DEFAULT_DATADIR" 2>/dev/null | cut -f1)"
            printf '           creates the tree merely by being invoked. Still worth removing:\n'
            printf '           it is the directory a later bare Hemisd would sync mainnet into.\n'
        fi
        printf '           Left in place on purpose; inspect it, then remove it.\n'
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
# ★★ ANTI-VACUITY. A leg that judges by "the daemon died" is only meaningful if
# the daemon died of the thing under test. An environment failure -- missing
# sapling params, a busy port, a missing binary -- kills it too, and then a GREEN
# leg reports a defect it did not find and, far worse, a RED leg reports
# [RED ok] for a defect it never actually detected. That is not hypothetical:
# with the params bug above, all four RED legs passed while every daemon was
# dying of LoadSaplingParams, and RED 3 printed the sapling error as its evidence
# that the injected role-defect had been caught. cold-sync-test.sh guards this
# explicitly; this file did not.
ENVIRONMENTAL_DEATH='Cannot find the Sapling parameters|Cannot obtain a lock on data directory|Unable to bind to|No such file or directory'
died_of_environment() {   # $1 = datadir; 0 = yes, it died of the harness
    local dd="$1"
    [ -f "$dd/test-stdout.log" ] || return 1
    grep -qE "$ENVIRONMENTAL_DEATH" "$dd/test-stdout.log" 2>/dev/null
}

# ★ The verdict for any RED leg that judges by "the daemon died". Credit the
# leg ONLY if it died of its own injected defect; if the harness killed it,
# score it VACUOUS -- which is the same word this file's verdict already uses,
# and the honest one: the leg ran and proved nothing.
red_death_verdict() {   # $1 = datadir, $2 = the sentence to print when genuine
    local dd="$1" msg="$2"
    if died_of_environment "$dd"; then
        printf '  \033[31m[RED VACUOUS]\033[0m %s\n' "$msg"
        note "BUT IT DID NOT DIE OF THE INJECTED DEFECT -- the harness killed it:"
        grep -m1 -E "$ENVIRONMENTAL_DEATH" "$dd/test-stdout.log" 2>/dev/null | sed 's/^/           /'
        note "this leg proved NOTHING. Fix the environment and re-run."
        RED_FAIL=$((RED_FAIL + 1))
        return 1
    fi
    printf '  \033[32m[RED ok]\033[0m %s\n' "$msg"
    RED_PASS=$((RED_PASS + 1))
    return 0
}

start_daemon() {   # $1 = datadir; echoes pid
    local dd="$1"
    # ★ -paramsdir is NOT optional here. Without it the daemon resolves
    # $HOME/.Hemis-params while install.sh wrote $PARAMS_DIR, and dies in init
    # for a reason that has nothing to do with what any leg is testing.
    "$HEMISD" -datadir="$dd" -paramsdir="$PARAMS_DIR" >"$dd/test-stdout.log" 2>&1 &
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
    # ★ EVERY unquoted heredoc in the file, not just emit_conf's. The first
    # version of this check only looked at emit_conf and reported clean while the
    # systemd-unit heredoc (<<UNITEOF) was still executing `Hemisd -daemon` three
    # lines apart -- a check scoped to the place the bug was FOUND rather than the
    # place it can LIVE. `<<'"'"'EOF'"'"'` (quoted delimiter) does not substitute and is
    # skipped; only bare <<WORD is a risk.
    bad_lines="$(awk '
        inhd == "" { if (match($0, /<<[A-Za-z_][A-Za-z0-9_]*[ \t]*$/)) {
                         d = substr($0, RSTART+2, RLENGTH-2); gsub(/[ \t]+$/, "", d); inhd = d }
                     next }
        { t = $0; gsub(/^[ \t]+|[ \t]+$/, "", t); if (t == inhd) { inhd = ""; next } }
        /`/ { printf "%d: %s\n", NR, $0 }
    ' "$f" | grep -v '\\`' || true)"
    if [ -n "$bad_lines" ]; then
        bad "install.sh: UNESCAPED BACKTICK inside an unquoted heredoc -- it WILL be executed."
        printf '%s\n' "$bad_lines" | sed 's/^/           /'
        note "escape as \\\` . An example command written in backticks inside a"
        note "COMMENT still runs: that is how install.sh came to start a bare"
        note "mainnet daemon on every install, and then hang. See BUG-047."
        return 1
    fi
    ok "heredocs: no unescaped backticks anywhere (nothing in them will execute)"
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
    # ★ -datadir, even though the credentials are supplied on the command line and
    # override the file. Without it Hemis-cli falls back to the DEFAULT datadir and
    # CREATES $HOME/.Hemis (an empty wallets/ tree) just by being invoked. That is
    # how this harness tripped its own BUG-047 tripwire on the tripwire's first
    # real run -- 8 KB, no debug.log, nothing synced, but the directory a later
    # bare `Hemisd` would happily fill with mainnet.
    if timeout 20 "$HEMISCLI" -datadir="$dd" -rpcconnect=127.0.0.1 -rpcport="$rpc" -rpcuser=nobody -rpcpassword="$pw" \
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
    local params="$PARAMS_DIR"

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
        # ★ PTX_EXTERNALIP: install.sh now REQUIRES a routable IPv6 on a gamemaster
        # (KDD-110) and this harness must run on hosts that have none.
        # ★★ NOT 2001:db8::1. The documentation range is the obvious choice and the
        # daemon REFUSES it: CNetAddr::IsRFC3849() (netaddress.cpp:329) is consulted
        # by IsValid() (:438), so init dies with "Cannot resolve -externalip
        # address" and every green daemon shuts down cleanly -- seven checks
        # failing for a reason that has nothing to do with what they test. Measured
        # both bare and bracketed; brackets are irrelevant, the PREFIX is the issue.
        # A global-unicast address is required, so this uses one from the network's
        # own prefix with an obviously-fake host part. The REFUSAL path is tested
        # separately below.
        # ★★ THIS COMMENT LIVES ABOVE THE COMMAND, NOT INSIDE IT. Put between two
        # backslash-continued lines it is joined onto the same logical line, so
        # everything after the # -- INCLUDING `bash ./install.sh` -- becomes part of
        # the comment. `bash -n` accepts it happily; the install simply never runs,
        # and seven green checks fail somewhere else entirely.
        ( cd "$HERE" && PATH="$(dirname "$HEMISD"):$PATH" \
            PTX_EXTERNALIP=2a07:244:46:6400::ffff \
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
        red_death_verdict "$dd" "RED 1 -- the daemon did not survive; C1-C3 could not be evaluated, which is itself a fail" \
            && explain_exit "$dd" "49165 51473"
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
        red_death_verdict "$dd" "RED 2 -- the daemon did not survive; C3 could not be evaluated, which is itself a fail" \
            && explain_exit "$dd" "29993 29902"
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
        # ★ RED 3 is the sharpest instance: its evidence line greps for /^Error/,
        # so a sapling failure printed "Error: Cannot find the Sapling parameters"
        # AS PROOF that the missing-key defect had been caught.
        if red_death_verdict "$dd" "RED 3 -- the daemon refused to start, as install.sh section 5 says it does"; then
            note "it said: $(grep -m1 -i 'priv key cannot be empty' "$dd/test-stdout.log" 2>/dev/null || echo '(no key-specific line -- suspicious)')"
        fi
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
# ★★ THE BARE-INVOCATION LEG -- BUG-047, AND THE REASON IT WAS INVISIBLE HERE.
#
# Every other leg in this file passes -datadir explicitly, because the fixture
# runs three GMs on one host. That fixture is exactly why the harness could not
# see BUG-047: the defect only exists for an invocation the harness never made.
# On 2026-08-23 two real ones happened on a clean host -- install.sh executing a
# backticked `Hemisd -daemon` out of a heredoc, and a bare Hemis-cli creating the
# tree -- and nothing here noticed either.
#
# ★ HOME is redirected rather than using the real one. GetDefaultDataDir() reads
# getenv("HOME") (util/system.cpp), so pointing HOME at the scratch tree exercises
# the REAL default-datadir code path without writing to the operator's actual
# $HOME/.Hemis. Installing for real would test the same thing and cost the tester
# their own datadir.
# AND NO PTX_PARAMS_DIR HERE, DELIBERATELY. The sapling params are found at
# $HOME/.Hemis-params (ZC_GetBaseParamsDir, util/system.cpp -- it reads HOME the
# same way GetDefaultDataDir does). Pointing PTX_PARAMS_DIR outside the redirected
# HOME put the params where install.sh wrote them and NOT where the daemon looks,
# so LoadSaplingParams failed and the daemon exited during init -- which this leg
# duly reported as "the bare daemon did not stay up": correct, and for a reason
# that had nothing to do with what it was testing. Letting install.sh use its own
# default keeps params and datadir under the same HOME, as a real install does.
bare_invocation_run() {
    say "BARE INVOCATION — no -datadir anywhere (BUG-047)"
    # ★ Separate statements: in a single `local a=X b="$a/y"`, $a is not yet
    # assigned when b is expanded, and under `set -u` that is an unbound-variable
    # abort -- which is exactly how this leg failed on its first run.
    local fh dd pid rc=0
    fh="$BASE/fakehome"
    dd="$fh/.Hemis"
    rm -rf "$fh"; mkdir -p "$fh"
    ( cd "$HERE" && HOME="$fh" PATH="$(dirname "$HEMISD"):$PATH" \
        PTX_EXTERNALIP=2a07:244:46:6400::ffff \
        PTX_REPO="$TEST_REPO" PTX_REF="$TEST_REF" \
        PTX_PREFIX="$BASE/bare-prefix" \
        bash ./install.sh ) >"$BASE/bare-install.log" 2>&1 \
        || { bad "install.sh failed with no PTX_DATADIR -- see $BASE/bare-install.log"; return 1; }
    [ -f "$dd/Hemis.conf" ] \
        && ok "install.sh wrote the config into the DEFAULT datadir ($dd/Hemis.conf)" \
        || { bad "install.sh did not write $dd/Hemis.conf -- a bare daemon will find no config."; return 1; }

    # Bare: no -datadir, no -conf, no -ptxtestnet. Only HOME points anywhere.
    HOME="$fh" "$HEMISD" >"$BASE/bare-stdout.log" 2>&1 &
    pid=$!; PIDS="$PIDS $pid"
    wait_settled "$pid" || { bad "the bare daemon did not stay up"; return 1; }

    local chain
    chain="$(HOME="$fh" timeout 60 "$HEMISCLI" -rpcwait getblockchaininfo 2>/dev/null \
             | sed -n 's/.*"chain"[^"]*"\([^"]*\)".*/\1/p' | head -1)"
    if [ "$chain" = "ptxtestnet" ]; then
        ok "a BARE Hemisd came up on ptxtestnet (chain=\"$chain\")"
    else
        bad "a BARE Hemisd came up on '\''${chain:-<no answer>}'\'', not ptxtestnet. THAT IS BUG-047."
        rc=1
    fi
    # ★ The independent limb: the chain NAME could be right while mainnet data is
    # also on disk. Mainnet lands at the datadir top; ptxtestnet in a subdir.
    if [ -d "$dd/blocks" ]; then
        bad "$dd/blocks exists -- that is MAINNET chain data at the datadir top."
        rc=1
    else
        ok "no mainnet chain data at the datadir top (ptxtestnet/ subdir only)"
    fi
    stop_daemon "$pid"
    return $rc
}

# ---------------------------------------------------------------------------
# ROLE leg. install.sh builds a ROLE (PTX_ROLE=gamemaster|wallet) and the two
# configs differ in exactly three things. ★ This exists because a wrong role is
# the silent failure: a gamemaster built as a wallet registers, syncs, reports
# ENABLED and NEVER RECEIVES A SIGNING REQUEST -- no error, anywhere. A check
# that only ever sees the right answer would not catch that, so the RED half
# below asserts the two roles actually DIFFER rather than that either one parses.
role_run() {
    say "ROLE — PTX_ROLE builds two different configs, and says which"
    local r c fh out rc=0
    for r in gamemaster wallet; do
        fh="$BASE/role-$r"; rm -rf "$fh"; mkdir -p "$fh"
        out="$BASE/role-$r.log"
        # ★ was 203.0.113.9 -- an IPv4 override, which install.sh now refuses for a
        # gamemaster. That refusal is correct and is tested as its own RED leg.
        # (Comment above the command, not inside the continuation -- see the note
        # on the green install for what that costs.)
        ( cd "$HERE" && HOME="$fh" PATH="$(dirname "$HEMISD"):$PATH" \
            PTX_ROLE="$r" PTX_EXTERNALIP=2a07:244:46:6400::ffff \
            PTX_REPO="$TEST_REPO" PTX_REF="$TEST_REF" \
            PTX_PREFIX="$BASE/role-prefix-$r" PTX_PARAMS_DIR="$PARAMS_DIR" \
            bash ./install.sh ) >"$out" 2>&1 \
            || { bad "install.sh failed for PTX_ROLE=$r -- see $out"; return 1; }
        c="$fh/.Hemis/Hemis.conf"
        grep -qE "^# ROLE: $r" "$c" \
            && ok "PTX_ROLE=$r stamped the config (# ROLE: $r) -- self-check can read it back" \
            || { bad "PTX_ROLE=$r wrote no '# ROLE: $r' line; self-check.sh 0b cannot verify the role."; rc=1; }
        grep -qiE "ROLE: +${r}" "$out" \
            && ok "PTX_ROLE=$r announced the role in the output an operator reads" \
            || { bad "PTX_ROLE=$r did not print its role. Silent role selection is the defect this leg exists for."; rc=1; }
    done

    # ★ THE DISCRIMINATING ASSERTION. Both roles installing cleanly proves
    # nothing -- one config for both machines also installs cleanly, which is
    # exactly the state this replaced. The claim is that they DIFFER.
    local gm="$BASE/role-gamemaster/.Hemis/Hemis.conf" wa="$BASE/role-wallet/.Hemis/Hemis.conf"
    # listen is deliberately the SAME in both roles and is pinned here as such:
    # a wallet host listens so it returns peers to a network with no DNS seed.
    # It was briefly listen=0 on a symmetry argument with externalip that did not
    # hold -- externalip advertises for REGISTRATION, listen merely accepts.
    if grep -qE '^listen=1' "$gm" && grep -qE '^listen=1' "$wa"; then
        ok "both roles listen (listen=1) -- a wallet host contributes peers, by decision"
    else
        bad "a role is not listening. Both roles must set listen=1."; rc=1
    fi
    if grep -qE '^externalip=' "$gm" && ! grep -qE '^externalip=' "$wa"; then
        ok "the two roles differ on externalip (gamemaster sets it, wallet does not)"
    else
        bad "externalip does not distinguish the roles -- a wallet host is advertising, or a gamemaster is not."; rc=1
    fi

    # ★★ THE THIRD ROLE DIFFERENCE: a gamemaster has NO WALLET. It signs with a
    # BLS key read from the config, never from a wallet, and it never holds
    # funds. Asserted on the UNCOMMENTED line specifically -- the setting sat in
    # this file as "# disablewallet=1" for months, and a check that merely
    # grepped for the word would have passed against a comment the whole time.
    # ★★ THE ROLE STAMP DECLARES THE ROLE AND NOTHING ELSE. It used to describe
    # the configuration as well ("Wallet ON ..., externalip set"), which is a
    # CACHED COPY of lines a few above it -- and a field host was found whose
    # stamp claimed "externalip set" while externalip was commented and the
    # wallet was off. Nothing reads the prose: self-check 0b takes the role NAME
    # and verifies the rest against the config. So the prose had exactly one
    # audience, a human, and it lied to them.
    local st
    for st in "$gm" "$wa"; do
        local stamp
        stamp="$(grep -oE '^# ROLE: .*' "$st" 2>/dev/null | head -1)"
        [ -n "$stamp" ] || { bad "$(basename "$(dirname "$st")") has no '# ROLE:' stamp"; rc=1; continue; }
        if printf '%s' "$stamp" | grep -qiE 'wallet (on|off)|externalip|listen=|inbound|collateral'; then
            bad "the role stamp describes CONFIGURATION, not just the role: $stamp -- that is a cached copy of lines in the same file and it goes stale the moment an operator edits them."
            rc=1
        fi
    done
    grep -qxE '# ROLE: (gamemaster|wallet)' "$gm" && grep -qxE '# ROLE: (gamemaster|wallet)' "$wa" \
        && ok "the role stamp names the role and asserts nothing else about the config" \
        || { bad "a role stamp is not the bare '# ROLE: <role>' form self-check 0b parses."; rc=1; }

    if grep -qE '^disablewallet=1' "$gm" && ! grep -qE '^disablewallet=1' "$wa"; then
        ok "the two roles differ on the wallet (gamemaster disables it, wallet host keeps it)"
    else
        if grep -qE '^#[[:space:]]*disablewallet' "$gm"; then
            bad "gamemaster has disablewallet COMMENTED OUT -- that is the pre-decision state, not the decision."
        else
            bad "the wallet does not distinguish the roles -- a gamemaster is carrying a wallet, or a wallet host has lost one."
        fi
        rc=1
    fi

    # ★ And the wallet host must still HAVE one, stated positively rather than
    # inferred from the absence above: the collateral lives there.
    if ! grep -qE '^disablewallet' "$wa"; then
        ok "wallet host keeps its wallet -- it holds the collateral and registers the GMs"
    else
        bad "wallet host has a disablewallet line; it cannot hold collateral or register anything."; rc=1
    fi

    # ★ The unit decision is role-shaped, and this asserts the DIFFERENCE rather
    # than either message. In a container systemctl is absent, so the wallet arm
    # takes its warn path; what must hold in BOTH environments is that only the
    # gamemaster arm says "not started" -- that sentence is the GM's key
    # dependency, and a wallet host has no key to wait for.
    # ★ install.sh writes the unit only when /run/systemd/system exists. Without
    # it NEITHER role reaches the role-conditional, so this is unrunnable rather
    # than failing -- say so instead of scoring it.
    if grep -q "no systemd here" "$BASE/role-gamemaster.log"; then
        unk "unit/role posture NOT CHECKED -- no systemd in this environment, so install.sh wrote no unit and neither role reached the decision. Re-run on a systemd host before trusting it."
    elif grep -q "not started" "$BASE/role-gamemaster.log" \
         && ! grep -q "not started" "$BASE/role-wallet.log"; then
        ok "the unit decision follows the role (gamemaster defers to the BLS key; wallet does not)"
        grep -qE "enable --now hemis-ptx" "$BASE/role-gamemaster.log" \
            && ok "the gamemaster arm names BOTH words (enable --now), not just start" \
            || { bad "the gamemaster arm does not tell the operator to ENABLE the unit -- the half people skip, and it costs them the next reboot."; rc=1; }
    else
        bad "both roles report the same unit posture. A wallet host has no key to wait for and must not be told to defer."; rc=1
    fi

    # ★★ RED: the ROLE COLLISION, driven as a real sequence rather than asserted.
    # Install as a wallet, then re-run the SAME datadir as a gamemaster. Before
    # the guard this was silent: the config stayed a wallet's, and the banner
    # said GAMEMASTER. The leg fails if the second run succeeds, and it also
    # fails if it exits for the WRONG reason -- a refusal that does not name the
    # collision sends the operator to debug something else.
    # ★ PTX_EXTERNALIP again: this leg needs a WALLET fixture, and a wallet install
    # now also refuses a host with no routable IPv6 (its seeds are IPv6). The
    # override is the one documented escape and is IPv6-validated, so it serves
    # both roles. Without it this leg cannot build its own fixture and reports a
    # collision failure that has nothing to do with collisions.
    local cfh="$BASE/role-collide"; rm -rf "$cfh"; mkdir -p "$cfh"
    ( cd "$HERE" && HOME="$cfh" PATH="$(dirname "$HEMISD"):$PATH" \
        PTX_EXTERNALIP=2a07:244:46:6400::ffff \
        PTX_ROLE=wallet PTX_REPO="$TEST_REPO" PTX_REF="$TEST_REF" \
        PTX_PREFIX="$BASE/collide-prefix" PTX_PARAMS_DIR="$PARAMS_DIR" \
        bash ./install.sh ) >"$BASE/collide-1.log" 2>&1
    if ! grep -qE '^# ROLE: wallet' "$cfh/.Hemis/Hemis.conf" 2>/dev/null; then
        bad "RED collision: the wallet install did not stamp '# ROLE: wallet'; the leg cannot run."
        rc=1
    else
        ( cd "$HERE" && HOME="$cfh" PATH="$(dirname "$HEMISD"):$PATH" \
            PTX_ROLE=gamemaster PTX_REPO="$TEST_REPO" PTX_REF="$TEST_REF" \
            PTX_PREFIX="$BASE/collide-prefix" PTX_PARAMS_DIR="$PARAMS_DIR" \
            bash ./install.sh ) >"$BASE/collide-2.log" 2>&1
        local crc=$?
        if [ "$crc" = "0" ]; then
            printf '  \033[31m[RED BROKEN]\033[0m gamemaster-after-wallet SUCCEEDED. The config is still a wallet'"'"'s and the banner will claim otherwise.\n'
            RED_FAIL=$((RED_FAIL + 1)); rc=1
        elif grep -qi "ROLE COLLISION" "$BASE/collide-2.log"; then
            printf '  \033[32m[RED ok]\033[0m gamemaster-after-wallet refused, and named the collision (exit %s)\n' "$crc"
            RED_PASS=$((RED_PASS + 1))
            grep -qE '^# ROLE: wallet' "$cfh/.Hemis/Hemis.conf" \
                && ok "the refused run left the existing wallet config untouched" \
                || { bad "the refused run modified the config it refused to convert."; rc=1; }
        else
            printf '  \033[31m[RED BROKEN]\033[0m it exited %s but never named the collision -- the operator debugs the wrong thing.\n' "$crc"
            RED_FAIL=$((RED_FAIL + 1)); rc=1
        fi
    fi

    # ★★ BUG-057 CLASS: EVERY CONFIG KEY WE TELL AN OPERATOR TO WRITE MUST BE ONE
    # THE DAEMON READS. The template shipped `gmoperatorprivatekey` (correct) while
    # three instruction lines said `gamemasterblsprivkey` -- a name that exists
    # NOWHERE in src/. An operator following those got a daemon that starts,
    # reports healthy, and silently is not a gamemaster.
    #
    # ★ It fails silently because the daemon ACCEPTS unknown options: measured,
    # `Hemisd -notarealoption=1` exits 0. Nothing rejects a typo, so nothing but
    # this check will ever catch one.
    #
    # The authoritative list is built from source -- HelpMessageOpt registrations
    # plus every gArgs read -- rather than from `-help`, because `-help` omits
    # debug-category options (rpcworkqueue is real and absent from it, which made
    # a first pass of this sweep report a false positive).
    say "CONFIG KEYS — every key we tell an operator to write must be one the daemon reads"
    local reg="$BASE/argnames.txt"
    { grep -rhoE 'HelpMessageOpt\("-[a-zA-Z0-9._-]+' "$TEST_REPO/src" 2>/dev/null | sed 's/.*"-//'
      grep -rhoE 'gArgs\.(GetArg|GetBoolArg|IsArgSet|GetArgs)\("-[a-zA-Z0-9._-]+' "$TEST_REPO/src" 2>/dev/null | sed 's/.*"-//'
    } | sort -u > "$reg"
    if [ ! -s "$reg" ]; then
        unk "could not build the daemon argument table from $TEST_REPO/src -- key sweep NOT PERFORMED"
    else
        note "daemon argument table: $(grep -c . "$reg") names"
        local badkeys=0 checked=0
        # (a) every key the generated configs emit, live AND commented
        for cfg in "$BASE/role-gamemaster/.Hemis/Hemis.conf" "$BASE/role-wallet/.Hemis/Hemis.conf"; do
            [ -f "$cfg" ] || continue
            while read -r k; do
                [ -z "$k" ] && continue
                checked=$((checked+1))
                grep -qx "$k" "$reg" || { bad "config key '$k' in $cfg is NOT an option the daemon reads"; badkeys=1; }
            done < <(grep -oE '^#? *[a-zA-Z][a-zA-Z0-9._-]*=' "$cfg" | tr -d '# =' | sort -u)
        done
        # (b) ★ the half that would have caught BUG-057: keys our DOCS and SCRIPTS
        #     tell an operator to put in Hemis.conf. The template was right; the
        #     instructions were not.
        for f in "$TEST_REPO/GM_QUICKSTART.md" "$TEST_REPO/vps-install.sh" \
                 "$TEST_REPO/testnet/operator/install.sh" \
                 "$TEST_REPO/testnet/operator/OPERATOR_GUIDE.md"; do
            [ -f "$f" ] || continue
            while read -r k; do
                [ -z "$k" ] && continue
                checked=$((checked+1))
                grep -qx "$k" "$reg" || { bad "$(basename "$f") names config key '$k', which the daemon does not read"; badkeys=1; }
            done < <(grep -oE '\b(gmoperatorprivatekey|gamemasterblsprivkey|gamemaster|externalip|addnode|disablewallet|listen|rpcbind|rpcallowip|rpcuser|rpcpassword|rpcport|rpcworkqueue|rpcthreads|ptxtestnet|port)=' "$f" | tr -d '=' | sort -u)
        done
        [ "$badkeys" = "0" ] && ok "all $checked config-key mentions name real daemon options" || rc=1
    fi

    # ★★ FACT LEG: PORTS AND COLLATERAL, GUARDED BEFORE THEY DRIFT.
    # The tag has pin-check, the register command has the guide/page agreement
    # test, config keys have the leg above -- every fact that has ALREADY drifted
    # has a guard, and the ones that have not drifted yet have none. That is
    # guards written reactively, and it means the next stale-fact bug is
    # guaranteed to be in the unguarded set. These are the two largest.
    #
    # ★ ONE SOURCE OF TRUTH EACH, read rather than restated: the ports come from
    # install.sh's own defaults (the same way pin-check reads REF from it), and
    # the collateral from CPTXTestNetParams in chainparams.cpp. ★ The ports are
    # still read from install.sh rather than chainparams, and that is now a
    # DELIBERATE redundancy rather than a necessity: until 2026-09-06 ptxtestnet
    # defaulted to P2P 29993 while install.sh wrote 29994, so reading chainparams
    # would have guarded the wrong number. BUG-071 made the default 29994 and the
    # two agree -- but install.sh remains the authority here, because what this
    # leg checks is that the DOCUMENTS match what the INSTALLER writes, and
    # sourcing both sides from chainparams would make the check tautological.
    #
    # ★★ SCOPED TO INSTRUCTIONAL USES IN OPERATOR-FACING FILES, and that scoping
    # is the whole design. A naive scan of every number flagged coordinator float
    # figures, this harness's own port fixtures and the deliberate
    # "1000 HMS IS THE WRONG NUMBER" warning -- a linter that cries wolf gets
    # ignored, so it matches port= / rpcport= / ]:port forms and HMS figures
    # written next to the word collateral, and nothing else.
    local a_p2p a_rpc a_coll
    a_p2p="$(sed -n 's/^P2P_PORT="${PTX_P2P_PORT:-\([0-9]*\)}".*/\1/p' "$HERE/install.sh")"
    a_rpc="$(sed -n 's/^RPC_PORT="${PTX_RPC_PORT:-\([0-9]*\)}".*/\1/p' "$HERE/install.sh")"
    a_coll="$(awk '/class CPTXTestNetParams/,/^}/' "$HERE/../../src/chainparams.cpp" 2>/dev/null \
              | sed -n 's/.*nGMCollateralAmt = \([0-9]*\) \* COIN.*/\1/p' | head -1)"
    if [ -z "$a_p2p" ] || [ -z "$a_rpc" ] || [ -z "$a_coll" ]; then
        unk "could not read the authority values (p2p=$a_p2p rpc=$a_rpc coll=$a_coll) -- fact leg DID NOT RUN. Not a pass."
    else
        # Deliberate non-matching mentions, named individually so a blanket skip
        # cannot hide a real one. file <TAB> text that must appear on the line.
        local FACT_EXEMPT
        FACT_EXEMPT="$(printf '%s\n' 'install.sh	Bound to [::]:29993' 'self-check.sh	an explicit port that disagrees')"
        local opfiles ff badfact=0 nfact=0
        opfiles="GM_QUICKSTART.md vps-install.sh testnet/operator/OPERATOR_GUIDE.md"
        opfiles="$opfiles testnet/operator/OPERATOR_ONEPAGER.md testnet/operator/install.sh"
        opfiles="$opfiles testnet/operator/self-check.sh"
        # ★ The AUTHORED corpus carries operator-facing numbers (100 HMS, 29994)
        # and must be guarded exactly like the guide. The DERIVED copies are not
        # listed: they are byte-copies of files already in this list, and the
        # staleness leg below enforces that equality -- checking them here would
        # double-report the same fact under a second path.
        opfiles="$opfiles testnet/operator/faq/weirdness.md"
        opfiles="$opfiles testnet/operator/faq/protocol.md"
        for ff in $opfiles; do
            [ -f "$HERE/../../$ff" ] || continue
            while IFS=: read -r ln txt; do
                [ -n "${ln:-}" ] || continue
                local v ex=""
                v="$(printf '%s' "$txt" | grep -oE '299[0-9][0-9]' | head -1)"
                [ "$v" = "$a_p2p" ] || [ "$v" = "$a_rpc" ] || {
                    while IFS=$'\t' read -r hf ht; do
                        [ -n "${hf:-}" ] || continue
                        case "$ff" in *"$hf") : ;; *) continue ;; esac
                        printf '%s' "$txt" | grep -qF -- "$ht" && ex=1 && break
                    done <<< "$FACT_EXEMPT"
                    [ -n "$ex" ] || { bad "$ff:$ln names port $v; install.sh's default is $a_p2p/$a_rpc"; badfact=1; }
                }
                nfact=$((nfact+1))
            # ★ WHOLE LINES, not grep -o matches. -o yields only the matched
            # token (]:29993), so an exemption keyed on the surrounding text --
            # which is the only thing that distinguishes a quoted measurement from
            # a stale instruction -- could never match, and the exemption silently
            # did nothing. Caught because the GREEN run failed on the one line the
            # exemption existed for.
            done < <(grep -nE "(rpcport|port)=299[0-9][0-9]|\]:299[0-9][0-9]|[0-9]:299[0-9][0-9]" "$HERE/../../$ff" 2>/dev/null)
            while IFS=: read -r ln txt; do
                [ -n "${ln:-}" ] || continue
                local cv
                # ★ A FUNDING FORMULA STATES A TOTAL, NOT A COLLATERAL. "(N x 100)
                # + 500 HMS" would otherwise be read as a 500 HMS collateral. It is
                # not skipped, though -- the MULTIPLICAND in it IS the collateral,
                # so that is what gets checked, which is stricter than skipping and
                # stricter than the prose check it replaces.
                # ★ Skip funding-formula lines here; they are checked by their own
                # scan below. "(N x 100) + 500 HMS -- the collaterals, plus 500 to
                # stake" reads to this loop as a 500 HMS collateral, which it is
                # not. A formula states a TOTAL.
                printf '%s' "$txt" | grep -qE '[Nn] *[×x] *[0-9]+' && continue
                cv="$(printf '%s' "$txt" | grep -oE '[0-9]+ HMS' | head -1)"
                [ -z "$cv" ] || [ "$cv" = "$a_coll HMS" ] || {
                    bad "$ff:$ln states $cv beside the word collateral; chainparams says $a_coll HMS"; badfact=1; }
                [ -z "$cv" ] || nfact=$((nfact+1))
            done < <(grep -noiE ".{0,40}collateral.{0,40}" "$HERE/../../$ff" 2>/dev/null)
            # ★★ FUNDING FORMULAS GET THEIR OWN SCAN, and they need one: a formula
            # is NOT written next to the word "collateral" -- the one-pager's sits
            # beside "for N gamemasters" -- so a check living inside the
            # collateral-context loop never sees it. Measured: a deliberately
            # broken "(N x 1000)" passed until this scan existed.
            # The MULTIPLICAND is the collateral, so that is what is checked; the
            # margin (+500, for staking) is a policy number with no source of
            # truth in code and is deliberately NOT guarded.
            while IFS=: read -r ln txt; do
                [ -n "${ln:-}" ] || continue
                local mult
                mult="$(printf '%s' "$txt" | grep -oE '[Nn] *[×x] *[0-9]+' | grep -oE '[0-9]+' | head -1)"
                [ -z "$mult" ] || [ "$mult" = "$a_coll" ] || {
                    bad "$ff:$ln funding formula multiplies by $mult; the collateral is $a_coll"; badfact=1; }
                [ -z "$mult" ] || nfact=$((nfact+1))
            done < <(grep -nE "[Nn] *[×x] *[0-9]+" "$HERE/../../$ff" 2>/dev/null)
        done
        [ "$badfact" = "0" ] \
            && ok "ports and collateral: $nfact instructional mentions all match install.sh ($a_p2p/$a_rpc) and chainparams ($a_coll HMS)" \
            || rc=1
    fi

    # ★★ OPERATOR-DOCUMENT INVARIANTS (ODC-105 + the presence limb). Delegated to
    # doc-check.sh rather than reimplemented here, so there is ONE exemption list
    # and one place to look when it fires. ★ It checks presence, absence and one
    # ordering -- it does NOT catch a semantic reversal, and its own header says so.
    if [ -x "$HERE/doc-check.sh" ]; then
        local dcout
        if dcout="$(bash "$HERE/doc-check.sh" 2>&1)"; then
            ok "operator documents: 5 invariants hold ($(printf '%s' "$dcout" | grep -c '\[ok\]') checks)"
        else
            printf '%s\n' "$dcout" | grep '\[FAIL\]' | sed 's/^/      /'
            bad "doc-check.sh: operator-document invariants FAILED -- see above"
            rc=1
        fi
    else
        unk "doc-check.sh not executable -- the document invariants DID NOT RUN. Not a pass."
    fi

    # ★★ FAQ CORPUS STALENESS. The corpus's derived half is a BYTE-COPY of the
    # operator documents, which is the only form of "derived" that cannot drift
    # semantically -- the text either equals the source or this fails. Copying by
    # hand, or paraphrasing into the corpus, would recreate the second-source
    # problem the copy exists to avoid (BUG-054's shape, four instances in a
    # week).
    local man="$HERE/faq/derived/MANIFEST.txt"
    if [ ! -f "$man" ]; then
        unk "no FAQ corpus manifest -- the staleness check DID NOT RUN. Not a pass."
    else
        local stale=0 nsrc=0 want got src
        while read -r want src; do
            [ -n "${src:-}" ] || continue
            nsrc=$((nsrc+1))
            if [ ! -f "$HERE/../../$src" ]; then
                bad "corpus manifest names $src, which does not exist"; stale=1; continue
            fi
            got="$(sha256sum "$HERE/../../$src" | awk '{print $1}')"
            [ "$got" = "$want" ] || {
                bad "corpus is STALE against $src -- re-run testnet/operator/faq/build-corpus.sh"; stale=1; }
        done < "$man"
        # ★ And the copy must equal the source BYTE FOR BYTE, not merely have been
        # generated from a matching hash at some point: a hand-edit to the copy
        # would leave the manifest correct and the corpus wrong.
        local d body
        for d in "$HERE"/faq/derived/*.md; do
            [ -f "$d" ] || continue
            src="$(sed -n 's/^<!-- CORPUS-SOURCE: \(.*\) -->$/\1/p' "$d" | head -1)"
            [ -n "$src" ] || { bad "$(basename "$d") has no CORPUS-SOURCE header"; stale=1; continue; }
            [ -f "$HERE/../../$src" ] || continue
            body="$(sed -n '/^> edited for the FAQ bot/,$p' "$d" | tail -n +3)"
            if [ "$body" != "$(cat "$HERE/../../$src")" ]; then
                bad "$(basename "$d") is not a byte-copy of $src -- it has been edited in place"; stale=1
            fi
        done
        [ "$stale" = "0" ] \
            && ok "FAQ corpus: $nsrc derived documents are byte-identical to their sources" \
            || rc=1
    fi

    # RED: an unknown role must ABORT, never fall back to a default.
    if ( cd "$HERE" && HOME="$BASE/role-bad" PTX_ROLE=nonsense bash ./install.sh ) >"$BASE/role-bad.log" 2>&1; then
        printf '  \033[31m[RED BROKEN]\033[0m PTX_ROLE=nonsense was ACCEPTED. An unrecognised role must abort, not silently pick one.\n'
        RED_FAIL=$((RED_FAIL + 1)); rc=1
    else
        printf '  \033[32m[RED ok]\033[0m PTX_ROLE=nonsense aborted instead of defaulting\n'
        RED_PASS=$((RED_PASS + 1))
    fi

    # ★★ RED: an IPv4 external address must be REFUSED for a gamemaster, and
    # refused BEFORE anything is written. Signing is point-to-point and no relay
    # bridges address families (KDD-110), so an IPv4-registered gamemaster syncs,
    # reports Ready, and is invisible. This leg runs anywhere -- it depends on the
    # OVERRIDE, not on what addresses the test host happens to have.
    local v4home="$BASE/role-v4" v4pre="$BASE/role-v4-prefix"
    rm -rf "$v4home" "$v4pre"; mkdir -p "$v4home"
    if ( cd "$HERE" && HOME="$v4home" PTX_ROLE=gamemaster PTX_EXTERNALIP=203.0.113.9 \
         PTX_PREFIX="$v4pre" bash ./install.sh ) >"$BASE/role-v4.log" 2>&1; then
        printf '  \033[31m[RED BROKEN]\033[0m an IPv4 PTX_EXTERNALIP was ACCEPTED for a gamemaster.\n'
        RED_FAIL=$((RED_FAIL + 1)); rc=1
    else
        if grep -q "not an IPv6 address" "$BASE/role-v4.log" 2>/dev/null; then
            printf '  \033[32m[RED ok]\033[0m IPv4 external address refused for a gamemaster, and the message says why\n'
            RED_PASS=$((RED_PASS + 1))
        else
            printf '  \033[31m[RED VACUOUS]\033[0m it aborted, but NOT for the IPv6 reason -- see %s\n' "$BASE/role-v4.log"
            RED_FAIL=$((RED_FAIL + 1)); rc=1
        fi
        # ★ "aborts before writing anything" is a CLAIM IN THE MESSAGE, so it is
        # checked rather than trusted: a half-configured host is worse than none.
        if [ ! -e "$v4pre" ]; then
            ok "the refused gamemaster install wrote nothing -- the host is left clean"
        else
            bad "the refused install created $v4pre. It must abort BEFORE writing."; rc=1
        fi
    fi

    # ★ RED: no routable IPv6 at all must refuse. Only testable on a host that
    # HAS none -- three states, and a host with IPv6 reports NOT PERFORMED rather
    # than passing a leg it never ran.
    local hostv6
    hostv6="$(ip -o -6 addr show scope global 2>/dev/null | awk '{print $4}' | cut -d/ -f1 \
              | grep -viE '^(fc|fd|fe80)' | grep -vxE '::1' | sort -u || true)"
    if [ -n "$hostv6" ]; then
        unk "this host HAS routable IPv6 ($(printf '%s' "$hostv6" | tr '\n' ' ')) so the no-IPv6 refusal DID NOT RUN. Not a pass."
    else
        # ★★ BOTH ROLES, and the messages must DIFFER. A gamemaster is refused
        # because signing is point-to-point and it would be invisible to callers;
        # a wallet host is refused because the seed peers are IPv6 and it would
        # have nothing to dial. Same outcome, different cause -- and an operator
        # who is handed the wrong explanation goes looking in the wrong place, so
        # this leg asserts the ROLE-SPECIFIC sentence, not just that it aborted.
        local r6
        for r6 in gamemaster wallet; do
            local nohome="$BASE/role-nov6-$r6" nopre="$BASE/role-nov6-$r6-prefix"
            rm -rf "$nohome" "$nopre"; mkdir -p "$nohome"
            if ( cd "$HERE" && HOME="$nohome" PTX_ROLE="$r6" PTX_PREFIX="$nopre" \
                 bash ./install.sh ) >"$BASE/role-nov6-$r6.log" 2>&1; then
                printf '  \033[31m[RED BROKEN]\033[0m %s install SUCCEEDED on a host with no routable IPv6.\n' "$r6"
                RED_FAIL=$((RED_FAIL + 1)); rc=1
                continue
            fi
            if ! grep -q "No global IPv6 address found" "$BASE/role-nov6-$r6.log" 2>/dev/null; then
                printf '  \033[31m[RED VACUOUS]\033[0m %s aborted for some other reason -- see %s\n' \
                    "$r6" "$BASE/role-nov6-$r6.log"
                RED_FAIL=$((RED_FAIL + 1)); rc=1
                continue
            fi
            # the role-specific reason, which is the part an operator acts on
            if [ "$r6" = "gamemaster" ]; then
                grep -q "point-to-point" "$BASE/role-nov6-$r6.log" \
                    && printf '  \033[32m[RED ok]\033[0m no-IPv6 GAMEMASTER refused, and the reason given is signing reachability\n' \
                    || { printf '  \033[31m[RED VACUOUS]\033[0m gamemaster refusal did not explain WHY (no point-to-point reason)\n'; RED_FAIL=$((RED_FAIL+1)); rc=1; continue; }
            else
                grep -q "seed peers" "$BASE/role-nov6-$r6.log" \
                    && printf '  \033[32m[RED ok]\033[0m no-IPv6 WALLET host refused, and the reason given is the IPv6 seed peers\n' \
                    || { printf '  \033[31m[RED VACUOUS]\033[0m wallet refusal gave the gamemaster reason, which sends an operator the wrong way\n'; RED_FAIL=$((RED_FAIL+1)); rc=1; continue; }
            fi
            RED_PASS=$((RED_PASS + 1))
            [ ! -e "$nopre" ] && ok "the refused $r6 install wrote nothing" \
                || { bad "the no-IPv6 $r6 refusal created $nopre"; rc=1; }
        done
    fi
    return $rc
}

preflight

MODE="${1:-all}"
case "$MODE" in
    --green-only) green_run ;;
    --red-only)   green_run >/dev/null 2>&1; red_run ;;
    all|"")       green_run; bare_invocation_run || true; role_run || true; red_run ;;
    *) die "unknown argument '$MODE' (expected --green-only, --red-only, or nothing)" ;;
esac

say "Verdict"
printf '  green checks: %s passed, %s failed\n' "$PASS" "$FAIL"
printf '  red   legs:   %s falsified, %s vacuous\n' "$RED_PASS" "$RED_FAIL"
[ "$UNKNOWN" -gt 0 ] && printf '  not performed: %s  <- NOT passes. Re-run where they can run.\n' "$UNKNOWN"
if [ "$FAIL" -gt 0 ] || [ "$RED_FAIL" -gt 0 ]; then
    printf '\n  NOT SHIPPABLE.\n'
    [ "$RED_FAIL" -gt 0 ] && printf '  A vacuous RED leg means the matching green check proves nothing.\n'
    exit 1
fi
# ★ "Nothing failed" and "everything was checked" are different sentences, and
# only one of them is earned when a check could not run. Do not print the
# stronger one on the strength of the weaker.
if [ "$UNKNOWN" -gt 0 ]; then
    printf '\n  No failures -- but %s check(s) COULD NOT RUN and are not passes.\n' "$UNKNOWN"
    printf '  Shippable on this evidence only if those are known-unrunnable here.\n'
    exit 0
fi
printf '\n  All checks pass and every check has been seen to fail against the defect it names.\n'
exit 0
