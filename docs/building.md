# Building

The port is C++20 with SDL3. CMake downloads SDL3 and nlohmann/json on the first configure.

The game needs the extracted original data (see [extraction.md](extraction.md)). At runtime it looks for `extracted/resources`:

1. in the `MYULK_DATA` environment variable,
2. relative to the working directory,
3. relative to the executable.

## Tools

On Windows, one script installs everything. Run it from the repository folder:

```bat
powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1
```

| Tool | Installed to |
|---|---|
| VS 2022 Build Tools (MSVC, C++ workload) | `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools` (asks for admin) |
| CMake | `C:\Program Files\CMake` |
| Ninja | winget's package folder, put on `PATH` by winget |
| Emscripten SDK | `C:\Tools\emsdk` (change with `-EmsdkDir D:\path`); `EMSDK` and `PATH` are set for your user |

Use `-SkipWeb` for a desktop-only setup. The script skips anything already installed, so it's safe to run again. Open a new terminal afterwards.

To install manually instead:

| Tool | Windows install |
|---|---|
| MSVC (VS 2022 Build Tools, C++ workload) | `winget install Microsoft.VisualStudio.2022.BuildTools --override "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive"` |
| CMake ≥ 3.24 | `winget install Kitware.CMake` |
| Ninja | `winget install Ninja-build.Ninja` |
| Emscripten (web build only) | clone `emsdk`, then `emsdk install latest` and `emsdk activate latest` |

## Desktop (Windows)

The easy way, from any terminal (it finds and sets up MSVC, builds, and runs the tests):

```bat
tools\build-native.bat
build\native\myulk.exe
```

Or by hand, from a "x64 Native Tools Command Prompt for VS 2022":

```bat
cmake --preset native
cmake --build --preset native
ctest --preset native
build\native\myulk.exe            :: or: build\native\myulk.exe TMENUFORM
```

Linux and macOS use the same commands with the system compiler.

## Browser

Run these from a shell where `emsdk_env` has been sourced (`EMSDK` is set):

```bash
cmake --preset web -DPython3_EXECUTABLE=<python with capstone>
cmake --build --preset web --target myulk_recomp
python -m http.server -d build/web 8080     # open http://localhost:8080/myulk_recomp.html
```

`myulk_recomp` is the recompiled original game in the browser:

