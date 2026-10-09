#requires -Version 7.0
param(
    [string]$Server = "$PSScriptRoot/../out/build/windows-x64-zenz/Release/nkkz-ime-server.exe",
    [string]$Model = "$PSScriptRoot/../out/models/zenz-v3.2-small/ggml-model-Q5_K_M.gguf",
    [ValidateRange(1,64)][int]$Threads = 4,
    [ValidateRange(1,1000)][int]$Rounds = 30,
    [string]$Output
)
$ErrorActionPreference = 'Stop'
$Server = (Resolve-Path -LiteralPath $Server).Path
$Model = (Resolve-Path -LiteralPath $Model).Path
# These fixed public examples are smoke checks, not a representative quality corpus.
$fixtures = @(
    @{ kana='かいとう'; left='試験問題の'; expected='解答'; candidates=@('回答','解答','解凍') },
    @{ kana='かいとう'; left='冷凍食品を電子レンジで'; expected='解凍'; candidates=@('回答','解答','解凍') },
    @{ kana='こうこうのきょうし'; left=''; expected='高校の教師'; candidates=@('高校の教師','高校の教示','孝行の教師') }
)
$start = [Diagnostics.ProcessStartInfo]::new()
$start.FileName = $Server
$start.UseShellExecute = $false
$start.CreateNoWindow = $true
$start.RedirectStandardInput = $start.RedirectStandardOutput = $start.RedirectStandardError = $true
$start.StandardInputEncoding = $start.StandardOutputEncoding = $start.StandardErrorEncoding = [Text.UTF8Encoding]::new($false)
foreach ($option in @('--model',$Model,'--stdio','--threads',"$Threads",'--timeout-ms','1000')) { $start.ArgumentList.Add($option) }
$timer = [Diagnostics.Stopwatch]::StartNew()
$process = [Diagnostics.Process]::Start($start)
$stderr = $process.StandardError.ReadToEndAsync()
function Read-Response {
    $line = $process.StandardOutput.ReadLineAsync()
    if (-not $line.Wait(15000)) { throw 'Server response timed out.' }
    $value = $line.GetAwaiter().GetResult()
    if ($null -eq $value) { throw 'Server closed stdout.' }
    return $value | ConvertFrom-Json
}
function Percentile($Values, [double]$Quantile) {
    $sorted = @($Values | Sort-Object)
    return [Math]::Round($sorted[[Math]::Max(0,[int][Math]::Ceiling($sorted.Count*$Quantile)-1)],3)
}
try {
    $hello = Read-Response
    $startupMs = $timer.Elapsed.TotalMilliseconds
    if ($hello.backend -ne 'zenz') { throw 'Expected the zenz backend.' }
    $samples = [Collections.Generic.List[object]]::new()
    $requestId = 0
    # Round zero warms each operation/context; discard it from measured samples.
    for ($round=0; $round -le $Rounds; ++$round) {
        foreach ($fixture in $fixtures) {
            foreach ($operation in @('convert','score')) {
                $request = @{ id=++$requestId; op=$operation; kana=$fixture.kana; left=$fixture.left; right='' }
                if ($operation -eq 'convert') { $request.n=5 } else { $request.candidates=$fixture.candidates }
                $json = $request | ConvertTo-Json -Compress
                $timer.Restart()
                $process.StandardInput.WriteLine($json)
                $process.StandardInput.Flush()
                $response = Read-Response
                $elapsed = $timer.Elapsed.TotalMilliseconds
                if ($response.error -or $response.id -ne $requestId) { throw 'Invalid response or model timeout.' }
                if ($operation -eq 'convert') { $correct = $response.candidates[0] -ceq $fixture.expected }
                else {
                    if ($response.scores.Count -ne $fixture.candidates.Count) { throw 'Score count mismatch.' }
                    $best = 0
                    for ($i=1; $i -lt $response.scores.Count; ++$i) { if ($response.scores[$i] -gt $response.scores[$best]) { $best=$i } }
                    $correct = $fixture.candidates[$best] -ceq $fixture.expected
                }
                if ($round -gt 0) { $samples.Add(@{ operation=$operation; ms=$elapsed; correct=$correct }) }
            }
        }
    }
    $metrics = @{}
    foreach ($operation in @('convert','score')) {
        $group = @($samples | Where-Object operation -EQ $operation)
        $metrics[$operation] = @{ samples=$group.Count; p50_ms=(Percentile $group.ms 0.5); p95_ms=(Percentile $group.ms 0.95);
            max_ms=(Percentile $group.ms 1); top1_correct=@($group | Where-Object correct).Count }
    }
    $process.StandardInput.Close()
    if (-not $process.WaitForExit(10000) -or $process.ExitCode -ne 0) { throw 'Server did not exit cleanly.' }
    $result = [ordered]@{
        timestamp=[DateTimeOffset]::Now.ToString('o'); transport='stdio'; configuration='Release (recommended)';
        model_sha256=(Get-FileHash -LiteralPath $Model -Algorithm SHA256).Hash.ToLowerInvariant();
        cpu=$env:PROCESSOR_IDENTIFIER; logical_processors=[Environment]::ProcessorCount; threads=$Threads;
        startup_ms=[Math]::Round($startupMs,3); fixtures=$fixtures.Count; rounds=$Rounds; metrics=$metrics;
        notes='Process startup/model load is measured with the current OS file cache; this is not a cold disk test. Request times include PowerShell/JSON/stdio overhead. Fixed smoke examples do not measure general conversion quality.'
    } | ConvertTo-Json -Depth 6
    if ($Output) {
        $outputPath = [IO.Path]::GetFullPath($Output)
        [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($outputPath)) | Out-Null
        [IO.File]::WriteAllText($outputPath,$result,[Text.UTF8Encoding]::new($false))
    }
    $result
} finally {
    if (-not $process.HasExited) { $process.Kill(); $process.WaitForExit() }
    $process.Dispose()
}
