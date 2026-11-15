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
