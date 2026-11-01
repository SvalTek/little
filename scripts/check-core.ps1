param([string]$Compiler = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ($env:GCC_PATH) {
    $env:PATH = "$(Join-Path $env:GCC_PATH 'bin')$([IO.Path]::PathSeparator)$env:PATH"
}
$Compiler = if ($Compiler) { $Compiler } else { "gcc" }
$include = Join-Path $repo "src"
foreach ($header in @("little.h", "little_value.h", "little_frontend.h", "little_api.h", "little_vm_layout.h", "little_std.h", "little_async.h", "little_bundle.h", "little_bundle_build.h", "little_loadlib.h")) {
    "#include `"$header`"" | & $Compiler -std=c11 -I $include -x c -fsyntax-only -
    if ($LASTEXITCODE -ne 0) { throw "Header is not self-contained: $header" }
}
$exe = Join-Path $repo "build/core-only.exe"
$platformFlags = if ($env:OS -eq "Windows_NT") { @() } else { @("-pthread") }
& $Compiler -std=c11 -I $include (Join-Path $repo "tests/native/core-only.c") (Join-Path $repo "build/liblittle.a") $platformFlags -lm -o $exe
if ($LASTEXITCODE -ne 0) { throw "Core-only test link failed" }
& $exe
if ($LASTEXITCODE -ne 0) { throw "Core-only test failed with exit code $LASTEXITCODE" }
Write-Host "Core-only test and public header checks passed"
