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
        self.method_types: dict[str, dict] = {}   # event type name -> signature
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
                dyn.append({"index": idx, "addr": _fmt(addr), "msg": msg_name(idx)})
        c["dynamic"] = dyn
        c["properties"] = self.read_props(seg, v) if ti else []
        return c

    def _accessor(self, seg: int, off: int, v: int):
        """Decode a PropInfo Get/Set/Stored slot.

        far relocated pointer -> static method; seg word FFFF -> field
        offset; FFFE -> virtual method slot (byte offset in VMT).
        """
        t = self.farptr(seg, off)
        if isinstance(t, tuple) and len(t) == 2:
            return {"kind": "static", "addr": _fmt(t)}
        lo, hi = self.w(seg, off), self.w(seg, off + 2)
        if hi == 0xFFFF:
            return {"kind": "field", "offset": lo}
        if hi == 0xFFFE:
            addr = self.farptr(seg, v + lo)
            return {"kind": "virtual", "slot": lo, "addr": _fmt(addr)}
        if hi == 0 and lo in (0, 1):
            return {"kind": "const", "value": bool(lo)}
        return {"kind": "raw", "value": f"{hi:04X}:{lo:04X}"}

    def read_method_type(self, seg: int, off: int) -> dict:
        """tkMethod TypeInfo: kind, name, byte MethodKind, byte ParamCount,
        { byte Flags, shortstring Name, shortstring TypeName }, [ResultType]."""
        p = off + 2 + self.b(seg, off + 1)
        kind, count = self.b(seg, p), self.b(seg, p + 1)
        p += 2
        params = []
        for _ in range(count):
            flags = self.b(seg, p)
            name = self.sstr(seg, p + 1)
            p += 2 + len(name)
            tname = self.sstr(seg, p)
            p += 1 + len(tname)
            params.append({"name": name, "type": tname,
                           "var": bool(flags & 1), "const": bool(flags & 2)})
        result = self.sstr(seg, p) if kind == 1 else None
        return {"kind": "function" if kind == 1 else "procedure",
                "params": params, "result": result}

    def read_props(self, seg: int, v: int) -> list[dict]:
        """Published properties declared by this class (not inherited).

        TypeInfo: kind, name, far ClassType, far ParentInfo, word PropCount
        (total incl. inherited), shortstring UnitName, word Count, then
        PropInfo: far PropType, far GetProc, far SetProc, far StoredProc,
        int16 Index, int32 Default, int16 NameIndex, shortstring Name.
        """
        ti = self.w(seg, v - 32)
        p = ti + 2 + self.b(seg, ti + 1) + 8 + 2
        p += 1 + self.b(seg, p)
        cnt = self.w(seg, p)
        p += 2
        props = []
        for _ in range(cnt):
            pt = self.farptr(seg, p)
            ptype = None
            if isinstance(pt, tuple) and len(pt) == 2 and pt[0] in self.segs:
                ptype = self.sstr(pt[0], pt[1] + 1)
                if self.b(pt[0], pt[1]) == 8:      # tkMethod: remember signature
                    self.method_types.setdefault(ptype, self.read_method_type(*pt))
            idx = struct.unpack_from("<h", self.d, self.segs[seg].offset + p + 16)[0]
            name = self.sstr(seg, p + 24)
            props.append({"name": name, "type": ptype,
                          "get": self._accessor(seg, p + 4, v),
                          "set": self._accessor(seg, p + 8, v),
                          "index": None if idx == -32768 else idx})
            p += 24 + 1 + self.b(seg, p + 24)
        return props


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


