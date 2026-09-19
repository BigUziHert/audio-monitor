#Requires -Version 5.0
# Filesystem integration tests. Never register startup or create shortcuts.
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
