param(
    [Parameter(Mandatory = $true)]
    [string]$Exe
)

$ErrorActionPreference = "Stop"

function Normalize([string]$Text) {
    return ($Text -replace "`r`n", "`n").TrimEnd()
}

function Assert-Run([string]$Name, [string]$Expected, [scriptblock]$Command) {
    $actual = Normalize((& $Command 2>&1 | Out-String))
    if ($LASTEXITCODE -ne 0 -or $actual -ne $Expected) {
        throw "$Name failed.`nExpected:`n$Expected`nActual:`n$actual"
    }
}

function Assert-Fails([string]$Name, [int]$ExpectedExit, [string]$ExpectedText, [scriptblock]$Command) {
    $actual = Normalize((& $Command 2>&1 | Out-String))
    if ($LASTEXITCODE -ne $ExpectedExit -or !$actual.Contains($ExpectedText)) {
        throw "$Name failed.`nExpected exit: $ExpectedExit`nExpected text: $ExpectedText`nActual exit: $LASTEXITCODE`nActual:`n$actual"
    }
}

$repo = Split-Path -Parent $PSScriptRoot
$build = Join-Path $repo "build"
$nativeExt = if ($env:OS -eq "Windows_NT") { ".dll" } elseif ($IsMacOS) { ".dylib" } else { ".so" }
$nativeMath = Join-Path $repo "nativelib/native_math/build/native_math$nativeExt"
if (!(Test-Path $nativeMath)) {
    throw "Expected native test library at $nativeMath"
}

Assert-Fails "interactive source conflict" 2 "Usage: little" { & $Exe --no-config -i -e 'io.print("no")' }

$script = Join-Path $build "cli-args.little"
Set-Content -LiteralPath $script -NoNewline -Value @'
io.print(arg[0])
io.print(arg[1])
io.print(arg[2])
'@

Assert-Run "script arguments" "${script}`nfirst`nsecond" { & $Exe --no-config $script first second }

$nativeScript = Join-Path $build "cli-native.little"
Set-Content -LiteralPath $nativeScript -NoNewline -Value @'
var add = loadLibrary("native_math")
io.print(add(2, 3))
'@

Assert-Run "native library path" "5.000000" { & $Exe --no-config -L (Split-Path -Parent $nativeMath) $nativeScript }

$config = Join-Path $build "little-cli-test.conf"
Set-Content -LiteralPath $config -NoNewline -Value "library_path = $(Split-Path -Parent $nativeMath)`nrepl_echo = true"
Assert-Run "configured native library path" "5.000000" { & $Exe --config $config $nativeScript }

$portable = Join-Path $build "cli-portable"
$portableLibraries = Join-Path $portable "libs/native_math"
New-Item -ItemType Directory -Force -Path $portableLibraries | Out-Null
Copy-Item -LiteralPath $Exe -Destination (Join-Path $portable "little.exe") -Force
Copy-Item -LiteralPath $nativeMath -Destination (Join-Path $portableLibraries "native_math$nativeExt") -Force
$portableScript = Join-Path $build "cli-portable-native.little"
Set-Content -LiteralPath $portableScript -NoNewline -Value @'
var add = loadLibrary("native_math")
io.print(add(2, 3))
'@
Assert-Run "portable native library path" "5.000000" { & (Join-Path $portable "little.exe") --no-config $portableScript }

$prefix = Join-Path $build "cli-prefix"
$prefixBin = Join-Path $prefix "bin"
$prefixLibraries = Join-Path $prefix "lib/little/native_math"
New-Item -ItemType Directory -Force -Path $prefixBin, $prefixLibraries | Out-Null
Copy-Item -LiteralPath $Exe -Destination (Join-Path $prefixBin "little.exe") -Force
Copy-Item -LiteralPath $nativeMath -Destination (Join-Path $prefixLibraries "native_math$nativeExt") -Force
Assert-Run "installed native library path" "5.000000" { & (Join-Path $prefixBin "little.exe") --no-config $portableScript }

Write-Host "All CLI tests passed."
