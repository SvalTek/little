param(
    [string]$Exe = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$little = if ($Exe) { $Exe } else { Join-Path $repo "build/little.exe" }

if (!(Test-Path $little)) {
    & (Join-Path $repo "build.ps1")
}

$examples = Get-ChildItem -Path $PSScriptRoot -Filter "*.little" | Sort-Object Name

foreach ($example in $examples) {
    Write-Host ""
    Write-Host "== $($example.Name) =="
    & $little $example.FullName
    if ($LASTEXITCODE -ne 0) {
        throw "Example failed: $($example.Name)"
    }
}
