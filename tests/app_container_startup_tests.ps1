#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Executable)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$directory = Split-Path -Parent $executablePath
foreach ($scenario in @('child-failure', 'broker-failure', 'readiness-timeout')) {
    $before = @(Get-ChildItem -LiteralPath $directory -Directory -Filter 'container-test-*' | ForEach-Object FullName)
    $output = @(& $executablePath --startup-scenario $scenario 2>&1)
    $code = $LASTEXITCODE
    $fixtures = @(Get-ChildItem -LiteralPath $directory -Directory -Filter 'container-test-*' | Where-Object { $_.FullName -notin $before })
    if ($code -ne 1 -or $fixtures.Count -ne 1) {
        $output | Write-Output
        throw "Unexpected result for startup scenario $scenario (exit $code, fixtures $($fixtures.Count))."
    }
    $fixture = $fixtures[0]
    # Delete only the uniquely created fixture beside this test executable,
    # after its diagnostic format and all process cleanup have been verified.
    if ($fixture.Parent.FullName -cne $directory -or $fixture.Name -cnotmatch '^container-test-[a-f0-9]+$' -or
        ($fixture.Attributes -band [IO.FileAttributes]::ReparsePoint)) { throw 'Unexpected startup fixture path.' }
    $records = @(Get-Content -LiteralPath (Join-Path $fixture.FullName 'startup.jsonl') | ForEach-Object { $_ | ConvertFrom-Json })
    if (@($records | Where-Object format -CNE 'nskkzime-startup-test').Count) { throw 'Unexpected startup diagnostic format.' }
    $failures = @($records | Where-Object phase -CEQ 'failure')
    $cleanups = @($records | Where-Object phase -CEQ 'cleanup_completed')
    if ($failures.Count -ne 1 -or $cleanups.Count -ne 1 -or @($records | Where-Object phase -CEQ 'cleanup_failed').Count) {
        throw "Incomplete failure/cleanup diagnostics for $scenario; retained $($fixture.FullName)."
    }
    $failure = $failures[0]
    $cleanup = $cleanups[0]
    foreach ($process in @($cleanup.children.on, $cleanup.children.input, $cleanup.broker)) {
        if ($process.running) { throw "Owned process still running after $scenario; retained $($fixture.FullName)." }
        if ($process.created -and ($process.wait_error -ne 0 -or $process.exit_error -ne 0)) {
            throw "Could not verify owned-process cleanup after $scenario; retained $($fixture.FullName)."
        }
    }
    switch ($scenario) {
        'child-failure' {
            if ($failure.at_phase -cne 'choices_wait' -or $failure.children.on.exit_code -ne 1 -or $failure.broker.created) {
                throw "Child failure was misdiagnosed; retained $($fixture.FullName)."
            }
        }
        'broker-failure' {
            if ($failure.at_phase -cne 'broker_start' -or $failure.error -notmatch 'Win32 error 2' -or $failure.broker.created) {
                throw "Broker launch failure was misdiagnosed; retained $($fixture.FullName)."
            }
        }
        'readiness-timeout' {
            if ($failure.at_phase -cne 'children_wait' -or -not $failure.broker.running -or
                $failure.children.on.exit_code -ne 1 -or $failure.children.input.exit_code -ne 1) {
                throw "Readiness timeout was misdiagnosed; retained $($fixture.FullName)."
            }
            foreach ($name in @('startup-on.log', 'startup-input.log')) {
                $child = @(Get-Content -LiteralPath (Join-Path $fixture.FullName $name) |
                    Where-Object { $_.StartsWith('{') } | ForEach-Object { $_ | ConvertFrom-Json })
                $timeouts = @($child | Where-Object phase -CEQ 'dictionary_wait_failed')
                $probes = @($child | Where-Object phase -CEQ 'bootstrap_probe')
                if ($timeouts.Count -ne 1 -or $timeouts[0].ready -or $probes.Count -ne 1 -or
                    $null -eq $probes[0].reason -or $null -eq $probes[0].win32_error -or
                    -not (Test-Path -LiteralPath (Join-Path $fixture.FullName 'diagnostics/broker.jsonl'))) {
                    throw "Missing transport diagnostics; retained $($fixture.FullName)."
                }
            }
        }
    }
    Remove-Item -LiteralPath $fixture.FullName -Recurse -Force
    Write-Output "PASS $scenario diagnostics and owned-process cleanup"
}
