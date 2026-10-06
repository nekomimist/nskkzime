#Requires -Version 7.0
param([Parameter(Mandatory)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
Import-Module (Join-Path $PSScriptRoot '../scripts/source-fingerprint.psm1') -Force
$fixture = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('metadata-test 日本語 space-' + [Guid]::NewGuid().ToString('N'))
$repository = Join-Path $fixture 'source'
[IO.Directory]::CreateDirectory((Join-Path $repository 'src')) | Out-Null
function Assert-Metadata([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Invoke-FixtureGit([string[]]$Arguments) {
    & git -C $repository @Arguments | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'Fixture git failed.' }
}
try {
    $source = Join-Path $repository 'src/main.cpp'
    [IO.File]::WriteAllText($source, 'int main() { return 0; }')
    Invoke-FixtureGit @('init', '--quiet')
    Invoke-FixtureGit @('add', '--', 'src/main.cpp')
    Invoke-FixtureGit @('-c', 'user.name=Build Test', '-c', 'user.email=build-test@example.invalid', 'commit', '--quiet', '-m', 'fixture')
    $clean = Get-NskkzimeSourceFingerprint $repository
    Assert-Metadata (-not $clean.Modified) 'Clean source marked modified.'
    [IO.File]::WriteAllText((Join-Path $repository 'SKK-JISYO.utf8.L'), 'User data is not a build input.')
    $unrelated = Get-NskkzimeSourceFingerprint $repository
    Assert-Metadata ($unrelated.SHA256 -eq $clean.SHA256 -and -not $unrelated.Modified) 'Unrelated dictionary affected source metadata.'
    [IO.File]::AppendAllText($source, '// changed')
    $changed = Get-NskkzimeSourceFingerprint $repository
    Assert-Metadata ($changed.Revision -eq $clean.Revision -and $changed.Modified -and $changed.SHA256 -ne $clean.SHA256) 'Working source change was not identified.'
    Invoke-FixtureGit @('checkout', '--', 'src/main.cpp')
    Assert-Metadata ((Get-NskkzimeSourceFingerprint $repository).SHA256 -eq $clean.SHA256) 'Restored source fingerprint differs.'
    [IO.File]::Delete($source)
    $deleted = Get-NskkzimeSourceFingerprint $repository
    Assert-Metadata ($deleted.Modified -and $deleted.SHA256 -ne $clean.SHA256) 'Deleted build input was omitted.'
    Invoke-FixtureGit @('checkout', '--', 'src/main.cpp')
    $newSource = Join-Path $repository 'src/日本語 new.cpp'
    [IO.File]::WriteAllText($newSource, 'int new_function() { return 1; }')
    $untracked = Get-NskkzimeSourceFingerprint $repository
    Assert-Metadata ($untracked.Modified -and $untracked.SHA256 -ne $clean.SHA256) 'New build input was omitted.'
    [IO.File]::Delete($newSource)
    $cache = Join-Path $fixture 'CMakeCache.txt'
    [IO.File]::WriteAllText($cache, 'test cache')
    $output = Join-Path $fixture 'build_metadata.cpp'
    $generator = Join-Path $PSScriptRoot '../scripts/write-build-metadata.ps1'
    $arguments = @{ Repository = $repository; Output = $output; Configuration = 'Release'; CompilerVersion = '19.44.1.0'; CacheFile = $cache; Variant = 'test-only' }
    & $generator @arguments
    [IO.File]::SetLastWriteTimeUtc($output, [DateTime]::new(2000, 1, 1, 0, 0, 0, [DateTimeKind]::Utc))
    & $generator @arguments
    Assert-Metadata ((Get-Item -LiteralPath $output).LastWriteTimeUtc.Year -eq 2000) 'Unchanged metadata rewrote compiler input.'
    $old = [IO.File]::ReadAllText($output)
    [IO.File]::AppendAllText($source, '// incremental change')
    & $generator @arguments
    Assert-Metadata ([IO.File]::ReadAllText($output) -cne $old) 'Incremental build metadata stayed stale.'
    $vendor = Join-Path $fixture 'vendor'
    [IO.Directory]::CreateDirectory($vendor) | Out-Null
    $vendorFile = Join-Path $vendor 'implementation.cpp'
    [IO.File]::WriteAllText($vendorFile, 'one')
    $vendorHash = Get-NskkzimeDirectoryFingerprint $vendor
    [IO.File]::WriteAllText($vendorFile, 'two')
    Assert-Metadata ((Get-NskkzimeDirectoryFingerprint $vendor) -ne $vendorHash) 'Modified dependency source was omitted.'
    $link = Join-Path $repository 'src/linked'
    New-Item -ItemType Junction -Path $link -Target $vendor | Out-Null
    try {
        $refused = $false
        try { Get-NskkzimeSourceFingerprint $repository | Out-Null } catch { $refused = $_.Exception.Message.Contains('再解析ポイント') }
        Assert-Metadata $refused 'Source junction was accepted.'
    } finally { [IO.Directory]::Delete($link) }
    Write-Output 'PASS clean/modified/new source, unrelated dictionary exclusion, incremental refresh, unchanged-file preservation, dependency changes and junction rejection'
} finally {
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
