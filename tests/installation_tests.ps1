#Requires -Version 7.0
param([Parameter(Mandatory)][string]$BuildDirectory, [Parameter(Mandatory)][string]$Configuration)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Configuration -ne 'Release') { Write-Output 'SKIP installation fixtures require Release'; exit 77 }
$repository = Split-Path -Parent $PSScriptRoot
$verifier = Join-Path $repository 'scripts/package.ps1'
Import-Module (Join-Path $repository 'scripts/installation.psm1') -Force
$fixture = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('install-test 日本語 space-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$root = Join-Path $fixture 'managed'
$state = @{ Dll = $null; X86Dll = $null; Startup = $null; StartupFailure = $false; Calls = [Collections.Generic.List[string]]::new(); Failures = [Collections.Generic.List[object]]::new() }
$setStartup = {
    param([string]$Broker)
    $state.Startup = $Broker
    if ($state.StartupFailure) { $state.StartupFailure = $false; throw 'startup write interrupted' }
}.GetNewClosure()
$readRegistration = { return $state.Dll }.GetNewClosure()
$readX86Registration = { return $state.X86Dll }.GetNewClosure()
$registrar = {
    param([string]$Dll, [bool]$Unregister)
    $operation = if ($Unregister) { 'remove' } else { 'add' }
    $state.Calls.Add("${operation}:$Dll")
    $failure = $null
    if ($state.Failures.Count -and $state.Failures[0].Dll -eq $Dll -and $state.Failures[0].Operation -eq $operation) {
        $failure = $state.Failures[0]; $state.Failures.RemoveAt(0)
    }
    if ($failure -and $failure.Mode -eq 'before') { return 5 }
    if ($failure -and $failure.Mode -eq 'lie') { return 0 }
    $slot = if ([IO.Path]::GetFileName([IO.Path]::GetDirectoryName($Dll)) -eq 'x86') { 'X86Dll' } else { 'Dll' }
    $state[$slot] = if ($Unregister) { $null } else { $Dll }
    if ($failure -and $failure.Mode -eq 'foreign') { $state.Dll = 'C:\foreign\nskkzime_tsf.dll'; return 5 }
    if ($failure) { return 5 }
    return 0
}.GetNewClosure()
function Assert-Install([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Expect-Failure([scriptblock]$Action, [string]$Expected) {
    $message = ''
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert-Install ($message.Contains($Expected)) "Expected '$Expected', got '$message'."
}
function Transition([string]$Target) { Invoke-NskkzimeTransition $root $Target $verifier $readRegistration $registrar $setStartup $readX86Registration }
function Recover { Repair-NskkzimeTransition $root $verifier $readRegistration $registrar $setStartup $readX86Registration }
function Fail-Next([string]$Dll, [string]$Operation, [string]$Mode) {
    $state.Failures.Add(@{ Dll = $Dll; Operation = $Operation; Mode = $Mode })
}
function Journal([string]$Previous, [string]$Target) {
    @{ format_version = 2; previous = $(if ($Previous) { $Previous } else { $null });
        target = $(if ($Target) { $Target } else { $null }) } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'transaction.json')
}
try {
    $status = Get-NskkzimeInstallStatus $root $readRegistration
    Assert-Install (-not (Test-Path -LiteralPath $root) -and -not $status.RegisteredDll -and $status.AvailableVersions.Count -eq 0) 'Status wrote to disk.'
    $sentinel = Join-Path $fixture 'settings-and-dictionary.txt'
    [IO.File]::WriteAllText($sentinel, 'user data must survive')
    $sentinelHash = (Get-FileHash -LiteralPath $sentinel).Hash
    $zip = & $verifier -SourceDirectory (Join-Path $BuildDirectory 'Release') -Output (Join-Path $fixture 'package.zip')
    $first = Join-Path $fixture 'first'
    $second = Join-Path $fixture 'second'
    [IO.Compression.ZipFile]::ExtractToDirectory($zip.Path, $first)
    [IO.Compression.ZipFile]::ExtractToDirectory($zip.Path, $second)
    # Simulate an existing format-2 install: no broker, but identical trusted
    # binary metadata. New installations must still be able to roll back to it.
    [IO.File]::Delete((Join-Path $first 'bin/nskkzime_broker.exe'))
    [IO.File]::Delete((Join-Path $first 'bin/x86/nskkzime_tsf.dll'))
    [IO.File]::Delete((Join-Path $first 'bin/x86/ime_probe.exe'))
    [IO.Directory]::Delete((Join-Path $first 'bin/x86'))
    $legacyPath = Join-Path $first 'manifest.json'
    $legacy = Get-Content -LiteralPath $legacyPath -Raw | ConvertFrom-Json
    $legacy.format_version = 2
    $legacy.files = @($legacy.files | Where-Object { $_.path -ne 'bin/nskkzime_broker.exe' -and -not $_.path.StartsWith('bin/x86/') })
    $legacy.PSObject.Properties.Remove('x86_build_metadata')
    $legacy | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $legacyPath
    $readme = Join-Path $second 'README.md'
    [IO.File]::AppendAllText($readme, "`nFixture second version.`n")
    $manifestPath = Join-Path $second 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $entry = $manifest.files | Where-Object path -eq README.md
    $entry.size = (Get-Item -LiteralPath $readme).Length
    $entry.sha256 = (Get-FileHash -LiteralPath $readme).Hash.ToLowerInvariant()
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $lock = Enter-NskkzimeInstallLock $root
    try { Expect-Failure { Enter-NskkzimeInstallLock $root } '別のインストール処理' } finally { $lock.Dispose() }
    $lock = Enter-NskkzimeInstallLock $root
    $lock.Dispose()
    $a = Add-NskkzimeVersion $root $first $verifier
    $b = Add-NskkzimeVersion $root $second $verifier
    Assert-Install ($a.Version -ne $b.Version) 'Distinct bundles have same version.'
    $reused = Add-NskkzimeVersion $root $first $verifier
    Assert-Install ($reused.Version -eq $a.Version) 'Failed to reuse intact version.'
    Expect-Failure { Get-NskkzimeInstalledBundle $root '../escape' $verifier } '64桁'
    Assert-Install ((Transition $a.Version).Result -eq 'Changed' -and $state.Dll -eq $a.Dll) 'Initial install failed.'
    $count = $state.Calls.Count
    Assert-Install ((Transition $a.Version).Result -eq 'Unchanged' -and $state.Calls.Count -eq $count) 'Same version was re-registered.'
    Transition $b.Version | Out-Null
    Assert-Install ($state.Dll -eq $b.Dll -and $state.X86Dll -eq $b.X86Dll) 'Paired update failed.'
    Assert-Install ($state.Startup -eq $b.Broker -and $b.Broker) 'Broker startup missing after update.'
    Transition $a.Version | Out-Null
    Assert-Install ($state.Dll -eq $a.Dll -and -not $state.X86Dll) 'Rollback to x64-only bundle failed.'
    Assert-Install (-not $state.Startup) 'Rollback to legacy version retained broker startup.'
    $state.StartupFailure = $true
    Expect-Failure { Transition $b.Version } '変更前の登録へ復帰'
    Assert-Install ($state.Dll -eq $a.Dll -and -not $state.Startup) 'Interrupted startup write was not rolled back.'
    Transition $b.Version | Out-Null
    Transition $null | Out-Null
    Assert-Install (-not $state.Startup) 'Unregister retained broker startup.'
    Assert-Install (-not $state.Dll -and (Test-Path -LiteralPath $a.Dll) -and (Test-Path -LiteralPath $b.Dll)) 'Unregister removed files or retained registration.'
    Fail-Next $a.Dll add after
    Expect-Failure { Transition $a.Version } '変更前の登録へ復帰'
    Assert-Install (-not $state.Dll -and -not (Test-Path -LiteralPath (Join-Path $root 'transaction.json'))) 'Failed first install was not cleaned.'
    Transition $a.Version | Out-Null
    foreach ($mode in @('before', 'after', 'lie')) {
        Fail-Next $b.Dll add $mode
        Expect-Failure { Transition $b.Version } '変更前の登録へ復帰'
        Assert-Install ($state.Dll -eq $a.Dll -and $state.Failures.Count -eq 0) "Failed update $mode lost previous version."
    }
    foreach ($mode in @('before', 'after')) {
        Fail-Next $a.Dll remove $mode
        Expect-Failure { Transition $b.Version } '変更前の登録へ復帰'
        Assert-Install ($state.Dll -eq $a.Dll) "Partial unregister $mode was not repaired."
    }
    Fail-Next $b.Dll add after
    Fail-Next $b.Dll remove before
    Expect-Failure { Transition $b.Version } '更新と復帰に失敗'
    Assert-Install ($state.Dll -eq $b.Dll -and (Test-Path -LiteralPath (Join-Path $root 'transaction.json'))) 'Failed rollback lost recovery journal.'
    $count = $state.Calls.Count
    Expect-Failure { Transition $a.Version } 'Recoverを先に'
    Assert-Install ($state.Calls.Count -eq $count) 'Pending operation was overwritten.'
    Assert-Install ((Recover).Result -eq 'Recovered' -and $state.Dll -eq $a.Dll) 'Retry failed to restore old version.'
    Assert-Install ((Recover).Result -eq 'NoPendingTransaction') 'Recovery was not idempotent.'
    foreach ($current in @($null, $a.Dll, $b.Dll)) {
        Journal $a.Version $b.Version
        $state.Dll = $current; $state.X86Dll = $null
        Recover | Out-Null
        Assert-Install ($state.Dll -eq $a.Dll) 'Interrupted update recovery failed.'
    }
    Journal $null $a.Version
    Recover | Out-Null
    Assert-Install (-not $state.Dll) 'Interrupted first install recovery failed.'
    Journal $a.Version $null
    Recover | Out-Null
    Assert-Install ($state.Dll -eq $a.Dll) 'Interrupted uninstall recovery failed.'
    $state.Dll = 'C:\foreign\nskkzime_tsf.dll'
    $count = $state.Calls.Count
    Expect-Failure { Transition $b.Version } '管理外'
    Assert-Install ($state.Calls.Count -eq $count -and $state.Dll.StartsWith('C:\foreign')) 'Unmanaged registration changed.'
    $state.Dll = $a.Dll
    Fail-Next $b.Dll add foreign
    Expect-Failure { Transition $b.Version } '更新と復帰に失敗'
    $count = $state.Calls.Count
    Expect-Failure { Recover } '管理外'
    Assert-Install ($state.Calls.Count -eq $count) 'Recovery touched concurrent foreign registration.'
    $state.Dll = $b.Dll
    Recover | Out-Null
    foreach ($mode in @('before', 'after', 'lie')) {
        Fail-Next $b.X86Dll add $mode
        Expect-Failure { Transition $b.Version } '変更前の登録へ復帰'
        Assert-Install ($state.Dll -eq $a.Dll -and -not $state.X86Dll) "Failed x86 registration $mode was not restored."
    }
    Transition $b.Version | Out-Null
    foreach ($mode in @('before', 'after')) {
        Fail-Next $b.X86Dll remove $mode
        Expect-Failure { Transition $a.Version } '変更前の登録へ復帰'
        Assert-Install ($state.Dll -eq $b.Dll -and $state.X86Dll -eq $b.X86Dll) "Failed x86 removal $mode lost pair."
    }
    foreach ($native in @($null, $a.Dll, $b.Dll)) {
        foreach ($companion in @($null, $b.X86Dll)) {
            Journal $b.Version $a.Version
            $state.Dll = $native; $state.X86Dll = $companion
            Recover | Out-Null
            Assert-Install ($state.Dll -eq $b.Dll -and $state.X86Dll -eq $b.X86Dll) 'Interrupted paired rollback did not restore both views.'
        }
    }
    Transition $a.Version | Out-Null
    $state.X86Dll = 'C:\foreign\x86\nskkzime_tsf.dll'
    $count = $state.Calls.Count
    Expect-Failure { Transition $b.Version } '登録先が変更'
    Assert-Install ($state.Calls.Count -eq $count -and $state.Dll -eq $a.Dll) 'Foreign x86 owner changed x64 registration.'
    $state.X86Dll = $null
    $third = Join-Path $fixture 'third'
    [IO.Compression.ZipFile]::ExtractToDirectory($zip.Path, $third)
    $thirdReadme = Join-Path $third 'README.md'
    [IO.File]::AppendAllText($thirdReadme, "`nPaired fixture third version.`n")
    $thirdManifestPath = Join-Path $third 'manifest.json'
    $thirdManifest = Get-Content $thirdManifestPath -Raw | ConvertFrom-Json
    $thirdEntry = $thirdManifest.files | Where-Object path -eq 'README.md'
    $thirdEntry.size = (Get-Item $thirdReadme).Length
    $thirdEntry.sha256 = (Get-FileHash $thirdReadme).Hash.ToLowerInvariant()
    $thirdManifest | ConvertTo-Json -Depth 8 | Set-Content $thirdManifestPath
    $c = Add-NskkzimeVersion $root $third $verifier
    Transition $b.Version | Out-Null
    Transition $c.Version | Out-Null
    Assert-Install ($state.Dll -eq $c.Dll -and $state.X86Dll -eq $c.X86Dll) 'Paired to paired update failed.'
    $count = $state.Calls.Count
    Assert-Install ((Transition $c.Version).Result -eq 'Unchanged' -and $state.Calls.Count -eq $count) 'Paired unchanged version re-registered.'
    Journal $c.Version $b.Version
    $state.Dll = $b.Dll; $state.X86Dll = $c.X86Dll
    Recover | Out-Null
    Assert-Install ($state.Dll -eq $c.Dll -and $state.X86Dll -eq $c.X86Dll) 'Mixed paired update did not restore coherent pair.'
    Journal $c.Version $b.Version
    $recordPath = Join-Path $root 'transaction.json'
    $oldRecord = Get-Content $recordPath -Raw | ConvertFrom-Json
    $oldRecord.format_version = 1
    $oldRecord | ConvertTo-Json | Set-Content $recordPath
    $state.Dll = $c.Dll; $state.X86Dll = $c.X86Dll
    $count = $state.Calls.Count
    Expect-Failure { Recover } '旧形式'
    Assert-Install ($state.Calls.Count -eq $count) 'Legacy journal removed companion.'
    [IO.File]::Delete($recordPath)
    Transition $a.Version | Out-Null
    foreach ($bundle in @($a, $b)) {
        $path = Join-Path $bundle.Directory 'README.md'
        $bytes = [IO.File]::ReadAllBytes($path)
        try {
            [IO.File]::AppendAllText($path, 'tampered')
            $count = $state.Calls.Count
            Expect-Failure { Transition $b.Version } 'ハッシュまたは長さ'
            Assert-Install ($state.Calls.Count -eq $count) 'Tampered bundle reached registrar.'
            if ($bundle.Version -eq $a.Version) {
                Expect-Failure { Add-NskkzimeVersion $root $first $verifier } 'ハッシュまたは長さ'
            }
        } finally { [IO.File]::WriteAllBytes($path, $bytes) }
    }
    [IO.File]::WriteAllText((Join-Path $root 'transaction.json'), '{"format_version":1,"previous":"../escape","target":null}')
    $count = $state.Calls.Count
    Expect-Failure { Recover } '版IDが不正'
    Assert-Install ($state.Calls.Count -eq $count) 'Invalid journal reached registrar.'
    [IO.File]::Delete((Join-Path $root 'transaction.json'))
    $status = Get-NskkzimeInstallStatus $root $readRegistration
    Assert-Install ($status.ManagedVersion -eq $a.Version -and $status.AvailableVersions.Count -eq 3 -and -not $status.Pending) 'Status is inconsistent.'
    Assert-Install ((Get-FileHash -LiteralPath $sentinel).Hash -eq $sentinelHash) 'User data changed.'
    Write-Output 'PASS install/update/rollback/unregister, exclusive lock, hash checks, foreign registration, partial failures, interrupted recovery, preserved journals, read-only status and retained files'
} finally {
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Assert-NskkzimePath $absolute
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
