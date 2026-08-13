#include "little_loadlib.h"
#include "little_common.h"
#include "little_internal.h"

#include <stdio.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

typedef lt_Value (*lt_NativeLibraryOpenFn)(lt_VM* vm, const lt_Api* api);

static void* _lt_api_alloc(lt_VM* vm, size_t size)
{
    return vm->alloc(size);
}

static void _lt_api_free(lt_VM* vm, void* ptr)
{
    vm->free(ptr);
}

static uint8_t _lt_api_is_promise(lt_Value value)
{
    return LT_IS_OBJECT(value) && LT_GET_OBJECT(value)->type == LT_OBJECT_PROMISE;
}

static lt_PromiseState _lt_api_promise_state(lt_Value value)
{
    if (!_lt_api_is_promise(value)) return LT_PROMISE_REJECTED;
    return LT_GET_OBJECT(value)->promise.state;
}

static lt_Value _lt_api_promise_result(lt_Value value)
{
    if (!_lt_api_is_promise(value)) return LT_VALUE_NULL;
    return LT_GET_OBJECT(value)->promise.result;
}

static const lt_Api _lt_native_api = {
    LT_API_VERSION,
    sizeof(lt_Api),
    _lt_api_alloc,
    _lt_api_free,
    lt_runtime_error,
    lt_push,
    lt_pop,
    lt_exec,
    lt_make_number,
    lt_get_number,
    lt_make_string,
    lt_get_string,
    lt_make_table,
    lt_make_array,
    lt_make_native,
    lt_make_ptr,
    lt_get_ptr,
    lt_table_set,
    lt_table_get,
    lt_table_next,
    lt_table_pop,
    lt_array_push,
    lt_array_get,
    lt_array_set,
    lt_array_remove,
    lt_array_length,
    lt_poll,
    _lt_api_is_promise,
    _lt_api_promise_state,
    _lt_api_promise_result,
};

static const char* _lt_native_library_suffix(void)
{
#ifdef _WIN32
    return ".dll";
#elif defined(__APPLE__)
    return ".dylib";
#else
    return ".so";
#endif
}

static char* _lt_resolve_native_library_base(lt_VM* vm, const char* base)
{
    const char* suffix = _lt_native_library_suffix();
    char* candidate = 0;

    if (lt_common_has_suffix(base, suffix))
        candidate = lt_common_copy_string(vm, base);
    else
        candidate = lt_common_make_suffixed_path(vm, base, suffix);

    if (lt_common_file_exists(candidate))
        return candidate;
    vm->free(candidate);

    char* init_base = lt_common_join_path(vm, base, "init");
    candidate = lt_common_make_suffixed_path(vm, init_base, suffix);
    vm->free(init_base);

    if (lt_common_file_exists(candidate))
        return candidate;
    vm->free(candidate);

    return 0;
}

static char* _lt_resolve_native_library(lt_VM* vm, const char* requested)
{
    char* resolved = _lt_resolve_native_library_base(vm, requested);
    lt_Value paths = lt_common_module_paths(vm, 0);

    if (!resolved && LT_IS_ARRAY(paths))
    {
        for (uint32_t i = 0; i < lt_array_length(paths); ++i)
        {
            lt_Value entry = lt_array_get(vm, paths, i);
            if (!LT_IS_STRING(entry)) continue;
            char* base = lt_common_expand_path_pattern(vm, lt_get_string(vm, entry), requested);
            resolved = _lt_resolve_native_library_base(vm, base);
            vm->free(base);
            if (resolved) break;
        }
    }

    return resolved;
}

static void* _lt_open_native_library(const char* path, char* error, size_t error_size)
{
#ifdef _WIN32
    HMODULE handle = LoadLibraryA(path);
    if (!handle)
    {
        snprintf(error, error_size, "Unable to load native library!");
        return 0;
    }
    return (void*)handle;
#else
    dlerror();
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle)
    {
        const char* message = dlerror();
        snprintf(error, error_size, "%s", message ? message : "Unable to load native library!");
        return 0;
    }
    return handle;
