"""Static recompiler: MYULK.EXE (16-bit NE, x86) -> C++.

Usage:
    python tools/recomp/recomp.py EXE OUT_DIR [--extra-entries FILE]

Writes into OUT_DIR:
    segNN.cpp           one C++ function per discovered routine (f_SS_OOOO)
    funcs.hpp           declarations
    dispatch.cpp        address -> function table for indirect calls
    imports.cpp         import table: (module, ordinal) -> runtime thunk
    report.txt          coverage and problems

The generated code runs on the runtime in src/recomp/runtime: an emulated
CPU (registers, flags, x87) over segmented memory loaded from the same EXE
with its relocations applied. Selector of NE segment n is (n << 3) | 7.

Function discovery is recursive descent from known entry points, so data
between routines (string constants, VMTs) is never decoded as code.
"""
from __future__ import annotations

import collections
import csv
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
from capstone import CS_ARCH_X86, CS_MODE_16, Cs  # noqa: E402
from capstone import x86 as X  # noqa: E402
from ne import NEFile  # noqa: E402

GAME_SEGS = range(1, 23)


def sel(n: int) -> int:
    return (n << 3) | 7


# --------------------------------------------------------------------------
# Image and relocations
# --------------------------------------------------------------------------
class Image:
    def __init__(self, path: str):
        self.ne = NEFile(path)
        self.d = self.ne.data
        self.segs = {s.index: s for s in self.ne.segments()}
        self.code_segs = [s.index for s in self.segs.values() if not s.is_data]
        self.data_seg = [s.index for s in self.segs.values() if s.is_data][0]
        self.bytes = {n: bytearray(self.d[s.offset:s.offset + s.length]) for n, s in self.segs.items()}
        self.rel: dict[int, dict[int, object]] = {n: {} for n in self.segs}
        for s in self.segs.values():
            for r in self.ne.relocations(s):
                self.rel[s.index][r.offset] = r
        self.modules = self.ne.imported_modules()

    def reloc_in(self, seg: int, start: int, end: int):
        return [(o, r) for o, r in self.rel[seg].items() if start <= o < end]


# --------------------------------------------------------------------------
# Disassembly and function discovery
# --------------------------------------------------------------------------
class Func:
    def __init__(self, seg: int, entry: int):
        self.seg = seg
        self.entry = entry
        self.insns: dict[int, object] = {}
        self.labels: set[int] = set()
        self.bad: str | None = None
        self.jumptables: dict[int, list[int]] = {}
        self.stops: set[int] = set()
        self.cover: dict[int, int] = {}
        self.continuations: set[int] = set()

    @property
    def name(self) -> str:
        return f"f_{self.seg:02d}_{self.entry:04X}"


TERMINATORS = {"ret", "retf", "iret", "jmp", "ljmp", "hlt"}
JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns", "jp", "jnp",
       "jl", "jge", "jle", "jg", "jcxz", "jecxz", "loop", "loope", "loopne"}
# opcodes that only appear when data is decoded as code
INVALID = {"insb", "insw", "outsb", "outsw", "in", "out", "arpl", "bound", "lock", "hlt",
           "clts", "str", "ud0", "ud2", "addps", "punpckldq", "int1", "into", "les", "lds"}


class Disasm:
    def __init__(self, img: Image):
        self.img = img
        self.md = Cs(CS_ARCH_X86, CS_MODE_16)
        self.md.detail = True
        self.cache: dict[tuple[int, int], object] = {}

    def at(self, seg: int, off: int):
        key = (seg, off)
        if key not in self.cache:
            code = bytes(self.img.bytes[seg][off:off + 16])
            ins = next(self.md.disasm(code, off), None)
            self.cache[key] = ins
        return self.cache[key]


# Win16 exported-callback prologues: "mov ax, ss / push ds; pop ax", nop, "inc bp; push bp; mov bp, sp"
CALLBACK_PROLOGUES = (b"\x8c\xd0\x90\x45\x55\x8b\xec", b"\x1e\x58\x90\x45\x55\x8b\xec",
                      b"\x8c\xd0\x90\x45\x55\x89\xe5", b"\x1e\x58\x90\x45\x55\x89\xe5")


def is_plausible_entry(dis: Disasm, seg: int, off: int) -> bool:
    b = dis.img.bytes[seg]
    if off + 3 > len(b):
        return False
    return b[off:off + 3] == b"\x55\x89\xe5" or (b[off] == 0xC8 and off + 3 < len(b) and b[off + 3] < 8) or \
        b[off:off + 4] == b"\x45\x55\x8b\xec" or b[off:off + 3] == b"\x55\x8b\xec" or \
        any(b[off:off + len(p)] == p for p in CALLBACK_PROLOGUES)


