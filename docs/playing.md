# Playing Myulk

You need your own copy of the *Myulk* CD, as a disc or an image (`.iso`). Nothing from the game is in this repository, so you build the game once from the source code and your CD. On Windows that is mostly waiting.

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

### A package to keep

```bat
python tools\package.py desktop --game extracted\iso
```

This makes `dist\myulk-desktop` (and a `.zip` of it): `Myulk.exe`, the game files in `game\`, a short README and the licences. The folder can live anywhere. Without `--game` the `game` folder stays empty, and you copy the CD's contents into it yourself. The package is for you only (see [Legal](../README.md#legal)).

## The browser version

Chrome or Edge (it needs WebAssembly JSPI), on a computer, phone or tablet.

```bat
cmake --preset web -DPython3_EXECUTABLE=<python with capstone>
cmake --build --preset web --target myulk_recomp
python -m http.server -d build/web 8080
```

Open `http://localhost:8080/myulk_recomp.html` and click to start (browsers only allow sound after a click). The page downloads each game file the first time it is needed and keeps it in the browser, so later visits start at once. Saves and settings are kept in the browser too.

`python tools\package.py web --game extracted\iso` puts the page and the game files together in `dist\myulk-web`, to serve with any web server, for example `python -m http.server` in that folder. Keep it to your own computer or network: the game files must not be put on a public site.

On a phone or tablet a tap is a click, the game asks to go fullscreen in landscape, the 👁 button shows what can be clicked, ⚙ opens the settings (and the handbook) and ⛶ switches fullscreen.

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

- **"The game files were not found"**: the program looks for `MYULK\MYULK.EXE` in a `game` folder next to it, in its own folder, in `MYULK_GAME_DIR`, or in `extracted\iso` of the repository.
- **The build stops at "Recompiling MYULK.EXE"**: Python or capstone is missing. Run the setup script again, or `python -m pip install capstone`.
- **The browser page says JSPI is missing**: use a current Chrome or Edge.
