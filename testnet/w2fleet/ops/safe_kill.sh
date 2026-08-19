#!/bin/bash
# safe_kill.sh <pattern> [signal]
#
# `pkill -f <pattern>` has now killed the invoking shell FOUR times in this arc:
# the shell's own command line contains the pattern, so the pattern matches the
# process doing the matching. It is documented in two places and it kept firing,
# which means a note was the wrong mitigation. This is the guard.
#
# Resolves PIDs by pattern, then EXCLUDES self, parent, the whole process group,
# and anything whose cmdline mentions this helper -- then kills BY PID and says
# what it did. Refuses rather than guesses when nothing matches.
set -u
PAT=${1:?usage: safe_kill.sh <pattern> [signal]}
SIG=${2:-TERM}
SELF=$$
PARENT=$PPID
PGID=$(ps -o pgid= -p $$ | tr -d ' ')

mapfile -t CAND < <(pgrep -f -- "$PAT" 2>/dev/null || true)
declare -A CMDS; VICTIMS=()
for p in "${CAND[@]:-}"; do
    [ -z "$p" ] && continue
    [ "$p" = "$SELF" ] && continue
    [ "$p" = "$PARENT" ] && continue
    [ "$(ps -o pgid= -p "$p" 2>/dev/null | tr -d ' ')" = "$PGID" ] && continue
    # Read the cmdline ONCE and skip if unreadable: pgrep's own subshell exits
    # between the scan and the check, so a transient pid otherwise reaches the
    # kill list and the helper reports killing a process that no longer exists.
    # The redirect failure is reported by the SHELL, not tr, so 2>/dev/null on tr
    # does not silence it -- test readability first.
    [ -r "/proc/$p/cmdline" ] || continue
    cmd=$(tr '\0' ' ' < "/proc/$p/cmdline" 2>/dev/null) || continue
    [ -z "$cmd" ] && continue
    case "$cmd" in *safe_kill.sh*) continue;; esac
    CMDS[$p]="$cmd"
    VICTIMS+=("$p")
done

if [ ${#VICTIMS[@]} -eq 0 ]; then
    echo "safe_kill: no process matches '$PAT' (after excluding self/parent/pgroup)"
    exit 1
fi
for p in "${VICTIMS[@]}"; do
    echo "safe_kill: SIG$SIG -> pid $p : $(echo "${CMDS[$p]}" | cut -c1-90)"
    kill -"$SIG" "$p" 2>/dev/null || echo "  (already gone)"
done
