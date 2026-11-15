# Playing Myulk

You need your own copy of the *Myulk* CD, as a disc or an image (`.iso`, `.bin`/`.cue`).

**If you already have the program** (a `Myulk.exe` folder, or the web page on a server), there is nothing to set up: see [Your CD on first start](#your-cd-on-first-start). The rest of this page is about building the program from the source code, which also needs the CD, because the recompiler translates your `MYULK.EXE`. On Windows that is mostly waiting.

## 1. Tools (once)

From the repository folder, in PowerShell:

```bat
powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1
```

It installs, skipping anything you already have: Visual Studio 2022 Build Tools (the C++ compiler; asks for administrator rights), CMake, Ninja, Python 3 with the `capstone` package (the recompiler uses them), 7-Zip, and the Emscripten SDK for the browser version (`-SkipWeb` leaves it out). Open a new terminal afterwards. [building.md](building.md) lists the tools for installing by hand, and Linux and macOS.

## 2. Your CD

Put the CD's contents in `extracted\iso`, so that `extracted\iso\MYULK\MYULK.EXE` exists:

```bat
"C:\Program Files\7-Zip\7z.exe" x -y -oextracted\iso C:\path\to\Myulk.iso
```

From a disc, copy everything on it into `extracted\iso` instead. These files stay on your computer; git ignores the folder.

## 3. Build and play

```bat
tools\build-native.bat
build\native\myulk_recomp.exe
```

The first build translates `MYULK.EXE` into C++ and compiles it, which takes a few minutes; later builds are quick.

### A package

```bat
python tools\package.py desktop
```

This makes `dist\myulk-desktop` (and `dist\myulk-desktop.zip`, about 5 MB):

```
myulk-desktop\
  Myulk.exe        the game (17 MB, everything linked in)
  licenses\        licences of this project and the libraries in Myulk.exe
  README.txt       keys, saves, short instructions (Swedish and English)
```

Nothing else is needed: no installation, no Python, no Visual C++ runtime (it is linked into `Myulk.exe`), on any 64-bit Windows 10 or 11. Unzip it anywhere and start `Myulk.exe`; it asks for the CD the first time. Saves and settings go to the user's `%APPDATA%\myulk-recomp\Myulk`, never into the folder.

`--game extracted\iso` adds the CD's files in a `game` folder (about 130 MB zipped), so the package plays without asking; that one is for your own use (see [Legal](../README.md#legal)). `build\native` is the build folder, full of intermediate files; the package is the part to keep or move.

## The browser version

Chrome or Edge (it needs WebAssembly JSPI), on a computer, phone or tablet.

```bat
cmake --preset web -DPython3_EXECUTABLE=<python with capstone>
cmake --build --preset web --target myulk_recomp
python -m http.server -d build/web 8080
```

Open `http://localhost:8080/myulk_recomp.html` and click to start (browsers only allow sound after a click). The page downloads each game file the first time it is needed and keeps it in the browser, so later visits start at once. Saves and settings are kept in the browser too.

`python tools\package.py web` puts the page in `dist\myulk-web`, to serve with any web server (for example `python -m http.server` in that folder; other computers need an `https://` address for the browser to keep the files). Without game files on the server, each player's browser asks for their own CD image on the first visit. With `--game extracted\iso` the server provides the game files instead, for your own use.

On a phone or tablet a tap is a click, the game asks to go fullscreen in landscape, the 👁 button shows what can be clicked, ⚙ opens the settings (and the handbook) and ⛶ switches fullscreen.

## Your CD on first start

When the program finds no game files, it shows *Välkommen till Myulk* and asks for the CD:

- **Välj skivavbild...**: the CD image, `.iso` or `.bin`/`.cue` (in the browser `.iso` or `.bin`).
- **Välj mapp eller cd...**: the CD drive, or a folder with the CD's files (desktop).
- Or drop the image or folder on the window.

It checks that `MYULK.EXE` is the edition the program was made from (a different edition or a damaged copy gets a clear message) and copies the game's folders, 265 MB:

- **Desktop:** into `%APPDATA%\myulk-recomp\Myulk\game`, in a few seconds. The CD isn't needed again.
- **Browser:** into the browser's storage for that address, in about half a minute. The image is read in the browser; nothing is uploaded. Each browser and device asks once.

To use another copy later, delete that `game` folder (desktop) or the site's data in the browser.

## First start

The game asks how you want to play:

- **Med förbättringar** (with improvements): every helper below is on.
- **Som originalet (1996)**: none of them; the game as it was, with only the picture and text adapted to today's screens.

You can change it any time under **Inställningar** (F2), all at once with *Spelstil* or one helper at a time.

## Keys

| Key | |
|---|---|
| **F1** | the handbook |
| **F2** | settings |
| **F5 / F9** | quick-save / quick-load |
| **F7 / F8** | picture scaling / text style |
| **F11** | fullscreen |
| **F12** | screenshot (Pictures\Myulk, or a download in the browser) |
| **H** (hold) | show everything you can click in the scene |
| **Arrows, WASD** | turn and walk; down or Backspace goes back |
| **Right-click, mouse wheel** | go back / walk forward |
| **P** | pause the farm |
| **N**, or a click on the day clock | next day on the farm |

The settings menu has two pages: *Bild och ljud* (scaling, de-dither filter, text, volume for everything, music and speech, fullscreen) and *Spelhjälp* (the helpers and the farm speed).

## Saves

The game's own menu (the "Meny" sign at the left edge, or on the farm) saves and loads, now with cards that show a picture, the name, the place and the date. Quick-save keeps one save (`SNABB`), autosave three (`AUTO1`–`AUTO3`).

- **Windows:** `%APPDATA%\myulk-recomp\Myulk\drive_c\MYULK`, with `settings.txt` one folder up. The game files are never changed.
- **Browser:** stored by the browser for that address, kept after reloading.

Saves are the original `.MLK` files, so they can be moved between the desktop and the browser (in the browser through the developer tools), and to the original game.

## If something goes wrong

- **Where the program looks for the game files**: `MYULK_GAME_DIR`, a `game` folder next to the program, the program's own folder, the files imported on first start, and `extracted\iso` of the repository. If it finds none, it asks for the CD.
- **"En annan version av Myulk"**: the program was made from a different edition of `MYULK.EXE` than the one on this CD (or the copy is damaged). Building the program from this CD makes one that fits it.
- **The build stops at "Recompiling MYULK.EXE"**: Python or capstone is missing. Run the setup script again, or `python -m pip install capstone`.
- **The browser page says JSPI is missing**: use a current Chrome or Edge.
