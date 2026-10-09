#Requires -Version 7.0
[CmdletBinding()]
param([switch]$Unregister)
$ErrorActionPreference = 'Stop'
if ($Unregister) {
    & (Join-Path $PSScriptRoot 'install.ps1') -Action Unregister
    return
}
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Import-Module (Join-Path $PSScriptRoot 'installation.psm1') -Force
$temporaryRoot = Join-Path $repository 'out/tmp'
$stage = Join-Path $temporaryRoot ('register-dev-' + [Guid]::NewGuid().ToString('N'))
Assert-NskkzimePath $stage
[IO.Directory]::CreateDirectory($stage) | Out-Null
try {
    $package = & (Join-Path $PSScriptRoot 'package.ps1') -Output (Join-Path $stage 'package.zip')
    $directory = Join-Path $stage 'expanded'
    [IO.Compression.ZipFile]::ExtractToDirectory($package.Path, $directory)
    & (Join-Path $PSScriptRoot 'install.ps1') -Action Install -PackageDirectory $directory
} catch {
    Write-Warning "導入の作業ファイルを残しました: $stage"
    throw
}
if ([IO.Path]::GetDirectoryName([IO.Path]::GetFullPath($stage)) -ne $temporaryRoot) {
    throw '作業フォルダーがout/tmpの外です。'
}
Assert-NskkzimePath $stage
Remove-Item -LiteralPath $stage -Recurse -Force
