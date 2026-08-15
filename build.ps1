param(
    [string]$Compiler = "",
    [string]$Output = "build/little.exe",
    [string]$CFlags = "",
    [string]$LdFlags = ""
)

$ErrorActionPreference = "Stop"

$repo = $PSScriptRoot
$toolchainBin = if ($env:GCC_PATH) { Join-Path $env:GCC_PATH "bin" } else { "" }
if ($toolchainBin -and (Test-Path $toolchainBin)) {
    $env:PATH = "$toolchainBin$([IO.Path]::PathSeparator)$env:PATH"
}
$Compiler = if ($Compiler) { $Compiler } elseif ($env:GCC_PATH) { Join-Path $env:GCC_PATH "bin/gcc.exe" } else { "gcc" }
$includeFlags = if ($env:INCLUDES_PATH) { @("-I", $env:INCLUDES_PATH) } else { @() }
$outPath = Join-Path $repo $Output
$outDir = Split-Path -Parent $outPath
$threadFlags = @()
$dynamicFlags = @()
if ($env:OS -ne "Windows_NT") {
    $threadFlags += "-pthread"
    if (-not $IsMacOS) {
        $dynamicFlags += "-rdynamic"
        $dynamicFlags += "-ldl"
    }
}
else {
    $dynamicFlags += "-Wl,--export-all-symbols"
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
    $includeFlags `
    (Join-Path $repo "main.c") `
    (Join-Path $repo "src/little_buffer.c") `
    (Join-Path $repo "src/little.c") `
    (Join-Path $repo "src/little_common.c") `
    (Join-Path $repo "src/little_std.c") `
    (Join-Path $repo "src/little_loadlib.c") `
    (Join-Path $repo "src/little_std_io.c") `
    (Join-Path $repo "src/little_std_math.c") `
    (Join-Path $repo "src/little_std_array.c") `
    (Join-Path $repo "src/little_std_table.c") `
    (Join-Path $repo "src/little_std_string.c") `
    (Join-Path $repo "src/little_std_gc.c") `
    (Join-Path $repo "src/little_async.c") `
    $threadFlags `
    $dynamicFlags `
    -lm `
    $extraLdFlags `
    -o $outPath

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $Output"
