#requires -Version 7.0
param(
    [ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug',
    [switch]$Zenz, # Accepted for compatibility; zenz is now the default.
    [switch]$TestOnly,
    [switch]$UpstreamExperimental,
    [switch]$X86, # Build/test only the companion; x64 services must already exist.
    [ValidateSet('x64', 'ARM64')][string]$Architecture = 'x64',
    [switch]$BuildOnly, # Cross-built ARM64 executables must be tested on ARM64 Windows.
    [ValidateRange(1, 64)][int]$Jobs = [Math]::Min(8, [Environment]::ProcessorCount)
)
$ErrorActionPreference = 'Stop'
if ($Architecture -eq 'ARM64') {
    if ($X86 -or (-not $TestOnly -and -not $UpstreamExperimental)) { throw 'ARM64は -TestOnly または -UpstreamExperimental の実験用ビルドに対応しています。' }
    if (-not $BuildOnly -and [Runtime.InteropServices.RuntimeInformation]::OSArchitecture -ne [Runtime.InteropServices.Architecture]::Arm64) {
        throw 'ARM64テストはARM64 Windowsで実行してください。クロスビルドには -BuildOnly を指定します。'
    }
}
if ($UpstreamExperimental -and ($TestOnly -or $X86)) { throw '-UpstreamExperimental は -TestOnly / -X86 と併用しません。' }
if ($UpstreamExperimental -and $Configuration -ne 'Release') { throw 'upstreamの推論付き実験はRelease構成を使用します。' }
if ($X86 -and ($TestOnly -or $Zenz)) { throw '-X86 は -TestOnly / -Zenz と併用しません。' }
if ($Zenz -and $TestOnly) { throw '-Zenz と -TestOnly は同時に指定できません。' }
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer / vswhere.exe が見つかりません。' }
$requiredTools = if ($Architecture -eq 'ARM64') { 'Microsoft.VisualStudio.Component.VC.Tools.ARM64' } else { 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' }
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires $requiredTools -property installationPath
if (-not $vsRoot) { throw 'Visual Studio 2022 の C++ Build Tools が見つかりません。' }
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'Build Tools の C++ CMake tools for Windows が必要です。' }
$preset = $Configuration.ToLowerInvariant()
$configurePreset = 'windows-x64'
if ($TestOnly) { $preset += '-test-only'; $configurePreset += '-test-only' }
if ($X86) { $preset += '-x86'; $configurePreset = 'windows-x86' }
if ($UpstreamExperimental) { $preset += '-upstream-experimental'; $configurePreset += '-upstream-experimental' }
if ($Architecture -eq 'ARM64') {
    $suffix = if ($TestOnly) { 'test-only' } else { 'upstream-experimental' }
    $preset = $Configuration.ToLowerInvariant() + '-arm64-' + $suffix
    $configurePreset = 'windows-arm64-' + $suffix
}
# Some launcher environments contain both PATH and Path. Legacy MSBuild's
# ProcessStartInfo rejects that environment. Normalize only our child process,
# without changing the caller's environment or the user's persisted settings.
function Invoke-BuildTool([string]$Executable, [string[]]$ToolArguments) {
    $start = [System.Diagnostics.ProcessStartInfo]::new()
    $start.FileName = $Executable
    $start.UseShellExecute = $false
    $start.WorkingDirectory = $repoRoot
    $start.Environment.Clear()
    foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) {
        $start.Environment[$entry.Key] = $entry.Value
    }
    foreach ($argument in $ToolArguments) { $start.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($start)
    try {
        $process.WaitForExit()
        if ($process.ExitCode -ne 0) { throw "$Executable failed with exit code $($process.ExitCode)." }
    } finally { $process.Dispose() }
}
Push-Location $repoRoot
try {
    Invoke-BuildTool $cmake @('--preset', $configurePreset, "-DCMAKE_GENERATOR_INSTANCE=$vsRoot")
    Invoke-BuildTool $cmake @('--build', '--preset', $preset, '--parallel', "$Jobs")
    if (-not $TestOnly -and -not $X86 -and $Architecture -eq 'x64') {
        Invoke-BuildTool $cmake @('--preset', 'windows-x86', "-DCMAKE_GENERATOR_INSTANCE=$vsRoot")
        $companionPreset = $Configuration.ToLowerInvariant() + '-x86'
        Invoke-BuildTool $cmake @('--build', '--preset', $companionPreset, '--parallel', "$Jobs")
        if (-not $BuildOnly) { Invoke-BuildTool $ctest @('--preset', $companionPreset) }
    }
    if (-not $BuildOnly) { Invoke-BuildTool $ctest @('--preset', $preset) }
} finally { Pop-Location }
