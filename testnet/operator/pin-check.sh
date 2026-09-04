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
testnet/operator/pin-check.sh	git clone -b v0.3.2-testnet
testnet/operator/pin-check.sh	v0.3.3-testnet went out with
testnet/operator/faq/weirdness.md	and later, where `install.sh` enforces this
testnet/operator/faq/weirdness.md	its instructions told me to clone
testnet/operator/faq/questions.md	its instructions told me to clone
testnet/operator/OPERATOR_GUIDE.md	BUG-059 — fixed in
testnet/operator/faq/weirdness.md	BUG-060 and fixed in
testnet/operator/faq/weirdness.md	or later**. If you already installed
testnet/operator/faq/weirdness.md	tag only. Fixed in
testnet/operator/faq/weirdness.md	shipped with a one-page onboarding document
testnet/operator/OPERATOR_GUIDE.md	and earlier** it fails from
testnet/operator/OPERATOR_GUIDE.md	made gamemasters ship
EOF
)"

PIN_GLOBS=(GM_QUICKSTART.md vps-install.sh 'testnet/operator/*')
mapfile -t FILES < <(git ls-files "${PIN_GLOBS[@]}" | grep -v '^testnet/operator/faq/derived/')
# ★★ faq/derived/ IS EXCLUDED FROM CONTENT SCANNING, AND THIS IS NOT A HOLE.
# Those files are BYTE-COPIES of documents this script already scans at their
# source paths, and build-corpus.sh embeds each source's SHA256 while
# install-test.sh fails if any has moved. So their content is checked at its
# origin and their equality to that origin is enforced separately -- scanning the
# copy would only re-report the source's own exemptions under a path those
# exemptions do not name.
# ★ They remain inside the UNTRACKED check below, deliberately: an uncommitted
# derived file means the corpus was regenerated and not committed, which is
# exactly the BUG-060 shape this script now refuses to pass.
if [ "${#FILES[@]}" -eq 0 ]; then
    echo "pin-check: no files matched -- run me from inside the repository" >&2
    exit 2
fi

# ★★ UNTRACKED FILES IN PIN SCOPE ARE A HARD FAILURE, AND THIS IS NOT PEDANTRY --
# IT SHIPPED A DEFECT.  `git ls-files` lists TRACKED files only, so a document
# that has just been created is invisible to this script AND to the sed that
# bumps the pins.  v0.3.3-testnet went out with
# `testnet/operator/OPERATOR_ONEPAGER.md` telling operators to
# `git clone -b v0.3.2-testnet`: the file was created and pinned in the same
# commit, so at bump time and at gate time it did not exist to either, and
# pin-check reported "OK across 7 files" -- true, and meaningless, because 7 was
# the count precisely BECAUSE the new file was not counted.
#
# ★ The failure shape is KDD-112's: a check that is never reached is
# indistinguishable from a check that passes.  Here the check narrowed its own
# scope silently, which is worse than being wrong -- it was CORRECT about a
# smaller set than the reader believed it covered.
#
# ★ So the count is only meaningful if the scope is complete.  Refuse to answer
# rather than answer for part of it.
mapfile -t UNTRACKED < <(git ls-files --others --exclude-standard "${PIN_GLOBS[@]}")
if [ "${#UNTRACKED[@]}" -gt 0 ]; then
    printf 'pin-check: %d UNTRACKED file(s) inside pin scope -- this check does NOT cover them:\n' "${#UNTRACKED[@]}" >&2
    printf '  %s\n' "${UNTRACKED[@]}" >&2
    printf '\n  git ls-files sees tracked files only, so an untracked file is invisible to\n' >&2
    printf '  this gate AND to whatever bumps the pins. Commit them (or remove them) and\n' >&2
    printf '  re-run. Reporting OK now would report OK for a smaller set than you think.\n\n' >&2
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
