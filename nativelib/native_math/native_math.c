#include "little.h"

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

static const lt_Api* lt = 0;

static uint8_t native_add(lt_VM* vm, uint8_t argc)
{
    if (argc != 2)
    {
        while (argc--) lt->pop(vm);
        lt->push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_Value right = lt->pop(vm);
    lt_Value left = lt->pop(vm);
    if (!LT_IS_NUMBER(left) || !LT_IS_NUMBER(right))
    {
        lt->push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt->push(vm, lt->make_number(lt->get_number(left) + lt->get_number(right)));
    return 1;
}

LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* api)
{
    if (!api || api->version != LT_API_VERSION || api->size < sizeof(lt_Api))
        return LT_VALUE_NULL;

    lt = api;
    return lt->make_native(vm, native_add);
}
