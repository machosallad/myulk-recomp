# Installs the toolchain for building the Myulk port on Windows.
#
#   powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1
#   powershell -ExecutionPolicy Bypass -File tools\setup-windows.ps1 -SkipWeb   # desktop only
#
# Installs (skipping anything already present):
#   - Visual Studio 2022 Build Tools, C++ workload
#       -> C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
#   - CMake            -> C:\Program Files\CMake
#   - Ninja            -> winget's package folder (on PATH via winget links)
#   - Python 3.12 + capstone (pip, per user), 7-Zip
#   - Emscripten SDK   -> C:\Tools\emsdk (or -EmsdkDir)
#
# The Build Tools installer asks for administrator rights (UAC prompt).
# Open a new terminal afterwards so the updated PATH is picked up.

param(
    [string]$EmsdkDir = "C:\Tools\emsdk",
    [switch]$SkipWeb
)

$ErrorActionPreference = "Stop"

function Step($msg) { Write-Host "`n==> $msg" -ForegroundColor Cyan }

function Have-WingetPackage($id) {
    $out = winget list --id $id --exact --accept-source-agreements 2>$null | Out-String
    return $out -match [regex]::Escape($id)
}

function Install-WingetPackage($id, [string[]]$extra = @()) {
    if (Have-WingetPackage $id) {
        Write-Host "    $id already installed"
        return
    }
    winget install --id $id --exact --accept-package-agreements --accept-source-agreements @extra
    if ($LASTEXITCODE -ne 0) { throw "winget install $id failed ($LASTEXITCODE)" }
}

if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    throw "winget not found. Install 'App Installer' from the Microsoft Store first."
}

Step "Visual Studio 2022 Build Tools (C++ workload) - this can take a while"
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$hasVc = $false
if (Test-Path $vswhere) {
    $hasVc = [bool](& $vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -latest -property installationPath)
}
if ($hasVc) {
    Write-Host "    MSVC already installed"
} else {
    Install-WingetPackage "Microsoft.VisualStudio.2022.BuildTools" @(
        "--override", "--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive --wait")
}

Step "CMake"
Install-WingetPackage "Kitware.CMake"

Step "Ninja"
Install-WingetPackage "Ninja-build.Ninja"

Step "Python 3 and capstone (the recompiler runs at build time)"
if (-not (Get-Command python -ErrorAction SilentlyContinue) -or
    -not ((& python --version 2>&1) -match "Python 3\.(1[0-9])")) {
    Install-WingetPackage "Python.Python.3.12"
    $env:Path = [Environment]::GetEnvironmentVariable("Path", "User") + ";" + $env:Path
}
& python -m pip install --user --upgrade capstone
if ($LASTEXITCODE -ne 0) { throw "pip install capstone failed" }

Step "7-Zip (to unpack the CD image)"
if (-not (Test-Path "$env:ProgramFiles\7-Zip\7z.exe")) { Install-WingetPackage "7zip.7zip" }
else { Write-Host "    7-Zip already installed" }

if (-not $SkipWeb) {
    Step "Emscripten SDK -> $EmsdkDir"
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Install-WingetPackage "Git.Git" }
    if (-not (Test-Path "$EmsdkDir\emsdk.bat")) {
        New-Item -ItemType Directory -Force (Split-Path $EmsdkDir) | Out-Null
        git clone https://github.com/emscripten-core/emsdk.git $EmsdkDir
        if ($LASTEXITCODE -ne 0) { throw "git clone emsdk failed" }
    } else {
        Write-Host "    emsdk already cloned, updating"
        git -C $EmsdkDir pull --ff-only
    }
    Push-Location $EmsdkDir
    try {
        & .\emsdk.bat install latest
        if ($LASTEXITCODE -ne 0) { throw "emsdk install failed" }
        # --permanent stores EMSDK and PATH in the user environment
        & .\emsdk.bat activate latest --permanent
        if ($LASTEXITCODE -ne 0) { throw "emsdk activate failed" }
    } finally {
        Pop-Location
    }
}

Step "Done"
Write-Host @"
Open a NEW terminal in the repository folder, then (see docs/playing.md):

  1. Unpack your CD:   "C:\Program Files\7-Zip\7z.exe" x -y -oextracted\iso <your Myulk.iso>
                       (or copy the CD's contents to extracted\iso)
  2. Build and play:   tools\build-native.bat
                       build\native\myulk_recomp.exe
  3. A package:        python tools\package.py desktop

  Web:  cmake --preset web
        cmake --build --preset web --target myulk_recomp
"@
