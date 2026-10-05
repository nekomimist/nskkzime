#requires -Version 7.0
param([switch]$SkipModel)
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$revision = '66afb885a8ddcc0511798aec9fb5a01017941018'
$archiveHash = 'df8e483c6bb30e358389f6be382a63f14bd8648d327b4994fc2f0b1df63ff97b'
$modelRevision = 'c67e03e07d215c869f591b274c1631170d3e11fe'
$modelHash = '29c223d4c23327b80fd13ebb5ab2555057a46317997d5da391584ffbef0db673'
$dependencies = Join-Path $repoRoot 'out/deps'
$archive = Join-Path $dependencies 'llama-66afb88.zip'
$source = Join-Path $dependencies "llama.cpp-$revision"
function Assert-Checksum([string]$Path, [string]$Expected) {
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Expected) {
        throw "Checksum mismatch: $Path. The file was not accepted; inspect it before retrying."
    }
}
New-Item -ItemType Directory -Path $dependencies -Force | Out-Null
if (-not (Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri "https://codeload.github.com/azooKey/llama.cpp/zip/$revision" -OutFile $archive
}
Assert-Checksum $archive $archiveHash
if (-not (Test-Path -LiteralPath $source)) {
    Expand-Archive -LiteralPath $archive -DestinationPath $dependencies
}
if (-not (Test-Path -LiteralPath (Join-Path $source 'CMakeLists.txt'))) { throw 'llama.cpp extraction is incomplete.' }
Write-Host "llama.cpp archive verified: $revision"
if (-not $SkipModel) {
    $modelDirectory = Join-Path $repoRoot 'out/models/zenz-v3.2-small'
    $modelFile = Join-Path $modelDirectory 'ggml-model-Q5_K_M.gguf'
    if (-not (Test-Path -LiteralPath $modelFile)) {
        if (-not (Get-Command hf -ErrorAction SilentlyContinue)) { throw 'Install the Hugging Face hf CLI to download the model.' }
        $settings = @{
            HF_HOME = (Join-Path $repoRoot 'out/huggingface')
            HF_HUB_DISABLE_IMPLICIT_TOKEN = '1'
            HF_HUB_DISABLE_TELEMETRY = '1'
            HF_HUB_DISABLE_XET = '1'
        }
        $previous = @{}
        try {
            foreach ($name in $settings.Keys) {
                $previous[$name] = [Environment]::GetEnvironmentVariable($name,'Process')
                [Environment]::SetEnvironmentVariable($name,$settings[$name],'Process')
            }
            & hf download Miwa-Keita/zenz-v3.2-small-gguf ggml-model-Q5_K_M.gguf README.md --revision $modelRevision --local-dir $modelDirectory --quiet
            if ($LASTEXITCODE -ne 0) { throw 'Model download failed.' }
        } finally {
            foreach ($name in $previous.Keys) { [Environment]::SetEnvironmentVariable($name,$previous[$name],'Process') }
        }
    }
    Assert-Checksum $modelFile $modelHash
    Write-Host "zenz model verified: $modelFile"
}
