#Requires -Version 5.0
# ZIP preparation, cleanup, and cancelled handoff: never launch/stop the app or
# read/write its registry/configuration. Child PowerShell processes belong only
# to this test. Both Windows PowerShell 5.1 and PowerShell 7 run this.
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$repo = Split-Path -Parent $PSScriptRoot
$helper = Join-Path $repo 'packaging/Update.ps1'
$passed = 0
$stages = New-Object 'System.Collections.Generic.List[string]'
$allowedFiles = @('audio-monitor.exe', 'audiomon-cli.exe', 'LICENSE', 'Install.ps1', 'Install.cmd')
$requiredFiles = @('audio-monitor.exe', 'LICENSE', 'Install.ps1', 'Install.cmd')

function Check([bool]$Condition, [string]$Message) {
    if (-not $Condition) { throw $Message }
    $script:passed++
}
function Expect-Failure([scriptblock]$Action, [string]$Message) {
    $failed = $false
    try { & $Action } catch { $failed = $true }
    Check $failed $Message
}

$tokens = $null
$parseErrors = $null
$ast = [Management.Automation.Language.Parser]::ParseFile($helper, [ref]$tokens, [ref]$parseErrors)
Check ($parseErrors.Count -eq 0) 'Update helper contains PowerShell syntax errors.'
foreach ($definition in $ast.FindAll({ param($node)
    $node -is [Management.Automation.Language.FunctionDefinitionAst]
}, $false)) {
    . ([scriptblock]::Create($definition.Extent.Text))
}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

