#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Archive,[Parameter(Mandatory)][string]$ExpectedSHA256,
    [Parameter(Mandatory)][string]$ExpectedSourceSHA256,[Parameter(Mandatory)][string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
Import-Module (Join-Path $PSScriptRoot 'arm64-ci.psm1') -Force
if(-not $IsWindows -or [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne [Runtime.InteropServices.Architecture]::Arm64){throw 'CI execution requires Windows ARM64.'}
foreach($hash in @($ExpectedSHA256,$ExpectedSourceSHA256)){if($hash -cnotmatch '^[a-f0-9]{64}$'){throw 'Invalid expected hash.'}}
if((Get-FileHash -LiteralPath $Archive -Algorithm SHA256).Hash.ToLowerInvariant() -cne $ExpectedSHA256){throw 'CI archive changed.'}
$root=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $root){throw 'Use a new CI execution directory.'}
$zip=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Archive))
try {
    $names=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase);$total=0L
    foreach($entry in $zip.Entries){
        $name=$entry.FullName.TrimEnd('/')
        Assert-NskkzimeCiRelativePath $name
        if(-not $names.Add($name)){throw 'Duplicate CI ZIP entry.'}
        $total+=$entry.Length
        if($total -gt 512MB){throw 'CI ZIP is too large.'}
    }
    [IO.Compression.ZipFileExtensions]::ExtractToDirectory($zip,$root)
}finally{$zip.Dispose()}
$manifest=Get-Content -LiteralPath (Join-Path $root 'ci-manifest.json') -Raw | ConvertFrom-Json
if($manifest.format -cne 'nskkzime-arm64x-ci-v1' -or $manifest.profile -cne 'ci-headless-no-model' -or $manifest.source.SHA256 -cne $ExpectedSourceSHA256){throw 'Unexpected CI bundle identity.'}
$expected=@(Get-NskkzimeArm64CiTests | Sort-Object)
if((@($manifest.tests | Sort-Object) -join "`n") -cne ($expected -join "`n")){throw 'CI test inventory differs.'}
$inventory=[Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$null=$inventory.Add('ci-manifest.json')
foreach($file in $manifest.files){
    Assert-NskkzimeCiRelativePath $file.path
    if(-not $inventory.Add($file.path)){throw 'Duplicate CI file inventory.'}
    $path=Join-Path $root $file.path
    if((Get-Item -LiteralPath $path).Length -ne $file.size -or (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -cne $file.sha256){throw "CI file changed: $($file.path)"}
}
$actual=@(Get-ChildItem -LiteralPath $root -File -Recurse -Force)
if($actual.Count -ne $inventory.Count){throw 'Unexpected CI files.'}
foreach($file in $actual){if(-not $inventory.Contains([IO.Path]::GetRelativePath($root,$file.FullName).Replace('\','/'))){throw 'Unverified CI file.'}}
$report=Join-Path $root 'reports';[IO.Directory]::CreateDirectory($report) | Out-Null
$runtimes=[ordered]@{}
foreach($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)){
    $base=[Microsoft.Win32.RegistryKey]::OpenBaseKey([Microsoft.Win32.RegistryHive]::LocalMachine,$view)
    try{foreach($abi in @('arm64','x64','x86')){
        $key=$base.OpenSubKey('Software\Microsoft\VisualStudio\14.0\VC\Runtimes\'+$abi)
        try{$runtimes["$view/$abi"]=if($key){$key.GetValue('Version')}else{$null}}finally{if($key){$key.Dispose()}}
    }}finally{$base.Dispose()}
}
$environment=[ordered]@{os=[Environment]::OSVersion.VersionString;os_architecture=[Runtime.InteropServices.RuntimeInformation]::OSArchitecture.ToString();powershell_architecture=[Runtime.InteropServices.RuntimeInformation]::ProcessArchitecture.ToString();session=[Diagnostics.Process]::GetCurrentProcess().SessionId;image=$env:ImageVersion;runtimes=$runtimes;build=(Get-Content -LiteralPath (Join-Path $root 'build-environment.json') -Raw|ConvertFrom-Json);source=$manifest.source;archive_sha256=$ExpectedSHA256;tests=$expected}
$environment | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $report 'environment.json') -Encoding utf8
$template=[IO.File]::ReadAllText((Join-Path $root 'build/CTestTestfile.cmake.in'))
if($template -match '[a-zA-Z]:[\\/]|\\\\'){throw 'Developer path in CI CTest template.'}
$pwsh=(Get-Command pwsh -CommandType Application | Select-Object -First 1).Source.Replace('\','/')
$testFile=$template.Replace('@BUNDLE@',$root.Replace('\','/')).Replace('@PWSH@',$pwsh)
[IO.File]::WriteAllText((Join-Path $root 'build/CTestTestfile.cmake'),$testFile,[Text.UTF8Encoding]::new($false))
$ctest=Join-Path $root 'tools/bin/ctest.exe'
$plan=& $ctest --test-dir (Join-Path $root 'build') -C Release --show-only=json-v1
if($LASTEXITCODE){throw 'Cannot inspect CI test plan.'}
$planned=@((($plan -join "`n")|ConvertFrom-Json).tests.name | Sort-Object)
if(($planned -join "`n") -cne ($expected -join "`n")){throw 'Generated CTest plan differs from the exact CI allowlist.'}
& $ctest --test-dir (Join-Path $root 'build') -C Release --timeout 120 --output-on-failure --no-tests=error --output-junit (Join-Path $report 'results.xml') --output-log (Join-Path $report 'ctest.log')
if($LASTEXITCODE){throw 'ARM64 CI tests failed.'}
[xml]$junit=Get-Content -LiteralPath (Join-Path $report 'results.xml') -Raw
if([int]$junit.testsuite.tests -ne $expected.Count -or [int]$junit.testsuite.skipped -ne 0 -or [int]$junit.testsuite.failures -ne 0){throw 'CI has missing, skipped or failed tests.'}
[ordered]@{passed=$true;profile=$manifest.profile;tests=$expected.Count;source=$manifest.source;registration_tested=$false;interactive_input_tested=$false;model_tested=$false} |
    ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $report 'result.json') -Encoding utf8
