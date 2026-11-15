# Contributing

Thanks for helping bring Myulk back. Bug reports, fixes, testing on other systems and documentation are all welcome.

## The one firm rule

**Nothing from the game goes into the repository:** no files from the CD (pictures, films, sounds, `MYULK.EXE`, the help file), no screenshots that show the game's art, no text from the game (dialogue, help text), no code generated from `MYULK.EXE` (`build/*/gen/`), and no decompiler output (`decomp/`, `ghidra_project/`). `.gitignore` covers the usual folders; please check your commits anyway. Screenshots of the project's own screens (settings, the first-start screen) are fine.

## Building

See [docs/playing.md](docs/playing.md) for the quick way and [docs/building.md](docs/building.md) for the details. In short, with your CD's files in `extracted/iso`:

```bat
tools\build-native.bat
build\native\myulk_recomp.exe
```

Without the CD files, CMake builds everything except the recompiled game (the earlier hand-written port and the unit tests); that is also what CI checks.

## Where things are

- `tools/recomp/recomp.py`: the static recompiler ([docs/recompiler.md](docs/recompiler.md) explains it).
- `src/recomp/runtime/`: the CPU model and loading the program.
- `src/recomp/win16/`: the Windows 3.1 layer on SDL3. Most game bugs end up here: a Windows function that behaves differently from the real one.
- `src/recomp/enhance/`: the modern extras (settings, save screen, handbook, helpers, first-start import).
- `analysis/`: facts about the original program (classes, routines, imports); the recompiler reads some of them.

## Testing your change

- `ctest --preset native` runs the unit tests.
- The game can be driven without a mouse: `MYULK_CLICKS`, `MYULK_KEYS`, `MYULK_ROUTE` and friends script a run, and `MYULK_SHOT` takes screenshots at given times. [docs/building.md](docs/building.md#automation-and-testing) lists them. Set `MYULK_SAVE_DIR` and `MYULK_SETTINGS` to a temporary folder so tests don't touch your own saves.
- If you change the Windows layer, play the scenes it affects. A save file (`.MLK`) just before a problem makes it quick to get back there.

## Style

- C++20, the style of the surrounding code: 4 spaces, `snake_case` functions, comments that say why rather than what.
- Behaviour should match Windows 3.1 as the game expects it; when in doubt, describe what the original does in a comment.
- Every gameplay helper is optional and can be switched off (settings, *Spelhjälp*). New ones should be too, and must not change the game's saves.
- Commit messages: a short summary line, then what changed and why.

## Reporting a bug

Open an issue with what you did, what happened, what you expected, and whether it was the desktop or the browser version (and which browser). Attach a save file from just before, and the log if you have one (stderr of `myulk_recomp.exe`, or the browser console).
