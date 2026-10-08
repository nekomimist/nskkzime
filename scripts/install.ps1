#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Status', 'Install', 'Rollback', 'Unregister', 'Recover')][string]$Action = 'Status',
    [string]$PackageDirectory = (Join-Path $PSScriptRoot '..'),
    [string]$Version
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or -not [Environment]::Is64BitOperatingSystem -or -not [Environment]::Is64BitProcess) {
    throw 'Windows x64のPowerShell 7で実行してください。'
}
Import-Module (Join-Path $PSScriptRoot 'installation.psm1') -Force
$verifier = Join-Path $PSScriptRoot 'package.ps1'
$root = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'nskkzime'
$classPath = 'Software\Classes\CLSID\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}'
$readRegistration = {
    $shadow = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($classPath)
    if ($shadow) { $shadow.Dispose(); throw 'HKCUに同じCLSIDがあります。登録元の手順で解除してください。' }
    $class = [Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($classPath)
    if (-not $class) { return $null }
    try {
        $server = $class.OpenSubKey('InprocServer32')
        if (-not $server) { throw 'CLSIDの登録が不完全です。登録元の手順で修復してください。' }
        try {
            $dll = $server.GetValue('', $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
            if ($server.GetValueKind('') -ne [Microsoft.Win32.RegistryValueKind]::String -or
                $dll -isnot [string] -or -not [IO.Path]::IsPathFullyQualified($dll)) {
                throw 'CLSIDの登録先が不正です。登録元の手順で修復してください。'
            }
            return $dll
        } finally { $server.Dispose() }
    } finally { $class.Dispose() }
}.GetNewClosure()
if ($Action -eq 'Status') { Get-NskkzimeInstallStatus $root $readRegistration; return }
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '使用する本人のアカウントで、管理者として起動したPowerShell 7から実行してください。'
}
if ($Action -eq 'Rollback' -and $Version -cnotmatch '^[a-f0-9]{64}$') { throw 'RollbackにはStatusで確認した64桁の版IDを指定してください。' }
if ($Action -ne 'Rollback' -and $Version) { throw '-VersionはRollbackだけで使用できます。' }

function Assert-ProtectedItem([string]$Path) {
    Assert-NskkzimePath $Path
    $acl = Get-Acl -LiteralPath $Path
    $owner = $acl.GetOwner([Security.Principal.SecurityIdentifier]).Value
    if ($owner -notin @('S-1-5-18', 'S-1-5-32-544')) { throw "配置先の所有者がSystem/Administratorsではありません: $Path" }
    # Users may read/execute. Only SYSTEM and Administrators may write, delete,
    # change permissions or take ownership of the files that regsvr32 executes.
    $readOnly = [int][Security.AccessControl.FileSystemRights]::ReadAndExecute -bor [int][Security.AccessControl.FileSystemRights]::Synchronize
    foreach ($rule in $acl.GetAccessRules($true, $true, [Security.Principal.SecurityIdentifier])) {
        if ($rule.AccessControlType -eq [Security.AccessControl.AccessControlType]::Allow -and
            $rule.IdentityReference.Value -notin @('S-1-5-18', 'S-1-5-32-544') -and
            (([int]$rule.FileSystemRights -band (-bnot $readOnly)) -ne 0)) {
            throw "一般ユーザーが変更できる配置先は使用できません: $Path"
        }
    }
}
Assert-NskkzimePath $root
# Refuse unmanaged registrations before creating directories or copying files.
$null = Get-NskkzimeManagedVersion $root (& $readRegistration)
if (-not (Test-Path -LiteralPath $root)) {
    if ($Action -ne 'Install') { throw 'インストール先がありません。' }
    $null = Get-NskkzimeBundle $PackageDirectory $verifier
    $security = [Security.AccessControl.DirectorySecurity]::new()
    $security.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)')
    # Apply the protected DACL at creation, with no temporarily writable root.
    [IO.FileSystemAclExtensions]::CreateDirectory($security, $root) | Out-Null
    Assert-ProtectedItem $root
    $markerStream = [IO.File]::Open((Join-Path $root 'install-root.json'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $markerStream.Write([Text.Encoding]::UTF8.GetBytes('{"format_version":1,"product":"nskkzime"}')); $markerStream.Flush($true) }
    finally { $markerStream.Dispose() }
}
Assert-ProtectedItem $root
$marker = Join-Path $root 'install-root.json'
Assert-ProtectedItem $marker
$identity = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
if ($identity.format_version -ne 1 -or $identity.product -cne 'nskkzime') { throw 'インストール先の識別情報が不正です。' }
$lock = Enter-NskkzimeInstallLock $root
try {
    # Reject unexpected writable/reparse-point descendants before reusing them.
    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($root)
    while ($queue.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $queue.Dequeue() -Force) {
            Assert-ProtectedItem $item.FullName
            if ($item.PSIsContainer) { $queue.Enqueue($item.FullName) }
        }
    }
    if (@(Get-Process -Name regsvr32 -ErrorAction SilentlyContinue).Count) {
        throw 'regsvr32が動作中です。終了後に再実行してください。中断した子プロセスとは並行して復旧しません。'
    }
    $status = Get-NskkzimeInstallStatus $root $readRegistration
    if ($status.Pending -and $Action -ne 'Recover') { throw '未完了の更新記録があります。Recoverを先に実行してください。' }
    # This tree contains verified public program files only. Private dictionaries,
    # settings, model files and IPC objects retain their existing access rules.
    $publicAcl = Get-Acl -LiteralPath $root
    $publicAcl.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new(
        [Security.Principal.SecurityIdentifier]::new('S-1-15-2-1'),
        [Security.AccessControl.FileSystemRights]::ReadAndExecute,
        [Security.AccessControl.InheritanceFlags]'ContainerInherit,ObjectInherit',
        [Security.AccessControl.PropagationFlags]::None, [Security.AccessControl.AccessControlType]::Allow))
    Set-Acl -LiteralPath $root -AclObject $publicAcl
    Assert-ProtectedItem $root
    $previousBroker = if ($status.ManagedVersion) { (Get-NskkzimeInstalledBundle $root $status.ManagedVersion $verifier).Broker } else { $null }
    $setStartup = {
        param([string]$Broker)
        if ($Broker) { Assert-ProtectedItem $Broker }
        $run = [Microsoft.Win32.Registry]::CurrentUser.CreateSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
        try {
            if ($Broker) { $run.SetValue('nskkzime broker', ('"' + $Broker + '" --replace'), [Microsoft.Win32.RegistryValueKind]::String) }
            else { $run.DeleteValue('nskkzime broker', $false) }
        } finally { $run.Dispose() }
    }
    $registrar = {
        param([string]$Dll, [bool]$Unregister)
        Assert-ProtectedItem $Dll
        $start = [Diagnostics.ProcessStartInfo]::new((Join-Path ([Environment]::SystemDirectory) 'regsvr32.exe'))
        $start.UseShellExecute = $false
        $start.CreateNoWindow = $true
        $start.ArgumentList.Add('/s')
        if ($Unregister) { $start.ArgumentList.Add('/u') }
        $start.ArgumentList.Add($Dll)
        $child = [Diagnostics.Process]::Start($start)
        try { $child.WaitForExit(); return $child.ExitCode }
        finally { if (-not $child.HasExited) { $child.WaitForExit() }; $child.Dispose() }
    }
    switch ($Action) {
        Install {
            $bundle = Add-NskkzimeVersion $root $PackageDirectory $verifier
            Invoke-NskkzimeTransition $root $bundle.Version $verifier $readRegistration $registrar $setStartup
        }
        Rollback { Invoke-NskkzimeTransition $root $Version $verifier $readRegistration $registrar $setStartup }
        Unregister { Invoke-NskkzimeTransition $root $null $verifier $readRegistration $registrar $setStartup }
        Recover { Repair-NskkzimeTransition $root $verifier $readRegistration $registrar $setStartup }
    }
    # Registration + startup are transactional. Live process replacement is best
    # effort; startup also repairs it on the next sign-in. Never run the broker
    # elevated: private data and helper processes belong to the desktop user.
    try {
        if ($previousBroker) {
            $stop = [Diagnostics.ProcessStartInfo]::new($previousBroker)
            $stop.UseShellExecute = $false; $stop.CreateNoWindow = $true; $stop.ArgumentList.Add('--stop')
            $child = [Diagnostics.Process]::Start($stop)
            try { $child.WaitForExit(); if ($child.ExitCode) { throw '補助プロセスを停止できません。' } } finally { $child.Dispose() }
        }
        $current = Get-NskkzimeManagedVersion $root (& $readRegistration)
        $broker = if ($current) { (Get-NskkzimeInstalledBundle $root $current $verifier).Broker } else { $null }
        if ($broker) {
            $shell = New-Object -ComObject Shell.Application
            $desktopHandle = 0
            $desktop = $shell.Windows().FindWindowSW(0, 0, 8, [ref]$desktopHandle, 1)
            if (-not $desktop) { throw '通常ユーザーのExplorerが見つかりません。' }
            $desktop.Document.Application.ShellExecute($broker, '--replace', (Split-Path -Parent $broker), 'open', 0)
        }
    } catch { Write-Warning "登録は完了しましたが、補助プロセスの切り替えに失敗しました。サインアウト・サインインで反映できます。$($_.Exception.Message)" }
    Get-NskkzimeInstallStatus $root $readRegistration
} finally { $lock.Dispose() }
