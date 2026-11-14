"""Extract resources from Myulk's 16-bit NE executables and DLLs.

Usage:
    python tools/extract_resources.py [ISO_ROOT] [OUT_DIR]

Defaults: ISO_ROOT = extracted/iso, OUT_DIR = extracted/resources

For each module it writes:
    bitmaps/<id>.bmp           RT_BITMAP as standalone .bmp files
    cursors/<id>.cur           RT_GROUP_CURSOR rebuilt as .cur files
    icons/<id>.ico             RT_GROUP_ICON rebuilt as .ico files
    strings.json               RT_STRING tables {id: text}
    forms/<NAME>.dfm.txt       Delphi forms in readable text form
    forms/<NAME>.json          Delphi forms as a JSON component tree
    forms/<NAME>/...           graphics embedded in the forms
    other/<type>_<id>.bin      anything else
    module.json                header, segments, imports, resource index
"""
from __future__ import annotations

import glob
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dfm  # noqa: E402
from ne import NEFile  # noqa: E402

RT_CURSOR, RT_BITMAP, RT_ICON, RT_STRING, RT_RCDATA = 1, 2, 3, 6, 10
RT_GROUP_CURSOR, RT_GROUP_ICON = 12, 14


def safe(name) -> str:
    s = str(name)
    return "".join(c if c.isalnum() or c in "-_." else "_" for c in s)


