param(
    [string]$Output = "build/miniz"
)

$ErrorActionPreference = "Stop"

$repo = Split-Path -Parent $PSScriptRoot
$source = Join-Path $repo "vendor/miniz"
$destination = Join-Path $repo $Output
$files = @(
    "miniz.c",
    "miniz.h",
    "miniz_common.h",
    "miniz_tdef.c",
    "miniz_tdef.h",
    "miniz_tinfl.c",
    "miniz_tinfl.h",
    "miniz_zip.c",
    "miniz_zip.h"
)

New-Item -ItemType Directory -Force -Path $destination | Out-Null
foreach ($file in $files) {
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination (Join-Path $destination $file) -Force
}

@'
#ifndef MINIZ_EXPORT_H
#define MINIZ_EXPORT_H

#ifndef MINIZ_EXPORT
#define MINIZ_EXPORT
#endif

#endif
'@ | Set-Content -LiteralPath (Join-Path $destination "miniz_export.h") -NoNewline -Encoding ascii
