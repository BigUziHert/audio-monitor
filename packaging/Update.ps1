#Requires -Version 5.0
# Embedded in the application. Runs only inside its private temporary directory.
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][ValidateSet('Prepare', 'Apply')][string]$Mode,
    [Parameter(Mandatory = $true)][string]$Directory,
    [int]$ParentProcessId,
    [long]$ParentStartedFileTime,
    [string]$OriginalExecutable,
    [string]$ReadyEvent,
    [string]$CancelEvent
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version 2.0
$allowedFiles = @('audio-monitor.exe', 'audiomon-cli.exe', 'LICENSE', 'Install.ps1', 'Install.cmd')
$requiredFiles = @('audio-monitor.exe', 'LICENSE', 'Install.ps1', 'Install.cmd')

function Assert-NoReparsePoint([string]$Path) {
    $cursor = [IO.Path]::GetFullPath($Path)
    while ($cursor) {
        if ((Test-Path -LiteralPath $cursor) -and
            ((Get-Item -LiteralPath $cursor -Force).Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Update staging cannot contain a symbolic link or junction: $cursor"
        }
        $next = Split-Path -Parent $cursor
        if ($next -eq $cursor) { break }
        $cursor = $next
    }
}

function Get-ValidatedStage([string]$Path) {
    $full = [IO.Path]::GetFullPath($Path).TrimEnd('\')
    $temporary = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\')
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals((Split-Path -Parent $full), $temporary) -or
        (Split-Path -Leaf $full) -notmatch '^AudioMonitorUpdate-[0-9a-f]{32}$') {
        throw 'Invalid private update directory.'
    }
    Assert-NoReparsePoint $full
    if (-not (Test-Path -LiteralPath $full -PathType Container)) { throw 'The private update directory is missing.' }
    return $full
}

function Assert-RegularFile([string]$Path, [long]$Maximum = 67108864) {
    Assert-NoReparsePoint $Path
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing update file: $Path" }
    $file = Get-Item -LiteralPath $Path -Force
    if ($file.Length -le 0 -or $file.Length -gt $Maximum) { throw "Invalid update file size: $Path" }
}

function Get-UpdateFileHash([string]$Path) {
    # Get-FileHash is a script-module command in Windows PowerShell 5.1. A
    # parent launched from PowerShell 7 can inherit a PSModulePath that hides
    # that module, so integrity checks use the framework directly.
    $stream = [IO.File]::OpenRead($Path)
    try {
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '') }
        finally { $algorithm.Dispose() }
    } finally { $stream.Dispose() }
}

function Expand-UpdatePackage([string]$Stage) {
    $archivePath = Join-Path $Stage 'update.zip'
    Assert-RegularFile $archivePath
    $payload = Join-Path $Stage 'payload'
    if (Test-Path -LiteralPath $payload) { throw 'An update payload already exists in this staging directory.' }
    Add-Type -AssemblyName System.IO.Compression
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        if ($archive.Entries.Count -lt 4 -or $archive.Entries.Count -gt 5) { throw 'Unexpected number of files in the update ZIP.' }
        $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
        [long]$total = 0
        foreach ($entry in $archive.Entries) {
            if ($allowedFiles -cnotcontains $entry.FullName -or -not $seen.Add($entry.FullName)) {
                throw "Unexpected, nested, or duplicate file in the update ZIP: $($entry.FullName)"
            }
            $unixKind = ($entry.ExternalAttributes -shr 16) -band 0xF000
            if (($unixKind -ne 0 -and $unixKind -ne 0x8000) -or ($entry.ExternalAttributes -band 0x410)) {
                throw "The update ZIP contains a link or directory: $($entry.FullName)"
            }
            if ($entry.Length -le 0 -or $entry.Length -gt 67108864) { throw 'Invalid expanded update file size.' }
            $total += $entry.Length
            if ($total -gt 134217728) { throw 'The expanded update is too large.' }
        }
        foreach ($name in $requiredFiles) {
            if (-not $seen.Contains($name)) { throw "The update ZIP is missing $name." }
        }
        New-Item -ItemType Directory -Path $payload | Out-Null
        $manifest = @()
        $buffer = New-Object byte[] 65536
        foreach ($entry in $archive.Entries) {
            $destination = Join-Path $payload $entry.FullName
            $sourceStream = $entry.Open()
            try {
                $targetStream = [IO.File]::Open($destination, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
                try {
                    [long]$written = 0
                    while (($count = $sourceStream.Read($buffer, 0, $buffer.Length)) -gt 0) {
                        $written += $count
                        if ($written -gt $entry.Length) { throw 'Expanded update data exceeds its declared size.' }
                        $targetStream.Write($buffer, 0, $count)
                    }
                    if ($written -ne $entry.Length) { throw 'The update ZIP is truncated.' }
                } finally { $targetStream.Dispose() }
            } finally { $sourceStream.Dispose() }
            $manifest += [PSCustomObject]@{ name = $entry.FullName; size = $written; sha256 = Get-UpdateFileHash $destination }
        }
        $json = ConvertTo-Json -InputObject @($manifest) -Depth 3
        [IO.File]::WriteAllText((Join-Path $Stage 'prepared.json'), $json, (New-Object Text.UTF8Encoding $false))
    } finally { $archive.Dispose() }
}