- **Browser support:** it uses WebAssembly JSPI (so the game's blocking message loop can wait for the browser) and native wasm exceptions (for Delphi exceptions). Current Chrome and Edge support both; the page says so if a browser lacks JSPI.
- **Start:** the page loads the data, then asks for a click. Browsers only allow sound after the user has interacted with the page.
- **Data on demand:** the build copies the folders the game reads from your extracted CD (`extracted/iso`: MYULK, PICTDLL, ART, SOUNDS, LJUD, MOVIES, IMAGES; 967 files, about 265 MB) to `build/web/game/` and writes `game/manifest.txt` (`tools/web_data.py`). The page itself is about 14 MB. At start it fetches only the manifest and creates the folder tree with empty placeholders at `/game` (C:\ inside the game); each file is downloaded the first time the game opens it (`src/recomp/win16/webdata.cpp`), and a small "Laddar…" note shows while that takes a moment. Downloaded files are kept in the browser's Cache Storage (named after a hash of the manifest; caches of other versions are deleted), so later visits only fetch the manifest. This needs a secure context (`https://` or `localhost`); elsewhere files are simply downloaded each visit. `build/web/game/` is the original game's data: don't publish it.
- **Saves** are stored in the browser's IndexedDB (mounted at `/saves`), so they survive reloads.
- **Text** (desktop and web) is anti-aliased TrueType, drawn with the embedded Liberation fonts in place of the Windows fonts; see `docs/third-party.md`.
- **Settings menu:** **F2** or the ⚙ button on the web page; the game pauses while it is open. Two pages (**Tab** switches): *Bild och ljud* (picture and sound) and *Spelhjälp* (gameplay helpers). Hovering a row shows what it does.
- **Play style:** every gameplay helper below can be switched on or off on its own (setting names in italics). The *Spelstil* row on top sets them all at once: *Original* (all off and the farm at 1×: the 1996 game, with only the picture and text adapted to today's screens) or *Modern* (all on); a mix shows *Anpassad*. On the first start the game asks once, "Hur vill du spela Myulk?", with these two choices (Esc keeps Modern). Scripted runs skip the question.
- **Gameplay helpers** (desktop and web):
  - **Skip:** a click (or tap) skips the movie or speech clip the game is waiting for; music keeps playing. *Klick hoppar över filmer och tal*.
  - **Farm speed:** *Gårdens hastighet* 1×/2×/4×/8× (a farm day is about 12.5 s at 1×).
  - **Farm helper** (*Gårdshjälp*): **P** pauses and resumes farm time; a click on the *Ett dygn* clock (or **N**) moves on to the next day at once (the game's own day tick, so events and messages happen as usual); short notes show the date after such a step, when a season begins, and when the balance changes ("–4 048 kr (saldo 145 952 kr)"; changes within 0.7 s are added up). `src/recomp/enhance/farm.cpp`.
  - **Quick-save / quick-load** (*Snabbspara*): **F5** / **F9** (and menu buttons) save to and load `SNABB.MLK`, through the game's own save code.
  - **Autosave** (*Autospara*): about 5 s after a chapter starts or the farm opens, and every 10 minutes, into three rotating files `AUTO1`–`AUTO3`.
  - **Hotspot hints** (*Visa klickbara ställen*): hold **H** (on touch screens the 👁 button) to see everything clickable in the current adventure scene: hotspots in yellow, directions in blue with arrows.
  - **Save screen** (*Ny spara/ladda-skärm*): see "Save and load" below; off gives the original Windows dialogs.
  - **Moving around** (*Gå med tangenter och mus*): in the adventure scenes the **arrow keys** (or **WASD**) turn left/right and walk forward (up looks up where there is no way forward), **down** looks down or goes back, **PgUp/PgDn** look up/down, and **Backspace**, a **right-click** in the picture or **scrolling down** leave a close-up; **scrolling up** walks forward. Each shortcut clicks where the game expects the click (`src/recomp/enhance/nav.cpp`), so only exits the scene has work.
  - **Fortsätt (continue)** (*Fortsätt-kort vid start*): when there are saves, a card over the start of the intro offers the newest one (picture, name, place, time). *Fortsätt* (or Enter) skips the intro silently and loads it as soon as chapter 1 is running, about 3 seconds; *Nytt spel* (or Esc) plays the intro as usual. Scripted runs skip the card; `MYULK_START_CARD=0/1` turns it off or forces it.
- **Always available** (not part of the play style):
  - **Volume:** *Volym (allt)*, plus *Musik* (the `MU_*` music loops) and *Tal och ljud* (speech, effects and movie sound) on their own.
  - **Window:** the desktop window opens with the size, position, maximised and fullscreen state it had last time.
  - **Screenshots:** **F12** saves the game picture (640×480, as shown, without notes) as a PNG in `Pictures/Myulk` (in the browser: a download). `MYULK_SCREENSHOT_DIR` points tests elsewhere.
- **Display settings** (desktop and web, saved between sessions): **F7** cycles the picture scaling: *mjuk skalning* (default; enlarges by the largest whole factor with sharp pixels, then smoothly to the window), *utjämnad* (smooth), *skarpa pixlar* (sharp), *heltalsskalning* (only whole multiples like 2× or 3×, with a black border around the rest, so every original pixel is the same size). *Avdithring* (settings menu, off by default) smooths away the speckle of the art's 256-colour dithering in skies and flat areas with an edge-preserving filter, keeping outlines and text sharp (`src/recomp/enhance/dedither.cpp`; only the changed part of the screen is filtered again). **F8** switches text between smooth and crisp (no anti-aliasing, like the original). A short note shows the new setting. Settings live in `settings.txt` in the user's app-data folder (in the browser: IndexedDB); `MYULK_SETTINGS` points tests elsewhere, and `MYULK_SHOT_WINDOW=1` makes screenshots capture the scaled window.
- **Handbook** (desktop and web): the game's own help file, `MYULK.HLP` ("Myulk handbok": how to play, the glossary of farming words, the icons, and hints for each part of the adventure), opens in a modern viewer in place of the Windows 3.1 help program. It opens from the game's **Handbok** button (the game calls `WinHelp`), with **F1**, or from the settings menu. On the left the topics (contents and glossary first) with a search field that matches titles and text; on the right the topic with its pictures, text flowing around them. Links jump to other topics, dotted links open a definition card; **<** goes back, the wheel, arrow keys, PgUp/PgDn or dragging scroll, Esc closes. The file is read from your CD data at run time (`src/recomp/enhance/helpfile.cpp`, a reader for the Windows 3.1 help format as documented by helpdeco; `helpscreen.cpp` is the viewer).
- **Save and load** use a new screen (both desktop and web): cards with a picture of the game, the save's name, chapter or farm date, balance and when it was saved; "Ny sparning" asks for a name; loading, overwriting and deleting ask first. Saves keep the original `.MLK` format (the game's own code still writes and reads them), with `NAME.BMP` (picture) and `NAME.TXT` (name and time) next to each. The setting *Ny spara/ladda-skärm* (or `MYULK_CLASSIC_SAVE=1`) brings back the original dialogs; quick-save and autosave work either way.
- **Phones and tablets:** a tap is a click. The game's hover areas (the menu at the left edge, the inventory at the bottom) open on the tap itself. Starting the game asks for fullscreen and landscape; a ⛶ button in the corner toggles fullscreen. When a text box gets the focus (save dialog), the page focuses a hidden field so the phone's keyboard opens, and passes what is typed to the game. Browser zoom, scrolling and long-press menus are disabled over the game.
- `-DPython3_EXECUTABLE` must name a Python with capstone installed, because emsdk brings its own Python for the recompiler step.

The older `myulk` target (the hand-written port) still builds as `myulk.html`.

## The current app

`myulk` starts **the game**: the intro postcards and the first area of chapter 1 (see `src/adventure/scenes.cpp`).

- **Click** the scene to move. The cursor shows what a click does: arrows for turning or looking up/down, the hand for going forward or using a hotspot, the back arrow for leaving a close-up.
- **F11:** fullscreen. **Esc:** quit.
- **Not implemented yet:**
  - Movies: they are skipped, and the scene continues as if each one had finished.
  - Inventory, dialogue, the menu, and every scene after the first area.

`myulk --viewer [FORM]` opens the **form viewer** for the original screens:

- **Left/Right** (or **PgUp/PgDn**): switch between the 57 forms.
- **F1:** show component outlines (red = has event handlers).
- **Hover** over a component: the status bar shows its name and handlers.
- **Click:** logs the handler the original game would run.

Text uses SDL's built-in 8×8 debug font for now, with Swedish letters folded to ASCII.

### Automation and testing

`--screenshot out.bmp` renders, saves one frame and exits. `--frames N` waits N frames first. `--click X,Y` (repeatable) clicks at 640×480 screen coordinates before the screenshot. For example, this goes from the intro to the farmyard:

```bat
build\native\myulk.exe --click 300,200 --click 300,200 --click 300,200 --click 225,207 --screenshot out.bmp --frames 30
```
