param(
    [string]$Compiler = "",
    [string]$CxxCompiler = ""
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$toolchainBin = if ($env:GCC_PATH) { Join-Path $env:GCC_PATH "bin" } else { "" }
if ($toolchainBin -and (Test-Path $toolchainBin)) {
    $env:PATH = "$toolchainBin$([IO.Path]::PathSeparator)$env:PATH"
}
$Compiler = if ($Compiler) { $Compiler } elseif ($env:GCC_PATH) { Join-Path $env:GCC_PATH "bin/gcc.exe" } else { "gcc" }
$webuiVendor = Join-Path $repo "vendor/webui"
$nativeExt = if ($env:OS -eq "Windows_NT") { ".dll" } elseif ($IsMacOS) { ".dylib" } else { ".so" }
$CxxCompiler = if ($CxxCompiler) { $CxxCompiler } elseif ($env:GCC_PATH) { Join-Path $env:GCC_PATH "bin/g++.exe" } elseif ($Compiler -eq "gcc") { "g++" } elseif ($Compiler -eq "clang") { "clang++" } else { $Compiler }
$includeFlags = if ($env:INCLUDES_PATH) { @("-I", $env:INCLUDES_PATH) } else { @() }
$sharedLibraryFlag = if ($IsMacOS) { "-dynamiclib" } else { "-shared" }
$nativeFlags = @("-std=c11", $sharedLibraryFlag, "-I", (Join-Path $repo "src"))
$nativeFlags += $includeFlags
if ($env:OS -ne "Windows_NT") {
    $nativeFlags += "-fPIC"
}

$jsonOut = Join-Path $PSScriptRoot "json/build/json$nativeExt"
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $jsonOut) | Out-Null

& $Compiler `
    $nativeFlags `
    (Join-Path $PSScriptRoot "json/json.c") `
    -o $jsonOut

if ($LASTEXITCODE -ne 0) {
    throw "Native JSON library build failed with exit code $LASTEXITCODE"
}

Write-Host "Built $jsonOut"

if (Test-Path $webuiVendor) {
    $webuiOutDir = Join-Path $PSScriptRoot "webui/build"
    $webuiOut = Join-Path $webuiOutDir "webui$nativeExt"
    New-Item -ItemType Directory -Force -Path $webuiOutDir | Out-Null

    $webuiInclude = Join-Path $webuiVendor "include"
    $webuiDefines = @("-DNO_SSL", "-DNDEBUG", "-DNO_CACHING", "-DNO_CGI", "-DUSE_WEBSOCKET")

    if ($env:OS -eq "Windows_NT") {
        $webuiDefines += "-DMUST_IMPLEMENT_CLOCK_GETTIME"
        $webuiDefines += "-D_WIN32_WINNT=0x0600"
        $objects = @(
            (Join-Path $webuiOutDir "webui_little.o"),
            (Join-Path $webuiOutDir "webui.o"),
            (Join-Path $webuiOutDir "civetweb.o"),
            (Join-Path $webuiOutDir "win32_wv2.o")
        )

        & $Compiler -std=c11 $includeFlags -I (Join-Path $repo "src") -I $webuiInclude -c (Join-Path $PSScriptRoot "webui/webui_little.c") -o $objects[0]
        if ($LASTEXITCODE -ne 0) { throw "Native WebUI wrapper build failed with exit code $LASTEXITCODE" }

        & $Compiler -std=c11 $includeFlags -I $webuiInclude $webuiDefines -c (Join-Path $webuiVendor "src/webui.c") -o $objects[1]
        if ($LASTEXITCODE -ne 0) { throw "WebUI core build failed with exit code $LASTEXITCODE" }

        & $Compiler -std=c11 -w $includeFlags -I $webuiInclude $webuiDefines -c (Join-Path $webuiVendor "src/civetweb/civetweb.c") -o $objects[2]
        if ($LASTEXITCODE -ne 0) { throw "WebUI civetweb build failed with exit code $LASTEXITCODE" }

        & $CxxCompiler -std=c++17 -I $webuiInclude $webuiDefines -c (Join-Path $webuiVendor "src/webview/win32_wv2.cpp") -o $objects[3]
        if ($LASTEXITCODE -ne 0) { throw "WebUI WebView2 build failed with exit code $LASTEXITCODE" }

        & $CxxCompiler -shared $objects -static -static-libgcc -static-libstdc++ -lws2_32 -luser32 -lole32 -luuid -o $webuiOut
        if ($LASTEXITCODE -ne 0) { throw "Native WebUI library link failed with exit code $LASTEXITCODE" }
    }
    else {
        $webuiFlags = @("-std=c11", $sharedLibraryFlag, "-fPIC") + $includeFlags + @("-I", (Join-Path $repo "src"), "-I", $webuiInclude) + $webuiDefines
        $webuiLibs = @("-lpthread", "-lm", "-ldl")
        if ($IsMacOS) {
            $webuiLibs = @("-lpthread", "-lm", "-framework", "Cocoa", "-framework", "WebKit")
        }
        $webuiSources = @(
            (Join-Path $PSScriptRoot "webui/webui_little.c"),
            (Join-Path $webuiVendor "src/webui.c"),
            (Join-Path $webuiVendor "src/civetweb/civetweb.c")
        )
        if ($IsMacOS) {
            $webuiSources += Join-Path $webuiVendor "src/webview/wkwebview.m"
        }

        & $Compiler `
            $webuiFlags `
            $webuiSources `
            $webuiLibs `
            -o $webuiOut

        if ($LASTEXITCODE -ne 0) {
            throw "Native WebUI library build failed with exit code $LASTEXITCODE"
        }
    }

    Write-Host "Built $webuiOut"
}
else {
    Write-Host "Skipping WebUI native library; vendor/webui was not found."
}
