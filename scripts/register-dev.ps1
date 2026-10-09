#Requires -Version 7.0
[CmdletBinding()]
param([switch]$Unregister)
$ErrorActionPreference = 'Stop'
if ($Unregister) {
    & (Join-Path $PSScriptRoot 'install.ps1') -Action Unregister
    return
}
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$archive = Join-Path $repository ('out/packages/dev-' + [Guid]::NewGuid().ToString('N') + '.zip')
$package = & (Join-Path $PSScriptRoot 'package.ps1') -Output $archive
$directory = $archive + '.expanded'
[IO.Compression.ZipFile]::ExtractToDirectory($package.Path, $directory)
& (Join-Path $PSScriptRoot 'install.ps1') -Action Install -PackageDirectory $directory
