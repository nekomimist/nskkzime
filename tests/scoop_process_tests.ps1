#Requires -Version 5.1
param(
    [Parameter(Mandatory)][string]$ScoopScript,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [ValidateSet('Test', 'Wrapper', 'Setup', 'Broker')][string]$Mode = 'Test',
    [string]$Fixture
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$ScoopScript = [IO.Path]::GetFullPath($ScoopScript)
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$powerShell = Join-Path $PSHOME $(if ($PSVersionTable.PSEdition -eq 'Core') { 'pwsh.exe' } else { 'powershell.exe' })
function New-FixtureStart([string]$ChildMode) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $powerShell
    $start.Arguments = '-NoProfile -ExecutionPolicy Bypass -File "' + $PSCommandPath +
        '" -ScoopScript "' + $ScoopScript + '" -BuildDirectory "' + $BuildDirectory +
        '" -Mode ' + $ChildMode + ' -Fixture "' + $Fixture + '"'
    $start.UseShellExecute = $false
    $start.CreateNoWindow = $true
    return $start
}
if ($Mode -eq 'Broker') {
    [IO.File]::WriteAllText((Join-Path $Fixture 'broker.ready'), '')
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not (Test-Path -LiteralPath (Join-Path $Fixture 'stop')) -and $timer.Elapsed.TotalSeconds -lt 60) {
        Start-Sleep -Milliseconds 100
    }
    exit 0
}
if ($Mode -eq 'Setup') {
    $broker = [Diagnostics.Process]::Start((New-FixtureStart 'Broker'))
    try {
        [IO.File]::WriteAllText((Join-Path $Fixture 'broker.pid'), [string]$broker.Id)
        $timer = [Diagnostics.Stopwatch]::StartNew()
        while (-not (Test-Path -LiteralPath (Join-Path $Fixture 'broker.ready'))) {
            if ($broker.HasExited -or $timer.Elapsed.TotalSeconds -gt 10) { throw 'Fixture broker did not start.' }
            Start-Sleep -Milliseconds 100
        }
        [IO.File]::WriteAllText((Join-Path $Fixture 'setup.complete'), '')
    } finally { $broker.Dispose() }
    exit 0
}
if ($Mode -eq 'Wrapper') {
    # Substitute only the executable: the real Start-Process retains the
    # wrapper's waiting behavior and the fixture starts a real descendant.
    function Start-Process {
        param($FilePath, $ArgumentList, $Wait, $PassThru, $WindowStyle)
        $start = New-FixtureStart 'Setup'
        Microsoft.PowerShell.Management\Start-Process -FilePath $start.FileName -ArgumentList $start.Arguments `
            -Wait:([bool]$Wait) -PassThru:$PassThru -WindowStyle Hidden
    }
    & $ScoopScript -Action Install -PackageDirectory (Join-Path $Fixture 'package')
    exit 0
}
$Fixture = Join-Path $BuildDirectory ('scoop-process space-' + [Guid]::NewGuid().ToString('N'))
$package = Join-Path $Fixture 'package'
[IO.Directory]::CreateDirectory((Join-Path $package 'bin')) | Out-Null
[IO.File]::WriteAllText((Join-Path $package 'bin/nskkzime_setup.exe'), 'Fixture: must never execute.')
@{format_version=8;architecture='x64';variant='zenz-cpu'} | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $package 'manifest.json')
$wrapper = $null
$broker = $null
try {
    $wrapper = [Diagnostics.Process]::Start((New-FixtureStart 'Wrapper'))
    $finished = $wrapper.WaitForExit(20000)
    $brokerPidFile = Join-Path $Fixture 'broker.pid'
    if (Test-Path -LiteralPath $brokerPidFile) {
        $broker = [Diagnostics.Process]::GetProcessById([int](Get-Content -LiteralPath $brokerPidFile -Raw))
    }
    if (-not (Test-Path -LiteralPath (Join-Path $Fixture 'setup.complete'))) { throw 'Fixture setup did not complete.' }
    if ($null -eq $broker -or $broker.HasExited) { throw 'Fixture broker must remain running after setup.' }
    if (-not $finished) { throw 'Scoop waited for the background broker after setup completed.' }
    if ($wrapper.ExitCode -ne 0) { throw "Scoop wrapper failed (exit $($wrapper.ExitCode))." }
    Write-Output 'PASS Scoop returns after setup while its background broker remains running'
} finally {
    [IO.File]::WriteAllText((Join-Path $Fixture 'stop'), '')
    foreach ($child in @($broker, $wrapper)) {
        if ($null -eq $child) { continue }
        try {
            if (-not $child.WaitForExit(10000)) { $child.Kill(); $child.WaitForExit() }
        } finally { $child.Dispose() }
    }
    $absolute = [IO.Path]::GetFullPath($Fixture)
    $allowed = $BuildDirectory.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
