#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install', 'Unregister')][string]$Action,
    [string]$PackageDirectory = (Join-Path $PSScriptRoot '..'),
    [Parameter(Mandatory)][string]$PowerShellPath
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or
    [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne [Runtime.InteropServices.Architecture]::X64) {
    throw 'nskkzime requires Windows x64.'
}
$directory = [IO.Path]::GetFullPath($PackageDirectory)
$installer = Join-Path $directory 'tools/install.ps1'
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw "Installer not found: $installer" }
if (-not (Test-Path -LiteralPath $PowerShellPath -PathType Leaf)) { throw "PowerShell 7 not found: $PowerShellPath" }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$principal = New-Object Security.Principal.WindowsPrincipal($identity)
# Start-Process joins ArgumentList into one Windows command line. File paths
# cannot contain double quotes; quote each path to preserve spaces and Unicode.
$arguments = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $installer + '"'),
    '-Action', $Action, '-PackageDirectory', ('"' + $directory.TrimEnd('\', '/') + '"'),
    '-ExpectedUserSid', $identity.User.Value)
$start = @{
    FilePath = $PowerShellPath; ArgumentList = $arguments
    Wait = $true; PassThru = $true; WindowStyle = 'Hidden'
}
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { $start.Verb = 'RunAs' }
try { $child = Start-Process @start }
catch { throw "nskkzime $Action could not start. Approve UAC as the same user and retry. $($_.Exception.Message)" }
try {
    if ($child.ExitCode -ne 0) {
        throw "nskkzime $Action failed (exit $($child.ExitCode)). Run tools/install.ps1 -Action Status from a normal Explorer-launched PowerShell 7 window; use Recover if a transaction is pending."
    }
} finally { $child.Dispose() }
