#include "little_std.h"

#include <stdio.h>
#include <string.h>

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

    lt_push(vm, lt_make_string(vm, accum));
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
    if (argc < 2) lt_runtime_error(vm, "Expected at least two arguments to string.sub!");

    lt_Value len = LT_VALUE_NULL;
    if (argc == 3) len = lt_pop(vm);

    lt_Value start = lt_pop(vm);
    lt_Value str = lt_pop(vm);

    if (!LT_IS_STRING(str)) lt_runtime_error(vm, "Non-string argument to string.sub!");
    if (!LT_IS_NUMBER(start)) lt_runtime_error(vm, "Non-number starting point to string.sub!");

    const char* cstr = lt_get_string(vm, str);

    if (!LT_IS_NUMBER(len))
    {
        len = LT_VALUE_NUMBER(strlen(cstr) - start);
    }

    char* newstr = vm->alloc(LT_GET_NUMBER(len) + 1);
    memcpy(newstr, cstr + start, len);

    lt_push(vm, lt_make_string(vm, newstr));
    vm->free(newstr);
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
                    fmtbuf[fmtloc++] = *format++;
                    fmtbuf[fmtloc] = 0;
                    o_idx += sprintf_s(output + o_idx, 1024 - o_idx, fmtbuf, (int32_t)LT_GET_NUMBER(vm->stack[vm->top - argc + current_arg++]));
                } break;
                case 'o': case 'u': case 'x': case 'X': {
                    fmtbuf[fmtloc++] = *format++;
                    fmtbuf[fmtloc] = 0;
                    o_idx += sprintf_s(output + o_idx, 1024 - o_idx, fmtbuf, (uint32_t)LT_GET_NUMBER(vm->stack[vm->top - argc + current_arg++]));
                } break;
                case 'e': case 'E': case 'f': case 'g': case 'G': {
                    fmtbuf[fmtloc++] = *format++;
                    fmtbuf[fmtloc] = 0;
                    o_idx += sprintf_s(output + o_idx, 1024 - o_idx, fmtbuf, LT_GET_NUMBER(vm->stack[vm->top - argc + current_arg++]));
                } break;
                case 's': {
                    fmtbuf[fmtloc++] = *format++;
                    fmtbuf[fmtloc] = 0;
                    o_idx += sprintf_s(output + o_idx, 1024 - o_idx, fmtbuf, lt_get_string(vm, vm->stack[vm->top - argc + current_arg++]));
                } break;
                default:
                    fmtbuf[fmtloc++] = *format++;
                    goto scan_format;
                    break;
                }
            }
        }
        else
        {
            output[o_idx++] = *format++;
        }
    }

    output[o_idx] = 0;
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

    lt_table_set(vm, vm->global, lt_make_string(vm, "string"), t);
}
