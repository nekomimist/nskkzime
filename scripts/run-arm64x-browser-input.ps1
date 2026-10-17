#Requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Start','Status','Watch','Stop')][string]$Action,
    [Parameter(Mandatory)][ValidatePattern('^[a-z0-9][a-z0-9-]{0,63}$')][string]$Name,
    [ValidatePattern('^[a-zA-Z0-9][a-zA-Z0-9_-]{0,79}$')][string]$RunName = 'browser-01',
    [string]$BundleDirectory = $PSScriptRoot,
    [string]$HtmlPath,
    [string]$OriginalImeName
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$bundle = [IO.Path]::GetFullPath($BundleDirectory)
$directory = Join-Path $bundle "reports/registration-$Name/browser/$RunName"
$statePath = Join-Path $directory 'browser-state.json'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
function Save-State($State) { $State | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $statePath -Encoding utf8 }
function Root-Process($State) {
    $process = [Diagnostics.Process]::GetProcessById($State.pid)
    if ($process.StartTime.ToUniversalTime().Ticks -ne ([DateTime]$State.started).ToUniversalTime().Ticks -or
        $process.SessionId -ne $State.session -or
        -not [string]::Equals($process.MainModule.FileName,$State.edge,[StringComparison]::OrdinalIgnoreCase)) {
        $process.Dispose(); throw 'Browser PID identity changed.'
    }
    $command = (Get-CimInstance Win32_Process -Filter ('ProcessId='+$State.pid)).CommandLine
    if ($command.IndexOf(('--user-data-dir='+$State.profile),[StringComparison]::OrdinalIgnoreCase) -lt 0) {
        $process.Dispose(); throw 'Browser profile identity changed.'
    }
    return $process
}
if ($Action -eq 'Start') {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -or
        -not [Environment]::UserInteractive -or [Diagnostics.Process]::GetCurrentProcess().SessionId -eq 0) {
        throw 'Start the browser unelevated in the signed-in user desktop.'
    }
    if ([string]::IsNullOrWhiteSpace($OriginalImeName)) { throw 'Record the original IME name before manual switching.' }
    if (Test-Path -LiteralPath $directory) { throw 'Choose a new RunName; browser reports already exist.' }
    Import-Module (Join-Path $bundle 'installation.psm1') -Force
    $stage = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) "nskkzime-arm64-registration-tests/$Name"
    $journalPath = Join-Path $stage 'registration-test.json'
    Assert-NskkzimeProtectedPath $stage; Assert-NskkzimeProtectedPath $journalPath
    $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json -AsHashtable
    if ($journal.format -cne 'nskkzime-arm64-registration-test-v1' -or $journal.phase -cne 'registered' -or
        $journal.identity.user_sid -cne $identity.User.Value -or $journal.identity.reports -cne (Join-Path $bundle "reports/registration-$Name")) {
        throw 'Test registration or user identity differs.'
    }
    foreach ($image in $journal.identity.images) {
        if ($image.path -cnotmatch '^bin/(x86/)?[a-z0-9_-]+\.(dll|exe)$') { throw 'Invalid registered image path.' }
        $file = Join-Path $stage $image.path
        Assert-NskkzimeProtectedPath ([IO.Path]::GetDirectoryName($file)); Assert-NskkzimeProtectedPath $file
        if ((Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -cne $image.sha256) { throw 'Registered image changed.' }
    }
    foreach ($hive in @([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryHive]::CurrentUser)) {
        $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,[Microsoft.Win32.RegistryView]::Registry64)
        try {
            $key = $base.OpenSubKey('Software\Policies\Microsoft\Edge')
            if ($key) {
                try {
                    foreach ($setting in @('UserDataDir','DownloadDirectory')) {
                        if ($null -ne $key.GetValue($setting,$null)) { throw "Managed Edge $setting prevents this isolated test." }
                    }
                } finally { $key.Dispose() }
            }
        } finally { $base.Dispose() }
    }
    $edge = Join-Path ${env:ProgramFiles(x86)} 'Microsoft/Edge/Application/msedge.exe'
    $bytes = [IO.File]::ReadAllBytes($edge); $pe = [BitConverter]::ToUInt32($bytes,60)
    if ([BitConverter]::ToUInt16($bytes,$pe+4) -ne 0xaa64) { throw 'This test expects native ARM64 Edge.' }
    $manifest = Get-Content -LiteralPath (Join-Path $bundle 'test-images.json') -Raw | ConvertFrom-Json -AsHashtable
    $entry = @($manifest.images | Where-Object path -ceq 'build/Release/nkkz-ime-server.exe')
    $model = Join-Path $bundle 'source/out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf'
    if ($manifest.format -cne 'nskkzime-arm64x-tests-v1' -or $entry.Count -ne 1 -or $entry[0].machine -ne 0xaa64 -or $manifest.source.SHA256 -cne $journal.identity.source.SHA256 -or
        $manifest.model_sha256 -cne '29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673' -or
        (Get-FileHash -LiteralPath $model -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifest.model_sha256) { throw 'Unexpected test model or source.' }
    $sourceServer = Join-Path $bundle $entry[0].path
    if ((Get-FileHash -LiteralPath $sourceServer -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry[0].sha256) { throw 'Test server changed.' }
    New-Item -ItemType Directory -Path (Join-Path $directory 'server') -Force | Out-Null
    $server = Join-Path $directory 'server/nkkz-ime-server.exe'
    Copy-Item -LiteralPath $sourceServer -Destination $server
    if ((Get-FileHash -LiteralPath $server -Algorithm SHA256).Hash.ToLowerInvariant() -cne $entry[0].sha256) { throw 'Copied server changed.' }
    $dictionary = Join-Path $directory 'browser.utf8.skk'
    $dictionaryText = [IO.File]::ReadAllText((Join-Path $bundle 'source/tests/fixtures/native-zenz.utf8.skk')) + "`nてすとぶんり /NSKKZIME-ARM64-ISOLATED/`n"
    [IO.File]::WriteAllText($dictionary,$dictionaryText,[Text.UTF8Encoding]::new($false))
    $config = Join-Path $directory 'settings.ini'
    $settings = "version=1`nlearning=off`ndictionary=$dictionary`ninference=zenz`nzenz-server=$server`nzenz-model=$model`ninference-threads=4`ninference-timeout-ms=2000`nzenz-idle-seconds=10`nzenz-rerank=off`ninput-log=off`nzenz-learn-fallback=off`n"
    [IO.File]::WriteAllText($config,$settings,[Text.UTF8Encoding]::new($false))
    if (-not $HtmlPath) { $HtmlPath = Join-Path $bundle 'source/tests/fixtures/arm64-browser-input.html' }
    $page = Join-Path $directory 'input.html'
    $html = [IO.File]::ReadAllText($HtmlPath).Replace('@@ORIGINAL_IME@@',[Net.WebUtility]::HtmlEncode($OriginalImeName))
    [IO.File]::WriteAllText($page,$html,[Text.UTF8Encoding]::new($false))
    $profile = Join-Path $directory 'edge-profile'
    New-Item -ItemType Directory -Path (Join-Path $profile 'Default') -Force | Out-Null
    @{download=@{default_directory=$directory;prompt_for_download=$false};background_mode=@{enabled=$false}} |
        ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $profile 'Default/Preferences') -Encoding utf8
    $state = @{format='nskkzime-browser-input-v1';phase='prepared';user_sid=$identity.User.Value;edge=$edge;
        profile=$profile;original_ime=$OriginalImeName;expected_dll=$journal.native;source=$journal.identity.source;
        server=$server;server_sha256=$entry[0].sha256;model=$model;config=$config;session=[Diagnostics.Process]::GetCurrentProcess().SessionId;
        script_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant();
        page_sha256=(Get-FileHash -LiteralPath $page -Algorithm SHA256).Hash.ToLowerInvariant()}
    Save-State $state
    $start = [Diagnostics.ProcessStartInfo]::new($edge); $start.UseShellExecute = $false
    $start.Environment['NSKKZIME_CONFIG'] = $config
    foreach ($argument in @(('--user-data-dir='+$profile),'--no-first-run','--no-default-browser-check','--disable-sync','--new-window',([Uri]$page).AbsoluteUri)) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try { $state.pid=$process.Id; $state.started=$process.StartTime.ToUniversalTime().ToString('o'); $state.phase='running'; Save-State $state }
    finally { $process.Dispose() }
    $state | ConvertTo-Json -Depth 6; exit
}
$state = Get-Content -LiteralPath $statePath -Raw | ConvertFrom-Json -AsHashtable
if ($state.format -cne 'nskkzime-browser-input-v1' -or $state.user_sid -cne $identity.User.Value -or
    $state.edge -cne (Join-Path ${env:ProgramFiles(x86)} 'Microsoft/Edge/Application/msedge.exe') -or
    $state.profile -cne (Join-Path $directory 'edge-profile') -or $state.server -cne (Join-Path $directory 'server/nkkz-ime-server.exe')) { throw 'Browser state identity differs.' }
