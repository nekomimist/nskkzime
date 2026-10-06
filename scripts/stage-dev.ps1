#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$SourceDirectory = (Join-Path $PSScriptRoot '../out/build/windows-x64/Release'),
    [string]$DestinationRoot = (Join-Path $PSScriptRoot '../out/installed')
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath($SourceDirectory)
$bundleRoot = [IO.Path]::GetFullPath($DestinationRoot)
$requiredFiles = @('nskkzime_tsf.dll', 'nskkzime_dictionary_server.exe', 'nskkzime_settings.exe', 'nskkzime_dictionary_gui.exe')
$hashes = [ordered]@{}
foreach ($name in $requiredFiles) {
    $source = Join-Path $sourceRoot $name
    if (-not (Test-Path -LiteralPath $source -PathType Leaf)) { throw "ビルド成果物がありません: $source" }
    $hashes[$name] = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
}
$identity = ($requiredFiles | ForEach-Object { $_ + ':' + $hashes[$_] }) -join "`n"
$bundleDigest = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($identity)))
$destination = Join-Path $bundleRoot $bundleDigest.Substring(0, 16).ToLowerInvariant()
[IO.Directory]::CreateDirectory($destination) | Out-Null
foreach ($name in $requiredFiles) {
    $target = Join-Path $destination $name
    if (-not (Test-Path -LiteralPath $target)) {
        # Never replace an already staged or loaded binary, including a racing stager.
        try { [IO.File]::Copy((Join-Path $sourceRoot $name), $target, $false) }
        catch { if (-not (Test-Path -LiteralPath $target -PathType Leaf)) { throw } }
    }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hashes[$name]) {
        throw "配置内容が一致しません。登録は行いません: $target"
    }
}
[pscustomobject]@{ Directory = $destination; Dll = (Join-Path $destination 'nskkzime_tsf.dll'); Hashes = $hashes }
