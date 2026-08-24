"""Decode PTX payloads from a spec extracted from the C++ header.

This module knows how to read WIRE PRIMITIVES.  It does not know the field
order of any payload -- that comes from ptx_payload_spec, which reads it out of
SERIALIZE_METHODS.  Adding a field to the C++ struct changes what this decodes
with no edit here, and test_decoder.py fails if the two ever disagree.

Every field records its BYTE RANGE.  That is not decoration: a reader checking
the decode needs to see where each value came from in the raw bytes, not just
be told what it is.
"""
import struct

from ptx_payload_spec import extract_spec


class PayloadError(ValueError):
    """Raised with the offset, so a bad decode says WHERE it went wrong."""


class _Reader:
    def __init__(self, data):
        self.d = data
        self.o = 0

    def _need(self, n, what):
        if self.o + n > len(self.d):
            raise PayloadError(
                "ran off the end reading %s: wanted %d byte(s) at offset %d, "
                "payload is %d bytes" % (what, n, self.o, len(self.d)))

    def take(self, n, what):
        self._need(n, what)
        b = self.d[self.o:self.o + n]
        self.o += n
        return b

    def compact_size(self):
        """Bitcoin CompactSize. Non-canonical encodings are REJECTED, not
        tolerated -- a lenient reader here would accept bytes the consensus
        deserialiser refuses, so the two would disagree about the same tx."""
        b = self.take(1, "compact-size prefix")[0]
        if b < 0xfd:
            return b
        if b == 0xfd:
            v = struct.unpack("<H", self.take(2, "compact-size u16"))[0]
            if v < 0xfd:
                raise PayloadError("non-canonical compact size %d at offset %d" % (v, self.o - 3))
            return v
        if b == 0xfe:
            v = struct.unpack("<I", self.take(4, "compact-size u32"))[0]
            if v < 0x10000:
                raise PayloadError("non-canonical compact size %d at offset %d" % (v, self.o - 5))
            return v
        v = struct.unpack("<Q", self.take(8, "compact-size u64"))[0]
        if v < 0x100000000:
            raise PayloadError("non-canonical compact size %d at offset %d" % (v, self.o - 9))
        return v


def _read_kind(r, kind, name):
    if kind == "u32":
        return struct.unpack("<I", r.take(4, name))[0]
    if kind == "i64":
        return struct.unpack("<q", r.take(8, name))[0]
    if kind == "bool":
        b = r.take(1, name)[0]
        if b > 1:
            raise PayloadError("%s: bool byte is 0x%02x, not 0 or 1" % (name, b))
        return bool(b)
    if kind == "u256":
        # ★ uint256 serialises LITTLE-ENDIAN on the wire and is DISPLAYED
        # big-endian (the usual bitcoin txid convention).  Getting this
        # backwards yields a hash that looks plausible and matches nothing.
        return r.take(32, name)[::-1].hex()
    if kind == "string":
        n = r.compact_size()
        return r.take(n, name).decode("utf-8", "replace")
    if kind == "bytes":
        n = r.compact_size()
        return r.take(n, name).hex()
    if kind == "vec_i64":
        n = r.compact_size()
        return [struct.unpack("<q", r.take(8, name))[0] for _ in range(n)]
    if kind == "vec_string":
        n = r.compact_size()
        out = []
        for _ in range(n):
            ln = r.compact_size()
            out.append(r.take(ln, name).decode("utf-8", "replace"))
        return out
    raise PayloadError("no reader for wire kind %r (field %s)" % (kind, name))


def decode(payload_hex, struct_name, spec=None):
    """-> {"struct","fields":[{name,cpp,kind,value,start,end,raw}],"size","trailing"}

    ★ TRAILING BYTES ARE REPORTED, NOT IGNORED.  Bytes left over after the last
    field mean the payload is NOT the struct we think it is -- most likely a
    newer version carrying a field this build does not know.  Silently
    discarding them is how the previous decoder read a v2 payload as a v1 and
    said nothing.
    """
    try:
        raw = bytes.fromhex(payload_hex.strip())
    except ValueError as e:
        raise PayloadError("not valid hex: %s" % e)

    spec = spec or extract_spec()
    if struct_name not in spec:
        raise PayloadError("unknown payload struct %r" % struct_name)

    r = _Reader(raw)
    fields = []
    for name, cpp, kind in spec[struct_name]:
        start = r.o
        value = _read_kind(r, kind, name)
        fields.append({
            "name": name, "cpp": cpp, "kind": kind, "value": value,
            "start": start, "end": r.o, "raw": raw[start:r.o].hex(),
        })
    return {
        "struct": struct_name,
        "fields": fields,
        "size": len(raw),
        "consumed": r.o,
        "trailing": raw[r.o:].hex(),
    }


def as_dict(decoded):
    return {f["name"]: f["value"] for f in decoded["fields"]}
