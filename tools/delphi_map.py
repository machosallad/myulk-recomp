"""Recover the Delphi 1 class structure of MYULK.EXE.

Usage:
    python tools/delphi_map.py [EXE] [FORMS_DIR] [OUT_DIR]

Defaults: extracted/iso/MYULK/MYULK.EXE, extracted/resources/MYULK/forms,
analysis/

Writes:
    analysis/classes.json   every class: VMT location, parent, unit, size,
                            virtual methods, published methods and fields
    analysis/events.json    form -> component -> event -> handler address
    analysis/symbols.csv    seg,offset,name — named code locations

Delphi 1 (16-bit) VMT layout, offsets from the VMT pointer, all in the
class's own code segment (near pointers are offsets in that segment):

    -32  near  TypeInfo
    -30  near  FieldTable
    -28  near  MethodTable
    -26  near  DynamicTable
    -24  near  ClassName (shortstring)
    -22  word  InstanceSize
    -20  far   Parent VMT
    -16  far   DefaultHandler
    -12  far   NewInstance
     -8  far   FreeInstance
     -4  far   Destroy
      0  far[] user virtual methods (until the ClassName string)

MethodTable : word count, { far code ptr, shortstring name }
FieldTable  : word count, near class-table ptr,
              { word offset, word class index, shortstring name }
DynamicTable: word count, word index[count], far ptr[count]
TypeInfo    : byte kind(7), shortstring name, far ClassType,
              far ParentInfo, word PropCount, shortstring UnitName, ...
"""
from __future__ import annotations

import csv
import glob
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ne import NEFile  # noqa: E402

STD_VIRTUALS = {-16: "DefaultHandler", -12: "NewInstance", -8: "FreeInstance",
                -4: "Destroy"}


