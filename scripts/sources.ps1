# Shared Little source sets. Paths are relative to the Little repository root.
# Dot-source this file from a script that has already resolved that root.

$LittleCoreSources = @(
    "src/little_buffer.c"
    "src/little_error.c"
    "src/little_value.c"
    "src/little_table.c"
    "src/little_iter.c"
    "src/little_lexer.c"
    "src/little_parser.c"
    "src/little_compiler.c"
    "src/little_gc.c"
    "src/little_class.c"
    "src/little_api.c"
    "src/little_shared.c"
    "src/little_runloop.c"
    "src/little.c"
    "src/little_async.c"
)

$LittleAnalysisSources = @(
    $LittleCoreSources | Where-Object { $_ -ne "src/little_api.c" }
    "src/little_analysis.c"
)

$LittleOptionalSources = @(
    "src/little_async_std.c"
    "src/little_task.c"
)

$LittleStdSources = @(
    "src/little_common.c"
    "src/little_host.c"
    $LittleOptionalSources
    "src/little_std.c"
    "src/little_std_module.c"
    "src/little_std_convert.c"
    "src/little_loadlib.c"
    "src/little_std_io.c"
    "src/little_std_web.c"
    "src/little_std_math.c"
    "src/little_std_number.c"
    "src/little_std_bit.c"
    "src/little_std_array.c"
    "src/little_std_table.c"
    "src/little_std_string.c"
    "src/little_std_gc.c"
    "src/little_std_json.c"
    "src/little_std_ws.c"
    "src/little_net.c"
)

$LittleBundleSources = @(
    "src/little_bundle.c"
    "src/little_bundle_build.c"
)

$LittleEmbeddingSources = @(
    $LittleCoreSources
    $LittleStdSources
)

$LittleTermSources = @(
    "src/little_std_term.c"
    "nativelib/term/term.c"
)

$LittleCliLibrarySources = @(
    $LittleEmbeddingSources
    $LittleBundleSources
)

$LittleCliExecutableSources = @(
    "main.c"
    "src/repl.c"
    "src/cli_config.c"
    $LittleCliLibrarySources
    $LittleTermSources
)

$LittleVendorSources = @(
    "vendor/llhttp/llhttp.c"
    "vendor/llhttp/api.c"
    "vendor/llhttp/http.c"
    "vendor/yyjson/yyjson.c"
    "vendor/pcg/pcg_basic.c"
    "vendor/wslay/lib/wslay_event.c"
    "vendor/wslay/lib/wslay_frame.c"
    "vendor/wslay/lib/wslay_net.c"
    "vendor/wslay/lib/wslay_queue.c"
    "vendor/wslay/lib/wslay_stack.c"
)
