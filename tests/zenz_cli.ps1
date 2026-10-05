#requires -Version 7.0
param([string]$Server, [string]$Model)
$ErrorActionPreference = 'Stop'
if (-not (Test-Path -LiteralPath $Model)) { exit 77 }
function Run-Server([string[]]$Options, [string]$InputText = '', [int]$Expected = 0) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Server
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.RedirectStandardInput = $start.RedirectStandardOutput = $start.RedirectStandardError = $true
    $start.StandardInputEncoding = $start.StandardOutputEncoding = $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
    foreach ($option in $Options) { $start.ArgumentList.Add($option) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        $process.StandardInput.Write($InputText)
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(30000)) { $process.Kill(); throw 'Server command timed out.' }
        if ($process.ExitCode -ne $Expected) { throw "Unexpected exit $($process.ExitCode): $($stderr.Result)" }
        return $stdout.Result
    } finally { $process.Dispose() }
}
$version = (Run-Server @('--version')) | ConvertFrom-Json
if ($version.backend -ne 'zenz') { throw 'Wrong backend.' }
$response = (Run-Server @('--model',$Model,'--convert','かいとう','--left','試験問題の','-n','3')) | ConvertFrom-Json
if ($response.candidates[0] -ne '解答' -or $response.scores.Count -ne $response.candidates.Count) { throw 'CLI conversion failed.' }
$unicodeModel = Join-Path (Split-Path -Parent $Server) ("モデル 検証-$([Guid]::NewGuid().ToString('N')).gguf")
try {
    Copy-Item -LiteralPath $Model -Destination $unicodeModel
    $unicode = (Run-Server @('--model',$unicodeModel,'--convert','こうこうのきょうし')) | ConvertFrom-Json
    if ($unicode.candidates[0] -ne '高校の教師') { throw 'Unicode model path failed.' }
} finally { if (Test-Path -LiteralPath $unicodeModel) { Remove-Item -LiteralPath $unicodeModel } }
$scored = (Run-Server @('--model',$Model,'--convert','かいとう','--left','冷凍食品を電子レンジで','--score','回答','--score','解答','--score','解凍')) | ConvertFrom-Json
if ($scored.scores.Count -ne 3 -or $scored.scores[2] -le $scored.scores[1]) { throw 'CLI scoring failed.' }
$requests = @(
    '{"id":1,"op":"convert","kana":"こうこうのきょうし","left":"","right":"","n":1}',
    '{"id":2,"op":"score","kana":"かいとう","left":"試験問題の","right":"","candidates":["回答","解答","解凍",""]}',
    '{"id":3,"id":4,"op":"convert"}',
    ('x' * 16385),
    '{"id":5,"op":"convert","kana":"かいとう","left":"冷凍食品を電子レンジで","right":"","n":1}',
    '{"id":6,"op":"score","kana":"あ","left":"","right":"","candidates":[]}'
)
$output = Run-Server @('--model',$Model,'--stdio') (($requests -join "`n") + "`n")
$lines = @($output -split "`r?`n" | Where-Object { $_ -ne '' } | ForEach-Object { $_ | ConvertFrom-Json })
if ($lines.Count -ne 7 -or $lines[0].hello -ne 'nkkz-ime-server' -or $lines[1].candidates[0] -ne '高校の教師' -or
    $lines[2].scores[1] -le $lines[2].scores[0] -or -not $lines[3].error -or -not $lines[4].error -or
    $lines[5].candidates[0] -ne '解凍' -or $lines[6].scores.Count -ne 0) { throw 'JSON Lines framing or recovery failed.' }
foreach ($options in @(
    @('--model',$Model,'--threads','0'), @('--model',$Model,'--convert','あ','-n','9'),
    @('--model',$Model,'--stdio','--pipe-token','conflict'), @('--model',$Model,'--score','alone'),
    @('--dummy','--model',$Model,'--pipe-token','conflict'), @('--model',$Model,'--threads','4','--threads','4')
)) { $null = Run-Server $options '' 2 }
Write-Host 'PASS real-model CLI, UTF-8 JSON Lines, score order, bounded framing, malformed-request recovery and options'