if ($Action -eq 'Watch') {
    $limit = [Environment]::TickCount64 + 900000
    do {
        & $PSCommandPath -Action Status -Name $Name -RunName $RunName -BundleDirectory $bundle | Out-Null
        $latest = Get-Content -LiteralPath (Join-Path $directory 'status.json') -Raw | ConvertFrom-Json -AsHashtable
        if (-not $latest.root_running) { break }
        Start-Sleep -Seconds 2
    } while ([Environment]::TickCount64 -lt $limit)
    exit
}
if ($Action -eq 'Stop') {
    $principal = [Security.Principal.WindowsPrincipal]::new($identity)
    if ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -or
        [Diagnostics.Process]::GetCurrentProcess().SessionId -ne $state.session) { throw 'Stop the browser unelevated in its original desktop session.' }
    & $PSCommandPath -Action Status -Name $Name -RunName $RunName -BundleDirectory $bundle | Out-Null
    $owned = @{}; $tracked = [Collections.Generic.HashSet[int]]::new()
    foreach ($snapshot in Get-ChildItem -LiteralPath $directory -Filter 'snapshot-*.json') {
        $record = Get-Content -LiteralPath $snapshot.FullName -Raw | ConvertFrom-Json -AsHashtable
        foreach ($process in $record.owned_processes) { $owned[([string]$process.pid+'-'+([DateTime]$process.started).ToUniversalTime().Ticks)]=$process }
        foreach ($processId in $record.tracked_pids) { $null=$tracked.Add([int]$processId) }
    }
    if (-not $owned.Count) { throw 'No owned browser process snapshot; closure cannot be verified.' }
    $root = $null
    try { $root = Root-Process $state } catch [ArgumentException] { }
    if ($root) {
        try { $null=$root.CloseMainWindow(); if (-not $root.WaitForExit(5000)) { $root.Kill($true); $root.WaitForExit() } }
        finally { $root.Dispose() }
    }
    $closureLimit = [Environment]::TickCount64 + 5000
    do {
        $survivors = @()
        foreach ($record in $owned.Values) {
            $process = $null
            try {
                $process = [Diagnostics.Process]::GetProcessById($record.pid)
                if ($process.StartTime.ToUniversalTime().Ticks -eq ([DateTime]$record.started).ToUniversalTime().Ticks -and
                    [string]::Equals($process.MainModule.FileName,$record.path,[StringComparison]::OrdinalIgnoreCase) -and
                    $process.SessionId -eq $record.session) { $survivors += $record.pid }
            } catch [ArgumentException] { }
            catch { if ($process -and -not $process.HasExited) { throw } }
            finally { if($process){$process.Dispose()} }
        }
        foreach ($processId in $tracked | Where-Object { $_ -notin @($owned.Values.pid) }) {
            $process=$null
            try { $process=[Diagnostics.Process]::GetProcessById($processId); if(-not $process.HasExited){$survivors += $processId} }
            catch [ArgumentException] { }
            finally { if($process){$process.Dispose()} }
        }
        if (-not $survivors.Count) { break }
        Start-Sleep -Milliseconds 100
    } while ([Environment]::TickCount64 -lt $closureLimit)
    if ($survivors.Count) { throw 'Owned browser children remain; keep files and inspect snapshots.' }
    $limit = [Environment]::TickCount64 + 15000
    do {
        $remaining = @()
        foreach ($process in [Diagnostics.Process]::GetProcessesByName('nkkz-ime-server')) {
            try { if(-not $process.HasExited -and [string]::Equals($process.MainModule.FileName,$state.server,[StringComparison]::OrdinalIgnoreCase)){$remaining += $process.Id} }
            catch { if(-not $process.HasExited){throw} }
            finally { $process.Dispose() }
        }
        if (-not $remaining.Count) { break }
        Start-Sleep -Milliseconds 100
    } while ([Environment]::TickCount64 -lt $limit)
    if ($remaining.Count) { throw 'The isolated browser inference server did not idle out; keep its files.' }
    $state.phase='stopped'; $state.closed_processes=$tracked.Count; $state.server_idle_exit=$true; Save-State $state
}
$modules = @(); $ownedProcesses=@(); $trackedPids=@(); $servers=@(); $rootRunning=$false
try {
    $root = Root-Process $state; $rootRunning=$true; $root.Dispose()
    $all = @(Get-CimInstance Win32_Process -Filter "Name='msedge.exe'" | Where-Object SessionId -eq $state.session)
    $ids = [Collections.Generic.HashSet[int]]::new(); $null=$ids.Add([int]$state.pid)
    do { $added=$false; foreach ($item in $all) { if ($ids.Contains([int]$item.ParentProcessId) -and $ids.Add([int]$item.ProcessId)) { $added=$true } } } while ($added)
    $trackedPids=@($ids)
    foreach ($item in $all | Where-Object { $ids.Contains([int]$_.ProcessId) }) {
        $process=$null
        try {
            $process=[Diagnostics.Process]::GetProcessById($item.ProcessId)
            $ownedProcesses += @{pid=$process.Id;started=$process.StartTime.ToUniversalTime().ToString('o');path=$process.MainModule.FileName;session=$process.SessionId}
            foreach ($module in $process.Modules | Where-Object ModuleName -ieq 'nskkzime_tsf.dll') {
                $modules += @{pid=$item.ProcessId;type=$(if($item.CommandLine -match '--type=([^\s"]+)'){$Matches[1]}else{'browser'});
                    dll=$module.FileName;expected=[string]::Equals($module.FileName,$state.expected_dll,[StringComparison]::OrdinalIgnoreCase)}
            }
        } catch { $modules += @{pid=$item.ProcessId;inspection_error=$_.Exception.GetType().Name} }
        finally { if($process){$process.Dispose()} }
    }
} catch [ArgumentException] { }
foreach ($process in [Diagnostics.Process]::GetProcessesByName('nkkz-ime-server')) {
    try {
        if (-not $process.HasExited -and [string]::Equals($process.MainModule.FileName,$state.server,[StringComparison]::OrdinalIgnoreCase)) {
            $servers += @{pid=$process.Id;path=$process.MainModule.FileName;started=$process.StartTime.ToUniversalTime().ToString('o');machine=0xaa64}
        }
    } catch { if(-not $process.HasExited){throw} }
    finally { $process.Dispose() }
}
$resultPath = Join-Path $directory 'browser-result.json'
$result = if(Test-Path -LiteralPath $resultPath){Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json -AsHashtable}else{$null}
$status = @{phase=$state.phase;root_running=$rootRunning;tracked_pids=$trackedPids;owned_processes=$ownedProcesses;modules=$modules;servers=$servers;manual_result=$result;state=$state;
    observer_sha256=(Get-FileHash -LiteralPath $PSCommandPath -Algorithm SHA256).Hash.ToLowerInvariant()}
$status | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $directory ('snapshot-'+[Guid]::NewGuid().ToString('N')+'.json')) -Encoding utf8
$status | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath (Join-Path $directory 'status.json') -Encoding utf8
$status | ConvertTo-Json -Depth 12
