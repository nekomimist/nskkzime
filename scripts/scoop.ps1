#Requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Install', 'Unregister')][string]$Action,
    [string]$PackageDirectory = (Join-Path $PSScriptRoot '..'),
    [string]$PowerShellPath # Ignored for compatibility with the previous bucket manifest.
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ([Environment]::OSVersion.Platform -ne [PlatformID]::Win32NT -or
    [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne [Runtime.InteropServices.Architecture]::X64) {
    throw 'nskkzime requires Windows x64.'
}
$directory = [IO.Path]::GetFullPath($PackageDirectory)
$installer = Join-Path $directory 'bin/nskkzime_setup.exe'
if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) { throw "Installer not found: $installer" }
$manifest=Get-Content -LiteralPath (Join-Path $directory 'manifest.json') -Raw | ConvertFrom-Json
if ($manifest.format_version -ne 8 -or $manifest.architecture -cne 'x64' -or $manifest.variant -cne 'zenz-cpu') {
    throw 'Scoop requires an x64 release package; experimental ARM64 packages are not supported.'
}
# Start-Process joins ArgumentList into one Windows command line. File paths
# cannot contain double quotes; quote each path to preserve spaces and Unicode.
$arguments = @('--action', $Action, '--package-directory', ('"' + $directory.TrimEnd('\', '/') + '"'))
$start = @{
    FilePath = $installer; ArgumentList = $arguments
    Wait = $true; PassThru = $true; WindowStyle = 'Normal'
}
try { $child = Start-Process @start }
catch { throw "nskkzime $Action could not start. Approve UAC as the same user and retry. $($_.Exception.Message)" }
try {
    if ($child.ExitCode -ne 0) {
        throw "nskkzime $Action failed (exit $($child.ExitCode)). Run bin/nskkzime_setup.exe --action Status from Explorer or a normal terminal; use Recover if a transaction is pending. See %ProgramFiles%/nskkzime/setup.log if present."
    }
} finally { $child.Dispose() }
