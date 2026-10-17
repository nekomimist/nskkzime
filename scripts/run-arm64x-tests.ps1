#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Name = '.*',
    [string]$ReportName = ((Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,8))
)
$ErrorActionPreference = 'Stop'
$manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'test-images.json') -Raw | ConvertFrom-Json -AsHashtable
if ($manifest.format -cne 'nskkzime-arm64x-tests-v1') { throw 'Invalid ARM64X test manifest.' }
foreach ($image in $manifest.images) {
    if ($image.path -cnotmatch '^build/Release/(x86/)?[A-Za-z0-9_-]+\.(exe|dll)$') { throw 'Invalid test image path.' }
    if ((Get-FileHash -LiteralPath (Join-Path $PSScriptRoot $image.path) -Algorithm SHA256).Hash.ToLowerInvariant() -cne $image.sha256) {
        throw "Test image changed: $($image.path)"
    }
}
$model = Join-Path $PSScriptRoot 'source/out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf'
if ((Get-FileHash -LiteralPath $model -Algorithm SHA256).Hash.ToLowerInvariant() -cne $manifest.model_sha256) { throw 'Test model changed.' }
Write-Host "Verified $($manifest.images.Count) test images; ARM64X DLL $($manifest.arm64x_dll_sha256)."
& (Join-Path $PSScriptRoot 'run-arm64-tests.ps1') -Name $Name -ReportName $ReportName
$result = $LASTEXITCODE
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'test-images.json') -Destination (Join-Path $PSScriptRoot "reports/$ReportName/test-images.json")
exit $result
