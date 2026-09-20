#Requires -Version 5.0
# Filesystem integration and pure policy tests. Never inspect/message running
# apps, register startup, or create shortcuts.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repo = Split-Path -Parent $PSScriptRoot
$temporaryBase = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
$root = Join-Path $temporaryBase ('audio-monitor-installer-test-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $root 'package'
$sandbox = Join-Path $root 'sandbox'
$installation = Join-Path $sandbox 'Programs/Audio Monitor'
$installer = Join-Path $package 'Install.ps1'
$passed = 0

function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:passed++
}

function Install-TestPackage {
    & $installer -TestRoot $sandbox -NoLaunch -SkipRegistration
}

function Expect-Failure([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Check $failed $Message
}

try {
    New-Item -ItemType Directory -Path $package, $sandbox -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo 'packaging/Install.ps1') -Destination $installer

    # Load only the bounded helpers, never dot-source the installer entrypoint.
    # Native identity/command parsing tests do not call FindApps or RequestExit.
    $tokens = $null
    $parseErrors = $null
    $ast = [Management.Automation.Language.Parser]::ParseFile($installer, [ref]$tokens, [ref]$parseErrors)
    Check ($parseErrors.Count -eq 0) 'Installer contains PowerShell syntax errors.'
    foreach ($name in @('Initialize-InstallerWindows', 'Get-StartupPlan')) {
        $definition = $ast.Find({ param($node)
            $node -is [Management.Automation.Language.FunctionDefinitionAst] -and $node.Name -eq $name
        }, $false)
        Check ($null -ne $definition) "Missing installer helper: $name"
        . ([scriptblock]::Create($definition.Extent.Text))
    }
    Initialize-InstallerWindows
    $installedExe = 'C:\Users\Test\AppData\Local\Programs\Audio Monitor\audio-monitor.exe'
    $expectedCommand = '"' + $installedExe + '" --tray'
    Check ((Get-StartupPlan $false $null $installedExe) -eq $expectedCommand) 'Fresh install did not default to startup.'
    Check ($null -eq (Get-StartupPlan $true $null $installedExe)) 'Existing use with a missing Run entry was re-enabled.'
    $portableRun = @{ Kind = 'String'; Value = '"C:\Downloads\Audio Monitor\audio-monitor.exe" --tray' }
    Check ((Get-StartupPlan $true $portableRun $installedExe) -eq $expectedCommand) 'Portable startup path was not migrated.'
    Check ((Get-StartupPlan $false $portableRun $installedExe) -eq $expectedCommand) 'Existing registration required a saved config.'
    $expandedRun = @{ Kind = 'ExpandString'; Value = '"%TEMP%\Audio Monitor\audio-monitor.exe" --tray' }
    Check ((Get-StartupPlan $true $expandedRun $installedExe) -eq $expectedCommand) 'Expandable startup command was not migrated.'
    foreach ($custom in @(
        @{ Kind = 'String'; Value = '"C:\Tools\launcher.exe" --tray' },
        @{ Kind = 'String'; Value = '"C:\Tools\audio-monitor.exe" --custom' },
        @{ Kind = 'String'; Value = '"C:\Tools\audio-monitor.exe" --tray extra' },
        @{ Kind = 'String'; Value = 'audio-monitor.exe --tray' },
        @{ Kind = 'String'; Value = '\audio-monitor.exe --tray' },
        @{ Kind = 'String'; Value = 'C:audio-monitor.exe --tray' },
        @{ Kind = 'DWord'; Value = 1 }
    )) {
        Check ($null -eq (Get-StartupPlan $true $custom $installedExe)) 'Installer overwrote a custom/invalid startup value.'
    }
    $longExecutable = 'C:\' + ('a' * 250) + '\audio-monitor.exe'
    Expect-Failure { Get-StartupPlan $false $null $longExecutable } 'Installer accepted a startup command longer than Windows supports.'
    Check ($null -eq (Get-StartupPlan $true $null $longExecutable)) 'An opted-out startup unnecessarily rejected a long path.'

    $sid = 'S-1-5-21-1000'
    Check ([AudioMonitorInstaller.Windows]::MatchesApp($installedExe, 'AudioMonitorWindow', 1, $sid, 1, $sid)) 'Installed app was not selected.'
    Check ([AudioMonitorInstaller.Windows]::MatchesApp('C:\Downloads\AUDIO-MONITOR.EXE', 'AudioMonitorWindow', 1, $sid, 1, $sid)) 'Portable app was not selected.'
    Check (-not [AudioMonitorInstaller.Windows]::MatchesApp($installedExe, 'OtherWindow', 1, $sid, 1, $sid)) 'Unrelated window was selected.'
    Check (-not [AudioMonitorInstaller.Windows]::MatchesApp('C:\Tools\unrelated.exe', 'AudioMonitorWindow', 1, $sid, 1, $sid)) 'Unrelated executable was selected.'
    Check (-not [AudioMonitorInstaller.Windows]::MatchesApp($installedExe, 'AudioMonitorWindow', 2, $sid, 1, $sid)) 'Another Windows session was selected.'
    Check (-not [AudioMonitorInstaller.Windows]::MatchesApp($installedExe, 'AudioMonitorWindow', 1, 'S-1-5-21-2000', 1, $sid)) 'Another Windows user was selected.'
    Check (-not [AudioMonitorInstaller.Windows]::MatchesApp($installedExe, 'AudioMonitorWindow', 1, '', 1, $sid)) 'An unidentified Windows user was selected.'
    Check (-not [AudioMonitorInstaller.Windows]::MatchesApp('audio-monitor.exe', 'AudioMonitorWindow', 1, $sid, 1, $sid)) 'An unverified relative image path was selected.'

    # A worker holds only a unique test mutex, without a window. This models
    # a portable copy that is still starting before its HWND exists.
    if (-not ('AudioMonitorInstallerTest.Owner' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Threading;
namespace AudioMonitorInstallerTest {
    public sealed class Owner : IDisposable {
        private readonly ManualResetEvent ready = new ManualResetEvent(false);
        private readonly ManualResetEvent release = new ManualResetEvent(false);
        private readonly Thread thread;
        public Owner(string name) {
            thread = new Thread(delegate() {
                using (var mutex = new Mutex(false, name)) {
                    mutex.WaitOne();
                    ready.Set();
                    release.WaitOne();
                    mutex.ReleaseMutex();
                }
            });
            thread.Start();
            ready.WaitOne();
        }
        public void Dispose() { release.Set(); thread.Join(); ready.Dispose(); release.Dispose(); }
    }
}
'@
    }
    $mutexName = 'Local\AudioMonitorInstallerTest-' + [Guid]::NewGuid().ToString('N')
    $testOwner = New-Object 'AudioMonitorInstallerTest.Owner' $mutexName
    $guard = New-Object 'AudioMonitorInstaller.Windows+InstallGuard' $mutexName
    try {
        Check (-not $guard.TryAcquire()) 'Installer ignored a starting process with no window.'
        $testOwner.Dispose()
        $testOwner = $null
        Check ($guard.TryAcquire()) 'Installer could not acquire the lock after the old app exited.'
        Check ($guard.TryAcquire()) 'Checking acquired installation lock lost ownership.'
        # Opening a conflicting kernel object name must fail closed.
        $eventName = 'Local\AudioMonitorInstallerTest-' + [Guid]::NewGuid().ToString('N')
        $testEvent = New-Object Threading.EventWaitHandle($false, [Threading.EventResetMode]::ManualReset, $eventName)
        try {
            Expect-Failure { New-Object 'AudioMonitorInstaller.Windows+InstallGuard' $eventName } 'Installer ignored a mutex creation error.'
        } finally { $testEvent.Dispose() }
    } finally {
        if ($null -ne $testOwner) { $testOwner.Dispose() }
        $guard.Dispose()
    }

    [IO.File]::WriteAllText((Join-Path $package 'audio-monitor.exe'), 'fake GUI build one')
    [IO.File]::WriteAllText((Join-Path $package 'LICENSE'), 'test license')

    Expect-Failure { & $installer -TestRoot $sandbox -NoLaunch } 'Test override allowed registration.'
    Expect-Failure { & $installer -TestRoot $sandbox -SkipRegistration } 'Test override allowed a launch.'
    Expect-Failure { & $installer -TestRoot $temporaryBase -NoLaunch -SkipRegistration } 'TEMP itself was accepted as a test root.'
    Check (-not (Test-Path -LiteralPath $installation)) 'Rejected invocations created an installation.'

    Install-TestPackage
    Check ((Get-Content -LiteralPath (Join-Path $installation 'audio-monitor.exe') -Raw) -eq 'fake GUI build one') 'Fresh GUI was not installed.'
    Check ((Get-ChildItem -LiteralPath $installation -Force -Directory | Measure-Object).Count -eq 0) 'Fresh install left staging files.'

    $configDirectory = Join-Path $sandbox 'Roaming/audio-monitor'
    New-Item -ItemType Directory -Path $configDirectory -Force | Out-Null
    $configPath = Join-Path $configDirectory 'config.json'
    $configContents = '{ "startWithWindows": false, "sources": [{ "label": "My microphone" }], "futureSetting": 123 }'
    [IO.File]::WriteAllText($configPath, $configContents)
    $configHash = (Get-FileHash -LiteralPath $configPath).Hash
    [IO.File]::WriteAllText((Join-Path $installation 'personal-notes.txt'), 'keep this file')
    [IO.File]::WriteAllText((Join-Path $package 'audio-monitor.exe'), 'fake GUI build two')
    [IO.File]::WriteAllText((Join-Path $package 'audiomon-cli.exe'), 'fake CLI build two')
    Install-TestPackage
    Check ((Get-Content -LiteralPath (Join-Path $installation 'audio-monitor.exe') -Raw) -eq 'fake GUI build two') 'Update did not replace the GUI.'
    Check ((Get-Content -LiteralPath (Join-Path $installation 'audiomon-cli.exe') -Raw) -eq 'fake CLI build two') 'Optional CLI was not installed.'
    Check ((Get-FileHash -LiteralPath $configPath).Hash -eq $configHash) 'Update rewrote mixer settings.'
    Check ((Get-Content -LiteralPath (Join-Path $installation 'personal-notes.txt') -Raw) -eq 'keep this file') 'Update deleted an unrelated file.'

    # The GUI swap succeeds, then a locked LICENSE causes a later swap to fail.
    # Verify that the previous GUI is restored, not a mixed or partial update.
    [IO.File]::WriteAllText((Join-Path $package 'audio-monitor.exe'), 'fake GUI build three')
    $lockedLicense = [IO.File]::Open((Join-Path $installation 'LICENSE'), [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
    try { Expect-Failure { Install-TestPackage } 'Replacing a locked file unexpectedly succeeded.' }
    finally { $lockedLicense.Dispose() }
    Check ((Get-Content -LiteralPath (Join-Path $installation 'audio-monitor.exe') -Raw) -eq 'fake GUI build two') 'Failed update did not roll back the GUI.'
    Check ((Get-Content -LiteralPath (Join-Path $installation 'audiomon-cli.exe') -Raw) -eq 'fake CLI build two') 'Failed update changed the CLI.'
    Check ((Get-FileHash -LiteralPath $configPath).Hash -eq $configHash) 'Failed update changed settings.'
    Check ((Get-ChildItem -LiteralPath $installation -Force -Directory | Measure-Object).Count -eq 0) 'Successful rollback left staging files.'

    [IO.File]::WriteAllText($configPath, 'malformed config')
    Expect-Failure { Install-TestPackage } 'Malformed settings were silently replaced or ignored.'
    Check ((Get-Content -LiteralPath $configPath -Raw) -eq 'malformed config') 'Malformed settings were modified.'
    Check ((Get-Content -LiteralPath (Join-Path $installation 'audio-monitor.exe') -Raw) -eq 'fake GUI build two') 'Config validation failed after changing binaries.'
    Check ((Get-ChildItem -LiteralPath $installation -Force -Directory | Measure-Object).Count -eq 0) 'Config validation failure left staging files.'
    [IO.File]::WriteAllText($configPath, $configContents)

    Remove-Item -LiteralPath (Join-Path $package 'LICENSE')
    Expect-Failure { Install-TestPackage } 'An incomplete release was installed.'
    Check ((Get-Content -LiteralPath (Join-Path $installation 'audio-monitor.exe') -Raw) -eq 'fake GUI build two') 'Incomplete release changed existing installation.'
    [IO.File]::WriteAllText((Join-Path $package 'LICENSE'), 'test license')

    # Junctions do not require elevation on Windows. An alternate test root
    # must not let writes escape through its Programs directory.
    $junctionSandbox = Join-Path $root 'junction-sandbox'
    $outside = Join-Path $root 'outside'
    New-Item -ItemType Directory -Path $junctionSandbox, $outside -Force | Out-Null
    $junction = Join-Path $junctionSandbox 'Programs'
    New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
    try {
        Expect-Failure { & $installer -TestRoot $junctionSandbox -NoLaunch -SkipRegistration } 'Installer followed a directory junction.'
        Check ((Get-ChildItem -LiteralPath $outside -Force | Measure-Object).Count -eq 0) 'Junction target was modified.'
    } finally {
        # Directory.Delete removes just the junction; never recurse through it.
        [IO.Directory]::Delete($junction)
    }
    Write-Host "Installer integration tests passed ($passed assertions)."
} finally {
    $resolvedRoot = [IO.Path]::GetFullPath($root)
    if (-not $resolvedRoot.StartsWith($temporaryBase + '\audio-monitor-installer-test-', [StringComparison]::OrdinalIgnoreCase)) {
        throw "Unsafe test cleanup path: $resolvedRoot"
    }
    if (Test-Path -LiteralPath $resolvedRoot) { Remove-Item -LiteralPath $resolvedRoot -Recurse -Force }
}
