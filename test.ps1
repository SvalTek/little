param(
    [string]$Exe = "build/little.exe"
)

$ErrorActionPreference = "Stop"

$repo = $PSScriptRoot
$exePath = Join-Path $repo $Exe

if (!(Test-Path $exePath)) {
    & (Join-Path $repo "build.ps1") -Output $Exe
}

& (Join-Path $repo "tests/run-e2e.ps1") -Exe $exePath
& (Join-Path $repo "tests/run-cli.ps1") -Exe $exePath

$node = Get-Command node -ErrorAction SilentlyContinue
if ($node) {
    $checkDir = Join-Path $repo "build/std-docs-check"
    if (Test-Path $checkDir) { Remove-Item -Recurse -Force $checkDir }
    & node (Join-Path $repo "scripts/generate-std-docs.mjs") "-o" $checkDir
    if ($LASTEXITCODE -ne 0) { throw "std doc generation failed with exit code $LASTEXITCODE" }

    foreach ($file in Get-ChildItem -Path $checkDir -Filter *.md) {
        $committed = Join-Path (Join-Path $repo "doc/std") $file.Name
        if (!(Test-Path $committed)) {
            throw "doc/std/$($file.Name) is missing; run scripts/generate-std-docs.mjs"
        }
        # Compare content, not checkout line endings (autocrlf vs generated LF).
        $expected = (Get-Content -Raw $committed) -replace "`r`n", "`n"
        $actual = (Get-Content -Raw $file.FullName) -replace "`r`n", "`n"
        if ($expected -ne $actual) {
            throw "doc/std/$($file.Name) is stale; run scripts/generate-std-docs.mjs"
        }
    }
    foreach ($file in Get-ChildItem -Path (Join-Path $repo "doc/std") -Filter *.md) {
        if (!(Test-Path (Join-Path $checkDir $file.Name))) {
            throw "doc/std/$($file.Name) is not generated; remove it or document a binding"
        }
    }
}
else {
    Write-Host "node not found; skipping std doc freshness check"
}
