#Requires -Version 7.0
param([Parameter(Mandatory)][string]$BuildDirectory, [Parameter(Mandatory)][string]$Configuration, [switch]$ExperimentalArm64)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Configuration -ne 'Release') { Write-Output 'SKIP installation fixtures require Release'; exit 77 }
$repository = Split-Path -Parent $PSScriptRoot
$verifier = Join-Path $repository $(if($ExperimentalArm64){'scripts/package-arm64x.ps1'}else{'scripts/package.ps1'})
Import-Module (Join-Path $repository 'scripts/installation.psm1') -Force
$fixture = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('install-test 日本語 space-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$root = Join-Path $fixture 'managed'
# The registry and HKCU Run are simulated; the real OS registration is never touched.
$state = @{ Dll = $null; X86Dll = $null; Startup = $null; FailNextAdd = $false; FailNextX86Add = $false; RegistrarCalls=0 }
$setStartup = { param([string]$Broker) $state.Startup = $Broker }.GetNewClosure()
$readRegistration = { return $state.Dll }.GetNewClosure()
$readX86Registration = { return $state.X86Dll }.GetNewClosure()
$registrar = {
    param([string]$Dll, [bool]$Unregister)
    ++$state.RegistrarCalls
    if (-not $Unregister -and $state.FailNextAdd) { $state.FailNextAdd = $false; return 5 }
    $slot = if ([IO.Path]::GetFileName([IO.Path]::GetDirectoryName($Dll)) -eq 'x86') { 'X86Dll' } else { 'Dll' }
    if (-not $Unregister -and $slot -eq 'X86Dll' -and $state.FailNextX86Add) { $state.FailNextX86Add=$false; return 5 }
    $state[$slot] = if ($Unregister) { $null } else { $Dll }
    return 0
}.GetNewClosure()
function Assert-Install([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Transition([string]$Target) { Invoke-NskkzimeTransition $root $Target $verifier $readRegistration $registrar $setStartup $readX86Registration }
function Expand-Version([string]$Name, [string]$Marker) {
    # Same binaries with a distinct README give a second, verifiable version.
    $directory = Join-Path $fixture $Name
    [IO.Compression.ZipFile]::ExtractToDirectory($zip.Path, $directory)
    $readme = Join-Path $directory 'README.md'
    [IO.File]::AppendAllText($readme, "`n$Marker`n")
    $manifestPath = Join-Path $directory 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $entry = $manifest.files | Where-Object path -eq README.md
    $entry.size = (Get-Item -LiteralPath $readme).Length
    $entry.sha256 = (Get-FileHash -LiteralPath $readme).Hash.ToLowerInvariant()
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath
    return Add-NskkzimeVersion $root $directory $verifier
}
try {
    $zip = & $verifier -SourceDirectory (Join-Path $BuildDirectory 'Release') -Output (Join-Path $fixture 'package.zip')
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $a = Expand-Version 'first' 'Fixture first version.'
    $b = Expand-Version 'second' 'Fixture second version.'
    Transition $a.Version | Out-Null
    Assert-Install ($state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll -and $state.Startup -eq $a.Broker) 'Initial install failed.'
    Transition $b.Version | Out-Null
    Assert-Install ($state.Dll -eq $b.Dll -and $state.X86Dll -eq $b.X86Dll -and $state.Startup -eq $b.Broker) 'Update failed.'
    Transition $a.Version | Out-Null
    Assert-Install ($state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll) 'Rollback failed.'
    # A failed registration restores the previous version.
    $state.FailNextAdd = $true
    $failed = $false
    try { Transition $b.Version | Out-Null } catch { $failed = $true }
    Assert-Install ($failed -and $state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll) 'Failed update lost the previous version.'
    $state.FailNextX86Add=$true; $failed=$false
    try { Transition $b.Version | Out-Null } catch { $failed=$true }
    Assert-Install ($failed -and $state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll -and $state.Startup -eq $a.Broker) 'Partial x86 registration failure lost the previous version.'
    $calls=$state.RegistrarCalls; $state.X86Dll='C:\foreign\nskkzime_tsf.dll'; $failed=$false
    try { Transition $b.Version | Out-Null } catch { $failed=$true }
    Assert-Install ($failed -and $state.RegistrarCalls -eq $calls -and $state.Dll -eq $a.Dll) 'Foreign x86 owner reached the registrar.'
    $state.X86Dll=$a.X86Dll
    # Recover an interrupted transition with different native/x86 owners.
    @{format_version=2;previous=$a.Version;target=$b.Version} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'transaction.json')
    $state.Dll=$b.Dll
    Repair-NskkzimeTransition $root $verifier $readRegistration $registrar $setStartup $readX86Registration | Out-Null
    Assert-Install ($state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll -and -not (Test-Path -LiteralPath (Join-Path $root 'transaction.json'))) 'Interrupted transition did not recover.'
    Transition $null | Out-Null
    Assert-Install (-not $state.Dll -and -not $state.X86Dll -and -not $state.Startup -and (Test-Path -LiteralPath $a.Dll)) 'Unregister failed or removed files.'
    $status = Get-NskkzimeInstallStatus $root $readRegistration
    Assert-Install (-not $status.ManagedVersion -and $status.AvailableVersions.Count -eq 2 -and -not $status.Pending) 'Status is inconsistent.'
    Write-Output 'PASS install, update, rollback, native/x86 failure recovery, interrupted recovery, unregister and status'
} finally {
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Assert-NskkzimePath $absolute
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
