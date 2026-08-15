#include "little_std.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void _lt_string_append(lt_VM* vm, char** out, uint32_t* len, uint32_t* cap, const char* str, uint32_t str_len)
{
    if (*len + str_len + 1 > *cap)
    {
        uint32_t newcap = *cap ? *cap : 32;
        while (*len + str_len + 1 > newcap) newcap *= 2;

        char* next = vm->alloc(newcap);
        if (*out)
        {
            memcpy(next, *out, *len);
            vm->free(*out);
        }
        *out = next;
        *cap = newcap;
    }

    memcpy(*out + *len, str, str_len);
    *len += str_len;
    (*out)[*len] = 0;
}

static uint8_t _lt_expect_string(lt_VM* vm, lt_Value val, const char* message, const char** out)
{
    if (!LT_IS_STRING(val)) lt_runtime_error(vm, message);
    *out = lt_get_string(vm, val);
    return 1;
}

static uint8_t _lt_string_from(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to string.from!");
    lt_Value val = lt_pop(vm);
    char* temp = ltstd_tostring(vm, val);
    lt_Value str = lt_make_string(vm, temp);
    vm->free(temp);
    lt_push(vm, str);
    return 1;
}

static uint8_t _lt_string_concat(lt_VM* vm, uint8_t argc)
{
    if (argc < 2) lt_runtime_error(vm, "Expected at least two arguments to string.concat!");

    char* accum = 0;
    uint32_t len = 0;

    for (int32_t i = argc - 1; i >= 0; --i)
    {
        lt_Value val = vm->stack[vm->top - 1 - i];
        if (!LT_IS_STRING(val)) lt_runtime_error(vm, "Non-string argument to string.concat!");
        uint32_t oldlen = len;
        const char* str = lt_get_string(vm, val);

        char* oldaccum = accum;
        len += strlen(str);

        accum = vm->alloc(len + 1);
        if (oldaccum)
        {
            memcpy(accum, oldaccum, oldlen);
            vm->free(oldaccum);
        }

        memcpy(accum + oldlen, str, len - oldlen);
        accum[len] = 0;
    }

    lt_Value result = lt_make_string(vm, accum);
    vm->top -= argc;
    lt_push(vm, result);
    vm->free(accum);

    return 1;
}

static uint8_t _lt_string_len(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to string.len!");
    lt_Value val = lt_pop(vm);
    if (!LT_IS_STRING(val)) lt_runtime_error(vm, "Non-string argument to string.len!");
    lt_push(vm, LT_VALUE_NUMBER(strlen(lt_get_string(vm, val))));
    return 1;
}

static uint8_t _lt_string_sub(lt_VM* vm, uint8_t argc)
{
    if (argc < 2 || argc > 3) lt_runtime_error(vm, "Expected 2-3 arguments to string.sub!");

    lt_Value lenval = LT_VALUE_NULL;
    if (argc == 3) lenval = lt_pop(vm);

    lt_Value startval = lt_pop(vm);
    lt_Value str = lt_pop(vm);

    if (!LT_IS_STRING(str)) lt_runtime_error(vm, "Non-string argument to string.sub!");
    if (!LT_IS_NUMBER(startval)) lt_runtime_error(vm, "Non-number starting point to string.sub!");

    const char* cstr = lt_get_string(vm, str);
    uint32_t str_len = strlen(cstr);
    uint32_t start = (uint32_t)LT_GET_NUMBER(startval);
    if (start > str_len) start = str_len;

    uint32_t len = str_len - start;
    if (argc == 3)
    {
        if (!LT_IS_NUMBER(lenval)) lt_runtime_error(vm, "Non-number length to string.sub!");
        len = (uint32_t)LT_GET_NUMBER(lenval);
        if (start + len > str_len) len = str_len - start;
    }

    char* newstr = vm->alloc(len + 1);
    memcpy(newstr, cstr + start, len);
    newstr[len] = 0;

    lt_push(vm, lt_make_string(vm, newstr));
    vm->free(newstr);
    return 1;
}

