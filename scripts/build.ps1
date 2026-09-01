#Requires -Version 5.0
<#
.SYNOPSIS
    Configures and builds audio-monitor with MSVC.

.DESCRIPTION
    Visual Studio ships its own CMake, but only puts it on PATH inside the
    "Developer PowerShell for VS 2022" shell -- so a plain PowerShell window
    reports 'cmake is not recognized' even on a machine that has everything
    needed. This script locates Visual Studio through vswhere and falls back to
    the bundled CMake when there is none on PATH.

.EXAMPLE
    .\scripts\build.ps1
    .\scripts\build.ps1 -Config Debug
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',

    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Push-Location $repo

try {
    # --- locate Visual Studio -------------------------------------------------
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw @"
Visual Studio Installer not found.

Install Visual Studio 2022 (Community is free) with the
"Desktop development with C++" workload:
    winget install Microsoft.VisualStudio.2022.Community
or the smaller build-tools-only package:
    winget install Microsoft.VisualStudio.2022.BuildTools
Either way you must tick "Desktop development with C++" in the installer.
"@
    }

    # Select-Object -First 1: vswhere emits one line per install, and several
    # side-by-side editions is common. Without this $vsPath becomes an array
    # and every Join-Path against it fails.
    $vsPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath |
        Select-Object -First 1
    if (-not $vsPath) {
        throw @"
Visual Studio is installed, but without the C++ compiler.

Open the Visual Studio Installer, click Modify, and tick
"Desktop development with C++".
"@
    }
    Write-Host "Visual Studio: $vsPath" -ForegroundColor Cyan

    # --- locate cmake ---------------------------------------------------------
    $cmakeExe = (Get-Command cmake -ErrorAction SilentlyContinue).Source
    if ($cmakeExe) {
        Write-Host "CMake (PATH):  $cmakeExe" -ForegroundColor Cyan
    }
    else {
        $bundled = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) {
            $cmakeExe = $bundled
            Write-Host "CMake (VS):    $cmakeExe" -ForegroundColor Cyan
        }
        else {
            throw @"
CMake is not on PATH and Visual Studio did not bundle it.

Either tick "C++ CMake tools for Windows" in the Visual Studio Installer,
or install CMake standalone:
    winget install Kitware.CMake
Then open a NEW terminal so the updated PATH takes effect.
"@
        }
    }

    # --- build ----------------------------------------------------------------
    if ($Clean -and (Test-Path build)) {
        Write-Host "Removing previous build directory..." -ForegroundColor Yellow
        Remove-Item -Recurse -Force build
    }

    & $cmakeExe -S . -B build -G 'Visual Studio 17 2022' -A x64
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed with exit code $LASTEXITCODE." }

    & $cmakeExe --build build --config $Config
    if ($LASTEXITCODE -ne 0) { throw "Build failed with exit code $LASTEXITCODE." }

    Write-Host ""
    Write-Host "Build succeeded." -ForegroundColor Green
    Get-ChildItem (Join-Path 'build' $Config) -Filter *.exe -ErrorAction SilentlyContinue |
        ForEach-Object { Write-Host ("  {0}  ({1:N0} bytes)" -f $_.FullName, $_.Length) }
    Write-Host ""
    Write-Host "Start with the headless tool -- it opens no audio devices:" -ForegroundColor Cyan
    Write-Host "    .\build\$Config\audiomon-cli.exe --list"
}
finally {
    Pop-Location
}
