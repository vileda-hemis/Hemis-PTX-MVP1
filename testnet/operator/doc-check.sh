#!/usr/bin/env bash
#
# Operator-document invariants. ODC-105, with a presence limb.
#
# ★★ WHAT THIS DOES NOT CATCH, SAID HERE RATHER THAN ONLY IN THE REGISTER.
# It checks that required steps are PRESENT, that forbidden instructions are
# ABSENT, and that one ORDERING holds. It does not catch a semantic reversal.
# The disablewallet case had the setting correct in two places and prose arguing
# the opposite in a third -- every token present, every document internally
# consistent, and the meaning contradicted. No structural check finds that; it
# took a bot answering a question with four mutually contradictory sentences.
# ★ ODC-105 was raised because a sweep checked documents against each other and
# never against the installer. A guard that catches step drift and not semantic
# reversal is worth having ONLY if nobody believes it covers the second.
#
# ★★ WHY THIS IS NOT A CROSS-DOCUMENT DIFF, WHICH IS WHAT WAS FIRST PROPOSED.
# Measured: comparing the one-pager's gamemaster sequence against the guide's
# Part A produced EIGHT differences, SIX of them legitimate -- the one-pager
# covers registration end to end where the guide splits it onto the wallet host,
# and `Hemis-cli stop` + `Hemisd -daemon` versus `systemctl restart hemis-ptx`
# are the same operation by different mechanisms. GM_QUICKSTART.md is worse: it
# is a bootstrap path, not a terser version of the same sequence, so demanding
# agreement would demand they match when they should not. Both real defects came
# from NAMED INVARIANTS, not from the diff. The comparison was the expensive part
# and contributed nothing.
#
# ★ Ordering is checked WHOLE-DOCUMENT, deliberately. Delimiting sections needs a
# map the documents do not carry -- a first attempt ran past Part A into Part B
# and picked up `sendtoaddress` -- and a hand-maintained section map is a fourth
# thing to drift, on a guard whose entire purpose is catching drift. Weaker and
# maintainable beats precise and rotting.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/../.." || exit 2

DOCS=(testnet/operator/OPERATOR_ONEPAGER.md testnet/operator/OPERATOR_GUIDE.md
      testnet/operator/ONBOARDING.md GM_QUICKSTART.md vps-install.sh)

# ★ Exemptions are file<TAB>substring, and they are CHECKED: one that stops
# matching is reported, so the allowlist cannot rot into a hole. Same model as
# pin-check.sh, deliberately -- one mechanism, one place to look.
EXEMPT="$(cat <<'EOF'
testnet/operator/ONBOARDING.md	no `-ptxfanoutport` to match
testnet/operator/ONBOARDING.md	wrong fan-out port
testnet/operator/ONBOARDING.md	a dedicated fan-out credential
testnet/operator/ONBOARDING.md	.hemis-spork
EOF
)"
FAIL=0; USED=""
say()  { printf '\n=== %s ===\n' "$*"; }
bad()  { printf '  [FAIL] %s\n' "$*"; FAIL=$((FAIL+1)); }
ok()   { printf '  [ok]   %s\n' "$*"; }

exempt() {  # $1=file $2=line-text -> 0 if exempted
    while IFS=$'\t' read -r ef es; do
        [ -z "${ef:-}" ] && continue
        if [ "$ef" = "$1" ] && [ -n "$es" ] && [[ "$2" == *"$es"* ]]; then
            USED="$USED$ef	$es
"; return 0
        fi
    done <<< "$EXEMPT"
    return 1
}

say "INV-1  generateblskeypair must be preceded by a daemon start"
for f in "${DOCS[@]}"; do
    [ -f "$f" ] || continue
    g=$(grep -n "generateblskeypair" "$f" | head -1 | cut -d: -f1)
    [ -z "$g" ] && continue
    d=$(grep -nE 'Hemisd( -datadir=[^ ]*)? -daemon|systemctl (start|enable --now) hemis-ptx' "$f" | head -1 | cut -d: -f1)
    if [ -z "$d" ]; then
        bad "$f:$g generateblskeypair, and this document never starts a daemon. It is an RPC call -- it needs one listening."
    elif [ "$d" -gt "$g" ]; then
        bad "$f:$g generateblskeypair appears BEFORE the daemon start at :$d. As written this yields 'couldn't connect to server'."
    else
        ok "$f  daemon start :$d precedes generateblskeypair :$g"
    fi
done

say "INV-2  a gamemaster document must say to enable the unit"
for f in "${DOCS[@]}"; do
    [ -f "$f" ] || continue
    grep -qE 'PTX_ROLE=gamemaster|gmoperatorprivatekey' "$f" || continue
    if grep -q 'systemctl enable --now hemis-ptx' "$f"; then
        ok "$f  names 'systemctl enable --now hemis-ptx'"
    else
        bad "$f describes a gamemaster setup but never says to ENABLE the unit. Without it the daemon runs until the next reboot and silently does not come back -- measured on four coordinator hosts, 2026-09-02."
    fi
done

say "INV-3  externalip= must never be given as a line to ADD"
for f in "${DOCS[@]}"; do
    [ -f "$f" ] || continue
    while IFS=: read -r n line; do
        [ -z "${n:-}" ] && continue
        case "$line" in
            *"DO NOT ADD"*|*"already written"*|*"already there"*) continue ;;
        esac
        exempt "$f" "$line" && continue
        bad "$f:$n presents 'externalip=' as a line to add. install.sh writes it; a second copy is a coin-flip at arming. Mark it 'DO NOT ADD' or show a grep instead."
    done < <(grep -nE '^\s*externalip=' "$f")
