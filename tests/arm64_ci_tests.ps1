#Requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$Archive,[Parameter(Mandatory)][string]$ExpectedSHA256,
    [Parameter(Mandatory)][string]$ExpectedSourceSHA256,[Parameter(Mandatory)][string]$OutputDirectory)
Set-StrictMode -Version Latest
$ErrorActionPreference='Stop'
$runner=Join-Path $PSScriptRoot '../scripts/run-arm64-ci.ps1'
$root=[IO.Path]::GetFullPath($OutputDirectory)
if(Test-Path -LiteralPath $root){throw 'Use a new CI validation directory.'}
[IO.Directory]::CreateDirectory($root)|Out-Null
# Run the real transported plan once, then ensure malformed inputs never run it.
& $runner -Archive $Archive -ExpectedSHA256 $ExpectedSHA256 -ExpectedSourceSHA256 $ExpectedSourceSHA256 -OutputDirectory (Join-Path $root 'positive')
function Rejected([string]$Name,[string]$Path,[string]$ZipHash,[string]$SourceHash,[string]$Reason){
    $message=$null
    try{& $runner -Archive $Path -ExpectedSHA256 $ZipHash -ExpectedSourceSHA256 $SourceHash -OutputDirectory (Join-Path $root $Name)}catch{$message=$_.Exception.Message}
    if(-not $message -or $message -notmatch $Reason){throw "Wrong rejection for ${Name}: $message"}
    [ordered]@{case=$Name;rejected=$true;error=$message}
}
function Mutated([string]$Name){
    $path=Join-Path $root ($Name+'.zip')
    $output=[IO.Compression.ZipFile]::Open($path,[IO.Compression.ZipArchiveMode]::Create)
    try{
        if($Name -in @('traversal','duplicate')){
            $names=if($Name -eq 'traversal'){@('../escaped.txt')}else{@('ci-manifest.json','CI-MANIFEST.JSON')}
            foreach($name in $names){$entry=$output.CreateEntry($name);$writer=[IO.StreamWriter]::new($entry.Open());try{$writer.Write('{}')}finally{$writer.Dispose()}}
        }else{
            $inputZip=[IO.Compression.ZipFile]::OpenRead([IO.Path]::GetFullPath($Archive))
            try{
                foreach($entry in $inputZip.Entries){
                    if($Name -eq 'plan' -and $entry.FullName -eq 'ci-manifest.json'){
                        $reader=[IO.StreamReader]::new($entry.Open());try{$manifest=$reader.ReadToEnd()|ConvertFrom-Json}finally{$reader.Dispose()}
                        $file=@($manifest.files|Where-Object path -CEQ 'build/CTestTestfile.cmake.in')[0]
                        $templateEntry=$inputZip.GetEntry('build/CTestTestfile.cmake.in')
                        $reader=[IO.StreamReader]::new($templateEntry.Open());try{$template=$reader.ReadToEnd()+"`nadd_test(unexpected @BUNDLE@/tools/bin/cmake.exe -E true)`n"}finally{$reader.Dispose()}
                        $bytes=[Text.UTF8Encoding]::new($false).GetBytes($template)
                        $file.size=$bytes.Length;$file.sha256=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
                        $newEntry=$output.CreateEntry($entry.FullName);$writer=[IO.StreamWriter]::new($newEntry.Open(),[Text.UTF8Encoding]::new($false))
                        try{$writer.Write(($manifest|ConvertTo-Json -Depth 12))}finally{$writer.Dispose()}
                        continue
                    }
                    $newEntry=$output.CreateEntry($entry.FullName)
                    $source=$entry.Open();$target=$newEntry.Open()
                    try{
                        if($Name -eq 'plan' -and $entry.FullName -eq 'build/CTestTestfile.cmake.in'){
                            $reader=[IO.StreamReader]::new($source);try{$text=$reader.ReadToEnd()+"`nadd_test(unexpected @BUNDLE@/tools/bin/cmake.exe -E true)`n"}finally{$reader.Dispose()}
                            $bytes=[Text.UTF8Encoding]::new($false).GetBytes($text);$target.Write($bytes)
                        }else{
                            $source.CopyTo($target)
                            if($Name -eq 'tamper' -and $entry.FullName -eq 'source/tests/config_cli.cmake'){$target.WriteByte(32)}
                        }
                    }finally{$source.Dispose();$target.Dispose()}
                }
                if($Name -eq 'extra'){$newEntry=$output.CreateEntry('unexpected.txt');$stream=$newEntry.Open();try{$stream.WriteByte(32)}finally{$stream.Dispose()}}
            }finally{$inputZip.Dispose()}
        }
    }finally{$output.Dispose()}
    $path
}
$cases=@()
$cases+=Rejected 'checksum' $Archive ('0'*64) $ExpectedSourceSHA256 '^CI archive changed'
$cases+=Rejected 'source' $Archive $ExpectedSHA256 ('0'*64) '^Unexpected CI bundle identity'
foreach($name in @('traversal','duplicate','tamper','extra','plan')){
    $path=Mutated $name;$hash=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
    $reason=switch($name){traversal{'^Invalid CI bundle path'};duplicate{'^Duplicate CI ZIP entry'};tamper{'^CI file changed: source/tests/config_cli.cmake'};extra{'^Unexpected CI files'};plan{'^Generated CTest plan differs'}}
    $cases+=Rejected $name $path $hash $ExpectedSourceSHA256 $reason
}
if(Test-Path -LiteralPath (Join-Path $root 'escaped.txt')){throw 'Traversal created a file outside the extraction directory.'}
$positive=Get-Content -LiteralPath (Join-Path $root 'positive/reports/result.json') -Raw|ConvertFrom-Json
[ordered]@{passed=$true;positive_tests=$positive.tests;negative_cases=$cases} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $root 'validation.json') -Encoding utf8
Write-Output 'PASS ARM64 transported tests and seven CI bundle rejection cases'
