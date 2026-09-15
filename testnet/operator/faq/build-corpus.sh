#!/bin/bash
# build-corpus.sh — regenerate testnet/operator/faq/derived/ from the operator
# documents. Run it whenever one of them changes; install-test.sh fails by name
# if you forget, and pin-check.sh refuses to run if you leave the result
# uncommitted (BUG-060's fix).
#
# ★ WRITTEN 2026-09-15 (BUG-088). The README has documented this command since
# the corpus was created and the script was never committed — the derived half
# was produced once, by hand, and the only way to refresh it was to reproduce a
# format nobody had written down. The byte-copy discipline is load-bearing
# (a paraphrase re-creates the second-source problem the copy exists to avoid),
# so the tool that enforces it has to exist.
#
# The format is fixed by two consumers, and both are byte-exact:
#   install-test.sh  body = everything after the "> edited for the FAQ bot"
#                    line and the blank line following it, == the source file
#   pin-check.sh     derived/ is excluded from content scanning but NOT from
#                    the untracked check
set -euo pipefail
cd "$(git rev-parse --show-toplevel)"
DERIVED=testnet/operator/faq/derived
MAN="$DERIVED/MANIFEST.txt"
# ★ The tag comes from the pin that install.sh actually uses, so the corpus can
# never name a different release from the one an operator installs.
TAG="$(sed -n 's/^REF="${PTX_REF:-\([^}]*\)}".*/\1/p' testnet/operator/install.sh | head -1)"
[ -n "$TAG" ] || { echo "build-corpus: cannot read the tag from testnet/operator/install.sh" >&2; exit 2; }
# source path -> derived basename. Order is the manifest's order.
SOURCES="testnet/operator/OPERATOR_ONEPAGER.md testnet/operator/OPERATOR_GUIDE.md GM_QUICKSTART.md"
: > "$MAN"
for src in $SOURCES; do
    [ -f "$src" ] || { echo "build-corpus: $src does not exist" >&2; exit 2; }
    out="$DERIVED/$(basename "$src")"
    sha="$(sha256sum "$src" | awk '{print $1}')"
    {
        printf '<!-- CORPUS-SOURCE: %s -->\n' "$src"
        printf '<!-- CORPUS-TAG: %s -->\n' "$TAG"
        printf '<!-- CORPUS-SHA256: %s -->\n' "$sha"
        printf '\n'
        printf '> **This document is a verbatim copy of `%s` at `%s`.** It is not\n' "$src" "$TAG"
        printf '> edited for the FAQ bot. If it disagrees with anything else in this corpus, it wins.\n'
        printf '\n'
        cat "$src"
    } > "$out"
    printf '%s  %s\n' "$sha" "$src" >> "$MAN"
    echo "  $out  <- $src  ($sha)"
done
# ★ Verify what was just written the way the gate will, rather than trusting the
# generator: a generator that agrees with itself proves nothing.
rc=0
for src in $SOURCES; do
    out="$DERIVED/$(basename "$src")"
    body="$(sed -n '/^> edited for the FAQ bot/,$p' "$out" | tail -n +3)"
    [ "$body" = "$(cat "$src")" ] || { echo "build-corpus: $out is NOT a byte-copy of $src" >&2; rc=1; }
done
[ "$rc" = 0 ] && echo "build-corpus: $(wc -l < "$MAN") documents, all byte-identical to their sources, tag $TAG"
exit $rc
