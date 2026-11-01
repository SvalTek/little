#include "little_std.h"

#include <stdio.h>
#include <string.h>

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
        case LT_OBJECT_CLASS: len = snprintf(scratch, sizeof(scratch), "%s", lt_get_string(vm, obj->class_def.name)); break;
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
