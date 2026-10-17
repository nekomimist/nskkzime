#Requires -Version 5.1
param(
    [Parameter(Mandatory)][string]$ScoopScript,
    [Parameter(Mandatory)][string]$BuildDirectory,
    [string]$InstallScript
)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$fixture = Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('scoop-test space-' + [Guid]::NewGuid().ToString('N'))
$directory = Join-Path $fixture ('package ' + [char]0x65e5 + [char]0x672c + [char]0x8a9e)
[IO.Directory]::CreateDirectory((Join-Path $directory 'tools')) | Out-Null
$installer = Join-Path $directory 'tools/install.ps1'
$shellPath = Join-Path $fixture 'PowerShell 7/pwsh.exe'
[IO.Directory]::CreateDirectory((Split-Path -Parent $shellPath)) | Out-Null
[IO.File]::WriteAllText($installer, '# Fixture: must never execute.')
@{format_version=6;architecture='x64';variant='zenz-cpu'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'manifest.json')
[IO.File]::WriteAllText($shellPath, 'Fixture: must never execute.')
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
    & $ScoopScript -Action Install -PackageDirectory $directory -PowerShellPath $shellPath
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    Assert-Scoop ($state.Calls -eq 1 -and $state.Disposed -and $state.Wait -and $state.PassThru -and
        $state.WindowStyle -ceq 'Hidden' -and $state.FilePath -ceq $shellPath) 'Wrapper did not wait for the hidden child.'
    Assert-Scoop (($state.Arguments -join '|') -ceq (@('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', ('"' + $installer + '"'),
        '-Action', 'Install', '-PackageDirectory', ('"' + $directory + '"'), '-ExpectedUserSid', $identity.User.Value) -join '|')) 'Spaces, Unicode paths, action or user SID were lost.'
    Assert-Scoop ($principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) -or $state.Verb -ceq 'RunAs') 'Unelevated install did not request UAC.'
    & $ScoopScript -Action Unregister -PackageDirectory $directory -PowerShellPath $shellPath
    Assert-Scoop ($state.Arguments[7] -ceq 'Unregister') 'Unregister action was lost.'
    $state.ExitCode = 17
    Expect-Failure { & $ScoopScript -Action Install -PackageDirectory $directory -PowerShellPath $shellPath } 'exit 17'
    $state.FailStart = $true
    Expect-Failure { & $ScoopScript -Action Install -PackageDirectory $directory -PowerShellPath $shellPath } 'UAC cancelled fixture'
    $calls = $state.Calls
    Expect-Failure { & $ScoopScript -Action Install -PackageDirectory (Join-Path $fixture 'missing') -PowerShellPath $shellPath } 'Installer not found'
    Assert-Scoop ($state.Calls -eq $calls) 'Missing package attempted to elevate.'
    @{format_version=7;architecture='arm64';variant='zenz-cpu';channel='experimental'} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $directory 'manifest.json')
    Expect-Failure { & $ScoopScript -Action Install -PackageDirectory $directory -PowerShellPath $shellPath } 'Scoop requires an x64 release package'
    Assert-Scoop ($state.Calls -eq $calls) 'Experimental ARM64 package attempted to elevate.'
    if ($InstallScript) {
        # The real entry point rejects a different user before any OS writes.
        Expect-Failure { & $InstallScript -Action Unregister -ExpectedUserSid 'S-1-0-0' } ([char]0x6607 + [char]0x683c + [char]0x524d)
    }
    Write-Output 'PASS Scoop process arguments, same-user handoff, UAC cancellation and failure propagation'
} finally {
    $absolute = [IO.Path]::GetFullPath($fixture)
    $allowed = [IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $absolute.StartsWith($allowed, [StringComparison]::OrdinalIgnoreCase)) { throw 'Fixture escaped build directory.' }
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
