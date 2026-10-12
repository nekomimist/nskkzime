#Requires -Version 7.0
[CmdletBinding(DefaultParameterSetName = 'Create')]
param(
    [Parameter(ParameterSetName = 'Create')][string]$SourceDirectory,
    [Parameter(ParameterSetName = 'Create')][switch]$Zenz, # Legacy no-op; creation always includes zenz.
    [Parameter(Mandatory, ParameterSetName = 'Create')][string]$Output,
    [Parameter(Mandatory, ParameterSetName = 'Verify')][string]$Verify,
    [Parameter(Mandatory, ParameterSetName = 'Directory')][string]$VerifyDirectory
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$binaries = @('nskkzime_tsf.dll', 'nskkzime_settings.exe',
    'nskkzime_dictionary_gui.exe', 'nskkzime_config.exe', 'nskkzime_user_dictionary.exe', 'ime_probe.exe')
$baseLicenses = @('licenses/nlohmann-MIT.txt', 'licenses/Unicode-3.0.txt')
$zenzLicenses = @('licenses/skk-zenz-MIT.txt', 'licenses/llama-MIT.txt')
function Get-Digest([byte[]]$Bytes) {
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}
function Assert-PeArchitecture([byte[]]$Bytes, [string]$Name) {
    $x86 = $Name.StartsWith('bin/x86/')
    $label = if ($x86) { 'x86 PE32' } else { 'x64 PE32+' }
    if ($Bytes.Length -lt 64 -or [BitConverter]::ToUInt16($Bytes, 0) -ne 0x5a4d) { throw "PEではありません: $Name" }
    $pe = [long][BitConverter]::ToUInt32($Bytes, 60)
    if ($pe -gt $Bytes.Length - 26 -or [BitConverter]::ToUInt32($Bytes, $pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($Bytes, $pe + 4) -ne $(if ($x86) { 0x14c } else { 0x8664 }) -or
        [BitConverter]::ToUInt16($Bytes, $pe + 24) -ne $(if ($x86) { 0x10b } else { 0x20b })) { throw "$label ではありません: $Name" }
}
function Assert-CompanionSource($HostMetadata, $CompanionMetadata) {
    foreach ($key in @('source_revision', 'source_sha256', 'source_modified', 'configuration', 'compiler')) {
        if ($HostMetadata.$key -cne $CompanionMetadata.$key) { throw 'x64とx86のビルド元が一致しません。両方を再ビルドしてください。' }
    }
}
function Get-BuildMetadata([byte[]]$Bytes, [string]$Name) {
    $companion = $Name.StartsWith('bin/x86/')
    # Parse the dedicated read-only PE data section. Never load/execute an image.
    $invalid = "ビルド情報が不正または不足しています: $Name"
    if ($Bytes.Length -lt 64 -or [BitConverter]::ToUInt16($Bytes, 0) -ne 0x5a4d) { throw $invalid }
    $pe = [long][BitConverter]::ToUInt32($Bytes, 60)
    if ($pe -gt $Bytes.Length - 24 -or [BitConverter]::ToUInt32($Bytes, $pe) -ne 0x4550) { throw $invalid }
    $count = [BitConverter]::ToUInt16($Bytes, $pe + 6)
    $table = $pe + 24 + [BitConverter]::ToUInt16($Bytes, $pe + 20)
    if ($count -lt 1 -or $count -gt 96 -or $table + 40L * $count -gt $Bytes.Length) { throw $invalid }
    $found = $null
    for ($index = 0; $index -lt $count; ++$index) {
        $section = $table + 40L * $index
        if ([Text.Encoding]::ASCII.GetString($Bytes, $section, 8).TrimEnd([char]0) -cne '.nskkz') { continue }
        if ($null -ne $found) { throw $invalid }
        $size = [long][BitConverter]::ToUInt32($Bytes, $section + 16)
        $offset = [long][BitConverter]::ToUInt32($Bytes, $section + 20)
        $flags = [BitConverter]::ToUInt32($Bytes, $section + 36)
        if ($size -lt 2 -or $size -gt 8192 -or $offset -lt $table + 40L * $count -or $offset + $size -gt $Bytes.Length -or
            ($flags -band 0xE0000000L) -ne 0x40000000L) { throw $invalid }
        $length = [Array]::IndexOf($Bytes, [byte]0, [int]$offset, [int]$size) - $offset
        if ($length -lt 2 -or $length -gt 4096) { throw $invalid }
        try {
            $json = [Text.UTF8Encoding]::new($false, $true).GetString($Bytes, $offset, $length)
            $document = [Text.Json.JsonDocument]::Parse($json)
            try {
                if ($document.RootElement.ValueKind -ne [Text.Json.JsonValueKind]::Object) { throw $invalid }
                $keys = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
                foreach ($property in $document.RootElement.EnumerateObject()) {
                    if (-not $keys.Add($property.Name)) { throw $invalid }
                }
            } finally { $document.Dispose() }
            $found = $json | ConvertFrom-Json -AsHashtable
        }
        catch { throw $invalid }
    }
    $required = @('format_version', 'source_revision', 'source_sha256', 'source_modified', 'configuration', 'compiler',
        'variant', 'cmake_cache_sha256', 'llama_revision', 'llama_source_sha256', 'binary')
    if ($found -isnot [Collections.IDictionary] -or $found.Count -ne $required.Count) { throw $invalid }
    foreach ($key in $required) { if (-not $found.Contains($key)) { throw $invalid } }
    foreach ($key in $required | Where-Object { $_ -notin @('format_version', 'source_modified') }) {
        if ($companion -and $key -in @('llama_revision', 'llama_source_sha256')) { continue }
        if ($found[$key] -isnot [string]) { throw $invalid }
    }
    if ($found.format_version -isnot [long] -or $found.format_version -ne 1 -or
        $found.source_revision -cnotmatch '^([a-f0-9]{40}|[a-f0-9]{64})$' -or
        $found.source_sha256 -cnotmatch '^[a-f0-9]{64}$' -or $found.cmake_cache_sha256 -cnotmatch '^[a-f0-9]{64}$' -or
        $found.source_modified -isnot [bool] -or $found.configuration -cne 'Release' -or
        $found.variant -cne $(if ($companion) { 'x86-companion' } else { 'zenz-cpu' }) -or $found.compiler -cnotmatch '^MSVC [0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$' -or
        (-not $companion -and ($found.llama_revision -cnotmatch '^[a-f0-9]{40}$' -or $found.llama_source_sha256 -cnotmatch '^[a-f0-9]{64}$')) -or
        ($companion -and ($null -ne $found.llama_revision -or $null -ne $found.llama_source_sha256)) -or
        $found.binary -cne [IO.Path]::GetFileName($Name)) { throw $invalid }
    $null = $found.Remove('binary')
    return $found
}
function Get-MetadataIdentity($Metadata) {
    # Compare values independently of JSON property order.
    $ordered = [ordered]@{}
    if ($Metadata -is [Collections.IDictionary]) {
        foreach ($key in @($Metadata.Keys | Sort-Object -CaseSensitive)) { $ordered[$key] = $Metadata[$key] }
    } elseif ($Metadata -is [pscustomobject]) {
        foreach ($property in @($Metadata.PSObject.Properties | Sort-Object -Property Name -CaseSensitive)) { $ordered[$property.Name] = $property.Value }
    } else { throw 'マニフェストのビルド情報が一致しません。' }
    ConvertTo-Json -InputObject $ordered -Compress -Depth 4
}
function Get-PackageNames([bool]$WithZenz, [int]$Format = 5) {
    # Read-only compatibility with previously generated SKK archives. New
    # packages always pass true; no separate SKK build or release is produced.
    $names = @($binaries | ForEach-Object { 'bin/' + $_ }) + $baseLicenses + @('README.md', 'tools/package.ps1')
    if ($WithZenz) { $names += @('bin/nkkz-ime-server.exe') + $zenzLicenses }
    if ($Format -ge 2) { $names += @('tools/install.ps1', 'tools/installation.psm1') }
    if ($Format -ge 3) { $names += 'bin/nskkzime_broker.exe' }
    if ($Format -ge 4) { $names += @('bin/x86/nskkzime_tsf.dll', 'bin/x86/ime_probe.exe') }
    # Format 5 writes the user dictionary from the IME itself, without a helper.
    if ($Format -le 4) { $names += 'bin/nskkzime_dictionary_server.exe' }
    $names | Sort-Object -CaseSensitive
}
function Test-PackageContents($Contents) {
    if (-not $Contents.ContainsKey('manifest.json')) { throw 'manifest.jsonがありません。' }
    if ($Contents['manifest.json'].Length -gt 1MB) { throw 'manifest.jsonが大きすぎます。' }
    $manifest = [Text.Encoding]::UTF8.GetString($Contents['manifest.json']) | ConvertFrom-Json
    if ($manifest.format_version -notin @(1, 2, 3, 4, 5) -or $manifest.product -cne 'nskkzime' -or
        $manifest.architecture -cne 'x64' -or $manifest.configuration -cne 'Release' -or
        $manifest.variant -cnotin @('skk', 'zenz-cpu') -or
        ($manifest.format_version -ge 2 -and $manifest.variant -cne 'zenz-cpu')) {
        throw '未対応のパッケージ形式です。'
    }
    $names = @(Get-PackageNames ($manifest.variant -ceq 'zenz-cpu') $manifest.format_version)
    if ($Contents.Count -ne $names.Count + 1 -or $manifest.files.Count -ne $names.Count) { throw 'ファイル一覧が一致しません。' }
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
    foreach ($file in $manifest.files) {
        if ($file.path -cnotin $names -or -not $seen.Add($file.path) -or -not $Contents.ContainsKey($file.path)) {
            throw "不足・余分・重複したファイル: $($file.path)"
        }
        if ($file.size -ne $Contents[$file.path].Length -or $file.sha256 -cne (Get-Digest $Contents[$file.path])) {
            throw "ハッシュまたは長さが一致しません: $($file.path)"
        }
    }
    # Old archives remain readable; paired bundles require provenance for both
    # builds and their own PE architecture, even after rehashing a wrong image.
    if ($manifest.format_version -ge 4 -and
        ($manifest.PSObject.Properties.Name -notcontains 'binary_build_metadata' -or
         $manifest.PSObject.Properties.Name -notcontains 'x86_build_metadata')) { throw 'ビルド情報が不足しています。' }
    if ($manifest.PSObject.Properties.Name -contains 'binary_build_metadata') {
        $identities = @{}
        foreach ($name in $names | Where-Object { $_.StartsWith('bin/') }) {
            Assert-PeArchitecture $Contents[$name] $name
            $embedded = Get-BuildMetadata $Contents[$name] $name
            $arch = if ($name.StartsWith('bin/x86/')) { 'x86' } else { 'x64' }
            $identity = Get-MetadataIdentity $embedded
            if ($identities.ContainsKey($arch) -and $identities[$arch] -cne $identity) { throw '異なるビルドのバイナリーが混在しています。' }
            $identities[$arch] = $identity
        }
        if ($identities.x64 -cne (Get-MetadataIdentity $manifest.binary_build_metadata) -or
            $manifest.binary_build_revision -cne $manifest.binary_build_metadata.source_revision) { throw 'マニフェストのビルド情報が一致しません。' }
        if ($manifest.format_version -ge 4) {
            if ($identities.x86 -cne (Get-MetadataIdentity $manifest.x86_build_metadata)) { throw 'マニフェストのビルド情報が一致しません。' }
            Assert-CompanionSource $manifest.binary_build_metadata $manifest.x86_build_metadata
        }
    }
    return $manifest
}
function Read-Package([string]$Path) {
    $archive = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $contents = [Collections.Generic.Dictionary[string, byte[]]]::new([StringComparer]::Ordinal)
        $caseNames = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $total = 0L
        if ($archive.Entries.Count -gt 32) { throw 'パッケージのファイル数が上限を超えています。' }
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName
            if ($name -cnotmatch '^[A-Za-z0-9_.-]+(/[A-Za-z0-9_.-]+)*$' -or
                @($name.Split('/') | Where-Object { $_ -in @('.', '..') -or $_.EndsWith('.') }).Count -or
                -not $caseNames.Add($name)) { throw "不正または重複したファイル名: $name" }
            $total += $entry.Length
            if ($entry.Length -gt 128MB -or $total -gt 256MB) { throw 'パッケージのサイズが上限を超えています。' }
            $stream = $entry.Open()
            $memory = [IO.MemoryStream]::new()
            try {
                # Read at most the declared size plus one byte, even for malformed ZIPs.
                $buffer = [byte[]]::new(65536)
                $remaining = $entry.Length + 1
                while ($remaining -gt 0) {
                    $read = $stream.Read($buffer, 0, [int][Math]::Min($buffer.Length, $remaining))
                    if (-not $read) { break }
                    $memory.Write($buffer, 0, $read)
                    $remaining -= $read
                }
                if ($memory.Length -ne $entry.Length) { throw "ZIPの長さが一致しません: $name" }
                $contents.Add($name, $memory.ToArray())
            } finally { $memory.Dispose(); $stream.Dispose() }
        }
        return Test-PackageContents $contents
    } finally { $archive.Dispose() }
}
if ($PSCmdlet.ParameterSetName -eq 'Directory') {
    $path = [IO.Path]::GetFullPath($VerifyDirectory)
    $root = Get-Item -LiteralPath $path -Force
    if (-not $root.PSIsContainer) { throw 'パッケージのディレクトリーを指定してください。' }
    for ($ancestor = $root; $null -ne $ancestor; $ancestor = $ancestor.Parent) {
        if ($ancestor.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw '再解析ポイントは検査できません。' }
    }
    # Installed folders of earlier versions remain verifiable for rollback.
    $names = @(Get-PackageNames $true 4) + @(Get-PackageNames $true 5) + @('manifest.json') | Sort-Object -Unique -CaseSensitive
    $contents = [Collections.Generic.Dictionary[string, byte[]]]::new([StringComparer]::Ordinal)
    $folders = [Collections.Generic.Queue[string]]::new()
    $folders.Enqueue($path)
    $total = 0L
    while ($folders.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $folders.Dequeue() -Force) {
            $relative = [IO.Path]::GetRelativePath($path, $item.FullName).Replace('\', '/')
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "再解析ポイントは検査できません: $relative" }
            if ($item.PSIsContainer) {
                if ($relative -cnotin @('bin', 'bin/x86', 'licenses', 'tools')) { throw "余分なディレクトリー: $relative" }
                $folders.Enqueue($item.FullName)
                continue
            }
            if ($relative -cnotin $names) { throw "余分なファイル: $relative" }
            $stream = [IO.File]::Open($item.FullName, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::Read)
            $memory = [IO.MemoryStream]::new()
            try {
                $size = $stream.Length
                $total += $size
                if ($size -gt 128MB -or $total -gt 256MB) { throw 'パッケージのサイズが上限を超えています。' }
                $buffer = [byte[]]::new(65536)
                $remaining = $size + 1
                while ($remaining -gt 0) {
                    $read = $stream.Read($buffer, 0, [int][Math]::Min($buffer.Length, $remaining))
                    if (-not $read) { break }
                    $memory.Write($buffer, 0, $read)
                    $remaining -= $read
                }
                if ($memory.Length -ne $size) { throw "ファイルの長さが変わりました: $relative" }
                $contents.Add($relative, $memory.ToArray())
            } finally { $memory.Dispose(); $stream.Dispose() }
        }
    }
    $manifest = Test-PackageContents $contents
    [pscustomobject]@{ Path = $path; Variant = $manifest.variant; Files = $manifest.files.Count;
        FormatVersion = $manifest.format_version; ManifestSHA256 = (Get-Digest $contents['manifest.json']);
        Paths = @($contents.Keys | Sort-Object -CaseSensitive); Verified = $true }
    return
}
if ($PSCmdlet.ParameterSetName -eq 'Verify') {
    $path = [IO.Path]::GetFullPath($Verify)
    $manifest = Read-Package $path
    [pscustomobject]@{ Path = $path; Variant = $manifest.variant; Files = $manifest.files.Count;
        SHA256 = (Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant(); Verified = $true }
    return
}
if (-not $SourceDirectory) {
    $SourceDirectory = Join-Path $repository 'out/build/windows-x64-zenz/Release'
}
$source = [IO.Path]::GetFullPath($SourceDirectory)
$destination = [IO.Path]::GetFullPath($Output)
if (Test-Path -LiteralPath $destination) { throw "出力先は既に存在します。上書きしません: $destination" }
if ([IO.Path]::GetFileName($source) -cne 'Release') { throw 'Releaseディレクトリーを指定してください。' }
$cache = Get-Content -LiteralPath (Join-Path (Split-Path -Parent $source) 'CMakeCache.txt') -Raw
if ($cache -notmatch '(?m)^CMAKE_GENERATOR_PLATFORM:INTERNAL=x64\r?$') { throw 'x64ビルドではありません。' }
if ($cache -notmatch '(?m)^NSKKZIME_WITH_ZENZ:BOOL=ON\r?$') { throw 'zenzを含まないテスト専用ビルドは配布できません。' }
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if ($LASTEXITCODE -ne 0 -or -not $vs) { throw 'Build Tools 2022が見つかりません。' }
$toolset = (Get-Content -LiteralPath (Join-Path $vs 'VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt') -Raw).Trim()
$dumpbin = Join-Path $vs "VC/Tools/MSVC/$toolset/bin/Hostx64/x64/dumpbin.exe"
$systemLibraries = @('kernel32.dll', 'user32.dll', 'gdi32.dll', 'advapi32.dll', 'ole32.dll', 'oleaut32.dll',
    'shell32.dll', 'bcrypt.dll', 'comctl32.dll', 'uuid.dll', 'imm32.dll', 'msctf.dll', 'shlwapi.dll', 'userenv.dll')
$runtimeLibraries = @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$sources = [ordered]@{}
foreach ($name in $binaries) { $sources['bin/' + $name] = Join-Path $source $name }
$sources['bin/x86/nskkzime_tsf.dll'] = Join-Path $source 'x86/nskkzime_tsf.dll'
$sources['bin/x86/ime_probe.exe'] = Join-Path $source 'x86/ime_probe.exe'
$sources['bin/nkkz-ime-server.exe'] = Join-Path $source 'nkkz-ime-server.exe'
$sources['bin/nskkzime_broker.exe'] = Join-Path $source 'nskkzime_broker.exe'
$sources['README.md'] = Join-Path $repository 'docs/PACKAGE_README.md'
$sources['tools/package.ps1'] = $PSCommandPath
$sources['tools/install.ps1'] = Join-Path $PSScriptRoot 'install.ps1'
$sources['tools/installation.psm1'] = Join-Path $PSScriptRoot 'installation.psm1'
$sources['licenses/nlohmann-MIT.txt'] = Join-Path $repository 'third_party/nlohmann/LICENSE.MIT'
$sources['licenses/Unicode-3.0.txt'] = Join-Path $repository 'third_party/unicode/LICENSE.txt'
$sources['licenses/skk-zenz-MIT.txt'] = Join-Path $repository 'third_party/skk-zenz/LICENSE'
$sources['licenses/llama-MIT.txt'] = Join-Path $repository 'third_party/llama.LICENSE'
$payload = [ordered]@{}
$files = @()
$buildMetadata = $null
$x86Metadata = $null
foreach ($name in @(Get-PackageNames $true)) {
    $path = $sources[$name]
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -eq 0 -or $bytes.Length -gt 128MB) { throw "ファイルサイズが不正です: $name" }
    $record = [ordered]@{ path = $name; size = $bytes.Length; sha256 = (Get-Digest $bytes) }
    if ($name.StartsWith('bin/')) {
        Assert-PeArchitecture $bytes $name
        $embedded = Get-BuildMetadata $bytes $name
        $dump = & $dumpbin /nologo /dependents $path 2>&1
        if ($LASTEXITCODE -ne 0) { throw "依存DLLを調べられません: $name" }
        $imports = @($dump | ForEach-Object { if ($_ -match '^\s+([A-Za-z0-9_.-]+\.dll)\s*$') { $Matches[1].ToLowerInvariant() } } | Sort-Object -Unique)
        if (-not $imports.Count) { throw "依存DLLが検出されません: $name" }
        foreach ($library in $imports) {
            if ($library -notin $systemLibraries -and $library -notin $runtimeLibraries -and
                $library -notmatch '^api-ms-win-crt-[a-z0-9-]+\.dll$') {
                throw "未確認の依存DLL（Debugランタイムを含む）: $name -> $library"
            }
        }
        $record['imports'] = $imports
        $record['authenticode_status'] = (Get-AuthenticodeSignature -LiteralPath $path).Status.ToString()
        if ((Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant() -cne $record.sha256) { throw "検査中にファイルが変更されました: $name" }
        if ($name.StartsWith('bin/x86/')) {
            if ($null -ne $x86Metadata -and (Get-MetadataIdentity $x86Metadata) -cne (Get-MetadataIdentity $embedded)) { throw '異なるビルドのバイナリーが混在しています。' }
            $x86Metadata = $embedded
        } else {
            if ($null -ne $buildMetadata -and (Get-MetadataIdentity $buildMetadata) -cne (Get-MetadataIdentity $embedded)) { throw '異なるビルドのバイナリーが混在しています。全対象を再ビルドしてください。' }
            $buildMetadata = $embedded
        }
    }
    $payload[$name] = $bytes
    $files += [pscustomobject]$record
}
Assert-CompanionSource $buildMetadata $x86Metadata
$revision = & git -C $repository rev-parse --verify HEAD
if ($LASTEXITCODE -ne 0) { throw 'ソースのGit revisionを取得できません。' }
& git -C $repository diff --quiet HEAD --
$dirtyCode = $LASTEXITCODE
if ($dirtyCode -notin @(0, 1)) { throw 'ソースの状態を取得できません。' }
$manifest = [ordered]@{
    format_version = 5; product = 'nskkzime'; version = '0.0.1-dev'; architecture = 'x64'; configuration = 'Release'
    variant = 'zenz-cpu'
    source_revision_at_packaging = $revision.Trim(); tracked_source_changes = ($dirtyCode -eq 1)
    binary_build_revision = $buildMetadata.source_revision
    binary_build_metadata = $buildMetadata
    x86_build_metadata = $x86Metadata
    provenance_note = 'Build metadata is embedded during compilation; it is not signed attestation or publisher authentication. Modified source is identified separately from its base revision.'
    cpu_requirement = 'zenz-msvc-avx2-v1: native x64; SSE3/SSSE3/SSE4.1/SSE4.2/POPCNT/AVX/AVX2/FMA/F16C/BMI1/BMI2/LZCNT; XSAVE/OSXSAVE and OS XMM/YMM state'
    runtime_requirement = 'Microsoft Visual C++ v14 Redistributable x64 and x86, at least the toolset version used to build'
    inspected_with_msvc_tools = $toolset
    signing_note = 'Packaging does not sign files. SHA-256 verification checks consistency, not publisher identity.'
    first_party_license = 'Not yet specified; development validation package, not a public release.'
    includes_model = $false; includes_dictionary = $false; includes_user_settings = $false
    files = $files
}
$payload['manifest.json'] = [Text.Encoding]::UTF8.GetBytes(($manifest | ConvertTo-Json -Depth 8) + "`n")
$parent = Split-Path -Parent $destination
[IO.Directory]::CreateDirectory($parent) | Out-Null
$temporary = $destination + '.tmp-' + [Guid]::NewGuid().ToString('N')
try {
    $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try {
        $zip = [IO.Compression.ZipArchive]::new($stream, [IO.Compression.ZipArchiveMode]::Create, $true)
        try {
            foreach ($name in @($payload.Keys | Sort-Object -CaseSensitive)) {
                $entry = $zip.CreateEntry($name, [IO.Compression.CompressionLevel]::Optimal)
                $entry.LastWriteTime = [DateTimeOffset]::new(1980, 1, 1, 0, 0, 0, [TimeSpan]::Zero)
                $target = $entry.Open()
                try { $target.Write($payload[$name], 0, $payload[$name].Length) } finally { $target.Dispose() }
            }
        } finally { $zip.Dispose() }
    } finally { $stream.Dispose() }
    $null = Read-Package $temporary
    [IO.File]::Move($temporary, $destination, $false)
} finally {
    if (Test-Path -LiteralPath $temporary -PathType Leaf) { Remove-Item -LiteralPath $temporary }
}
[pscustomobject]@{ Path = $destination; Variant = $manifest.variant; Files = $files.Count;
    SHA256 = (Get-FileHash -LiteralPath $destination).Hash.ToLowerInvariant(); Verified = $true }
