"""Annotate decompiled code with the Pascal string constants it points at.

Usage:
    python tools/annotate_strings.py [EXE] [DECOMP_DIR]

Delphi 1 keeps string literals in the code segment right before the routine
that uses them; the decompiler shows them as bare pointers:

    FUN_1008_1584(AdvForm,(byte *)0x10082adc);
    FUN_1008_224a(Self,0,0,0,0,&UNK_1008_3cd5);
    System::PStrCmp((byte *)CONCAT22(unaff_CS,(byte *)0x37a1), ...);

This writes decomp/annotated/segNN.c where each such pointer is followed by
/* "string" */. Far pointers (0x1SSSOOOO, UNK_1SSS_OOOO) name their segment;
near ones (0xOOOO next to unaff_CS) use the current function's segment.
"""
from __future__ import annotations

import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ne import NEFile  # noqa: E402

FAR = re.compile(r"(\(byte \*\)|\(char \*\)|\(undefined1 \*\)|&UNK_)(?:0x)?(1[01][0-9a-f])([0-9a-f])[_]?([0-9a-f]{4})\b")
NEAR = re.compile(r"\(byte \*\)0x([0-9a-f]{3,4})\)")
HEADER = re.compile(r"^// ---- (\d+):([0-9A-F]{4})")


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "extracted", "iso", "MYULK", "MYULK.EXE")
    ddir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "decomp")
    ne = NEFile(exe)
    segs = {s.index: s for s in ne.segments()}
    d = ne.data

    def pstr(seg: int, off: int) -> str | None:
        s = segs.get(seg)
        if not s or s.is_data or off >= s.length:
            return None
        n = d[s.offset + off]
        if n < 1 or off + 1 + n > s.length:
            return None
        raw = d[s.offset + off + 1:s.offset + off + 1 + n]
        if any(b < 0x20 and b not in (9,) for b in raw):
            return None
        try:
            txt = raw.decode("cp1252")
        except UnicodeDecodeError:
            return None
        if n == 1 and not txt.isalnum():
            return None
        return txt.replace("*/", "* /")

    def sel_to_seg(sel: int) -> int:
        return (sel - 0x1000) // 8 + 1

    out_dir = os.path.join(ddir, "annotated")
    os.makedirs(out_dir, exist_ok=True)
    total = 0
    for path in sorted(glob.glob(os.path.join(ddir, "seg*.c"))):
        cur = None
        lines = []
        with open(path, encoding="utf-8") as f:
            for line in f:
                m = HEADER.match(line)
                if m:
                    cur = int(m.group(1))
                    cur_off = int(m.group(2), 16)
                notes = []
                for fm in FAR.finditer(line):
                    sel = int(fm.group(2) + fm.group(3), 16)
                    if not (0x1000 <= sel < 0x1108):
                        continue
                    off = int(fm.group(4), 16)
                    s = None
                    # Ghidra often guesses the segment of a `push cs` pointer
                    # wrongly; literals sit just before their routine, so try
                    # the current segment first when the offset fits.
                    if cur is not None and cur_off - 0x600 <= off < cur_off:
                        s = pstr(cur, off)
                    if s is None:
                        s = pstr(sel_to_seg(sel), off)
                    if s is not None:
                        notes.append(s)
                if cur is not None and ("unaff_CS" in line or "uVar" in line):
                    for nm in NEAR.finditer(line):
                        s = pstr(cur, int(nm.group(1), 16))
                        if s is not None:
                            notes.append(s)
                if notes:
                    total += len(notes)
                    line = line.rstrip("\n") + "  /* " + " | ".join(f'"{n}"' for n in notes) + " */\n"
                lines.append(line)
        with open(os.path.join(out_dir, os.path.basename(path)), "w", encoding="utf-8") as f:
            f.writelines(lines)
    print(f"annotated {total} string references -> {out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
