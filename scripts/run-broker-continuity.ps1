#Requires -Version 7.0
[CmdletBinding()]
param([ValidateSet('Run','Status','Resume','Stop')][string]$Action='Status',
    [Parameter(Mandatory)][string]$RunDirectory,[string]$BinaryDirectory,[string]$Model)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath($RunDirectory)
$sid=[Security.Principal.WindowsIdentity]::GetCurrent().User.Value
function Save([string]$Name,$Value){$Value|ConvertTo-Json -Depth 35|Set-Content -LiteralPath (Join-Path $root ($Name+'.json')) -Encoding utf8}
function Registry-Tree([Microsoft.Win32.RegistryKey]$Key){
    if(-not $Key){return $null}
    try{
        $values=[ordered]@{};$children=[ordered]@{}
        foreach($name in $Key.GetValueNames()|Sort-Object -CaseSensitive){$values[$name]=[ordered]@{kind=$Key.GetValueKind($name).ToString();value=$Key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}}
        foreach($name in $Key.GetSubKeyNames()|Sort-Object -CaseSensitive){$children[$name]=Registry-Tree ($Key.OpenSubKey($name))}
        [ordered]@{values=$values;children=$children}
    }finally{$Key.Dispose()}
}
function Ordinary-State {
    $state=[ordered]@{}
    foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
        foreach($hive in @([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryHive]::LocalMachine)){
            $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,$view)
            try{foreach($path in @('Software\Classes\CLSID\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}','Software\Microsoft\CTF\TIP\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}')){$state["$view/$hive/$path"]=Registry-Tree ($base.OpenSubKey($path))}}
            finally{$base.Dispose()}
        }
    }
    foreach($path in @('Keyboard Layout','Control Panel\International\User Profile')){$state[$path]=Registry-Tree ([Microsoft.Win32.Registry]::CurrentUser.OpenSubKey($path))}
    $run=[Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\Windows\CurrentVersion\Run')
    try{$state.startup=if($run -and $run.GetValueNames() -contains 'nskkzime broker'){[ordered]@{kind=$run.GetValueKind('nskkzime broker').ToString();value=$run.GetValue('nskkzime broker',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}}else{$null}}
    finally{if($run){$run.Dispose()}}
    $files=[ordered]@{};$ordinary=Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'nskkzime'
    if(Test-Path -LiteralPath $ordinary){foreach($file in Get-ChildItem -LiteralPath $ordinary -File -Recurse -Force|Sort-Object FullName){$files[[IO.Path]::GetRelativePath($ordinary,$file.FullName)]=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}}
    $state.files=$files
    $state
}
function Process-Identity([Diagnostics.Process]$Process){
    @{pid=$Process.Id;started_utc_ticks=$Process.StartTime.ToUniversalTime().Ticks;path=$Process.Path;sha256=(Get-FileHash -LiteralPath $Process.Path -Algorithm SHA256).Hash.ToLowerInvariant()}
}
function Owned-Servers {
    @(Get-CimInstance Win32_Process -Filter "Name='nkkz-ime-server.exe'"|Where-Object {[string]::Equals($_.ExecutablePath,(Join-Path $root 'bin/nkkz-ime-server.exe'),[StringComparison]::OrdinalIgnoreCase)}|Select-Object ProcessId,SessionId,CreationDate,ExecutablePath)
}
function Events {
    $file=Join-Path $root 'probe.jsonl'
    if(Test-Path -LiteralPath $file){Get-Content -LiteralPath $file|Where-Object {$_}|ForEach-Object {$_|ConvertFrom-Json}}
}
if($Action -ne 'Run'){
    $context=Get-Content -LiteralPath (Join-Path $root 'context.json') -Raw|ConvertFrom-Json
    if($context.user_sid -cne $sid -or $context.root -cne $root){throw 'Another continuity run/user.'}
    if($Action -eq 'Status'){
        $resultFile=Join-Path $root 'result.json'
        [pscustomobject]@{context=$context;events=@(Events);servers=@(Owned-Servers);result=$(if(Test-Path $resultFile){Get-Content -LiteralPath $resultFile -Raw|ConvertFrom-Json}else{$null})};return
    }
    $process=Get-Process -Id $context.probe.pid -ErrorAction Stop
    try{if($process.StartTime.ToUniversalTime().Ticks -ne $context.probe.started_utc_ticks -or -not [string]::Equals($process.Path,$context.probe.path,[StringComparison]::OrdinalIgnoreCase) -or
        (Get-FileHash -LiteralPath $process.Path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $context.probe.sha256){throw 'Probe identity changed.'}}
    finally{$process.Dispose()}
    $signal=if($Action -eq 'Resume'){'--resume'}else{'--stop'}
    & $context.probe.path $signal $context.token
    if($LASTEXITCODE){throw 'Checkpoint signal failed.'}
    return
}
if(-not $IsWindows){throw 'Windows required.'}
$principal=[Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)){throw 'Run the probe as the normal signed-in user.'}
if(Test-Path -LiteralPath $root){throw 'Use a new continuity run directory.'}
foreach($path in @($root,$BinaryDirectory,$Model)){if(-not [IO.Path]::IsPathFullyQualified($path) -or $path.IndexOfAny([char[]]@('"',"`r","`n")) -ge 0){throw 'Use absolute local paths without quotes or newlines.'}}
if((Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash.ToLowerInvariant() -cne '29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673'){throw 'Use the pinned test model.'}
$machine=switch([Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString()){'Arm64'{0xaa64};'X64'{0x8664};default{throw 'Unsupported OS architecture.'}}
foreach($name in @('broker_continuity_probe.exe','nskkzime_broker.exe','nkkz-ime-server.exe')){
    $bytes=[IO.File]::ReadAllBytes((Join-Path $BinaryDirectory $name))
    if($bytes.Length -lt 64){throw 'Invalid executable.'}
    $pe=[BitConverter]::ToUInt32($bytes,60)
    if($pe -gt $bytes.Length-26 -or [BitConverter]::ToUInt32($bytes,$pe) -ne 0x4550 -or [BitConverter]::ToUInt16($bytes,$pe+4) -ne $machine){throw 'Continuity executables must match the native OS architecture.'}
}
[IO.Directory]::CreateDirectory((Join-Path $root 'bin'))|Out-Null
foreach($name in @('broker_continuity_probe.exe','nskkzime_broker.exe','nkkz-ime-server.exe')){[IO.File]::Copy((Join-Path $BinaryDirectory $name),(Join-Path $root ('bin/'+$name)))}
$before=Ordinary-State;Save 'before' $before
$token=[Guid]::NewGuid().ToString('N');$config=Join-Path $root 'settings.ini';$dict=Join-Path $root 'dictionary.skk'
[IO.File]::WriteAllText($dict,"かんじ /NSKKZIME-CONTINUITY/`n",[Text.UTF8Encoding]::new($false))
$server=Join-Path $root 'bin/nkkz-ime-server.exe';$broker=Join-Path $root 'bin/nskkzime_broker.exe';$probe=Join-Path $root 'bin/broker_continuity_probe.exe'
[IO.File]::WriteAllText($config,"version=1`nlearning=off`ndictionary=$dict`ninference=zenz`nzenz-model=$Model`nzenz-server=$server`ninference-threads=4`ninference-timeout-ms=5000`nzenz-score-timeout-ms=5000`nzenz-idle-seconds=10`ninput-log=off`n",[Text.UTF8Encoding]::new($false))
$brokerProcess=$null;$probeProcess=$null;$errorText=$null;$passed=$false;$cleanupErrors=@()
try{
    $start=[Diagnostics.ProcessStartInfo]::new($broker);$start.UseShellExecute=$false;$start.CreateNoWindow=$true
    foreach($argument in @('--replace','--config',$config,'--test-package',('nskkzime-continuity-'+$token))){$start.ArgumentList.Add($argument)}
    $brokerProcess=[Diagnostics.Process]::Start($start)
    # Wait only for startup; no polling or periodic work runs during the checkpoint.
    Start-Sleep -Milliseconds 500
    if($brokerProcess.HasExited){throw 'Dedicated broker exited during startup.'}
    $arguments='--config "'+$config+'" --checkpoint '+$token
    $probeProcess=Start-Process -FilePath $probe -ArgumentList $arguments -WindowStyle Hidden -RedirectStandardOutput (Join-Path $root 'probe.jsonl') -RedirectStandardError (Join-Path $root 'probe-stderr.log') -PassThru
    $context=@{root=$root;token=$token;user_sid=$sid;session=[Diagnostics.Process]::GetCurrentProcess().SessionId;os_architecture=[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString();powershell_architecture=[Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString();elevated=$false;probe=(Process-Identity $probeProcess);broker=(Process-Identity $brokerProcess);config_sha256=(Get-FileHash -LiteralPath $config -Algorithm SHA256).Hash.ToLowerInvariant()}
    Save 'context' $context
    if(-not $probeProcess.WaitForExit(30*60*1000)){throw 'Continuity probe exceeded the run limit.'}
    $last=@(Events)[-1]
    if($probeProcess.ExitCode -ne 0 -or $last.event -cne 'passed'){throw ('Continuity probe failed: '+($last|ConvertTo-Json -Compress))}
    $passed=$true
}catch{$errorText=$_.ToString()}
finally{
    if($probeProcess -and -not $probeProcess.HasExited){
        & $probe --stop $token
        if(-not $probeProcess.WaitForExit(15000)){$cleanupErrors+='Probe did not stop; retain its files.'}
    }
    if($brokerProcess){
        $start=[Diagnostics.ProcessStartInfo]::new($broker);$start.UseShellExecute=$false;$start.CreateNoWindow=$true
        foreach($argument in @('--stop','--config',$config)){$start.ArgumentList.Add($argument)}
        $stop=[Diagnostics.Process]::Start($start)
        try{if(-not $stop.WaitForExit(15000) -or $stop.ExitCode -ne 0){$cleanupErrors+='Broker stop helper failed.'}}finally{$stop.Dispose()}
        if(-not $brokerProcess.WaitForExit(15000)){$cleanupErrors+='Broker remains; retain its files.'}
        $brokerProcess.Dispose()
    }
    foreach($item in @(Owned-Servers)){
        $process=Get-Process -Id $item.ProcessId -ErrorAction SilentlyContinue
        if($process){try{if(-not $process.WaitForExit(25000)){$cleanupErrors+='Private server remains; retain its files.'}}finally{$process.Dispose()}}
    }
    if($probeProcess){$probeProcess.Dispose()}
    $after=Ordinary-State;Save 'after' $after
    $unchanged=($before|ConvertTo-Json -Depth 35 -Compress -WarningAction Stop) -ceq ($after|ConvertTo-Json -Depth 35 -Compress -WarningAction Stop)
    Save 'result' @{passed=($passed -and $unchanged -and $cleanupErrors.Count -eq 0);probe_passed=$passed;ordinary_state_unchanged=$unchanged;cleanup_errors=$cleanupErrors;error=$errorText;actual_sleep_verified=$false;registration_tested=$false;input_tested=$false}
}
if(-not $passed -or -not $unchanged -or $cleanupErrors.Count){throw 'Continuity run failed; inspect the retained reports.'}
