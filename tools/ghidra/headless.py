"""Run Ghidra's headless analyzer against the Myulk project.

Usage:
    python tools/ghidra/headless.py import          # fresh import + analysis + symbols
    python tools/ghidra/headless.py script X.java [args...]   # run a script on the existing program

Environment:
    GHIDRA_HOME   Ghidra install folder (default C:\\Tools\\ghidra)

Ghidra's settings folder is redirected into ghidra_project/settings: with
the default location under %APPDATA% the OSGi framework fails to start
("The data file must be inside the data dir").
"""
from __future__ import annotations

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
GHIDRA = os.environ.get("GHIDRA_HOME", r"C:\Tools\ghidra")
PROJ_DIR = os.path.join(ROOT, "ghidra_project")
PROJ = "Myulk"
EXE = os.path.join(ROOT, "extracted", "iso", "MYULK", "MYULK.EXE")
SCRIPTS = os.path.join(ROOT, "tools", "ghidra")
LOG = os.path.join(PROJ_DIR, "headless.log")


def run(args: list[str]) -> int:
    os.makedirs(os.path.join(PROJ_DIR, "settings"), exist_ok=True)
    env = dict(os.environ)
    env["GHIDRA_HEADLESS_JAVA_OPTIONS"] = (
        f"-Dapplication.settingsdir={os.path.join(PROJ_DIR, 'settings')} -Dmyulk.root={ROOT}")
    exe = os.path.join(GHIDRA, "support",
                       "analyzeHeadless.bat" if os.name == "nt" else "analyzeHeadless")
    cmd = [exe, PROJ_DIR, PROJ] + args + ["-scriptPath", SCRIPTS, "-log", LOG]
    with open(os.path.join(PROJ_DIR, "headless.out"), "w", encoding="utf-8", errors="replace") as out:
        p = subprocess.run(cmd, env=env, stdin=subprocess.DEVNULL,
                           stdout=out, stderr=subprocess.STDOUT)
    with open(os.path.join(PROJ_DIR, "headless.out"), encoding="utf-8", errors="replace") as f:
        for line in f:
            if any(k in line for k in ("ERROR", "SCRIPT", ".java>", "REPORT")):
                print(line.rstrip())
    return p.returncode


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    if sys.argv[1] == "import":
        return run(["-import", EXE, "-overwrite",
                    "-preScript", "ApplySymbols.java",
                    "-postScript", "ApplySymbols.java",
                    "-postScript", "ApplyClassTypes.java"])
    if sys.argv[1] == "script":
        return run(["-process", "MYULK.EXE", "-noanalysis",
                    "-postScript"] + sys.argv[2:])
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main())
