#include "little.h"

#ifdef _WIN32
#define LT_NATIVE_EXPORT __declspec(dllexport)
#else
#define LT_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

static const lt_Api* lt = 0;

static uint32_t run_collect(lt_VM* vm)
{
    lt_Value gc = lt->table_get(vm, vm->global, lt->make_string(vm, "gc"));
    if (!LT_IS_TABLE(gc)) return 0;
    lt_Value collect = lt->table_get(vm, gc, lt->make_string(vm, "collect"));
    if (!LT_IS_NATIVE(collect)) return 0;
    if (lt->exec(vm, collect, 0) != 1) return 0;
    return (uint32_t)lt->get_number(lt->pop(vm));
}

/* Roots both an object and a string held only in C locals across re-entry. */
static uint8_t rooted_hold(lt_VM* vm, uint8_t argc)
{
    while (argc--) lt->pop(vm);

    lt_Value kept = lt->make_table(vm);
    lt->table_set(vm, kept, lt->make_string(vm, "marker"), lt->make_string(vm, "ALIVE"));
    lt_Value local_string = lt->make_string(vm, "STRING ALIVE");
    lt->root(vm, kept);
    lt->root(vm, local_string);

    run_collect(vm);

    lt_Value marker = lt->table_get(vm, kept, lt->make_string(vm, "marker"));
    lt->unroot(vm, local_string);
    lt->push(vm, marker);
    lt->push(vm, local_string);
    return 2;
}

/* Collection reports a positive count without dereferencing the swept table. */
static uint8_t unrooted_collected(lt_VM* vm, uint8_t argc)
{
    while (argc--) lt->pop(vm);

    lt_Value unreachable = lt->make_table(vm);
    lt->table_set(vm, unreachable, lt->make_string(vm, "marker"), lt->make_string(vm, "UNROOTED"));
    uint32_t collected = run_collect(vm);

    lt->push(vm, lt->make_number((double)collected));
    return 1;
}

static uint8_t api_members(lt_VM* vm, uint8_t argc)
{
    while (argc--) lt->pop(vm);
    lt_Value report = lt->make_table(vm);
    lt->table_set(vm, report, lt->make_string(vm, "hasRoot"), lt->root ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    lt->table_set(vm, report, lt->make_string(vm, "hasUnroot"), lt->unroot ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    lt->table_set(vm, report, lt->make_string(vm, "hasEquals"), lt->equals ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    lt->table_set(vm, report, lt->make_string(vm, "sizeOk"),
                  lt->size >= sizeof(lt_Api) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    lt->push(vm, report);
    return 1;
}

static uint8_t equals_native(lt_VM* vm, uint8_t argc)
{
    if (argc != 2)
    {
        while (argc--) lt->pop(vm);
        lt->push(vm, LT_VALUE_NULL);
        return 1;
    }

    lt_Value right = lt->pop(vm);
    lt_Value left = lt->pop(vm);
    lt->push(vm, lt->equals(left, right) ? LT_VALUE_TRUE : LT_VALUE_FALSE);
    return 1;
}

static void set_native(lt_VM* vm, lt_Value module, const char* name, lt_NativeFn fn)
{
    lt->table_set(vm, module, lt->make_string(vm, name), lt->make_native(vm, fn));
}

LT_NATIVE_EXPORT lt_Value ltopen(lt_VM* vm, const lt_Api* api)
{
    if (!api || api->version != LT_API_VERSION || api->size < sizeof(lt_Api))
        return LT_VALUE_NULL;

    lt = api;
    lt_Value module = lt->make_table(vm);
    set_native(vm, module, "rootedHold", rooted_hold);
    set_native(vm, module, "unrootedCollected", unrooted_collected);
    set_native(vm, module, "apiMembers", api_members);
    set_native(vm, module, "equals", equals_native);
    return module;
}
