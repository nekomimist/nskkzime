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
$arguments = @{ SourceDirectory = (Join-Path $BuildDirectory $Configuration); Output = (Join-Path $fixture 'package.zip') }
function Assert-Package([bool]$Condition, [string]$Reason) { if (-not $Condition) { throw $Reason } }
function Expect-Failure([scriptblock]$Action, [string]$Expected) {
    $message = $null
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert-Package ($null -ne $message -and $message.Contains($Expected)) "Expected '$Expected', got '$message'."
}
try {
    if ($Configuration -eq 'Debug' -or $TestOnly) {
        Expect-Failure { & $PackageScript @arguments } $(if ($TestOnly) { 'テスト専用ビルドは配布できません' } else { 'Releaseディレクトリー' })
        Assert-Package (-not (Test-Path -LiteralPath $arguments.Output)) 'A non-distributable build was packaged.'
        Write-Output 'PASS non-distributable build rejected'
        return
    }
    # The product package is created, verifies as an archive and as expanded files, and detects tampering.
    $created = & $PackageScript @arguments
    $verified = & $PackageScript -Verify $created.Path
    $cache = Get-Content -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt') -Raw
    Assert-Package ($cache -match '(?m)^CMAKE_PROJECT_VERSION:STATIC=(?<version>[0-9]+\.[0-9]+\.[0-9]+)\r?$') 'Missing build version.'
    $expectedVersion = $Matches.version
    Assert-Package ($verified.Verified -and $verified.Files -eq 20 -and $verified.Variant -eq 'zenz-cpu' -and
        $verified.Version -ceq $expectedVersion -and $created.Version -ceq $expectedVersion) 'Incomplete or incorrectly versioned zenz package.'
    $checksum = [IO.File]::ReadAllText($created.ChecksumPath)
    Assert-Package ($checksum -ceq ($verified.SHA256 + '  package.zip' + "`n")) 'Release checksum does not match the ZIP.'
    Expect-Failure { & $PackageScript @arguments } '上書きしません'
    $occupiedOutput = Join-Path $fixture 'occupied.zip'
    [IO.File]::WriteAllText(($occupiedOutput + '.sha256'), 'existing checksum')
    Expect-Failure { & $PackageScript -SourceDirectory $arguments.SourceDirectory -Output $occupiedOutput } '上書きしません'
    Assert-Package (-not (Test-Path -LiteralPath $occupiedOutput)) 'Created a ZIP beside an existing checksum.'
    $expanded = Join-Path $fixture 'expanded'
    [IO.Compression.ZipFile]::ExtractToDirectory($created.Path, $expanded)
    $manifestPath = Join-Path $expanded 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert-Package ($manifest.format_version -eq 6 -and $manifest.first_party_license -ceq 'MIT' -and
        [IO.File]::ReadAllText((Join-Path $expanded 'LICENSE')).Contains('Copyright (c) 2026 Hiroyuki Ishikura')) 'Missing first-party license.'
    $directory = & $PackageScript -VerifyDirectory $expanded
    Assert-Package ($directory.Verified -and $directory.Version -ceq $expectedVersion) 'Expanded package verification failed.'
    # Reconfiguring without rebuilding must not relabel old binaries as a new release.
    $mismatchRoot = Join-Path $fixture 'mismatched-build'
    [IO.Directory]::CreateDirectory($mismatchRoot) | Out-Null
    Copy-Item -LiteralPath (Join-Path $expanded 'bin') -Destination (Join-Path $mismatchRoot 'Release') -Recurse
    [IO.File]::WriteAllText((Join-Path $mismatchRoot 'CMakeCache.txt'),
        ($cache -replace '(?m)^CMAKE_PROJECT_VERSION:STATIC=.*$', 'CMAKE_PROJECT_VERSION:STATIC=99.0.0'))
    Expect-Failure { & $PackageScript -SourceDirectory (Join-Path $mismatchRoot 'Release') -Output (Join-Path $fixture 'mismatch.zip') } '再ビルドしてください'
    Assert-Package (-not (Test-Path -LiteralPath (Join-Path $fixture 'mismatch.zip'))) 'Relabeled stale binaries.'
    $manifest.version = 'nightly'
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath
    Expect-Failure { & $PackageScript -VerifyDirectory $expanded } '版番号またはライセンス'
    $manifest.version = $expectedVersion
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath
    [IO.File]::AppendAllText((Join-Path $expanded 'README.md'), 'corrupt')
    Expect-Failure { & $PackageScript -VerifyDirectory $expanded } 'ハッシュまたは長さ'
    # Existing format-5 installations remain readable by the new verifier.
    [IO.File]::WriteAllBytes((Join-Path $expanded 'README.md'), [IO.File]::ReadAllBytes((Join-Path (Split-Path -Parent $PackageScript) '../docs/PACKAGE_README.md')))
    Remove-Item -LiteralPath (Join-Path $expanded 'LICENSE')
    Remove-Item -LiteralPath (Join-Path $expanded 'tools/scoop.ps1')
    $manifest.files = @($manifest.files | Where-Object { $_.path -cnotin @('LICENSE', 'tools/scoop.ps1') })
    $manifest.format_version = 5
    $manifest.version = '0.0.1-dev'
    $manifest.first_party_license = 'Not yet specified; development validation package, not a public release.'
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath
    $legacy = & $PackageScript -VerifyDirectory $expanded
    Assert-Package ($legacy.Verified -and $legacy.FormatVersion -eq 5 -and $legacy.Files -eq 18) 'Legacy installation is no longer verifiable.'
    $legacyZip = Join-Path $fixture 'legacy.zip'
    [IO.Compression.ZipFile]::CreateFromDirectory($expanded, $legacyZip)
    Assert-Package ((& $PackageScript -Verify $legacyZip).Verified) 'Legacy archive is no longer verifiable.'
    Write-Output 'PASS versioned MIT package, release checksum, overwrite and stale-build rejection, tamper detection, format-5 compatibility'
} finally {
    # Only this uniquely named workspace fixture may be recursively removed.
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
