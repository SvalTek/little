param(
    [string]$Compiler = "gcc",
    [string]$Exe = "",
    [switch]$SkipBuild
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $repo "build"
$exe = if ($Exe) { $Exe } else { Join-Path $buildDir "little-e2e.exe" }

if (!$SkipBuild) {
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null
    $threadFlags = @()
    if ($env:OS -ne "Windows_NT") {
        $threadFlags += "-pthread"
    }

    & $Compiler -std=c11 `
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
        -lm -o $exe

    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE"
    }
}

function Normalize([string]$Text) {
    return ($Text -replace "`r`n", "`n").TrimEnd()
}

$failed = 0
$tests = Get-ChildItem -Path (Join-Path $PSScriptRoot "e2e") -Filter "*.little" | Sort-Object Name

foreach ($test in $tests) {
    $testFailed = $false
    $output = Normalize((& $exe $test.FullName 2>&1 | Out-String))
    $expectedPath = "$($test.FullName).expected"
    $containsPath = "$($test.FullName).contains"

    if (Test-Path $expectedPath) {
        $expected = Normalize((Get-Content -Raw $expectedPath))
        if ($output -ne $expected) {
            Write-Host "FAIL $($test.Name)"
            Write-Host "Expected:"
            Write-Host $expected
            Write-Host "Actual:"
            Write-Host $output
            $failed++
            $testFailed = $true
            continue
        }
    }
    elseif (Test-Path $containsPath) {
        $needles = Get-Content $containsPath | Where-Object { $_.Trim().Length -gt 0 }
        foreach ($needle in $needles) {
            if (!$output.Contains($needle)) {
                Write-Host "FAIL $($test.Name)"
                Write-Host "Missing expected text: $needle"
                Write-Host "Actual:"
                Write-Host $output
                $failed++
                $testFailed = $true
                break
            }
        }
        if ($testFailed) { continue }
    }
    else {
        Write-Host "FAIL $($test.Name)"
        Write-Host "Missing .expected or .contains file"
        $failed++
        continue
    }

    Write-Host "PASS $($test.Name)"
}

if ($failed -gt 0) {
    throw "$failed e2e test(s) failed"
}

Write-Host "All e2e tests passed."
