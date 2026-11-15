"""Make a ready-to-play package of the recompiled game.

Usage:
    python tools/package.py desktop [--game DIR] [--out dist]
    python tools/package.py web     [--game DIR] [--out dist]

desktop: build/native/myulk_recomp.exe -> dist/myulk-desktop/Myulk.exe, with
         the licences and a player README.
web:     build/web/myulk_recomp.* -> dist/myulk-web/ (index.html), likewise.

Without --game a package holds no game files: on first start each player
chooses their own CD image (.iso, .bin/.cue), CD drive or folder, which is
checked and copied once (src/recomp/enhance/setup.cpp). The program still
contains code translated from MYULK.EXE, but none of the CD's files.
--game:  include the CD files (the folder holding MYULK, PICTDLL, ...), so
         the package plays at once. For the web this also writes the
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

Made from the myulk-recomp source code. The program contains code translated
from the original game{contents}. Myulk belongs to its rights holders.
Licences for the parts the program uses are in "licenses".
"""

DESKTOP_WHAT = "Start Myulk.exe. Windows 10/11, 64-bit."
DESKTOP_START = ""
DESKTOP_SAVES = ("  In %APPDATA%\\myulk-recomp\\Myulk\\drive_c\\MYULK, the game files are never\n"
                 "  changed. Settings: settings.txt in %APPDATA%\\myulk-recomp\\Myulk.")
WEB_WHAT = ("The game for the browser (Chrome or Edge; it needs WebAssembly JSPI).\n"
            "Serve this folder over http://localhost or https:// and open index.html.")
WEB_RUN = """
STARTING / STARTA
  python -m http.server 8080      then open http://localhost:8080/
  Other computers need an https:// address for the browser to keep the files.
"""
WEB_SAVES = "  In the browser (IndexedDB) for this address; they stay after reloading."
GAME_MISSING_DESKTOP = """  Not included: you need your own Myulk CD. On the first start, choose the
  CD image (.iso, or .bin/.cue), the CD drive or a folder with the CD's
  files, or drop it on the window. The files are checked and copied once;
  after that the CD is not needed.
  Ingår inte: du behöver en egen Myulk-cd. Vid första starten väljer du
  cd-avbilden (.iso eller .bin/.cue), cd-enheten eller en mapp med cd:ns
  filer, eller drar den till fönstret. Filerna kontrolleras och kopieras en
  gång; sedan behövs inte skivan.
"""
GAME_MISSING_WEB = """  Not on the server: each player needs their own Myulk CD image. On the
  first visit the page asks for it (.iso or .bin, or drop it on the page).
  It is read and kept in the player's browser; nothing is uploaded.
  Finns inte på servern: varje spelare behöver en egen avbild av Myulk-cd:n.
  Vid första besöket ber sidan om den (.iso eller .bin, eller dra den hit).
  Den läses och sparas i spelarens webbläsare; inget laddas upp.
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
        game=GAME_INCLUDED if with_game else (GAME_MISSING_WEB if web else GAME_MISSING_DESKTOP),
        start=WEB_RUN if web else DESKTOP_START,
        contents=" and the game's files from the CD" if with_game else ", but none of the game's files",
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
    if args.game:
        os.makedirs(os.path.join(dst, "game"))
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
    print("includes the game files" if args.game else "no game files: players choose their CD image on first start")
    return 0


if __name__ == "__main__":
    sys.exit(main())
