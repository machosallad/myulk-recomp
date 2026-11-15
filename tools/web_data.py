"""Prepare the game data for the web build of the recompiled game.

Copies the folders the game reads from the user's extracted CD into the web
output folder (only files that are missing or changed) and writes
manifest.txt: one line per file, "relative/path<TAB>size". The page fetches
the manifest at start and every other file on first use.

Usage: python web_data.py <extracted/iso> <build/web/game>
"""
import os
import shutil
import sys

FOLDERS = ["MYULK", "PICTDLL", "ART", "SOUNDS", "LJUD", "MOVIES", "IMAGES"]


def main() -> int:
    src, dst = sys.argv[1], sys.argv[2]
    lines = []
    copied = 0
    for folder in FOLDERS:
        base = os.path.join(src, folder)
        for root, _dirs, files in os.walk(base):
            for name in sorted(files):
                s = os.path.join(root, name)
                rel = os.path.relpath(s, src).replace(os.sep, "/")
                d = os.path.join(dst, rel)
                st = os.stat(s)
                if not os.path.exists(d) or os.path.getsize(d) != st.st_size or \
                        int(os.path.getmtime(d)) != int(st.st_mtime):
                    os.makedirs(os.path.dirname(d), exist_ok=True)
                    shutil.copy2(s, d)
                    copied += 1
                lines.append(f"{rel}\t{st.st_size}")
    os.makedirs(dst, exist_ok=True)
    manifest = os.path.join(dst, "manifest.txt")
    text = "\n".join(lines) + "\n"
    if not os.path.exists(manifest) or open(manifest, encoding="utf-8").read() != text:
        with open(manifest, "w", encoding="utf-8", newline="\n") as fh:
            fh.write(text)
    print(f"web data: {len(lines)} files, {copied} copied")
    return 0


if __name__ == "__main__":
    sys.exit(main())
