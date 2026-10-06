#Requires -Version 7.0
[CmdletBinding()]
param(
    [string]$Trace = (Join-Path $PSScriptRoot '../out/build/windows-x64-zenz/Release/skk_trace.exe'),
    [string]$Emacs,
    [string]$Ddskk = (Join-Path $PSScriptRoot '../out/reference/ddskk'),
    [switch]$CoreOnly,
    [string]$Cases = (Join-Path $PSScriptRoot '../tests/fixtures/ddskk-comparison.json'),
    [string]$OutputRoot = (Join-Path $PSScriptRoot '../out')
)
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$tracePath = (Resolve-Path -LiteralPath $Trace).Path
$fixture = Join-Path $repository 'tests/fixtures/ddskk-comparison.utf8.skk'
$specification = Get-Content -LiteralPath $Cases -Raw | ConvertFrom-Json
if (-not $specification.cases.Count) { throw 'Comparison requires at least one case.' }
$ids = [Collections.Generic.HashSet[string]]::new()
foreach ($case in $specification.cases) {
    if ($case.id -notmatch '^[a-z0-9-]+$' -or -not $ids.Add($case.id)) { throw 'Invalid or duplicate comparison case id.' }
    if ($case.nskkzime -cne $case.ddskk -and -not $case.difference) { throw "Document the expected difference for $($case.id)." }
}
$source = $null
if (-not $CoreOnly) {
    if (-not $Emacs) { throw 'Pass -Emacs with the GNU Emacs executable path, or use -CoreOnly.' }
    $emacsPath = (Resolve-Path -LiteralPath $Emacs).Path
    $source = (Resolve-Path -LiteralPath $Ddskk).Path
    $revision = & git -C $source rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $revision -ne $specification.ddskk_commit) { throw 'DDSKK revision differs from the comparison fixture.' }
    $dirty = & git -C $source status --porcelain --untracked-files=all
    if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Use an unmodified DDSKK checkout.' }
}
$runRoot = Join-Path ([IO.Path]::GetFullPath($OutputRoot)) ('ddskk-comparison-' + [Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($runRoot) | Out-Null

function Invoke-ComparisonProcess([string]$Executable, [string[]]$Arguments, [hashtable]$EnvironmentOverrides, [string]$Directory) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    $start.WorkingDirectory = $Directory
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.Encoding]::UTF8
    $start.StandardErrorEncoding = [Text.Encoding]::UTF8
    $start.Environment.Clear()
    foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) { $start.Environment[$entry.Key] = $entry.Value }
    foreach ($entry in $EnvironmentOverrides.GetEnumerator()) { $start.Environment[$entry.Key] = $entry.Value }
    foreach ($argument in $Arguments) { $start.ArgumentList.Add($argument) }
    $child = [Diagnostics.Process]::Start($start)
    try {
        $stdout = $child.StandardOutput.ReadToEndAsync()
        $stderr = $child.StandardError.ReadToEndAsync()
        if (-not $child.WaitForExit(20000)) {
            $child.Kill($true); $child.WaitForExit()
            throw 'Comparison child timed out.'
        }
        $text = $stdout.GetAwaiter().GetResult()
        $errors = $stderr.GetAwaiter().GetResult()
        if ($child.ExitCode -ne 0) { throw "Comparison child exited with $($child.ExitCode): $errors" }
        return $text
    } finally { $child.Dispose() }
}

$results = [Collections.Generic.List[object]]::new()
$failures = 0
foreach ($case in $specification.cases) {
    $caseRoot = Join-Path $runRoot $case.id
    [IO.Directory]::CreateDirectory($caseRoot) | Out-Null
    try {
        $output = Invoke-ComparisonProcess $tracePath @('--dict', $fixture, '--', $case.keys) @{} $caseRoot
        [IO.File]::WriteAllText((Join-Path $caseRoot 'trace.jsonl'), $output)
        $last = ($output.TrimEnd() -split "`n")[-1] | ConvertFrom-Json
        $actual = $last.document + $last.preedit
        $ddskkResult = $null
        if (-not $CoreOnly) {
            $resultPath = Join-Path $caseRoot 'ddskk.json'
            $variables = @{
                HOME = $caseRoot
                NSKKZIME_COMPARE_ROOT = $caseRoot
                NSKKZIME_DDSKK_SOURCE = $source
                NSKKZIME_COMPARE_AUTOLOADS = (Join-Path $runRoot 'skk-autoloads.el')
                NSKKZIME_COMPARE_DICT = $fixture
                NSKKZIME_COMPARE_KEYS = $case.keys
                NSKKZIME_COMPARE_RESULT = $resultPath
            }
            $null = Invoke-ComparisonProcess $emacsPath @('--batch', '-Q', '-L', $source, '-l', (Join-Path $repository 'tests/ddskk_driver.el')) $variables $caseRoot
            $ddskkResult = Get-Content -LiteralPath $resultPath -Raw | ConvertFrom-Json
        }
        $passed = $actual -ceq $case.nskkzime -and ($CoreOnly -or $ddskkResult.text -ceq $case.ddskk)
        if (-not $passed) { ++$failures }
        $results.Add([pscustomobject]@{ id=$case.id; keys=$case.keys; passed=$passed; nskkzime=$actual; ddskk=$ddskkResult; expected_nskkzime=$case.nskkzime; expected_ddskk=$case.ddskk; difference=$case.difference })
        Write-Output "$(if ($passed) { 'PASS' } else { 'FAIL' }) $($case.id)"
    } catch {
        ++$failures
        $results.Add([pscustomobject]@{ id=$case.id; passed=$false; error=$_.Exception.Message })
        Write-Output "FAIL $($case.id): $($_.Exception.Message)"
    }
}
$report = Join-Path $runRoot 'report.json'
[IO.File]::WriteAllText($report, ([pscustomobject]@{ ddskk_commit=$specification.ddskk_commit; core_only=[bool]$CoreOnly; results=$results.ToArray() } | ConvertTo-Json -Depth 8) + "`n")
Write-Output "Report: $report"
if ($failures) { throw "$failures comparison cases failed." }
