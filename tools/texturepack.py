"""Make an HD texture pack for the recompiled Myulk from your own game files.

The game keeps drawing with its original small bitmaps; a texture pack only
adds larger versions next to them, used for what ends up on screen
(src/recomp/enhance/texture_pack.cpp). Each picture is matched by a hash of its
decoded pixels, so it does not matter which DLL, form or ART file it came from.

Workflow (see docs/texture-pack.md):

    python -I tools/texturepack.py export GAME_DIR WORK_DIR
        GAME_DIR is the folder holding MYULK\\, PICTDLL\\, ART\\, IMAGES\\
        (your imported CD, or extracted/iso). Writes every picture once to
        WORK_DIR/src/<category>/<hash>.png plus WORK_DIR/index.csv.

    ... upscale WORK_DIR/src/<category> with Upscayl, chaiNNer,
        realesrgan-ncnn-vulkan, ... into any folder(s). Output file names must
        still start with the 16-digit hash (Upscayl's suffixes are fine).

    python -I tools/texturepack.py build WORK_DIR PACK_DIR --scale 3 UPSCALED_DIR [...]
        Resizes every upscaled picture to exactly scale x the original size
        and writes PACK_DIR/pack.json + PACK_DIR/hd/<hash>.png.

    python -I tools/texturepack.py build WORK_DIR PACK_DIR --scale 3 --fake
        A test pack: plain pixel-doubled pictures with a green tint, to see
        what gets replaced in the game.

    python -I tools/texturepack.py hash FILE.bmp [...]
        Print the hash the game computes for a picture.

The pack is made from copyrighted game art: keep it for yourself, like the
game files (never commit it).
"""
from __future__ import annotations

import argparse
import csv
import glob
import io
import json
import os
import re
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dfm  # noqa: E402
from extract_resources import RT_BITMAP, RT_RCDATA, dib_to_bmp  # noqa: E402
from ne import NEFile  # noqa: E402

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    sys.exit("Pillow is needed: python -m pip install Pillow")

PACK_VERSION = 1
HASH_RE = re.compile(r"^([0-9a-fA-F]{16})")


# ----------------------------------------------------------------------
# The hash: over width and height (uint32 little-endian each), then every
# pixel top-down, left to right, as R, G, B bytes: CRC-32 in the high 32 bits,
# Adler-32 in the low. The game computes the same over the decoded bitmap
# (texture_pack.cpp: pixel_hash).
def pixel_hash(img: Image.Image) -> str:
    rgb = img.convert("RGB")
    data = struct.pack("<II", rgb.width, rgb.height) + rgb.tobytes()
    return f"{zlib.crc32(data):08x}{zlib.adler32(data):08x}"


def open_bmp(data: bytes) -> Image.Image | None:
    try:
        img = Image.open(io.BytesIO(data))
        img.load()
    except Exception:
        return None
    if img.mode == "1":          # monochrome masks are drawn as they are
        return None
    return img


