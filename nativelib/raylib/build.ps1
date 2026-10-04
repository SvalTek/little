param(
    [Parameter(Mandatory = $true)]
    [string]$Compiler,
    [string[]]$IncludeFlags = @()
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$raylibVendor = Join-Path $repo "vendor/raylib/src"
$nativeExt = if ($env:OS -eq "Windows_NT") { ".dll" } elseif ($IsMacOS) { ".dylib" } else { ".so" }
$outputDir = Join-Path $PSScriptRoot "build"
$output = Join-Path $outputDir "raylib$nativeExt"

if (!(Test-Path $raylibVendor)) {
    throw "vendor/raylib is required to build the raylib native library; initialize submodules with 'git submodule update --init --recursive'."
}

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null

$sharedLibraryFlag = if ($IsMacOS) { "-dynamiclib" } else { "-shared" }
$sources = @(
    (Join-Path $PSScriptRoot "raylib_little.c"),
    (Join-Path $raylibVendor "rcore.c"),
    (Join-Path $raylibVendor "rshapes.c"),
    (Join-Path $raylibVendor "rtextures.c"),
    (Join-Path $raylibVendor "rtext.c"),
    (Join-Path $raylibVendor "rglfw.c"),
    (Join-Path $raylibVendor "rmodels.c"),
    (Join-Path $raylibVendor "raudio.c")
)
$defines = @("-DPLATFORM_DESKTOP", "-DGRAPHICS_API_OPENGL_33", "-DUNICODE", "-D_GNU_SOURCE")
if ($env:OS -ne "Windows_NT" -and -not $IsMacOS) {
    # GLFW's bundled sources need an explicit platform outside Windows and macOS.
    $defines += "-D_GLFW_X11"
}
$flags = @("-std=c99", $sharedLibraryFlag) + $IncludeFlags + @(
    "-I", (Join-Path $repo "src"),
    "-I", $raylibVendor,
    "-I", (Join-Path $raylibVendor "external/glfw/include")
) + $defines
if ($env:OS -ne "Windows_NT") {
    $flags += "-fPIC"
}

if ($env:OS -eq "Windows_NT") {
    $libs = @("-lopengl32", "-lgdi32", "-lwinmm", "-lshell32")
}
elseif ($IsMacOS) {
    $libs = @("-framework", "OpenGL", "-framework", "Cocoa", "-framework", "IOKit", "-framework", "CoreAudio", "-framework", "CoreVideo")
}
else {
    $libs = @("-lGL", "-lm", "-lpthread", "-ldl", "-lrt", "-lX11")
}

& $Compiler $flags $sources $libs -o $output
if ($LASTEXITCODE -ne 0) {
    throw "Native raylib library build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $output"
