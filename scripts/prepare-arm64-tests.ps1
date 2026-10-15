#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '../out/build/windows-arm64-test-only'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot ('../out/tmp/arm64-tests-' + [Guid]::NewGuid().ToString('N')))
)
$ErrorActionPreference = 'Stop'
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = (Resolve-Path -LiteralPath $BuildDirectory).Path
$cache = Get-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=ARM64\r?$') {
    throw 'Use an experimental ARM64 build.'
}
$withZenz = $cache -match '(?m)^NSKKZIME_WITH_ZENZ:BOOL=ON\r?$'
if ($withZenz -and $cache -notmatch '(?m)^NSKKZIME_LLAMA_VARIANT:STRING=upstream-v0.6.0\r?$') { throw 'Unexpected ARM64 backend.' }
if ($cache -notmatch '(?m)^CMAKE_COMMAND:INTERNAL=(.+)\r?$') { throw 'Missing CMake executable in the build cache.' }
$cmakeExe = $Matches[1].Trim()
$cmakeRoot = Split-Path -Parent (Split-Path -Parent $cmakeExe)
$destination = [IO.Path]::GetFullPath($OutputDirectory)
if (Test-Path -LiteralPath $destination) { throw 'The test bundle directory already exists; choose a new directory.' }
$images = @(Get-ChildItem -LiteralPath (Join-Path $buildRoot 'Release') -File |
    Where-Object Extension -in @('.exe', '.dll'))
if (-not $images.Count) { throw 'No Release test binaries found.' }
foreach ($image in $images) {
    $bytes = [IO.File]::ReadAllBytes($image.FullName)
    if ($bytes.Length -lt 64) { throw "Invalid PE image: $($image.Name)" }
    $pe = [BitConverter]::ToUInt32($bytes, 60)
    if ($pe -gt $bytes.Length - 26 -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0xaa64) { throw "Not an ARM64 image: $($image.Name)" }
}
$release = Join-Path $destination 'build/Release'
New-Item -ItemType Directory -Path $release -Force | Out-Null
foreach ($image in $images) { Copy-Item -LiteralPath $image.FullName -Destination $release }
foreach ($relativePath in @('tests/fixtures', 'tests/config_cli.cmake', 'tests/user_dictionary_cli.cmake',
        'tests/app_container_startup_tests.ps1', 'tests/zenz_cli.ps1', 'scripts/compare-ddskk.ps1', 'third_party/unicode/JIS0212.TXT')) {
    $target = Join-Path $destination ('source/' + $relativePath)
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repoRoot $relativePath) -Destination $target -Recurse
}
if ($withZenz) {
    if ($cache -notmatch '(?m)^NSKKZIME_ZENZ_TEST_MODEL:FILEPATH=(.+)\r?$') { throw 'Missing test model.' }
    $model = $Matches[1].Trim()
    $target = Join-Path $destination 'source/out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf'
    if ([IO.Path]::GetFullPath($model) -ne [IO.Path]::GetFullPath((Join-Path $repoRoot 'out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf'))) {
        throw 'Use the pinned test model in the repository model directory.'
    }
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath $model -Destination $target
}
# CTest's generated commands contain developer-machine paths. Keep the original
# test properties and substitute only the known roots when running the bundle.
$testFile = Get-Content -LiteralPath (Join-Path $buildRoot 'CTestTestfile.cmake') -Raw
$cmakeBin = (Split-Path -Parent $cmakeExe).Replace('\', '/')
$testFile = $testFile.Replace($buildRoot.Replace('\', '/'), '@BUNDLE@/build')
$testFile = $testFile.Replace($repoRoot.Replace('\', '/'), '@BUNDLE@/source')
$testFile = $testFile.Replace($cmakeBin, '@BUNDLE@/tools/bin')
if ($cache -notmatch '(?m)^NSKKZIME_PWSH:FILEPATH=(.+)\r?$') { throw 'Missing configured PowerShell executable.' }
$pwshExe = $Matches[1].Trim().Replace('\', '/')
$testFile = [regex]::Replace($testFile, [regex]::Escape($pwshExe), '@PWSH@', [Text.RegularExpressions.RegexOptions]::IgnoreCase)
[IO.File]::WriteAllText((Join-Path $destination 'build/CTestTestfile.cmake.in'), $testFile, [Text.UTF8Encoding]::new($false))
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'run-arm64-tests.ps1') -Destination $destination
# A private portable copy avoids installing build tools on the test machine.
# The driver may be x64 under emulation; every tested EXE/DLL above is ARM64.
$toolsBin = Join-Path $destination 'tools/bin'
New-Item -ItemType Directory -Path $toolsBin -Force | Out-Null
foreach ($name in @('cmake.exe', 'ctest.exe')) { Copy-Item -LiteralPath (Join-Path $cmakeRoot ('bin/' + $name)) -Destination $toolsBin }
foreach ($share in Get-ChildItem -LiteralPath (Join-Path $cmakeRoot 'share') -Directory -Filter 'cmake-*') {
    $target = Join-Path $destination ('tools/share/' + $share.Name)
    New-Item -ItemType Directory -Path $target -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $share.FullName 'Modules') -Destination $target -Recurse
}
$copyright = Get-ChildItem -LiteralPath (Join-Path $cmakeRoot 'doc') -Recurse -File -Filter 'Copyright.txt' | Select-Object -First 1
if (-not $copyright) { throw 'CMake copyright notice not found.' }
Copy-Item -LiteralPath $copyright.FullName -Destination (Join-Path $destination 'tools/Copyright.txt')
[pscustomobject]@{ Path = $destination; Architecture = 'ARM64'; Images = $images.Count }
