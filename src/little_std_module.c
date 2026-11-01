#include "little_std_internal.h"
#include "little_common.h"
#include "little.h"
#include "little_buffer_internal.h"
#include "little_runtime_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE* _ltstd_open_module_base(lt_VM* vm, const char* base, char** resolved)
{
    char* candidate = 0;
    if (lt_common_has_little_extension(base))
        candidate = lt_common_copy_string(vm, base);
    else
        candidate = lt_common_make_suffixed_path(vm, base, ".little");

    FILE* file = fopen(candidate, "rb");
    if (file)
    {
        *resolved = candidate;
        return file;
    }
    vm->free(candidate);

    candidate = lt_common_join_path(vm, base, "init.little");
    file = fopen(candidate, "rb");
    if (file)
    {
        *resolved = candidate;
        return file;
    }
    vm->free(candidate);

    return 0;
}

static char* _ltstd_read_module_file(lt_VM* vm, const char* requested, char** resolved)
{
    FILE* file = _ltstd_open_module_base(vm, requested, resolved);
    lt_Value paths = lt_common_module_paths(vm, 0);

    if (!file && LT_IS_ARRAY(paths))
    {
        for (uint32_t i = 0; i < lt_array_length(paths); ++i)
        {
            lt_Value entry = lt_array_get(vm, paths, i);
            if (!LT_IS_STRING(entry)) continue;
            char* base = lt_common_expand_path_pattern(vm, lt_get_string(vm, entry), requested);
            file = _ltstd_open_module_base(vm, base, resolved);
            vm->free(base);
            if (file) break;
        }
    }

    if (!file)
        return 0;

    if (fseek(file, 0, SEEK_END) != 0)
    {
        fclose(file);
        vm->free(*resolved);
        *resolved = 0;
        return 0;
    }

    long size = ftell(file);
    if (size < 0)
    {
        fclose(file);
        vm->free(*resolved);
        *resolved = 0;
        return 0;
    }
    rewind(file);

    char* source = vm->alloc((size_t)size + 1);
    size_t read = fread(source, 1, (size_t)size, file);
    fclose(file);
    if (read != (size_t)size)
    {
        vm->free(source);
        vm->free(*resolved);
        *resolved = 0;
        return 0;
    }
    source[read] = 0;

    return source;
}

lt_ModuleLoaderResult ltstd_file_module_loader_internal(lt_VM* vm, const char* requested, char** source, char** module_name, void* userdata)
{
    (void)userdata;
    *module_name = 0;
    *source = _ltstd_read_module_file(vm, requested, module_name);
    return *source ? LT_MODULE_LOADER_FOUND : LT_MODULE_LOADER_NOT_FOUND;
}

static uint8_t _ltstd_read_module(lt_VM* vm, const char* requested, char** source, char** module_name)
{
    for (uint32_t i = 0; i < vm->module_loaders.length; ++i)
    {
        lt_ModuleLoader* loader = lt_buffer_at(&vm->module_loaders, i);
        *source = 0;
        *module_name = 0;
        if (loader->load(vm, requested, source, module_name, loader->userdata) != LT_MODULE_LOADER_FOUND)
            continue;
        if (!*source) lt_runtime_error(vm, "Module loader returned no source!");
        if (!*module_name || !**module_name)
            *module_name = lt_common_copy_string(vm, requested);
        return 1;
    }
    return 0;
}

uint8_t ltstd_module_add_path_internal(lt_VM* vm, uint8_t argc)
{
    if (argc < 1) lt_runtime_error(vm, "Expected at least one path for module.addPath!");
    uint16_t base = vm->top - argc;
    lt_Value paths = lt_common_module_paths(vm, 1);
    for (uint16_t i = base; i < vm->top; ++i)
    {
        lt_Value path = vm->stack[i];
        if (!LT_IS_STRING(path)) lt_runtime_error(vm, "Expected module search path to be string!");
        lt_array_push(vm, paths, path);
    }
    vm->top = base;
    return 0;
}

uint8_t ltstd_module_clear_paths_internal(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt_runtime_error(vm, "Expected no arguments to module.clearPaths!");
    lt_Value paths = lt_common_module_paths(vm, 1);
    while (lt_array_length(paths) > 0) lt_array_remove(vm, paths, lt_array_length(paths) - 1);
    return 0;
}

