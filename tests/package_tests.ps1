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
    Assert-Package ($verified.Verified -and $verified.Files -eq 18 -and $verified.Variant -eq 'zenz-cpu') 'Incomplete zenz package.'
    $expanded = Join-Path $fixture 'expanded'
    [IO.Compression.ZipFile]::ExtractToDirectory($created.Path, $expanded)
    Assert-Package ((& $PackageScript -VerifyDirectory $expanded).Verified) 'Expanded package verification failed.'
    [IO.File]::AppendAllText((Join-Path $expanded 'README.md'), 'corrupt')
    Expect-Failure { & $PackageScript -VerifyDirectory $expanded } 'ハッシュまたは長さ'
    Write-Output 'PASS zenz package creation, archive and expanded verification, tamper detection'
} finally {
    # Only this uniquely named workspace fixture may be recursively removed.
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
