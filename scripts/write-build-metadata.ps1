#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][string]$Configuration,
    [Parameter(Mandatory)][string]$CompilerVersion,
    [Parameter(Mandatory)][string]$CacheFile,
    [Parameter(Mandatory)][ValidateSet('zenz-cpu', 'test-only', 'x86-companion')][string]$Variant,
    [string]$LlamaDirectory,
    [string]$LlamaRevision
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot 'source-fingerprint.psm1') -Force
$source = Get-NskkzimeSourceFingerprint $Repository
$metadata = [ordered]@{
    format_version = 1
    source_revision = $source.Revision
    source_sha256 = $source.SHA256
    source_modified = $source.Modified
    configuration = $Configuration
    compiler = 'MSVC ' + $CompilerVersion
    variant = $Variant
    cmake_cache_sha256 = (Get-FileHash -LiteralPath $CacheFile -Algorithm SHA256).Hash.ToLowerInvariant()
    llama_revision = $null
    llama_source_sha256 = $null
}
if ($Variant -eq 'zenz-cpu') {
    if (-not $LlamaDirectory -or $LlamaRevision -cnotmatch '^[a-f0-9]{40}$') { throw 'llama.cppのビルド元がありません。' }
    $metadata.llama_revision = $LlamaRevision
    $metadata.llama_source_sha256 = Get-NskkzimeDirectoryFingerprint $LlamaDirectory
}
$json = $metadata | ConvertTo-Json -Compress
# Only fixed keys, hashes, booleans and constrained build values enter the image.
if ($json.Contains(')nskkz"') -or $json.Length -gt 3000) { throw 'ビルド情報を埋め込めません。' }
$json = $json.Substring(0, $json.Length - 1)
$code = @"
// Generated before compilation. This section contains data only, never input text.
#ifndef NSKKZIME_BINARY_NAME
#error NSKKZIME_BINARY_NAME must identify the output image
#endif
#pragma section(".nskkz", read)
extern "C" {
__declspec(dllexport) __declspec(allocate(".nskkz")) extern const char nskkzime_build_metadata[] =
R"nskkz($json,"binary":")nskkz" NSKKZIME_BINARY_NAME R"nskkz("})nskkz";
}
"@
[IO.Directory]::CreateDirectory((Split-Path -Parent ([IO.Path]::GetFullPath($Output)))) | Out-Null
if (-not (Test-Path -LiteralPath $Output) -or [IO.File]::ReadAllText($Output) -cne $code) {
    [IO.File]::WriteAllText($Output, $code, [Text.UTF8Encoding]::new($false))
}
