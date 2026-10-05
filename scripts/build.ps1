#requires -Version 7.0
param([ValidateSet('Debug', 'Release')][string]$Configuration = 'Debug', [switch]$Zenz)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Visual Studio Installer / vswhere.exe が見つかりません。' }
$vsRoot = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'Visual Studio 2022 の C++ Build Tools が見つかりません。' }
$cmake = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path -LiteralPath $cmake)) { throw 'Build Tools の C++ CMake tools for Windows が必要です。' }
$preset = $Configuration.ToLowerInvariant()
$configurePreset = 'windows-x64'
if ($Zenz) { $preset += '-zenz'; $configurePreset += '-zenz' }
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
    Invoke-BuildTool $cmake @('--build', '--preset', $preset, '--parallel')
    Invoke-BuildTool $ctest @('--preset', $preset)
} finally { Pop-Location }
