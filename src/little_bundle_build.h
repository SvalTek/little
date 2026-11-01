#pragma once

#include <stddef.h>
#include <stdint.h>

int lt_bundle_create(
    const char* runtime_path,
    const char* output_path,
    const char* entry_path,
    const char* const* include_paths,
    uint32_t include_count,
    char* error,
    size_t error_size
);
