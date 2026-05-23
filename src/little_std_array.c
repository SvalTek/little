#include "little_std.h"

static uint8_t _lt_array_next(lt_VM* vm, uint8_t argc)
{
    lt_Value current = lt_getupval(vm, 1);
    lt_Value arr = lt_getupval(vm, 0);

    uint32_t idx = lt_get_number(current);
    lt_Value to_return = idx >= lt_array_length(arr) ? LT_VALUE_NULL : *lt_array_at(arr, idx);

    lt_setupval(vm, 1, LT_VALUE_NUMBER(idx + 1));
    lt_push(vm, to_return);

    return 1;
}

static uint8_t _lt_array_each(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.each!");

    lt_Value arr = lt_pop(vm);

    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected argument to array.each to be array!");

    lt_push(vm, lt_make_native(vm, _lt_array_next));
    lt_push(vm, LT_VALUE_NUMBER(0));
    lt_push(vm, arr);
    lt_close(vm, 2);

    return 1;
}

static uint8_t _lt_range_iter(lt_VM* vm, uint8_t argc)
{
    lt_Value start = lt_getupval(vm, 2);
    lt_Value end = lt_getupval(vm, 1);
    lt_Value step = lt_getupval(vm, 0);

    if (lt_get_number(start) >= lt_get_number(end)) { lt_push(vm, LT_VALUE_NULL); return 1; }

    lt_setupval(vm, 2, lt_make_number(lt_get_number(start) + lt_get_number(step)));

    lt_push(vm, start);
    return 1;
}

static uint8_t _lt_range(lt_VM* vm, uint8_t argc)
{
    lt_Value start = LT_VALUE_NUMBER(0);
    lt_Value end = LT_VALUE_NUMBER(0);
    lt_Value step = LT_VALUE_NUMBER(1);

    if (argc == 1)
    {
        end = lt_pop(vm);
    }
    else if (argc == 2)
    {
        end = lt_pop(vm);
        start = lt_pop(vm);
    }
    else if (argc == 3)
    {
        step = lt_pop(vm);
        end = lt_pop(vm);
        start = lt_pop(vm);
    }
    else lt_runtime_error(vm, "Expected 1-3 args for array.range([start,] end [, step]!");

    if (!LT_IS_NUMBER(start) || !LT_IS_NUMBER(end) || !LT_IS_NUMBER(step))
        lt_runtime_error(vm, "Expected all arguments to array.range to be numbers!");

    lt_push(vm, lt_make_native(vm, _lt_range_iter));
    lt_push(vm, start);
    lt_push(vm, end);
    lt_push(vm, step);
    lt_close(vm, 3);

    return 1;
}

static uint8_t _lt_array_len(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.len!");
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected argument to array.len to be array!");

    lt_push(vm, lt_make_number(lt_array_length(arr)));
    return 1;
}

static uint8_t _lt_array_pop(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.pop!");
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected argument to array.pop to be array!");

    lt_push(vm, lt_array_remove(vm, arr, lt_array_length(arr) - 1));
    return 1;
}

static uint8_t _lt_array_last(lt_VM* vm, uint8_t argc)
{
    if (argc != 1) lt_runtime_error(vm, "Expected one argument to array.last!");
    lt_Value arr = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected argument to array.last to be array!");
    if (lt_array_length(arr) == 0) lt_runtime_error(vm, "Expected argument to array.last to be non-empty!");

    lt_push(vm, *lt_array_at(arr, lt_array_length(arr) - 1));
    return 1;
}

static uint8_t _lt_array_push(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to array.push!");
    lt_Value arr = lt_pop(vm);
    lt_Value val = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected first argument to array.push to be array!");

    lt_array_push(vm, arr, val);
    return 0;
}

static uint8_t _lt_array_remove(lt_VM* vm, uint8_t argc)
{
    if (argc != 2) lt_runtime_error(vm, "Expected two arguments to array.remove!");
    lt_Value arr = lt_pop(vm);
    lt_Value idx = lt_pop(vm);
    if (!LT_IS_ARRAY(arr)) lt_runtime_error(vm, "Expected first argument to array.remove to be array!");
    if (!LT_IS_NUMBER(idx)) lt_runtime_error(vm, "Expected second argument to array.remove to be number!");

    lt_array_remove(vm, arr, (uint32_t)lt_get_number(idx));
    return 0;
}

void ltstd_open_array(lt_VM* vm)
{
    lt_Value t = lt_make_table(vm);
    lt_table_set(vm, t, lt_make_string(vm, "each"), lt_make_native(vm, _lt_array_each));
    lt_table_set(vm, t, lt_make_string(vm, "range"), lt_make_native(vm, _lt_range));

    lt_table_set(vm, t, lt_make_string(vm, "len"), lt_make_native(vm, _lt_array_len));
    lt_table_set(vm, t, lt_make_string(vm, "last"), lt_make_native(vm, _lt_array_last));
    lt_table_set(vm, t, lt_make_string(vm, "pop"), lt_make_native(vm, _lt_array_pop));
    lt_table_set(vm, t, lt_make_string(vm, "push"), lt_make_native(vm, _lt_array_push));
    lt_table_set(vm, t, lt_make_string(vm, "remove"), lt_make_native(vm, _lt_array_remove));

    lt_table_set(vm, vm->global, lt_make_string(vm, "array"), t);
}