#endif
}

static void* _lt_native_library_symbol(void* handle, const char* name)
{
#ifdef _WIN32
    return (void*)GetProcAddress((HMODULE)handle, name);
#else
    return dlsym(handle, name);
#endif
}

static void _lt_close_native_library(void* handle)
{
#ifdef _WIN32
    FreeLibrary((HMODULE)handle);
#else
    dlclose(handle);
#endif
}

static uint8_t _lt_load_library(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to loadLibrary!");
    lt_Value path_value = lt_pop(vm);
    if (!LT_IS_STRING(path_value)) lt_runtime_error(vm, "Expected native library path to be string!");

    const char* requested = LT_GET_STRING(vm, path_value);
    lt_Value cache_key = lt_make_string(vm, "__native_libraries");
    lt_Value cache = lt_table_get(vm, vm->global, cache_key);
    if (!LT_IS_TABLE(cache))
    {
        cache = lt_make_table(vm);
        lt_table_set(vm, vm->global, cache_key, cache);
    }

    lt_Value requested_key = lt_make_string(vm, requested);
    lt_Value cached = lt_table_get(vm, cache, requested_key);
    if (LT_IS_TABLE(cached))
    {
        lt_push(vm, lt_table_get(vm, cached, lt_make_string(vm, "value")));
        return 1;
    }

    char* resolved = _lt_resolve_native_library(vm, requested);
    if (!resolved)
    {
        char message[256];
        snprintf(message, sizeof(message), "Failed to load native library '%s'!", requested);
        lt_runtime_error(vm, message);
    }

    lt_Value resolved_key = lt_make_string(vm, resolved);
    cached = lt_table_get(vm, cache, resolved_key);
    if (LT_IS_TABLE(cached))
    {
        lt_Value value = lt_table_get(vm, cached, lt_make_string(vm, "value"));
        lt_table_set(vm, cache, requested_key, cached);
        vm->free(resolved);
        lt_push(vm, value);
        return 1;
    }

    char error[256];
    void* handle = _lt_open_native_library(resolved, error, sizeof(error));
    if (!handle)
    {
        char message[512];
        snprintf(message, sizeof(message), "Failed to load native library '%s': %s", resolved, error);
        vm->free(resolved);
        lt_runtime_error(vm, message);
    }

    lt_NativeLibraryOpenFn open_fn = (lt_NativeLibraryOpenFn)_lt_native_library_symbol(handle, "ltopen");
    if (!open_fn)
    {
        _lt_close_native_library(handle);
        vm->free(resolved);
        lt_runtime_error(vm, "Native library does not export ltopen!");
    }

    lt_Value value = open_fn(vm, &_lt_native_api);
    if (value == LT_VALUE_NULL)
    {
        _lt_close_native_library(handle);
        vm->free(resolved);
        lt_runtime_error(vm, "Native library rejected the Little API version!");
    }
    if (LT_IS_OBJECT(value)) lt_nocollect(vm, LT_GET_OBJECT(value));

    lt_NativeLibrary library = { handle, _lt_close_native_library };
    lt_buffer_push(vm, &vm->native_libraries, &library);

    lt_Value wrapper = lt_make_table(vm);
    lt_table_set(vm, wrapper, lt_make_string(vm, "value"), value);
    lt_table_set(vm, cache, resolved_key, wrapper);
    lt_table_set(vm, cache, requested_key, wrapper);

    if (LT_IS_OBJECT(value)) lt_resumecollect(vm, LT_GET_OBJECT(value));
    vm->free(resolved);
    lt_push(vm, value);
    return 1;
}

void ltstd_open_loadlib(lt_VM* vm)
{
    lt_table_set(vm, vm->global, lt_make_string(vm, "loadLibrary"), lt_make_native(vm, _lt_load_library));
}
