# Third-party components

Fetched by CMake at build time (pinned by URL and SHA-256 in `CMakeLists.txt`); nothing here is stored in the repository.

| Component | Use | License |
|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) | window, input, audio, file dialogs | zlib |
| [nlohmann/json](https://github.com/nlohmann/json) | the hand-written port's data files | MIT |
| [stb_truetype](https://github.com/nothings/stb) | rendering TrueType text in the Windows layer | public domain / MIT |
| [Liberation fonts 2.1.5](https://github.com/liberationfonts/liberation-fonts) | the game's text (embedded into `myulk_recomp`) | SIL Open Font License 1.1 |

Used while building, not part of the program:

| Tool | Use | License |
|---|---|---|
| [Capstone](https://www.capstone-engine.org/) (Python package) | disassembling `MYULK.EXE` in the recompiler | BSD-3-Clause |
| [Emscripten](https://emscripten.org/) | the browser build | MIT / University of Illinois NCSA |
| [Ghidra](https://ghidra-sre.org/) | studying the original program (not needed to build) | Apache 2.0 |

`tools/package.py` puts the licences of the components linked into the program (SDL 3, stb, Liberation fonts) into each package's `licenses` folder.

## Fonts

The recompiled game draws text with the Liberation fonts, which have the same character widths as the Windows fonts the game was laid out with:

| Game font | Drawn with |
|---|---|
| System, MS Sans Serif, Arial | Liberation Sans (System in bold, as on Windows) |
| Times New Roman, MS Serif | Liberation Serif |
| Courier, Courier New | Liberation Mono |
| Wingdings | the two symbols the game uses (empty box, ticked box) are drawn as shapes |

Builds that are passed on must include the SIL Open Font License text (`OFL.txt`, or `LICENSE` in the font archive).

## Help file format

The handbook reader (`src/recomp/enhance/helpfile.cpp`) is written from the description of the Windows 3.1 help file format in helpdeco's `helpfile.txt` (Manfred Winterhoff, based on Pete Davis's articles in Dr. Dobb's Journal, 1993). No helpdeco code is used. The help file itself (`MYULK.HLP`) is part of the game's data and is read from your CD at run time.
