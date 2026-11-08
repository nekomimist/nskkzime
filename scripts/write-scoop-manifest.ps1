#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$PackageArchive, [Parameter(Mandatory)][string]$Output)
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
$package = & (Join-Path $PSScriptRoot 'package.ps1') -Verify $PackageArchive
if (-not $package.Verified -or $package.FormatVersion -ne 8) { throw 'The native x64 release package (format 8) is required.' }
$manifest = [ordered]@{
    version = $package.Version
    description = 'Windows SKK-style Japanese IME with a CPU zenz inference server'
    homepage = 'https://github.com/nekomimist/nskkzime'
    license = 'MIT'
    architecture = @{'64bit' = @{url = "https://github.com/nekomimist/nskkzime/releases/download/v$($package.Version)/nskkzime-v$($package.Version)-windows-x64.zip"; hash = $package.SHA256}}
    extract_to = 'package'
    depends = @('extras/vcredist2022')
    pre_install = "if (`$global) { abort 'Use a per-user Scoop installation; nskkzime manages the machine-wide IME registration for one managing user.' }"
    installer = @{script = 'try { & "$dir\package\tools\scoop.ps1" -Action Install -PackageDirectory "$dir\package" } catch { abort $_.Exception.Message }'}
    uninstaller = @{script = @('if ($cmd -eq ''uninstall'') {', '    try { & "$dir\package\tools\scoop.ps1" -Action Unregister -PackageDirectory "$dir\package" } catch { abort $_.Exception.Message }', '}')}
    bin = @('package\bin\nskkzime_config.exe', 'package\bin\nskkzime_user_dictionary.exe')
    shortcuts = @(@('package\bin\nskkzime_settings.exe', 'nskkzime Settings'), @('package\bin\nskkzime_dictionary_gui.exe', 'nskkzime User Dictionary'))
    notes = @(
        'IME registration uses native setup and requires UAC approval by the same user. PowerShell 7 is not required. Windows x64 only; global Scoop installs are unsupported.',
        'Launch from Explorer or a normal terminal; setup rejects packaged or virtualized execution contexts.',
        'Configure a separate SKK dictionary and zenz model from nskkzime Settings. Inference requires AVX2/FMA/F16C/BMI2 support.',
        'Close applications before updating. Restart them or sign out and back in to use the new DLL.',
        'Uninstall unregisters the IME and broker startup, retaining protected program files and user data. See the packaged README for complete removal.',
        'This release is unsigned. See %ProgramFiles%\nskkzime\setup.log for installation results if present.'
    )
    checkver = 'github'
    autoupdate = @{architecture = @{'64bit' = @{url = 'https://github.com/nekomimist/nskkzime/releases/download/v$version/nskkzime-v$version-windows-x64.zip'}}; hash = @{url = '$url.sha256'}}
}
$path = [IO.Path]::GetFullPath($Output)
[IO.Directory]::CreateDirectory((Split-Path -Parent $path)) | Out-Null
$stream = [IO.File]::Open($path, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write, [IO.FileShare]::None)
try { $bytes = [Text.Encoding]::UTF8.GetBytes(($manifest | ConvertTo-Json -Depth 10) + "`n"); $stream.Write($bytes); $stream.Flush($true) }
finally { $stream.Dispose() }
Get-Item -LiteralPath $path