function New-Stage {
    $path = Join-Path ([IO.Path]::GetTempPath()) ('AudioMonitorUpdate-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $path | Out-Null
    $stages.Add($path)
    return $path
}

function New-Fixture([string[]]$Names = $requiredFiles, [string]$EmptyName = '', [string]$LinkName = '') {
    $stage = New-Stage
    $archive = [IO.Compression.ZipFile]::Open((Join-Path $stage 'update.zip'), [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($name in $Names) {
            $entry = $archive.CreateEntry($name)
            if ($name -eq $LinkName) { $entry.ExternalAttributes = -1585446912 } # Unix symlink 0xA1800000
            $stream = $entry.Open()
            try {
                if ($name -ne $EmptyName) {
                    $content = if ($name -eq 'Install.ps1') { "throw 'Test sentinel: no installation is allowed.'" } else { "fixture for $name" }
                    $bytes = [Text.Encoding]::UTF8.GetBytes($content)
                    $stream.Write($bytes, 0, $bytes.Length)
                }
            } finally { $stream.Dispose() }
        }
    } finally { $archive.Dispose() }
    return $stage
}

try {
    $stage = New-Fixture
    Check ((Get-ValidatedStage $stage) -eq $stage) 'Private temporary directory was rejected.'
    Expand-UpdatePackage $stage
    $payload = Assert-PreparedPayload $stage
    Check ((Get-ChildItem -LiteralPath $payload -File).Count -eq 4) 'Required ZIP files were not extracted.'
    Check ((Get-Content -LiteralPath (Join-Path $payload 'LICENSE') -Raw) -eq 'fixture for LICENSE') 'Extraction changed file content.'
    Expect-Failure { Expand-UpdatePackage $stage } 'Preparation overwrote an existing payload.'
    [IO.File]::WriteAllText((Join-Path $payload 'Install.ps1'), 'changed after preparation')
    Expect-Failure { Assert-PreparedPayload $stage } 'Changed installer script passed manifest verification.'
    Remove-UpdateStage $stage
    Check (-not (Test-Path -LiteralPath $stage)) 'Verified staging directory was not cleaned up.'

    $stage = New-Fixture $allowedFiles
    Expand-UpdatePackage $stage
    Check ((Get-ChildItem -LiteralPath (Assert-PreparedPayload $stage) -File).Count -eq 5) 'Optional CLI was rejected.'
    $manifestPath = Join-Path $stage 'prepared.json'
    $manifest = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($manifestPath))
    $manifest[0].name = '../audio-monitor.exe'
    [IO.File]::WriteAllText($manifestPath, (ConvertTo-Json -InputObject $manifest))
    Expect-Failure { Assert-PreparedPayload $stage } 'A path-traversing manifest was accepted.'
    Remove-UpdateStage $stage

    foreach ($names in @(
        @('../audio-monitor.exe', 'LICENSE', 'Install.ps1', 'Install.cmd'),
        @('folder/audio-monitor.exe', 'LICENSE', 'Install.ps1', 'Install.cmd'),
        @('C:\audio-monitor.exe', 'LICENSE', 'Install.ps1', 'Install.cmd'),
        @('audio-monitor.exe:stream', 'LICENSE', 'Install.ps1', 'Install.cmd'),
        @('audio-monitor.exe', 'LICENSE', 'Install.ps1', 'Install.ps1'),
        @('audio-monitor.exe', 'LICENSE', 'Install.ps1', 'audiomon-cli.exe'),
        @('audio-monitor.exe', 'LICENSE', 'Install.ps1', 'Install.cmd', 'unknown.exe')
    )) {
        $stage = New-Fixture $names
        Expect-Failure { Expand-UpdatePackage $stage } 'Unsafe/incomplete ZIP was accepted.'
        Check (-not (Test-Path -LiteralPath (Join-Path $stage 'payload'))) 'Invalid ZIP wrote a payload before validation.'
        Remove-UpdateStage $stage
    }
    $stage = New-Fixture -EmptyName 'audio-monitor.exe'
    Expect-Failure { Expand-UpdatePackage $stage } 'Empty executable was accepted.'
    Remove-UpdateStage $stage
    $stage = New-Fixture -LinkName 'audio-monitor.exe'
    Expect-Failure { Expand-UpdatePackage $stage } 'ZIP symlink was accepted.'
    Remove-UpdateStage $stage

    $stage = New-Fixture
    Expand-UpdatePackage $stage
    [IO.File]::WriteAllText((Join-Path $stage 'unexpected.txt'), 'must survive')
    Expect-Failure { Remove-UpdateStage $stage } 'Cleanup removed unexpected staging contents.'
    Check ((Get-Content -LiteralPath (Join-Path $stage 'unexpected.txt') -Raw) -eq 'must survive') 'Cleanup changed an unrelated file.'
    [IO.File]::Delete((Join-Path $stage 'unexpected.txt'))
    Remove-UpdateStage $stage

    $stage = New-Fixture
    $outside = New-Stage
    $junction = Join-Path $stage 'payload'
    New-Item -ItemType Junction -Path $junction -Target $outside | Out-Null
    try {
        Expect-Failure { Remove-UpdateStage $stage } 'Cleanup followed a junction.'
        Check (Test-Path -LiteralPath $outside -PathType Container) 'Cleanup removed the junction target.'
    } finally { [IO.Directory]::Delete($junction) }
    Remove-UpdateStage $stage
    Remove-UpdateStage $outside
    Expect-Failure { Get-ValidatedStage ([IO.Path]::GetTempPath()) } 'TEMP itself was accepted as a staging directory.'
    Expect-Failure { Get-ValidatedStage (Join-Path $repo 'AudioMonitorUpdate-00000000000000000000000000000000') } 'A repository path was accepted as staging.'

    # Run the actual preparation entrypoint, including transcript/manifest
    # handling, before the separate cancelled-handoff test below.
    $stage = New-Fixture
    $powershell = (Get-Process -Id $PID).Path
    & $powershell -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File $helper -Mode Prepare -Directory $stage
    Check ($LASTEXITCODE -eq 0) 'Preparation entrypoint failed.'
    Check (Test-Path -LiteralPath (Join-Path $stage 'prepared.json')) 'Preparation entrypoint did not create its manifest.'
    Check (Test-Path -LiteralPath (Join-Path $stage 'update.log')) 'Preparation entrypoint did not create a log.'
    $null = Assert-PreparedPayload $stage
    Remove-UpdateStage $stage

    # A helper may signal readiness only after retaining the exact parent
    # process. Cancelling that handoff must leave the parent alive and must
    # never invoke even this inert, always-throwing fixture installer.
    $stage = New-Fixture
    Expand-UpdatePackage $stage
    $token = [Guid]::NewGuid().ToString('N')
    $readyName = 'Local\AudioMonitorUpdateReady-' + $token
    $cancelName = 'Local\AudioMonitorUpdateCancel-' + $token
    $ready = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $readyName)
    $cancel = [Threading.EventWaitHandle]::new($false, [Threading.EventResetMode]::ManualReset, $cancelName)
    $testParent = $null
    $testHelper = $null
    try {
        $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes('Start-Sleep -Seconds 45'))
        $testParent = Start-Process -FilePath $powershell -ArgumentList @('-NoProfile', '-NonInteractive', '-EncodedCommand', $encoded) -WindowStyle Hidden -PassThru
        $started = $testParent.StartTime.ToUniversalTime().ToFileTimeUtc()
        $arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $helper + '"'),
            '-Mode', 'Apply', '-Directory', ('"' + $stage + '"'), '-ParentProcessId', $testParent.Id,
            '-ParentStartedFileTime', $started, '-OriginalExecutable', ('"' + $powershell + '"'),
            '-ReadyEvent', $readyName, '-CancelEvent', $cancelName)
        $testHelper = Start-Process -FilePath $powershell -ArgumentList $arguments -WindowStyle Hidden -PassThru
        Check ($ready.WaitOne(10000)) 'Helper did not validate the parent and signal readiness.'
        Check (-not $testParent.HasExited) 'Readiness stopped the parent process.'
        $null = $cancel.Set()
        Check ($testHelper.WaitForExit(10000)) 'Cancelled helper did not exit.'
        Check ($testHelper.ExitCode -ne 0) 'Cancelled helper claimed success.'
        Check (-not $testParent.HasExited) 'Cancelled handoff stopped the parent process.'
        Check ((Get-Content -LiteralPath (Join-Path $stage 'error.txt') -Raw) -match 'cancelled') 'Cancellation ran the fixture installer instead of stopping safely.'
    } finally {
        $null = $cancel.Set()
        if ($testHelper) {
            if (-not $testHelper.WaitForExit(2000)) { $testHelper.Kill(); $testHelper.WaitForExit() }
            $testHelper.Dispose()
        }
        if ($testParent) {
            if (-not $testParent.HasExited) { $testParent.Kill(); $testParent.WaitForExit() }
            $testParent.Dispose()
        }
        $ready.Dispose()
        $cancel.Dispose()
    }
    Remove-UpdateStage $stage
    Write-Host "Automatic update helper tests passed ($passed assertions)."
} finally {
    foreach ($stage in $stages) {
        if (Test-Path -LiteralPath $stage) {
            # Same flat allowlist as production. If a failed test leaves an
            # unknown item or link, retain it rather than recurse through it.
            try { Remove-UpdateStage $stage } catch { Write-Warning "Test stage retained: $stage" }
        }
    }
}
