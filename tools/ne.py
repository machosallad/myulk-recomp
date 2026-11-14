"""Minimal parser for 16-bit Windows NE (New Executable) files.

Covers what the Myulk project needs: header info, segment table,
imported module names and the resource table.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass, field

RT_NAMES = {
    1: "CURSOR", 2: "BITMAP", 3: "ICON", 4: "MENU", 5: "DIALOG",
    6: "STRING", 7: "FONTDIR", 8: "FONT", 9: "ACCELERATOR", 10: "RCDATA",
    12: "GROUP_CURSOR", 14: "GROUP_ICON",
}


@dataclass
class Resource:
    type_id: int | str      # int for ordinal types, str for named types
    res_id: int | str       # int for ordinal ids, str for named ids
    offset: int             # file offset
    length: int             # length in file (may include alignment padding)
    flags: int
    data: bytes = field(repr=False, default=b"")

    @property
    def type_name(self) -> str:
        if isinstance(self.type_id, int):
            return RT_NAMES.get(self.type_id, f"TYPE{self.type_id}")
        return self.type_id


@dataclass
class Segment:
    index: int              # 1-based
    offset: int             # file offset (0 = no data in file)
    length: int             # bytes in file
    flags: int
    min_alloc: int

    @property
    def is_data(self) -> bool:
        return bool(self.flags & 0x0001)


class NEFile:
    def __init__(self, path: str):
        self.path = path
        with open(path, "rb") as f:
            self.data = f.read()
        d = self.data
        if d[:2] != b"MZ":
            raise ValueError(f"{path}: not an MZ executable")
        self.ne = struct.unpack_from("<I", d, 0x3C)[0]
        if d[self.ne:self.ne + 2] != b"NE":
            raise ValueError(f"{path}: not an NE executable")
        ne = self.ne
        (self.linker_major, self.linker_minor) = d[ne + 2], d[ne + 3]
        self.flags = struct.unpack_from("<H", d, ne + 0x0C)[0]
        self.auto_data_seg = struct.unpack_from("<H", d, ne + 0x0E)[0]
        self.heap_size, self.stack_size = struct.unpack_from("<HH", d, ne + 0x10)
        self.cs_ip = struct.unpack_from("<HH", d, ne + 0x14)  # (ip, cs)
        self.ss_sp = struct.unpack_from("<HH", d, ne + 0x18)  # (sp, ss)
        self.n_segments = struct.unpack_from("<H", d, ne + 0x1C)[0]
        self.n_modrefs = struct.unpack_from("<H", d, ne + 0x1E)[0]
        self.segtab = ne + struct.unpack_from("<H", d, ne + 0x22)[0]
        self.rsrctab = ne + struct.unpack_from("<H", d, ne + 0x24)[0]
        self.resnametab = ne + struct.unpack_from("<H", d, ne + 0x26)[0]
        self.modreftab = ne + struct.unpack_from("<H", d, ne + 0x28)[0]
        self.imptab = ne + struct.unpack_from("<H", d, ne + 0x2A)[0]
        self.align_shift = struct.unpack_from("<H", d, ne + 0x32)[0] or 9
        self.target_os = d[ne + 0x36]
        self.expected_win_ver = struct.unpack_from("<H", d, ne + 0x3E)[0]

    # ------------------------------------------------------------------
    def module_name(self) -> str:
        d = self.data
        n = d[self.resnametab]
        return d[self.resnametab + 1:self.resnametab + 1 + n].decode("latin1")

    def imported_modules(self) -> list[str]:
        d = self.data
        out = []
        for i in range(self.n_modrefs):
            off = struct.unpack_from("<H", d, self.modreftab + 2 * i)[0]
            n = d[self.imptab + off]
            out.append(d[self.imptab + off + 1:self.imptab + off + 1 + n].decode("latin1"))
        return out

    def segments(self) -> list[Segment]:
        d = self.data
        out = []
        for i in range(self.n_segments):
            off, ln, fl, mn = struct.unpack_from("<HHHH", d, self.segtab + 8 * i)
            length = ln if ln or not off else 0x10000
            out.append(Segment(i + 1, off << self.align_shift, length, fl, mn or 0x10000))
        return out

    # ------------------------------------------------------------------
    def _rname(self, off: int) -> str:
        d = self.data
        p = self.rsrctab + off
        return d[p + 1:p + 1 + d[p]].decode("latin1")

    def resources(self) -> list[Resource]:
        d = self.data
        if self.rsrctab == self.resnametab:  # empty resource table
            return []
        shift = struct.unpack_from("<H", d, self.rsrctab)[0]
        p = self.rsrctab + 2
        out: list[Resource] = []
        while True:
            type_id, count = struct.unpack_from("<HH", d, p)
            if type_id == 0:
                break
            p += 8
            t: int | str = type_id & 0x7FFF if type_id & 0x8000 else self._rname(type_id)
            for _ in range(count):
                off, ln, fl, rid = struct.unpack_from("<HHHH", d, p)
                p += 12
                r: int | str = rid & 0x7FFF if rid & 0x8000 else self._rname(rid)
                foff, flen = off << shift, ln << shift
                out.append(Resource(t, r, foff, flen, fl, d[foff:foff + flen]))
        return out