# ----------------------------------------------------------------------
def dib_to_bmp(dib: bytes) -> bytes:
    """Prepend a BITMAPFILEHEADER to a packed DIB, trimming padding."""
    hsize = struct.unpack_from("<I", dib, 0)[0]
    if hsize == 12:  # BITMAPCOREHEADER
        w, h, planes, bpp = struct.unpack_from("<HHHH", dib, 4)
        ncol = (1 << bpp) if bpp <= 8 else 0
        pal = ncol * 3
        comp, size_img = 0, 0
    else:
        w, h, planes, bpp, comp, size_img, _, _, clr_used = struct.unpack_from(
            "<iiHHIIiiI", dib, 4)
        ncol = clr_used or ((1 << bpp) if bpp <= 8 else 0)
        pal = ncol * 4 + (12 if comp == 3 and hsize == 40 else 0)
    stride = ((abs(w) * bpp + 31) // 32) * 4
    img = size_img if comp not in (0, 3) and size_img else stride * abs(h)
    total = min(len(dib), hsize + pal + img)
    off = 14 + hsize + pal
    return b"BM" + struct.pack("<IHHI", 14 + total, 0, 0, off) + dib[:total]


def build_group(group: bytes, members: dict, is_cursor: bool) -> bytes:
    _, typ, count = struct.unpack_from("<HHH", group, 0)
    entries, blobs = [], []
    for i in range(count):
        e = group[6 + 14 * i:6 + 14 * (i + 1)]
        if is_cursor:
            w, h2, planes, bpp, size, nid = struct.unpack("<HHHHIH", e)
            raw = members.get(nid, b"")
            hx, hy = struct.unpack_from("<HH", raw, 0) if len(raw) >= 4 else (0, 0)
            body = raw[4:]
            entries.append((w & 0xFF, (h2 // 2) & 0xFF, 0, 0, hx, hy, body))
        else:
            bw, bh, cc, _, planes, bpp, size, nid = struct.unpack("<BBBBHHIH", e)
            body = members.get(nid, b"")[:size]
            entries.append((bw, bh, cc, 0, planes, bpp, body))
        blobs.append(entries[-1][-1])
    out = struct.pack("<HHH", 0, 2 if is_cursor else 1, count)
    off = 6 + 16 * count
    for e in entries:
        out += struct.pack("<BBBBHHII", *e[:6], len(e[6]), off)
        off += len(e[6])
    return out + b"".join(blobs)


def parse_strings(block_id: int, data: bytes) -> dict:
    out, p = {}, 0
    for i in range(16):
        if p >= len(data):
            break
        n = data[p]
        if n:
            out[(block_id - 1) * 16 + i] = data[p + 1:p + 1 + n].decode(dfm.ENCODING)
        p += 1 + n
    return out


# ----------------------------------------------------------------------
def extract_form_graphics(comp: dict, form_dir: str, rel_base: str, path: list) -> int:
    """Replace binary props with file refs, writing graphics to disk."""
    count = 0
    here = path + [comp["name"] or comp["class"]]

    def walk(props: dict, prefix: str):
        nonlocal count
        for k, v in list(props.items()):
            if isinstance(v, dict) and "binary" in v:
                ext, payload = dfm.split_graphic(k, v["binary"])
                fname = safe("_".join(here[1:] + [prefix + k]) or k) + "." + ext
                os.makedirs(form_dir, exist_ok=True)
                with open(os.path.join(form_dir, fname), "wb") as f:
                    f.write(payload)
                props[k] = {"file": f"{rel_base}/{fname}", "size": len(payload),
                            "kind": ext}
                count += 1
            elif isinstance(v, dict) and "collection" in v:
                for i, it in enumerate(v["collection"]):
                    walk(it["props"], f"{prefix}{k}{i}_")

    walk(comp["props"], "")
    for c in comp["children"]:
        count += extract_form_graphics(c, form_dir, rel_base, here)
    return count


# ----------------------------------------------------------------------
def extract_module(path: str, out_root: str) -> dict:
    ne = NEFile(path)
    mod = os.path.splitext(os.path.basename(path))[0].upper()
    out = os.path.join(out_root, mod)
    os.makedirs(out, exist_ok=True)
    res = ne.resources()
    by_type: dict = {}
    for r in res:
        by_type.setdefault(r.type_id, {})[r.res_id] = r

    stats = {"module": mod, "forms": 0, "bitmaps": 0, "form_graphics": 0,
             "strings": 0, "cursors": 0, "icons": 0, "other": 0, "errors": []}

    for rid, r in by_type.get(RT_BITMAP, {}).items():
        os.makedirs(os.path.join(out, "bitmaps"), exist_ok=True)
        with open(os.path.join(out, "bitmaps", safe(rid) + ".bmp"), "wb") as f:
            f.write(dib_to_bmp(r.data))
        stats["bitmaps"] += 1

    for kind, grp_t, mem_t, ext in (("cursors", RT_GROUP_CURSOR, RT_CURSOR, "cur"),
                                    ("icons", RT_GROUP_ICON, RT_ICON, "ico")):
        members = {k: v.data for k, v in by_type.get(mem_t, {}).items()}
        for rid, r in by_type.get(grp_t, {}).items():
            os.makedirs(os.path.join(out, kind), exist_ok=True)
            with open(os.path.join(out, kind, f"{safe(rid)}.{ext}"), "wb") as f:
                f.write(build_group(r.data, members, kind == "cursors"))
            stats[kind] += 1

    strings: dict = {}
    for rid, r in by_type.get(RT_STRING, {}).items():
        if isinstance(rid, int):
            strings.update(parse_strings(rid, r.data))
    if strings:
        with open(os.path.join(out, "strings.json"), "w", encoding="utf-8") as f:
            json.dump(dict(sorted(strings.items())), f, ensure_ascii=False, indent=1)
        stats["strings"] = len(strings)

    for rid, r in by_type.get(RT_RCDATA, {}).items():
        if r.data[:4] == b"TPF0":
            fdir = os.path.join(out, "forms")
            os.makedirs(fdir, exist_ok=True)
            try:
                tree = dfm.parse(r.data)
            except Exception as e:  # keep going, record the failure
                stats["errors"].append(f"form {rid}: {e}")
                with open(os.path.join(fdir, safe(rid) + ".dfm.bin"), "wb") as f:
                    f.write(r.data)
                continue
            name = safe(rid)
            stats["form_graphics"] += extract_form_graphics(
                tree, os.path.join(fdir, name), name, [])
            with open(os.path.join(fdir, name + ".json"), "w", encoding="utf-8") as f:
                json.dump(tree, f, ensure_ascii=False, indent=1)
            with open(os.path.join(fdir, name + ".dfm.txt"), "w", encoding="utf-8") as f:
                f.write(dfm.to_text(tree) + "\n")
            stats["forms"] += 1
        else:
            os.makedirs(os.path.join(out, "other"), exist_ok=True)
            with open(os.path.join(out, "other", f"RCDATA_{safe(rid)}.bin"), "wb") as f:
                f.write(r.data)
            stats["other"] += 1

    handled = {RT_BITMAP, RT_CURSOR, RT_ICON, RT_STRING, RT_RCDATA,
               RT_GROUP_CURSOR, RT_GROUP_ICON}
    for t, items in by_type.items():
        if t in handled:
            continue
        for rid, r in items.items():
            os.makedirs(os.path.join(out, "other"), exist_ok=True)
            with open(os.path.join(out, "other", f"{safe(r.type_name)}_{safe(rid)}.bin"),
                      "wb") as f:
                f.write(r.data)
            stats["other"] += 1

    info = {
        "file": os.path.basename(path),
        "module_name": ne.module_name(),
        "linker": f"{ne.linker_major}.{ne.linker_minor}",
        "flags": hex(ne.flags),
        "entry_cs_ip": {"cs": ne.cs_ip[1], "ip": hex(ne.cs_ip[0])},
        "auto_data_segment": ne.auto_data_seg,
        "imports": ne.imported_modules(),
        "segments": [{"n": s.index, "file_offset": hex(s.offset), "length": s.length,
                      "data": s.is_data, "flags": hex(s.flags)} for s in ne.segments()],
        "resources": [{"type": r.type_name, "id": r.res_id, "size": r.length}
                      for r in res],
        "stats": stats,
    }
    with open(os.path.join(out, "module.json"), "w", encoding="utf-8") as f:
        json.dump(info, f, ensure_ascii=False, indent=1)
    return stats


def main() -> int:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    iso = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "extracted", "iso")
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(here, "extracted", "resources")
    targets = [os.path.join(iso, "MYULK", "MYULK.EXE"), os.path.join(iso, "INSTALL.EXE")]
    targets += sorted(glob.glob(os.path.join(iso, "PICTDLL", "*.DLL")))
    total: dict = {}
    for t in targets:
        s = extract_module(t, out)
        for k, v in s.items():
            if isinstance(v, int):
                total[k] = total.get(k, 0) + v
        if s["errors"] or s["forms"] or s["strings"]:
            print(f"{s['module']}: {s}")
    print(f"{len(targets)} modules -> {out}")
    print("totals:", total)
    return 0


if __name__ == "__main__":
    sys.exit(main())
