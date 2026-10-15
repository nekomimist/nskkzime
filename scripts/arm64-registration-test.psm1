#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Save-Arm64RegistrationJournal([string]$Path, [Collections.IDictionary]$Journal, [string]$Phase) {
    $Journal.phase = $Phase
    $temporary = $Path + '.' + [Guid]::NewGuid().ToString('N') + '.tmp'
    $bytes = [Text.Encoding]::UTF8.GetBytes(($Journal | ConvertTo-Json -Depth 24) + "`n")
    $stream = [IO.File]::Open($temporary, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
    try { $stream.Write($bytes); $stream.Flush($true) } finally { $stream.Dispose() }
    try { [IO.File]::Move($temporary, $Path, $true) }
    finally { if (Test-Path -LiteralPath $temporary) { [IO.File]::Delete($temporary) } }
}
function Assert-Arm64RegistrationOwner($State, [Collections.IDictionary]$Journal) {
    if ($State.shadow) { throw 'A per-user COM registration shadows the test. No registration will be changed.' }
    foreach ($view in @('native', 'x86')) {
        if ($State[$view].exists -and -not [string]::Equals($State[$view].dll, $Journal[$view], [StringComparison]::OrdinalIgnoreCase)) {
            throw "The $view COM registration is not owned by this test. No registration will be changed."
        }
    }
    if ($State.machine_tip -and -not $State.native.exists) { throw 'Machine TSF registration has no matching native COM owner. Inspect the retained journal.' }
}
function New-Arm64RegistrationJournal([string]$Path, [string]$NativeDll, [string]$X86Dll,
    [Collections.IDictionary]$Identity, [scriptblock]$ReadState) {
    if (Test-Path -LiteralPath $Path) { throw 'A registration journal already exists. Use Recover for an interrupted test.' }
    $initial = & $ReadState
    if ($initial.shadow -or $initial.native.exists -or $initial.x86.exists -or $initial.tip) {
        throw 'This test requires no existing nskkzime COM or TSF registration.'
    }
    $journal = [ordered]@{format='nskkzime-arm64-registration-test-v1';native=$NativeDll;x86=$X86Dll;
        identity=$Identity;initial=@{user_state_sha256=$initial.user_state_sha256};owned_user_tip=$null;phase='prepared'}
    Save-Arm64RegistrationJournal $Path $journal 'prepared'
    return $journal
}
function Add-Arm64TestRegistration([string]$Path, [Collections.IDictionary]$Journal,
    [scriptblock]$ReadState, [scriptblock]$Registrar, [scriptblock]$Probe) {
    if ($Journal.phase -cne 'prepared') { throw 'Only a new test journal can register the IME.' }
    $before = & $ReadState
    if ($before.native.exists -or $before.x86.exists -or $before.tip -or $before.shadow) { throw 'Registration changed after preflight.' }
    Save-Arm64RegistrationJournal $Path $Journal 'register-native-intent'
    $code = & $Registrar 'native' $false
    $state = & $ReadState
    Assert-Arm64RegistrationOwner $state $Journal
    if ($state.native.exists) {
        $Journal.owned_user_tip = $state.user_tip
        Save-Arm64RegistrationJournal $Path $Journal 'native-observed'
    }
    if ($code -ne 0) { throw 'Native test registration failed. Recover with the retained DLL.' }
    if (-not $state.native.exists -or -not $state.tip -or $state.x86.exists) { throw 'Native registration did not produce the expected state.' }
    foreach ($hostArchitecture in @('arm64','x64','arm64ec')) { & $Probe $hostArchitecture $true }
    Save-Arm64RegistrationJournal $Path $Journal 'register-x86-intent'
    if ((& $Registrar 'x86' $false) -ne 0) { throw 'x86 test registration failed. Recover with the retained DLLs.' }
    $state = & $ReadState
    Assert-Arm64RegistrationOwner $state $Journal
    if (-not $state.native.exists -or -not $state.x86.exists -or -not $state.tip) { throw 'Paired registration is incomplete.' }
    & $Probe 'x86' $true
    Save-Arm64RegistrationJournal $Path $Journal 'registered'
}
function Remove-Arm64TestRegistration([string]$Path, [Collections.IDictionary]$Journal,
    [scriptblock]$ReadState, [scriptblock]$Registrar, [scriptblock]$Probe, [scriptblock]$RestoreUserTip) {
    # Check BOTH owners before removing either. A crash can leave either call's
    # outcome unknown; intent alone never authorizes deleting another owner.
    $state = & $ReadState
    Assert-Arm64RegistrationOwner $state $Journal
    $diagnostics = [Collections.Generic.List[string]]::new()
    if ($state.x86.exists) {
        Save-Arm64RegistrationJournal $Path $Journal 'unregister-x86-intent'
        if ((& $Registrar 'x86' $true) -ne 0) { throw 'x86 cleanup failed. Keep the DLL and retry Recover.' }
        $state = & $ReadState
        Assert-Arm64RegistrationOwner $state $Journal
        if ($state.x86.exists) { throw 'x86 COM registration remains after cleanup.' }
        try { & $Probe 'x86' $false } catch { $diagnostics.Add($_.ToString()) }
        if ($state.native.exists) {
            if (-not $state.machine_tip) { $diagnostics.Add('x86 cleanup removed the shared TSF profile.') }
            foreach ($hostArchitecture in @('arm64','x64','arm64ec')) {
                try { & $Probe $hostArchitecture $true } catch { $diagnostics.Add($_.ToString()) }
            }
        }
    }
    $state = & $ReadState
    Assert-Arm64RegistrationOwner $state $Journal
    if ($state.native.exists) {
        $Journal.owned_user_tip = $state.user_tip
        Save-Arm64RegistrationJournal $Path $Journal 'unregister-native-intent'
        if ((& $Registrar 'native' $true) -ne 0) { throw 'Native cleanup failed. Keep the DLL and retry Recover.' }
    }
    $after = & $ReadState
    Assert-Arm64RegistrationOwner $after $Journal
    if (-not $after.native.exists -and -not $after.x86.exists -and -not $after.machine_tip -and $after.tip) {
        if (-not $RestoreUserTip -or $null -eq $Journal.owned_user_tip) { throw 'User TSF state remains without a recorded owned snapshot. Inspect the retained reports.' }
        Save-Arm64RegistrationJournal $Path $Journal 'restore-user-tip-intent'
        & $RestoreUserTip $Journal.owned_user_tip
        $after = & $ReadState
        Assert-Arm64RegistrationOwner $after $Journal
    }
    if ($after.native.exists -or $after.x86.exists -or $after.tip) { throw 'Test registration remains after cleanup.' }
    foreach ($hostArchitecture in @('arm64','x64','arm64ec','x86')) {
        try { & $Probe $hostArchitecture $false } catch { $diagnostics.Add($_.ToString()) }
    }
    if ($after.user_state_sha256 -cne $Journal.initial.user_state_sha256) {
        $diagnostics.Add('User input defaults or ordinary nskkzime files changed; audit the before/after reports.')
    }
    $Journal.validation_errors = $diagnostics.ToArray()
    Save-Arm64RegistrationJournal $Path $Journal $(if ($diagnostics.Count) { 'removed-with-errors' } else { 'complete' })
    if ($diagnostics.Count) { throw ('Test registration is removed, but validation failed: ' + ($diagnostics -join ' / ')) }
}
Export-ModuleMember -Function New-Arm64RegistrationJournal, Add-Arm64TestRegistration, Remove-Arm64TestRegistration
