#Requires -Version 7.0
[CmdletBinding()]
param(
    [ValidateSet('Plan','Register','Unregister','Recover','Status')][string]$Action = 'Plan',
    [ValidatePattern('^[a-z0-9][a-z0-9-]{0,63}$')][string]$Name = 'arm64x',
    [string]$BundleDirectory = $PSScriptRoot
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
if (-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne 'Arm64' -or
    [Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture -ne 'Arm64') { throw 'Use native ARM64 PowerShell 7 on Windows.' }
Import-Module (Join-Path $PSScriptRoot 'installation.psm1') -Force
Import-Module (Join-Path $PSScriptRoot 'arm64-registration-test.psm1') -Force
$bundle = [IO.Path]::GetFullPath($BundleDirectory)
$parent = Join-Path ([Environment]::GetFolderPath('ProgramFiles')) 'nskkzime-arm64-registration-tests'
$stage = Join-Path $parent $Name
$reports = Join-Path $bundle "reports/registration-$Name"
$journalPath = Join-Path $stage 'registration-test.json'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
$sid = $identity.User.Value
$classPath = 'Software\Classes\CLSID\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}'
$tipPath = 'Software\Microsoft\CTF\TIP\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}'
$native = Join-Path $stage 'bin/nskkzime_tsf.dll'
$x86 = Join-Path $stage 'bin/x86/nskkzime_tsf.dll'
$inspectTsf = $false
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Registry-Tree([Microsoft.Win32.RegistryKey]$Root, [string]$Path, [int]$Depth = 0) {
    if ($Depth -gt 8) { throw 'Unexpected registry snapshot depth.' }
    $key = $Root.OpenSubKey($Path)
    if (-not $key) { return $null }
    try {
        $values = [ordered]@{}; $children = [ordered]@{}
        foreach ($name in @($key.GetValueNames() | Sort-Object)) {
            $values[$name] = [ordered]@{kind=$key.GetValueKind($name).ToString();value=$key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)}
        }
        foreach ($name in @($key.GetSubKeyNames() | Sort-Object)) { $children[$name] = Registry-Tree $key $name ($Depth + 1) }
        return [ordered]@{values=$values;children=$children}
    } finally { $key.Dispose() }
}
function Read-State {
    $state = @{native=@{exists=$false;dll=$null};x86=@{exists=$false;dll=$null};shadow=$false;tip=$false;machine_tip=$false;
        user_tip=[ordered]@{};tip_views=[ordered]@{}}
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)) {
        $slot = if ($view -eq 'Registry64') { 'native' } else { 'x86' }
        foreach ($hive in @([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryHive]::CurrentUser)) {
            $root = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,$view)
            try {
                $key = $root.OpenSubKey($classPath)
                if ($key) {
                    try {
                        if ($hive -eq 'CurrentUser') { $state.shadow = $true }
                        else {
                            $state[$slot].exists = $true
                            $server = $key.OpenSubKey('InprocServer32')
                            if ($server) {
                                try {
                                    if ($server.GetValueKind('') -ne 'String') { throw 'Malformed COM owner path.' }
                                    $state[$slot].dll = $server.GetValue('',$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                                } finally { $server.Dispose() }
                            }
                        }
                    } finally { $key.Dispose() }
                }
                $tip = Registry-Tree $root $tipPath
                $state.tip_views[$view.ToString() + '/' + $hive.ToString()] = $tip
                if ($hive -eq 'CurrentUser') { $state.user_tip[$view.ToString()] = $tip }
                if ($null -ne $tip) {
                    $state.tip = $true
                    if ($hive -eq 'LocalMachine') { $state.machine_tip = $true }
                }
            } finally { $root.Dispose() }
        }
    }
    if ($inspectTsf) {
        $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $stage 'bin/tsf_architecture_tests-arm64.exe'))
        $start.UseShellExecute = $false; $start.CreateNoWindow = $true; $start.WorkingDirectory = $reports
        $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
        $start.ArgumentList.Add('--registration-state')
        $process = [Diagnostics.Process]::Start($start)
        try {
            $output = $process.StandardOutput.ReadToEndAsync(); $errors = $process.StandardError.ReadToEndAsync()
            if (-not $process.WaitForExit(30000)) { $process.Kill(); $process.WaitForExit(); throw 'TSF state inspection timed out.' }
            $json = $output.GetAwaiter().GetResult(); $errorText = $errors.GetAwaiter().GetResult()
            if ($process.ExitCode) { throw "TSF state inspection failed: $errorText" }
            $state.tsf_api = $json | ConvertFrom-Json -AsHashtable
            if ($state.tsf_api.processor_registered -or $state.tsf_api.profile_exists -or $state.tsf_api.category_count -ne 0) {
                $state.machine_tip = $true; $state.tip = $true
            }
        } finally { $process.Dispose() }
    }
    $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,[Microsoft.Win32.RegistryView]::Registry64)
    try {
        $defaults = [ordered]@{}
        foreach ($path in @('Keyboard Layout','Control Panel\International\User Profile')) { $defaults[$path] = Registry-Tree $user $path }
        $state.user_defaults = $defaults
    } finally { $user.Dispose() }
    $ordinary = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'nskkzime'
    $files = [ordered]@{}
    if (Test-Path -LiteralPath $ordinary) {
        Assert-NskkzimePath $ordinary
        foreach ($file in Get-ChildItem -LiteralPath $ordinary -File -Recurse | Sort-Object FullName) {
            Assert-NskkzimePath $file.FullName
            $files[[IO.Path]::GetRelativePath($ordinary,$file.FullName)] = Hash $file.FullName
        }
    }
    $state.ordinary_files = $files
    $json = [ordered]@{defaults=$state.user_defaults;files=$files} | ConvertTo-Json -Depth 24 -Compress
    $state.user_state_sha256 = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([Text.Encoding]::UTF8.GetBytes($json))).ToLowerInvariant()
    return $state
}
function Check-Stage($Journal) {
    Assert-NskkzimeProtectedPath $parent
    Assert-NskkzimeProtectedPath $stage
    Assert-NskkzimeProtectedPath $journalPath
    if ($Journal.format -cne 'nskkzime-arm64-registration-test-v1' -or $Journal.identity.user_sid -cne $sid -or
        $Journal.identity.reports -cne $reports -or $Journal.native -cne $native -or $Journal.x86 -cne $x86) { throw 'Test journal identity differs.' }
    if ($Journal.identity.images.Count -ne 10) { throw 'Unexpected test inventory.' }
    foreach ($image in $Journal.identity.images) {
        if ($image.path -cnotmatch '^bin/(x86/)?[a-z0-9_-]+\.(dll|exe)$') { throw 'Invalid test image path.' }
        $file = Join-Path $stage $image.path
        Assert-NskkzimeProtectedPath ([IO.Path]::GetDirectoryName($file))
        Assert-NskkzimeProtectedPath $file
        if ((Hash $file) -cne $image.sha256) { throw "Test image changed; do not register or unregister it: $file" }
    }
}
function Invoke-Registrar([string]$Architecture, [bool]$Unregister) {
    $directory = if ($Architecture -eq 'x86') { Join-Path $env:SystemRoot 'SysWOW64' } else { [Environment]::SystemDirectory }
    $dll = if ($Architecture -eq 'x86') { $x86 } else { $native }
    $start = [Diagnostics.ProcessStartInfo]::new((Join-Path $directory 'regsvr32.exe'))
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true
    $start.ArgumentList.Add('/s'); if ($Unregister) { $start.ArgumentList.Add('/u') }; $start.ArgumentList.Add($dll)
    $process = [Diagnostics.Process]::Start($start)
    # Do not terminate a registrar whose outcome is unknown. Recovery refuses
    # while it is running, and the intent journal remains available.
    try {
        if (-not $process.WaitForExit(60000)) { throw 'Registrar is still running; wait before Recover.' }
        return $process.ExitCode
    } finally { $process.Dispose() }
}
function Invoke-Probe([string]$Architecture, [bool]$Present) {
    $file = if ($Architecture -eq 'x86') { Join-Path $stage 'bin/x86/tsf_architecture_tests.exe' }
        else { Join-Path $stage "bin/tsf_architecture_tests-$Architecture.exe" }
    $directory = Join-Path $reports ('com-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $directory | Out-Null
    $start = [Diagnostics.ProcessStartInfo]::new($file)
    $start.UseShellExecute = $false; $start.CreateNoWindow = $true; $start.WorkingDirectory = $directory
    $start.RedirectStandardOutput = $true; $start.RedirectStandardError = $true
    $start.ArgumentList.Add('--registered'); $start.ArgumentList.Add($(if ($Architecture -eq 'x86') { $x86 } else { $native }))
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync(); $stderr = $process.StandardError.ReadToEndAsync()
        if (-not $process.WaitForExit(30000)) { $process.Kill(); $process.WaitForExit(); throw 'COM probe timed out.' }
        $output = $stdout.GetAwaiter().GetResult(); $errorText = $stderr.GetAwaiter().GetResult()
        [IO.File]::WriteAllText((Join-Path $directory 'stdout.log'),$output)
        [IO.File]::WriteAllText((Join-Path $directory 'stderr.log'),$errorText)
        if ($Present) {
            if ($process.ExitCode) { throw "Registered $Architecture COM probe failed: $errorText" }
            $record = $output | ConvertFrom-Json -AsHashtable
            if ($record.host -cne $Architecture -or -not $record.registered_com -or
                $record.source_sha256 -cne $journal.identity.source.SHA256) { throw 'Registered COM identity differs.' }
        } elseif ($process.ExitCode -ne 1 -or $errorText -notmatch 'CoCreateInstance HRESULT 0x80040154') {
            throw "Expected REGDB_E_CLASSNOTREG in a fresh $Architecture host: $errorText"
        }
        [ordered]@{architecture=$Architecture;present=$Present;exit_code=$process.ExitCode;directory=$directory} |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'result.json')
    } finally { $process.Dispose() }
}
function Restore-UserTip($Expected) {
    # The initial per-user key was absent. Remove only an exact snapshot
    # recorded while this test still owned the native machine registration.
    foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)) {
        $state = Read-State
        if ($state.native.exists -or $state.x86.exists -or $state.machine_tip -or $state.shadow) { throw 'COM or machine TSF state changed during user cleanup.' }
        $user = [Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::CurrentUser,$view)
        try {
            $actual = Registry-Tree $user $tipPath
            if ($null -eq $actual) { continue } # Views can share this key.
            $recorded = $Expected[$view.ToString()]
            if ($null -eq $recorded -or ($actual | ConvertTo-Json -Depth 24 -Compress) -cne ($recorded | ConvertTo-Json -Depth 24 -Compress)) {
                throw 'User TIP state differs from the owned snapshot. No user state was deleted.'
            }
            $user.DeleteSubKeyTree($tipPath,$false)
        } finally { $user.Dispose() }
    }
}
$current = Read-State
if ($Action -eq 'Plan') {
    [ordered]@{action='Plan';name=$Name;stage=$stage;reports=$reports;state=$current;
        can_register=(-not ($current.shadow -or $current.native.exists -or $current.x86.exists -or $current.tip));
        sequence=@('ARM64X registration','fresh ARM64/x64/ARM64EC COM probes','x86 registration and COM probe',
            'unelevated interactive input probes','x86 removal and shared-profile check','native removal and restoration audit')} | ConvertTo-Json -Depth 24
    return
}
if ($Action -eq 'Status') {
    [ordered]@{stage=$stage;state=$current;journal=$(if(Test-Path -LiteralPath $journalPath){Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json})} | ConvertTo-Json -Depth 24
    return
}
$principal = [Security.Principal.WindowsPrincipal]::new($identity)
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Use an elevated shell for registration or cleanup.' }
if (@(Get-Process -Name regsvr32 -ErrorAction SilentlyContinue).Count) { throw 'regsvr32 is running. Wait before changing registration or recovering.' }
if ($Action -eq 'Register') {
    if ($current.shadow -or $current.native.exists -or $current.x86.exists -or $current.tip) { throw 'Existing COM or TSF registration must be left untouched.' }
    Assert-NskkzimePath $parent
    $security = [Security.AccessControl.DirectorySecurity]::new()
    $security.SetSecurityDescriptorSddlForm('O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)')
    if (-not (Test-Path -LiteralPath $parent)) { [IO.FileSystemAclExtensions]::CreateDirectory($security,$parent) | Out-Null }
    Assert-NskkzimeProtectedPath $parent
    if (Test-Path -LiteralPath $stage) { throw 'Test name already exists. Use a new name, or Recover.' }
    if (Test-Path -LiteralPath $reports) { throw 'Test reports already exist. Use a new name.' }
    $manifest = Get-Content -LiteralPath (Join-Path $bundle 'test-images.json') -Raw | ConvertFrom-Json -AsHashtable
    if ($manifest.format -cne 'nskkzime-arm64x-tests-v1') { throw 'Invalid test bundle.' }
    $names = @('nskkzime_tsf.dll','x86/nskkzime_tsf.dll','x86/tsf_architecture_tests.exe','x86/ime_probe.exe')
    foreach ($architecture in @('arm64','x64','arm64ec')) { $names += "tsf_architecture_tests-$architecture.exe"; $names += "ime_probe-$architecture.exe" }
    [IO.Directory]::CreateDirectory($stage) | Out-Null
    New-Item -ItemType Directory -Path $reports | Out-Null
    $images = @()
    foreach ($imageName in $names) {
        $entry = @($manifest.images | Where-Object path -ceq ('build/Release/' + $imageName))
        $sourceFile = Join-Path $bundle ('build/Release/' + $imageName)
        if ($entry.Count -ne 1 -or (Hash $sourceFile) -cne $entry[0].sha256) { throw "Unverified test image: $imageName" }
        $destination = Join-Path $stage ('bin/' + $imageName)
        New-Item -ItemType Directory -Path ([IO.Path]::GetDirectoryName($destination)) -Force | Out-Null
        Copy-Item -LiteralPath $sourceFile -Destination $destination
        Assert-NskkzimeProtectedPath $destination
        if ((Hash $destination) -cne $entry[0].sha256) { throw "Image changed during copying: $imageName" }
        $images += @{path=('bin/' + $imageName);sha256=$entry[0].sha256}
    }
    $testIdentity = @{user_sid=$sid;reports=$reports;source=$manifest.source;images=$images}
    $journal = New-Arm64RegistrationJournal $journalPath $native $x86 $testIdentity ${function:Read-State}
} else {
    Assert-NskkzimeProtectedPath $journalPath
    if ((Get-Item -LiteralPath $journalPath).Length -gt 128KB) { throw 'Test journal is too large.' }
    $journal = Get-Content -LiteralPath $journalPath -Raw | ConvertFrom-Json -AsHashtable
}
Check-Stage $journal
$inspectTsf = $true
$lock = [IO.File]::Open((Join-Path $stage 'operation.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
try {
    Read-State | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $reports ('before-' + $Action + '.json'))
    if ($Action -eq 'Register') {
        try { Add-Arm64TestRegistration $journalPath $journal ${function:Read-State} ${function:Invoke-Registrar} ${function:Invoke-Probe} }
        catch {
            $original = $_
            if (@(Get-Process -Name regsvr32 -ErrorAction SilentlyContinue).Count -eq 0) {
                try { Remove-Arm64TestRegistration $journalPath $journal ${function:Read-State} ${function:Invoke-Registrar} ${function:Invoke-Probe} ${function:Restore-UserTip} }
                catch { throw "Registration failed: $original / cleanup pending: $_. Use Recover with name $Name." }
            }
            throw $original
        }
    } else { Remove-Arm64TestRegistration $journalPath $journal ${function:Read-State} ${function:Invoke-Registrar} ${function:Invoke-Probe} ${function:Restore-UserTip} }
    [ordered]@{name=$Name;phase=$journal.phase;stage=$stage;reports=$reports} | ConvertTo-Json
} finally {
    try { Read-State | ConvertTo-Json -Depth 24 | Set-Content -LiteralPath (Join-Path $reports ('after-' + $Action + '.json')) }
    finally { $lock.Dispose() }
}
