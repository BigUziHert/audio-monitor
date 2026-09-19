#Requires -Version 5.0
<#
.SYNOPSIS
    Packages a built Windows release with the per-user installer.
#>
[CmdletBinding()]
param(
    [string]$BuildDir = 'build',
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo')]
    [string]$Config = 'Release',
    [string]$OutputDir = 'build/package'
)

$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$workspacePrefix = $repo.TrimEnd('\') + '\'
$buildPath = [IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
$outputPath = [IO.Path]::GetFullPath((Join-Path $repo $OutputDir))
foreach ($path in @($buildPath, $outputPath)) {
    if (-not $path.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw 'BuildDir and OutputDir must be subdirectories of this repository.'
    }
    $cursor = $path
    while ($cursor -and $cursor.Length -ge $repo.Length) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing to package through a junction or symbolic link: $cursor"
        }
        $cursor = Split-Path -Parent $cursor
    }
}
$binaryDir = Join-Path $buildPath $Config
if (-not (Test-Path -LiteralPath (Join-Path $binaryDir 'audio-monitor.exe') -PathType Leaf)) {
    throw "Build the GUI first: missing $binaryDir\audio-monitor.exe"
}
New-Item -ItemType Directory -Path $outputPath -Force | Out-Null
$stage = Join-Path $outputPath ('stage-' + [Guid]::NewGuid().ToString('N'))
$archive = Join-Path $outputPath 'audio-monitor-win64.zip'
$temporaryArchive = Join-Path $outputPath ('package-' + [Guid]::NewGuid().ToString('N') + '.zip')
try {
    New-Item -ItemType Directory -Path $stage | Out-Null
    Copy-Item -LiteralPath (Join-Path $binaryDir 'audio-monitor.exe') -Destination $stage
    if (Test-Path -LiteralPath (Join-Path $binaryDir 'audiomon-cli.exe') -PathType Leaf) {
        Copy-Item -LiteralPath (Join-Path $binaryDir 'audiomon-cli.exe') -Destination $stage
    }
    Copy-Item -LiteralPath (Join-Path $repo 'LICENSE') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repo 'packaging/Install.ps1') -Destination $stage
    Copy-Item -LiteralPath (Join-Path $repo 'packaging/Install.cmd') -Destination $stage
    Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $temporaryArchive
    if (Test-Path -LiteralPath $archive) {
        [IO.File]::Replace($temporaryArchive, $archive, [System.Management.Automation.Language.NullString]::Value)
    } else {
        [IO.File]::Move($temporaryArchive, $archive)
    }
    Write-Host "Package: $archive"
    Write-Host ('SHA256: ' + (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash)
}
finally {
    # These are unique paths under the checked repository output directory.
    foreach ($temporary in @($stage, $temporaryArchive)) {
        $full = [IO.Path]::GetFullPath($temporary)
        if (-not $full.StartsWith($outputPath.TrimEnd('\') + '\', [StringComparison]::OrdinalIgnoreCase)) {
            throw "Unsafe package cleanup path: $full"
        }
        if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full -Recurse -Force }
    }
}
