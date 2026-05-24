#include "little_std.h"
#include "little_internal.h"

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t _ltstd_is_callable(lt_Value value)
{
    if (!LT_IS_OBJECT(value)) return 0;
    lt_Object* obj = LT_GET_OBJECT(value);
    return obj->type == LT_OBJECT_FN
        || obj->type == LT_OBJECT_CLOSURE
        || obj->type == LT_OBJECT_NATIVEFN
        || obj->type == LT_OBJECT_BOUND_NATIVE
        || obj->type == LT_OBJECT_CLASS;
}

static lt_Value _ltstd_pcall_result(lt_VM* vm, uint8_t ok, lt_Value value, const char* error)
{
    lt_Value result = lt_make_table(vm);
    lt_table_set(vm, result, lt_make_string(vm, "ok"), ok ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    if (ok) lt_table_set(vm, result, lt_make_string(vm, "value"), value);
    else lt_table_set(vm, result, lt_make_string(vm, "error"), lt_make_string(vm, error ? error : "Unknown error"));
    return result;
}

static uint8_t _ltstd_pcall(lt_VM* vm, uint8_t argc)
{
    if (argc < 1) lt_runtime_error(vm, "Expected callable argument to pcall!");

    uint16_t base = vm->top - argc;
    lt_Value callable = vm->stack[base];
    if (!_ltstd_is_callable(callable)) lt_runtime_error(vm, "Expected callable argument to pcall!");

    for (uint16_t i = base + 1; i < vm->top; ++i)
        vm->stack[i - 1] = vm->stack[i];
    vm->top--;

    uint16_t saved_depth = vm->depth;
    lt_Frame* saved_current = vm->current;
    void* saved_error_buf = vm->error_buf;
    uint8_t saved_trap_errors = vm->trap_errors;
    char* saved_error_trap = vm->error_trap;

    jmp_buf error_buf;
    vm->error_buf = &error_buf;
    vm->trap_errors = 1;
    vm->error_trap = 0;

    if (!setjmp(error_buf))
    {
        uint16_t nret = lt_exec_internal(vm, callable, argc - 1);
        lt_Value value = LT_VALUE_NULL;
        if (nret > 0)
        {
            value = lt_pop(vm);
            while (--nret > 0) lt_pop(vm);
        }

        vm->error_buf = saved_error_buf;
        vm->trap_errors = saved_trap_errors;
        if (vm->error_trap) vm->free(vm->error_trap);
        vm->error_trap = saved_error_trap;

        lt_push(vm, _ltstd_pcall_result(vm, 1, value, 0));
        return 1;
    }

    char* error = vm->error_trap;
    vm->top = base;
    vm->depth = saved_depth;
    vm->current = saved_current;
    vm->error_buf = saved_error_buf;
    vm->trap_errors = saved_trap_errors;
    vm->error_trap = saved_error_trap;

    lt_push(vm, _ltstd_pcall_result(vm, 0, LT_VALUE_NULL, error));
    if (error) vm->free(error);
    return 1;
}

static uint8_t _ltstd_unpack(lt_VM* vm, uint8_t argc)
{
    if (argc < 1 || argc > 3) lt_runtime_error(vm, "Expected 1-3 arguments to unpack!");

    lt_Value end_value = LT_VALUE_NULL;
    lt_Value start_value = LT_VALUE_NUMBER(0);
    if (argc == 3) end_value = lt_pop(vm);
    if (argc >= 2) start_value = lt_pop(vm);
    lt_Value array = lt_pop(vm);

    if (!LT_IS_ARRAY(array)) lt_runtime_error(vm, "Expected first argument to unpack to be array!");
    if (!LT_IS_NUMBER(start_value)) lt_runtime_error(vm, "Expected start argument to unpack to be number!");
    if (!LT_IS_NULL(end_value) && !LT_IS_NUMBER(end_value)) lt_runtime_error(vm, "Expected end argument to unpack to be number!");

    int32_t start = (int32_t)LT_GET_NUMBER(start_value);
    int32_t end = LT_IS_NULL(end_value) ? (int32_t)lt_array_length(array) - 1 : (int32_t)LT_GET_NUMBER(end_value);
    if (start < 0 || end < -1) lt_runtime_error(vm, "Expected unpack bounds to be non-negative!");
    if (start > end || start >= (int32_t)lt_array_length(array)) return 0;
    if (end >= (int32_t)lt_array_length(array)) end = (int32_t)lt_array_length(array) - 1;

    uint8_t count = 0;
    for (int32_t i = start; i <= end && count < LT_MAX_RETURNS; ++i)
    {
        lt_push(vm, lt_array_get(vm, array, (uint32_t)i));
        count++;
    }
    return count;
}

static char* _ltstd_copy_string(lt_VM* vm, const char* string)
{
    size_t len = strlen(string);
    char* copy = vm->alloc(len + 1);
    memcpy(copy, string, len + 1);
    return copy;
}

static char* _ltstd_make_suffixed_path(lt_VM* vm, const char* path, const char* suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    char* result = vm->alloc(path_len + suffix_len + 1);
    memcpy(result, path, path_len);
    memcpy(result + path_len, suffix, suffix_len + 1);
    return result;
}

static char* _ltstd_join_path(lt_VM* vm, const char* root, const char* path)
{
    size_t root_len = strlen(root);
    size_t path_len = strlen(path);
    uint8_t needs_sep = root_len > 0 && root[root_len - 1] != '/' && root[root_len - 1] != '\\';
    char* result = vm->alloc(root_len + needs_sep + path_len + 1);
    memcpy(result, root, root_len);
    if (needs_sep) result[root_len++] = '/';
    memcpy(result + root_len, path, path_len + 1);
    return result;
}

static char* _ltstd_expand_module_pattern(lt_VM* vm, const char* pattern, const char* requested)
{
    const char* marker = strchr(pattern, '?');
    if (!marker) return _ltstd_join_path(vm, pattern, requested);

    const char* first_sep = strchr(requested, '/');
    const char* first_backslash = strchr(requested, '\\');
    if (!first_sep || (first_backslash && first_backslash < first_sep)) first_sep = first_backslash;

    size_t package_len = first_sep ? (size_t)(first_sep - requested) : strlen(requested);
    const char* subpath = first_sep ? first_sep + 1 : "";
    size_t subpath_len = strlen(subpath);
    uint8_t has_subpath = subpath_len > 0;

    size_t total = has_subpath ? subpath_len + 2 : 1;
    for (const char* cursor = pattern; *cursor; ++cursor)
        total += *cursor == '?' ? package_len : 1;

    char* result = vm->alloc(total);
    char* out = result;
    for (const char* cursor = pattern; *cursor; ++cursor)
    {
        if (*cursor == '?')
        {
            memcpy(out, requested, package_len);
            out += package_len;
        }
        else *out++ = *cursor;
    }
    if (has_subpath)
    {
        if (out > result && out[-1] != '/' && out[-1] != '\\') *out++ = '/';
        memcpy(out, subpath, subpath_len);
        out += subpath_len;
    }
    *out = 0;
    return result;
}

static FILE* _ltstd_open_module_base(lt_VM* vm, const char* base, char** resolved)
{
    char* candidate = 0;
    size_t base_len = strlen(base);
    if (base_len >= 7 && strcmp(base + base_len - 7, ".little") == 0)
        candidate = _ltstd_copy_string(vm, base);
    else
        candidate = _ltstd_make_suffixed_path(vm, base, ".little");

    FILE* file = fopen(candidate, "rb");
    if (file)
    {
        *resolved = candidate;
        return file;
    }
    vm->free(candidate);

    candidate = _ltstd_join_path(vm, base, "init.little");
    file = fopen(candidate, "rb");
    if (file)
    {
        *resolved = candidate;
        return file;
    }
    vm->free(candidate);

    return 0;
}

static lt_Value _ltstd_module_paths(lt_VM* vm, uint8_t create)
{
    lt_Value key = lt_make_string(vm, "__module_paths");
    lt_Value paths = lt_table_get(vm, vm->global, key);
    if (!LT_IS_ARRAY(paths) && create)
    {
        paths = lt_make_array(vm);
        lt_table_set(vm, vm->global, key, paths);
    }
    return paths;
}

static char* _ltstd_read_module_file(lt_VM* vm, const char* requested, char** resolved)
{
    FILE* file = _ltstd_open_module_base(vm, requested, resolved);
    lt_Value paths = _ltstd_module_paths(vm, 0);

    if (!file && LT_IS_ARRAY(paths))
    {
        for (uint32_t i = 0; i < lt_array_length(paths); ++i)
        {
            lt_Value entry = lt_array_get(vm, paths, i);
            if (!LT_IS_STRING(entry)) continue;
            char* base = _ltstd_expand_module_pattern(vm, lt_get_string(vm, entry), requested);
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

static uint8_t _ltstd_module_add_path(lt_VM* vm, uint8_t argc)
{
    if (argc < 1) lt_runtime_error(vm, "Expected at least one path for module.addPath!");
    uint16_t base = vm->top - argc;
    lt_Value paths = _ltstd_module_paths(vm, 1);
    for (uint16_t i = base; i < vm->top; ++i)
    {
        lt_Value path = vm->stack[i];
        if (!LT_IS_STRING(path)) lt_runtime_error(vm, "Expected module search path to be string!");
        lt_array_push(vm, paths, path);
    }
    vm->top = base;
    return 0;
}

static uint8_t _ltstd_module_clear_paths(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt_runtime_error(vm, "Expected no arguments to module.clearPaths!");
    lt_Value paths = _ltstd_module_paths(vm, 1);
    while (lt_array_length(paths) > 0) lt_array_remove(vm, paths, lt_array_length(paths) - 1);
    return 0;
}

static uint8_t _ltstd_import(lt_VM* vm, uint8_t argc)
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
    char* source = _ltstd_read_module_file(vm, requested, &resolved);
    if (!source)
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

void ltstd_open_all(lt_VM* vm)
{
    lt_table_set(vm, vm->global, lt_make_string(vm, "pcall"), lt_make_native(vm, _ltstd_pcall));
    lt_table_set(vm, vm->global, lt_make_string(vm, "unpack"), lt_make_native(vm, _ltstd_unpack));
    lt_table_set(vm, vm->global, lt_make_string(vm, "import"), lt_make_native(vm, _ltstd_import));
    lt_Value module = lt_make_table(vm);
    lt_table_set(vm, module, lt_make_string(vm, "addPath"), lt_make_native(vm, _ltstd_module_add_path));
    lt_table_set(vm, module, lt_make_string(vm, "clearPaths"), lt_make_native(vm, _ltstd_module_clear_paths));
    lt_table_set(vm, vm->global, lt_make_string(vm, "module"), module);
    ltstd_open_io(vm);
    ltstd_open_math(vm);
    ltstd_open_array(vm);
    ltstd_open_table(vm);
    ltstd_open_string(vm);
    ltstd_open_gc(vm);
}

char* ltstd_tostring(lt_VM* vm, lt_Value val)
{
    char scratch[256];
    int len = 0;

    if (LT_IS_NUMBER(val)) len = snprintf(scratch, sizeof(scratch), "%f", LT_GET_NUMBER(val));
    if (LT_IS_NULL(val)) len = snprintf(scratch, sizeof(scratch), "null");
    if (LT_IS_TRUE(val)) len = snprintf(scratch, sizeof(scratch), "true");
    if (LT_IS_FALSE(val)) len = snprintf(scratch, sizeof(scratch), "false");
    if (LT_IS_STRING(val)) len = snprintf(scratch, sizeof(scratch), "%s", lt_get_string(vm, val));

    if (LT_IS_OBJECT(val))
    {
        lt_Object* obj = LT_GET_OBJECT(val);
        switch (obj->type)
        {
        case LT_OBJECT_CHUNK: len = snprintf(scratch, sizeof(scratch), "chunk %p", (void*)obj); break;
        case LT_OBJECT_CLOSURE: len = snprintf(scratch, sizeof(scratch), "closure %p | %u upvals", (void*)LT_GET_OBJECT(obj->closure.function), obj->closure.captures.length); break;
        case LT_OBJECT_FN: len = snprintf(scratch, sizeof(scratch), "function %p", (void*)obj); break;
        case LT_OBJECT_TABLE: len = snprintf(scratch, sizeof(scratch), "table %p", (void*)obj); break;
        case LT_OBJECT_ARRAY: len = snprintf(scratch, sizeof(scratch), "array | %u", lt_array_length(val)); break;
        case LT_OBJECT_NATIVEFN: len = snprintf(scratch, sizeof(scratch), "native %p", (void*)obj); break;
        case LT_OBJECT_BOUND_NATIVE: len = snprintf(scratch, sizeof(scratch), "bound_native %p", (void*)obj); break;
        case LT_OBJECT_PROMISE: len = snprintf(scratch, sizeof(scratch), "promise %p", (void*)obj); break;
        case LT_OBJECT_CLASS: len = snprintf(scratch, sizeof(scratch), "class %p", (void*)obj); break;
        case LT_OBJECT_INSTANCE: len = snprintf(scratch, sizeof(scratch), "instance %p", (void*)obj); break;
        case LT_OBJECT_CELL: len = snprintf(scratch, sizeof(scratch), "cell %p", (void*)obj); break;
        case LT_OBJECT_PTR: len = snprintf(scratch, sizeof(scratch), "ptr %p", (void*)obj); break;
        case LT_OBJECT_SHARED_TABLE: len = snprintf(scratch, sizeof(scratch), "shared_table %p", (void*)obj->shared); break;
        case LT_OBJECT_SHARED_ARRAY: len = snprintf(scratch, sizeof(scratch), "shared_array %p | %u", (void*)obj->shared, lt_array_length(val)); break;
    }
    }

    uint32_t out_len = 0;
    if (len < 0)
    {
        while (out_len < sizeof(scratch) && scratch[out_len]) ++out_len;
    }
    else out_len = (uint32_t)len;
    if (out_len >= sizeof(scratch)) out_len = sizeof(scratch) - 1;

    char* str = vm->alloc(out_len + 1);
    memcpy(str, scratch, out_len);
    str[out_len] = 0;

    return str;
}
