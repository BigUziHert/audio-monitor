#Requires -Version 5.0
<#
.SYNOPSIS
    Installs or updates Audio Monitor for the current Windows user.
.DESCRIPTION
    Extract the complete release ZIP, then run Install.cmd. No administrator
    access is needed. Mixer settings in AppData are never replaced.
#>
[CmdletBinding()]
param(
    [switch]$NoLaunch,
    [switch]$SkipRegistration,
    # Integration tests only: a pre-existing directory inside the user's TEMP.
    # This override cannot register startup, create a shortcut, or launch apps.
    [string]$TestRoot
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0

function Assert-SafePath([string]$Path, [string]$Parent) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $parentFull = [IO.Path]::GetFullPath($Parent).TrimEnd('\')
    if (-not $full.StartsWith($parentFull + '\', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must be inside ${parentFull}: $full"
    }
    # Resolve every existing ancestor; a lexical containment check alone does
    # not protect a junction which redirects writes outside the installation.
    $cursor = $full
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Refusing a junction or symbolic link: $cursor"
        }
        $next = Split-Path -Parent $cursor
        if ($next -eq $cursor) { break }
        $cursor = $next
    }
    return $full
}

function Request-AppExit([string]$Executable) {
    $matching = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path -and [StringComparer]::OrdinalIgnoreCase.Equals($_.Path, $Executable) }
        catch { $false }
    })
    if ($matching.Count -eq 0) { return }
    if (-not ('AudioMonitorInstaller.Windows' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
namespace AudioMonitorInstaller {
    public static class Windows {
        private delegate bool EnumCallback(IntPtr window, IntPtr context);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumCallback callback, IntPtr context);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int capacity);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterWindowMessage(string name);
        [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        public static void RequestExit(int processId, bool legacy) {
            uint message = legacy ? 0x0010u : RegisterWindowMessage("AudioMonitor.ExitForUpdate");
            if (message == 0) return;
            EnumWindows(delegate(IntPtr window, IntPtr context) {
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner == (uint)processId) {
                    var name = new StringBuilder(256);
                    GetClassName(window, name, name.Capacity);
                    if (name.ToString() == "AudioMonitorWindow")
                        PostMessage(window, message, IntPtr.Zero, IntPtr.Zero);
                }
                return true;
            }, IntPtr.Zero);
        }
    }
}
'@
    }
    foreach ($process in $matching) {
        [AudioMonitorInstaller.Windows]::RequestExit($process.Id, $false)
    }
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    foreach ($process in $matching) {
        if (-not $process.WaitForExit(1500)) {
            # Older versions understand WM_CLOSE; close-to-tray is respected.
            [AudioMonitorInstaller.Windows]::RequestExit($process.Id, $true)
            $remaining = [Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
            if (-not $process.WaitForExit($remaining)) {
                throw 'Audio Monitor is still running. Choose Exit from its tray menu, then run Install.cmd again.'
            }
        }
    }
}

