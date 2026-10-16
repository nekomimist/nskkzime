#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9-]{0,63}$')][string]$Name,
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$')][string]$RunName = ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)),
    [ValidateRange(30,600)][int]$TimeoutSeconds = 330,
    [switch]$WithZenz,
    [string]$BundleDirectory = $PSScriptRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'installation.psm1') -Force
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -or
    -not [Environment]::UserInteractive -or [Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0) {
    throw 'Run input probes unelevated in the signed-in user desktop.'
}
$bundle = [IO.Path]::GetFullPath($BundleDirectory)
$stage = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) "nskkzime-arm64-registration-tests/$Name"
$journalPath = Join-Path $stage 'registration-test.json'
Assert-NskkzimeProtectedPath $stage
Assert-NskkzimeProtectedPath $journalPath
$journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json -AsHashtable
$reports = Join-Path $bundle "reports/registration-$Name"
if ($journal.format -cne 'nskkzime-arm64-registration-test-v1' -or $journal.phase -cne 'registered' -or
    $journal.identity.user_sid -cne $identity.User.Value -or $journal.identity.reports -cne $reports) { throw 'Test registration or user identity differs.' }
foreach ($image in $journal.identity.images) {
    if ($image.path -cnotmatch '^bin/(x86/)?[a-z0-9_-]+\.(dll|exe)$') { throw 'Invalid registered image path.' }
    $file = Join-Path $stage $image.path
    Assert-NskkzimeProtectedPath ([IO.Path]::GetDirectoryName($file))
    Assert-NskkzimeProtectedPath $file
    if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $image.sha256) { throw 'Registered test image changed.' }
}
$inputReports = Join-Path $reports ('input/' + $RunName)
if (Test-Path -LiteralPath $inputReports) { throw 'Input test reports already exist. Choose a new RunName.' }
$dictionary = Join-Path $bundle 'source/tests/fixtures/basic.utf8.skk'
$server = $null; $model = $null; $serverRuns = @()
if ($WithZenz) {
    $manifest = Get-Content -LiteralPath (Join-Path $bundle 'test-images.json') -Raw | ConvertFrom-Json -AsHashtable
    $entry = @($manifest.images | Where-Object path -ceq 'build/Release/nkkz-ime-server.exe')
    if ($manifest.format -cne 'nskkzime-arm64x-tests-v1' -or $entry.Count -ne 1 -or $entry[0].machine -ne 0xaa64 -or
        $manifest.model_sha256 -cne '29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673') { throw 'Unexpected inference test manifest.' }
    $sourceServer = Join-Path $bundle $entry[0].path
    $model = Join-Path $bundle 'source/out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf'
    if ((Get-FileHash -LiteralPath $sourceServer -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry[0].sha256 -or
        (Get-FileHash -LiteralPath $model -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifest.model_sha256) { throw 'Inference server or model changed.' }
    $dictionary = Join-Path $bundle 'source/tests/fixtures/native-zenz.utf8.skk'
    # Server identity includes its path, not the INI path. Each run needs its own
    # executable path so it cannot share an installed or another test endpoint.
    $serverDirectory = Join-Path $inputReports 'server'
    New-Item -ItemType Directory -Path $serverDirectory -Force | Out-Null
    $server = Join-Path $serverDirectory 'nkkz-ime-server.exe'
    Copy-Item -LiteralPath $sourceServer -Destination $server
    if ((Get-FileHash -LiteralPath $server -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry[0].sha256) { throw 'Copied test server changed.' }
}
function Get-TestServers {
    foreach ($process in [Diagnostics.Process]::GetProcessesByName('nkkz-ime-server')) {
        try {
            if ($process.SessionId -ne [Diagnostics.Process]::GetCurrentProcess().SessionId -or $process.HasExited) { continue }
            if ([string]::Equals($process.MainModule.FileName,$server,[StringComparison]::OrdinalIgnoreCase)) {
                [ordered]@{pid=$process.Id;path=$process.MainModule.FileName;started=$process.StartTime.ToUniversalTime().ToString('o');machine=0xaa64}
            }
        } catch { if (-not $process.HasExited) { throw } }
        finally { $process.Dispose() }
    }
}
$results = @()
try {
foreach ($architecture in @('arm64','x64','arm64ec','x86')) {
    $directory = Join-Path $inputReports $architecture
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    if ($WithZenz) { New-Item -ItemType Directory -Path (Join-Path $directory 'tsf') | Out-Null }
    $report = Join-Path $directory 'probe.txt'
    $executable = if ($architecture -eq 'x86') { Join-Path $stage 'bin/x86/ime_probe.exe' } else { Join-Path $stage "bin/ime_probe-$architecture.exe" }
    $expectedDll = if ($architecture -eq 'x86') { $journal.x86 } else { $journal.native }
    $start = [Diagnostics.ProcessStartInfo]::new($executable)
    $start.UseShellExecute = $false; $start.WorkingDirectory = $directory
    $start.ArgumentList.Add($(if ($WithZenz) { '--self-test-zenz' } else { '--self-test' }))
    $start.ArgumentList.Add($dictionary)
    if ($WithZenz) { $start.ArgumentList.Add($server); $start.ArgumentList.Add($model) }
    $start.ArgumentList.Add($report)
    $process = [Diagnostics.Process]::Start($start)
    try {
        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) { throw "The $architecture input probe timed out waiting for foreground or input." }
        $text = if (Test-Path -LiteralPath $report) { [IO.File]::ReadAllText($report) } else { '' }
        $dllLine = @($text -split '\r?\n' | Where-Object { $_.StartsWith('dll=') })
        if ($process.ExitCode -ne 0 -or $text -notmatch "(?m)^host=$architecture\r?$" -or
            $text -notmatch '(?m)^activation=registered-profile\r?$' -or $dllLine.Count -ne 1 -or
            -not [string]::Equals($dllLine[0].Substring(4),$expectedDll.Replace('\','/'),[StringComparison]::OrdinalIgnoreCase) -or
            $text -notmatch 'PASS RichEdit composition/conversion/commit/cancel/okuri' -or
            $text -notmatch 'PASS EDIT native conversion/commit/cancel/okuri' -or
            $text -notmatch 'PASS previous process input profile restored') { throw "The $architecture registered input probe failed. Inspect $report." }
        if ($WithZenz) {
            foreach ($control in @('RichEdit','EDIT')) {
                foreach ($context in @('exam','food')) {
                    if ($text -notmatch "(?m)^PASS $control zenz context=$context generated/commit/continued-input\r?`$") {
                        throw "The $architecture $control did not apply zenz for $context. Inspect $report."
                    }
                }
            }
            $running = @(Get-TestServers)
            if ($running.Count -ne 1) { throw 'Expected one native ARM64 server for this test run.' }
            $serverRuns += $running
        }
        $results += [ordered]@{host=$architecture;exit_code=$process.ExitCode;registered_profile=$true;
            rich_edit_path='real HWND/document, ITfKeystrokeMgr';edit_path='SendInput, foreground and focus guarded';
            inference=[bool]$WithZenz;generated_contexts=$(if ($WithZenz) { 4 } else { 0 });restored=$true;report=$report}
    } finally {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
        $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $inputReports 'results.json')
    }
}
} finally {
    if ($WithZenz) {
        $limit = [Environment]::TickCount64 + 15000
        do {
            $remaining = @(Get-TestServers)
            if (-not $remaining.Count) { break }
            Start-Sleep -Milliseconds 100
        } while ([Environment]::TickCount64 -lt $limit)
        [ordered]@{path=$server;sha256=$entry[0].sha256;model_sha256=$manifest.model_sha256;
            observed=$serverRuns;idle_exit=($remaining.Count -eq 0);remaining=$remaining} |
            ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $inputReports 'server.json')
        if ($remaining.Count) { throw 'The isolated test server did not idle out; keep its files and inspect server.json.' }
    }
}
$results | ConvertTo-Json -Depth 5
