param(
    [string]$Output = "build",
    [string]$GccPath = $env:GCC_PATH
)

$ErrorActionPreference = "Stop"
if (!$GccPath) { throw "GCC_PATH is required to stage Windows runtime DLLs" }
$outputPath = Join-Path (Split-Path -Parent $PSScriptRoot) $Output
New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
$objdump = Join-Path $GccPath "bin/objdump.exe"
if (!(Test-Path $objdump)) { throw "Expected objdump at $objdump" }

$queue = [Collections.Queue]::new()
$seen = @{}
foreach ($name in @("libcurl-4.dll", "libssl-3-x64.dll", "libcrypto-3-x64.dll")) {
    $source = Join-Path $GccPath "bin/$name"
    if (!(Test-Path $source)) { throw "Expected runtime DLL $name in $GccPath/bin" }
    $seen[$name.ToLowerInvariant()] = $true
    $queue.Enqueue($source)
}

while ($queue.Count -gt 0) {
    $source = $queue.Dequeue()
    Copy-Item -LiteralPath $source -Destination $outputPath -Force
    $dependencies = & $objdump -p $source | Select-String -Pattern 'DLL Name:\s*(.+)$'
    foreach ($line in $dependencies) {
        $name = $line.Matches[0].Groups[1].Value.Trim()
        $key = $name.ToLowerInvariant()
        if ($seen.ContainsKey($key)) { continue }
        $dependency = Join-Path $GccPath "bin/$name"
        if (!(Test-Path $dependency)) { continue }
        $seen[$key] = $true
        $queue.Enqueue($dependency)
    }
}
