#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$product = Join-Path $repository 'out/arm64x/Release'
$manifest = Get-Content -LiteralPath (Join-Path $product 'arm64x-build.json') -Raw | ConvertFrom-Json -AsHashtable
Import-Module (Join-Path $PSScriptRoot 'source-fingerprint.psm1') -Force
$source = Get-NskkzimeSourceFingerprint $repository
if ($manifest.format -cne 'nskkzime-arm64x-build-v1' -or $manifest.source.Revision -cne $source.Revision -or
    $manifest.source.SHA256 -cne $source.SHA256 -or $manifest.source.Modified -ne $source.Modified) {
    throw 'Rebuild with scripts/build-arm64x.ps1 before preparing the test bundle.'
}
if ((Get-FileHash -LiteralPath (Join-Path $product 'nskkzime_tsf.dll') -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifest.dll_sha256) {
    throw 'ARM64X DLL differs from its build manifest.'
}
$destination = [IO.Path]::GetFullPath($OutputDirectory)
$null = & (Join-Path $PSScriptRoot 'prepare-arm64-tests.ps1') `
    -BuildDirectory (Join-Path $repository 'out/build/windows-arm64-upstream-experimental') -OutputDirectory $destination
$release = Join-Path $destination 'build/Release'
Copy-Item -LiteralPath (Join-Path $product 'nskkzime_tsf.dll') -Destination $release
Copy-Item -LiteralPath (Join-Path $product 'arm64x-build.json') -Destination $destination
foreach ($script in @('run-arm64x-tests.ps1','run-arm64x-registration.ps1','run-arm64x-input-probes.ps1',
        'arm64-registration-test.psm1','installation.psm1')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot $script) -Destination $destination
}
$template = [IO.File]::ReadAllText((Join-Path $destination 'build/CTestTestfile.cmake.in'))
foreach ($architecture in @('arm64','arm64ec','x64','x86')) {
    $dll = '@BUNDLE@/build/Release/nskkzime_tsf.dll'
    $prefix = 'build/Release'
    if ($architecture -eq 'x86') {
        $prefix += '/x86'; $dll = '@BUNDLE@/' + $prefix + '/nskkzime_tsf.dll'
        New-Item -ItemType Directory -Path (Join-Path $destination $prefix) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $product 'x86/nskkzime_tsf.dll') -Destination (Join-Path $destination $prefix)
    }
    foreach ($target in @('tsf_architecture_tests','tsf_smoke','tsf_learning','tsf_zenz','ime_probe','ime_probe_local')) {
        $filename = if ($architecture -eq 'x86') { "$target.exe" } else { "$target-$architecture.exe" }
        $relative = $prefix + '/' + $filename
        $sourceFile = if ($architecture -eq 'x86') { Join-Path $product ('x86/' + $filename) } else { Join-Path $product $filename }
        Copy-Item -LiteralPath $sourceFile -Destination (Join-Path $destination $relative)
        if ($target -in @('ime_probe','ime_probe_local')) { continue } # Interactive/registered checks run separately.
        $name = 'arm64x_' + $architecture + '_' + $target
        $arguments = @(('@BUNDLE@/' + $relative), $dll)
        if ($target -in @('tsf_smoke','tsf_learning')) { $arguments += '@BUNDLE@/source/tests/fixtures/basic.utf8.skk' }
        if ($target -eq 'tsf_zenz') {
            $arguments += '@BUNDLE@/build/Release/nkkz-ime-server.exe'
            $arguments += '@BUNDLE@/source/out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf'
        }
        $quoted = ($arguments | ForEach-Object { '"' + $_ + '"' }) -join ' '
        $template += "`nadd_test([=[$name]=] $quoted)`n"
        $template += "set_tests_properties([=[$name]=] PROPERTIES TIMEOUT 60 ENVIRONMENT_MODIFICATION `"NSKKZIME_CONFIG=set:@BUNDLE@/build/no-test-settings.ini`")`n"
        if ($target -eq 'tsf_architecture_tests') {
            $template += "set_tests_properties([=[$name]=] PROPERTIES PASS_REGULAR_EXPRESSION [=[`"host`":`"$architecture`"]=])`n"
        }
    }
}
[IO.File]::WriteAllText((Join-Path $destination 'build/CTestTestfile.cmake.in'), $template, [Text.UTF8Encoding]::new($false))
$images = @()
foreach ($file in Get-ChildItem -LiteralPath $release -Recurse -File | Where-Object Extension -in @('.exe','.dll')) {
    $relative = [IO.Path]::GetRelativePath($destination, $file.FullName).Replace('\','/')
    $bytes = [IO.File]::ReadAllBytes($file.FullName)
    if ($bytes.Length -lt 64) { throw "Invalid image: $relative" }
    $pe = [BitConverter]::ToUInt32($bytes, 60)
    if ($pe -gt $bytes.Length - 26 -or [BitConverter]::ToUInt32($bytes, $pe) -ne 0x4550) { throw "Invalid PE: $relative" }
    $machine = [BitConverter]::ToUInt16($bytes, $pe + 4)
    $expected = if ($relative.Contains('/x86/')) { 0x14c } elseif ($file.BaseName -match '-(x64|arm64ec)$') { 0x8664 } else { 0xaa64 }
    if ($machine -ne $expected) { throw "Unexpected PE machine: $relative" }
    $images += [ordered]@{path=$relative;machine=$machine;sha256=(Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()}
}
[ordered]@{format='nskkzime-arm64x-tests-v1';source=$source;arm64x_dll_sha256=$manifest.dll_sha256;
    model_sha256='29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673';images=$images} |
    ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $destination 'test-images.json') -Encoding utf8
[pscustomobject]@{Path=$destination;NativeTests=50;ArchitectureTests=16;Images=$images.Count}