if ($TestRoot) {
    if (-not $NoLaunch -or -not $SkipRegistration) {
        throw 'TestRoot requires both -NoLaunch and -SkipRegistration.'
    }
    $base = Assert-SafePath $TestRoot ([IO.Path]::GetTempPath())
    if (-not (Test-Path -LiteralPath $base -PathType Container)) {
        throw 'TestRoot must already exist.'
    }
    $installDir = Assert-SafePath (Join-Path $base 'Programs/Audio Monitor') $base
    $configPath = Join-Path $base 'Roaming/audio-monitor/config.json'
} else {
    $localAppData = [Environment]::GetFolderPath('LocalApplicationData')
    if (-not $localAppData) { throw 'The current user has no LocalApplicationData directory.' }
    $installDir = Assert-SafePath (Join-Path $localAppData 'Programs/Audio Monitor') $localAppData
    $configPath = Join-Path ([Environment]::GetFolderPath('ApplicationData')) 'audio-monitor/config.json'
}
$source = [IO.Path]::GetFullPath($PSScriptRoot)
if ([StringComparer]::OrdinalIgnoreCase.Equals($source.TrimEnd('\'), $installDir)) {
    throw 'Extract the release ZIP to another folder before installing it.'
}
$payload = @('audio-monitor.exe', 'LICENSE')
if (Test-Path -LiteralPath (Join-Path $source 'audiomon-cli.exe') -PathType Leaf) {
    $payload += 'audiomon-cli.exe'
}
foreach ($name in $payload) {
    $file = Assert-SafePath (Join-Path $source $name) $source
    if (-not (Test-Path -LiteralPath $file -PathType Leaf)) {
        throw "The release is incomplete. Extract the whole ZIP; missing $name."
    }
    if ((Get-Item -LiteralPath $file).Length -eq 0) { throw "The release contains an empty file: $name" }
}

$startup = $true
if (Test-Path -LiteralPath $configPath -PathType Leaf) {
    try {
        $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
        if ($config.PSObject.Properties.Name -contains 'startWithWindows' -and
            $config.startWithWindows -is [bool]) {
            $startup = $config.startWithWindows
        }
    } catch {
        # Never rewrite a damaged config, and do not guess a startup preference.
        throw "Cannot read existing mixer settings. They were left untouched: $configPath"
    }
}

New-Item -ItemType Directory -Path $installDir -Force | Out-Null
$stage = Assert-SafePath (Join-Path $installDir ('.install-' + [Guid]::NewGuid().ToString('N'))) $installDir
$incoming = Join-Path $stage 'incoming'
$backups = Join-Path $stage 'backups'
$changes = New-Object 'System.Collections.Generic.List[object]'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$oldRun = $null
$runChanged = $false
$shortcut = $null
$oldShortcut = $null
$shortcutChanged = $false
$rollbackFailed = $false
try {
    New-Item -ItemType Directory -Path $incoming, $backups -Force | Out-Null
    foreach ($name in $payload) {
        $destination = Assert-SafePath (Join-Path $installDir $name) $installDir
        if (Test-Path -LiteralPath $destination -PathType Container) {
            throw "Cannot replace a directory with a release file: $destination"
        }
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $incoming $name)
    }
    $executable = Join-Path $installDir 'audio-monitor.exe'
    Request-AppExit $executable
    foreach ($name in $payload) {
        $destination = Assert-SafePath (Join-Path $installDir $name) $installDir
        $replacement = Join-Path $incoming $name
        $backup = Join-Path $backups $name
        $existed = Test-Path -LiteralPath $destination -PathType Leaf
        if ($existed) { [IO.File]::Replace($replacement, $destination, $backup) }
        else { [IO.File]::Move($replacement, $destination) }
        $changes.Add([PSCustomObject]@{ Destination = $destination; Backup = $backup; Existed = $existed })
    }
    if (-not $SkipRegistration) {
        if (Test-Path -LiteralPath $runKey) {
            $runValues = Get-Item -LiteralPath $runKey
            if ($runValues.GetValueNames() -contains 'AudioMonitor') {
                $oldRun = @{ Value = $runValues.GetValue('AudioMonitor', $null, 'DoNotExpandEnvironmentNames');
                             Kind = $runValues.GetValueKind('AudioMonitor').ToString() }
            }
        }
        if (-not (Test-Path -LiteralPath $runKey)) { New-Item -Path $runKey -Force | Out-Null }
        $runChanged = $true
        if ($startup) {
            New-ItemProperty -LiteralPath $runKey -Name AudioMonitor -Value ('"' + $executable + '" --tray') -PropertyType String -Force | Out-Null
        } elseif ($oldRun) {
            Remove-ItemProperty -LiteralPath $runKey -Name AudioMonitor
        }
        $programs = [Environment]::GetFolderPath('Programs')
        $shortcut = Assert-SafePath (Join-Path $programs 'Audio Monitor.lnk') $programs
        if (Test-Path -LiteralPath $shortcut -PathType Leaf) { $oldShortcut = [IO.File]::ReadAllBytes($shortcut) }
        $shell = New-Object -ComObject WScript.Shell
        try {
            $link = $shell.CreateShortcut($shortcut)
            $link.TargetPath = $executable
            $link.WorkingDirectory = $installDir
            $link.Description = 'Audio Monitor'
            $link.IconLocation = "$executable,0"
            $shortcutChanged = $true
            $link.Save()
        } finally { [void][Runtime.InteropServices.Marshal]::ReleaseComObject($shell) }
    }
} catch {
    $failure = $_
    if ($shortcutChanged) {
        try {
            if ($null -ne $oldShortcut) { [IO.File]::WriteAllBytes($shortcut, $oldShortcut) }
            elseif (Test-Path -LiteralPath $shortcut) { Remove-Item -LiteralPath $shortcut -Force }
        } catch {
            $rollbackFailed = $true
            Write-Warning "Could not restore the previous shortcut: $($_.Exception.Message)"
        }
    }
    if ($runChanged) {
        try {
            if ($oldRun) { New-ItemProperty -LiteralPath $runKey -Name AudioMonitor -Value $oldRun.Value -PropertyType $oldRun.Kind -Force | Out-Null }
            else { Remove-ItemProperty -LiteralPath $runKey -Name AudioMonitor -ErrorAction SilentlyContinue }
        } catch {
            $rollbackFailed = $true
            Write-Warning "Could not restore the previous startup entry: $($_.Exception.Message)"
        }
    }
    for ($index = $changes.Count - 1; $index -ge 0; --$index) {
        try {
            $change = $changes[$index]
            $destination = Assert-SafePath $change.Destination $installDir
            if ($change.Existed) { [IO.File]::Replace($change.Backup, $destination, [System.Management.Automation.Language.NullString]::Value) }
            else { [IO.File]::Delete($destination) }
        } catch {
            $rollbackFailed = $true
            Write-Warning "Could not restore $($change.Destination): $($_.Exception.Message)"
        }
    }
    if ($rollbackFailed) { Write-Warning "Rollback could not finish. Backup files were retained at $backups." }
    throw $failure
} finally {
    if (-not $rollbackFailed -and (Test-Path -LiteralPath $stage)) {
        $checked = Assert-SafePath $stage $installDir
        Remove-Item -LiteralPath $checked -Recurse -Force
    }
}
Write-Host "Installed Audio Monitor: $installDir"
if (-not $SkipRegistration) { Write-Host "Start with Windows preference: $startup" }
if (-not $NoLaunch) { Start-Process -FilePath (Join-Path $installDir 'audio-monitor.exe') -WorkingDirectory $installDir }