def discover(img: Image, dis: Disasm, extra: list[tuple[int, int]]) -> dict[tuple[int, int], Func]:
    entries: set[tuple[int, int]] = set()
    code = set(img.code_segs)
    # program entry
    entries.add((img.ne.cs_ip[1], img.ne.cs_ip[0]))
    # far-pointer relocations to code: calls, VMT slots, method tables...
    for seg in img.segs:
        b = img.bytes[seg]
        for off, r in img.rel[seg].items():
            t = r.target
            if t[0] != "internal" or t[1] not in code:
                continue
            if r.src_type == 3:
                is_call = seg in code and off >= 1 and b[off - 1] in (0x9A, 0xEA)
                tb = img.bytes[t[1]]
                # a target right after a return instruction starts a routine
                after_ret = t[2] >= 1 and (tb[t[2] - 1] in (0xC3, 0xCB) or
                                           (t[2] >= 3 and tb[t[2] - 3] in (0xC2, 0xCA)))
                if is_call or after_ret or is_plausible_entry(dis, t[1], t[2]):
                    entries.add((t[1], t[2]))
            elif r.src_type == 2 and seg in code:
                # "mov [x], OFFSET ; mov [x+2], SEG" or "push OFFSET/push SEG" pairs:
                # look for a 16-bit immediate in the 12 bytes before the segment fixup
                for back in range(3, 13):
                    p = off - back
                    if p < 0:
                        break
                    ins = dis.at(seg, p)
                    if ins and p + ins.size <= off and ins.imm_size == 2 and ins.operands:
                        imm = ins.operands[-1].imm if ins.operands[-1].type == X.X86_OP_IMM else None
                        if imm is not None and is_plausible_entry(dis, t[1], imm & 0xFFFF):
                            entries.add((t[1], imm & 0xFFFF))
    # prologue scan in every code segment
    for seg in code:
        b = img.bytes[seg]
        for pat in (b"\x55\x89\xe5",) + CALLBACK_PROLOGUES:
            i = b.find(pat)
            while i >= 0:
                entries.add((seg, i))
                i = b.find(pat, i + 1)
    entries |= set(extra)
    # virtual / published / message methods recovered from the class tables
    cj = os.path.join(os.path.dirname(os.path.dirname(HERE)), "analysis", "classes.json")
    if os.path.isfile(cj):
        import json
        for cl in json.load(open(cj, encoding="utf-8")):
            for group in ("virtuals", "methods", "dynamic"):
                for m in cl.get(group, []):
                    a = m.get("addr")
                    if a and ":" in a:
                        s, o = a.split(":")
                        if int(s) in code:
                            entries.add((int(s), int(o, 16)))
            for pr in cl.get("properties", []):
                for acc in (pr.get("get", {}), pr.get("set", {})):
                    a = acc.get("addr")
                    if a and ":" in a:
                        s, o = a.split(":")
                        entries.add((int(s), int(o, 16)))

    funcs: dict[tuple[int, int], Func] = {}
    work = sorted(entries)
    seen = set()
    while work:
        seg, ent = work.pop()
        if (seg, ent) in seen:
            continue
        seen.add((seg, ent))
        f = Func(seg, ent)
        explore(img, dis, f, work)
        funcs[(seg, ent)] = f
    # drop prologue-scan junk: bad functions nobody calls directly
    called = set()
    for f in funcs.values():
        if f.bad:
            continue
        for ins in f.insns.values():
            if ins.mnemonic in ("call", "lcall") and ins.operands and ins.operands[0].type == X.X86_OP_IMM:
                if ins.mnemonic == "call":
                    called.add((f.seg, ins.operands[0].imm & 0xFFFF))
    for k in list(funcs):
        if funcs[k].bad and k not in called and k not in set(extra) and k != (img.ne.cs_ip[1], img.ne.cs_ip[0]):
            del funcs[k]
    return funcs


def explore(img: Image, dis: Disasm, f: Func, work: list):
    seg = f.seg
    n = len(img.bytes[seg])
    todo = [f.entry]
    f.labels.add(f.entry)
    while todo:
        a = todo.pop()
        while True:
            if a in f.insns:
                break
            ins = dis.at(seg, a) if a < n else None
            m = ins.mnemonic.split()[-1] if ins else ""
            if ins is None or (m in INVALID and not (m in ("les", "lds") and ins.operands[1].type == X.X86_OP_MEM)):
                if a == f.entry:
                    f.bad = f"invalid code at entry {a:04X}"
                    return
                # Data after a call that never returns (Halt, RunError, raise):
                # stop this path; reaching it at runtime is reported.
                f.stops.add(a)
                break
            # real code never overlaps; overlapping decodes mean a bogus entry
            for k in range(a, a + ins.size):
                owner = f.cover.get(k)
                if owner is not None and owner != a:
                    f.bad = f"overlapping instructions at {a:04X}/{owner:04X}"
                    return
            for k in range(a, a + ins.size):
                f.cover[k] = a
            f.insns[a] = ins
            nxt = a + ins.size
            if m in JCC:
                t = ins.operands[0].imm & 0xFFFF
                f.labels.add(t)
                todo.append(t)
                f.labels.add(nxt)
            elif m == "jmp":
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    t = op.imm & 0xFFFF
                    f.labels.add(t)
                    todo.append(t)
                else:
                    tab = jumptable(img, dis, f, ins)
                    if tab:
                        f.jumptables[a] = tab
                        for t in tab:
                            f.labels.add(t)
                            todo.append(t)
                break
            elif m == "push" and ins.operands and ins.operands[0].type == X.X86_OP_REG \
                    and ins.reg_name(ins.operands[0].reg) == "cs":
                # Delphi try..finally: "mov ax, CONT; push cs; push ax; ...; retf"
                # pushes a continuation that the finally block's retf jumps to.
                cont = continuation_target(img, dis, f, ins)
                if cont is not None:
                    f.continuations.add(cont)
                    f.labels.add(cont)
                    todo.append(cont)
            elif m == "call":
                op = ins.operands[0]
                if op.type == X.X86_OP_IMM:
                    work.append((seg, op.imm & 0xFFFF))
                else:
                    # call word ptr cs:[bx + TABLE]: table of local subroutines
                    for t in jumptable(img, dis, f, ins, local_only=False) or []:
                        work.append((seg, t))
            elif m == "lcall":
                r = img.rel[seg].get(a + 1)
                if r is not None and r.target[0] == "internal":
                    work.append((r.target[1], r.target[2]))
            if m in TERMINATORS:
                break
            a = nxt


