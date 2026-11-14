"""Decoder for Delphi binary form files (TPF0 streams, Delphi 1 era).

Produces a tree of plain dicts that can be written as JSON, and can
render the classic Delphi text form (.dfm text) for human reading.
"""
from __future__ import annotations

import math
import struct

ENCODING = "cp1252"  # Swedish Windows 3.1 text

# TValueType
VA_NULL, VA_LIST, VA_INT8, VA_INT16, VA_INT32, VA_EXTENDED, VA_STRING, \
    VA_IDENT, VA_FALSE, VA_TRUE, VA_BINARY, VA_SET, VA_LSTRING, VA_NIL, \
    VA_COLLECTION = range(15)


class DfmError(Exception):
    pass


def _extended(b: bytes) -> float:
    mant, se = struct.unpack("<QH", b)
    sign = -1.0 if se & 0x8000 else 1.0
    exp = se & 0x7FFF
    if exp == 0 and mant == 0:
        return 0.0 * sign
    if exp == 0x7FFF:
        return sign * (math.inf if (mant << 1) == 0 else math.nan)
    return sign * mant * 2.0 ** (exp - 16383 - 63)


class Reader:
    def __init__(self, data: bytes):
        self.d = data
        self.p = 0

    def byte(self) -> int:
        v = self.d[self.p]
        self.p += 1
        return v

    def peek(self) -> int:
        return self.d[self.p]

    def unpack(self, fmt: str):
        v = struct.unpack_from(fmt, self.d, self.p)
        self.p += struct.calcsize(fmt)
        return v

    def sstr(self) -> str:
        n = self.byte()
        s = self.d[self.p:self.p + n].decode(ENCODING)
        self.p += n
        return s

    # --------------------------------------------------------------
    def value(self):
        """Read one typed value. Returns a JSON-friendly object."""
        t = self.byte()
        if t == VA_LIST:
            items = []
            while self.peek() != VA_NULL:
                items.append(self.value())
            self.byte()
            return {"list": items}
        if t == VA_INT8:
            return self.unpack("<b")[0]
        if t == VA_INT16:
            return self.unpack("<h")[0]
        if t == VA_INT32:
            return self.unpack("<i")[0]
        if t == VA_EXTENDED:
            v = _extended(self.d[self.p:self.p + 10])
            self.p += 10
            return {"float": v}
        if t == VA_STRING:
            return self.sstr()
        if t == VA_LSTRING:
            n = self.unpack("<I")[0]
            s = self.d[self.p:self.p + n].decode(ENCODING)
            self.p += n
            return s
        if t == VA_IDENT:
            return {"ident": self.sstr()}
        if t == VA_FALSE:
            return False
        if t == VA_TRUE:
            return True
        if t == VA_NIL:
            return None
        if t == VA_BINARY:
            n = self.unpack("<I")[0]
            b = self.d[self.p:self.p + n]
            self.p += n
            return {"binary": b}
        if t == VA_SET:
            items = []
            while True:
                s = self.sstr()
                if not s:
                    break
                items.append(s)
            return {"set": items}
        if t == VA_COLLECTION:
            items = []
            while self.peek() != VA_NULL:
                item: dict = {}
                if self.peek() in (VA_INT8, VA_INT16, VA_INT32):
                    item["order"] = self.value()
                if self.byte() != VA_LIST:
                    raise DfmError(f"collection item without list at {self.p - 1:#x}")
                item["props"] = self._props()
                items.append(item)
            self.byte()
            return {"collection": items}
        raise DfmError(f"unknown value type {t} at {self.p - 1:#x}")

    def _props(self) -> dict:
        props = {}
        while self.peek() != VA_NULL:
            name = self.sstr()
            props[name] = self.value()
        self.byte()
        return props

    def component(self) -> dict:
        flags = 0
        if self.peek() & 0xF0 == 0xF0:  # Delphi 2+ filer flags; harmless here
            flags = self.byte() & 0x0F
            if flags & 0x02:  # ffChildPos
                self.value()
        cls = self.sstr()
        name = self.sstr()
        comp = {"class": cls, "name": name, "props": self._props(), "children": []}
        if flags:
            comp["flags"] = flags
        while self.peek() != VA_NULL:
            comp["children"].append(self.component())
        self.byte()
        return comp


def parse(data: bytes) -> dict:
    if data[:4] != b"TPF0":
        raise DfmError("missing TPF0 signature")
    return Reader(data[4:]).component()


# ----------------------------------------------------------------------
# Embedded graphics
# ----------------------------------------------------------------------
def split_graphic(prop_name: str, blob: bytes) -> tuple[str, bytes]:
    """Return (extension, payload) for a binary property blob.

    TPicture.Data  = shortstring class name + graphic stream
    TBitmap stream = int32 size + .bmp file
    TIcon stream   = .ico file
    """
    b = blob
    cls = None
    if b and 0 < b[0] < 64 and b[1:2] == b"T":
        n = b[0]
        try:
            cand = b[1:1 + n].decode("ascii")
            if cand.isidentifier():
                cls, b = cand, b[1 + n:]
        except UnicodeDecodeError:
            pass
    if len(b) >= 6 and b[4:6] == b"BM":
        size = struct.unpack_from("<I", b, 0)[0]
        return "bmp", b[4:4 + size]
    if b[:2] == b"BM":
        return "bmp", b
    if b[:4] == b"\0\0\1\0":
        return "ico", b
    if cls == "TMetafile" or b[:4] == b"\xd7\xcd\xc6\x9a":
        return "wmf", b
    return "bin", blob


# ----------------------------------------------------------------------
# Text rendering
# ----------------------------------------------------------------------
def _qstr(s: str) -> str:
    out, inq = [], False
    for ch in s:
        if 32 <= ord(ch) < 127 or ord(ch) >= 160:
            if not inq:
                out.append("'")
                inq = True
            out.append("''" if ch == "'" else ch)
        else:
            if inq:
                out.append("'")
                inq = False
            out.append(f"#{ord(ch)}")
    if inq:
        out.append("'")
    return "".join(out) or "''"


def _val_text(v, ind: str) -> str:
    if isinstance(v, bool):
        return "True" if v else "False"
    if v is None:
        return "nil"
    if isinstance(v, int):
        return str(v)
    if isinstance(v, str):
        return _qstr(v)
    if "ident" in v:
        return v["ident"]
    if "float" in v:
        return repr(v["float"])
    if "set" in v:
        return "[" + ", ".join(v["set"]) + "]"
    if "file" in v:
        return "{ binary -> " + v["file"] + f" ({v['size']} bytes) }}"
    if "list" in v:
        inner = "".join(f"\n{ind}  {_val_text(x, ind + '  ')}" for x in v["list"])
        return "(" + inner + ")"
    if "collection" in v:
        parts = []
        for it in v["collection"]:
            hdr = f"item [{it['order']}]" if "order" in it else "item"
            body = "".join(f"\n{ind}    {k} = {_val_text(x, ind + '    ')}"
                           for k, x in it["props"].items())
            parts.append(f"\n{ind}  {hdr}{body}\n{ind}  end")
        return "<" + "".join(parts) + ">"
    return repr(v)


def to_text(comp: dict, ind: str = "") -> str:
    lines = [f"{ind}object {comp['name']}: {comp['class']}"]
    for k, v in comp["props"].items():
        lines.append(f"{ind}  {k} = {_val_text(v, ind + '  ')}")
    for c in comp["children"]:
        lines.append(to_text(c, ind + "  "))
    lines.append(f"{ind}end")
    return "\n".join(lines)