uint8_t ltstd_import_internal(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to import!");
    lt_Value path_value = lt_pop(vm);
    if (!LT_IS_STRING(path_value)) lt_runtime_error(vm, "Expected import path to be string!");

    const char* requested = LT_GET_STRING(vm, path_value);
    lt_Value modules_key = lt_make_string(vm, "__modules");
    lt_Value modules = lt_table_get(vm, vm->global, modules_key);
    if (!LT_IS_TABLE(modules))
    {
        modules = lt_make_table(vm);
        lt_table_set(vm, vm->global, modules_key, modules);
    }

    lt_Value cache_key = lt_make_string(vm, requested);
    lt_Value cached = lt_table_get(vm, modules, cache_key);
    if (LT_IS_TABLE(cached))
    {
        lt_Value value = lt_table_get(vm, cached, lt_make_string(vm, "value"));
        lt_push(vm, value);
        return 1;
    }

    size_t requested_len = strlen(requested);
    char* fallback = vm->alloc(requested_len + 8);
    memcpy(fallback, requested, requested_len);
    memcpy(fallback + requested_len, ".little", 8);
    lt_Value fallback_key = lt_make_string(vm, fallback);
    cached = lt_table_get(vm, modules, fallback_key);
    if (LT_IS_TABLE(cached))
    {
        lt_Value value = lt_table_get(vm, cached, lt_make_string(vm, "value"));
        vm->free(fallback);
        lt_push(vm, value);
        return 1;
    }
    vm->free(fallback);

    char* resolved = 0;
    char* source = 0;
    if (!_ltstd_read_module(vm, requested, &source, &resolved))
    {
        char message[256];
        snprintf(message, sizeof(message), "Failed to import module '%s'!", requested);
        lt_runtime_error(vm, message);
    }

    cache_key = lt_make_string(vm, resolved);
    cached = lt_table_get(vm, modules, cache_key);
    if (LT_IS_TABLE(cached))
    {
        lt_Value value = lt_table_get(vm, cached, lt_make_string(vm, "value"));
        vm->free(source);
        vm->free(resolved);
        lt_push(vm, value);
        return 1;
    }

    lt_Value callable = lt_loadstring(vm, source, resolved);
    if (callable == LT_VALUE_NULL)
    {
        vm->free(source);
        vm->free(resolved);
        lt_runtime_error(vm, "Failed to compile imported module!");
    }

    lt_Value value_key = lt_make_string(vm, "value");
    lt_push(vm, callable);
    lt_Value wrapper = lt_make_table(vm);
    lt_push(vm, wrapper);
    lt_table_set(vm, wrapper, value_key, LT_VALUE_TRUE);
    lt_table_set(vm, modules, cache_key, wrapper);
    lt_pop(vm);
    callable = lt_pop(vm);

    uint16_t saved_top = vm->top;
    uint16_t saved_depth = vm->depth;
    lt_Frame* saved_current = vm->current;
    uint32_t saved_native_roots = vm->native_roots.length;
    uint32_t saved_temporary_roots = vm->temporary_roots.length;
    uint32_t saved_native_root_floor = vm->native_root_floor;
    void* saved_error_buf = vm->error_buf;
    uint8_t saved_trap_errors = vm->trap_errors;
    char* saved_error_trap = vm->error_trap;

    jmp_buf error_buf;
    vm->error_buf = &error_buf;
    vm->trap_errors = 1;
    vm->error_trap = 0;

    uint16_t nret = 0;
    if (!setjmp(error_buf))
    {
        nret = lt_exec_internal(vm, callable, 0);
        vm->error_buf = saved_error_buf;
        vm->trap_errors = saved_trap_errors;
        if (vm->error_trap) vm->free(vm->error_trap);
        vm->error_trap = saved_error_trap;
    }
    else
    {
        char* error = vm->error_trap;
        vm->top = saved_top;
        vm->depth = saved_depth;
        vm->current = saved_current;
        vm->native_roots.length = saved_native_roots;
        vm->temporary_roots.length = saved_temporary_roots;
        vm->native_root_floor = saved_native_root_floor;
        vm->error_buf = saved_error_buf;
        vm->trap_errors = saved_trap_errors;
        vm->error_trap = saved_error_trap;
        lt_table_pop(vm, modules, cache_key);
        lt_table_pop(vm, modules, lt_make_string(vm, requested));
        vm->free(source);
        vm->free(resolved);
        if (saved_trap_errors)
        {
            if (vm->error_trap) vm->free(vm->error_trap);
            vm->error_trap = error;
            if (vm->error_buf) longjmp(*(jmp_buf*)vm->error_buf, 1);
            abort();
        }
        lt_error(vm, error ? error : "Unknown import error");
    }

    lt_Value value = LT_VALUE_TRUE;
    if (nret > 0)
    {
        uint16_t base = vm->top - nret;
        value = vm->stack[base];
        vm->stack[base] = value;
        vm->top = base + 1;
    }
    else lt_push(vm, value);

    lt_table_set(vm, wrapper, lt_make_string(vm, "value"), value);
    lt_pop(vm);
    vm->free(source);
    vm->free(resolved);

    lt_push(vm, value);
    return 1;
}