done
[ "$FAIL" -eq 0 ] && ok "no document tells an operator to add an externalip line"

say "INV-4  retired nouns must not appear unmarked"
RETIRED='@reboot|fan-?out|32000-33000|ptxfanoutport|ip_local_reserved_ports'
MARKER='delet|no longer|used to|gone|retired|removed|vestigial|KDD-085|ODC-106|was |cleanup|stale|not exist|used to say|this paragraph'
n4=0
for f in "${DOCS[@]}"; do
    [ -f "$f" ] || continue
    while IFS=: read -r n line; do
        [ -z "${n:-}" ] && continue
        # ★ THE MARKER IS CHECKED OVER A THREE-LINE WINDOW, NOT THE MATCHING LINE.
        # First version checked only the hit, and fired on vps-install.sh:58 --
        # where "KDD-085 DELETED the fan-out" sits on :57 and the noun wraps onto
        # :58. Prose wraps; a marker that must share a line with its noun is a
        # marker that misses every wrapped sentence.
        ctx="$(sed -n "$((n>1?n-1:1)),$((n+1))p" "$f")"
        printf '%s' "$ctx" | grep -qiE "$MARKER" && continue
        exempt "$f" "$line" && continue
        bad "$f:$n names a retired mechanism with no retirement marker: $(printf '%s' "$line" | cut -c1-70)"
        n4=$((n4+1))
    done < <(grep -niE "$RETIRED" "$f")
done
[ "$n4" -eq 0 ] && ok "every mention of a retired mechanism is marked as retired"

say "INV-5  no document may tell an operator to start the daemon by hand"
# ★★★ THIS INVARIANT WAS REPLACED 2026-09-05, and the replacement is the point.
# It used to require a `Hemis-cli stop` STEP between a documented hand-start and
# `systemctl enable --now`, because those two composed into an enabled unit that
# could not bind the datadir. That rule was correct and it was still not enough:
# ptx004 reached the broken state TWICE anyway -- once at install and once during
# maintenance -- with the stop present in the document and this gate passing.
#
# ★★ The reason is that the failure is not an ordering mistake, it is a HABIT.
# `Hemis-cli stop && Hemisd` is the mainnet restart reflex, and it produces the
# damage with no collision and no error at all: the stop leaves the unit inactive,
# the datadir frees, the hand-start succeeds, and systemd simply no longer owns
# the process. OPERATOR_GUIDE.md literally prescribed that pair at :403.
#
# ★ So the documents no longer hand-start ANYWHERE -- step 8 uses
# `systemctl start hemis-ptx`, and `gamemaster=1` is still commented at that
# point so the unit comes up cleanly. A step that does not exist cannot be
# skipped, and this gate now enforces its absence rather than its ordering.
# Keeping the old check would have been a gate enforcing a rule that no longer
# applies -- BUG-060's shape, one layer up.
for f in "${DOCS[@]}"; do
    [ -f "$f" ] || continue
    # Prescriptions only: a hand-start on its own line, inside a fenced block for
    # markdown. Prose ABOUT the hand-start is how this defect gets explained, and
    # explaining it must stay legal or the documents cannot warn about it.
    # ★ `Hemisd -version` / `-help` are QUERIES, not starts -- they exit immediately
    # and own no datadir. Counting them would make the gate cry wolf on the very
    # command the guide uses to check which build is installed.
    case "$f" in
        *.md) hits=$(awk '
                  /^[[:space:]]*```/ { inb = !inb; next }
                  inb && /^[[:space:]]*(sudo )?Hemisd([[:space:]]|$)/ && !/-version|-help|-\?/ { print NR }' "$f") ;;
        *)    hits=$(awk '/^[[:space:]]*(sudo )?Hemisd([[:space:]]|$)/ && !/-version|-help|-\?/ { print NR }' "$f") ;;
    esac
    # ★ A start against a DISPOSABLE datadir (spork work) is legitimate: there is
    # no unit for it, so there is nothing for it to strand. Those are exemptions
    # rather than pattern holes, so they stay visible and are themselves checked.
    kept=""
    for h in $hits; do
        line=$(sed -n "${h}p" "$f")
        exempt "$f" "$line" || kept="$kept $h"
    done
    hits="$kept"
    if [ -z "${hits// /}" ]; then
        ok "$f  never tells the operator to start the daemon by hand"
    else
        for h in $hits; do
            bad "$f:$h prescribes a hand-start. A daemon started by hand is not owned by systemd: it serves RPC perfectly, survives until the next reboot and then does not come back. Use 'sudo systemctl start hemis-ptx' (or 'restart'). This is the \`Hemis-cli stop && Hemisd\` state that self-check.sh section 1b reports."
        done
    fi
done

say "Exemptions"
while IFS=$'\t' read -r ef es; do
    [ -z "${ef:-}" ] && continue
    if [[ "$USED" == *"$ef	$es"* ]]; then
        ok "exemption still matches: $ef -- \"$es\""
    else
        bad "STALE EXEMPTION: $ef -- \"$es\" matched nothing. Remove it or repair it; an allowlist entry that stops applying is a hole."
    fi
done <<< "$EXEMPT"

say "Verdict"
if [ "$FAIL" -eq 0 ]; then
    printf '  doc-check: OK -- %d documents, 5 invariants, every exemption still matches\n' "${#DOCS[@]}"
    exit 0
fi
printf '  doc-check: %d FAILURE(S)\n' "$FAIL"
exit 1
