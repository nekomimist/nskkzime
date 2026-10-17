#Requires -Version 7.0
param([Parameter(Mandatory)][string]$BuildDirectory, [Parameter(Mandatory)][string]$Configuration,
    [switch]$ExperimentalArm64, [string]$PackageArchive)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if ($Configuration -ne 'Release') { Write-Output 'SKIP installation fixtures require Release'; exit 77 }
$repository = Split-Path -Parent $PSScriptRoot
$verifier = Join-Path $repository $(if($ExperimentalArm64){'scripts/package-arm64x.ps1'}else{'scripts/package.ps1'})
Import-Module (Join-Path $repository 'scripts/installation.psm1') -Force
$fixture = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('install-test 日本語 space-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$root = Join-Path $fixture 'managed'
# The registry and HKCU Run are simulated; the real OS registration is never touched.
$state = @{ Dll = $null; X86Dll = $null; Startup = $null; FailNextAdd = $false; FailNextX86Add = $false; RegistrarCalls=0 }
$setStartup = { param([string]$Broker) $state.Startup = $Broker }.GetNewClosure()
$readRegistration = { return $state.Dll }.GetNewClosure()
$readX86Registration = { return $state.X86Dll }.GetNewClosure()
$registrar = {
    param([string]$Dll, [bool]$Unregister)
    ++$state.RegistrarCalls
    if (-not $Unregister -and $state.FailNextAdd) { $state.FailNextAdd = $false; return 5 }
    $slot = if ([IO.Path]::GetFileName([IO.Path]::GetDirectoryName($Dll)) -eq 'x86') { 'X86Dll' } else { 'Dll' }
    if (-not $Unregister -and $slot -eq 'X86Dll' -and $state.FailNextX86Add) { $state.FailNextX86Add=$false; return 5 }
    $state[$slot] = if ($Unregister) { $null } else { $Dll }
    return 0
}.GetNewClosure()
function Assert-Install([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Transition([string]$Target) { Invoke-NskkzimeTransition $root $Target $verifier $readRegistration $registrar $setStartup $readX86Registration }
function Expand-Version([string]$Name, [string]$Marker) {
    # Same binaries with a distinct README give a second, verifiable version.
    $directory = Join-Path $fixture $Name
    [IO.Compression.ZipFile]::ExtractToDirectory($zip.Path, $directory)
    $readme = Join-Path $directory 'README.md'
    [IO.File]::AppendAllText($readme, "`n$Marker`n")
    $manifestPath = Join-Path $directory 'manifest.json'
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $entry = $manifest.files | Where-Object path -eq README.md
    $entry.size = (Get-Item -LiteralPath $readme).Length
    $entry.sha256 = (Get-FileHash -LiteralPath $readme).Hash.ToLowerInvariant()
    $manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath
    return Add-NskkzimeVersion $root $directory $verifier
}
try {
    $private=Join-Path $fixture '設定 folder/settings.ini'
    [IO.Directory]::CreateDirectory((Split-Path -Parent $private)) | Out-Null
    [IO.File]::WriteAllText($private,"version=1`nlearning=off`n")
    Assert-Install ((Resolve-NskkzimeBrokerConfig $private $root -RequireFile) -ceq $private) 'Private broker configuration path changed.'
    if (-not ('NskkzimeInstallCommandTest' -as [type])) {
        Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class NskkzimeInstallCommandTest {
    [DllImport("shell32.dll",CharSet=CharSet.Unicode)] static extern IntPtr CommandLineToArgvW(string line,out int count);
    [DllImport("kernel32.dll")] static extern IntPtr LocalFree(IntPtr memory);
    public static string[] Parse(string line) {
        int count; IntPtr memory=CommandLineToArgvW(line,out count);
        if(memory==IntPtr.Zero) throw new InvalidOperationException();
        try { var result=new string[count]; for(int i=0;i<count;i++) result[i]=Marshal.PtrToStringUni(Marshal.ReadIntPtr(memory,i*IntPtr.Size)); return result; }
        finally { LocalFree(memory); }
    }
}
'@
    }
    $broker='C:\Program Files\nskkzime\nskkzime_broker.exe'
    $command=Get-NskkzimeBrokerCommandLine $broker $private
    $arguments=[NskkzimeInstallCommandTest]::Parse($command)
    Assert-Install ($arguments.Count -eq 4 -and $arguments[0] -ceq $broker -and $arguments[1] -ceq '--replace' -and
        $arguments[2] -ceq '--config' -and $arguments[3] -ceq $private) 'Startup quoting changed configuration arguments.'
    Assert-Install ((Get-NskkzimeBrokerCommandLine $broker $null) -ceq ('"'+$broker+'" --replace')) 'Legacy broker startup changed.'
    foreach($invalid in @('relative.ini',('C:\bad'+[char]34+'name.ini'),("C:\bad`nname.ini"),'\\server\share\settings.ini',(Join-Path $root 'settings.ini'))) {
        $failed=$false; try { Resolve-NskkzimeBrokerConfig $invalid $root | Out-Null } catch { $failed=$true }
        Assert-Install $failed 'Invalid/public broker configuration was accepted.'
    }
    Remove-Item -LiteralPath $private
    Assert-Install ((Resolve-NskkzimeBrokerConfig $private $root) -ceq $private) 'Missing configuration blocked recovery path selection.'
    $failed=$false; try { Resolve-NskkzimeBrokerConfig $private $root -RequireFile | Out-Null } catch { $failed=$true }
    Assert-Install $failed 'Missing configuration was accepted for a new launch.'
    Assert-Install ((Get-NskkzimeBrokerParameters stop $private) -ceq ('--stop --config "'+$private+'"')) 'Stop does not use the saved configuration.'
    $zip = if ($PackageArchive) { & $verifier -Verify $PackageArchive } else {
        & $verifier -SourceDirectory (Join-Path $BuildDirectory 'Release') -Output (Join-Path $fixture 'package.zip')
    }
    Assert-Install $zip.Verified 'Installation fixture package was not verified.'
    [IO.Directory]::CreateDirectory($root) | Out-Null
    $a = Expand-Version 'first' 'Fixture first version.'
    $sid='S-1-5-21-1-2-3-1001'
    $snapshot='{"Registry64":{"Enable":{"kind":"DWord","value":1}},"Registry32":null}'
    Assert-Install (-not(Test-NskkzimeUserTipOwnership $root $sid $snapshot $verifier)) 'Unrecorded user TIP was accepted.'
    $state.Dll=$a.Dll
    Write-NskkzimeUserTipOwnership $root $a.Version $sid $snapshot $verifier $readRegistration
    $state.Dll=$null
    Assert-Install (Test-NskkzimeUserTipOwnership $root $sid $snapshot $verifier) 'Owned retained user TIP was refused.'
    Assert-Install (-not(Test-NskkzimeUserTipOwnership $root $sid ($snapshot.Replace('"value":1','"value":0')) $verifier)) 'Changed user TIP was accepted.'
    $failed=$false;try{Test-NskkzimeUserTipOwnership $root 'S-1-5-21-1-2-3-1002' $snapshot $verifier | Out-Null}catch{$failed=$true}
    Assert-Install $failed 'Another SID reused the user TIP ownership record.'
    $recordPath=Join-Path $root 'user-tip.json';$recordHash=(Get-FileHash -LiteralPath $recordPath).Hash
    $failed=$false;try{Write-NskkzimeUserTipOwnership $root $a.Version $sid $snapshot $verifier $readRegistration}catch{$failed=$true}
    Assert-Install ($failed -and (Get-FileHash -LiteralPath $recordPath).Hash -ceq $recordHash) 'Absent COM owner established TIP ownership.'
    $otherRoot=Join-Path $fixture 'other-root';[IO.Directory]::CreateDirectory($otherRoot) | Out-Null
    [IO.File]::Copy($recordPath,(Join-Path $otherRoot 'user-tip.json'))
    $failed=$false;try{Test-NskkzimeUserTipOwnership $otherRoot $sid $snapshot $verifier | Out-Null}catch{$failed=$true}
    Assert-Install $failed 'Another root reused the user TIP ownership record.'
    $b = Expand-Version 'second' 'Fixture second version.'
    Transition $a.Version | Out-Null
    Assert-Install ($state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll -and $state.Startup -eq $a.Broker) 'Initial install failed.'
    Transition $b.Version | Out-Null
    Assert-Install ($state.Dll -eq $b.Dll -and $state.X86Dll -eq $b.X86Dll -and $state.Startup -eq $b.Broker) 'Update failed.'
    Transition $a.Version | Out-Null
    Assert-Install ($state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll) 'Rollback failed.'
    # A failed registration restores the previous version.
    $state.FailNextAdd = $true
    $failed = $false
    try { Transition $b.Version | Out-Null } catch { $failed = $true }
    Assert-Install ($failed -and $state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll) 'Failed update lost the previous version.'
    $state.FailNextX86Add=$true; $failed=$false
    try { Transition $b.Version | Out-Null } catch { $failed=$true }
    Assert-Install ($failed -and $state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll -and $state.Startup -eq $a.Broker) 'Partial x86 registration failure lost the previous version.'
    $calls=$state.RegistrarCalls; $state.X86Dll='C:\foreign\nskkzime_tsf.dll'; $failed=$false
    try { Transition $b.Version | Out-Null } catch { $failed=$true }
    Assert-Install ($failed -and $state.RegistrarCalls -eq $calls -and $state.Dll -eq $a.Dll) 'Foreign x86 owner reached the registrar.'
    $state.X86Dll=$a.X86Dll
    # Recover an interrupted transition with different native/x86 owners.
    @{format_version=2;previous=$a.Version;target=$b.Version} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'transaction.json')
    $state.Dll=$b.Dll
    Repair-NskkzimeTransition $root $verifier $readRegistration $registrar $setStartup $readX86Registration | Out-Null
    Assert-Install ($state.Dll -eq $a.Dll -and $state.X86Dll -eq $a.X86Dll -and -not (Test-Path -LiteralPath (Join-Path $root 'transaction.json'))) 'Interrupted transition did not recover.'
    Transition $null | Out-Null
    Assert-Install (-not $state.Dll -and -not $state.X86Dll -and -not $state.Startup -and (Test-Path -LiteralPath $a.Dll)) 'Unregister failed or removed files.'
    $status = Get-NskkzimeInstallStatus $root $readRegistration
    Assert-Install (-not $status.ManagedVersion -and $status.AvailableVersions.Count -eq 2 -and -not $status.Pending) 'Status is inconsistent.'
    # A native registrar can partially write TSF, clean its own COM entry, and
    # still report failure. COM absence alone must not erase recovery evidence.
    $partialRoot=Join-Path $fixture 'partial'
    [IO.Directory]::CreateDirectory($partialRoot) | Out-Null
    $partialBundle=Add-NskkzimeVersion $partialRoot $a.Directory $verifier
    $partial=@{Dll=$null;X86Dll=$null;UserTip=$null;StartupCalls=0}
    $partialRead={return $partial.Dll}.GetNewClosure()
    $partialReadX86={return $partial.X86Dll}.GetNewClosure()
    $partialRegistrar={param($Dll,$Unregister)
        if(-not $Unregister){$partial.UserTip='unrecorded partial TSF state';$partial.Dll=$null;return 5}
        if([IO.Path]::GetFileName([IO.Path]::GetDirectoryName($Dll)) -eq 'x86'){$partial.X86Dll=$null}else{$partial.Dll=$null}
        return 0
    }.GetNewClosure()
    $partialStartup={param($Broker) ++$partial.StartupCalls}.GetNewClosure()
    $validateUnregistered={
        if($partial.UserTip -and -not(Test-NskkzimeUserTipOwnership $partialRoot $sid $partial.UserTip $verifier)){
            throw 'Unrecorded user TIP remains; recovery is incomplete.'
        }
    }.GetNewClosure()
    $failed=$false
    try{Invoke-NskkzimeTransition $partialRoot $partialBundle.Version $verifier $partialRead $partialRegistrar $partialStartup $partialReadX86 $validateUnregistered|Out-Null}catch{$failed=$true}
    $journal=Join-Path $partialRoot 'transaction.json'
    Assert-Install ($failed -and (Test-Path -LiteralPath $journal) -and $partial.StartupCalls -eq 0) 'Partial TSF failure was reported restored or changed startup.'
    $journalHash=(Get-FileHash -LiteralPath $journal).Hash
    $failed=$false
    try{Repair-NskkzimeTransition $partialRoot $verifier $partialRead $partialRegistrar $partialStartup $partialReadX86 $validateUnregistered|Out-Null}catch{$failed=$true}
    Assert-Install ($failed -and (Get-FileHash -LiteralPath $journal).Hash -ceq $journalHash -and $partial.StartupCalls -eq 0) 'Unresolved recovery lost its journal or changed startup.'
    $partial.UserTip=$null # Simulate the unresolved residue being handled externally.
    Repair-NskkzimeTransition $partialRoot $verifier $partialRead $partialRegistrar $partialStartup $partialReadX86 $validateUnregistered|Out-Null
    Assert-Install (-not(Test-Path -LiteralPath $journal) -and $partial.StartupCalls -eq 1) 'Recovery did not finish after the residue was resolved.'
    $partial.UserTip='unrecorded user TIP'
    $failed=$false
    try{Repair-NskkzimeTransition $partialRoot $verifier $partialRead $partialRegistrar $partialStartup $partialReadX86 $validateUnregistered|Out-Null}catch{$failed=$true}
    Assert-Install ($failed -and -not(Test-Path -LiteralPath $journal) -and $partial.StartupCalls -eq 1) 'No-journal recovery skipped TSF validation.'
    $failed=$false
    try{Invoke-NskkzimeTransition $partialRoot $null $verifier $partialRead $partialRegistrar $partialStartup $partialReadX86 $validateUnregistered|Out-Null}catch{$failed=$true}
    Assert-Install ($failed -and -not(Test-Path -LiteralPath $journal) -and $partial.StartupCalls -eq 1) 'Unchanged unregistered transition skipped TSF validation.'
    $partial.Dll=$partialBundle.Dll;$partial.X86Dll=$partialBundle.X86Dll;$partial.UserTip=$snapshot
    Write-NskkzimeUserTipOwnership $partialRoot $partialBundle.Version $sid $snapshot $verifier $partialRead
    Invoke-NskkzimeTransition $partialRoot $null $verifier $partialRead $partialRegistrar $partialStartup $partialReadX86 $validateUnregistered|Out-Null
    Assert-Install (-not $partial.Dll -and -not $partial.X86Dll -and -not(Test-Path -LiteralPath $journal) -and $partial.StartupCalls -eq 2) 'Owned retained TIP blocked normal unregister.'
    $partial.Dll=$partialBundle.Dll;$partial.X86Dll=$partialBundle.X86Dll;$partial.UserTip='changed while still registered'
    Repair-NskkzimeTransition $partialRoot $verifier $partialRead $partialRegistrar $partialStartup $partialReadX86 $validateUnregistered|Out-Null
    Invoke-NskkzimeTransition $partialRoot $partialBundle.Version $verifier $partialRead $partialRegistrar $partialStartup $partialReadX86 $validateUnregistered|Out-Null
    Assert-Install ($partial.Dll -ceq $partialBundle.Dll -and $partial.StartupCalls -eq 4) 'Registered no-op incorrectly required an unregistered TIP snapshot.'
    Write-Output 'PASS partial TSF failure retains recovery evidence, validates no-op paths, and accepts owned retained TIP'
    Write-Output 'PASS broker configuration quoting/validation and install, update, rollback, native/x86 failure recovery, interrupted recovery, unregister and status'
} finally {
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Assert-NskkzimePath $absolute
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
