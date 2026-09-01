#!/bin/sh
# Copyright (c) 2012-2016 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C
if [ $# -gt 1 ]; then
    cd "$2" || exit 1
fi
if [ $# -gt 0 ]; then
    FILE="$1"
    shift
    if [ -f "$FILE" ]; then
        INFO="$(head -n 1 "$FILE")"
    fi
else
    echo "Usage: $0 <filename> <srcroot>"
    exit 1
fi

git_check_in_repo() {
    ! { git status --porcelain -uall --ignored "$@" 2>/dev/null || echo '??'; } | grep -q '?'
}

DESC=""
SUFFIX=""
# ★★ ODC-092: ACCEPT THE VERSION RATHER THAN REDISCOVERING IT.
# Four fixes tried to make the git derivation below work in the release runner
# (annotated tags, tag-created-before-build, fetch-depth: 0). Each was correct
# and each was insufficient, because every one of them sits UPSTREAM of $DESC:
# git describe, tag objects, fetch depth, whether .git is even present in the
# directory this runs from. The release workflow ALREADY KNOWS the tag -- it is
# a dispatch input -- so it passes it here and none of those links matter.
#
# ★ FALLBACK PRESERVED, DELIBERATELY. With PTX_BUILD_DESC unset (every local
# build, every developer, px1) this is byte-identical to before: the git
# derivation runs untouched. Input if present, else derive.
#
# ★ THE TRADE, recorded because it is real: a derived version is SELF-VERIFYING
# -- a binary cannot claim a tag it was not built from. An asserted one can, if
# whoever dispatches mistypes the tag field. What makes that acceptable is
# self-check.sh section 0, which compares the binary's string against the
# checked-out source at the operator's node, so a wrong assertion is caught
# rather than believed.
if [ -n "${PTX_BUILD_DESC:-}" ]; then
    DESC="$PTX_BUILD_DESC"
elif [ "${BITCOIN_GENBUILD_NO_GIT}" != "1" ] && [ -e "$(command -v git)" ] && [ "$(git rev-parse --is-inside-work-tree 2>/dev/null)" = "true" ] && git_check_in_repo share/genbuild.sh; then
    # clean 'dirty' status of touched files that haven't been modified
    git diff >/dev/null 2>/dev/null

    # if latest commit is tagged and not dirty, then override using the tag name
    RAWDESC=$(git describe --abbrev=0 2>/dev/null)
    if [ "$(git rev-parse HEAD)" = "$(git rev-list -1 "$RAWDESC" 2>/dev/null)" ]; then
        git diff-index --quiet HEAD -- && DESC=$RAWDESC
    fi

    # otherwise generate suffix from git, i.e. string like "59887e8-dirty"
    SUFFIX=$(git rev-parse --short HEAD)
    git diff-index --quiet HEAD -- || SUFFIX="$SUFFIX-dirty"
fi

if [ -n "$DESC" ]; then
    NEWINFO="#define BUILD_DESC \"$DESC\""
elif [ -n "$SUFFIX" ]; then
    NEWINFO="#define BUILD_SUFFIX $SUFFIX"
else
    NEWINFO="// No build information available"
fi

# only update build.h if necessary
if [ "$INFO" != "$NEWINFO" ]; then
    echo "$NEWINFO" >"$FILE"
fi
