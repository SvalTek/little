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
    $msysRoot = Split-Path (Split-Path $toolchainBin -Parent) -Parent
    $msysBin = Join-Path $msysRoot "usr/bin"
    if (Test-Path (Join-Path $msysBin "cp.exe")) {
        $env:PATH = "$env:PATH$([IO.Path]::PathSeparator)$msysBin"
    }
}
$Compiler = if ($Compiler) { $Compiler } elseif ($env:GCC_PATH) { Join-Path $env:GCC_PATH "bin/gcc.exe" } else { "gcc" }
$includeFlags = if ($env:INCLUDES_PATH) { @("-I", $env:INCLUDES_PATH) } else { @() }
$platformCFlags = if ($env:OS -eq "Windows_NT") { @() } else { @("-D_XOPEN_SOURCE=700") }
$outPath = Join-Path $repo $Output
$outDir = Split-Path -Parent $outPath
$minizName = [IO.Path]::GetFileNameWithoutExtension($Output)
$minizOutput = "build/miniz-$minizName"
$minizDir = Join-Path $repo $minizOutput
& (Join-Path $repo "scripts/prepare-miniz.ps1") -Output $minizOutput
if (-not $?) { throw "miniz preparation failed" }
$minizFlags = @("-I", $minizDir)
$minizSources = @(
    (Join-Path $minizDir "miniz.c"),
    (Join-Path $minizDir "miniz_zip.c"),
    (Join-Path $minizDir "miniz_tinfl.c"),
    (Join-Path $minizDir "miniz_tdef.c")
)
. (Join-Path $repo "scripts/sources.ps1")
$littleSources = @($LittleCliExecutableSources | ForEach-Object { Join-Path $repo $_ })
$vendorSources = @($LittleVendorSources | ForEach-Object { Join-Path $repo $_ })
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

$terminalVendor = Join-Path $repo "vendor/pdcursesmod"
$terminalPort = if ($env:OS -eq "Windows_NT") { "wincon" } else { "vt" }
$terminalDir = Join-Path $terminalVendor $terminalPort
$make = if ($toolchainBin -and (Test-Path (Join-Path $toolchainBin "make.exe"))) {
    Join-Path $toolchainBin "make.exe"
} elseif ($toolchainBin -and (Test-Path (Join-Path $toolchainBin "mingw32-make.exe"))) {
    Join-Path $toolchainBin "mingw32-make.exe"
} else {
    "make"
}
if (!(Test-Path $terminalDir)) { throw "Expected terminal backend at $terminalDir" }
$makeArgs = @("-C", $terminalDir, "PDCURSES_SRCDIR=..", "WIDE=Y", "UTF8=Y")
if ($env:OS -eq "Windows_NT") { $makeArgs += "PREFIX=" }
& $make $makeArgs
if ($LASTEXITCODE -ne 0) { throw "Terminal backend build failed with exit code $LASTEXITCODE" }
$terminalLibName = if ($env:OS -eq "Windows_NT") { "pdcurses.a" } else { "libpdcurses.a" }
$terminalLib = Join-Path $terminalDir $terminalLibName
$terminalFlags = if ($env:OS -eq "Windows_NT") { @("-lwinmm") } else { @() }
$socketFlags = if ($env:OS -eq "Windows_NT") { @("-lws2_32", "-lcrypt32") } else { @() }

New-Item -ItemType Directory -Force -Path $outDir | Out-Null

& $Compiler -std=c11 `
    $extraCFlags `
    $platformCFlags `
    $includeFlags `
    -I (Join-Path $repo "src") `
    -I (Join-Path $repo "vendor/llhttp") `
    -I (Join-Path $repo "vendor/pcg") `
    -I (Join-Path $repo "vendor/wslay/lib") `
    -I (Join-Path $repo "vendor/wslay/lib/includes") `
    -I (Join-Path $repo "vendor/yyjson") `
    -DYYJSON_READER_DEPTH_LIMIT=64 `
    -DYYJSON_WRITER_DEPTH_LIMIT=64 `
    $minizFlags `
    $littleSources `
    $vendorSources `
    $minizSources `
    -I $terminalVendor `
    -DPDC_FORCE_UTF8 `
    $threadFlags `
    $dynamicFlags `
    $terminalLib `
    $terminalFlags `
    -lssl -lcrypto -lcurl `
    $socketFlags `
    -lm `
    $extraLdFlags `
    -o $outPath

if ($LASTEXITCODE -ne 0) {
    throw "Build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $Output"
