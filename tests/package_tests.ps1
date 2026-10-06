#Requires -Version 7.0
param(
    [Parameter(Mandatory)][string]$PackageScript,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [Parameter(Mandatory)][string]$Configuration,
    [switch]$TestOnly
)
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('package-test 日本語 space-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$source = Join-Path $BuildDirectory $Configuration
$arguments = @{ SourceDirectory = $source; Output = (Join-Path $fixture 'package.zip') }
function Assert-Package([bool]$Condition, [string]$Reason) { if (-not $Condition) { throw $Reason } }
function Expect-Failure([scriptblock]$Action, [string]$Expected) {
    $message = $null
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert-Package ($null -ne $message -and $message.Contains($Expected)) "Expected '$Expected', got '$message'."
}
function Rewrite-Zip([string]$InputZip, [string]$OutputZip, [string]$Mode) {
    $inputArchive = [IO.Compression.ZipFile]::OpenRead($InputZip)
    $outputArchive = [IO.Compression.ZipFile]::Open($OutputZip, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($entry in $inputArchive.Entries) {
            if ($Mode -eq 'missing' -and $entry.FullName -eq 'licenses/Unicode-3.0.txt') { continue }
            if ($Mode -eq 'missing-server' -and $entry.FullName -eq 'bin/nkkz-ime-server.exe') { continue }
            $memory = [IO.MemoryStream]::new()
            $stream = $entry.Open()
            try { $stream.CopyTo($memory); $bytes = $memory.ToArray() } finally { $stream.Dispose(); $memory.Dispose() }
            if ($Mode -eq 'corrupt' -and $entry.FullName -eq 'bin/ime_probe.exe') { $bytes[0] = $bytes[0] -bxor 1 }
            if ($Mode -eq 'manifest-duplicate' -and $entry.FullName -eq 'manifest.json') {
                $document = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
                $document.files[1] = $document.files[0]
                $bytes = [Text.Encoding]::UTF8.GetBytes(($document | ConvertTo-Json -Depth 8))
            }
            $copy = $outputArchive.CreateEntry($entry.FullName).Open()
            try { $copy.Write($bytes, 0, $bytes.Length) } finally { $copy.Dispose() }
        }
        $extra = switch ($Mode) { 'duplicate' { 'README.md' }; 'case-duplicate' { 'readme.md' }; 'traversal' { '../escape.txt' }; 'extra' { 'settings.ini' } }
        if ($extra) { $outputArchive.CreateEntry($extra) | Out-Null }
    } finally { $outputArchive.Dispose(); $inputArchive.Dispose() }
}
try {
    if ($Configuration -eq 'Debug') {
        Expect-Failure { & $PackageScript @arguments } 'Releaseディレクトリー'
        Assert-Package (-not (Test-Path -LiteralPath $arguments.Output)) 'Debug package was written.'
        Write-Output 'PASS Debug configuration rejected without creating an archive'
        return
    }
    if ($TestOnly) {
        Expect-Failure { & $PackageScript @arguments } 'テスト専用ビルドは配布できません'
        Assert-Package (-not (Test-Path -LiteralPath $arguments.Output)) 'Test-only package was written.'
        Write-Output 'PASS test-only configuration rejected without creating an archive'
        return
    }
    $created = & $PackageScript @arguments
    $verified = & $PackageScript -Verify $created.Path
    Assert-Package ($verified.Verified -and $verified.Files -eq 14 -and $verified.Variant -eq 'zenz-cpu') 'Incomplete zenz package.'
    Assert-Package ($verified.SHA256 -eq $created.SHA256) 'Verification changed archive.'
    Expect-Failure { & $PackageScript @arguments } '上書きしません'
    Assert-Package ((Get-FileHash -LiteralPath $created.Path).Hash -eq $created.SHA256) 'Existing archive overwritten.'
    $arguments.Output = Join-Path $fixture 'same-input.zip'
    $repeat = & $PackageScript @arguments
    Assert-Package ($repeat.SHA256 -eq $created.SHA256) 'Identical inputs did not reproduce ZIP bytes.'
    $failures = [ordered]@{ corrupt = 'ハッシュまたは長さ'; missing = 'ファイル一覧'; 'missing-server' = 'ファイル一覧'; 'manifest-duplicate' = '不足・余分・重複';
        duplicate = '不正または重複'; 'case-duplicate' = '不正または重複'; traversal = '不正または重複'; extra = 'ファイル一覧' }
    foreach ($mode in $failures.Keys) {
        $bad = Join-Path $fixture ($mode + '.zip')
        Rewrite-Zip $created.Path $bad $mode
        Expect-Failure { & $PackageScript -Verify $bad } $failures[$mode]
    }
    # Check a genuine wrong-architecture input without changing build artifacts.
    $fake = Join-Path $fixture 'Release'
    [IO.Directory]::CreateDirectory($fake) | Out-Null
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakeCache.txt'), "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64`nNSKKZIME_WITH_ZENZ:BOOL=ON`n")
    $bytes = [IO.File]::ReadAllBytes((Join-Path $source 'ime_probe.exe'))
    $pe = [BitConverter]::ToUInt32($bytes, 60)
    $bytes[$pe + 4] = 0x4c; $bytes[$pe + 5] = 0x01
    [IO.File]::WriteAllBytes((Join-Path $fake 'ime_probe.exe'), $bytes)
    $arguments.SourceDirectory = $fake
    $arguments.Output = Join-Path $fixture 'invalid.zip'
    Expect-Failure { & $PackageScript @arguments } 'x64 PE32+'
    Assert-Package (-not (Test-Path -LiteralPath $arguments.Output)) 'Invalid image was packaged.'
    $bytes = [IO.File]::ReadAllBytes((Join-Path $source 'ime_probe.exe'))
    $offset = [Text.Encoding]::Latin1.GetString($bytes).IndexOf('KERNEL32.dll', [StringComparison]::OrdinalIgnoreCase)
    Assert-Package ($offset -ge 0) 'Fixture has no KERNEL32 import.'
    [Text.Encoding]::ASCII.GetBytes('BADDEP32.dll').CopyTo($bytes, $offset)
    [IO.File]::WriteAllBytes((Join-Path $fake 'ime_probe.exe'), $bytes)
    Expect-Failure { & $PackageScript @arguments } '未確認の依存DLL'
    Assert-Package (-not (Test-Path -LiteralPath $arguments.Output)) 'Unknown dependency was packaged.'
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakeCache.txt'), "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64`nNSKKZIME_WITH_ZENZ:BOOL=OFF`n")
    Expect-Failure { & $PackageScript @arguments } 'テスト専用ビルドは配布できません'
    Assert-Package (-not (Test-Path -LiteralPath $arguments.Output)) 'A test-only build was packaged.'
    Write-Output 'PASS zenz-only package, required server, Unicode paths, reproducible bytes, no overwrite, corruption, missing/duplicate/traversing/extra files, wrong architecture, unknown dependency and test-only rejection'
} finally {
    # Enumerate only our uniquely named workspace fixture; no recursive deletion.
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    $fake = Join-Path $absolute 'Release'
    if (Test-Path -LiteralPath $fake) {
        foreach ($file in Get-ChildItem -LiteralPath $fake -File) { Remove-Item -LiteralPath $file.FullName }
        Remove-Item -LiteralPath $fake
    }
    foreach ($file in Get-ChildItem -LiteralPath $absolute -File) { Remove-Item -LiteralPath $file.FullName }
    Remove-Item -LiteralPath $absolute
}