def continuation_target(img: Image, dis: Disasm, f: Func, push_cs) -> int | None:
    """For 'push cs' find the offset pushed with it (mov r, imm before / push r
    or push imm after) and accept it as a continuation if it is code placed
    right after a retf, later in this segment."""
    seg = f.seg
    b = img.bytes[seg]
    a = push_cs.address
    nxt = dis.at(seg, a + push_cs.size)
    imm = None
    if nxt and nxt.mnemonic == "push" and nxt.operands:
        op = nxt.operands[0]
        if op.type == X.X86_OP_IMM:
            imm = op.imm & 0xFFFF
        elif op.type == X.X86_OP_REG:
            reg = nxt.reg_name(op.reg)
            # look back a few bytes for "mov reg, imm16"
            for back in (3, 4, 5, 6):
                p = a - back
                if p < 0:
                    continue
                prev = dis.at(seg, p)
                if prev and p + prev.size == a and prev.mnemonic == "mov" and len(prev.operands) == 2 \
                        and prev.operands[0].type == X.X86_OP_REG and prev.reg_name(prev.operands[0].reg) == reg \
                        and prev.operands[1].type == X.X86_OP_IMM:
                    imm = prev.operands[1].imm & 0xFFFF
                    break
    if imm is None or not (a < imm < len(b)) or imm < 1:
        return None
    if b[imm - 1] not in (0xCB,) and not (imm >= 3 and b[imm - 3] == 0xCA):
        return None
    return imm if dis.at(seg, imm) is not None else None


def jumptable(img: Image, dis: Disasm, f: Func, ins, local_only: bool = True) -> list[int] | None:
    """jmp/call word ptr cs:[bx + TABLE] -> list of targets (best effort)."""
    op = ins.operands[0]
    if op.type != X.X86_OP_MEM or op.size != 2:
        return None
    if ins.reg_name(op.mem.segment) != "cs" if op.mem.segment else True:
        return None
    base = op.mem.disp & 0xFFFF
    b = img.bytes[f.seg]
    out = []
    for i in range(256):
        p = base + 2 * i
        if p + 2 > len(b):
            break
        t = struct.unpack_from("<H", b, p)[0]
        # case targets lie inside the routine, after its entry and near the jump
        lo = f.entry if local_only else 1
        if not (lo <= t < len(b)) or abs(t - ins.address) > 0x1000 or dis.at(f.seg, t) is None:
            break
        out.append(t)
    return out or None


# --------------------------------------------------------------------------
# C++ emission
# --------------------------------------------------------------------------
REG16 = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"}
REG8 = {"al", "ah", "bl", "bh", "cl", "ch", "dl", "dh"}
REG32 = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"}
SREG = {"cs", "ds", "es", "ss", "fs", "gs"}


class Unsupported(Exception):
    pass


