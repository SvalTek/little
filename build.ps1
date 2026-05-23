param(
    [string]$Compiler = "gcc",
    [string]$Output = "build/little.exe"
)

$ErrorActionPreference = "Stop"

$repo = $PSScriptRoot
$outPath = Join-Path $repo $Output
$outDir = Split-Path -Parent $outPath

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

& $Compiler -std=c11 `
    (Join-Path $repo "main.c") `
    (Join-Path $repo "src/little.c") `
    (Join-Path $repo "src/little_std.c") `
    -lm -o $outPath

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $Output"
