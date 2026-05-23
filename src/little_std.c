#include "little_std.h"

#include <stdio.h>
#include <string.h>

void ltstd_open_all(lt_VM* vm)
{
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
