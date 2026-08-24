"""Extract PTX payload wire specs from the ONE source of truth: the C++ header.

★ WHY THIS EXISTS AND WHY IT IS NOT A HAND-WRITTEN TABLE.

The explorer that preceded this one hand-rolled a second serialiser in Python
(explorer/proxy.py:162-195).  It was correct on every field it covered and it
was STILL wrong: it stopped after quorum_sig and silently dropped quorum_hash,
the field that makes "this quorum produced this output" chain-computable
(KDD-074/076).  It survived that omission only because quorum_hash happens to
be the LAST field -- a field inserted mid-struct would have silently
mis-decoded everything after it and produced plausible wrong numbers with no
error at all.

A parallel serialiser drifts.  It drifts silently, and it drifts fastest in the
one component whose entire job is being trustworthy.  So this module does not
KNOW the layout: it READS it, out of src/primitives/transaction.h, from the
SERIALIZE_METHODS block that consensus itself uses.  When the payload changes,
the spec changes with it -- and test_decoder.py fails loudly if the extracted
spec ever stops matching the committed golden copy, which is the entire point
of the exercise rather than a nicety.
"""
import os
import re

HEADER_REL = os.path.join("src", "primitives", "transaction.h")

# C++ declared type -> wire reader name.  Every type the two PTX payloads use.
# ★ Deliberately NOT a permissive default: an unknown type is an ERROR, not a
# guess.  A guess here is exactly how a decoder starts producing confident
# wrong output after someone adds a field.
TYPE_MAP = {
    "std::string":               "string",
    "uint32_t":                  "u32",
    "int64_t":                   "i64",
    "bool":                      "bool",
    "uint256":                   "u256",
    "std::vector<uint8_t>":      "bytes",
    "std::vector<int64_t>":      "vec_i64",
    "std::vector<std::string>":  "vec_string",
}

STRUCTS = ("CProbabilisticTxPayload", "CPTXRollCommitPayload")

_DECL = re.compile(
    r"^\s*(?P<type>(?:std::)?[A-Za-z_][A-Za-z0-9_:]*(?:\s*<[^>]*>)?)"
    r"\s+(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*(?:\{[^}]*\})?\s*;"
)


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def find_repo_root(start=None):
    """Walk up until src/primitives/transaction.h is found."""
    d = os.path.abspath(start or os.path.dirname(__file__))
    while True:
        if os.path.isfile(os.path.join(d, HEADER_REL)):
            return d
        parent = os.path.dirname(d)
        if parent == d:
            raise RuntimeError(
                "could not locate %s by walking up from %s -- this tool reads the "
                "payload layout out of the C++ header and cannot run without it"
                % (HEADER_REL, start or __file__))
        d = parent


def _struct_body(src, name):
    m = re.search(r"\bstruct\s+%s\s*\{" % re.escape(name), src)
    if not m:
        raise RuntimeError("struct %s not found in %s" % (name, HEADER_REL))
    i = m.end()
    depth = 1
    while i < len(src) and depth:
        if src[i] == "{":
            depth += 1
        elif src[i] == "}":
            depth -= 1
        i += 1
    if depth:
        raise RuntimeError("unterminated struct %s" % name)
    return src[m.end():i - 1]


def extract_spec(repo_root=None):
    """-> {struct_name: [(field_name, cpp_type, wire_kind), ...]} in WIRE order.

    Wire order is READWRITE's order, not declaration order.  They agree today;
    relying on declaration order would be relying on a coincidence.
    """
    root = repo_root or find_repo_root()
    with open(os.path.join(root, HEADER_REL), encoding="utf-8") as fh:
        src = _strip_comments(fh.read())

    spec = {}
    for struct in STRUCTS:
        body = _struct_body(src, struct)

        declared = {}
        for line in body.splitlines():
            if "SERIALIZE_METHODS" in line or "READWRITE" in line:
                continue
            m = _DECL.match(line)
            if m:
                declared[m.group("name")] = re.sub(r"\s+", "", m.group("type"))

        rw = re.search(r"READWRITE\s*\((?P<args>.*?)\)\s*;", body, flags=re.S)
        if not rw:
            raise RuntimeError("no READWRITE(...) in struct %s" % struct)

        fields = []
        for raw in rw.group("args").split(","):
            arg = raw.strip()
            if not arg:
                continue
            if not arg.startswith("obj."):
                raise RuntimeError(
                    "%s: READWRITE argument %r is not a plain obj.<field> -- this "
                    "extractor only understands flat field lists, and guessing "
                    "past that is how a decoder goes quietly wrong" % (struct, arg))
            name = arg[4:].strip()
            if name not in declared:
                raise RuntimeError(
                    "%s: READWRITE names %r but no such member was declared" % (struct, name))
            cpp = declared[name]
            if cpp not in TYPE_MAP:
                raise RuntimeError(
                    "%s.%s has type %r which this extractor does not know how to "
                    "read. Add it to TYPE_MAP deliberately -- do not let it default."
                    % (struct, name, cpp))
            fields.append((name, cpp, TYPE_MAP[cpp]))
        spec[struct] = fields
    return spec


def spec_fingerprint(spec):
    """Stable text form -- what the divergence test pins."""
    out = []
    for struct in sorted(spec):
        out.append(struct)
        for i, (name, cpp, kind) in enumerate(spec[struct]):
            out.append("  %2d %-20s %-24s %s" % (i, name, cpp, kind))
    return "\n".join(out)


if __name__ == "__main__":
    print(spec_fingerprint(extract_spec()))
