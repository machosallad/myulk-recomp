# myulk-recomp

A modern reimplementation of *Myulk – Kampen om det öppna landskapet* (1996), a Swedish farm management game originally built as a 16-bit Windows 3.1 Delphi application.

Goal: run on modern desktops and in the browser (C++20 + SDL, planned), using the original game data from your own copy of the CD.

**This repository contains no original game data.** Put your ISO at `iso/Myulk.iso`. Both `iso/` and `extracted/` are gitignored.

- [docs/extraction.md](docs/extraction.md) – how assets are extracted and what the original game is made of
- [docs/code-map.md](docs/code-map.md) – recovered class structure, event handlers and program layout of the original EXE
