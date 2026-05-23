param(
    [string]$Exe = "build/little.exe"
)

$ErrorActionPreference = "Stop"

$repo = $PSScriptRoot
$exePath = Join-Path $repo $Exe

if (!(Test-Path $exePath)) {
    & (Join-Path $repo "build.ps1") -Output $Exe
}

& (Join-Path $repo "tests/run-e2e.ps1") -Exe $exePath -SkipBuild
