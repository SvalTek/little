param(
    [string]$Compiler = "gcc",
    [string]$Output = "build/little.exe",
    [string]$CFlags = "",
    [string]$LdFlags = ""
)

$ErrorActionPreference = "Stop"

$repo = $PSScriptRoot
$outPath = Join-Path $repo $Output
$outDir = Split-Path -Parent $outPath
$threadFlags = @()
if ($env:OS -ne "Windows_NT") {
    $threadFlags += "-pthread"
}
$extraCFlags = @()
if ($CFlags.Trim().Length -gt 0) {
    $extraCFlags = $CFlags -split '\s+'
}
$extraLdFlags = @()
if ($LdFlags.Trim().Length -gt 0) {
    $extraLdFlags = $LdFlags -split '\s+'
}

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

& $Compiler -std=c11 `
    $extraCFlags `
    (Join-Path $repo "main.c") `
    (Join-Path $repo "src/little_buffer.c") `
    (Join-Path $repo "src/little.c") `
    (Join-Path $repo "src/little_std.c") `
    (Join-Path $repo "src/little_std_io.c") `
    (Join-Path $repo "src/little_std_math.c") `
    (Join-Path $repo "src/little_std_array.c") `
    (Join-Path $repo "src/little_std_table.c") `
    (Join-Path $repo "src/little_std_string.c") `
    (Join-Path $repo "src/little_std_gc.c") `
    (Join-Path $repo "src/little_async.c") `
    $threadFlags `
    -lm `
    $extraLdFlags `
    -o $outPath

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $Output"
