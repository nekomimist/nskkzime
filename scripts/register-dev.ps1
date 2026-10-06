#Requires -Version 7.0
[CmdletBinding()]
param([switch]$Unregister)
$ErrorActionPreference = 'Stop'

# Registration requires elevation. Building and all automated tests do not.
$principal = [Security.Principal.WindowsPrincipal]::new([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw '管理者として起動したPowerShell 7で実行してください。'
}
if (-not [Environment]::Is64BitProcess) { throw 'x64のPowerShellを使用してください。' }
$repositoryRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$installRoot = [IO.Path]::GetFullPath((Join-Path $repositoryRoot 'out/installed'))
$registryPath = 'HKLM:\Software\Classes\CLSID\{6EF45A57-E12E-49F1-8A4E-2569D8361A50}\InprocServer32'
$registration = Get-ItemProperty -LiteralPath $registryPath -ErrorAction SilentlyContinue
$registeredDll = if ($registration) { $registration.'(default)' } else { $null }
$registrar = Join-Path $env:SystemRoot 'System32/regsvr32.exe'

if ($Unregister) {
    if (-not $registeredDll) { throw 'nskkzimeの登録済みDLLが見つかりません。' }
    $dllPath = [IO.Path]::GetFullPath($registeredDll)
    $expectedPrefix = $installRoot.TrimEnd('\') + '\'
    if (-not $dllPath.StartsWith($expectedPrefix, [StringComparison]::OrdinalIgnoreCase) -or
        [IO.Path]::GetFileName($dllPath) -ne 'nskkzime_tsf.dll') {
        throw "このスクリプトが配置したDLLではありません。登録元のDLLで解除してください: $dllPath"
    }
    if (-not (Test-Path -LiteralPath $dllPath -PathType Leaf)) { throw "登録済みDLLがありません: $dllPath" }
    $process = Start-Process -FilePath $registrar -ArgumentList @('/u', '/s', ('"' + $dllPath + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "登録解除に失敗しました (regsvr32 exit $($process.ExitCode))。" }
    if (Test-Path -LiteralPath $registryPath) { throw '登録解除後もCLSIDが残っています。' }
    Write-Output 'nskkzimeの登録を解除しました。ビルド・配置ファイルは保持しています。'
} else {
    $classPath = Split-Path -Parent $registryPath
    if (Test-Path -LiteralPath $classPath) { throw '既に登録されています。使用中アプリを閉じ、元のDLLで登録解除してから更新してください。' }
    $bundle = & (Join-Path $PSScriptRoot 'stage-dev.ps1')
    $dllPath = $bundle.Dll
    $process = Start-Process -FilePath $registrar -ArgumentList @('/s', ('"' + $dllPath + '"')) -WindowStyle Hidden -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "登録に失敗しました (regsvr32 exit $($process.ExitCode))。" }
    $actual = (Get-ItemProperty -LiteralPath $registryPath).'(default)'
    if ($actual -ne $dllPath) { throw '登録先のDLLが想定と一致しません。' }
    Write-Output "登録しました: $dllPath"
    Write-Output '既定IMEは変更していません。通常権限のime_probe.exeで入力を検証できます。'
}