function Assert-PreparedPayload([string]$Stage) {
    $manifestPath = Join-Path $Stage 'prepared.json'
    Assert-RegularFile $manifestPath 16384
    # PowerShell 5.1 emits a JSON array as one pipeline object, while 7 unwraps
    # it. Direct assignment preserves the actual array in both engines.
    $manifest = ConvertFrom-Json -InputObject ([IO.File]::ReadAllText($manifestPath))
    if ($manifest -isnot [Array] -or $manifest.Count -lt 4 -or $manifest.Count -gt 5) { throw 'Invalid prepared update manifest.' }
    $payload = Join-Path $Stage 'payload'
    Assert-NoReparsePoint $payload
    $actual = @(Get-ChildItem -LiteralPath $payload -Force)
    if ($actual.Count -ne $manifest.Count) { throw 'The prepared update files have changed.' }
    $seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $manifest) {
        if ($allowedFiles -cnotcontains $entry.name -or -not $seen.Add($entry.name) -or
            $entry.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid prepared update file.' }
        $path = Join-Path $payload $entry.name
        Assert-RegularFile $path
        if ((Get-Item -LiteralPath $path).Length -ne $entry.size -or
            (Get-UpdateFileHash $path) -ne $entry.sha256) {
            throw "The prepared update changed or was damaged: $($entry.name)"
        }
    }
    foreach ($name in $requiredFiles) {
        if (-not $seen.Contains($name)) { throw "The prepared update is missing $name." }
    }
    return $payload
}

function Remove-UpdateStage([string]$Stage) {
    $checked = Get-ValidatedStage $Stage
    # Delete only the known, flat staging files. Never recursively delete an
    # unexpected directory, a junction, or a path supplied by archive contents.
    $rootFiles = @('update.zip', 'Update.ps1', 'prepared.json', 'update.log', 'error.txt')
    foreach ($item in @(Get-ChildItem -LiteralPath $checked -Force)) {
        if ($item.Name -eq 'payload' -and $item.PSIsContainer) {
            Assert-NoReparsePoint $item.FullName
            foreach ($file in @(Get-ChildItem -LiteralPath $item.FullName -Force)) {
                if ($allowedFiles -cnotcontains $file.Name -or $file.PSIsContainer) { throw 'Unexpected file in update cleanup.' }
                Assert-NoReparsePoint $file.FullName
            }
        } elseif ($rootFiles -cnotcontains $item.Name -or $item.PSIsContainer) { throw 'Unexpected file in update cleanup.' }
        Assert-NoReparsePoint $item.FullName
    }
    $payload = Join-Path $checked 'payload'
    if (Test-Path -LiteralPath $payload) {
        foreach ($name in $allowedFiles) {
            $path = Join-Path $payload $name
            if (Test-Path -LiteralPath $path) { [IO.File]::Delete($path) }
        }
        [IO.Directory]::Delete($payload)
    }
    foreach ($name in $rootFiles) {
        $path = Join-Path $checked $name
        if (Test-Path -LiteralPath $path) { [IO.File]::Delete($path) }
    }
    [IO.Directory]::Delete($checked)
}

