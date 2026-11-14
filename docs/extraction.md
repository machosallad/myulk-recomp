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

## Notes for the port

- Cinepak is supported by ffmpeg; convert videos once at build time (e.g. to WebM/VP9 or a frame sequence) so the runtime doesn't need a Cinepak decoder. ffmpeg is not installed yet.
- 8-bit PCM WAV loads directly with SDL.
- The EXE also builds file names at runtime (`GA%d%d.bmp`, `Gabt%d.bmp`), so the full asset list has to come from the code, not just from the forms.
- `MYULK.HLP` (WinHelp) probably holds the in-game manual and rules text. It is not extracted yet.
