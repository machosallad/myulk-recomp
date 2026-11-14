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
cmake --preset web
cmake --build --preset web
python -m http.server -d build/web 8080     # open http://localhost:8080/myulk.html
```

The web build packages `extracted/resources` into `myulk.data` with `--preload-file`. That file contains the original game's data, so don't publish it.

## The current app

The current app is a **form viewer**, the first piece of the skeleton:

- **Left/Right** (or **PgUp/PgDn**): switch between the 57 forms.
- **F1:** show component outlines (red = has event handlers).
- **F11:** toggle fullscreen.
- **Hover** over a component: the status bar shows its name and handlers.
- **Click:** logs the handler the original game would run.

Text uses SDL's built-in 8×8 debug font for now, with Swedish letters folded to ASCII.
