"""Make a ready-to-play package of the recompiled game, for your own use.

The program contains code translated from MYULK.EXE, so a package is a
personal build: it may not be shared, and neither may the game files. What
can be shared is this repository's source, which anyone can build with their
own copy of the CD.

Usage:
    python tools/package.py desktop [--game DIR] [--out dist]
    python tools/package.py web     [--game DIR] [--out dist]

desktop: build/native/myulk_recomp.exe -> dist/myulk-desktop/Myulk.exe, with
         the licences and a "game" folder for the CD files.
web:     build/web/myulk_recomp.* -> dist/myulk-web/ (index.html), with the
         licences and an empty "game" folder.
--game:  copy the CD files (the folder holding MYULK, PICTDLL, ...) in as well,
         so the package is complete. For the web this also writes the
         manifest the page needs (tools/web_data.py).

Build first: tools\\build-native.bat (desktop) or cmake --build --preset web
--target myulk_recomp (web). Each package is also zipped.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME_FOLDERS = ["MYULK", "PICTDLL", "ART", "SOUNDS", "LJUD", "MOVIES", "IMAGES"]

PLAYER_README = """Myulk – Kampen om det öppna landskapet (recompiled)
===================================================

{what}

THE GAME FILES / SPELFILERNA
{game}{start}
KEYS / TANGENTER
  F1 handbook (handbok)        F2 settings (inställningar)
  F5 / F9 quick-save / load    F7 / F8 picture / text style
  F11 fullscreen               F12 screenshot
  H  show what can be clicked  P  pause the farm
  Arrow keys / WASD, right-click and the mouse wheel move around.
  Every helper can be switched off in F2 > Spelhjälp (play as in 1996).

SAVES / SPARADE SPEL
{saves}

This is a personal build made from the myulk-recomp source code. It contains
code translated from the original game and must not be shared, and neither
may the game files. Licences for the parts it uses are in "licenses".
"""

DESKTOP_WHAT = "Start Myulk.exe. Windows 10/11, 64-bit."
DESKTOP_START = ""
DESKTOP_SAVES = ("  In %APPDATA%\\myulk-recomp\\Myulk\\drive_c\\MYULK, the game files are never\n"
                 "  changed. Settings: settings.txt in %APPDATA%\\myulk-recomp\\Myulk.")
WEB_WHAT = ("The game for the browser (Chrome or Edge; it needs WebAssembly JSPI).\n"
            "Serve this folder over http://localhost or https:// and open index.html.")
WEB_MANIFEST = """
  The page also needs game/manifest.txt. Make it with the tool in the
  source code:  python tools/web_data.py <your CD folder> <this folder>/game
  (python tools/package.py web --game <your CD folder> does all of it.)
"""
WEB_RUN = """
STARTING / STARTA
  python -m http.server 8080      then open http://localhost:8080/
  Only on your own computer or network: the game files must not be put on
  a public web site.
"""
WEB_SAVES = "  In the browser (IndexedDB) for this address; they stay after reloading."
GAME_MISSING = """  The game itself is not included. Copy the contents of your Myulk CD into
  the folder "game" here, so that game{sep}MYULK{sep}MYULK.EXE exists.
  Spelet ingår inte. Kopiera innehållet på din Myulk-cd till mappen "game",
  så att game{sep}MYULK{sep}MYULK.EXE finns.
"""
GAME_INCLUDED = """  The files from the Myulk CD are in the folder "game"; nothing to install.
  Filerna från Myulk-cd:n finns i mappen "game"; inget behöver installeras.