$stage = $null
$parent = $null
$ready = $null
$cancel = $null
$parentExited = $false
$applied = $false
$transcribing = $false
try {
    $stage = Get-ValidatedStage $Directory
    Start-Transcript -LiteralPath (Join-Path $stage 'update.log') -Append -Force | Out-Null
    $transcribing = $true
    if ($Mode -eq 'Prepare') {
        Expand-UpdatePackage $stage
        $null = Assert-PreparedPayload $stage
    } else {
        if ($ReadyEvent -notmatch '^Local\\AudioMonitorUpdateReady-[0-9a-f]{32}$' -or
            $CancelEvent -notmatch '^Local\\AudioMonitorUpdateCancel-[0-9a-f]{32}$' -or
            $ParentProcessId -le 0 -or $ParentStartedFileTime -le 0) { throw 'Invalid installer handoff.' }
        $ready = [Threading.EventWaitHandle]::OpenExisting($ReadyEvent)
        $cancel = [Threading.EventWaitHandle]::OpenExisting($CancelEvent)
        $payload = Assert-PreparedPayload $stage
        $parent = [Diagnostics.Process]::GetProcessById($ParentProcessId)
        # Force the handle open before checking identity, retaining the original
        # process even if its numeric PID is reused after it exits.
        $null = $parent.Handle
        if ($parent.StartTime.ToUniversalTime().ToFileTimeUtc() -ne $ParentStartedFileTime -or
            -not [StringComparer]::OrdinalIgnoreCase.Equals($parent.MainModule.FileName, $OriginalExecutable)) {
            throw 'The application process no longer matches the update request.'
        }
        if ($cancel.WaitOne(0)) { throw 'The update handoff was cancelled.' }
        $null = $ready.Set()
        $deadline = [DateTime]::UtcNow.AddSeconds(60)
        while (-not $parent.WaitForExit(100)) {
            if ($cancel.WaitOne(0)) { throw 'The update handoff was cancelled.' }
            if ([DateTime]::UtcNow -ge $deadline) { throw 'Audio Monitor did not exit in time. Close it and try the update again.' }
        }
        $parentExited = $true
        if ($cancel.WaitOne(0)) { throw 'The update handoff was cancelled.' }
        $payload = Assert-PreparedPayload $stage
        $LASTEXITCODE = 0
        & (Join-Path $payload 'Install.ps1') -NoLaunch
        if (-not $? -or $LASTEXITCODE -ne 0) { throw "The installer failed with exit code $LASTEXITCODE." }
        $installed = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Programs/Audio Monitor/audio-monitor.exe'
        Assert-RegularFile $installed
        Start-Process -FilePath $installed -WorkingDirectory (Split-Path -Parent $installed)
        $applied = $true
    }
} catch {
    $message = $_.Exception.Message
    Write-Host ("Update failed: " + $message)
    if ($stage) {
        try { [IO.File]::WriteAllText((Join-Path $stage 'error.txt'), $message, (New-Object Text.UTF8Encoding $false)) } catch {}
    }
    if ($Mode -eq 'Apply' -and $parentExited) {
        $recovered = $false
        try {
            if (Test-Path -LiteralPath $OriginalExecutable -PathType Leaf) {
                Start-Process -FilePath $OriginalExecutable -WorkingDirectory (Split-Path -Parent $OriginalExecutable)
                $recovered = $true
            }
        } catch {}
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $recovery = if ($recovered) { 'Your previous app was reopened where possible.' } else { 'Open Audio Monitor again after checking the error.' }
            $null = [Windows.Forms.MessageBox]::Show("The update could not finish. $recovery`r`n`r`n$message`r`n`r`nDetails were kept at $stage\update.log. You can extract a fresh release and run Install.cmd to retry.", 'Audio Monitor update', 'OK', 'Error')
        } catch {}
    }
    exit 1
} finally {
    if ($parent) { $parent.Dispose() }
    if ($ready) { $ready.Dispose() }
    if ($cancel) { $cancel.Dispose() }
    if ($transcribing) { try { Stop-Transcript | Out-Null } catch {} }
}
if ($applied) {
    try { Remove-UpdateStage $stage } catch { }
}
exit 0
