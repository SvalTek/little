param([string]$Compiler = "")

$ErrorActionPreference = "Stop"
$repo = Split-Path -Parent $PSScriptRoot
if ($env:GCC_PATH) {
    $env:PATH = "$(Join-Path $env:GCC_PATH 'bin')$([IO.Path]::PathSeparator)$env:PATH"
}
$Compiler = if ($Compiler) { $Compiler } else { "gcc" }
. (Join-Path $repo "scripts/sources.ps1")
$objectDir = Join-Path $repo "build/core"
New-Item -ItemType Directory -Force -Path $objectDir | Out-Null
$objects = @()
$platformFlags = if ($env:OS -eq "Windows_NT") { @() } else { @("-D_XOPEN_SOURCE=700", "-pthread") }
foreach ($source in $LittleCoreSources) {
    $object = Join-Path $objectDir ([IO.Path]::GetFileNameWithoutExtension($source) + ".o")
    & $Compiler -std=c11 $platformFlags -I (Join-Path $repo "src") -c (Join-Path $repo $source) -o $object
    if ($LASTEXITCODE -ne 0) { throw "Core compilation failed: $source" }
    $objects += $object
}
$archive = Join-Path $repo "build/liblittle.a"
if (Test-Path $archive) { Remove-Item -LiteralPath $archive }
& ar rcs $archive $objects
if ($LASTEXITCODE -ne 0) { throw "Core archive creation failed" }
Write-Host "Built $archive"
