"""Find function entry points in MYULK.EXE that Ghidra's call-following misses.

Usage:
    python tools/find_functions.py [EXE] [OUT_CSV]

Many game procedures are only reached through procedure pointers (scene
and hotspot tables), so auto-analysis never sees a call to them. Two
sources of entry points:

1. Targets of far-pointer relocations into code segments that start with
   a Turbo Pascal / Delphi prologue: `55 89 E5` (push bp; mov bp,sp) or
   `C8 xx xx 00/01` (enter n,0/1).
2. In the game segments (1-22), every `55 89 E5` prologue. Delphi places
   string constants immediately before the routine that uses them, so
   prologues following data are a reliable entry signature.

Writes analysis/function_starts.csv: seg,offset,reason
"""
from __future__ import annotations

import csv
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from delphi_map import DelphiImage  # noqa: E402

GAME_SEGS = range(1, 23)


def is_prologue(b: bytes) -> bool:
    return b[:3] == b"\x55\x89\xe5" or (b[:1] == b"\xc8" and b[3:4] in (b"\x00", b"\x01"))


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "extracted", "iso", "MYULK", "MYULK.EXE")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "analysis", "function_starts.csv")
    img = DelphiImage(exe)

    starts: dict[tuple[int, int], str] = {}
    for t in img.far.values():
        if t[0] != "internal" or t[1] not in img.segs or img.segs[t[1]].is_data:
            continue
        seg = img.segs[t[1]]
        if t[2] + 4 > seg.length:
            continue
        if is_prologue(img.d[seg.offset + t[2]:seg.offset + t[2] + 4]):
            starts[(t[1], t[2])] = "farref"

    for s in GAME_SEGS:
        seg = img.segs[s]
        d = img.d[seg.offset:seg.offset + seg.length]
        i = d.find(b"\x55\x89\xe5")
        while i >= 0:
            starts.setdefault((s, i), "prologue")
            i = d.find(b"\x55\x89\xe5", i + 1)

    with open(out, "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["seg", "offset", "reason"])
        for (s, o), why in sorted(starts.items()):
            w.writerow([s, f"{o:04X}", why])
    by = {}
    for why in starts.values():
        by[why] = by.get(why, 0) + 1
    print(f"{len(starts)} function starts {by} -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
