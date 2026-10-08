#Requires -Version 7.0
param([Parameter(Mandatory)][string]$StageScript, [Parameter(Mandatory)][string]$TestRoot)
$ErrorActionPreference = 'Stop'
$testParent = [IO.Path]::GetFullPath($TestRoot)
$fixtureRoot = Join-Path $testParent ('stage-test 日本語 space-' + [Guid]::NewGuid().ToString('N'))
$source = Join-Path $fixtureRoot 'source'
$destination = Join-Path $fixtureRoot 'staged'
$names = @('nskkzime_tsf.dll', 'nskkzime_dictionary_server.exe', 'nskkzime_settings.exe', 'nskkzime_dictionary_gui.exe', 'nkkz-ime-server.exe', 'nskkzime_broker.exe')
$bundles = [Collections.Generic.List[string]]::new()
function Assert-Stage([bool]$Condition, [string]$Reason) { if (-not $Condition) { throw $Reason } }
function Expect-Failure([scriptblock]$Action) {
    $failed = $false
    try { & $Action | Out-Null } catch { $failed = $true }
    Assert-Stage $failed 'Expected staging to reject invalid input.'
}
try {
    [IO.Directory]::CreateDirectory($source) | Out-Null
    foreach ($name in $names) { [IO.File]::WriteAllText((Join-Path $source $name), "fixture-$name") }
    $first = & $StageScript -SourceDirectory $source -DestinationRoot $destination
    $bundles.Add($first.Directory)
    Assert-Stage ($first.Hashes.Count -eq 6) 'Server, broker or GUI binaries are missing from bundle.'
    foreach ($name in $names) {
        Assert-Stage ((Get-FileHash -LiteralPath (Join-Path $first.Directory $name)).Hash -eq $first.Hashes[$name]) 'Staged bytes differ.'
    }
    $again = & $StageScript -SourceDirectory $source -DestinationRoot $destination
    Assert-Stage ($again.Directory -eq $first.Directory) 'Identical files did not reuse their bundle.'
    $server = Join-Path $source 'nkkz-ime-server.exe'
    Remove-Item -LiteralPath $server
    Expect-Failure { & $StageScript -SourceDirectory $source -DestinationRoot $destination }
    [IO.File]::WriteAllText($server, 'updated server fixture')
    $serverChanged = & $StageScript -SourceDirectory $source -DestinationRoot $destination
    $bundles.Add($serverChanged.Directory)
    Assert-Stage ($serverChanged.Directory -ne $first.Directory) 'Server change did not change bundle identity.'
    Assert-Stage ([IO.File]::ReadAllText((Join-Path $first.Directory 'nkkz-ime-server.exe')) -eq 'fixture-nkkz-ime-server.exe') 'Old server was overwritten.'
    [IO.File]::WriteAllText($server, 'fixture-nkkz-ime-server.exe')
    $gui = Join-Path $source 'nskkzime_settings.exe'
    [IO.File]::WriteAllText($gui, 'updated GUI fixture')
    $second = & $StageScript -SourceDirectory $source -DestinationRoot $destination
    $bundles.Add($second.Directory)
    Assert-Stage ($second.Directory -ne $first.Directory) 'GUI change did not change bundle identity.'
    Assert-Stage ([IO.File]::ReadAllText((Join-Path $first.Directory 'nskkzime_settings.exe')) -eq 'fixture-nskkzime_settings.exe') 'Old installed bundle was overwritten.'
    Remove-Item -LiteralPath $gui
    Expect-Failure { & $StageScript -SourceDirectory $source -DestinationRoot $destination }
    [IO.File]::WriteAllText($gui, 'updated GUI fixture')
    [IO.File]::WriteAllText($second.Dll, 'corrupted fixture')
    Expect-Failure { & $StageScript -SourceDirectory $source -DestinationRoot $destination }
    Assert-Stage ([IO.File]::ReadAllText($second.Dll) -eq 'corrupted fixture') 'Mismatched installed binary was overwritten.'
    Write-Output 'PASS complete server/GUI bundle, Unicode paths, repeat staging, server/GUI version identity, missing files and existing-file preservation'
} finally {
    # Only known artifacts inside this test's unique directory; never recursively delete.
    $prefix = [IO.Path]::GetFullPath($fixtureRoot).TrimEnd('\') + '\'
    foreach ($folder in @($source) + $bundles.ToArray()) {
        $full = [IO.Path]::GetFullPath($folder)
        if (-not $full.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) { throw 'Cleanup path escaped fixture.' }
        foreach ($name in $names) {
            $file = Join-Path $full $name
            if (Test-Path -LiteralPath $file) { Remove-Item -LiteralPath $file }
        }
        if (Test-Path -LiteralPath $full) { Remove-Item -LiteralPath $full }
    }
    if (Test-Path -LiteralPath $destination) { Remove-Item -LiteralPath $destination }
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot }
}
