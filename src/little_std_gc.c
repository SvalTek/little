#include "little_std.h"

static uint8_t _lt_gc_collect(lt_VM* vm, uint8_t argc)
{
    if (argc != 0) lt_runtime_error(vm, "Expected no arguments to gc.collect!");
    uint32_t num_collected = lt_collect(vm);
    lt_push(vm, LT_VALUE_NUMBER((double)num_collected));
    return 1;
}

static uint8_t _lt_gc_addroot(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to gc.addroot!");
    lt_Value val = lt_pop(vm);
    if (!LT_IS_OBJECT(val)) lt_runtime_error(vm, "Expected argument to gc.addroot to be object!");
    lt_Object* obj = LT_GET_OBJECT(val);
    lt_nocollect(vm, obj);
    return 0;
}

static uint8_t _lt_gc_removeroot(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to gc.removeroot!");
    lt_Value val = lt_pop(vm);
    if (!LT_IS_OBJECT(val)) lt_runtime_error(vm, "Expected argument to gc.removeroot to be object!");
    lt_Object* obj = LT_GET_OBJECT(val);
    lt_resumecollect(vm, obj);
    return 0;
}

void ltstd_open_gc(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);

    lt_table_set(vm, t, lt_make_string(vm, "collect"), lt_make_native(vm, _lt_gc_collect));
    lt_table_set(vm, t, lt_make_string(vm, "addroot"), lt_make_native(vm, _lt_gc_addroot));
    lt_table_set(vm, t, lt_make_string(vm, "removeroot"), lt_make_native(vm, _lt_gc_removeroot));

    lt_table_set(vm, vm->global, lt_make_string(vm, "gc"), t);
}
