#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$Repository,
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][string]$Configuration,
    [Parameter(Mandatory)][string]$CompilerVersion,
    [Parameter(Mandatory)][string]$CacheFile,
    [Parameter(Mandatory)][ValidateSet('zenz-cpu', 'test-only', 'x86-companion', 'tsf-companion', 'arm64x-native', 'arm64x-ec')][string]$Variant,
    [string]$LlamaDirectory,
    [string]$LlamaRevision,
    [string]$LlamaBackendBuildDirectory
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
    if ($LlamaBackendBuildDirectory) {
        if ($Configuration -cne 'Release') { throw 'The experimental ARM64 backend is Release only.' }
        $compilerFiles = @(Get-ChildItem -LiteralPath (Join-Path $LlamaBackendBuildDirectory 'CMakeFiles') -Directory |
            ForEach-Object { Join-Path $_.FullName 'CMakeCXXCompiler.cmake' } | Where-Object { Test-Path -LiteralPath $_ })
        if ($compilerFiles.Count -ne 1) { throw 'Cannot identify the backend compiler.' }
        $compilerInfo = Get-Content -LiteralPath $compilerFiles[0] -Raw
        if ($compilerInfo -notmatch 'set\(CMAKE_CXX_COMPILER_ID "Clang"\)' -or
            $compilerInfo -notmatch 'set\(CMAKE_CXX_COMPILER_VERSION "([0-9.]+)"\)') { throw 'Expected a Clang backend.' }
        $backendCompiler = 'Clang ' + $Matches[1]
        $libraries = [ordered]@{}
        foreach ($library in @('llama', 'ggml', 'ggml-base', 'ggml-cpu')) {
            $libraries[$library] = (Get-FileHash -LiteralPath (Join-Path $LlamaBackendBuildDirectory "lib/Release/$library.lib") -Algorithm SHA256).Hash.ToLowerInvariant()
        }
        $metadata.llama_backend = [ordered]@{
            compiler = $backendCompiler
            configuration = $Configuration
            cpu_contract = 'zenz-arm64-armv8a-v1'
            cmake_cache_sha256 = (Get-FileHash -LiteralPath (Join-Path $LlamaBackendBuildDirectory 'CMakeCache.txt') -Algorithm SHA256).Hash.ToLowerInvariant()
            libraries_sha256 = $libraries
        }
    }
}
$json = $metadata | ConvertTo-Json -Compress -Depth 5
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
