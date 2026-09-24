#pragma once

#include "little.h"

#include <stddef.h>
#include <stdint.h>

typedef struct lt_Bundle lt_Bundle;

int lt_bundle_probe_self(const char* argv0);
lt_Bundle* lt_bundle_open_self(const char* path, char* error, size_t error_size);
void lt_bundle_close(lt_Bundle* bundle);
const char* lt_bundle_entry(const lt_Bundle* bundle);
char* lt_bundle_read_entry(lt_Bundle* bundle, const char* name, size_t* size, char* error, size_t error_size);
lt_ModuleLoaderResult lt_bundle_module_loader(lt_VM* vm, const char* requested, char** source, char** module_name, void* userdata);

int lt_bundle_create(
    const char* runtime_path,
    const char* output_path,
    const char* entry_path,
    const char* const* include_paths,
    uint32_t include_count,
    char* error,
    size_t error_size
);