"""


def copy_licenses(build: str, dst: str) -> None:
    lic = os.path.join(dst, "licenses")
    os.makedirs(lic, exist_ok=True)
    deps = os.path.join(build, "_deps")
    for src, name in [
        (os.path.join(ROOT, "LICENSE"), "myulk-recomp.txt"),
        (os.path.join(deps, "sdl3-src", "LICENSE.txt"), "SDL3 (zlib).txt"),
        (os.path.join(deps, "stb-src", "LICENSE"), "stb (MIT or public domain).txt"),
        (os.path.join(deps, "liberation_fonts-src", "LICENSE"), "Liberation fonts (SIL OFL 1.1).txt"),
    ]:
        if os.path.exists(src):
            shutil.copy(src, os.path.join(lic, name))
        else:
            print(f"  note: {src} not found, not included")


def copy_game(src: str, dst: str) -> None:
    if not os.path.exists(os.path.join(src, "MYULK", "MYULK.EXE")):
        sys.exit(f"{src} does not look like the Myulk CD (no MYULK/MYULK.EXE)")
    for folder in GAME_FOLDERS:
        s = os.path.join(src, folder)
        if os.path.isdir(s):
            shutil.copytree(s, os.path.join(dst, folder), dirs_exist_ok=True)


def write_readme(dst: str, web: bool, with_game: bool) -> None:
    sep = "/" if web else "\\"
    text = PLAYER_README.format(
        what=WEB_WHAT if web else DESKTOP_WHAT,
        game=GAME_INCLUDED if with_game else GAME_MISSING.format(sep=sep),
        start=(("" if with_game else WEB_MANIFEST) + WEB_RUN) if web else DESKTOP_START,
        saves=WEB_SAVES if web else DESKTOP_SAVES,
        sep=sep)
    with open(os.path.join(dst, "README.txt"), "w", encoding="utf-8-sig", newline="\r\n") as fh:
        fh.write(text)


def fresh(path: str) -> None:
    if os.path.exists(path):
        shutil.rmtree(path)
    os.makedirs(path)


def desktop(args) -> str:
    build = os.path.join(ROOT, "build", "native")
    exe = os.path.join(build, "myulk_recomp.exe" if os.name == "nt" else "myulk_recomp")
    if not os.path.exists(exe):
        sys.exit(f"{exe} not found: build first (tools\\build-native.bat)")
    dst = os.path.join(args.out, "myulk-desktop")
    fresh(dst)
    shutil.copy(exe, os.path.join(dst, "Myulk.exe" if os.name == "nt" else "myulk"))
    os.makedirs(os.path.join(dst, "game"))
    if args.game:
        copy_game(args.game, os.path.join(dst, "game"))
    copy_licenses(build, dst)
    write_readme(dst, web=False, with_game=bool(args.game))
    return dst


def web(args) -> str:
    build = os.path.join(ROOT, "build", "web")
    for f in ("myulk_recomp.html", "myulk_recomp.js", "myulk_recomp.wasm"):
        if not os.path.exists(os.path.join(build, f)):
            sys.exit(f"{f} not found in build/web: build first (cmake --build --preset web --target myulk_recomp)")
    dst = os.path.join(args.out, "myulk-web")
    fresh(dst)
    shutil.copy(os.path.join(build, "myulk_recomp.html"), os.path.join(dst, "index.html"))
    for f in ("myulk_recomp.js", "myulk_recomp.wasm"):
        shutil.copy(os.path.join(build, f), os.path.join(dst, f))
    os.makedirs(os.path.join(dst, "game"))
    if args.game:
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "web_data.py"), args.game,
                        os.path.join(dst, "game")], check=True)
    copy_licenses(build, dst)
    write_readme(dst, web=True, with_game=bool(args.game))
    return dst


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("target", choices=["desktop", "web"])
    ap.add_argument("--game", help="the CD's files (folder with MYULK, PICTDLL, ...) to include")
    ap.add_argument("--out", default=os.path.join(ROOT, "dist"))
    ap.add_argument("--no-zip", action="store_true")
    args = ap.parse_args()
    dst = desktop(args) if args.target == "desktop" else web(args)
    if not args.no_zip:
        archive = shutil.make_archive(dst, "zip", os.path.dirname(dst), os.path.basename(dst))
        print(f"zipped: {archive}")
    print(f"package: {dst}")
    if args.game:
        print("This package contains the game files: it is for your own use only.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
