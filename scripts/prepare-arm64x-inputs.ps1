#requires -Version 7.0
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$NativeDirectory,
    [string]$EcDirectory,
    [switch]$Verify
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$native = (Resolve-Path -LiteralPath $NativeDirectory).Path
$output = Join-Path $native 'arm64x/Release'
$manifestFile = Join-Path $output 'native-inputs.json'
$includeFile = Join-Path $output 'native-inputs.cmake'
Import-Module (Join-Path $PSScriptRoot 'source-fingerprint.psm1') -Force
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Write-Changed([string]$Path, [string]$Text) {
    if (-not (Test-Path -LiteralPath $Path) -or [IO.File]::ReadAllText($Path) -cne $Text) {
        [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
    }
}
function Native-Machine([byte[]]$Bytes, [int]$Offset, [int]$Length, [bool]$AllowEc = $false) {
    if ($Length -lt 20) { throw 'Truncated COFF input.' }
    $machine = [BitConverter]::ToUInt16($Bytes, $Offset)
    if ($machine -eq 0 -and [BitConverter]::ToUInt16($Bytes, $Offset + 2) -eq 0xffff) {
        $machine = [BitConverter]::ToUInt16($Bytes, $Offset + 6) # BigObj or import header.
    }
    if ($machine -ne 0xaa64 -and -not ($AllowEc -and $machine -eq 0xa641)) { throw ('Non-ARM64 COFF input: 0x{0:x4}' -f $machine) }
    $machine
}
function Check-NativeInput([string]$Path, [string]$Kind) {
    if ($Kind -notin @('obj','lib')) { return }
    $bytes = [IO.File]::ReadAllBytes($Path)
    if ($Kind -eq 'obj') { $null = Native-Machine $bytes 0 $bytes.Length; return }
    if ($bytes.Length -lt 8 -or [Text.Encoding]::ASCII.GetString($bytes, 0, 8) -cne "!<arch>`n") { throw "Invalid COFF archive: $Path" }
    # SDK 26100's arm64 import archives include both native/EC members and an
    # EC symbol index. Project static libraries must remain entirely native.
    $allowEc = @($hybridRoots | Where-Object { $Path.StartsWith($_ + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase) }).Count -ne 0
    $offset = 8; $members = 0; $nativeMembers = 0
    while ($offset -lt $bytes.Length) {
        if ($offset -gt $bytes.Length - 60 -or $bytes[$offset + 58] -ne 0x60 -or $bytes[$offset + 59] -ne 0x0a) { throw "Invalid archive member: $Path" }
        $name = [Text.Encoding]::ASCII.GetString($bytes, $offset, 16).Trim()
        $sizeText = [Text.Encoding]::ASCII.GetString($bytes, $offset + 48, 10).Trim()
        if ($sizeText -notmatch '^\d{1,10}$') { throw 'Invalid archive member length.' }
        $size = [long]::Parse($sizeText)
        $offset += 60
        if ($size -gt $bytes.Length - $offset) { throw 'Truncated archive member.' }
        if ($name -notin @('/','//','/<ECSYMBOLS>/')) {
            $machine = Native-Machine $bytes $offset ([int]$size) $allowEc
            if ($machine -eq 0xaa64) { $nativeMembers++ }
            $members++
        }
        $offset += [int]($size + $size % 2)
    }
    if (-not $members -or -not $nativeMembers) { throw "No native ARM64 COFF objects in $Path" }
}
$source = Get-NskkzimeSourceFingerprint $repository
$config = Get-Content -LiteralPath (Join-Path $output 'build-config.json') -Raw | ConvertFrom-Json -AsHashtable
$sdkLib = Join-Path ${env:ProgramFiles(x86)} ('Windows Kits/10/lib/' + $config.CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
$hybridRoots = @((Join-Path $sdkLib 'um/arm64')) |
    ForEach-Object { [IO.Path]::GetFullPath($_) }
if ($config.CMAKE_GENERATOR_PLATFORM -cne 'ARM64' -or $config.configuration -cne 'Release' -or
    $config.CMAKE_MSVC_RUNTIME_LIBRARY -cne 'MultiThreadedDLL' -or $config.scope -cne 'tsf-only') { throw 'Invalid native ARM64X build configuration.' }
if ($Verify) {
    if (-not (Test-Path -LiteralPath $manifestFile)) { throw 'Run scripts/build-arm64x.ps1 to prepare native inputs.' }
    $manifest = Get-Content -LiteralPath $manifestFile -Raw | ConvertFrom-Json -AsHashtable
    if ($manifest.format -cne 'nskkzime-arm64x-inputs-v1' -or $manifest.source.Revision -cne $source.Revision -or
        $manifest.source.SHA256 -cne $source.SHA256 -or $manifest.source.Modified -ne $source.Modified -or
        $manifest.native_cache_sha256 -cne (Hash (Join-Path $native 'CMakeCache.txt')) -or
        $manifest.native_rsp_sha256 -cne (Hash (Join-Path $output 'native-link.rsp')) -or
        $manifest.include_sha256 -cne (Hash $includeFile) -or
        $manifest.config_sha256 -cne (Hash (Join-Path $output 'build-config.json')) -or
        $manifest.native_dll_sha256 -cne (Hash (Join-Path $native 'Release/nskkzime_tsf.dll'))) {
        throw 'Stale ARM64X native build. Run scripts/build-arm64x.ps1 again.'
    }
    foreach ($inputFile in $manifest.inputs) {
        if ($inputFile.sha256 -cne (Hash $inputFile.path)) { throw "Changed ARM64X input: $($inputFile.path). Run scripts/build-arm64x.ps1." }
    }
    if ($EcDirectory) {
        $ec = Get-Content -LiteralPath (Join-Path $EcDirectory 'arm64x/Release/build-config.json') -Raw | ConvertFrom-Json -AsHashtable
        if ($ec.CMAKE_GENERATOR_PLATFORM -cne 'ARM64EC') { throw 'Expected an ARM64EC merge configuration.' }
        foreach ($key in $config.Keys | Where-Object { $_ -ne 'CMAKE_GENERATOR_PLATFORM' }) {
            $nativeValue = $config[$key]; $ecValue = $ec[$key]
            if ($key -eq 'CMAKE_CXX_FLAGS') {
                # CMake's Windows ARM64EC platform adds these two ABI header
                # definitions. Other user/compiler flags must still match.
                $ecValue = [regex]::Replace($ecValue, '(?<!\S)/D(_AMD64_|_ARM64EC_)(?!\S)', '')
                $ecValue = ($ecValue -split '\s+' | Where-Object { $_ }) -join ' '
                $nativeValue = ($nativeValue -split '\s+' | Where-Object { $_ }) -join ' '
            }
            if ($nativeValue -cne $ecValue) { throw "ARM64X native/EC configuration mismatch: $key" }
        }
    }
    Write-Host 'ARM64X native source, configuration and linker inputs verified.'
    exit 0
}
# /LINKREPROFULLPATHRSP from the supported MSVC version contains one quoted
# absolute input per line. Accept only that observed format, never a shell or
# a general-purpose response file. Native resources are recorded but not merged.
$inputs = [Collections.Generic.List[object]]::new()
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
$objects = [Collections.Generic.List[string]]::new()
$libraries = [Collections.Generic.List[string]]::new()
$definition = $null
foreach ($line in Get-Content -LiteralPath (Join-Path $output 'native-link.rsp')) {
    if ($line -notmatch '^"([^"\r\n]+)"$') { throw "Unsupported native response line: $line" }
    $argument = $Matches[1]
    $kind = ''
    if ($argument.StartsWith('/def:', [StringComparison]::OrdinalIgnoreCase)) {
        if ($definition) { throw 'Multiple native DEF inputs.' }
        $argument = $argument.Substring(5); $kind = 'def'; $definition = $argument
    } else { $kind = [IO.Path]::GetExtension($argument).TrimStart('.').ToLowerInvariant() }
    if ($argument -notmatch '^[a-zA-Z]:[\\/]' -or $argument.IndexOfAny([char[]]'";$[]') -ge 0 -or $kind -notin @('obj','lib','res','def')) {
        throw "Unsupported native linker input: $line"
    }
    $path = (Resolve-Path -LiteralPath $argument).Path
    if (-not $seen.Add($path)) { continue }
    Check-NativeInput $path $kind
    $inputs.Add([ordered]@{path=$path;kind=$kind;sha256=(Hash $path)})
    if ($kind -eq 'obj') { $objects.Add($path.Replace('\','/')) }
    if ($kind -eq 'lib') { $libraries.Add($path.Replace('\','/')) }
}
if (-not $definition -or $objects.Count -lt 1 -or $libraries.Count -lt 1) { throw 'Incomplete native linker inputs.' }
# The compiled DLL's single native record must describe these exact sources.
$image = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $native 'Release/nskkzime_tsf.dll')))
$marker = '{"format_version":1,"source_revision":"'
$begin = $image.IndexOf($marker, [StringComparison]::Ordinal)
if ($begin -lt 0 -or $image.IndexOf($marker, $begin + $marker.Length, [StringComparison]::Ordinal) -ge 0) { throw 'Expected a single native build metadata record.' }
$end = $image.IndexOf([char]0, $begin)
if ($end -lt $begin -or $end - $begin -gt 3000) { throw 'Invalid native build metadata.' }
$identity = $image.Substring($begin, $end - $begin) | ConvertFrom-Json -AsHashtable
if ($identity.variant -cne 'arm64x-native' -or $identity.configuration -cne 'Release' -or
    $identity.source_revision -cne $source.Revision -or $identity.source_sha256 -cne $source.SHA256 -or
    $identity.source_modified -ne $source.Modified -or $identity.cmake_cache_sha256 -cne (Hash (Join-Path $native 'CMakeCache.txt'))) {
    throw 'Native DLL does not match the current sources. Rebuild with scripts/build-arm64x.ps1.'
}
$include = "# Generated from verified native linker inputs; do not edit.`n"
foreach ($entry in @(@{name='OBJECTS';values=$objects}, @{name='LIBRARIES';values=$libraries}, @{name='DEF';values=@($definition.Replace('\','/'))})) {
    $include += "set(NSKKZIME_ARM64X_NATIVE_$($entry.name)`n"
    foreach ($value in $entry.values) { $include += "  [=[$value]=]`n" }
    $include += ")`n"
}
Write-Changed $includeFile $include
$manifest = [ordered]@{format='nskkzime-arm64x-inputs-v1';source=$source;native_configuration=$config;
    native_cache_sha256=(Hash (Join-Path $native 'CMakeCache.txt'));native_rsp_sha256=(Hash (Join-Path $output 'native-link.rsp'));
    config_sha256=(Hash (Join-Path $output 'build-config.json'));native_dll_sha256=(Hash (Join-Path $native 'Release/nskkzime_tsf.dll'));
    include_sha256=(Hash $includeFile);native_metadata=$identity;inputs=$inputs.ToArray()}
Write-Changed $manifestFile ($manifest | ConvertTo-Json -Depth 8)
Write-Host "Prepared $($objects.Count) native objects and $($libraries.Count) native libraries for ARM64X."
