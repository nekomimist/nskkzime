#Requires -Version 7.0
param([string]$BuildDirectory=(Join-Path $PSScriptRoot '../out/arm64x'))
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$repository=Split-Path -Parent $PSScriptRoot
$verifier=Join-Path $repository 'scripts/package-arm64x.ps1'
$normal=Join-Path $repository 'scripts/package.ps1'
Import-Module (Join-Path $repository 'scripts/installation.psm1') -Force
$fixture=Join-Path ([IO.Path]::GetFullPath($BuildDirectory)) ('package-test ARM64 日本語 space-'+[Guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($fixture) | Out-Null
$rejections=0
function Require([bool]$Value,[string]$Message) { if(-not $Value){throw $Message} }
function Reject([scriptblock]$Action,[string]$Label) {
    $failed=$false
    try { & $Action | Out-Null } catch { $failed=$true }
    Require $failed "Accepted invalid ARM64 package: $Label"
    ++$script:rejections
    "PASS rejection: $Label"
}
function Metadata-Section([byte[]]$Bytes) {
    $pe=[BitConverter]::ToUInt32($Bytes,60)
    $table=$pe+24+[BitConverter]::ToUInt16($Bytes,$pe+20)
    for($index=0;$index -lt [BitConverter]::ToUInt16($Bytes,$pe+6);++$index){
        $entry=$table+40*$index
        if([Text.Encoding]::ASCII.GetString($Bytes,$entry,8).TrimEnd([char]0) -ceq '.nskkz'){
            return @{raw=[BitConverter]::ToUInt32($Bytes,$entry+20);size=[BitConverter]::ToUInt32($Bytes,$entry+16)}
        }
    }
    throw 'Missing metadata section in test fixture.'
}
try {
    $created=& $verifier -SourceDirectory (Join-Path $BuildDirectory 'Release') -Output (Join-Path $fixture 'arm64.zip')
    Require ((& $verifier -Verify $created.Path).Verified) 'ARM64 ZIP verification failed.'
    $expanded=Join-Path $fixture 'expanded'
    [IO.Compression.ZipFile]::ExtractToDirectory($created.Path,$expanded)
    $checked=& $verifier -VerifyDirectory $expanded
    Require ($checked.Verified -and $checked.FormatVersion -eq 7 -and $checked.Files -eq 22 -and $checked.Architecture -ceq 'arm64') 'ARM64 inventory/policy differs.'
    Require (-not (Test-Path -LiteralPath (Join-Path $expanded 'tools/scoop.ps1'))) 'Scoop entry included in experimental package.'
    Require ([IO.File]::ReadAllText($created.ChecksumPath) -ceq ($created.SHA256+'  arm64.zip'+"`n")) 'ARM64 checksum differs.'
    Reject { & $normal -Verify $created.Path } 'normal ZIP entry rejects experimental format'
    Reject { & $normal -VerifyDirectory $expanded } 'normal directory entry rejects experimental format'
    Reject { & $verifier -Output $created.Path } 'existing output is retained'
    $manifestPath=Join-Path $expanded 'manifest.json'
    $originalManifest=[IO.File]::ReadAllText($manifestPath)
    function Rewrite-Manifest($Value) { $Value | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $manifestPath -Encoding utf8 }
    function Check-ManifestChange([scriptblock]$Change,[string]$Label) {
        $manifest=$originalManifest | ConvertFrom-Json -AsHashtable
        & $Change $manifest
        Rewrite-Manifest $manifest
        try { Reject { & $verifier -VerifyDirectory $expanded } $Label }
        finally { [IO.File]::WriteAllText($manifestPath,$originalManifest) }
    }
    Check-ManifestChange {param($m) $m.architecture='x64'} 'foreign architecture'
    Check-ManifestChange {param($m) $m.channel='release'} 'foreign channel'
    Check-ManifestChange {param($m) $m.installation_profile='x64-release'} 'foreign installation profile'
    function Check-ImageChange([string]$Name,[scriptblock]$Change,[string]$Label) {
        $path=Join-Path $expanded $Name
        $original=[IO.File]::ReadAllBytes($path)
        $bytes=[byte[]]$original.Clone()
        $changed=& $Change $bytes
        if($changed -is [byte[]]){$bytes=$changed}
        [IO.File]::WriteAllBytes($path,$bytes)
        $manifest=$originalManifest | ConvertFrom-Json -AsHashtable
        $entry=@($manifest.files | Where-Object path -ceq $Name)[0]
        $entry.size=$bytes.Length; $entry.sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        Rewrite-Manifest $manifest
        try { Reject { & $verifier -VerifyDirectory $expanded } $Label }
        finally { [IO.File]::WriteAllBytes($path,$original); [IO.File]::WriteAllText($manifestPath,$originalManifest) }
    }
    function Rewrite-Records([byte[]]$Bytes,[scriptblock]$Change) {
        $section=Metadata-Section $Bytes
        $texts=@([Text.Encoding]::UTF8.GetString($Bytes,$section.raw,$section.size).Split([char]0,[StringSplitOptions]::RemoveEmptyEntries))
        $records=@($texts | ForEach-Object { $_ | ConvertFrom-Json -AsHashtable })
        $rewritten=@(& $Change $records)
        $json=($rewritten | ForEach-Object { ($_ | ConvertTo-Json -Compress -Depth 10)+[char]0 }) -join ''
        $data=[Text.Encoding]::UTF8.GetBytes($json)
        Require ($data.Length -le $section.size) 'Changed metadata does not fit the real PE section.'
        [Array]::Clear($Bytes,$section.raw,$section.size)
        [Array]::Copy($data,0,$Bytes,$section.raw,$data.Length)
    }
    Check-ImageChange 'bin/nskkzime_tsf.dll' {
        param($bytes) Rewrite-Records $bytes {param($records) $records | Where-Object variant -ceq 'arm64x-native'}
    } 'missing EC metadata after rehash'
    Check-ImageChange 'bin/nskkzime_tsf.dll' {
        param($bytes) Rewrite-Records $bytes {param($records) $records[1].variant=$records[0].variant; $records}
    } 'duplicate metadata variant after rehash'
    Check-ImageChange 'bin/nskkzime_tsf.dll' {
        param($bytes) $pe=[BitConverter]::ToUInt32($bytes,60); [Array]::Clear($bytes,$pe+24+192,8)
    } 'AA64 and dual records without ARM64X load configuration'
    Check-ImageChange 'bin/nkkz-ime-server.exe' {
        param($bytes) $pe=[BitConverter]::ToUInt32($bytes,60); $bytes[$pe+4]=0x64; $bytes[$pe+5]=0x86
    } 'x64 server after rehash'
    Check-ImageChange 'bin/nkkz-ime-server.exe' {
        param($bytes) Rewrite-Records $bytes {param($records) $records[0].llama_backend.cpu_contract='zenz-msvc-avx2-v1'; $records}
    } 'foreign backend CPU contract after rehash'
    Check-ImageChange 'bin/nkkz-ime-server.exe' {
        param($bytes) Rewrite-Records $bytes {param($records) $records[0].source_sha256='0'*64; $records}
    } 'mixed native source after rehash'
    Check-ImageChange 'bin/ime_probe-arm64.exe' {
        param($bytes) Rewrite-Records $bytes {param($records) $records[0].cmake_cache_sha256='0'*64; $records}
    } 'probe/native TSF cache mismatch after rehash'
    Check-ImageChange 'bin/nkkz-ime-server.exe' {
        param($bytes) Rewrite-Records $bytes {param($records) $records[0].llama_backend.extra='unexpected'; $records}
    } 'extra backend metadata key after rehash'
    Check-ImageChange 'bin/nkkz-ime-server.exe' {
        param($bytes) Rewrite-Records $bytes {param($records) $records[0].llama_backend.compiler=@($records[0].llama_backend.compiler); $records}
    } 'array instead of backend compiler string after rehash'
    Check-ImageChange 'bin/nkkz-ime-server.exe' {
        param($bytes)
        $section=Metadata-Section $bytes
        $json=[Text.Encoding]::UTF8.GetString($bytes,$section.raw,$section.size).TrimEnd([char]0)
        $json=$json.Replace('"cpu_contract":','"cpu_contract":"zenz-arm64-armv8a-v1","cpu_contract":')
        $data=[Text.Encoding]::UTF8.GetBytes($json)
        Require ($data.Length -lt $section.size) 'Duplicate-key fixture exceeds section.'
        [Array]::Clear($bytes,$section.raw,$section.size); [Array]::Copy($data,0,$bytes,$section.raw,$data.Length)
    } 'duplicate nested JSON key after rehash'
    # A foreign target or previous bundle must be rejected before a registrar call.
    $root=Join-Path $fixture 'managed'; [IO.Directory]::CreateDirectory($root) | Out-Null
    $bundle=Add-NskkzimeVersion $root $expanded $verifier
    $foreign=Join-Path $root ('versions/'+('0'*64))
    [IO.Directory]::CreateDirectory($foreign) | Out-Null
    foreach($item in Get-ChildItem -LiteralPath $bundle.Directory){Copy-Item -LiteralPath $item.FullName -Destination $foreign -Recurse}
    $foreignManifest=Join-Path $foreign 'manifest.json'
    $manifest=Get-Content -LiteralPath $foreignManifest -Raw | ConvertFrom-Json -AsHashtable
    $manifest.channel='release'; $manifest | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $foreignManifest
    $state=@{calls=0;dll=$null;x86=$null}
    $read={return $state.dll}.GetNewClosure(); $readX86={return $state.x86}.GetNewClosure()
    $registrar={param($dll,$remove) ++$state.calls; return 0}.GetNewClosure()
    Reject { Invoke-NskkzimeTransition $root ('0'*64) $verifier $read $registrar $null $readX86 } 'foreign target policy before registration'
    @{format_version=2;previous=('0'*64);target=$bundle.Version} | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'transaction.json')
    Reject { Repair-NskkzimeTransition $root $verifier $read $registrar $null $readX86 } 'foreign previous policy during recovery'
    Require ($state.calls -eq 0) 'Foreign bundle reached the registrar.'
    "PASS ARM64 package, ZIP/directory verification, checksum and $rejections negative cases"
} finally {
    $absolute=[IO.Path]::GetFullPath($fixture)
    $allowed=[IO.Path]::GetFullPath($BuildDirectory).TrimEnd('\','/')+[IO.Path]::DirectorySeparatorChar
    if(-not $absolute.StartsWith($allowed,[StringComparison]::OrdinalIgnoreCase)){throw 'Fixture escaped build directory.'}
    Assert-NskkzimePath $absolute
    Remove-Item -LiteralPath $absolute -Recurse -Force
}
