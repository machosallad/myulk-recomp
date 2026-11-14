@echo off
rem Build the native (Windows) version with MSVC + Ninja, then run the tests.
rem   tools\build-native.bat            Release build (build\native)
rem   tools\build-native.bat debug      Debug build   (build\native-debug)
setlocal
set PRESET=native
if /i "%~1"=="debug" set PRESET=native-debug

rem Set up the MSVC environment unless we're already in a VS prompt.
where cl >nul 2>nul
if errorlevel 1 (
    for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set VSDIR=%%i
)
if defined VSDIR call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

cd /d "%~dp0.."
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
ctest --preset native --test-dir build\%PRESET% 2>nul || ctest --test-dir build\%PRESET% --output-on-failure || exit /b 1
echo.
echo Built build\%PRESET%\myulk.exe