# ----------------------------------------------------------------------
def find_ci(root: str, *parts: str) -> str | None:
    """Join path parts case-insensitively (CD folders vary in case)."""
    cur = root
    for p in parts:
        if not os.path.isdir(cur):
            return None
        hit = next((e for e in os.listdir(cur) if e.lower() == p.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def form_bitmaps(comp: dict):
    def walk(props: dict):
        for k, v in props.items():
            if isinstance(v, dict) and "binary" in v:
                ext, payload = dfm.split_graphic(k, v["binary"])
                if ext == "bmp":
                    yield payload
            elif isinstance(v, dict) and "collection" in v:
                for it in v["collection"]:
                    yield from walk(it["props"])
    yield from walk(comp["props"])
    for c in comp["children"]:
        yield from form_bitmaps(c)


def iter_pictures(game: str):
    """Yield (category, origin, bmp bytes) for every picture in the game."""
    modules = []
    exe = find_ci(game, "MYULK", "MYULK.EXE")
    if exe:
        modules.append(exe)
    pict = find_ci(game, "PICTDLL")
    if pict:
        modules += sorted(glob.glob(os.path.join(pict, "*.[dD][lL][lL]")))
    for path in modules:
        mod = os.path.splitext(os.path.basename(path))[0].upper()
        for r in NEFile(path).resources():
            if r.type_id == RT_BITMAP:
                cat = "ui" if mod == "MYULK" else "scenes"
                yield cat, f"{mod}/bitmap/{r.res_id}", dib_to_bmp(r.data)
            elif r.type_id == RT_RCDATA and r.data[:4] == b"TPF0":
                try:
                    tree = dfm.parse(r.data)
                except Exception:
                    continue
                for i, bmp in enumerate(form_bitmaps(tree)):
                    yield "ui", f"{mod}/form/{r.res_id}/{i}", bmp
    for folder, cat in (("ART", "art"), ("IMAGES", "scenes")):
        base = find_ci(game, folder)
        if not base:
            continue
        for dirpath, _, files in os.walk(base):
            for f in sorted(files):
                if f.lower().endswith(".bmp"):
                    p = os.path.join(dirpath, f)
                    with open(p, "rb") as fh:
                        yield cat, os.path.relpath(p, game).replace("\\", "/"), fh.read()


def cmd_export(args) -> int:
    game, work = args.game_dir, args.work_dir
    if not find_ci(game, "MYULK", "MYULK.EXE"):
        sys.exit(f"{game}: MYULK\\MYULK.EXE not found (give the folder holding MYULK, PICTDLL, ART)")
    seen: dict[str, dict] = {}
    skipped = 0
    for cat, origin, data in iter_pictures(game):
        img = open_bmp(data)
        if img is None:
            skipped += 1
            continue
        h = pixel_hash(img)
        if h in seen:
            seen[h]["origins"].append(origin)
            continue
        seen[h] = {"category": cat, "w": img.width, "h": img.height, "origins": [origin]}
        out = os.path.join(work, "src", cat)
        os.makedirs(out, exist_ok=True)
        img.convert("RGB").save(os.path.join(out, h + ".png"))
    with open(os.path.join(work, "index.csv"), "w", newline="", encoding="utf-8") as f:
        w = csv.writer(f)
        w.writerow(["hash", "category", "width", "height", "origins"])
        for h, e in sorted(seen.items(), key=lambda kv: (kv[1]["category"], kv[1]["origins"][0])):
            w.writerow([h, e["category"], e["w"], e["h"], " ".join(e["origins"])])
    counts: dict[str, int] = {}
    for e in seen.values():
        counts[e["category"]] = counts.get(e["category"], 0) + 1
    print(f"{len(seen)} unique pictures -> {os.path.join(work, 'src')} {counts}"
          f" ({skipped} monochrome/unreadable skipped)")
    return 0


# ----------------------------------------------------------------------
def read_index(work: str) -> dict[str, dict]:
    path = os.path.join(work, "index.csv")
    if not os.path.exists(path):
        sys.exit(f"{path} not found: run 'export' first")
    with open(path, encoding="utf-8") as f:
        return {r["hash"]: r for r in csv.DictReader(f)}


def tint(img: Image.Image) -> Image.Image:
    green = Image.new("RGB", img.size, (0, 255, 0))
    return Image.blend(img, green, 0.25)


def cmd_build(args) -> int:
    index = read_index(args.work_dir)
    k = args.scale
    hd = os.path.join(args.pack_dir, "hd")
    os.makedirs(hd, exist_ok=True)
    sources: dict[str, str] = {}
    if args.fake:
        for h, e in index.items():
            sources[h] = os.path.join(args.work_dir, "src", e["category"], h + ".png")
    else:
        if not args.upscaled:
            sys.exit("give the folder(s) with upscaled pictures, or --fake")
        for d in args.upscaled:
            for dirpath, _, files in os.walk(d):
                for f in files:
                    m = HASH_RE.match(f)
                    if m and f.lower().endswith((".png", ".jpg", ".jpeg", ".webp", ".bmp", ".tif", ".tiff")):
                        sources[m.group(1).lower()] = os.path.join(dirpath, f)
    written = unknown = 0
    for h, src in sorted(sources.items()):
        e = index.get(h)
        if e is None:
            unknown += 1
            continue
        size = (int(e["width"]) * k, int(e["height"]) * k)
        img = Image.open(src).convert("RGB")
        if args.fake:
            img = tint(img.resize(size, Image.NEAREST))
        elif img.size != size:
            img = img.resize(size, Image.LANCZOS)
        img.save(os.path.join(hd, h + ".png"), compress_level=args.compress)
        written += 1
    with open(os.path.join(args.pack_dir, "pack.json"), "w", encoding="utf-8") as f:
        json.dump({"version": PACK_VERSION, "scale": k, "name": args.name}, f, indent=1)
    print(f"{written} of {len(index)} pictures -> {hd} (scale {k})"
          + (f", {unknown} files with unknown hashes ignored" if unknown else ""))
    return 0


def cmd_hash(args) -> int:
    for p in args.files:
        with open(p, "rb") as f:
            img = open_bmp(f.read())
        print(f"{pixel_hash(img) if img else '-' * 16}  {img.width if img else 0}x{img.height if img else 0}  {p}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    e = sub.add_parser("export", help="write every game picture once, named by hash")
    e.add_argument("game_dir")
    e.add_argument("work_dir")
    e.set_defaults(func=cmd_export)
    b = sub.add_parser("build", help="turn upscaled pictures into a texture pack")
    b.add_argument("work_dir")
    b.add_argument("pack_dir")
    b.add_argument("upscaled", nargs="*")
    b.add_argument("--scale", type=int, default=3, choices=(2, 3, 4))
    b.add_argument("--fake", action="store_true", help="test pack: pixel-doubled, green tint")
    b.add_argument("--name", default="HD")
    b.add_argument("--compress", type=int, default=6, help="PNG compression 0-9")
    b.set_defaults(func=cmd_build)
    hsh = sub.add_parser("hash", help="print the game's hash of picture files")
    hsh.add_argument("files", nargs="+")
    hsh.set_defaults(func=cmd_hash)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