class Emitter:
    def __init__(self, img: Image, funcs: dict, imports: dict):
        self.img = img
        self.funcs = funcs
        self.imports = imports          # (module, ordinal) -> name
        self.used_imports: dict[tuple[str, int], int] = {}
        self.unsupported = collections.Counter()

    # ---- operands --------------------------------------------------------
    def reg_get(self, r: str, segn: int) -> str:
        if r in REG16 or r in REG8 or r in REG32:
            return f"c.{r}()"
        if r == "cs":
            return f"SEL{segn}"
        if r in SREG:
            return f"c.{r}"
        raise Unsupported(f"reg {r}")

    def reg_set(self, r: str, v: str) -> str:
        if r in REG16 or r in REG8 or r in REG32:
            return f"c.set_{r}({v});"
        if r in ("ds", "es", "ss", "fs", "gs"):
            return f"c.{r} = (uint16_t)({v});"
        raise Unsupported(f"set reg {r}")

    def ea(self, ins, op, segn: int, f: Func) -> tuple[str, str]:
        m = op.mem
        segr = ins.reg_name(m.segment) if m.segment else None
        base = ins.reg_name(m.base) if m.base else None
        idx = ins.reg_name(m.index) if m.index else None
        if segr is None:
            segr = "ss" if (base in ("bp", "ebp") or idx in ("bp", "ebp")) else "ds"
        parts = []
        if base:
            parts.append(f"c.{base}()")
        if idx:
            parts.append(f"c.{idx}()" + (f"*{m.scale}" if m.scale > 1 else ""))
        disp = m.disp & 0xFFFF
        # offset fixup patched into the displacement
        r = self.img.rel[f.seg].get(ins.address + ins.disp_offset) if ins.disp_offset else None
        if r is not None and r.src_type == 5 and r.target[0] == "internal":
            disp = r.target[2]
        if disp or not parts:
            parts.append(f"0x{disp:04X}")
        segexpr = f"SEL{segn}" if segr == "cs" else f"c.{segr}"
        return segexpr, "(uint16_t)(" + " + ".join(parts) + ")"

    def imm_value(self, ins, op, f: Func) -> str:
        """Immediate, honouring segment/offset relocations on it."""
        v = op.imm & (0xFFFFFFFF if op.size == 4 else 0xFFFF if op.size == 2 else 0xFF)
        if ins.imm_offset:
            r = self.img.rel[f.seg].get(ins.address + ins.imm_offset)
            if r is not None:
                t = r.target
                if r.src_type == 2:
                    if t[0] == "internal":
                        return f"SEL{t[1]}"
                    if t[0] in ("ordinal", "name"):
                        return f"imp_seg({self.imp_id(t)})"
                elif r.src_type == 5:
                    if t[0] == "internal":
                        return f"0x{t[2]:04X}"
                    if t[0] in ("ordinal", "name"):
                        return f"imp_off({self.imp_id(t)})"
        return f"0x{v:X}u"

    def imp_id(self, t) -> str:
        key = (t[1], t[2])
        self.used_imports.setdefault(key, len(self.used_imports))
        name = self.imports.get(key)
        if name is None:
            name = f"{t[1]}_{t[2]}"
        return f"IMP_{t[1]}_{t[2]}"

    def src(self, ins, op, segn: int, f: Func, size: int | None = None) -> str:
        sz = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_get(ins.reg_name(op.reg), segn)
        if op.type == X.X86_OP_IMM:
            return self.imm_value(ins, op, f)
        if op.type == X.X86_OP_MEM:
            s, o = self.ea(ins, op, segn, f)
            return f"c.rd{sz * 8}({s}, {o})"
        raise Unsupported("operand")

    def dst(self, ins, op, v: str, segn: int, f: Func, size: int | None = None) -> str:
        sz = size or op.size
        if op.type == X.X86_OP_REG:
            return self.reg_set(ins.reg_name(op.reg), v)
        if op.type == X.X86_OP_MEM:
            s, o = self.ea(ins, op, segn, f)
            return f"c.wr{sz * 8}({s}, {o}, {v});"
        raise Unsupported("dst")

    # ---- instructions ----------------------------------------------------
    def emit_func(self, f: Func) -> str:
        segn = f.seg
        out = [f"void {f.name}(Cpu& c) {{"]
        if f.bad:
            out.append(f'    c.bad_function({segn}, 0x{f.entry:04X}, "{f.bad}");')
            out.append("}")
            return "\n".join(out)
        addrs = sorted(f.insns)
        out.append(f"    RECOMP_TRACE({segn}, 0x{f.entry:04X});")
        if addrs[0] != f.entry:
            out.append(f"    goto L_{f.entry:04X};")
        # fall-through into a non-adjacent instruction needs a label too
        for i, a in enumerate(addrs):
            ins = f.insns[a]
            nxt = a + ins.size
            if ins.mnemonic.split()[-1] not in TERMINATORS and (i + 1 >= len(addrs) or addrs[i + 1] != nxt):
                f.labels.add(nxt)
        prev_falls = False
        prev_next = None
        for a in addrs:
            ins = f.insns[a]
            if prev_falls and prev_next != a:
                out.append(f"    goto L_{prev_next:04X};")
            if a in f.labels and a not in f.stops:
                out.append(f"L_{a:04X}:")
            try:
                code = self.insn(ins, f, segn)
            except Unsupported as e:
                self.unsupported[ins.mnemonic] += 1
                code = f'c.unsupported({segn}, 0x{a:04X}, "{ins.mnemonic} {ins.op_str}"); return;'
                _ = e
            out.append(f"    {code}  // {a:04X} {ins.mnemonic} {ins.op_str}")
            m = ins.mnemonic.split()[-1]
            prev_falls = m not in TERMINATORS
            prev_next = a + ins.size
        if prev_falls and prev_next not in f.stops:
            out.append(f'    c.fell_off({segn}, 0x{prev_next:04X}); return;')
        for s in sorted(f.stops):
            out.append(f"L_{s:04X}: c.fell_off({segn}, 0x{s:04X}); return;")
        out.append("}")
        return "\n".join(out)

    def call_target(self, seg: int, off: int) -> str:
        if (seg, off) in self.funcs and not self.funcs[(seg, off)].bad:
            return f"f_{seg:02d}_{off:04X}(c);"
        return f"c.call_far(SEL{seg}, 0x{off:04X});"

    def insn(self, ins, f: Func, segn: int) -> str:  # noqa: C901
        words = ins.mnemonic.split()
        m = words[-1]
        prefix = words[0] if len(words) > 1 else ""
        ops = ins.operands
        nxt = (ins.address + ins.size) & 0xFFFF
        S = lambda i, sz=None: self.src(ins, ops[i], segn, f, sz)  # noqa: E731
        D = lambda i, v, sz=None: self.dst(ins, ops[i], v, segn, f, sz)  # noqa: E731
        W = lambda i: ops[i].size * 8  # noqa: E731

        if m == "nop" or m == "wait":
            return ";"
        if m == "mov":
            if ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg) in SREG:
                return self.reg_set(ins.reg_name(ops[0].reg), S(1))
            return D(0, S(1, ops[0].size))
        if m in ("movzx", "movsx"):
            sw = W(1)
            v = S(1)
            if m == "movsx":
                v = f"(uint{W(0)}_t)(int{W(0)}_t)(int{sw}_t)({v})"
            return D(0, v)
        if m == "lea":
            _, o = self.ea(ins, ops[1], segn, f)
            return D(0, o)
        if m in ("les", "lds", "lfs", "lgs", "lss"):
            s, o = self.ea(ins, ops[1], segn, f)
            sr = m[1:]
            return (f"{{ uint16_t o_ = {o}; uint16_t v_ = c.rd16({s}, o_); "
                    f"c.{sr} = c.rd16({s}, (uint16_t)(o_ + 2)); {self.reg_set(ins.reg_name(ops[0].reg), 'v_')} }}")
        if m == "xchg":
            w = W(0)
            return f"{{ uint{w}_t t_ = {S(0)}; {D(0, S(1))} {D(1, 't_')} }}"
        if m == "push":
            if ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg) in REG32:
                return f"c.push32({S(0)});"
            if ops[0].type == X.X86_OP_IMM:
                return f"c.push16((uint16_t)({S(0)}));"
            return f"c.push16({S(0)});"
        if m == "pop":
            if ops[0].type == X.X86_OP_REG and ins.reg_name(ops[0].reg) in REG32:
                return D(0, "c.pop32()")
            return D(0, "c.pop16()")
        if m in ("pushaw", "pusha"):
            return "c.pusha();"
        if m in ("popaw", "popa"):
            return "c.popa();"
        if m in ("pushal", "pushad"):
            return "c.pushad();"
        if m in ("popal", "popad"):
            return "c.popad();"
        if m in ("pushf", "pushfw"):
            return "c.push16(c.get_flags());"
        if m in ("popf", "popfw"):
            return "c.set_flags(c.pop16());"
        if m == "lahf":
            return "c.set_ah((uint8_t)c.get_flags());"
        if m == "sahf":
            return "c.set_flags((uint16_t)((c.get_flags() & 0xFF00) | c.ah()));"
        # capstone names the one-byte 0x98/0x99 "cwde"/"cdq" even in 16-bit
        # mode; without an operand-size prefix they are CBW/CWD.
        if m == "cbw" or (m == "cwde" and ins.size == 1):
            return "c.set_ax((uint16_t)(int16_t)(int8_t)c.al());"
        if m == "cwd" or (m == "cdq" and ins.size == 1):
            return "c.set_dx((c.ax() & 0x8000) ? 0xFFFF : 0);"
        if m == "cwde":
            return "c.set_eax((uint32_t)(int32_t)(int16_t)c.ax());"
        if m == "cdq":
            return "c.set_edx((c.eax() & 0x80000000u) ? 0xFFFFFFFFu : 0);"
        if m in ("add", "adc", "sub", "sbb", "and", "or", "xor"):
            w = W(0)
            return D(0, f"c.{m}{w}({S(0)}, {S(1, ops[0].size)})")
        if m == "cmp":
            return f"c.sub{W(0)}({S(0)}, {S(1, ops[0].size)});"
        if m == "test":
            return f"c.and{W(0)}({S(0)}, {S(1, ops[0].size)});"
        if m in ("inc", "dec", "neg", "not"):
            return D(0, f"c.{m}{W(0)}({S(0)})")
        if m in ("shl", "sal", "shr", "sar", "rol", "ror", "rcl", "rcr"):
            cnt = S(1) if len(ops) > 1 else "1"
            mm = "shl" if m == "sal" else m
            return D(0, f"c.{mm}{W(0)}({S(0)}, (uint8_t)({cnt}))")
        if m in ("shld", "shrd"):
            return D(0, f"c.{m}{W(0)}({S(0)}, {S(1)}, (uint8_t)({S(2)}))")
        if m in ("mul", "div", "idiv") or (m == "imul" and len(ops) == 1):
            return f"c.{m}{W(0)}({S(0)});"
        if m == "imul":
            if len(ops) == 2:
                return D(0, f"c.imul{W(0)}_2({S(0)}, {S(1)})")
            return D(0, f"c.imul{W(0)}_2({S(1)}, {S(2)})")
        if m in JCC:
            t = ops[0].imm & 0xFFFF
            cond = {"jo": "c.of", "jno": "!c.of", "jb": "c.cf", "jae": "!c.cf", "je": "c.zf",
                    "jne": "!c.zf", "jbe": "(c.cf || c.zf)", "ja": "(!c.cf && !c.zf)", "js": "c.sf",
                    "jns": "!c.sf", "jp": "c.pf", "jnp": "!c.pf", "jl": "(c.sf != c.of)",
                    "jge": "(c.sf == c.of)", "jle": "(c.zf || c.sf != c.of)",
                    "jg": "(!c.zf && c.sf == c.of)", "jcxz": "(c.cx() == 0)", "jecxz": "(c.ecx() == 0)",
                    "loop": "(c.set_cx((uint16_t)(c.cx() - 1)), c.cx() != 0)",
                    "loope": "(c.set_cx((uint16_t)(c.cx() - 1)), c.cx() != 0 && c.zf)",
                    "loopne": "(c.set_cx((uint16_t)(c.cx() - 1)), c.cx() != 0 && !c.zf)"}[m]
            return f"if ({cond}) goto L_{t:04X};"
        if m == "jmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xFFFF
                if t in f.insns:
                    return f"goto L_{t:04X};"
                return self.call_target(segn, t) + " return;"
            tab = f.jumptables.get(ins.address)
            v = S(0)
            if tab:
                cases = " ".join(f"case 0x{t:04X}: goto L_{t:04X};" for t in sorted(set(tab)))
                return f"switch ({v}) {{ {cases} default: c.bad_jump({segn}, {v}); return; }}"
            return f"c.jump_near(SEL{segn}, {v}); return;"
        if m == "ljmp":
            op = ops[0]
            if op.type == X.X86_OP_IMM:
                r = self.img.rel[segn].get(ins.address + 1)
                if r is not None and r.target[0] == "internal":
                    return self.call_target(r.target[1], r.target[2]) + " return;"
                if r is not None:
                    return f"imp_call(c, {self.imp_id(r.target)}); return;"
                raise Unsupported("ljmp abs")
            s, o = self.ea(ins, op, segn, f)
            return f"{{ uint16_t o_ = {o}; c.call_far(c.rd16({s}, (uint16_t)(o_ + 2)), c.rd16({s}, o_)); }} return;"
        if m == "call":
            op = ops[0]
            pre = f"c.push16(0x{nxt:04X});"
            if op.type == X.X86_OP_IMM:
                t = op.imm & 0xFFFF
                return f"{pre} {self.call_target(segn, t)}"
            return f"{{ uint16_t t_ = {S(0)}; {pre} c.call_far(SEL{segn}, t_); }}"
        if m == "lcall":
            op = ops[0]
            pre = f"c.push16(SEL{segn}); c.push16(0x{nxt:04X});"
            if op.type == X.X86_OP_IMM:
                r = self.img.rel[segn].get(ins.address + 1)
                if r is None:
                    raise Unsupported("lcall without relocation")
                t = r.target
                if t[0] == "internal":
                    return f"{pre} {self.call_target(t[1], t[2])}"
                return f"{pre} imp_call(c, {self.imp_id(t)});"
            s, o = self.ea(ins, op, segn, f)
            return (f"{{ uint16_t o_ = {o}; uint16_t to_ = c.rd16({s}, o_); uint16_t ts_ = c.rd16({s}, (uint16_t)(o_ + 2)); "
                    f"{pre} c.call_far(ts_, to_); }}")
        if m == "ret":
            n = ops[0].imm if ops else 0
            return f"c.ret_near({n}); return;"
        if m == "retf":
            n = ops[0].imm if ops else 0
            if f.continuations and n == 0:
                cases = " ".join(f"case 0x{t:04X}: c.set_sp((uint16_t)(c.sp() + 4)); goto L_{t:04X};"
                                 for t in sorted(f.continuations))
                return (f"if (c.rd16(c.ss, (uint16_t)(c.sp() + 2)) == SEL{segn}) "
                        f"switch (c.rd16(c.ss, c.sp())) {{ {cases} default: break; }} c.ret_far(0); return;")
            return f"c.ret_far({n}); return;"
        if m == "iret":
            return "c.iret(); return;"
        if m == "enter":
            return f"c.enter({ops[0].imm}, {ops[1].imm});"
        if m == "leave":
            return "c.leave();"
        if m in ("clc", "stc", "cmc", "cld", "std", "cli", "sti"):
            return {"clc": "c.cf = false;", "stc": "c.cf = true;", "cmc": "c.cf = !c.cf;",
                    "cld": "c.df = false;", "std": "c.df = true;", "cli": ";", "sti": ";"}[m]
        if m == "salc":
            return "c.set_al(c.cf ? 0xFF : 0);"
        if m == "int":
            return f"c.interrupt(0x{ops[0].imm:02X});"
        if m == "int3":
            return "c.interrupt(3);"
        if m == "xlatb":
            seg = "c.ds"
            return f"c.set_al(c.rd8({seg}, (uint16_t)(c.bx() + c.al())));"
        if m in ("daa", "das", "aaa", "aas"):
            return f"c.{m}();"
        if m in ("aam", "aad"):
            base = ops[0].imm if ops else 10
            return f"c.{m}({base});"
        if m[:-1] in ("movs", "stos", "lods", "cmps", "scas") and m[-1] in "bwd":
            sz = {"b": 8, "w": 16, "d": 32}[m[-1]]
            srcseg = "c.ds"
            if m.startswith(("movs", "lods", "cmps")) and ops:
                mem_ops = [o for o in ops if o.type == X.X86_OP_MEM]
                for o in mem_ops:
                    if o.mem.base and ins.reg_name(o.mem.base) in ("si", "esi") and o.mem.segment:
                        sr = ins.reg_name(o.mem.segment)
                        srcseg = f"SEL{segn}" if sr == "cs" else f"c.{sr}"
            rep = {"rep": 1, "repe": 1, "repz": 1, "repne": 2, "repnz": 2}.get(prefix, 0)
            return f"c.{m[:-1]}{sz}({srcseg}, {rep});"
        if m.startswith("f"):
            return self.fpu(ins, f, segn)
        if m.startswith("set") and len(ops) == 1:
            cc = m[3:]
            cond = {"o": "c.of", "no": "!c.of", "b": "c.cf", "ae": "!c.cf", "e": "c.zf", "ne": "!c.zf",
                    "be": "(c.cf||c.zf)", "a": "(!c.cf&&!c.zf)", "s": "c.sf", "ns": "!c.sf",
                    "l": "(c.sf!=c.of)", "ge": "(c.sf==c.of)", "le": "(c.zf||c.sf!=c.of)",
                    "g": "(!c.zf&&c.sf==c.of)"}.get(cc)
            if cond:
                return D(0, f"(uint8_t)({cond} ? 1 : 0)")
        if m in ("bt", "bts", "btr", "btc"):
            return D(0, f"c.{m}{W(0)}({S(0)}, {S(1, ops[0].size)})") if m != "bt" else f"c.bt{W(0)}({S(0)}, {S(1, ops[0].size)});"
        raise Unsupported(m)

    def fpu(self, ins, f: Func, segn: int) -> str:
        m = ins.mnemonic
        ops = ins.operands

        def memarg(op):
            s, o = self.ea(ins, op, segn, f)
            return s, o

        def sti(op):
            r = ins.reg_name(op.reg)
            return int(r[3]) if r.startswith("st(") else 0

        if m in ("fbstp", "fbld") and ops and ops[0].type == X.X86_OP_MEM:
            s, o = memarg(ops[0])
            return f"c.fpu_{m}({s}, {o});"
        if m in ("fld", "fild", "fst", "fstp", "fist", "fistp") and ops and ops[0].type == X.X86_OP_MEM:
            s, o = memarg(ops[0])
            kind = {("fld", 4): "f32", ("fld", 8): "f64", ("fld", 10): "f80",
                    ("fild", 2): "i16", ("fild", 4): "i32", ("fild", 8): "i64",
                    ("fst", 4): "f32", ("fst", 8): "f64", ("fstp", 4): "f32", ("fstp", 8): "f64",
                    ("fstp", 10): "f80", ("fist", 2): "i16", ("fist", 4): "i32",
                    ("fistp", 2): "i16", ("fistp", 4): "i32", ("fistp", 8): "i64"}.get((m, ops[0].size))
            if kind is None:
                raise Unsupported(f"{m} size {ops[0].size}")
            if m in ("fld", "fild"):
                return f"c.fpu_load_{kind}({s}, {o});"
            pop = "true" if m.endswith("p") else "false"
            return f"c.fpu_store_{kind}({s}, {o}, {pop});"
        if m in ("fld", "fst", "fstp", "fxch") and ops and ops[0].type == X.X86_OP_REG:
            i = sti(ops[0])
            return f"c.fpu_{m}_st({i});"
        arith = {"fadd": "add", "fsub": "sub", "fsubr": "subr", "fmul": "mul", "fdiv": "div", "fdivr": "divr",
                 "fiadd": "add", "fisub": "sub", "fisubr": "subr", "fimul": "mul", "fidiv": "div", "fidivr": "divr",
                 "fcom": "com", "fcomp": "comp", "ficom": "com", "ficomp": "comp"}
        base = m[:-1] if m.endswith("p") and m not in ("fcomp", "ficomp") and m[:-1] in arith else m
        if base in arith:
            op_ = arith[base]
            pop = m.endswith("p") and m not in ("fcomp", "ficomp")
            if ops and ops[0].type == X.X86_OP_MEM:
                s, o = memarg(ops[0])
                kind = ("i16" if ops[0].size == 2 else "i32") if m.startswith("fi") else ("f32" if ops[0].size == 4 else "f64")
                return f"c.fpu_{op_}_mem_{kind}({s}, {o});"
            if len(ops) == 2:
                d, s_ = sti(ops[0]), sti(ops[1])
                return f"c.fpu_{op_}_st({d}, {s_}, {'true' if pop else 'false'});"
            if len(ops) == 1:
                return f"c.fpu_{op_}_st(0, {sti(ops[0])}, false);"
            # no operands: st(1) op st(0), pop
            return f"c.fpu_{op_}_st(1, 0, true);" if op_ not in ("com", "comp") else f"c.fpu_{op_}_st(0, 1, false);"
        if m == "fcompp":
            return "c.fpu_compp();"
        simple = {"fchs", "fabs", "fsqrt", "frndint", "fld1", "fldz", "fldpi", "fldl2e", "fldln2", "fldlg2",
                  "fldl2t", "ftst", "fxam", "fscale", "fprem", "fprem1", "f2xm1", "fyl2x", "fyl2xp1",
                  "fptan", "fpatan", "fsin", "fcos", "fsincos", "fninit", "finit", "fnclex", "fclex",
                  "fincstp", "fdecstp", "fwait", "fxtract"}
        if m in simple:
            return f"c.fpu_{m}();"
        if m in ("fnstsw", "fstsw"):
            if ops and ops[0].type == X.X86_OP_REG:
                return "c.set_ax(c.fpu_status());"
            s, o = memarg(ops[0])
            return f"c.wr16({s}, {o}, c.fpu_status());"
        if m in ("fnstcw", "fstcw"):
            s, o = memarg(ops[0])
            return f"c.wr16({s}, {o}, c.fpu_control);"
        if m == "fldcw":
            s, o = memarg(ops[0])
            return f"c.fpu_control = c.rd16({s}, {o});"
        if m == "ffree":
            return f"c.fpu_ffree({sti(ops[0])});"
        raise Unsupported(m)


