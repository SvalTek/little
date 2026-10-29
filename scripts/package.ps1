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

foreach ($library in @("webui")) {
    $source = Join-Path $repo "nativelib/$library/build/$library$nativeExt"
    if (!(Test-Path $source)) {
        throw "Expected built native library at $source"
    }
}

$runtimeDlls = @()
if ($env:OS -eq "Windows_NT") {
    $gccPath = $env:GCC_PATH
    if (!$gccPath) {
        $runtimeDlls = @(Get-ChildItem -Path (Join-Path $repo "build") -Filter "*.dll" -File | ForEach-Object FullName)
        if (!($runtimeDlls | Where-Object { (Split-Path $_ -Leaf) -eq "libcurl-4.dll" })) {
            throw "GCC_PATH is required to collect Windows runtime DLLs"
        }
    }
    else {
    $objdump = Join-Path $gccPath "bin/objdump.exe"
    if (!(Test-Path $objdump)) { throw "Expected objdump at $objdump" }
    $seen = @{}
    $queue = [Collections.Queue]::new()
    foreach ($name in @("libcurl-4.dll", "libssl-3-x64.dll", "libcrypto-3-x64.dll")) {
        $source = Join-Path $repo "build/$name"
        if (!(Test-Path $source)) { $source = Join-Path $gccPath "bin/$name" }
        if (!(Test-Path $source)) { throw "Expected runtime DLL $name in build/ or GCC_PATH/bin" }
        $seen[$name.ToLowerInvariant()] = $true
        $queue.Enqueue($source)
    }
    while ($queue.Count -gt 0) {
        $source = $queue.Dequeue()
        $runtimeDlls += $source
        $dependencies = & $objdump -p $source | Select-String -Pattern 'DLL Name:\s*(.+)$'
        foreach ($line in $dependencies) {
            $name = $line.Matches[0].Groups[1].Value.Trim()
            $key = $name.ToLowerInvariant()
            if ($seen.ContainsKey($key)) { continue }
            $dependency = Join-Path $gccPath "bin/$name"
            if (!(Test-Path $dependency)) { $dependency = Join-Path $repo "build/$name" }
            if (!(Test-Path $dependency)) { continue }
            $seen[$key] = $true
            $queue.Enqueue($dependency)
        }
    }
    }
}

if (Test-Path $packageStage) {
    Remove-Item -Recurse -Force -LiteralPath $packageStage
}
New-Item -ItemType Directory -Force -Path $packageStage, $dist | Out-Null
Copy-Item -LiteralPath $binarySource -Destination (Join-Path $packageStage "little$binaryExt")
foreach ($dll in $runtimeDlls) {
    Copy-Item -LiteralPath $dll -Destination $packageStage
}

foreach ($library in @("webui")) {
    $libraryStage = Join-Path $packageStage "libs/$library"
    New-Item -ItemType Directory -Force -Path $libraryStage | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo "nativelib/$library/build/$library$nativeExt") -Destination (Join-Path $libraryStage "$library$nativeExt")
}

$packageZip = Join-Path $dist "little-$Target.zip"
Remove-Item -Force -LiteralPath $packageZip -ErrorAction SilentlyContinue
Compress-Archive -Path (Join-Path $packageStage "*") -DestinationPath $packageZip

Write-Host "Created $packageZip"
