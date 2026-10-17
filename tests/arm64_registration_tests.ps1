#Requires -Version 7.0
param([string]$OutputDirectory = (Join-Path $PSScriptRoot '../out/tmp'))
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
Import-Module (Join-Path $PSScriptRoot '../scripts/arm64-registration-test.psm1') -Force
$root = Join-Path ([IO.Path]::GetFullPath($OutputDirectory)) ('registration-test-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root -Force | Out-Null
function Check([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Fixture([string]$Name) {
    $file = Join-Path $root ($Name + '.json')
    $context = @{native=@{exists=$false;dll=$null};x86=@{exists=$false;dll=$null};tip=$false;shadow=$false;sha='initial';
        nativePath='C:\protected\bin\nskkzime_tsf.dll';x86Path='C:\protected\bin\x86\nskkzime_tsf.dll';
        calls=[Collections.Generic.List[string]]::new();probes=[Collections.Generic.List[string]]::new();fail='';failProbe='';userTip=$false}
    $read = { return @{native=@{exists=$context.native.exists;dll=$context.native.dll};
        x86=@{exists=$context.x86.exists;dll=$context.x86.dll};tip=($context.tip -or $context.userTip);machine_tip=$context.tip;
        user_tip=@{Registry64=$(if($context.userTip){'owned-user-state'}else{$null})};shadow=$context.shadow;user_state_sha256=$context.sha} }.GetNewClosure()
    $registrar = {
        param([string]$Architecture,[bool]$Remove)
        $operation = $Architecture + ':' + $(if ($Remove) {'remove'} else {'add'})
        $phase = (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json).phase
        if ($phase -cne ($(if ($Remove) {'unregister-'} else {'register-'}) + $Architecture + '-intent')) { throw 'Intent was not durable before mutation.' }
        $context.calls.Add($operation)
        if ($context.fail -ceq ($operation + ':before')) { return 5 }
        $context[$Architecture].exists = -not $Remove
        $context[$Architecture].dll = if ($Remove) { $null } else { $context[$Architecture + 'Path'] }
        if ($Architecture -eq 'native') { $context.tip = -not $Remove }
        if ($context.fail -ceq ($operation + ':after')) { return 5 }
        return 0
    }.GetNewClosure()
    $probe = { param([string]$Architecture,[bool]$Present)
        $call=$Architecture + '=' + $Present;$context.probes.Add($call)
        if ($context.failProbe -ceq $call) {throw 'Simulated COM diagnostic failure.'}
    }.GetNewClosure()
    $restore = {param($Expected)
        if ((Get-Content -LiteralPath $file -Raw | ConvertFrom-Json).phase -cne 'restore-user-tip-intent') {throw 'User TIP cleanup intent was not durable.'}
        if ($Expected.Registry64 -cne 'owned-user-state') {throw 'Unrecorded user TIP removal attempted.'}
        $context.userTip=$false
    }.GetNewClosure()
    return @{file=$file;context=$context;read=$read;registrar=$registrar;probe=$probe;restore=$restore}
}
function Journal($Fixture) { New-Arm64RegistrationJournal $Fixture.file $Fixture.context.nativePath $Fixture.context.x86Path @{} $Fixture.read }
function Add($Fixture,$Journal) { Add-Arm64TestRegistration $Fixture.file $Journal $Fixture.read $Fixture.registrar $Fixture.probe }
function Remove($Fixture,$Journal) { Remove-Arm64TestRegistration $Fixture.file $Journal $Fixture.read $Fixture.registrar $Fixture.probe $Fixture.restore }
function Rejected([scriptblock]$Operation,[string]$Pattern) {
    $failure = $null
    try { & $Operation } catch { $failure = $_.ToString() }
    Check ($null -ne $failure -and $failure -match $Pattern) "Expected rejection: $Pattern; observed: $failure"
}
$records = @()
$f = Fixture 'normal'; $j = Journal $f
Add $f $j
Check ($j.phase -ceq 'registered') 'Paired registration missing.'
Remove $f $j
Check ($j.phase -ceq 'complete' -and -not $f.context.tip) 'Cleanup incomplete.'
Check (($f.context.calls -join ',') -ceq 'native:add,x86:add,x86:remove,native:remove') 'Wrong mutation order.'
Check (@($f.context.probes | Where-Object {$_ -ceq 'arm64=True'}).Count -eq 2) 'Shared profile was not checked after x86 removal.'
Check (@($f.context.probes | Select-Object -Last 4 | Where-Object {$_ -match '=False$'}).Count -eq 4) 'Final negative probes missing.'
$records += 'normal registration, companion removal, fresh COM checks and cleanup'
foreach ($kind in @('native','x86','shadow','tip')) {
    $f = Fixture ('existing-' + $kind)
    if ($kind -in @('native','x86')) {$f.context[$kind].exists=$true} else {$f.context[$kind]=$true}
    Rejected { Journal $f } 'requires no existing'
    Check (-not (Test-Path -LiteralPath $f.file) -and $f.context.calls.Count -eq 0) 'Preexisting state was changed.'
}
$records += 'preexisting native/x86 COM, per-user shadow and TSF registration refused'
$f = Fixture 'partial-x86'; $j = Journal $f; $f.context.fail='x86:add:after'
Rejected { Add $f $j } 'x86 test registration failed'
$f.context.fail=''; Remove $f $j
Check ($j.phase -ceq 'complete') 'Partially successful registrar was not recovered.'
$records += 'partial x86 registration recovered from actual owner state'
$f = Fixture 'interrupted-native'; $j = Journal $f
$f.context.native.exists=$true;$f.context.native.dll=$f.context.nativePath;$f.context.tip=$true
$disk=Get-Content $f.file -Raw | ConvertFrom-Json -AsHashtable;$disk.phase='register-native-intent'
$disk | ConvertTo-Json -Depth 6 | Set-Content $f.file
Remove $f $disk
Check ($disk.phase -ceq 'complete') 'Unknown registrar outcome was not recovered.'
$records += 'interrupted native registration recovered without a success flag'
$f = Fixture 'changed-owner'; $j = Journal $f; Add $f $j
$f.context.native.dll='C:\other\nskkzime_tsf.dll';$count=$f.context.calls.Count
Rejected { Remove $f $j } 'not owned by this test'
Check ($f.context.calls.Count -eq $count) 'Another owner caused a partial mutation.'
$records += 'both owners verified before cleanup mutates either view'
$f = Fixture 'cleanup-retry'; $j = Journal $f; Add $f $j;$f.context.fail='x86:remove:before'
Rejected { Remove $f $j } 'cleanup failed'
Check ((Get-Content $f.file -Raw | ConvertFrom-Json).phase -ceq 'unregister-x86-intent') 'Failed cleanup discarded intent.'
$f.context.fail='';Remove $f $j
Remove $f $j
Check ($j.phase -ceq 'complete') 'Cleanup retry or idempotence failed.'
$records += 'failed cleanup retains intent, retries and is idempotent'
$f = Fixture 'user-state-changed';$j = Journal $f;Add $f $j;$f.context.sha='changed'
Rejected { Remove $f $j } 'User input defaults'
Check (-not $f.context.native.exists -and -not $f.context.x86.exists -and $j.phase -ceq 'removed-with-errors') 'User-state audit incorrectly accepted cleanup.'
$records += 'ordinary user-state changes reported without restoring unrelated data'
$f = Fixture 'diagnostic-failure';$j = Journal $f;Add $f $j;$f.context.failProbe='x86=False'
Rejected { Remove $f $j } 'registration is removed, but validation failed'
Check (-not $f.context.native.exists -and -not $f.context.x86.exists -and $j.phase -ceq 'removed-with-errors') 'A diagnostic failure prevented owned registration cleanup.'
$records += 'diagnostic exceptions do not prevent remaining owned registration cleanup'
$f = Fixture 'owned-user-tip';$j = Journal $f;Add $f $j;$f.context.userTip=$true
Remove $f $j
Check (-not $f.context.userTip -and $j.phase -ceq 'complete') 'Recorded owned user TIP state was not restored.'
$records += 'residual per-user TIP restored only from a proven owned snapshot'
$f = Fixture 'unknown-user-tip';$j = Journal $f;$f.context.userTip=$true
Rejected { Remove $f $j } 'without a recorded owned snapshot'
Check ($f.context.userTip -and $f.context.calls.Count -eq 0) 'Unproven per-user state was removed.'
$records += 'unproven residual user TIP state is retained for inspection'
$records | ForEach-Object { Write-Output "PASS $_" }
