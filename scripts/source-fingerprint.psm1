Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Invoke-NskkzimeGit([string]$Repository, [string[]]$Arguments, [switch]$AllowDifference) {
    $start = [Diagnostics.ProcessStartInfo]::new()
    $start.FileName = 'git'
    $start.UseShellExecute = $false
    $start.RedirectStandardOutput = $true
    $start.RedirectStandardError = $true
    $start.StandardOutputEncoding = [Text.UTF8Encoding]::new($false, $true)
    $start.Environment.Clear()
    foreach ($entry in [Environment]::GetEnvironmentVariables('Process').GetEnumerator()) { $start.Environment[$entry.Key] = $entry.Value }
    foreach ($argument in @('-C', $Repository) + $Arguments) { $start.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($start)
    try {
        $output = $process.StandardOutput.ReadToEndAsync()
        $errors = $process.StandardError.ReadToEndAsync()
        $process.WaitForExit()
        $text = $output.GetAwaiter().GetResult()
        $null = $errors.GetAwaiter().GetResult()
        if ($process.ExitCode -ne 0 -and -not ($AllowDifference -and $process.ExitCode -eq 1)) { throw 'ビルド元のGit情報を取得できません。' }
        [pscustomobject]@{ Text = $text; ExitCode = $process.ExitCode }
    } finally { $process.Dispose() }
}
function Assert-NskkzimeFingerprintPath([string]$Path) {
    $item = Get-Item -LiteralPath $Path -Force
    if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'ビルド元の再解析ポイントは記録できません。' }
}
function Get-NskkzimeFilesFingerprint([string]$Root, [string[]]$Names, [switch]$PathsChecked) {
    [Array]::Sort($Names, [StringComparer]::Ordinal)
    $digest = [Security.Cryptography.IncrementalHash]::CreateHash([Security.Cryptography.HashAlgorithmName]::SHA256)
    try {
        foreach ($name in $Names) {
            $path = [IO.Path]::GetFullPath((Join-Path $Root $name))
            if (-not $path.StartsWith($Root.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'ビルド元の範囲外です。' }
            # A tracked but deleted file still changes the fingerprint.
            $hash = 'missing'
            if (Test-Path -LiteralPath $path) {
                for ($ancestor = $(if (-not $PathsChecked) { Get-Item -LiteralPath $path -Force }); $null -ne $ancestor; $ancestor = $ancestor.Parent) {
                    Assert-NskkzimeFingerprintPath $ancestor.FullName
                    if ($ancestor.FullName -eq $Root) { break }
                    if ($ancestor -is [IO.FileInfo]) {
                        $ancestor = $ancestor.Directory; Assert-NskkzimeFingerprintPath $ancestor.FullName
                        if ($ancestor.FullName -eq $Root) { break }
                    }
                }
                $stream = [IO.File]::OpenRead($path)
                try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant() }
                finally { $stream.Dispose() }
            }
            $digest.AppendData([Text.Encoding]::UTF8.GetBytes($name.Replace('\', '/') + "`0" + $hash + "`0"))
        }
        [Convert]::ToHexString($digest.GetHashAndReset()).ToLowerInvariant()
    } finally { $digest.Dispose() }
}
function Get-NskkzimeSourceFingerprint([string]$Repository) {
    $root = [IO.Path]::GetFullPath($Repository).TrimEnd('\', '/')
    $gitRoot = (Invoke-NskkzimeGit $root @('rev-parse', '--show-toplevel')).Text.Trim()
    if ([IO.Path]::GetFullPath($gitRoot).TrimEnd('\', '/') -ne $root) { throw 'リポジトリーのルートを指定してください。' }
    $revision = (Invoke-NskkzimeGit $root @('rev-parse', '--verify', 'HEAD')).Text.Trim()
    if ($revision -cnotmatch '^([a-f0-9]{40}|[a-f0-9]{64})$') { throw 'ビルド元のrevisionが不正です。' }
    # Only build inputs are included. Unrelated user dictionaries and out/ are not read.
    $inputs = @('CMakeLists.txt', 'CMakePresets.json', 'cmake', 'include', 'src', 'third_party', 'scripts', 'tests')
    $tracked = (Invoke-NskkzimeGit $root (@('ls-files', '-z', '--cached', '--') + $inputs)).Text.Split("`0", [StringSplitOptions]::RemoveEmptyEntries)
    $untracked = (Invoke-NskkzimeGit $root (@('ls-files', '-z', '--others', '--exclude-standard', '--') + $inputs)).Text.Split("`0", [StringSplitOptions]::RemoveEmptyEntries)
    $changed = Invoke-NskkzimeGit $root (@('diff', '--quiet', 'HEAD', '--') + $inputs) -AllowDifference
    [pscustomobject]@{ Revision = $revision; Modified = ($changed.ExitCode -eq 1 -or $untracked.Count -ne 0)
        SHA256 = Get-NskkzimeFilesFingerprint $root ([string[]](@($tracked) + @($untracked))) }
}
function Get-NskkzimeDirectoryFingerprint([string]$Directory) {
    $root = [IO.Path]::GetFullPath($Directory).TrimEnd('\', '/')
    $names = [Collections.Generic.List[string]]::new()
    $pending = [Collections.Generic.Queue[string]]::new()
    $pending.Enqueue($root)
    while ($pending.Count) {
        $folder = $pending.Dequeue()
        Assert-NskkzimeFingerprintPath $folder
        foreach ($item in Get-ChildItem -LiteralPath $folder -Force) {
            if ($item.Name -eq '.git') { continue }
            Assert-NskkzimeFingerprintPath $item.FullName
            if ($item.PSIsContainer) { $pending.Enqueue($item.FullName) }
            else { $names.Add([IO.Path]::GetRelativePath($root, $item.FullName)) }
        }
    }
    Get-NskkzimeFilesFingerprint $root $names.ToArray() -PathsChecked
}
Export-ModuleMember -Function Get-NskkzimeSourceFingerprint, Get-NskkzimeDirectoryFingerprint
