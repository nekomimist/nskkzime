#Requires -Version 7.0
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-Arm64JsonKeys($Value, [string[]]$Keys) {
    if ($Value -isnot [Collections.IDictionary] -or $Value.Count -ne $Keys.Count) { throw 'Invalid ARM64 build metadata schema.' }
    foreach ($key in $Keys) { if (-not $Value.Contains($key)) { throw 'Invalid ARM64 build metadata key.' } }
}
function Assert-Arm64JsonDuplicates($Element) {
    if ($Element.ValueKind -eq [Text.Json.JsonValueKind]::Object) {
        $keys = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($property in $Element.EnumerateObject()) {
            if (-not $keys.Add($property.Name)) { throw 'Duplicate ARM64 build metadata key.' }
            Assert-Arm64JsonDuplicates $property.Value
        }
    } elseif ($Element.ValueKind -eq [Text.Json.JsonValueKind]::Array) {
        foreach ($item in $Element.EnumerateArray()) { Assert-Arm64JsonDuplicates $item }
    }
}
function Get-NskkzimeArm64MetadataGroup([string]$Name) {
    if ($Name.StartsWith('bin/x86/')) { return 'x86' }
    if ($Name -ceq 'bin/nskkzime_tsf.dll') { return 'arm64x' }
    if ($Name -ceq 'bin/ime_probe-arm64.exe') { return 'probe' }
    return 'native'
}
function Get-NskkzimeArm64ImageMetadata([byte[]]$Bytes, [string]$Name) {
    # Inspect data only. Neither package creation nor verification loads an image.
    $invalid = "Invalid ARM64 PE/build metadata: $Name"
    $group = Get-NskkzimeArm64MetadataGroup $Name
    if ($Bytes.Length -lt 64 -or [BitConverter]::ToUInt16($Bytes,0) -ne 0x5a4d) { throw $invalid }
    $pe = [long][BitConverter]::ToUInt32($Bytes,60)
    if ($pe -gt $Bytes.Length - 26 -or [BitConverter]::ToUInt32($Bytes,$pe) -ne 0x4550 -or
        [BitConverter]::ToUInt16($Bytes,$pe+4) -ne $(if($group -eq 'x86'){0x14c}else{0xaa64}) -or
        [BitConverter]::ToUInt16($Bytes,$pe+24) -ne $(if($group -eq 'x86'){0x10b}else{0x20b})) { throw $invalid }
    $optionalSize = [BitConverter]::ToUInt16($Bytes,$pe+20)
    $count = [BitConverter]::ToUInt16($Bytes,$pe+6)
    $table = $pe + 24 + $optionalSize
    if ($optionalSize -lt $(if($group -eq 'x86'){96}else{112}) -or $count -lt 1 -or $count -gt 96 -or $table+40L*$count -gt $Bytes.Length) { throw $invalid }
    $sections = @(); $metadataSection = $null
    for ($index=0; $index -lt $count; ++$index) {
        $offset = $table + 40L*$index
        $section = @{name=[Text.Encoding]::ASCII.GetString($Bytes,$offset,8).TrimEnd([char]0);
            rva=[long][BitConverter]::ToUInt32($Bytes,$offset+12);size=[long][BitConverter]::ToUInt32($Bytes,$offset+16);
            raw=[long][BitConverter]::ToUInt32($Bytes,$offset+20);flags=[BitConverter]::ToUInt32($Bytes,$offset+36)}
        if ($section.size -and ($section.raw -lt $table+40L*$count -or $section.raw+$section.size -gt $Bytes.Length)) { throw $invalid }
        $sections += $section
        if ($section.name -ceq '.nskkz') {
            if ($metadataSection -or $section.size -lt 2 -or $section.size -gt 16384 -or ($section.flags -band 0xE0000000L) -ne 0x40000000L) { throw $invalid }
            $metadataSection = $section
        }
    }
    if (-not $metadataSection) { throw $invalid }
    if ($group -eq 'arm64x') {
        # Windows SDK IMAGE_LOAD_CONFIG_DIRECTORY64 / IMAGE_DYNAMIC_RELOCATION64.
        # AA64 alone also describes pure ARM64; require the ARM64X relocation entry.
        if ($optionalSize -lt 200 -or [BitConverter]::ToUInt32($Bytes,$pe+24+108) -lt 11) { throw $invalid }
        $rva = [long][BitConverter]::ToUInt32($Bytes,$pe+24+192)
        $size = [long][BitConverter]::ToUInt32($Bytes,$pe+24+196)
        $mapped = @($sections | Where-Object { $rva -ge $_.rva -and $rva+$size -le $_.rva+$_.size })
        if ($size -lt 232 -or $mapped.Count -ne 1) { throw $invalid }
        $load = $mapped[0].raw + $rva - $mapped[0].rva
        $declared=[BitConverter]::ToUInt32($Bytes,$load)
        if ($declared -lt 232 -or $declared -gt $size) { throw $invalid }
        $imageBase=[BitConverter]::ToUInt64($Bytes,$pe+24+24)
        $hybrid=[BitConverter]::ToUInt64($Bytes,$load+200)
        if ($hybrid -lt $imageBase -or $hybrid-$imageBase -gt [uint32]::MaxValue) { throw $invalid }
        $hybridRva=[long]($hybrid-$imageBase)
        $hybridSection=@($sections | Where-Object { $hybridRva -ge $_.rva -and $hybridRva+4 -le $_.rva+$_.size })
        if ($hybridSection.Count -ne 1) { throw $invalid }
        $hybridOffset=$hybridSection[0].raw+$hybridRva-$hybridSection[0].rva
        if ([BitConverter]::ToUInt32($Bytes,$hybridOffset) -ne 2) { throw $invalid }
        $relocOffset = [long][BitConverter]::ToUInt32($Bytes,$load+224)
        $relocSection = [BitConverter]::ToUInt16($Bytes,$load+228)
        if ($relocSection -lt 1 -or $relocSection -gt $count) { throw $invalid }
        $section = $sections[$relocSection-1]
        if ($relocOffset+8 -gt $section.size) { throw $invalid }
        $reloc = $section.raw + $relocOffset
        $relocSize = [long][BitConverter]::ToUInt32($Bytes,$reloc+4)
        if ([BitConverter]::ToUInt32($Bytes,$reloc) -ne 1 -or $relocSize -lt 12 -or $relocOffset+8+$relocSize -gt $section.size) { throw $invalid }
        $cursor = $reloc+8; $end = $cursor+$relocSize; $arm64x = $false
        while ($cursor -lt $end) {
            if ($cursor+12 -gt $end) { throw $invalid }
            $symbol = [BitConverter]::ToUInt64($Bytes,$cursor)
            $fixups = [long][BitConverter]::ToUInt32($Bytes,$cursor+8)
            if ($cursor+12+$fixups -gt $end) { throw $invalid }
            if ($symbol -eq 6 -and $fixups -gt 0) { $arm64x=$true }
            $cursor += 12+$fixups
        }
        if (-not $arm64x) { throw $invalid }
    }
    $records = @{}; $cursor=$metadataSection.raw; $end=$cursor+$metadataSection.size
    while ($cursor -lt $end) {
        if ($Bytes[$cursor] -eq 0) { ++$cursor; continue }
        $terminator = [Array]::IndexOf($Bytes,[byte]0,[int]$cursor,[int]($end-$cursor))
        if ($terminator -lt 0 -or $terminator-$cursor -gt 4096) { throw $invalid }
        $json = [Text.UTF8Encoding]::new($false,$true).GetString($Bytes,$cursor,$terminator-$cursor)
        $document = [Text.Json.JsonDocument]::Parse($json)
        try { Assert-Arm64JsonDuplicates $document.RootElement } finally { $document.Dispose() }
        $record = $json | ConvertFrom-Json -AsHashtable
        $keys = @('format_version','source_revision','source_sha256','source_modified','configuration','compiler','variant',
            'cmake_cache_sha256','llama_revision','llama_source_sha256','binary')
        if ($group -eq 'native') { $keys += 'llama_backend' }
        Assert-Arm64JsonKeys $record $keys
        foreach ($key in $keys | Where-Object { $_ -notin @('format_version','source_modified','llama_backend') }) {
            if ($group -ne 'native' -and $key -in @('llama_revision','llama_source_sha256')) { continue }
            if ($record[$key] -isnot [string]) { throw $invalid }
        }
        $variants = switch ($group) { native {@('zenz-cpu')} arm64x {@('arm64x-native','arm64x-ec')} probe {@('arm64x-native')} x86 {@('x86-companion')} }
        if ($record.format_version -isnot [long] -or $record.format_version -ne 1 -or $record.source_modified -isnot [bool] -or
            $record.source_revision -isnot [string] -or $record.source_revision -cnotmatch '^([a-f0-9]{40}|[a-f0-9]{64})$' -or
            $record.source_sha256 -isnot [string] -or $record.source_sha256 -cnotmatch '^[a-f0-9]{64}$' -or
            $record.cmake_cache_sha256 -isnot [string] -or $record.cmake_cache_sha256 -cnotmatch '^[a-f0-9]{64}$' -or
            $record.configuration -cne 'Release' -or $record.compiler -cnotmatch '^MSVC [0-9]+\.[0-9]+\.[0-9]+(\.[0-9]+)?$' -or
            $record.variant -cnotin $variants -or $record.binary -cne [IO.Path]::GetFileName($Name) -or $records.ContainsKey($record.variant)) { throw $invalid }
        if ($group -eq 'native') {
            if ($record.llama_revision -cne '8345f333951c661d166b00e6f9362e553768f292' -or $record.llama_source_sha256 -cnotmatch '^[a-f0-9]{64}$') { throw $invalid }
            $backend = $record.llama_backend
            Assert-Arm64JsonKeys $backend @('compiler','configuration','cpu_contract','cmake_cache_sha256','libraries_sha256')
            foreach ($key in @('compiler','configuration','cpu_contract','cmake_cache_sha256')) { if ($backend[$key] -isnot [string]) { throw $invalid } }
            if ($backend.compiler -cnotmatch '^Clang [0-9]+\.[0-9]+\.[0-9]+$' -or $backend.configuration -cne 'Release' -or
                $backend.cpu_contract -cne 'zenz-arm64-armv8a-v1' -or $backend.cmake_cache_sha256 -cnotmatch '^[a-f0-9]{64}$') { throw $invalid }
            Assert-Arm64JsonKeys $backend.libraries_sha256 @('llama','ggml','ggml-base','ggml-cpu')
            foreach ($hash in $backend.libraries_sha256.Values) { if ($hash -isnot [string] -or $hash -cnotmatch '^[a-f0-9]{64}$') { throw $invalid } }
        } elseif ($null -ne $record.llama_revision -or $null -ne $record.llama_source_sha256) { throw $invalid }
        $null = $record.Remove('binary'); $records[$record.variant]=$record
        $cursor=$terminator+1
    }
    if ($records.Count -ne $(if($group -eq 'arm64x'){2}else{1})) { throw $invalid }
    if ($group -eq 'arm64x') { return $records }
    return @($records.Values)[0]
}
function Assert-NskkzimeArm64BuildSet($Groups) {
    foreach ($group in @('native','arm64x','probe','x86')) { if (-not $Groups.ContainsKey($group)) { throw 'Incomplete ARM64 package build set.' } }
    foreach ($metadata in @($Groups.arm64x['arm64x-native'],$Groups.arm64x['arm64x-ec'],$Groups.probe,$Groups.x86)) {
        foreach ($key in @('source_revision','source_sha256','source_modified','configuration','compiler')) {
            if ($Groups.native[$key] -cne $metadata[$key]) { throw 'ARM64 package build sources differ; rebuild all targets.' }
        }
    }
    if ((Get-NskkzimeArm64MetadataIdentity $Groups.probe) -cne (Get-NskkzimeArm64MetadataIdentity $Groups.arm64x['arm64x-native'])) {
        throw 'ARM64 probe and native TSF build metadata differ.'
    }
}
function Get-NskkzimeArm64MetadataIdentity($Value) {
    function Order-Value($Item) {
        if ($Item -is [Collections.IDictionary]) {
            $result=[ordered]@{}
            foreach ($key in @($Item.Keys | Sort-Object -CaseSensitive)) { $result[$key]=Order-Value $Item[$key] }
            return $result
        }
        if ($Item -is [pscustomobject]) {
            $result=[ordered]@{}
            foreach ($property in @($Item.PSObject.Properties | Sort-Object Name -CaseSensitive)) { $result[$property.Name]=Order-Value $property.Value }
            return $result
        }
        return $Item
    }
    ConvertTo-Json -InputObject (Order-Value $Value) -Compress -Depth 8
}
Export-ModuleMember -Function Get-NskkzimeArm64MetadataGroup, Get-NskkzimeArm64ImageMetadata, Assert-NskkzimeArm64BuildSet, Get-NskkzimeArm64MetadataIdentity
