#requires -Version 7.0
param(
    [string]$BuildDirectory = "$PSScriptRoot/../out/build/windows-x64-zenz/Release",
    [string]$Model = "$PSScriptRoot/../out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf"
)
$ErrorActionPreference = 'Stop'
$BuildDirectory = (Resolve-Path -LiteralPath $BuildDirectory).Path
$Model = (Resolve-Path -LiteralPath $Model).Path
$server = Join-Path $BuildDirectory 'nkkz-ime-server.exe'
$config = Join-Path $BuildDirectory 'nskkzime_config.exe'
$probe = Join-Path $BuildDirectory 'ime_probe_local.exe'
foreach ($file in @($server,$config,$probe)) { if (-not (Test-Path -LiteralPath $file -PathType Leaf)) { throw "Missing build file: $file" } }
$version = & $server --version | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or $version.backend -ne 'zenz') { throw 'Build the real server with scripts/build.ps1 -Configuration Release -Zenz first.' }
$settings = Join-Path $BuildDirectory ("probe-zenz-$([Guid]::NewGuid().ToString('N')).ini")
try {
    & $config --config $settings --set-zenz $Model $server
    if ($LASTEXITCODE -ne 0) { throw 'Cannot configure zenz probe.' }
    & $config --config $settings --set-learning off
    if ($LASTEXITCODE -ne 0) { throw 'Cannot isolate probe learning.' }
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $probe
    $start.UseShellExecute = $false
    $start.Environment['NSKKZIME_CONFIG'] = $settings
    foreach ($name in @('NSKKZIME_DICTIONARY','NSKKZIME_INFERENCE_DUMMY','NSKKZIME_INFERENCE_TOKEN','NSKKZIME_INFERENCE_DELAY_MS','NSKKZIME_INFERENCE_TIMEOUT_MS')) {
        $null = $start.Environment.Remove($name)
    }
    $start.Environment['NSKKZIME_LEARNING'] = '0'
    $start.Environment['NSKKZIME_ROMAJI'] = 'builtin'
    Write-Host 'Select nskkzime in the probe, then try Koukounokyoushi + Space. Close the probe to finish.'
    $process = [Diagnostics.Process]::Start($start)
    try { $process.WaitForExit(); if ($process.ExitCode -ne 0) { throw "Probe exited with $($process.ExitCode)." } }
    finally { $process.Dispose() }
} finally {
    # Only this invocation's unique configuration; never the user's settings.
    if (Test-Path -LiteralPath $settings) { Remove-Item -LiteralPath $settings }
}
