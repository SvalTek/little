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
$packageStage = Join-Path $repo "build/package-$Target"

if (!(Test-Path $binarySource)) {
    throw "Expected built CLI at $binarySource"
}

foreach ($library in @("json", "webui")) {
    $source = Join-Path $repo "nativelib/$library/build/$library$nativeExt"
    if (!(Test-Path $source)) {
        throw "Expected built native library at $source"
    }
}

if (Test-Path $packageStage) {
    Remove-Item -Recurse -Force -LiteralPath $packageStage
}
New-Item -ItemType Directory -Force -Path $packageStage, $dist | Out-Null
Copy-Item -LiteralPath $binarySource -Destination (Join-Path $packageStage "little$binaryExt")

foreach ($library in @("json", "webui")) {
    $libraryStage = Join-Path $packageStage "libs/$library"
    New-Item -ItemType Directory -Force -Path $libraryStage | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "nativelib/$library/build/$library$nativeExt") -Destination (Join-Path $libraryStage "$library$nativeExt")
}

$packageZip = Join-Path $dist "little-$Target.zip"
Remove-Item -Force -LiteralPath $packageZip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $packageStage "*") -DestinationPath $packageZip

Write-Host "Created $packageZip"
