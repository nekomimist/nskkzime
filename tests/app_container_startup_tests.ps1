#Requires -Version 7.0
param([Parameter(Mandatory)][string]$Executable)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false
$executablePath = (Resolve-Path -LiteralPath $Executable).Path
$directory = Split-Path -Parent $executablePath
# A stop helper can be called by an elevated installer. It must not create a
# private configuration/diagnostics directory just to signal its named event.
$absent = Join-Path $directory ('broker-stop-test-' + [Guid]::NewGuid().ToString('N'))
$start = [Diagnostics.ProcessStartInfo]::new((Join-Path $directory 'nskkzime_broker.exe'))
$start.UseShellExecute=$false; $start.CreateNoWindow=$true
$start.ArgumentList.Add('--stop'); $start.ArgumentList.Add('--config'); $start.ArgumentList.Add((Join-Path $absent 'settings.ini'))
$child=[Diagnostics.Process]::Start($start)
try {
    if (-not $child.WaitForExit(20000)) { $child.Kill(); $child.WaitForExit(); throw 'Broker stop helper timed out.' }
    if ($child.ExitCode -ne 0 -or (Test-Path -LiteralPath $absent)) { throw 'Broker stop wrote private files or failed.' }
} finally { $child.Dispose() }
Write-Output 'PASS broker stop with missing configuration creates no private files'
# A loaded module keeps the junction spelling, but the broker's process image
# uses the real path. Authentication must still accept this same executable.
$alias = Join-Path $directory ('container-alias-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Junction -Path $alias -Target $directory | Out-Null
try {
    & (Join-Path $alias (Split-Path -Leaf $executablePath)) --startup-scenario delayed-broker
    if ($LASTEXITCODE -ne 0) { throw 'Broker authentication failed through a directory junction.' }
} finally {
    $link = Get-Item -LiteralPath $alias
    if ($link.LinkType -cne 'Junction' -or $link.Target -cne $directory -or $link.Parent.FullName -cne $directory) {
        throw 'Unexpected test junction; retained it for inspection.'
    }
    # Remove only the junction, never recursively traverse its target.
    Remove-Item -LiteralPath $alias -Force
}
Write-Output 'PASS broker authentication through a directory junction'
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
                $peers = @($child | Where-Object phase -CEQ 'broker_peer')
                if ($timeouts.Count -ne 1 -or $timeouts[0].ready -or $probes.Count -ne 1 -or
                    $null -eq $probes[0].reason -or $null -eq $probes[0].win32_error -or
                    -not (Test-Path -LiteralPath (Join-Path $fixture.FullName 'diagnostics/broker.jsonl'))) {
                    throw "Missing transport diagnostics; retained $($fixture.FullName)."
                }
                # This scenario exposes the broker only to the other package;
                # the metadata probe must report that it could not connect.
                if ($peers.Count -ne 1 -or -not $peers[0].PSObject.Properties['error'] -or
                    [string]::IsNullOrEmpty($peers[0].error) -or -not $peers[0].PSObject.Properties['win32_error']) {
                    throw "Missing peer-probe failure diagnostics; retained $($fixture.FullName)."
                }
            }
        }
    }
    Remove-Item -LiteralPath $fixture.FullName -Recurse -Force
    Write-Output "PASS $scenario diagnostics and owned-process cleanup"
}
