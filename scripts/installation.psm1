#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-NskkzimePath([string]$Path) {
    $current = [IO.Path]::GetFullPath($Path)
    while ($current) {
        $item = $null
        try { $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop }
        catch [System.Management.Automation.ItemNotFoundException] { }
        if ($item -and ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "再解析ポイントをインストールに使用できません: $current"
        }
        $current = [IO.Path]::GetDirectoryName($current)
    }
}
function Get-NskkzimeBundle([string]$Directory, [string]$Verifier) {
    $result = & $Verifier -VerifyDirectory $Directory
    if (-not $result.Verified -or $result.FormatVersion -notin @(2, 3) -or $result.Variant -cne 'zenz-cpu') {
        throw 'インストールには形式2または3のzenz入りパッケージが必要です。'
    }
    [pscustomobject]@{ Directory = $result.Path; Version = $result.ManifestSHA256;
        Dll = (Join-Path $result.Path 'bin/nskkzime_tsf.dll'); Paths = $result.Paths;
        Broker = $(if ($result.FormatVersion -ge 3) { Join-Path $result.Path 'bin/nskkzime_broker.exe' } else { $null }) }
}
function Get-NskkzimeInstalledBundle([string]$Root, [string]$Version, [string]$Verifier) {
    if ($Version -cnotmatch '^[a-f0-9]{64}$') { throw '版IDは64桁の小文字SHA-256で指定してください。' }
    $bundle = Get-NskkzimeBundle (Join-Path $Root "versions/$Version") $Verifier
    if ($bundle.Version -cne $Version) { throw '配置先の版IDとマニフェストが一致しません。' }
    return $bundle
}
function Get-NskkzimeManagedVersion([string]$Root, [string]$Dll) {
    if (-not $Dll) { return $null }
    if (-not [IO.Path]::IsPathFullyQualified($Dll)) { throw '登録先が絶対パスではありません。' }
    $relative = [IO.Path]::GetRelativePath((Join-Path ([IO.Path]::GetFullPath($Root)) 'versions'), [IO.Path]::GetFullPath($Dll))
    if ($relative -notmatch '^([a-f0-9]{64})[\\/]bin[\\/]nskkzime_tsf\.dll$') {
        throw "このインストーラーの管理外の登録です。登録元の手順で解除してください: $Dll"
    }
    return $Matches[1].ToLowerInvariant()
}
function Enter-NskkzimeInstallLock([string]$Root) {
    Assert-NskkzimePath $Root
    if (-not (Test-Path -LiteralPath $Root -PathType Container)) { throw 'インストール先がありません。' }
    $file = Join-Path $Root 'operation.lock'
    Assert-NskkzimePath $file
    try { return [IO.File]::Open($file, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None) }
    catch { throw '別のインストール処理が実行中か、インストール先へ書き込めません。' }
}
function Add-NskkzimeVersion([string]$Root, [string]$Source, [string]$Verifier) {
    Assert-NskkzimePath $Root
    $bundle = Get-NskkzimeBundle $Source $Verifier
    $versions = Join-Path $Root 'versions'
    Assert-NskkzimePath $versions
    [IO.Directory]::CreateDirectory($versions) | Out-Null
    $destination = Join-Path $versions $bundle.Version
    if (Test-Path -LiteralPath $destination) { return Get-NskkzimeInstalledBundle $Root $bundle.Version $Verifier }
    $staging = Join-Path $Root ('staging-' + [Guid]::NewGuid().ToString('N'))
    [IO.Directory]::CreateDirectory($staging) | Out-Null
    # Use the verifier's fixed inventory, never paths read from unverified JSON.
    # Failed staging is retained for diagnosis; it is never registered.
    foreach ($name in $bundle.Paths) {
        $target = Join-Path $staging $name
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
        [IO.File]::Copy((Join-Path $bundle.Directory $name), $target, $false)
    }
    $copied = Get-NskkzimeBundle $staging $Verifier
    if ($copied.Version -cne $bundle.Version) { throw 'コピー中にパッケージが変更されました。登録しません。' }
    [IO.Directory]::Move($staging, $destination)
    return Get-NskkzimeInstalledBundle $Root $bundle.Version $Verifier
}
function Write-NskkzimeJournal([string]$Root, [string]$Previous, [string]$Target) {
    $path = Join-Path $Root 'transaction.json'
    Assert-NskkzimePath $path
    $record = [ordered]@{ format_version = 1; previous = $(if ($Previous) { $Previous } else { $null });
        target = $(if ($Target) { $Target } else { $null }) }
    $temporary = Join-Path $Root ('transaction-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    $bytes = [Text.Encoding]::UTF8.GetBytes(($record | ConvertTo-Json -Compress) + "`n")
    $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes); $stream.Flush($true) } finally { $stream.Dispose() }
    try { [IO.File]::Move($temporary, $path, $false) }
    finally { if (Test-Path -LiteralPath $temporary) { [IO.File]::Delete($temporary) } }
}
function Read-NskkzimeJournal([string]$Root) {
    $path = Join-Path $Root 'transaction.json'
    Assert-NskkzimePath $path
    if (-not (Test-Path -LiteralPath $path)) { return $null }
    if ((Get-Item -LiteralPath $path).Length -gt 16KB) { throw '更新記録が大きすぎます。' }
    $record = [IO.File]::ReadAllText($path) | ConvertFrom-Json
    if ($record.format_version -ne 1) { throw '未対応の更新記録です。' }
    foreach ($version in @($record.previous, $record.target)) {
        if ($null -ne $version -and ($version -isnot [string] -or $version -cnotmatch '^[a-f0-9]{64}$')) { throw '更新記録の版IDが不正です。' }
    }
    return $record
}
function Assert-NskkzimeRegistration([scriptblock]$ReadRegistration, [string]$Expected) {
    $current = & $ReadRegistration
    if (-not [string]::Equals([string]$current, [string]$Expected, [StringComparison]::OrdinalIgnoreCase)) {
        throw '処理中に登録先が変更されました。'
    }
}
function Set-NskkzimeRegistrationTarget([string]$Root, [string]$Target, [string[]]$Allowed,
    [string]$Verifier, [scriptblock]$ReadRegistration, [scriptblock]$Registrar, [scriptblock]$SetStartup) {
    $current = & $ReadRegistration
    $version = Get-NskkzimeManagedVersion $Root $current
    if ($version -and $version -notin $Allowed) { throw '更新記録にない版へ登録先が変更されています。' }
    # Validate BOTH bundles before removing anything, including during recovery.
    $next = if ($Target) { Get-NskkzimeInstalledBundle $Root $Target $Verifier } else { $null }
    if ($version) {
        $old = Get-NskkzimeInstalledBundle $Root $version $Verifier
        Assert-NskkzimeRegistration $ReadRegistration $current
        $code = & $Registrar $old.Dll $true
        if ($code -ne 0) { throw "登録解除に失敗しました (regsvr32 exit $code)。" }
        Assert-NskkzimeRegistration $ReadRegistration $null
    }
    if ($next) {
        Assert-NskkzimeRegistration $ReadRegistration $null
        $code = & $Registrar $next.Dll $false
        if ($code -ne 0) { throw "登録に失敗しました (regsvr32 exit $code)。" }
        Assert-NskkzimeRegistration $ReadRegistration $next.Dll
    }
    if ($SetStartup) { & $SetStartup $(if ($next) { $next.Broker } else { $null }) }
}
function Invoke-NskkzimeTransition([string]$Root, [string]$Target, [string]$Verifier,
    [scriptblock]$ReadRegistration, [scriptblock]$Registrar, [scriptblock]$SetStartup) {
    if ($null -ne (Read-NskkzimeJournal $Root)) { throw '未完了の更新記録があります。Recoverを先に実行してください。' }
    $previous = Get-NskkzimeManagedVersion $Root (& $ReadRegistration)
    if ($previous) { $null = Get-NskkzimeInstalledBundle $Root $previous $Verifier }
    if ($Target) { $null = Get-NskkzimeInstalledBundle $Root $Target $Verifier }
    if ([string]$previous -ceq [string]$Target) {
        if ($SetStartup) { & $SetStartup $(if ($Target) { (Get-NskkzimeInstalledBundle $Root $Target $Verifier).Broker } else { $null }) }
        return [pscustomobject]@{ Result = 'Unchanged'; Version = $Target }
    }
    Write-NskkzimeJournal $Root $previous $Target
    $allowed = @($previous, $Target) | Where-Object { $_ }
    try {
        Set-NskkzimeRegistrationTarget $Root $Target $allowed $Verifier $ReadRegistration $Registrar $SetStartup
        [IO.File]::Delete((Join-Path $Root 'transaction.json'))
    } catch {
        $original = $_.Exception.Message
        $restored = $false
        $recoveryError = ''
        try {
            Set-NskkzimeRegistrationTarget $Root $previous $allowed $Verifier $ReadRegistration $Registrar $SetStartup
            [IO.File]::Delete((Join-Path $Root 'transaction.json'))
            $restored = $true
        } catch { $recoveryError = $_.Exception.Message }
        if ($restored) { throw "更新に失敗し、変更前の登録へ復帰しました。原因: $original" }
        throw "更新と復帰に失敗しました。記録を保持しています。Recoverで再試行してください。原因: $original / 復帰: $recoveryError"
    }
    [pscustomobject]@{ Result = 'Changed'; Version = $Target; Previous = $previous }
}
function Repair-NskkzimeTransition([string]$Root, [string]$Verifier,
    [scriptblock]$ReadRegistration, [scriptblock]$Registrar, [scriptblock]$SetStartup) {
    $record = Read-NskkzimeJournal $Root
    if ($null -eq $record) {
        $current = Get-NskkzimeManagedVersion $Root (& $ReadRegistration)
        if ($SetStartup) { & $SetStartup $(if ($current) { (Get-NskkzimeInstalledBundle $Root $current $Verifier).Broker } else { $null }) }
        return [pscustomobject]@{ Result = 'NoPendingTransaction' }
    }
    $allowed = @($record.previous, $record.target) | Where-Object { $_ }
    Set-NskkzimeRegistrationTarget $Root $record.previous $allowed $Verifier $ReadRegistration $Registrar $SetStartup
    [IO.File]::Delete((Join-Path $Root 'transaction.json'))
    [pscustomobject]@{ Result = 'Recovered'; Version = $record.previous }
}
function Get-NskkzimeInstallStatus([string]$Root, [scriptblock]$ReadRegistration) {
    Assert-NskkzimePath $Root
    $current = & $ReadRegistration
    $managed = $null
    try { $managed = Get-NskkzimeManagedVersion $Root $current } catch { }
    $versions = Join-Path $Root 'versions'
    Assert-NskkzimePath $versions
    $available = if (Test-Path -LiteralPath $versions) {
        @(Get-ChildItem -LiteralPath $versions -Directory | Where-Object { $_.Name -cmatch '^[a-f0-9]{64}$' } | ForEach-Object Name)
    } else { @() }
    [pscustomobject]@{ Root = $Root; RegisteredDll = $current; ManagedVersion = $managed;
        Pending = (Read-NskkzimeJournal $Root); AvailableVersions = @($available) }
}
Export-ModuleMember -Function Assert-NskkzimePath, Get-NskkzimeBundle, Get-NskkzimeInstalledBundle,
    Get-NskkzimeManagedVersion, Enter-NskkzimeInstallLock, Add-NskkzimeVersion,
    Invoke-NskkzimeTransition, Repair-NskkzimeTransition, Get-NskkzimeInstallStatus