WM_NAMES = {
    0x01: "WMCreate", 0x02: "WMDestroy", 0x03: "WMMove", 0x05: "WMSize",
    0x06: "WMActivate", 0x07: "WMSetFocus", 0x08: "WMKillFocus", 0x0A: "WMEnable",
    0x0C: "WMSetText", 0x0D: "WMGetText", 0x0E: "WMGetTextLength", 0x0F: "WMPaint",
    0x10: "WMClose", 0x11: "WMQueryEndSession", 0x13: "WMQueryOpen", 0x14: "WMEraseBkgnd",
    0x15: "WMSysColorChange", 0x16: "WMEndSession", 0x18: "WMShowWindow",
    0x19: "WMCtlColor", 0x1A: "WMWinIniChange", 0x1C: "WMActivateApp",
    0x1D: "WMFontChange", 0x1E: "WMTimeChange", 0x1F: "WMCancelMode",
    0x20: "WMSetCursor", 0x21: "WMMouseActivate", 0x24: "WMGetMinMaxInfo",
    0x2B: "WMDrawItem", 0x2C: "WMMeasureItem", 0x2D: "WMDeleteItem",
    0x30: "WMSetFont", 0x31: "WMGetFont", 0x37: "WMQueryDragIcon", 0x39: "WMCompareItem",
    0x46: "WMWindowPosChanging", 0x47: "WMWindowPosChanged",
    0x81: "WMNCCreate", 0x82: "WMNCDestroy", 0x83: "WMNCCalcSize", 0x84: "WMNCHitTest",
    0x85: "WMNCPaint", 0x86: "WMNCActivate", 0x87: "WMGetDlgCode",
    0xA0: "WMNCMouseMove", 0xA1: "WMNCLButtonDown", 0xA3: "WMNCLButtonDblClk",
    0x100: "WMKeyDown", 0x101: "WMKeyUp", 0x102: "WMChar", 0x104: "WMSysKeyDown",
    0x105: "WMSysKeyUp", 0x106: "WMSysChar", 0x110: "WMInitDialog", 0x111: "WMCommand",
    0x112: "WMSysCommand", 0x113: "WMTimer", 0x114: "WMHScroll", 0x115: "WMVScroll",
    0x116: "WMInitMenu", 0x117: "WMInitMenuPopup", 0x11F: "WMMenuSelect",
    0x120: "WMMenuChar", 0x121: "WMEnterIdle",
    0x200: "WMMouseMove", 0x201: "WMLButtonDown", 0x202: "WMLButtonUp",
    0x203: "WMLButtonDblClk", 0x204: "WMRButtonDown", 0x205: "WMRButtonUp",
    0x206: "WMRButtonDblClk", 0x207: "WMMButtonDown", 0x208: "WMMButtonUp",
    0x210: "WMParentNotify", 0x211: "WMEnterMenuLoop", 0x212: "WMExitMenuLoop",
    0x220: "WMMDICreate", 0x222: "WMMDIActivate", 0x233: "WMDropFiles",
    0x300: "WMCut", 0x301: "WMCopy", 0x302: "WMPaste", 0x303: "WMClear", 0x304: "WMUndo",
    0x30F: "WMQueryNewPalette", 0x310: "WMPaletteIsChanging", 0x311: "WMPaletteChanged",
    0x3B9: "MMMCINotify",
}


