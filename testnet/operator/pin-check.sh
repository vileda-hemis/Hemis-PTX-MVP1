#!/usr/bin/env bash
#
# pin-check.sh -- every release-tag pin in the operator-executable surface names
# the SAME tag, and every exception to that is DECLARED.
#
# ★ WHY THIS IS A CHECK AND NOT A CHECKLIST. The release procedure used to carry
# a hand-maintained list of pin sites. It went stale the way any pin goes stale:
# the v0.1.1 cut repinned install.sh and never touched vps-install.sh or
# GM_QUICKSTART.md, so the operator's literal FIRST command kept fetching a
# superseded build -- silently, because a spent tag still resolves (BUG-054).
# A list of pin sites is itself a pin site. This walks the tree instead.
#
# ★ THE FILE LIST IS DISCOVERED, NOT WRITTEN. It comes from `git ls-files`, so a
# new file under testnet/operator/ is covered the day it lands. Nothing to update.
#
# ★ EXEMPTIONS ARE CHECKED TOO. A historical mention that stops matching is
# reported as a STALE EXEMPTION -- so the allowlist cannot rot unnoticed either,
# which is the failure this script exists to prevent.
#
# Usage:  testnet/operator/pin-check.sh [expected-tag]
#         (default: whatever install.sh:22's REF fallback names)
# Exit:   0 = every pin agrees   1 = findings   2 = could not run
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

TAG_RE='v[0-9]+\.[0-9]+\.[0-9]+-testnet[a-zA-Z0-9.-]*'

EXPECTED="${1:-}"
if [ -z "$EXPECTED" ]; then
    EXPECTED="$(sed -n 's/^REF="${PTX_REF:-\([^}]*\)}".*/\1/p' testnet/operator/install.sh | head -1)"
fi
if [ -z "$EXPECTED" ]; then
    echo "pin-check: could not read the expected tag from testnet/operator/install.sh" >&2
    exit 2
fi

# Deliberately-historical mentions.  file <TAB> regex-matching-the-line.
# These record measurements taken against an older release; a blanket replace
# falsifies them, which is why they are named individually rather than skipped
# by file.
HISTORICAL="$(cat <<'EOF'
testnet/operator/install-test.sh	measured start-to-first-listening-socket
testnet/operator/install.sh	release binary on 2026-08-21
testnet/operator/ONBOARDING.md	ptx01 runs
vps-install.sh	CANNOT work here
EOF
)"

mapfile -t FILES < <(git ls-files GM_QUICKSTART.md vps-install.sh 'testnet/operator/*')
if [ "${#FILES[@]}" -eq 0 ]; then
    echo "pin-check: no files matched -- run me from inside the repository" >&2
    exit 2
fi

echo "pin-check: expected tag '$EXPECTED' across ${#FILES[@]} files"

rc=0
declare -A seen_exemption

while IFS= read -r hit; do
    [ -n "$hit" ] || continue
    f="${hit%%:*}"; rest="${hit#*:}"; ln="${rest%%:*}"; txt="${rest#*:}"

    exempt=""
    while IFS=$'\t' read -r hf hre; do
        [ -n "${hf:-}" ] || continue
        if [ "$hf" = "$f" ] && printf '%s' "$txt" | grep -qF -- "$hre"; then
            exempt="${hf}|${hre}"
            break
        fi
    done <<< "$HISTORICAL"

    if [ -n "$exempt" ]; then
        seen_exemption["$exempt"]=1
        continue
    fi

    while IFS= read -r t; do
        [ -n "$t" ] || continue
        if [ "$t" != "$EXPECTED" ]; then
            printf 'STALE PIN        %s:%s names %s, expected %s\n' "$f" "$ln" "$t" "$EXPECTED"
            printf '                 %s\n' "$txt"
            rc=1
        fi
    done < <(printf '%s' "$txt" | grep -oE "$TAG_RE" | sort -u)
done < <(grep -HnE "$TAG_RE" "${FILES[@]}" 2>/dev/null || true)

# An exemption that matches nothing is itself stale.
while IFS=$'\t' read -r hf hre; do
    [ -n "${hf:-}" ] || continue
    if [ -z "${seen_exemption["${hf}|${hre}"]:-}" ]; then
        printf 'STALE EXEMPTION  %s -- "%s" matched no line; remove it or repair it\n' "$hf" "$hre"
        rc=1
    fi
done <<< "$HISTORICAL"

# ---------------------------------------------------------------------------
# ★★ THE TAG ITSELF MUST BE ANNOTATED, AND IT MUST EXIST BEFORE THE BUILD.
# ---------------------------------------------------------------------------
# ODC-092. `share/genbuild.sh` overrides the numeric client version with the tag
# NAME -- but only via `git describe --abbrev=0`, WITHOUT `--tags`, which sees
# annotated tags only. A lightweight tag is invisible to it, `describe` falls
# through to the nearest annotated tag (`first-quorum`), the HEAD comparison
# fails, and every build reports `v1.3.1.0-<commit>` instead of the release name.
#
# ★ That is not cosmetic: the operator instruction is "verify you are on
# <tag>", and a binary that can only print a commit hash cannot answer it. Two
# separate investigations this week were spent reconciling a version string with
# a tag name because of exactly this.
#
# ★★ AND ANNOTATED IS NOT SUFFICIENT ON ITS OWN -- ORDER IS THE OTHER HALF.
# The release workflow BUILDS (build-and-release.yml:401-446) and only then
# creates the tag (:490, action-gh-release). At build time no tag exists at HEAD,
# so genbuild cannot see one however it is created. The tag must be pushed FIRST
# and the workflow dispatched from it. This check catches the annotated half; the
# ordering half is a procedure, written down in PTX_TESTNET_RELEASE.md §3.
check_tag_object() {
    local t="$1" kind=""
    if git rev-parse -q --verify "refs/tags/$t" >/dev/null 2>&1; then
        kind="$(git cat-file -t "$t" 2>/dev/null)"
        if [ "$kind" = "tag" ]; then
            echo "pin-check: tag $t is ANNOTATED -- genbuild will print the tag name"
            return 0
        fi
        printf 'LIGHTWEIGHT TAG  %s is a lightweight tag (cat-file -t = %s).\n' "$t" "$kind"
        printf '                 genbuild.sh cannot see it, so binaries built from it report\n'
        printf '                 v1.3.1.0-<commit> and NOT %s. Re-create it with: git tag -a %s\n' "$t" "$t"
        return 1
    fi
    echo "pin-check: tag $t does not exist yet (normal before a cut)."
    echo "           When you create it: git tag -a $t -m '...' && git push origin $t"
    echo "           ANNOTATED, and pushed BEFORE dispatching the release workflow --"
    echo "           the workflow builds before it would create the tag (ODC-092)."
    return 0
}
check_tag_object "$EXPECTED" || rc=1

if [ "$rc" -eq 0 ]; then
    echo "pin-check: OK -- every pin names $EXPECTED, every exemption still matches"
fi
exit "$rc"
