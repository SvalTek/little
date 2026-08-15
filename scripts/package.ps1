param(
    [Parameter(Mandatory = $true)]
    [ValidatePattern('^[a-z0-9][a-z0-9-]*$')]
    [string]$Target,
    [string]$Binary = "build/little.exe"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$nativeExt = if ($env:OS -eq "Windows_NT") { ".dll" } elseif ($IsMacOS) { ".dylib" } else { ".so" }
$binaryExt = if ($env:OS -eq "Windows_NT") { ".exe" } else { "" }
$binarySource = Join-Path $repo $Binary
$dist = Join-Path $repo "dist"
$nativeStage = Join-Path $repo "build/nativelibs-$Target"

if (!(Test-Path $binarySource)) {
    throw "Expected built CLI at $binarySource"
}

foreach ($library in @("json", "webui")) {
    $source = Join-Path $repo "nativelib/$library/build/$library$nativeExt"
    if (!(Test-Path $source)) {
        throw "Expected built native library at $source"
    }
}

if (Test-Path $nativeStage) {
    Remove-Item -Recurse -Force -LiteralPath $nativeStage
}
New-Item -ItemType Directory -Force -Path $nativeStage, $dist | Out-Null
Copy-Item -LiteralPath $binarySource -Destination (Join-Path $dist "little-$Target$binaryExt")

foreach ($library in @("json", "webui")) {
    $libraryStage = Join-Path $nativeStage $library
    New-Item -ItemType Directory -Force -Path $libraryStage | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "nativelib/$library/build/$library$nativeExt") -Destination (Join-Path $libraryStage "$library$nativeExt")
}

$nativeZip = Join-Path $dist "nativelibs-$Target.zip"
Remove-Item -Force -LiteralPath $nativeZip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $nativeStage "*") -DestinationPath $nativeZip

Write-Host "Created $(Join-Path $dist "little-$Target$binaryExt")"
Write-Host "Created $nativeZip"
