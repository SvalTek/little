#include "little_std.h"

#include <setjmp.h>
#include <stdio.h>
#include <string.h>

uint16_t _lt_exec(lt_VM* vm, lt_Value callable, uint8_t argc);

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
        uint16_t nret = _lt_exec(vm, callable, argc - 1);
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
    for (int32_t i = start; i <= end && count < UINT8_MAX; ++i)
    {
        lt_push(vm, *lt_array_at(array, (uint32_t)i));
        count++;
    }
    return count;
}

void ltstd_open_all(lt_VM* vm)
{
    lt_table_set(vm, vm->global, lt_make_string(vm, "pcall"), lt_make_native(vm, _ltstd_pcall));
    lt_table_set(vm, vm->global, lt_make_string(vm, "unpack"), lt_make_native(vm, _ltstd_unpack));
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
    uint8_t len = 0;

    if (LT_IS_NUMBER(val)) len = sprintf_s(scratch, 256, "%f", LT_GET_NUMBER(val));
    if (LT_IS_NULL(val)) len = sprintf_s(scratch, 256, "null");
    if (LT_IS_TRUE(val)) len = sprintf_s(scratch, 256, "true");
    if (LT_IS_FALSE(val)) len = sprintf_s(scratch, 256, "false");
    if (LT_IS_STRING(val)) len = sprintf_s(scratch, 256, "%s", lt_get_string(vm, val));

    if (LT_IS_OBJECT(val))
    {
        lt_Object* obj = LT_GET_OBJECT(val);
        switch (obj->type)
        {
        case LT_OBJECT_CHUNK: len = sprintf_s(scratch, 256, "chunk 0x%llx", (uintptr_t)obj); break;
        case LT_OBJECT_CLOSURE: len = sprintf_s(scratch, 256, "closure 0x%llx | %d upvals", (uintptr_t)LT_GET_OBJECT(obj->closure.function), obj->closure.captures.length); break;
        case LT_OBJECT_FN: len = sprintf_s(scratch, 256, "function 0x%llx", (uintptr_t)obj); break;
        case LT_OBJECT_TABLE: len = sprintf_s(scratch, 256, "table 0x%llx", (uintptr_t)obj); break;
        case LT_OBJECT_ARRAY: len = sprintf_s(scratch, 256, "array | %d", lt_array_length(val)); break;
        case LT_OBJECT_NATIVEFN: len = sprintf_s(scratch, 256, "native 0x%llx", (uintptr_t)obj); break;
        case LT_OBJECT_BOUND_NATIVE: len = sprintf_s(scratch, 256, "bound_native 0x%llx", (uintptr_t)obj); break;
        case LT_OBJECT_PROMISE: len = sprintf_s(scratch, 256, "promise 0x%llx", (uintptr_t)obj); break;
        case LT_OBJECT_CLASS: len = sprintf_s(scratch, 256, "class 0x%llx", (uintptr_t)obj); break;
        case LT_OBJECT_INSTANCE: len = sprintf_s(scratch, 256, "instance 0x%llx", (uintptr_t)obj); break;
        }
    }

    char* str = vm->alloc(len + 1);
    memcpy(str, scratch, len);
    str[len] = 0;

    return str;
}