def msg_name(idx: int) -> str:
    """Name for a dynamic-table index: Windows message, VCL CM_/CN_ message, or dynamic method."""
    if idx in WM_NAMES:
        return WM_NAMES[idx]
    if idx < 0x400:
        return f"WM_{idx:04X}"
    if 0xB000 <= idx < 0xBC00:          # CM_BASE / CN_BASE (VCL internal messages)
        return f"CM_{idx:04X}"
    if 0xBC00 <= idx < 0xC000:
        return f"CN_{idx:04X}"
    if idx >= 0xC000:
        return f"Dynamic_{idx:04X}"
    return f"Msg_{idx:04X}"


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
                "handler": h, "addr": cls_methods.get(h.upper()),
                "component_class": comp["class"]}
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
    # property accessors and message handlers, base classes first so the
    # declaring class (not a descendant) gets the name
    def depth(c: dict) -> int:
        n = 0
        while c and c.get("parent"):
            c = by_name.get(c["parent"])
            n += 1
        return n
    def owner(c: dict, addr: str) -> str:
        """Re-published properties keep the ancestor's accessor. If the class
        itself lives in the accessor's segment it owns it; otherwise credit
        the most basic ancestor in that segment (e.g. TControl, not
        TGraphicControl, for a Visible re-published by TGauge)."""
        seg = int(addr.split(":")[0])
        if c["seg"] == seg:
            return c["name"]
        best = None
        k = by_name.get(c["parent"]) if c.get("parent") else None
        while k:
            if k["seg"] == seg:
                best = k["name"]
            k = by_name.get(k["parent"]) if k.get("parent") else None
        return best or c["name"]

    for c in sorted(classes, key=depth):
        for pr in c.get("properties", []):
            for kind, prefix in (("get", "Get"), ("set", "Set")):
                acc = pr[kind]
                if acc["kind"] in ("static", "virtual") and acc.get("addr") and ":" in acc["addr"]:
                    syms.setdefault(acc["addr"], f"{owner(c, acc['addr'])}.{prefix}{pr['name']}")
        for dm in c["dynamic"]:
            if dm["addr"] and ":" in dm["addr"]:
                syms.setdefault(dm["addr"], f"{c['name']}.{dm['msg']}")
    # user virtual methods: name each slot after the class that first fills it
    # with a new address (base classes first)
    for c in sorted(classes, key=depth):
        parent = by_name.get(c["parent"]) if c.get("parent") else None
        inherited = {v["slot"]: v["addr"] for v in parent["virtuals"]} if parent else {}
        for vm in c["virtuals"]:
            a = vm["addr"]
            if vm.get("std") or not a or ":" not in a or inherited.get(vm["slot"]) == a:
                continue
            syms.setdefault(a, f"{c['name']}.Virtual_{vm['slot']:02X}")

    # hand-identified library routines override generated names
    lib = os.path.join(out, "library_names.csv")
    if os.path.isfile(lib):
        with open(lib, encoding="utf-8") as f:
            for row in csv.DictReader(f):
                syms[f"{int(row['seg'])}:{row['offset'].upper().zfill(4)}"] = row["name"]

    with open(os.path.join(out, "symbols.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.writer(f)
        wr.writerow(["seg", "offset", "name"])
        for a, nm in sorted(syms.items(), key=lambda x: (int(x[0].split(":")[0]), int(x[0].split(":")[1], 16))):
            s, o = a.split(":")
            wr.writerow([s, o, nm])

    # full signatures for event handlers, from the event property's tkMethod RTTI
    def prop_type(cls_name: str, prop: str):
        k = by_name.get(cls_name)
        while k:
            for pr in k.get("properties", []):
                if pr["name"] == prop:
                    return pr["type"]
            k = by_name.get(k["parent"]) if k.get("parent") else None
        return None

    def param_sig(p: dict) -> str:
        t = p["type"]
        if p["var"]:
            gt = f"ptr:{t}" if t in by_name else f"ptr:{t}"
        elif t in by_name:
            gt = f"ptr:{t}"
        elif t.lower() in ("string", "pchar", "pointer") or t in ("TRect",):
            gt = f"ptr:{t}"
        elif t in ("Longint", "LongInt", "TColor", "TPoint"):
            gt = f"dword:{t}"
        else:
            gt = f"word:{t}"
        return f"{p['name']}:{gt}"

    handler_sigs: dict[str, tuple[str, str]] = {}   # addr -> (class, sig)
    for form_cls, fe in events.items():
        for comp, evs in fe["events"].items():
            for ev, info in evs.items():
                if not info["addr"]:
                    continue
                mt = img.method_types.get(prop_type(info["component_class"], ev) or "")
                if not mt or mt["kind"] != "procedure":
                    continue
                owner_cls = syms.get(info["addr"], form_cls).split(".")[0]
                if owner_cls not in by_name:
                    owner_cls = form_cls
                # Pascal pushes left to right and Self last, so in Ghidra's
                # stdcall16far order: Self, then the declared params reversed
                sig = [f"Self:ptr:{owner_cls}"] + [param_sig(p) for p in reversed(mt["params"])]
                handler_sigs.setdefault(info["addr"], (owner_cls, ";".join(sig)))
    with open(os.path.join(out, "event_types.json"), "w", encoding="utf-8") as f:
        json.dump(img.method_types, f, ensure_ascii=False, indent=1)

    # class layouts for Ghidra structs: C = class, F = field (flattened with
    # inherited published fields), M = function whose Self is that class,
    # S = full signature (event handlers)
    with open(os.path.join(out, "class_layouts.csv"), "w", newline="", encoding="utf-8") as f:
        wr = csv.writer(f)
        wr.writerow(["kind", "a", "b", "c", "d"])
        for c in sorted(classes, key=lambda c: c["name"]):
            wr.writerow(["C", c["name"], c["instance_size"], c.get("parent") or "", ""])
            chain, k = [], c
            while k:
                chain.append(k)
                k = by_name.get(k["parent"]) if k.get("parent") else None
            seen = set()
            for k in reversed(chain):
                for fld in k["fields"]:
                    if fld["offset"] in seen or fld["offset"] + 4 > c["instance_size"]:
                        continue
                    seen.add(fld["offset"])
                    wr.writerow(["F", c["name"], fld["offset"], fld["name"], fld["class"] or ""])
        for a, nm in sorted(syms.items()):
            cls = nm.split(".")[0]
            if "." in nm and cls in by_name and a not in handler_sigs:
                s, o = a.split(":")
                wr.writerow(["M", s, o, cls, nm])
        for a, (cls, sig) in sorted(handler_sigs.items()):
            s, o = a.split(":")
            wr.writerow(["S", s, o, cls, sig])

    n_ev = sum(len(e) for f in events.values() for e in f["events"].values())
    n_res = sum(1 for f in events.values() for e in f["events"].values()
                for x in e.values() if x["addr"])
    print(f"classes: {len(classes)}  published methods: {sum(len(c['methods']) for c in classes)}")
    print(f"forms linked: {len(events)}  events: {n_ev}  resolved to code: {n_res}")
    print(f"symbols: {len(syms)}  -> {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
