#include "little_common.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static size_t checked_add(lt_VM* vm, size_t left, size_t right)
{
    if (left > SIZE_MAX - right) lt_runtime_error(vm, "String allocation size overflow!");
    return left + right;
}

char* lt_common_copy_string(lt_VM* vm, const char* string)
{
    size_t len = strlen(string);
    char* copy = vm->alloc(checked_add(vm, len, 1));
    memcpy(copy, string, len + 1);
    return copy;
}

char* lt_common_make_suffixed_path(lt_VM* vm, const char* path, const char* suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    char* result = vm->alloc(checked_add(vm, checked_add(vm, path_len, suffix_len), 1));
    memcpy(result, path, path_len);
    memcpy(result + path_len, suffix, suffix_len + 1);
    return result;
}

char* lt_common_join_path(lt_VM* vm, const char* root, const char* path)
{
    size_t root_len = strlen(root);
    size_t path_len = strlen(path);
    uint8_t needs_sep = root_len > 0 && root[root_len - 1] != '/' && root[root_len - 1] != '\\';
    char* result = vm->alloc(checked_add(vm, checked_add(vm, checked_add(vm, root_len, needs_sep), path_len), 1));
    memcpy(result, root, root_len);
    if (needs_sep) result[root_len++] = '/';
    memcpy(result + root_len, path, path_len + 1);
    return result;
}

char* lt_common_expand_path_pattern(lt_VM* vm, const char* pattern, const char* requested)
{
    const char* marker = strchr(pattern, '?');
    if (!marker) return lt_common_join_path(vm, pattern, requested);

    const char* first_sep = strchr(requested, '/');
    const char* first_backslash = strchr(requested, '\\');
    if (!first_sep || (first_backslash && first_backslash < first_sep)) first_sep = first_backslash;

    size_t package_len = first_sep ? (size_t)(first_sep - requested) : strlen(requested);
    const char* subpath = first_sep ? first_sep + 1 : "";
    size_t subpath_len = strlen(subpath);
    uint8_t has_subpath = subpath_len > 0;

    size_t total = has_subpath ? checked_add(vm, subpath_len, 2) : 1;
    for (const char* cursor = pattern; *cursor; ++cursor)
        total = checked_add(vm, total, *cursor == '?' ? package_len : 1);

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

uint8_t lt_common_has_suffix(const char* path, const char* suffix)
{
    size_t path_len = strlen(path);
    size_t suffix_len = strlen(suffix);
    return path_len >= suffix_len && strcmp(path + path_len - suffix_len, suffix) == 0;
}

uint8_t lt_common_file_exists(const char* path)
{
    FILE* file = fopen(path, "rb");
    if (!file) return 0;
    fclose(file);
    return 1;
}

lt_Value lt_common_module_paths(lt_VM* vm, uint8_t create)
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
