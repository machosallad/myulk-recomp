# Asset extraction

Myulk – Kampen om det öppna landskapet (1996), Swedish farm management game.

## How to run

Needs Python 3.10+, Pillow (only for verification) and 7-Zip.

```bash
"/c/Program Files/7-Zip/7z.exe" x -y -oextracted/iso iso/Myulk.iso
python -I tools/extract_resources.py
```

Everything lands in `extracted/` (gitignored — original game data is copyrighted and must not be committed).

## What the original game is

| Thing | Finding |
|---|---|
| Executable | `MYULK/MYULK.EXE`, 3.4 MB, 16-bit Windows 3.1 NE, 34 segments |
| Compiler | Borland Delphi 1 (1995) |
| Windows APIs used | KERNEL, USER, GDI, MMSYSTEM, KEYBOARD, TOOLHELP, WIN87EM — nothing exotic |
| Resolution | Fixed 640×480, black maximized borderless window; main scene view 450×250 |
| UI | 57 Delphi forms (screens/dialogs) |
| Video | 52 AVI, all Cinepak (`cvid`), 24-bit, 8-bit mono PCM audio, ~11.5 min total |
| Audio | 361 WAV, all 8-bit mono PCM, 11025 or 22050 Hz |
| Language | Swedish (cp1252) |

Runtime folders the EXE loads from (paths are relative to `MYULK\`, e.g. `\..\pictdll\`):
`ART`, `IMAGES`, `LJUD`, `MOVIES`, `PICTDLL`, `SOUNDS`. `VFW` is only the Video for Windows 1.1 installer and is not needed.

## Output layout

```
extracted/
  iso/                       raw ISO contents
  resources/<MODULE>/        one folder per NE file (MYULK, INSTALL, NB*)
    module.json              header, segment table, imports, resource index
    bitmaps/<id>.bmp         bitmap resources (scene frames live in PICTDLL\NB*.DLL)
    cursors/<id>.cur         custom mouse cursors
    icons/<id>.ico
    strings.json             string tables (mostly Delphi VCL messages)
    forms/<NAME>.dfm.txt     form, readable Delphi text format
    forms/<NAME>.json        form, JSON component tree (for code generation)
    forms/<NAME>/*.bmp       images embedded in that form
```

Counts: 58 forms (57 game + 1 installer), 1,425 bitmap resources, 282 images embedded in forms, 23 cursors, 517 strings. All 2,143 bitmaps (resources + `ART\`) were verified to open with Pillow. The images are 8-bit palettized except 35 truecolor ones.

## Form JSON format

```json
{"class": "TImage", "name": "Image", "props": {...}, "children": [...]}
```

Property values: plain int/string/bool/null, or a tagged object:
`{"ident": "clBlack"}`, `{"set": ["fsBold"]}`, `{"float": 1.5}`, `{"list": [...]}`,
`{"collection": [{"order": n, "props": {...}}]}`, or for extracted binary data
`{"file": "TMAIN/xyz.bmp", "size": n, "kind": "bmp"}` (path relative to `forms/`).

Event properties (`OnClick = ButtonClick` etc.) name the Delphi methods that hold the game logic. They are the entry points for the decompilation.

## Tools

- `tools/ne.py` – NE executable parser (header, segments, imports, resources)
- `tools/dfm.py` – Delphi binary form (TPF0) decoder plus text renderer
- `tools/extract_resources.py` – drives the above over every EXE/DLL

## How the recompiled game uses the files

The recompiled game doesn't need this extraction: it reads the CD's files as they are, like the original did. This page is for studying the game and for the earlier hand-written port.

- Films are decoded at run time (`src/recomp/win16/avi.cpp`, a Cinepak decoder), so nothing is converted.
- 8-bit PCM WAV loads directly with SDL.
- The EXE builds some file names at run time (`GA%d%d.bmp`, `Gabt%d.bmp`), which is one reason to run the original code rather than list the assets.
- `MYULK.HLP` (WinHelp) is the in-game handbook; the handbook viewer reads it directly (`src/recomp/enhance/helpfile.cpp`).