class DelphiImage:
    def __init__(self, path: str):
        self.ne = NEFile(path)
        self.d = self.ne.data
        self.segs = {s.index: s for s in self.ne.segments()}
        # (seg, offset) -> relocation target for far pointers
        self.far: dict[tuple[int, int], tuple] = {}
        # (seg, offset) -> target for 16-bit segment fixups
        self.segfix: dict[tuple[int, int], tuple] = {}
        for s in self.segs.values():
            for r in self.ne.relocations(s):
                if r.src_type == 3:
                    self.far[(s.index, r.offset)] = r.target
                elif r.src_type == 2:
                    self.segfix[(s.index, r.offset)] = r.target

    # --- raw access ------------------------------------------------
    def w(self, seg: int, off: int) -> int:
        return struct.unpack_from("<H", self.d, self.segs[seg].offset + off)[0]

    def b(self, seg: int, off: int) -> int:
        return self.d[self.segs[seg].offset + off]

    def sstr(self, seg: int, off: int) -> str:
        base = self.segs[seg].offset + off
        return self.d[base + 1:base + 1 + self.d[base]].decode("cp1252")

    def farptr(self, seg: int, off: int):
        """Resolve a relocated far pointer -> (seg, off), import tuple or None."""
        t = self.far.get((seg, off))
        if t is None:
            return None
        if t[0] == "internal":
            return (t[1], t[2])
        return t

    # --- class discovery -------------------------------------------
    def find_vmts(self) -> list[tuple[int, int]]:
        out = []
        for s in self.segs.values():
            if s.is_data:
                continue
            base, n = s.offset, s.length
            for v in range(32, n):
                cn = struct.unpack_from("<H", self.d, base + v - 24)[0]
                if not (v <= cn < n - 1):
                    continue
                ln = self.d[base + cn]
                if not (2 <= ln < 64):
                    continue
                name = self.d[base + cn + 1:base + cn + 1 + ln]
                if not name.isascii() or not name.decode().isidentifier():
                    continue
                ti = struct.unpack_from("<H", self.d, base + v - 32)[0]
                if ti:
                    if not (ti < n and self.d[base + ti] == 7
                            and self.d[base + ti + 1:base + ti + 2 + ln] == bytes([ln]) + name):
                        continue
                elif name != b"TObject":
                    # no RTTI: require a relocated parent pointer to a code segment
                    t = self.far.get((s.index, v - 20))
                    if not (t and t[0] == "internal" and t[1] in self.segs
                            and not self.segs[t[1]].is_data):
                        continue
                # user virtual slots must fit exactly before the name
                if (cn - v) % 4:
                    continue
                out.append((s.index, v))
        return out

    def read_class(self, seg: int, v: int) -> dict:
        cn = self.w(seg, v - 24)
        name = self.sstr(seg, cn)
        c: dict = {"name": name, "seg": seg, "vmt": v,
                   "instance_size": self.w(seg, v - 22)}
        parent = self.farptr(seg, v - 20)
        c["parent_vmt"] = list(parent) if isinstance(parent, tuple) and len(parent) == 2 else None

        ti = self.w(seg, v - 32)
        if ti:
            p = ti + 2 + self.b(seg, ti + 1) + 8
            c["prop_count"] = self.w(seg, p)
            c["unit"] = self.sstr(seg, p + 2)

        virt = []
        for off, nm in STD_VIRTUALS.items():
            t = self.farptr(seg, v + off)
            virt.append({"slot": off, "std": nm, "addr": _fmt(t)})
        for i in range((cn - v) // 4):
            virt.append({"slot": i * 4, "addr": _fmt(self.farptr(seg, v + i * 4))})
        c["virtuals"] = virt

        mt = self.w(seg, v - 28)
        methods = []
        if mt:
            cnt, p = self.w(seg, mt), mt + 2
            for _ in range(cnt):
                addr = self.farptr(seg, p)
                nm = self.sstr(seg, p + 4)
                methods.append({"name": nm, "addr": _fmt(addr)})
                p += 4 + 1 + self.b(seg, p + 4)
        c["methods"] = methods

        ft = self.w(seg, v - 30)
        fields = []
        if ft:
            cnt, ctab, p = self.w(seg, ft), self.w(seg, ft + 2), ft + 4
            for _ in range(cnt):
                off, idx = self.w(seg, p), self.w(seg, p + 2)
                nm = self.sstr(seg, p + 4)
                cls = None
                if ctab:  # class table: word count, far VMT ptr[count]
                    cls_ptr = self.farptr(seg, ctab + 2 + 4 * idx)
                    cls = list(cls_ptr) if isinstance(cls_ptr, tuple) and len(cls_ptr) == 2 else None
                fields.append({"name": nm, "offset": off, "class_vmt": cls})
                p += 4 + 1 + self.b(seg, p + 4)
        c["fields"] = fields

        dt = self.w(seg, v - 26)
        dyn = []
        if dt:
            cnt = self.w(seg, dt)
            for i in range(cnt):
                idx = self.w(seg, dt + 2 + 2 * i)
                addr = self.farptr(seg, dt + 2 + 2 * cnt + 4 * i)
                dyn.append({"index": idx, "addr": _fmt(addr)})
        c["dynamic"] = dyn
        return c


    # --- program body (.dpr) -------------------------------------------
    def parse_program(self, class_names: dict) -> dict:
        """Decode the main program: unit init calls and Application.CreateForm.

        Entry point is seg 1; the body starts with a run of far calls (InitTask,
        System init, each unit's initialization), then
        CreateForm(VMT, @FormVar) calls, then Application.Run and Halt.
        """
        from capstone import CS_ARCH_X86, CS_MODE_16, Cs
        seg = self.ne.cs_ip[1]
        start = self.ne.cs_ip[0]
        base = self.segs[seg].offset
        md = Cs(CS_ARCH_X86, CS_MODE_16)
        code = self.d[base + start:base + self.segs[seg].length]
        inits, forms, other = [], [], []
        create_form = None
        window: list = []
        for ins in md.disasm(code, start):
            window = (window + [ins])[-12:]
            if ins.mnemonic == "lcall":
                t = self.far.get((seg, ins.address + 1))
                if t is None:
                    continue
                if t[0] != "internal":
                    other.append({"at": f"{seg}:{ins.address:04X}", "target": _fmt(t)})
                    continue
                tgt = (t[1], t[2])
                pushes = [i for i in window[:-1] if i.mnemonic == "mov" and i.op_str.startswith("di, ")]
                segmov = [i for i in window[:-1] if i.mnemonic == "mov" and i.op_str.startswith("ax, ")]
                is_cf = (len(pushes) >= 2 and segmov
                         and (seg, segmov[-1].address + 1) in self.segfix
                         and "ptr [" in " ".join(i.op_str for i in window if i.mnemonic == "les"))
                if is_cf and (create_form is None or tgt == create_form):
                    create_form = tgt
                    sf = self.segfix[(seg, segmov[-1].address + 1)]
                    vmt = int(pushes[-2].op_str.split(", ")[1], 16)
                    var = int(pushes[-1].op_str.split(", ")[1], 16)
                    cls = class_names.get((sf[1], vmt), f"{sf[1]}:{vmt:04X}")
                    forms.append({"class": cls, "var_ds": var})
                elif not forms:
                    inits.append(_fmt(tgt))
                else:
                    other.append({"at": f"{seg}:{ins.address:04X}", "target": _fmt(tgt)})
            elif ins.mnemonic in ("ret", "retf"):
                break
        return {"entry": f"{seg}:{start:04X}", "unit_inits": inits,
                "create_form": _fmt(create_form) if create_form else None,
                "forms": forms, "after_forms": other}


def _fmt(t):
    if t is None:
        return None
    if len(t) == 2 and isinstance(t[0], int):
        return f"{t[0]}:{t[1]:04X}"
    return f"{t[1]}.{t[2]}"


# ----------------------------------------------------------------------
def walk_events(comp: dict, cls_methods: dict, out: dict, form_cls: str):
    for k, v in comp["props"].items():
        if k.startswith("On") and isinstance(v, dict) and "ident" in v:
            h = v["ident"]
            out.setdefault(comp["name"] or form_cls, {})[k] = {
                "handler": h, "addr": cls_methods.get(h.upper())}
    for c in comp["children"]:
        walk_events(c, cls_methods, out, form_cls)


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    exe = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "extracted", "iso", "MYULK", "MYULK.EXE")
    forms = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "extracted", "resources", "MYULK", "forms")
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(root, "analysis")
    os.makedirs(out, exist_ok=True)

    img = DelphiImage(exe)
    classes = [img.read_class(s, v) for s, v in img.find_vmts()]
    by_vmt = {(c["seg"], c["vmt"]): c for c in classes}
    for c in classes:
        p = c.pop("parent_vmt")
        c["parent"] = by_vmt[tuple(p)]["name"] if p and tuple(p) in by_vmt else None
        for f in c["fields"]:
            cv = f.pop("class_vmt")
            f["class"] = by_vmt[tuple(cv)]["name"] if cv and tuple(cv) in by_vmt else None
    by_name = {c["name"]: c for c in classes}

    # method lookup including inherited published methods
    def all_methods(c: dict) -> dict:
        m = {}
        chain = []
        while c:
            chain.append(c)
            c = by_name.get(c["parent"]) if c["parent"] else None
        for k in reversed(chain):
            for x in k["methods"]:
                m[x["name"].upper()] = x["addr"]
        return m

    events: dict = {}
    for fj in sorted(glob.glob(os.path.join(forms, "*.json"))):
        with open(fj, encoding="utf-8") as f:
            tree = json.load(f)
        cls = by_name.get(tree["class"])
        if not cls:
            continue
        ev: dict = {}
        walk_events(tree, all_methods(cls), ev, tree["class"])
        events[tree["class"]] = {"unit": cls.get("unit"), "seg": cls["seg"], "events": ev}

    with open(os.path.join(out, "classes.json"), "w", encoding="utf-8") as f:
        json.dump(classes, f, ensure_ascii=False, indent=1)
    with open(os.path.join(out, "events.json"), "w", encoding="utf-8") as f:
        json.dump(events, f, ensure_ascii=False, indent=1)

    # program body: unit inits, form globals
    names = {(c["seg"], c["vmt"]): c["name"] for c in classes}
    prog = img.parse_program(names)
    seg_units: dict = {}
    for c in classes:
        if c.get("unit"):
            seg_units.setdefault(c["seg"], set()).add(c["unit"])
    prog["segment_units"] = {s: sorted(u) for s, u in sorted(seg_units.items())}
    with open(os.path.join(out, "program.json"), "w", encoding="utf-8") as f:
        json.dump(prog, f, ensure_ascii=False, indent=1)

    globals_ = [{"ds": f"{g['var_ds']:04X}", "name": g["class"][1:], "type": g["class"]}
                for g in prog["forms"]]
    with open(os.path.join(out, "globals.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.writer(f)
        wr.writerow(["ds_offset", "name", "type"])
        for g in sorted(globals_, key=lambda x: x["ds"]):
            wr.writerow([g["ds"], g["name"], g["type"]])

    syms: dict[str, str] = {prog["entry"]: "ProgramEntry"}
    if prog["create_form"]:
        syms[prog["create_form"]] = "TApplication.CreateForm"
    internal_after = [x["target"] for x in prog["after_forms"] if ":" in x["target"]]
    if len(internal_after) >= 2:   # body ends with Application.Run; Halt
        syms[internal_after[-2]] = "TApplication.Run"
        syms[internal_after[-1]] = "System.Halt"
    if prog["unit_inits"]:          # first init call is System's own
        syms[prog["unit_inits"][0]] = "System.InitSystem"
    for a in prog["unit_inits"]:
        s = int(a.split(":")[0])
        u = sorted(seg_units.get(s, [])) or [f"Seg{s}"]
        syms.setdefault(a, f"{'_'.join(u)}.UnitInit_{a.replace(':', '_')}")
    for c in classes:
        syms.setdefault(f"{c['seg']}:{c['vmt']:04X}", f"VMT_{c['name']}")
        for m in c["methods"]:
            if m["addr"] and ":" in m["addr"]:
                syms.setdefault(m["addr"], f"{c['name']}.{m['name']}")
        for vm in c["virtuals"]:
            a = vm["addr"]
            if a and ":" in a and vm.get("std"):
                syms.setdefault(a, f"{c['name']}.{vm['std']}")
    with open(os.path.join(out, "symbols.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.writer(f)
        wr.writerow(["seg", "offset", "name"])
        for a, nm in sorted(syms.items(), key=lambda x: (int(x[0].split(":")[0]), int(x[0].split(":")[1], 16))):
            s, o = a.split(":")
            wr.writerow([s, o, nm])

    n_ev = sum(len(e) for f in events.values() for e in f["events"].values())
    n_res = sum(1 for f in events.values() for e in f["events"].values()
                for x in e.values() if x["addr"])
    print(f"classes: {len(classes)}  published methods: {sum(len(c['methods']) for c in classes)}")
    print(f"forms linked: {len(events)}  events: {n_ev}  resolved to code: {n_res}")
    print(f"symbols: {len(syms)}  -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
