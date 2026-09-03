#!/usr/bin/env python3
"""Every numeric RPC argument must be listed in the client-side conversion table.

Hemis-cli sends EVERY argument as a JSON string. src/rpc/client.cpp names the
(method, 0-based position) pairs to convert to real JSON types before the call
goes out. A method whose implementation calls params[N].get_int() -- but which
has no entry for N -- works over Python/curl RPC and throws
"JSON value is not an integer as expected" from the CLI.

That is BUG-059: protx_register was missing while protx_register_prepare, which
shares its handler and its signature, was present. The fleet registered 153
gamemasters over Python RPC, so the CLI path was never exercised and the gap was
invisible until an operator typed the command by hand.

★ Three outcomes, not two. Indices that are computed at runtime (params[i + 5])
cannot be resolved by reading the source, and this script reports them as
NOT ANALYSABLE rather than passing them silently -- a check that quietly skips
what it cannot see reads exactly like a check that found nothing wrong.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
RPC = os.path.join(ROOT, "src", "rpc")

# Accessors that REQUIRE a native JSON type. getValStr() is deliberately absent:
# it returns the raw string either way, which is why operatorReward -- parsed via
# ParseFixedPoint(params[..].getValStr()) -- needs no entry despite being numeric.
TYPED = r"get_int64|get_int|get_bool|get_real"

CONVERT_ENTRY = re.compile(r'\{\s*"([A-Za-z0-9_ ]+)"\s*,\s*(\d+)\s*,\s*"[^"]*"\s*\}')
CMD_ENTRY = re.compile(r'\{\s*"[a-z ]+"\s*,\s*"([A-Za-z0-9_ ]+)"\s*,\s*&([A-Za-z0-9_]+)\s*,')
FUNC_DEF = re.compile(r'^(?:static\s+)?UniValue\s+([A-Za-z0-9_]+)\s*\(', re.M)
LITERAL = re.compile(r'params\[\s*(\d+)\s*\]\s*\.\s*(?:' + TYPED + r')\s*\(')
COMPUTED = re.compile(r'params\[\s*([^\]]*?[^\d\s\]][^\]]*?)\s*\]\s*\.\s*(?:' + TYPED + r')\s*\(')


def read(p):
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def _scan(src, i, n):
    """Advance past a comment or string literal starting at i; else return i."""
    c = src[i]
    if c == "/" and i + 1 < n:
        if src[i + 1] == "/":
            j = src.find("\n", i)
            return n if j < 0 else j
        if src[i + 1] == "*":
            j = src.find("*/", i + 2)
            return n if j < 0 else j + 2
    if c in "\"'":
        j = i + 1
        while j < n:
            if src[j] == "\\":
                j += 2
                continue
            if src[j] == c:
                return j + 1
            j += 1
        return n
    return i


def _skip_to_brace(src, i):
    n = len(src)
    while i < n:
        k = _scan(src, i, n)
        if k != i:
            i = k
            continue
        if src[i] == "{":
            return i
        if src[i] == ";":      # a declaration, not a definition
            return -1
        i += 1
    return -1


def _match_brace(src, start):
    """Index of the } closing the { at start, ignoring braces in strings/comments.

    ★ Without this, a help string containing a literal { -- and RPC help is full
    of JSON examples -- unbalances the count and one function's body swallows the
    rest of the file. That is what produced the getblockchaininfo false positive.
    """
    n, depth, i = len(src), 0, start
    while i < n:
        k = _scan(src, i, n)
        if k != i:
            i = k
            continue
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def functions(src):
    """Split a translation unit into {name: body}, bounded by BRACE MATCHING.

    ★ Not "up to the next UniValue definition" -- that was the first version, and
    it silently swallowed any helper with a different return type sitting between
    two handlers, so validateaddress inherited the arguments of whatever followed
    it and this script reported two confident false positives.
    """
    out = {}
    for m in FUNC_DEF.finditer(src):
        open_brace = _skip_to_brace(src, m.end())
        if open_brace < 0:
            continue
        i = _match_brace(src, open_brace)
        if i < 0:
            continue
        out[m.group(1)] = src[m.start():i + 1]
    return out


def main():
    convert = set()
    for m in CONVERT_ENTRY.finditer(read(os.path.join(RPC, "client.cpp"))):
        convert.add((m.group(1), int(m.group(2))))

    methods, bodies = {}, {}
    for fn in sorted(os.listdir(RPC)):
        if not fn.endswith(".cpp") or fn == "client.cpp":
            continue
        src = read(os.path.join(RPC, fn))
        bodies.update(functions(src))
        for m in CMD_ENTRY.finditer(src):
            methods[m.group(1)] = (m.group(2), fn)

    missing, unanalysable = [], []
    for method, (handler, fn) in sorted(methods.items()):
        body = bodies.get(handler, "")
        # follow one level: a thin wrapper delegating to a shared implementation
        # (protx_register -> ProTxRegister) holds the accesses in the callee.
        seen = set(LITERAL.findall(body))
        comp = set(COMPUTED.findall(body))
        # ★ Follow ONLY shared helpers, never another registered handler: a
        # handler's params belong to ITS method, and treating a mention of it as
        # delegation attributes getnetworkhashps's arguments to getmininginfo.
        handlers = {h for h, _ in methods.values()}
        for other, obody in bodies.items():
            if (other != handler and other not in handlers
                    and re.search(r'\b%s\s*\(' % re.escape(other), body)):
                seen |= set(LITERAL.findall(obody))
                comp |= set(COMPUTED.findall(obody))
        for idx in sorted(int(i) for i in seen):
            if (method, idx) not in convert:
                missing.append((method, idx, handler, fn))
        for expr in sorted(comp):
            unanalysable.append((method, expr.strip(), handler))

    if unanalysable:
        print("NOT ANALYSABLE -- runtime-computed indices, verify by hand:")
        for method, expr, handler in unanalysable:
            print("  %-28s params[%s]  (%s)" % (method, expr, handler))
        print("Resolve by using a literal index, or record the decision here.")
        print()

    if missing:
        print("MISSING from src/rpc/client.cpp -- these throw from Hemis-cli:")
        for method, idx, handler, fn in missing:
            print('  { "%s", %d, "..." },   <- %s in %s' % (method, idx, handler, fn))
        print()
        print("A method missing here works over Python/curl RPC and fails from the")
        print("CLI with 'JSON value is not an integer as expected'. See BUG-059.")
        return 1

    if unanalysable:
        # ★ This FAILS rather than merely printing. lint-all.sh reads exit codes
        # only, so a warning that exits 0 is invisible in CI -- which is the same
        # false-green shape this script exists to catch.
        return 1

    print("rpc-convert: %d methods checked, all typed arguments listed." % len(methods))
    return 0


if __name__ == "__main__":
    sys.exit(main())
