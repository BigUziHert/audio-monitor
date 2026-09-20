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
    # This override cannot access running apps or the registry, create shortcuts,
    # or launch apps.
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

function Initialize-InstallerWindows {
    if (-not ('AudioMonitorInstaller.Windows' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
namespace AudioMonitorInstaller {
    public static class Windows {
        private delegate bool EnumCallback(IntPtr window, IntPtr context);
        [DllImport("user32.dll")] private static extern bool EnumWindows(EnumCallback callback, IntPtr context);
        [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern int GetClassName(IntPtr window, StringBuilder name, int capacity);
        [DllImport("user32.dll", CharSet = CharSet.Unicode)] private static extern uint RegisterWindowMessage(string name);
        [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
        [DllImport("kernel32.dll")] private static extern bool CloseHandle(IntPtr handle);
        [DllImport("kernel32.dll", SetLastError = true)] private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)] private static extern IntPtr CreateMutexW(IntPtr security, bool initialOwner, string name);
        [DllImport("kernel32.dll")] private static extern bool ReleaseMutex(IntPtr handle);
        [DllImport("kernel32.dll")] private static extern uint GetCurrentProcessId();
        [DllImport("kernel32.dll")] private static extern bool ProcessIdToSessionId(uint processId, out uint sessionId);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] private static extern bool QueryFullProcessImageName(IntPtr process, uint flags, StringBuilder path, ref uint size);
        [DllImport("advapi32.dll")] private static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
        [DllImport("shell32.dll", CharSet = CharSet.Unicode)] private static extern IntPtr CommandLineToArgvW(string command, out int count);
        [DllImport("kernel32.dll")] private static extern IntPtr LocalFree(IntPtr memory);

        private static bool AbsoluteExecutable(string path) {
            if (String.IsNullOrEmpty(path)) return false;
            bool drive = path.Length >= 3 &&
                ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z')) &&
                path[1] == ':' && (path[2] == '\\' || path[2] == '/');
            return drive || (path.Length > 2 && path[0] == '\\' && path[1] == '\\');
        }

        private static string WindowClass(IntPtr window) {
            var name = new StringBuilder(256);
            GetClassName(window, name, name.Capacity);
            return name.ToString();
        }

        // Kept separate so tests can exercise the selection boundary without
        // enumerating, messaging, or stopping any real process.
        public static bool MatchesApp(string image, string windowClass, uint session, string owner, uint currentSession, string currentOwner) {
            return AbsoluteExecutable(image) &&
                String.Equals(Path.GetFileName(image), "audio-monitor.exe", StringComparison.OrdinalIgnoreCase) &&
                windowClass == "AudioMonitorWindow" && session == currentSession &&
                !String.IsNullOrEmpty(owner) && owner == currentOwner;
        }

        public static bool IsStartupCommand(string command) {
            if (String.IsNullOrEmpty(command) || command.Length > 260 ||
                command.IndexOfAny(new char[] { '\0', '\r', '\n' }) >= 0) return false;
            int count;
            IntPtr args = CommandLineToArgvW(command, out count);
            if (args == IntPtr.Zero) return false;
            try {
                if (count != 2) return false;
                string image = Marshal.PtrToStringUni(Marshal.ReadIntPtr(args));
                string option = Marshal.PtrToStringUni(Marshal.ReadIntPtr(args, IntPtr.Size));
                return AbsoluteExecutable(image) && option == "--tray" &&
                    String.Equals(Path.GetFileName(image), "audio-monitor.exe", StringComparison.OrdinalIgnoreCase);
            } finally { LocalFree(args); }
        }

        // Keep the same instance lock as the application through the file and
        // registration swaps. A launching copy cannot slip into the gap after
        // its predecessor exits, and a not-yet-created HWND is not missed.
        public sealed class InstallGuard : IDisposable {
            private IntPtr handle;
            private bool owned;
            public InstallGuard(string name) {
                handle = CreateMutexW(IntPtr.Zero, false, name);
                if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            public bool TryAcquire() {
                if (owned) return true;
                uint result = WaitForSingleObject(handle, 0);
                if (result == 0 || result == 0x80) { owned = true; return true; }
                if (result == 0x102) return false;
                throw new Win32Exception(Marshal.GetLastWin32Error());
            }
            public void Dispose() {
                if (handle != IntPtr.Zero) {
                    if (owned) ReleaseMutex(handle);
                    CloseHandle(handle);
                    handle = IntPtr.Zero;
                    owned = false;
                }
            }
        }

        public sealed class AppProcess : IDisposable {
            private IntPtr handle;
            private readonly IntPtr window;
            private readonly uint id;
            internal AppProcess(IntPtr handle, IntPtr window, uint id) {
                this.handle = handle; this.window = window; this.id = id;
            }
            public bool WaitForExit(int milliseconds) {
                uint result = WaitForSingleObject(handle, (uint)Math.Max(0, milliseconds));
                if (result == 0xFFFFFFFF) throw new Win32Exception(Marshal.GetLastWin32Error());
                return result == 0;
            }
            public void RequestExit(bool legacy) {
                if (WaitForExit(0)) return;
                uint owner;
                GetWindowThreadProcessId(window, out owner);
                if (owner != id || WindowClass(window) != "AudioMonitorWindow") return;
                uint message = legacy ? 0x0010u : RegisterWindowMessage("AudioMonitor.ExitForUpdate");
                if (message != 0) PostMessage(window, message, IntPtr.Zero, IntPtr.Zero);
            }
            public void Dispose() {
                if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
            }
        }

        public static AppProcess[] FindApps() {
            var result = new List<AppProcess>();
            var seen = new HashSet<uint>();
            uint currentSession;
            if (!ProcessIdToSessionId(GetCurrentProcessId(), out currentSession))
                throw new InvalidOperationException("Cannot determine the installer's Windows session.");
            string currentOwner;
            using (var identity = WindowsIdentity.GetCurrent()) { currentOwner = identity.User.Value; }
            EnumWindows(delegate(IntPtr window, IntPtr context) {
                string windowClass = WindowClass(window);
                if (windowClass != "AudioMonitorWindow") return true;
                uint id, session;
                GetWindowThreadProcessId(window, out id);
                if (seen.Contains(id) || !ProcessIdToSessionId(id, out session) || session != currentSession) return true;
                // A retained query/synchronize handle also prevents PID reuse
                // while shutdown is pending. No terminate access is requested.
                IntPtr process = OpenProcess(0x101000, false, id);
                if (process == IntPtr.Zero) return true;
                try {
                    var image = new StringBuilder(32768);
                    uint length = (uint)image.Capacity;
                    if (!QueryFullProcessImageName(process, 0, image, ref length)) return true;
                    IntPtr token;
                    if (!OpenProcessToken(process, 8, out token)) return true;
                    string owner;
                    try { using (var identity = new WindowsIdentity(token)) { owner = identity.User.Value; } }
                    finally { CloseHandle(token); }
                    if (!MatchesApp(image.ToString(), windowClass, session, owner, currentSession, currentOwner)) return true;
                    result.Add(new AppProcess(process, window, id));
                    seen.Add(id);
                    process = IntPtr.Zero;
                } finally {
                    if (process != IntPtr.Zero) CloseHandle(process);
                }
                return true;
            }, IntPtr.Zero);
            return result.ToArray();
        }
    }
}
'@
    }
}

function Request-AppExit($Guard) {
    Initialize-InstallerWindows
    $deadline = [DateTime]::UtcNow.AddSeconds(10)
    $hadRunningApp = $false
    while (-not $Guard.TryAcquire()) {
        $hadRunningApp = $true
        $remaining = [Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        if ($remaining -eq 0) {
            throw 'Audio Monitor is still starting, stopping, or inaccessible. Choose Exit from its tray menu, then run Install.cmd again.'
        }
        $matching = [AudioMonitorInstaller.Windows]::FindApps()
        try {
            foreach ($process in $matching) { $process.RequestExit($false) }
            foreach ($process in $matching) {
                if (-not $process.WaitForExit([Math]::Min(1500, $remaining))) {
                    # Older versions understand WM_CLOSE; close-to-tray is respected.
                    $process.RequestExit($true)
                    $remaining = [Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
                    if (-not $process.WaitForExit($remaining)) {
                        throw 'Audio Monitor is still running. Choose Exit from its tray menu, then run Install.cmd again.'
                    }
                }
            }
        } finally {
            foreach ($process in $matching) { $process.Dispose() }
        }
        # An existing process may own the mutex before it creates its window.
        Start-Sleep -Milliseconds 50
    }
    return $hadRunningApp
}

function Get-StartupPlan([bool]$ExistingUse, $PreviousRun, [string]$Executable) {
    if ($null -ne $PreviousRun) {
        # Preserve custom launchers/invalid values. Only migrate the app's own
        # command, including a portable copy's existing startup registration.
        if ($PreviousRun.Kind -notin @('String', 'ExpandString') -or $PreviousRun.Value -isnot [string]) {
            return $null
        }
        $command = $PreviousRun.Value
        if ($PreviousRun.Kind -eq 'ExpandString') { $command = [Environment]::ExpandEnvironmentVariables($command) }
        Initialize-InstallerWindows
        if (-not [AudioMonitorInstaller.Windows]::IsStartupCommand($command)) { return $null }
    } elseif ($ExistingUse) {
        # A missing Run entry is an opt-out, even when the saved JSON says true.
        return $null
    }
    $command = '"' + $Executable + '" --tray'
    if ($command.Length -gt 260 -or $Executable.IndexOfAny([char[]]"`"`r`n") -ge 0) {
        throw 'The installation path cannot be registered for Windows startup (invalid or longer than 260 characters).'
    }
    return $command
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

$executable = Join-Path $installDir 'audio-monitor.exe'
$existingUse = (Test-Path -LiteralPath $executable -PathType Leaf) -or
               (Test-Path -LiteralPath $configPath -PathType Leaf)
New-Item -ItemType Directory -Path $installDir -Force | Out-Null
$stage = Assert-SafePath (Join-Path $installDir ('.install-' + [Guid]::NewGuid().ToString('N'))) $installDir
$incoming = Join-Path $stage 'incoming'
$backups = Join-Path $stage 'backups'
$changes = New-Object 'System.Collections.Generic.List[object]'
$runKey = 'HKCU:\Software\Microsoft\Windows\CurrentVersion\Run'
$oldRun = $null
$startupCommand = $null
$runChanged = $false
$shortcut = $null
$oldShortcut = $null
$shortcutChanged = $false
$rollbackFailed = $false
$instanceGuard = $null
try {
    New-Item -ItemType Directory -Path $incoming, $backups -Force | Out-Null
    foreach ($name in $payload) {
        $destination = Assert-SafePath (Join-Path $installDir $name) $installDir
        if (Test-Path -LiteralPath $destination -PathType Container) {
            throw "Cannot replace a directory with a release file: $destination"
        }
        Copy-Item -LiteralPath (Join-Path $source $name) -Destination (Join-Path $incoming $name)
    }
    # TestRoot is a filesystem sandbox only: never inspect/message real apps.
    if (-not $TestRoot) {
        Initialize-InstallerWindows
        $instanceGuard = New-Object 'AudioMonitorInstaller.Windows+InstallGuard' 'Local\AudioMonitorSingleInstance'
        $hadRunningApp = Request-AppExit $instanceGuard
        $existingUse = $existingUse -or $hadRunningApp -or (Test-Path -LiteralPath $executable -PathType Leaf)
    }
    # Shutdown can flush pending settings. Validate the final file and read the
    # live registry only after the previous app has finished saving its state.
    if (Test-Path -LiteralPath $configPath -PathType Leaf) {
        $existingUse = $true
        try { $null = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json }
        catch { throw "Cannot read existing mixer settings. They were left untouched: $configPath" }
    }
    if (-not $SkipRegistration) {
        if (Test-Path -LiteralPath $runKey) {
            $runValues = Get-Item -LiteralPath $runKey
            try {
                if ($runValues.GetValueNames() -contains 'AudioMonitor') {
                    $oldRun = @{ Value = $runValues.GetValue('AudioMonitor', $null, 'DoNotExpandEnvironmentNames');
                                 Kind = $runValues.GetValueKind('AudioMonitor').ToString() }
                }
            } finally { $runValues.Close() }
        }
        $startupCommand = Get-StartupPlan $existingUse $oldRun $executable
    }
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
        if ($null -ne $startupCommand) {
            if (-not (Test-Path -LiteralPath $runKey)) { New-Item -Path $runKey -Force | Out-Null }
            $runChanged = $true
            New-ItemProperty -LiteralPath $runKey -Name AudioMonitor -Value $startupCommand -PropertyType String -Force | Out-Null
        }
        # Do not modify StartupApproved: a Windows/Task Manager disable must
        # survive both upgrades and migration from a portable installation.
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
    try {
        if (-not $rollbackFailed -and (Test-Path -LiteralPath $stage)) {
            $checked = Assert-SafePath $stage $installDir
            Remove-Item -LiteralPath $checked -Recurse -Force
        }
    } finally {
        if ($null -ne $instanceGuard) { $instanceGuard.Dispose() }
    }
}
Write-Host "Installed Audio Monitor: $installDir"
if (-not $SkipRegistration) { Write-Host 'Existing Windows startup choices were preserved; new installs default to starting at sign-in.' }
if (-not $NoLaunch) { Start-Process -FilePath (Join-Path $installDir 'audio-monitor.exe') -WorkingDirectory $installDir }