static uint8_t _lt_string_contains(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to string.contains!");
    const char* needle;
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string needle to string.contains!", &needle);
    _lt_expect_string(vm, lt_pop(vm), "Non-string source to string.contains!", &str);
    lt_push(vm, strstr(str, needle) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_string_startswith(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to string.startsWith!");
    const char* prefix;
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string prefix to string.startsWith!", &prefix);
    _lt_expect_string(vm, lt_pop(vm), "Non-string source to string.startsWith!", &str);
    size_t prefix_len = strlen(prefix);
    lt_push(vm, strncmp(str, prefix, prefix_len) == 0 ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_string_endswith(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to string.endsWith!");
    const char* suffix;
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string suffix to string.endsWith!", &suffix);
    _lt_expect_string(vm, lt_pop(vm), "Non-string source to string.endsWith!", &str);
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    lt_push(vm, suffix_len <= str_len && strcmp(str + str_len - suffix_len, suffix) == 0 ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static uint8_t _lt_string_indexof(lt_VM* vm, uint8_t argc)
{
    if (argc < 2 || argc > 3) lt_runtime_error(vm, "Expected 2-3 arguments to string.indexOf!");
    lt_Value startval = LT_VALUE_NUMBER(0);
    if (argc == 3) startval = lt_pop(vm);
    const char* needle;
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string needle to string.indexOf!", &needle);
    _lt_expect_string(vm, lt_pop(vm), "Non-string source to string.indexOf!", &str);
    if (!LT_IS_NUMBER(startval)) lt_runtime_error(vm, "Non-number start to string.indexOf!");
    uint32_t start = (uint32_t)LT_GET_NUMBER(startval);
    uint32_t str_len = strlen(str);
    if (start > str_len) start = str_len;
    char* found = strstr(str + start, needle);
    double index = found ? (double)(found - str) : -1.0;
    lt_push(vm, LT_VALUE_NUMBER(index));
    return 1;
}

static uint8_t _lt_string_replace(lt_VM* vm, uint8_t argc)
{
    if (argc != 3) lt_runtime_error(vm, "Expected three arguments to string.replace!");
    const char* replacement;
    const char* needle;
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string replacement to string.replace!", &replacement);
    _lt_expect_string(vm, lt_pop(vm), "Non-string needle to string.replace!", &needle);
    _lt_expect_string(vm, lt_pop(vm), "Non-string source to string.replace!", &str);

    uint32_t needle_len = strlen(needle);
    if (needle_len == 0)
    {
        lt_push(vm, lt_make_string(vm, str));
        return 1;
    }

    char* out = 0;
    uint32_t len = 0;
    uint32_t cap = 0;
    uint32_t replacement_len = strlen(replacement);
    const char* cursor = str;
    char* found = 0;
    while ((found = strstr(cursor, needle)) != 0)
    {
        _lt_string_append(vm, &out, &len, &cap, cursor, (uint32_t)(found - cursor));
        _lt_string_append(vm, &out, &len, &cap, replacement, replacement_len);
        cursor = found + needle_len;
    }
    _lt_string_append(vm, &out, &len, &cap, cursor, strlen(cursor));

    lt_push(vm, lt_make_string(vm, out ? out : ""));
    if (out) vm->free(out);
    return 1;
}

static uint8_t _lt_string_split(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to string.split!");
    const char* sep;
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string separator to string.split!", &sep);
    _lt_expect_string(vm, lt_pop(vm), "Non-string source to string.split!", &str);

    lt_Value arr = lt_make_array(vm);
    uint32_t sep_len = strlen(sep);
    if (sep_len == 0)
    {
        for (const char* cursor = str; *cursor; ++cursor)
        {
            char one[2] = { *cursor, 0 };
            lt_array_push(vm, arr, lt_make_string(vm, one));
        }
        lt_push(vm, arr);
        return 1;
    }

    const char* cursor = str;
    char* found = 0;
    while ((found = strstr(cursor, sep)) != 0)
    {
        uint32_t part_len = (uint32_t)(found - cursor);
        char* part = vm->alloc(part_len + 1);
        memcpy(part, cursor, part_len);
        part[part_len] = 0;
        lt_array_push(vm, arr, lt_make_string(vm, part));
        vm->free(part);
        cursor = found + sep_len;
    }
    lt_array_push(vm, arr, lt_make_string(vm, cursor));
    lt_push(vm, arr);
    return 1;
}

static uint8_t _lt_string_trim_impl(lt_VM* vm, uint8_t argc, uint8_t left, uint8_t right, const char* name)
{
    if (argc != 1) lt_runtime_error(vm, name);
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string argument to string trim!", &str);
    const char* start = str;
    const char* end = str + strlen(str);
    if (left) while (start < end && isspace((unsigned char)*start)) ++start;
    if (right) while (end > start && isspace((unsigned char)*(end - 1))) --end;
    uint32_t len = (uint32_t)(end - start);
    char* out = vm->alloc(len + 1);
    memcpy(out, start, len);
    out[len] = 0;
    lt_push(vm, lt_make_string(vm, out));
    vm->free(out);
    return 1;
}

static uint8_t _lt_string_trim(lt_VM* vm, uint8_t argc) { return _lt_string_trim_impl(vm, argc, 1, 1, "Expected one argument to string.trim!"); }
static uint8_t _lt_string_ltrim(lt_VM* vm, uint8_t argc) { return _lt_string_trim_impl(vm, argc, 1, 0, "Expected one argument to string.ltrim!"); }
static uint8_t _lt_string_rtrim(lt_VM* vm, uint8_t argc) { return _lt_string_trim_impl(vm, argc, 0, 1, "Expected one argument to string.rtrim!"); }

static uint8_t _lt_string_case(lt_VM* vm, uint8_t argc, int (*convert)(int), const char* name)
{
    if (argc != 1) lt_runtime_error(vm, name);
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string argument to string case conversion!", &str);
    uint32_t len = strlen(str);
    char* out = vm->alloc(len + 1);
    for (uint32_t i = 0; i < len; ++i) out[i] = (char)convert((unsigned char)str[i]);
    out[len] = 0;
    lt_push(vm, lt_make_string(vm, out));
    vm->free(out);
    return 1;
}

static uint8_t _lt_string_lower(lt_VM* vm, uint8_t argc) { return _lt_string_case(vm, argc, tolower, "Expected one argument to string.lower!"); }
static uint8_t _lt_string_upper(lt_VM* vm, uint8_t argc) { return _lt_string_case(vm, argc, toupper, "Expected one argument to string.upper!"); }

static uint8_t _lt_string_repeat(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to string.repeat!");
    lt_Value countval = lt_pop(vm);
    const char* str;
    _lt_expect_string(vm, lt_pop(vm), "Non-string source to string.repeat!", &str);
    if (!LT_IS_NUMBER(countval)) lt_runtime_error(vm, "Non-number count to string.repeat!");
    double count_number = LT_GET_NUMBER(countval);
    if (count_number < 0 || count_number > UINT32_MAX || count_number != (double)(uint32_t)count_number)
        lt_runtime_error(vm, "Expected count argument to string.repeat to be a non-negative integer!");
    uint32_t count = (uint32_t)count_number;
    uint32_t str_len = (uint32_t)strlen(str);
    if (str_len != 0 && count > (UINT32_MAX - 1) / str_len)
        lt_runtime_error(vm, "string.repeat result is too large!");
    char* out = vm->alloc(str_len * count + 1);
    if (!out) lt_runtime_error(vm, "Unable to allocate string.repeat result!");
    uint32_t len = 0;
    for (uint32_t i = 0; i < count; ++i)
    {
        memcpy(out + len, str, str_len);
        len += str_len;
    }
    out[len] = 0;
    lt_push(vm, lt_make_string(vm, out));
    vm->free(out);
    return 1;
}

static lt_Value _lt_expand_lookup(lt_VM* vm, lt_Value values, const char* key, uint32_t key_len)
{
    char* name = vm->alloc(key_len + 1);
    memcpy(name, key, key_len);
    name[key_len] = 0;

    lt_Value value = LT_VALUE_NULL;
    if (LT_IS_TABLE(values))
    {
        value = lt_table_get(vm, values, lt_make_string(vm, name));
    }
    else if (LT_IS_ARRAY(values))
    {
        char* end = 0;
        long index = strtol(name, &end, 10);
        if (*name && *end == 0 && index > 0 && (uint32_t)index <= lt_array_length(values))
            value = lt_array_get(vm, values, (uint32_t)index - 1);
    }

    vm->free(name);
    return value;
}

static void _lt_expand_append_value(lt_VM* vm, char** out, uint32_t* len, uint32_t* cap, lt_Value value)
{
    char* rendered = ltstd_tostring(vm, value);
    _lt_string_append(vm, out, len, cap, rendered, strlen(rendered));
    vm->free(rendered);
}

static uint8_t _lt_string_expand(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to string.expand!");
    lt_Value values = lt_pop(vm);
    lt_Value template = lt_pop(vm);
    if (!LT_IS_STRING(template)) lt_runtime_error(vm, "Non-string template to string.expand!");
    if (!LT_IS_TABLE(values) && !LT_IS_ARRAY(values)) lt_runtime_error(vm, "Expected table or array values to string.expand!");

    const char* str = lt_get_string(vm, template);
    char* out = 0;
    uint32_t len = 0;
    uint32_t cap = 0;

    for (const char* cursor = str; *cursor;)
    {
        if (cursor[0] == '{' && cursor[1] == '{')
        {
            const char* key = cursor + 2;
            const char* end = strstr(key, "}}");
            if (end)
            {
                lt_Value value = _lt_expand_lookup(vm, values, key, (uint32_t)(end - key));
                _lt_expand_append_value(vm, &out, &len, &cap, value);
                cursor = end + 2;
                continue;
            }
        }

        if (*cursor == '$' && (isalnum((unsigned char)cursor[1]) || cursor[1] == '_'))
        {
            const char* key = cursor + 1;
            const char* end = key;
            while (isalnum((unsigned char)*end) || *end == '_') ++end;
            lt_Value value = _lt_expand_lookup(vm, values, key, (uint32_t)(end - key));
            _lt_expand_append_value(vm, &out, &len, &cap, value);
            cursor = end;
            continue;
        }

        _lt_string_append(vm, &out, &len, &cap, cursor, 1);
        ++cursor;
    }

    lt_push(vm, lt_make_string(vm, out ? out : ""));
    if (out) vm->free(out);
    return 1;
}

static uint8_t _lt_string_format(lt_VM* vm, uint8_t argc)
{
    if (argc < 1) lt_runtime_error(vm, "Expected at least a template string to string.format!");
    lt_Value val = vm->stack[vm->top - argc];
    if (!LT_IS_STRING(val)) lt_runtime_error(vm, "Non-string argument to string.format!");

    char output[1024];
    char fmtbuf[32];
    uint16_t o_idx = 0;

    const char* format = lt_get_string(vm, val);
    uint8_t current_arg = 1;

    while (*format)
    {
        if (*format == '%')
        {
            if (*(format + 1) == '%')
            {
                if (o_idx + 1 >= sizeof(output)) lt_runtime_error(vm, "string.format output too long!");
                output[o_idx++] = '%';
                format += 2;
            }
            else
            {
                uint8_t fmtloc = 0;
                fmtbuf[fmtloc++] = *format++;
                scan_format: switch (*format)
                {
                case 'd': case 'i': {
                    if (current_arg >= argc) lt_runtime_error(vm, "Not enough arguments to string.format!");
                    lt_Value arg = vm->stack[vm->top - argc + current_arg++];
                    if (!LT_IS_NUMBER(arg)) lt_runtime_error(vm, "Expected numeric argument to string.format!");
                    if (fmtloc + 1 >= sizeof(fmtbuf)) lt_runtime_error(vm, "Invalid or too long format specifier!");
                    fmtbuf[fmtloc++] = *format++;
                    fmtbuf[fmtloc] = 0;
                    int written = snprintf(output + o_idx, 1024 - o_idx, fmtbuf, (int32_t)LT_GET_NUMBER(arg));
                    if (written < 0 || written >= 1024 - o_idx) lt_runtime_error(vm, "string.format output too long!");
                    o_idx += written;
                } break;
                case 'o': case 'u': case 'x': case 'X': {
                    if (current_arg >= argc) lt_runtime_error(vm, "Not enough arguments to string.format!");
                    lt_Value arg = vm->stack[vm->top - argc + current_arg++];
                    if (!LT_IS_NUMBER(arg)) lt_runtime_error(vm, "Expected numeric argument to string.format!");
                    if (fmtloc + 1 >= sizeof(fmtbuf)) lt_runtime_error(vm, "Invalid or too long format specifier!");
                    fmtbuf[fmtloc++] = *format++;
                    fmtbuf[fmtloc] = 0;
                    int written = snprintf(output + o_idx, 1024 - o_idx, fmtbuf, (uint32_t)LT_GET_NUMBER(arg));
                    if (written < 0 || written >= 1024 - o_idx) lt_runtime_error(vm, "string.format output too long!");
                    o_idx += written;
                } break;
                case 'e': case 'E': case 'f': case 'g': case 'G': {
                    if (current_arg >= argc) lt_runtime_error(vm, "Not enough arguments to string.format!");
                    lt_Value arg = vm->stack[vm->top - argc + current_arg++];
                    if (!LT_IS_NUMBER(arg)) lt_runtime_error(vm, "Expected numeric argument to string.format!");
                    if (fmtloc + 1 >= sizeof(fmtbuf)) lt_runtime_error(vm, "Invalid or too long format specifier!");
                    fmtbuf[fmtloc++] = *format++;
                    fmtbuf[fmtloc] = 0;
                    int written = snprintf(output + o_idx, 1024 - o_idx, fmtbuf, LT_GET_NUMBER(arg));
                    if (written < 0 || written >= 1024 - o_idx) lt_runtime_error(vm, "string.format output too long!");
                    o_idx += written;
                } break;
                case 's': {
                    if (current_arg >= argc) lt_runtime_error(vm, "Not enough arguments to string.format!");
                    lt_Value arg = vm->stack[vm->top - argc + current_arg++];
                    if (!LT_IS_STRING(arg)) lt_runtime_error(vm, "Expected string argument to string.format!");
                    if (fmtloc + 1 >= sizeof(fmtbuf)) lt_runtime_error(vm, "Invalid or too long format specifier!");
                    fmtbuf[fmtloc++] = *format++;
                    fmtbuf[fmtloc] = 0;
                    int written = snprintf(output + o_idx, 1024 - o_idx, fmtbuf, lt_get_string(vm, arg));
                    if (written < 0 || written >= 1024 - o_idx) lt_runtime_error(vm, "string.format output too long!");
                    o_idx += written;
                } break;
                default:
                    if (*format == 0) lt_runtime_error(vm, "Incomplete format specifier!");
                    if (fmtloc + 1 >= sizeof(fmtbuf)) lt_runtime_error(vm, "Invalid or too long format specifier!");
                    fmtbuf[fmtloc++] = *format++;
                    goto scan_format;
                    break;
                }
            }
        }
        else
        {
            if (o_idx + 1 >= sizeof(output)) lt_runtime_error(vm, "string.format output too long!");
            output[o_idx++] = *format++;
        }
    }

    output[o_idx] = 0;
    vm->top -= argc;
    lt_push(vm, lt_make_string(vm, output));
    return 1;
}

void ltstd_open_string(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);

    lt_table_set(vm, t, lt_make_string(vm, "from"), lt_make_native(vm, _lt_string_from));
    lt_table_set(vm, t, lt_make_string(vm, "concat"), lt_make_native(vm, _lt_string_concat));
    lt_table_set(vm, t, lt_make_string(vm, "len"), lt_make_native(vm, _lt_string_len));
    lt_table_set(vm, t, lt_make_string(vm, "sub"), lt_make_native(vm, _lt_string_sub));
    lt_table_set(vm, t, lt_make_string(vm, "format"), lt_make_native(vm, _lt_string_format));
    lt_table_set(vm, t, lt_make_string(vm, "contains"), lt_make_native(vm, _lt_string_contains));
    lt_table_set(vm, t, lt_make_string(vm, "startsWith"), lt_make_native(vm, _lt_string_startswith));
    lt_table_set(vm, t, lt_make_string(vm, "endsWith"), lt_make_native(vm, _lt_string_endswith));
    lt_table_set(vm, t, lt_make_string(vm, "indexOf"), lt_make_native(vm, _lt_string_indexof));
    lt_table_set(vm, t, lt_make_string(vm, "replace"), lt_make_native(vm, _lt_string_replace));
    lt_table_set(vm, t, lt_make_string(vm, "split"), lt_make_native(vm, _lt_string_split));
    lt_table_set(vm, t, lt_make_string(vm, "trim"), lt_make_native(vm, _lt_string_trim));
    lt_table_set(vm, t, lt_make_string(vm, "ltrim"), lt_make_native(vm, _lt_string_ltrim));
    lt_table_set(vm, t, lt_make_string(vm, "rtrim"), lt_make_native(vm, _lt_string_rtrim));
    lt_table_set(vm, t, lt_make_string(vm, "lower"), lt_make_native(vm, _lt_string_lower));
    lt_table_set(vm, t, lt_make_string(vm, "upper"), lt_make_native(vm, _lt_string_upper));
    lt_table_set(vm, t, lt_make_string(vm, "repeat"), lt_make_native(vm, _lt_string_repeat));
    lt_table_set(vm, t, lt_make_string(vm, "expand"), lt_make_native(vm, _lt_string_expand));

    lt_table_set(vm, vm->global, lt_make_string(vm, "string"), t);
}
