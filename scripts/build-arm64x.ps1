#requires -Version 7.0
[CmdletBinding()]
param([ValidateRange(1,64)][int]$Jobs = 4, [switch]$TraceLink)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vsRoot = & $vswhere -latest -products '*' -version '[17.11,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.ARM64 Microsoft.VisualStudio.Component.VC.Tools.ARM64EC -property installationPath
if (-not $vsRoot) { throw 'Visual Studio 2022 17.11+ with ARM64 and ARM64EC tools is required.' }
$cmake = Join-Path $vsRoot 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
$pwsh = (Get-Command pwsh -CommandType Application | Select-Object -First 1).Source
function Invoke-Tool([string]$Executable, [string[]]$Arguments) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable; $start.WorkingDirectory = $repository; $start.UseShellExecute = $false
    $start.Environment.Clear()
    foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) { $start.Environment[$entry.Key] = $entry.Value }
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try { $process.WaitForExit(); if ($process.ExitCode) { throw "$Executable failed with exit $($process.ExitCode)" } }
    finally { $process.Dispose() }
}
function Build-Preset([string]$Name) {
    $configure = @('--preset', "windows-$Name", "-DCMAKE_GENERATOR_INSTANCE=$vsRoot")
    $build = @('--build', '--preset', "release-$Name", '--parallel', "$Jobs")
    if ($Name -eq 'arm64x-ec') {
        $configure += '-DNSKKZIME_ARM64X_TRACE_LINK=' + $(if ($TraceLink) { 'ON' } else { 'OFF' })
        if ($TraceLink) { $build += '--verbose' }
    }
    Invoke-Tool $cmake $configure
    Invoke-Tool $cmake $build
}
# Keep native services separate from both TSF-only inputs. No registration or
# installation is performed; every output remains under the repository's out/.
Invoke-Tool $pwsh @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'build.ps1'),
    '-Architecture', 'ARM64', '-UpstreamExperimental', '-BuildOnly', '-Configuration', 'Release', '-Jobs', "$Jobs")
Build-Preset 'arm64x-native'
$native = Join-Path $repository 'out/build/windows-arm64x-native'
Invoke-Tool $pwsh @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'prepare-arm64x-inputs.ps1'), '-NativeDirectory', $native)
Build-Preset 'arm64x-ec'
Invoke-Tool $pwsh @('-NoProfile', '-File', (Join-Path $PSScriptRoot 'prepare-arm64x-inputs.ps1'),
    '-NativeDirectory', $native, '-EcDirectory', (Join-Path $repository 'out/build/windows-arm64x-ec'), '-Verify')
$output = Join-Path $repository 'out/arm64x/Release'
New-Item -ItemType Directory -Path $output -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $repository 'out/build/windows-arm64x-ec/Release/nskkzime_tsf.dll') -Destination $output
$services = Join-Path $repository 'out/build/windows-arm64-upstream-experimental/Release'
foreach ($name in @('nkkz-ime-server','nskkzime_broker','nskkzime_config','nskkzime_settings','nskkzime_dictionary_gui','nskkzime_user_dictionary')) {
    Copy-Item -LiteralPath (Join-Path $services "$name.exe") -Destination $output
}
Build-Preset 'arm64x-x64-host'
Build-Preset 'arm64x-x86-host'
$inputs = Get-Content -LiteralPath (Join-Path $native 'arm64x/Release/native-inputs.json') -Raw | ConvertFrom-Json -AsHashtable
$dll = Join-Path $output 'nskkzime_tsf.dll'
$image = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes($dll))
$marker = '{"format_version":1,"source_revision":"'; $identities = @(); $offset = 0
while (($offset = $image.IndexOf($marker, $offset, [StringComparison]::Ordinal)) -ge 0) {
    $end = $image.IndexOf([char]0, $offset)
    if ($end -lt $offset -or $end - $offset -gt 3000) { throw 'Invalid ARM64X metadata record.' }
    $identity = $image.Substring($offset, $end - $offset) | ConvertFrom-Json -AsHashtable
    if ($identity.source_revision -cne $inputs.source.Revision -or $identity.source_sha256 -cne $inputs.source.SHA256 -or
        $identity.configuration -cne 'Release' -or $identity.binary -cne 'nskkzime_tsf.dll') { throw 'ARM64X input identity mismatch.' }
    $identities += $identity; $offset = $end + 1
}
if ($identities.Count -ne 2 -or @($identities.variant | Sort-Object) -join ',' -cne 'arm64x-ec,arm64x-native') {
    throw 'Expected distinct ARM64X native and EC metadata records.'
}
$manifest = [ordered]@{format='nskkzime-arm64x-build-v1';source=$inputs.source;configuration='Release';
    dll_sha256=(Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant();
    native_inputs_sha256=(Get-FileHash -LiteralPath (Join-Path $native 'arm64x/Release/native-inputs.json') -Algorithm SHA256).Hash.ToLowerInvariant();
    metadata_views=$identities;registration_performed=$false}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $output 'arm64x-build.json') -Encoding utf8
Write-Host "ARM64X prototype and ARM64/ARM64EC/x64/x86 hosts: $output"
