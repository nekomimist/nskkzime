#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Status', 'Plan', 'Install', 'Rollback', 'Unregister', 'Recover')][string]$Action = 'Status',
    [string]$PackageDirectory = (Join-Path $PSScriptRoot '..'),
    [string]$Version,
    [string]$ExpectedUserSid,
    [switch]$ExperimentalArm64
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Action -eq 'Plan' -and -not $ExperimentalArm64) { throw 'PlanはARM64実験版の専用入口で使用してください。' }
if ($ExperimentalArm64) {
    if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne [Runtime.InteropServices.Architecture]::Arm64 -or
        [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -ne [Runtime.InteropServices.Architecture]::Arm64 -or
        [Environment]::OSVersion.Version.Build -lt 22000) { throw 'ARM64実験版はWindows 11 ARM64のnative ARM64 PowerShell 7で実行してください。' }
} elseif (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne [Runtime.InteropServices.Architecture]::X64 -or
    -not [Environment]::Is64BitProcess) {
    throw 'Windows x64のPowerShell 7で実行してください。'
}
Import-Module (Join-Path $PSScriptRoot 'installation.psm1') -Force
$verifier = Join-Path $PSScriptRoot $(if($ExperimentalArm64){'package-arm64x.ps1'}else{'package.ps1'})
$root = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) $(if($ExperimentalArm64){'nskkzime-arm64-experimental'}else{'nskkzime'})
$invokingIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()
function Assert-RootIdentity($Record) {
    if ($ExperimentalArm64) {
        if ($Record.format_version -ne 2 -or $Record.product -cne 'nskkzime' -or $Record.architecture -cne 'arm64' -or
            $Record.channel -cne 'experimental' -or $Record.installation_profile -cne 'arm64-experimental-v1' -or
            $Record.user_sid -cne $invokingIdentity.User.Value) { throw 'ARM64実験版の配置先または所有ユーザーが一致しません。' }
    } elseif ($Record.format_version -ne 1 -or $Record.product -cne 'nskkzime') { throw 'インストール先の識別情報が不正です。' }
}
$classPath = 'Software\Classes\CLSID\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}'
function Read-RegistrationView([Microsoft.Win32.RegistryView]$View) {
    $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser, $View)
    $machine = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine, $View)
    try {
    $shadow = $user.OpenSubKey($classPath)
    if ($shadow) { $shadow.Dispose(); throw 'HKCUに同じCLSIDがあります。登録元の手順で解除してください。' }
    $class = $machine.OpenSubKey($classPath)
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
    } finally { $user.Dispose(); $machine.Dispose() }
}
$readRegistration = { Read-RegistrationView ([Microsoft.Win32.RegistryView]::Registry64) }
$readX86Registration = { Read-RegistrationView ([Microsoft.Win32.RegistryView]::Registry32) }
if ($Action -eq 'Status') {
    if ($ExperimentalArm64 -and (Test-Path -LiteralPath $root)) {
        Assert-NskkzimeProtectedPath $root
        Assert-NskkzimeProtectedPath (Join-Path $root 'install-root.json')
        Assert-RootIdentity (Get-Content -LiteralPath (Join-Path $root 'install-root.json') -Raw | ConvertFrom-Json)
    }
    Get-NskkzimeInstallStatus $root $readRegistration $readX86Registration; return
}
if ($ExpectedUserSid -and $invokingIdentity.User.Value -cne $ExpectedUserSid) {
    throw '昇格前と実行ユーザーが異なります。使用する本人のアカウントで管理者権限を承認してください。'
}
# A packaged caller (including an inherited virtualized child) can silently
# write HKCU Run to its private registry. Refuse before any installation writes.
if (-not ('NskkzimeInstallContext' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
using System.Text;
using Microsoft.Win32.SafeHandles;
public static class NskkzimeInstallContext {
    [DllImport("kernel32.dll")] static extern int GetCurrentPackageFullName(ref uint size, IntPtr name);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint creation, uint flags, IntPtr template);
    [DllImport("kernel32.dll", CharSet=CharSet.Unicode)] static extern uint GetFinalPathNameByHandle(SafeFileHandle file, StringBuilder name, uint size, uint flags);
    public static bool Redirected() {
        uint size=0;
        if(GetCurrentPackageFullName(ref size,IntPtr.Zero)!=15700) return true;
        string path=System.IO.Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"nskkzime","settings.ini");
        using(var file=CreateFile(path,0x80,7,IntPtr.Zero,3,0,IntPtr.Zero)) {
            if(file.IsInvalid) return false;
            var final=new StringBuilder(32768);
            uint length=GetFinalPathNameByHandle(file,final,32768,0);
            if(length==0 || length>=32768) throw new InvalidOperationException("Cannot identify installation context.");
            string opened=final.ToString(); if(opened.StartsWith(@"\\?\")) opened=opened.Substring(4);
            return !string.Equals(opened,path,StringComparison.OrdinalIgnoreCase);
        }
    }
}
'@
}
if ([NskkzimeInstallContext]::Redirected()) {
    throw 'アプリのデータ仮想化を検出しました。HKCUの起動登録が別の場所へ保存されるため、変更しません。通常のExplorerから起動したPowerShellで実行してください。'
}
$principal = [Security.Principal.WindowsPrincipal]::new($invokingIdentity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '使用する本人のアカウントで、管理者として起動したPowerShell 7から実行してください。'
}
if ($Action -eq 'Rollback' -and $Version -cnotmatch '^[a-f0-9]{64}$') { throw 'RollbackにはStatusで確認した64桁の版IDを指定してください。' }
if ($Action -ne 'Rollback' -and $Version) { throw '-VersionはRollbackだけで使用できます。' }

function Assert-ProtectedItem([string]$Path) {
    Assert-NskkzimeProtectedPath $Path
}
Assert-NskkzimePath $root
if ($Action -in @('Install','Plan')) { $plannedBundle=Get-NskkzimeBundle $PackageDirectory $verifier }
# Refuse unmanaged registrations before creating directories or copying files.
$nativeVersion = Get-NskkzimeManagedVersion $root (& $readRegistration)
$x86Version = Get-NskkzimeManagedVersion $root (& $readX86Registration) -X86
if ($ExperimentalArm64) {
    if (Test-Path -LiteralPath $root) {
        Assert-ProtectedItem $root; Assert-ProtectedItem (Join-Path $root 'install-root.json')
        Assert-RootIdentity (Get-Content -LiteralPath (Join-Path $root 'install-root.json') -Raw | ConvertFrom-Json)
    }
    $pending=(Get-NskkzimeInstallStatus $root $readRegistration $readX86Registration).Pending
    if ($pending) {
        foreach ($candidate in @($pending.previous,$pending.target) | Where-Object {$_}) { $null=Get-NskkzimeInstalledBundle $root $candidate $verifier }
    } elseif (-not $nativeVersion -and -not $x86Version) {
        # A TIP without a managed COM owner or protected recovery journal may
        # belong to a limited registration test or another installation.
        foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)) {
            foreach ($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryHive]::LocalMachine)) {
                $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,$view)
                try {
                    $tip=$base.OpenSubKey('Software\Microsoft\CTF\TIP\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}')
                    if ($tip) { $tip.Dispose(); throw '管理外のTSF登録があります。登録元の手順で解除してください。' }
                } finally { $base.Dispose() }
            }
        }
    }
    # The broker Run value is shared with normal installations. Never replace an
    # unmanaged value, including one from another root or the registration tests.
    $runKey=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
    try {
        $startup=if($runKey){$runKey.GetValue('nskkzime broker',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}else{$null}
        if ($null -ne $startup -and $runKey.GetValueKind('nskkzime broker') -ne [Microsoft.Win32.RegistryValueKind]::String) { throw '管理外のbroker起動登録があります。変更しません。' }
    }
    finally { if($runKey){$runKey.Dispose()} }
    if ($null -ne $startup) {
        if ($startup -isnot [string] -or $startup -cnotmatch '^"(.+)" --replace$') { throw '管理外のbroker起動登録があります。変更しません。' }
        $brokerPath=$Matches[1]
        $relative=[IO.Path]::GetRelativePath((Join-Path $root 'versions'),$brokerPath)
        if ($relative -cnotmatch '^([a-f0-9]{64})[\\/]bin[\\/]nskkzime_broker\.exe$') { throw '管理外のbroker起動登録があります。変更しません。' }
        $owned=Get-NskkzimeInstalledBundle $root $Matches[1] $verifier
        if (-not [string]::Equals($owned.Broker,$brokerPath,[StringComparison]::OrdinalIgnoreCase)) { throw 'broker起動登録が配置先と一致しません。' }
    }
    $startupGuard=@{Expected=$startup}
}
if ($Action -eq 'Plan') {
    if (Test-Path -LiteralPath $root) {
        Assert-ProtectedItem $root; Assert-ProtectedItem (Join-Path $root 'install-root.json')
        Assert-RootIdentity (Get-Content -LiteralPath (Join-Path $root 'install-root.json') -Raw | ConvertFrom-Json)
    }
    $status=Get-NskkzimeInstallStatus $root $readRegistration $readX86Registration
    $candidates=@($status.ManagedVersion,(Get-NskkzimeManagedVersion $root $status.RegisteredX86Dll -X86))
    if($status.Pending){$candidates+=@($status.Pending.previous,$status.Pending.target)}
    foreach($candidate in $candidates | Where-Object {$_} | Sort-Object -Unique){$null=Get-NskkzimeInstalledBundle $root $candidate $verifier}
    [pscustomobject]@{Action='Plan';InstallationProfile='arm64-experimental-v1';Root=$root;Version=$plannedBundle.Version;
        PackageDirectory=$plannedBundle.Directory;UserSid=$invokingIdentity.User.Value;NativeRegistrar=(Join-Path ([Environment]::SystemDirectory) 'regsvr32.exe');
        X86Registrar=(Join-Path $env:SystemRoot 'SysWOW64/regsvr32.exe');Current=$status;MutatesInstallation=$false}
    return
}
if (-not (Test-Path -LiteralPath $root)) {
    if ($Action -ne 'Install') { throw 'インストール先がありません。' }
    $null = Get-NskkzimeBundle $PackageDirectory $verifier
    $security = [Security.AccessControl.DirectorySecurity]::new()
    $security.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)')
    # Apply the protected DACL at creation, with no temporarily writable root.
    [IO.FileSystemAclExtensions]::CreateDirectory($security, $root) | Out-Null
    Assert-ProtectedItem $root
    $markerStream = [IO.File]::Open((Join-Path $root 'install-root.json'), [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    $record = if($ExperimentalArm64){@{format_version=2;product='nskkzime';architecture='arm64';channel='experimental';installation_profile='arm64-experimental-v1';user_sid=$invokingIdentity.User.Value}}else{@{format_version=1;product='nskkzime'}}
    try { $markerStream.Write([Text.Encoding]::UTF8.GetBytes(($record | ConvertTo-Json -Compress))); $markerStream.Flush($true) }
    finally { $markerStream.Dispose() }
}
Assert-ProtectedItem $root
$marker = Join-Path $root 'install-root.json'
Assert-ProtectedItem $marker
$identity = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
Assert-RootIdentity $identity
# Resolve all bundles before opening a writable lock or changing the public ACL.
# The same checks run under the lock again before registration can change.
function Assert-TransitionBundles($Status) {
    $candidates=@($Status.ManagedVersion,(Get-NskkzimeManagedVersion $root $Status.RegisteredX86Dll -X86),$Version)
    if ($Status.Pending) { $candidates += @($Status.Pending.previous,$Status.Pending.target) }
    foreach ($candidate in $candidates | Where-Object { $_ } | Sort-Object -Unique) { $null=Get-NskkzimeInstalledBundle $root $candidate $verifier }
}
Assert-TransitionBundles (Get-NskkzimeInstallStatus $root $readRegistration $readX86Registration)
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
    $status = Get-NskkzimeInstallStatus $root $readRegistration $readX86Registration
    if ($status.Pending -and $Action -ne 'Recover') { throw '未完了の更新記録があります。Recoverを先に実行してください。' }
    Assert-TransitionBundles $status
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
            if ($ExperimentalArm64 -and -not [string]::Equals([string]$run.GetValue('nskkzime broker',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames),[string]$startupGuard.Expected,[StringComparison]::Ordinal)) { throw '処理中にbroker起動登録が変更されました。' }
            if ($Broker) { $run.SetValue('nskkzime broker', ('"' + $Broker + '" --replace'), [Microsoft.Win32.RegistryValueKind]::String) }
            else { $run.DeleteValue('nskkzime broker', $false) }
            if ($ExperimentalArm64) { $startupGuard.Expected=if($Broker){'"'+$Broker+'" --replace'}else{$null} }
        } finally { $run.Dispose() }
    }
    $registrar = {
        param([string]$Dll, [bool]$Unregister)
        Assert-ProtectedItem $Dll
        $system = if ([IO.Path]::GetFileName([IO.Path]::GetDirectoryName($Dll)) -eq 'x86') { Join-Path $env:SystemRoot 'SysWOW64' } else { [Environment]::SystemDirectory }
        $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $system 'regsvr32.exe'))
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
            Invoke-NskkzimeTransition $root $bundle.Version $verifier $readRegistration $registrar $setStartup $readX86Registration
        }
        Rollback { Invoke-NskkzimeTransition $root $Version $verifier $readRegistration $registrar $setStartup $readX86Registration }
        Unregister { Invoke-NskkzimeTransition $root $null $verifier $readRegistration $registrar $setStartup $readX86Registration }
        Recover { Repair-NskkzimeTransition $root $verifier $readRegistration $registrar $setStartup $readX86Registration }
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
    Get-NskkzimeInstallStatus $root $readRegistration $readX86Registration
} finally { $lock.Dispose() }
