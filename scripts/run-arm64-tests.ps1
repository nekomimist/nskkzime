#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Name = '.*',
    [string]$ReportName = ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0, 8)),
    [switch]$ListOnly
)
$ErrorActionPreference = 'Stop'
if ($ReportName -notmatch '^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$') { throw 'Invalid report name.' }
if ([Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne [Runtime.InteropServices.Architecture]::Arm64) {
    throw 'Run this test bundle on ARM64 Windows.'
}
$root = $PSScriptRoot.Replace('\', '/')
$pwshExe = (Get-Command pwsh -CommandType Application).Source.Replace('\', '/')
$testFile = [IO.File]::ReadAllText((Join-Path $PSScriptRoot 'build/CTestTestfile.cmake.in'))
$testFile = $testFile.Replace('@BUNDLE@', $root).Replace('@PWSH@', $pwshExe)
[IO.File]::WriteAllText((Join-Path $PSScriptRoot 'build/CTestTestfile.cmake'), $testFile, [Text.UTF8Encoding]::new($false))
$reports = Join-Path $PSScriptRoot ('reports/' + $ReportName)
if (Test-Path -LiteralPath $reports) { throw 'The report directory already exists; choose a new name.' }
New-Item -ItemType Directory -Path $reports | Out-Null
[ordered]@{
    os_architecture = [Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()
    powershell_architecture = [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString()
    os_version = [Environment]::OSVersion.VersionString
    session_id = [Diagnostics.Process]::GetCurrentProcess().SessionId
    user_interactive = [Environment]::UserInteractive
} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $reports 'environment.json') -Encoding utf8
$ctestArguments = @('--test-dir', (Join-Path $PSScriptRoot 'build'), '-C', 'Release',
    '--output-on-failure', '--no-tests=error', '-R', $Name,
    '-E', '^(package|installation|scoop|scoop_windows_powershell)$',
    '--output-junit', (Join-Path $reports 'results.xml'),
    '--output-log', (Join-Path $reports 'ctest.log'))
if ($ListOnly) { $ctestArguments += '--show-only' }
& (Join-Path $PSScriptRoot 'tools/bin/ctest.exe') @ctestArguments
exit $LASTEXITCODE
