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
            if ($Mode -eq 'metadata-mismatch' -and $entry.FullName -eq 'manifest.json') {
                $document = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
                $document.binary_build_revision = '0' * 40
                $bytes = [Text.Encoding]::UTF8.GetBytes(($document | ConvertTo-Json -Depth 8))
            }
            if ($Mode -in @('legacy-metadata', 'reordered-metadata') -and $entry.FullName -eq 'manifest.json') {
                $document = [Text.Encoding]::UTF8.GetString($bytes) | ConvertFrom-Json
                if ($Mode -eq 'legacy-metadata') {
                    $document.PSObject.Properties.Remove('binary_build_metadata')
                    $document.binary_build_revision = $null
                } else {
                    $reordered = [ordered]@{}
                    foreach ($property in $document.binary_build_metadata.PSObject.Properties | Sort-Object Name -Descending) {
                        $reordered[$property.Name] = $property.Value
                    }
                    $document.binary_build_metadata = $reordered
                }
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
    Assert-Package ($verified.Verified -and $verified.Files -eq 19 -and $verified.Variant -eq 'zenz-cpu') 'Incomplete zenz package.'
    Assert-Package ($verified.SHA256 -eq $created.SHA256) 'Verification changed archive.'
    Expect-Failure { & $PackageScript @arguments } '上書きしません'
    Assert-Package ((Get-FileHash -LiteralPath $created.Path).Hash -eq $created.SHA256) 'Existing archive overwritten.'
    $arguments.Output = Join-Path $fixture 'same-input.zip'
    $repeat = & $PackageScript @arguments
    Assert-Package ($repeat.SHA256 -eq $created.SHA256) 'Identical inputs did not reproduce ZIP bytes.'
    $failures = [ordered]@{ corrupt = 'ハッシュまたは長さ'; missing = 'ファイル一覧'; 'missing-server' = 'ファイル一覧'; 'manifest-duplicate' = '不足・余分・重複';
        duplicate = '不正または重複'; 'case-duplicate' = '不正または重複'; traversal = '不正または重複'; extra = 'ファイル一覧'; 'metadata-mismatch' = 'ビルド情報が一致しません' }
    foreach ($mode in $failures.Keys) {
        $bad = Join-Path $fixture ($mode + '.zip')
        Rewrite-Zip $created.Path $bad $mode
        Expect-Failure { & $PackageScript -Verify $bad } $failures[$mode]
    }
    foreach ($mode in @('reordered-metadata')) {
        $compatible = Join-Path $fixture ($mode + '.zip')
        Rewrite-Zip $created.Path $compatible $mode
        Assert-Package ((& $PackageScript -Verify $compatible).Verified) "Rejected compatible $mode manifest."
    }
    $expanded = Join-Path $fixture 'expanded'
    [IO.Compression.ZipFile]::ExtractToDirectory($created.Path, $expanded)
    $directoryResult = & $PackageScript -VerifyDirectory $expanded
    Assert-Package ($directoryResult.Verified -and $directoryResult.FormatVersion -eq 4 -and $directoryResult.Paths.Count -eq 20) 'Expanded package verification failed.'
    $manifest = Get-Content -LiteralPath (Join-Path $expanded 'manifest.json') -Raw | ConvertFrom-Json
    Assert-Package ($manifest.binary_build_revision -match '^[a-f0-9]{40}$' -and
        $manifest.binary_build_revision -eq $manifest.binary_build_metadata.source_revision -and
        $manifest.binary_build_metadata.source_sha256 -match '^[a-f0-9]{64}$' -and
        $manifest.binary_build_metadata.llama_source_sha256 -match '^[a-f0-9]{64}$') 'Missing embedded build provenance.'
    $companionPath = Join-Path $expanded 'bin/x86/nskkzime_tsf.dll'
    $companionBytes = [IO.File]::ReadAllBytes($companionPath)
    $manifestFile = Join-Path $expanded 'manifest.json'
    $manifestBytes = [IO.File]::ReadAllBytes($manifestFile)
    try {
        [IO.File]::Copy((Join-Path $expanded 'bin/nskkzime_tsf.dll'), $companionPath, $true)
        $record = $manifest.files | Where-Object path -eq 'bin/x86/nskkzime_tsf.dll'
        $record.size = (Get-Item $companionPath).Length
        $record.sha256 = (Get-FileHash $companionPath).Hash.ToLowerInvariant()
        $manifest | ConvertTo-Json -Depth 8 | Set-Content $manifestFile
        Expect-Failure { & $PackageScript -VerifyDirectory $expanded } 'x86 PE32'
    } finally { [IO.File]::WriteAllBytes($companionPath, $companionBytes); [IO.File]::WriteAllBytes($manifestFile, $manifestBytes) }
    $directoryReadme = Join-Path $expanded 'README.md'
    $originalReadme = [IO.File]::ReadAllBytes($directoryReadme)
    [IO.File]::AppendAllText($directoryReadme, 'corrupt')
    Expect-Failure { & $PackageScript -VerifyDirectory $expanded } 'ハッシュまたは長さ'
    [IO.File]::Delete($directoryReadme)
    Expect-Failure { & $PackageScript -VerifyDirectory $expanded } 'ファイル一覧'
    [IO.File]::WriteAllBytes($directoryReadme, $originalReadme)
    $extraFile = Join-Path $expanded 'settings.ini'
    [IO.File]::WriteAllText($extraFile, 'private data')
    Expect-Failure { & $PackageScript -VerifyDirectory $expanded } '余分なファイル'
    [IO.File]::Delete($extraFile)
    $extraDirectory = Join-Path $expanded 'empty'
    [IO.Directory]::CreateDirectory($extraDirectory) | Out-Null
    Expect-Failure { & $PackageScript -VerifyDirectory $expanded } '余分なディレクトリー'
    [IO.Directory]::Delete($extraDirectory)
    $junction = Join-Path $fixture 'linked-package'
    New-Item -ItemType Junction -Path $junction -Target $expanded | Out-Null
    try { Expect-Failure { & $PackageScript -VerifyDirectory $junction } '再解析ポイント' }
    finally { [IO.Directory]::Delete($junction) }
    # Check a genuine wrong-architecture input without changing build artifacts.
    $fake = Join-Path $fixture 'Release'
    [IO.Directory]::CreateDirectory($fake) | Out-Null
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakeCache.txt'), "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64`nNSKKZIME_WITH_ZENZ:BOOL=ON`n")
    foreach ($name in @('nskkzime_tsf.dll', 'ime_probe.exe')) {
        [IO.Directory]::CreateDirectory((Join-Path $fake 'x86')) | Out-Null
        [IO.File]::Copy((Join-Path $source ('x86/' + $name)), (Join-Path $fake ('x86/' + $name)), $true)
    }
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
    # Real images with deliberately damaged provenance; no build output is modified.
    $original = [IO.File]::ReadAllBytes((Join-Path $source 'ime_probe.exe'))
    $sectionName = [Text.Encoding]::Latin1.GetString($original).IndexOf('.nskkz' + [char]0, [StringComparison]::Ordinal)
    Assert-Package ($sectionName -ge 0) 'Image has no metadata section.'
    $bytes = $original.Clone()
    $bytes[$sectionName + 1] = [byte][char]'x'
    [IO.File]::WriteAllBytes((Join-Path $fake 'ime_probe.exe'), $bytes)
    Expect-Failure { & $PackageScript @arguments } 'ビルド情報が不正または不足'
    $bytes = $original.Clone()
    [BitConverter]::GetBytes([uint32]($bytes.Length - 1)).CopyTo($bytes, $sectionName + 20)
    [IO.File]::WriteAllBytes((Join-Path $fake 'ime_probe.exe'), $bytes)
    Expect-Failure { & $PackageScript @arguments } 'ビルド情報が不正または不足'
    $bytes = $original.Clone()
    # Locate through Latin1, whose character offsets equal byte offsets even in a PE image.
    $targetOffset = [Text.Encoding]::Latin1.GetString($bytes).IndexOf('"binary":"ime_probe.exe"', [StringComparison]::Ordinal)
    Assert-Package ($targetOffset -ge 0) 'Missing image identity.'
    [Text.Encoding]::ASCII.GetBytes('"binary":"ime_wrong.exe"').CopyTo($bytes, $targetOffset)
    [IO.File]::WriteAllBytes((Join-Path $fake 'ime_probe.exe'), $bytes)
    Expect-Failure { & $PackageScript @arguments } 'ビルド情報が不正または不足'
    # Reject duplicate JSON fields and values with the wrong type.
    $sectionOffset = [BitConverter]::ToUInt32($original, $sectionName + 20)
    $sectionSize = [BitConverter]::ToUInt32($original, $sectionName + 16)
    $jsonLength = [Array]::IndexOf($original, [byte]0, [int]$sectionOffset, [int]$sectionSize) - $sectionOffset
    $json = [Text.Encoding]::UTF8.GetString($original, $sectionOffset, $jsonLength)
    foreach ($changedJson in @(('{"binary":"ime_probe.exe",' + $json.Substring(1)), $json.Replace('"format_version":1', '"format_version":"1"'))) {
        $changedBytes = [Text.Encoding]::UTF8.GetBytes($changedJson)
        Assert-Package ($changedBytes.Length -lt $sectionSize) 'Modified JSON exceeds fixture section.'
        $bytes = $original.Clone()
        [Array]::Clear($bytes, [int]$sectionOffset, [int]$sectionSize)
        $changedBytes.CopyTo($bytes, $sectionOffset)
        [IO.File]::WriteAllBytes((Join-Path $fake 'ime_probe.exe'), $bytes)
        Expect-Failure { & $PackageScript @arguments } 'ビルド情報が不正または不足'
    }
    foreach ($file in Get-ChildItem -LiteralPath $source -File | Where-Object { $_.Extension -in @('.exe', '.dll') }) {
        Copy-Item -LiteralPath $file.FullName -Destination (Join-Path $fake $file.Name) -Force
    }
    $bytes = $original.Clone()
    $hashMarker = '"source_sha256":"'
    $hashOffset = [Text.Encoding]::Latin1.GetString($bytes).IndexOf($hashMarker, [StringComparison]::Ordinal) + $hashMarker.Length
    Assert-Package ($hashOffset -ge $hashMarker.Length) 'Missing source fingerprint.'
    $bytes[$hashOffset] = if ($bytes[$hashOffset] -eq [byte][char]'a') { [byte][char]'b' } else { [byte][char]'a' }
    [IO.File]::WriteAllBytes((Join-Path $fake 'ime_probe.exe'), $bytes)
    Expect-Failure { & $PackageScript @arguments } '異なるビルド'
    Assert-Package (-not (Test-Path -LiteralPath $arguments.Output)) 'Mixed build was packaged.'
    [IO.File]::WriteAllText((Join-Path $fixture 'CMakeCache.txt'), "CMAKE_GENERATOR_PLATFORM:INTERNAL=x64`nNSKKZIME_WITH_ZENZ:BOOL=OFF`n")
    Expect-Failure { & $PackageScript @arguments } 'テスト専用ビルドは配布できません'
    Assert-Package (-not (Test-Path -LiteralPath $arguments.Output)) 'A test-only build was packaged.'
    Write-Output 'PASS zenz-only package, required server, Unicode paths, reproducible bytes, no overwrite, corruption, missing/duplicate/traversing/extra files, wrong architecture, unknown dependency, embedded provenance, mixed-build and test-only rejection'
} finally {
    # Only this uniquely named workspace fixture may be recursively removed.
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