# --------------------------------------------------------------------------
def load_imports(root: str) -> dict:
    out = {}
    p = os.path.join(root, "analysis", "imports.csv")
    if os.path.isfile(p):
        for r in csv.DictReader(open(p, encoding="utf-8")):
            if r["ordinal"]:
                out[(r["module"], int(r["ordinal"]))] = r["name"]
    return out


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    exe, out = sys.argv[1], sys.argv[2]
    extra = []
    if "--extra-entries" in sys.argv:
        p = sys.argv[sys.argv.index("--extra-entries") + 1]
        for line in open(p, encoding="utf-8"):
            line = line.split("#")[0].strip()
            if line:
                s, o = line.split(":")
                extra.append((int(s), int(o, 16)))
    root = os.path.dirname(os.path.dirname(HERE))
    os.makedirs(out, exist_ok=True)
    img = Image(exe)
    dis = Disasm(img)
    funcs = discover(img, dis, extra)
    em = Emitter(img, funcs, load_imports(root))

    by_seg = collections.defaultdict(list)
    for f in funcs.values():
        by_seg[f.seg].append(f)

    header = ['#pragma once', '#include "recomp/runtime/cpu.hpp"', 'namespace recomp {']
    for f in sorted(funcs.values(), key=lambda f: (f.seg, f.entry)):
        header.append(f"void {f.name}(Cpu& c);")
    header.append("}")

    for seg, fs in sorted(by_seg.items()):
        body = [f"// Generated by tools/recomp/recomp.py from NE segment {seg}. Do not edit.",
                '#include "funcs.hpp"', '#include "recomp/runtime/generated_support.hpp"',
                "#ifdef _MSC_VER", "#pragma warning(disable: 4102 4100 4189 4065)", "#endif",
                "namespace recomp {"]
        for f in sorted(fs, key=lambda f: f.entry):
            body.append(em.emit_func(f))
            body.append("")
        body.append("}")
        with open(os.path.join(out, f"seg{seg:02d}.cpp"), "w", encoding="utf-8") as fh:
            fh.write("\n".join(body) + "\n")

    # imports referenced by code
    imp_lines = ['#include "recomp/runtime/generated_support.hpp"', "namespace recomp {",
                 "const ImportInfo kImports[] = {"]
    for (mod, ordv), idx in sorted(em.used_imports.items(), key=lambda x: x[1]):
        name = em.imports.get((mod, ordv), f"{mod}_{ordv}")
        imp_lines.append(f'    {{"{mod}", {ordv}, "{name}"}},  // IMP_{mod}_{ordv} = {idx}')
    imp_lines.append("};")
    imp_lines.append(f"const int kImportCount = {len(em.used_imports)};")
    imp_lines.append("}")
    with open(os.path.join(out, "imports.cpp"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(imp_lines) + "\n")
    ids = ["#pragma once", "namespace recomp {", "enum ImportId : int {"]
    for (mod, ordv), idx in sorted(em.used_imports.items(), key=lambda x: x[1]):
        ids.append(f"    IMP_{mod}_{ordv} = {idx},")
    ids.append("};", )
    ids.append("}")
    with open(os.path.join(out, "import_ids.hpp"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(ids) + "\n")
    header.insert(2, '#include "import_ids.hpp"')
    with open(os.path.join(out, "funcs.hpp"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(header) + "\n")

    disp = ['#include "funcs.hpp"', '#include "recomp/runtime/generated_support.hpp"',
            "namespace recomp {", "const FuncEntry kFuncs[] = {"]
    for f in sorted(funcs.values(), key=lambda f: (sel(f.seg), f.entry)):
        disp.append(f"    {{0x{sel(f.seg):04X}, 0x{f.entry:04X}, &{f.name}}},")
    disp.append("};")
    disp.append(f"const int kFuncCount = {len(funcs)};")
    disp.append("}")
    with open(os.path.join(out, "dispatch.cpp"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(disp) + "\n")

    bad = [f for f in funcs.values() if f.bad]
    ninsn = sum(len(f.insns) for f in funcs.values())
    with open(os.path.join(out, "report.txt"), "w", encoding="utf-8") as fh:
        fh.write(f"functions: {len(funcs)} (bad: {len(bad)})\ninstructions: {ninsn}\n")
        fh.write(f"imports used: {len(em.used_imports)}\n")
        fh.write("unsupported mnemonics: " + repr(em.unsupported.most_common()) + "\n")
        for f in sorted(bad, key=lambda f: (f.seg, f.entry)):
            fh.write(f"bad {f.seg}:{f.entry:04X} {f.bad}\n")
    print(f"functions: {len(funcs)} (bad {len(bad)}), instructions: {ninsn}, "
          f"imports: {len(em.used_imports)}, unsupported: {sum(em.unsupported.values())}")
    print("unsupported:", em.unsupported.most_common(20))
    return 0


if __name__ == "__main__":
    sys.exit(main())
