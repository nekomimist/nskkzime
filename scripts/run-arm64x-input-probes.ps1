#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9-]{0,63}$')][string]$Name,
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$')][string]$RunName = ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8)),
    [ValidateRange(30,600)][int]$TimeoutSeconds = 330,
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
$results = @()
foreach ($architecture in @('arm64','x64','arm64ec','x86')) {
    $directory = Join-Path $inputReports $architecture
    New-Item -ItemType Directory -Path $directory -Force | Out-Null
    $report = Join-Path $directory 'probe.txt'
    $executable = if ($architecture -eq 'x86') { Join-Path $stage 'bin/x86/ime_probe.exe' } else { Join-Path $stage "bin/ime_probe-$architecture.exe" }
    $expectedDll = if ($architecture -eq 'x86') { $journal.x86 } else { $journal.native }
    $start = [Diagnostics.ProcessStartInfo]::new($executable)
    $start.UseShellExecute = $false; $start.WorkingDirectory = $directory
    $start.ArgumentList.Add('--self-test'); $start.ArgumentList.Add($dictionary); $start.ArgumentList.Add($report)
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
        $results += [ordered]@{host=$architecture;exit_code=$process.ExitCode;registered_profile=$true;
            rich_edit_path='real HWND/document, ITfKeystrokeMgr';edit_path='SendInput, foreground and focus guarded';restored=$true;report=$report}
    } finally {
        if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
        $process.Dispose()
        $results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $inputReports 'results.json')
    }
}
$results | ConvertTo-Json -Depth 5
