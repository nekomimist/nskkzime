#Requires -Version 5.1
param(
    [Parameter(Mandatory)][string]$ScoopScript,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [string]$NativeSetup
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('scoop-test space-' + [Guid]::NewGuid().ToString('N'))
$directory = Join-Path $fixture ('package ' + [char]0x65e5 + [char]0x672c + [char]0x8a9e)
[IO.Directory]::CreateDirectory((Join-Path $directory 'bin')) | Out-Null
$installer = Join-Path $directory 'bin/nskkzime_setup.exe'
[IO.File]::WriteAllText($installer, 'Fixture: must never execute.')
@{format_version=8;architecture='x64';variant='zenz-cpu'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'manifest.json')
$state = @{ Calls = 0; ExitCode = 0; FailStart = $false; Disposed = $false }
function Assert-Scoop([bool]$Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Expect-Failure([scriptblock]$Action, [string]$Expected) {
    $message = $null
    try { & $Action } catch { $message = $_.Exception.Message }
    Assert-Scoop ($null -ne $message -and $message.Contains($Expected)) "Expected '$Expected', got '$message'."
}
# Exercise the real wrapper while replacing only process creation. No UAC or
# real registration is performed, and the fake executable is never launched.
function Start-Process {
    param($FilePath, $ArgumentList, $Wait, $PassThru, $WindowStyle, $Verb)
    $state.Calls++
    $state.Arguments = $ArgumentList
    $state.FilePath = $FilePath
    $state.Wait = $Wait
    $state.PassThru = $PassThru
    $state.WindowStyle = $WindowStyle
    $state.Verb = $Verb
    if ($state.FailStart) { throw 'UAC cancelled fixture' }
    $child = [pscustomobject]@{ ExitCode = $state.ExitCode }
    $child | Add-Member -MemberType ScriptMethod -Name Dispose -Value { $state.Disposed = $true }
    return $child
}
try {
    & $ScoopScript -Action Install -PackageDirectory ($directory + '\')
    Assert-Scoop ($state.Calls -eq 1 -and $state.Disposed -and $state.Wait -and $state.PassThru -and
        $state.WindowStyle -ceq 'Normal' -and $state.FilePath -ceq $installer) 'Wrapper did not wait for native setup.'
    Assert-Scoop (($state.Arguments -join '|') -ceq (@('--action', 'Install', '--package-directory', ('"' + $directory + '"')) -join '|')) 'Spaces, Unicode paths or action were lost.'
    Assert-Scoop (-not $state.Verb) 'Scoop must not elevate the scripting host.'
    & $ScoopScript -Action Unregister -PackageDirectory $directory
    Assert-Scoop ($state.Arguments[1] -ceq 'Unregister') 'Unregister action was lost.'
    $state.ExitCode = 17
    Expect-Failure { & $ScoopScript -Action Install -PackageDirectory $directory } 'exit 17'
    $state.FailStart = $true
    Expect-Failure { & $ScoopScript -Action Install -PackageDirectory $directory } 'UAC cancelled fixture'
    $calls = $state.Calls
    Expect-Failure { & $ScoopScript -Action Install -PackageDirectory (Join-Path $fixture 'missing') } 'Installer not found'
    Assert-Scoop ($state.Calls -eq $calls) 'Missing package attempted to elevate.'
    @{format_version=7;architecture='arm64';variant='zenz-cpu';channel='experimental'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'manifest.json')
    Expect-Failure { & $ScoopScript -Action Install -PackageDirectory $directory } 'Scoop requires an x64 release package'
    Assert-Scoop ($state.Calls -eq $calls) 'Experimental ARM64 package attempted to elevate.'
    if ($NativeSetup) {
        # The real entry point rejects a different user before any OS writes.
        $errorText = (& $NativeSetup --action Unregister --expected-user-sid S-1-0-0 2>&1 | Out-String)
        Assert-Scoop ($LASTEXITCODE -ne 0 -and $errorText.Contains('Elevation changed the user')) 'Different-user native handoff was accepted.'
    }
    Write-Output 'PASS Scoop native launch, no elevated scripting host, Unicode arguments and failure propagation'
} finally {
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
