"""Extract readable text from a Windows 3.x WinHelp (.HLP) file.

Usage:
    python tools/hlp_text.py [HLP] [OUT_TXT]

Defaults: extracted/iso/MYULK/MYULK.HLP -> extracted/help/MYULK.txt

Handles the internal file system (B+ tree directory), |SYSTEM, LZ77
compressed |TOPIC blocks and |Phrases compression (HC 3.1). Formatting
is dropped; each topic is written as its title followed by its text.
"""
from __future__ import annotations

import os
import struct
import sys

ENC = "cp1252"


def lz77(data: bytes, limit: int | None = None) -> bytes:
    out = bytearray()
    i = 0
    while i < len(data):
        ctrl = data[i]
        i += 1
        for bit in range(8):
            if i >= len(data):
                break
            if ctrl & (1 << bit):
                w = struct.unpack_from("<H", data, i)[0]
                i += 2
                length, off = (w >> 12) + 3, (w & 0x0FFF) + 1
                for _ in range(length):
                    out.append(out[-off] if off <= len(out) else 0)
            else:
                out.append(data[i])
                i += 1
            if limit and len(out) >= limit:
                return bytes(out[:limit])
    return bytes(out)


class Hlp:
    def __init__(self, path: str):
        self.d = open(path, "rb").read()
        magic, self.dir_start = struct.unpack_from("<II", self.d, 0)
        if magic != 0x00035F3F:
            raise ValueError("not a WinHelp file")
        self.files = self._directory()

    def _btree_leaves(self, off: int):
        """Yield (name, value) from a B+ tree whose header starts at off."""
        d = self.d
        (magic, flags, page_size, _struct, _z, _splits, root, _neg1, total, levels,
         _entries) = struct.unpack_from("<HHH16sHHHhHHI", d, off)
        pages = off + 38
        page = root
        for _ in range(levels - 1):      # descend to leftmost leaf
            page = struct.unpack_from("<H", d, pages + page * page_size + 4)[0]
        while page != 0xFFFF:
            p = pages + page * page_size
            _unused, n, _prev, nxt = struct.unpack_from("<HHhH", d, p)
            q = p + 8
            for _ in range(n):
                end = d.index(b"\0", q)
                name = d[q:end].decode(ENC)
                val = struct.unpack_from("<I", d, end + 1)[0]
                q = end + 5
                yield name, val
            page = nxt

    def _directory(self) -> dict:
        return dict(self._btree_leaves(self.dir_start + 9))

    def file(self, name: str) -> bytes:
        off = self.files[name]
        _reserved, used, _flags = struct.unpack_from("<IIB", self.d, off)
        return self.d[off + 9:off + 9 + used]

    # ------------------------------------------------------------------
    def phrases(self, minor: int) -> list[bytes]:
        if "|Phrases" not in self.files:
            return []
        p = self.file("|Phrases")
        n, _hundred = struct.unpack_from("<HH", p, 0)
        if minor <= 16:
            offs = struct.unpack_from(f"<{n + 1}H", p, 4)
            base = 4
            blob = p
        else:
            size = struct.unpack_from("<I", p, 4)[0]
            offs = struct.unpack_from(f"<{n + 1}H", p, 8)
            base = 8
            start = base + 2 * (n + 1)
            blob = p[:start] + lz77(p[start:], size)
        return [blob[base + offs[i]:base + offs[i + 1]] for i in range(n)]

    def text(self) -> str:
        sysf = self.file("|SYSTEM")
        _magic, minor, _major, _date, flags = struct.unpack_from("<HHHIH", sysf, 0)
        compressed = minor > 16 and flags in (4, 8)
        block = 2048 if (minor <= 16 or flags == 8) else 4096
        phr = self.phrases(minor)

        topic = self.file("|TOPIC")
        stream = bytearray()
        for b in range(0, len(topic), block):
            payload = topic[b + 12:b + block]
            stream += lz77(payload, 0x4000) if compressed else payload

        out: list[str] = []
        pos = 0
        while pos + 21 <= len(stream):
            size, len2, _prev, _next, len1, rtype = struct.unpack_from("<IIIIIB", stream, pos)
            if size < 21 or pos + size > len(stream):
                break
            data2 = bytes(stream[pos + len1:pos + size])
            if len2 > len(data2) and phr:
                data2 = self._unphrase(data2, phr)
            parts = [s.decode(ENC, "replace") for s in data2.split(b"\0")]
            if rtype == 2:                      # topic header: title first
                title = parts[0].strip() if parts else ""
                out.append("\n" + "=" * 60 + "\n" + title + "\n" + "=" * 60)
            elif rtype in (0x20, 0x23):         # display text / table
                line = " ".join(s.strip() for s in parts if s.strip())
                if line:
                    out.append(line)
            pos += size
        return "\n".join(out) + "\n"

    @staticmethod
    def _unphrase(data: bytes, phr: list[bytes]) -> bytes:
        out = bytearray()
        i = 0
        while i < len(data):
            c = data[i]
            if c == 0 or c > 15:
                out.append(c)
                i += 1
                continue
            idx = 256 * (c - 1) + data[i + 1]
            i += 2
            out += phr[idx // 2] if idx // 2 < len(phr) else b""
            if idx & 1:
                out.append(0x20)
        return bytes(out)


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "extracted", "iso", "MYULK", "MYULK.HLP")
    dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "extracted", "help", "MYULK.txt")
    h = Hlp(src)
    print("internal files:", sorted(h.files))
    txt = h.text()
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "w", encoding="utf-8") as f:
        f.write(txt)
    print(f"{txt.count(chr(10))} lines -> {dst}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
