#Requires -Version 7.0
[CmdletBinding(DefaultParameterSetName = 'Create')]
param(
    [Parameter(ParameterSetName = 'Create')][string]$SourceDirectory,
    [Parameter(ParameterSetName = 'Create')][switch]$Zenz, # Legacy no-op; creation always includes zenz.
    [Parameter(Mandatory, ParameterSetName = 'Create')][string]$Output,
    [Parameter(Mandatory, ParameterSetName = 'Verify')][string]$Verify
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$binaries = @('nskkzime_tsf.dll', 'nskkzime_dictionary_server.exe', 'nskkzime_settings.exe',
    'nskkzime_dictionary_gui.exe', 'nskkzime_config.exe', 'nskkzime_user_dictionary.exe', 'ime_probe.exe')
$baseLicenses = @('licenses/nlohmann-MIT.txt', 'licenses/Unicode-3.0.txt')
$zenzLicenses = @('licenses/skk-zenz-MIT.txt', 'licenses/llama-MIT.txt')
function Get-Digest([byte[]]$Bytes) {
    [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}
function Get-PackageNames([bool]$WithZenz) {
    # Read-only compatibility with previously generated SKK archives. New
    # packages always pass true; no separate SKK build or release is produced.
    $names = @($binaries | ForEach-Object { 'bin/' + $_ }) + $baseLicenses + @('README.md', 'tools/package.ps1')
    if ($WithZenz) { $names += @('bin/nkkz-ime-server.exe') + $zenzLicenses }
    $names | Sort-Object -CaseSensitive
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
        if (-not $contents.ContainsKey('manifest.json')) { throw 'manifest.jsonがありません。' }
        $manifest = [Text.Encoding]::UTF8.GetString($contents['manifest.json']) | ConvertFrom-Json
        if ($manifest.format_version -ne 1 -or $manifest.architecture -cne 'x64' -or
            $manifest.configuration -cne 'Release' -or $manifest.variant -cnotin @('skk', 'zenz-cpu')) {
            throw '未対応のパッケージ形式です。'
        }
        $names = @(Get-PackageNames ($manifest.variant -ceq 'zenz-cpu'))
        if ($contents.Count -ne $names.Count + 1 -or $manifest.files.Count -ne $names.Count) { throw 'ファイル一覧が一致しません。' }
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
        foreach ($file in $manifest.files) {
            if ($file.path -cnotin $names -or -not $seen.Add($file.path) -or -not $contents.ContainsKey($file.path)) {
                throw "不足・余分・重複したファイル: $($file.path)"
            }
            if ($file.size -ne $contents[$file.path].Length -or $file.sha256 -cne (Get-Digest $contents[$file.path])) {
                throw "ハッシュまたは長さが一致しません: $($file.path)"
            }
        }
        return $manifest
    } finally { $archive.Dispose() }
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
    'shell32.dll', 'bcrypt.dll', 'comctl32.dll', 'uuid.dll', 'imm32.dll', 'msctf.dll', 'shlwapi.dll')
$runtimeLibraries = @('msvcp140.dll', 'msvcp140_atomic_wait.dll', 'msvcp140_codecvt_ids.dll', 'vcruntime140.dll', 'vcruntime140_1.dll')
$sources = [ordered]@{}
foreach ($name in $binaries) { $sources['bin/' + $name] = Join-Path $source $name }
$sources['bin/nkkz-ime-server.exe'] = Join-Path $source 'nkkz-ime-server.exe'
$sources['README.md'] = Join-Path $repository 'docs/PACKAGE_README.md'
$sources['tools/package.ps1'] = $PSCommandPath
$sources['licenses/nlohmann-MIT.txt'] = Join-Path $repository 'third_party/nlohmann/LICENSE.MIT'
$sources['licenses/Unicode-3.0.txt'] = Join-Path $repository 'third_party/unicode/LICENSE.txt'
$sources['licenses/skk-zenz-MIT.txt'] = Join-Path $repository 'third_party/skk-zenz/LICENSE'
$sources['licenses/llama-MIT.txt'] = Join-Path $repository 'third_party/llama.LICENSE'
$payload = [ordered]@{}
$files = @()
foreach ($name in @(Get-PackageNames $true)) {
    $path = $sources[$name]
    $bytes = [IO.File]::ReadAllBytes($path)
    if ($bytes.Length -eq 0 -or $bytes.Length -gt 128MB) { throw "ファイルサイズが不正です: $name" }
    $record = [ordered]@{ path = $name; size = $bytes.Length; sha256 = (Get-Digest $bytes) }
    if ($name.StartsWith('bin/')) {
        # Check PE32+ / AMD64 without loading or running the image.
        if ($bytes.Length -lt 64 -or $bytes[0] -ne 0x4d -or $bytes[1] -ne 0x5a) { throw "PEではありません: $name" }
        $pe = [BitConverter]::ToUInt32($bytes, 60)
        if ($pe -gt $bytes.Length - 26 -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550 -or
            [BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x8664 -or [BitConverter]::ToUInt16($bytes, $pe + 24) -ne 0x20b) {
            throw "x64 PE32+ではありません: $name"
        }
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
    }
    $payload[$name] = $bytes
    $files += [pscustomobject]$record
}
$revision = & git -C $repository rev-parse --verify HEAD
if ($LASTEXITCODE -ne 0) { throw 'ソースのGit revisionを取得できません。' }
& git -C $repository diff --quiet HEAD --
$dirtyCode = $LASTEXITCODE
if ($dirtyCode -notin @(0, 1)) { throw 'ソースの状態を取得できません。' }
$manifest = [ordered]@{
    format_version = 1; product = 'nskkzime'; version = '0.0.1-dev'; architecture = 'x64'; configuration = 'Release'
    variant = 'zenz-cpu'
    source_revision_at_packaging = $revision.Trim(); tracked_source_changes = ($dirtyCode -eq 1)
    binary_build_revision = $null
    provenance_note = 'The packaging revision is not a proof of which source produced existing binaries.'
    cpu_requirement = 'x64; zenz requires AVX2/FMA/F16C/BMI2 and OS AVX support'
    runtime_requirement = 'Microsoft Visual C++ v14 Redistributable x64, at least the toolset version used to build'
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
